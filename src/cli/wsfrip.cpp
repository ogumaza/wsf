// SPDX-License-Identifier: MIT

// wsfrip: builds WSF sets (.wsflib + .miniwsf files) from Wii games and sound archives.
//
// usage: wsfrip [options] <input> [<input> ...]
//   <input>          a Wii disc image with a decrypted game partition (NKit ISO or GCZ, RVZ, WIA, or an ISO, GCZ, WBFS,
//                    CISO or WDF made from one), an extracted disc, an unpacked WiiWare title (a folder with the
//                    title's TMD and decrypted contents), or a sound archive (.brsar). A title rips in archive mode
//                    only. A game whose sound library is JAudio2 rips in game mode only, and requires a driver profile.
//                    A main.dol among the inputs isn't ripped. The sound archives among the inputs take AX's microcode
//                    and the build of NW4R SND from it.
//   -o, --output DIR the folder for the rips (default: a folder next to each input, named after the input with "_wsf"
//                    added)
//   --list           list the game's sound archives, or a JAudio2 game's sounds and sections, and whether it has a
//                    driver profile, then stop
//   --mode game|archive
//                    game mode runs the game's code and requires a driver profile for the game (src/rip/rip.cpp). It's
//                    the default when a profile exists. Archive mode plays the sound archives with WSF's model of the
//                    SDK's sound player instead.
//   --microcode FILE the main.dol of the game that a sound archive comes from. Archive mode on a sound archive reads
//                    AX's microcode and the build of NW4R SND from this file. By default they come from a main.dol
//                    given with the archive, or else from one in the archive's folder.
//   --archive PATH   rip only this sound archive of a game (its path on the disc). A JAudio2 game has none.
//   --only REGEX     rip only sounds whose label matches (ECMAScript regex)
//   --bgm            rip only the music (rip::IsMusic): sequences labelled as BGM, or that play on a music player. In a
//                    JAudio2 game, the sounds labelled as BGM or in the section named BGM.
//   --no-length      skip length analysis (no length or fade tags, only the sequences at a volume of 0 are found to
//                    make no sound, and every JAudio2 sound is taken to start)
//   --jobs N         the number of length analyses run in parallel (default: the number of CPU threads, at most 8 in a
//                    32-bit build)
//   --game NAME      game tag (default: the profile's name, or else the game's title, or the archive's name)
//   --artist NAME, --year YEAR, --copyright TEXT, --by NAME (wsfby)
//   --version        print the version
//
// Each of a game's sound archives gets a separate folder in the output folder. The folder is named after the archive.
// Where two of the game's archives have the same name, the names of the folders above them are added. A JAudio2 game's
// sounds make one set, in the output folder. Sequences whose data isn't all there are listed and skipped. So are those
// that make no sound: those whose volume in the archive is 0, and those that the length analysis finds never sound. The
// inputs are ripped one after another. An input that fails doesn't stop the rest. Files dropped on the program become
// inputs.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"
#include "xsflib/sha1.h"

#include "nw4r/rsar_options.h"
#include "nw4r/util.h"
#include "rip/jaudio_analysis.h"
#include "rip/rip.h"
#include "wsf/format.h"

using namespace wsf;
namespace fs = std::filesystem;

namespace
{

// The file name for AX's microcode in a rip. The microcode comes from the game's main.dol.
constexpr char kMicrocodeName[] = "ax_microcode.bin";

// The notices for archive mode: on a game without a driver profile, on a sound archive, and on a game with a profile.
// The notice on a sound archive says where exact rips come from. On a game with a profile, only --mode archive picks
// archive mode. The notice for that case says to leave the option out.
constexpr char kArchiveModeNotice[] =
    "note: in archive mode, WSF's model of the SDK's sound player plays the sounds instead of the\n"
    "      game's code. The DSP still runs the game's AX microcode.\n";
constexpr char kArchiveFileNotice[] =
    "note: in archive mode, WSF's model of the SDK's sound player plays the sounds instead of the\n"
    "      game's code. The DSP still runs the game's AX microcode. For exact rips of a game with a\n"
    "      driver profile, rip the game itself.\n";
constexpr char kArchiveModeChosenNotice[] =
    "note: in archive mode, WSF's model of the SDK's sound player plays the sounds instead of the\n"
    "      game's code. The DSP still runs the game's AX microcode. For exact rips of this game,\n"
    "      leave out --mode archive.\n";

// The notice for a title. Game mode emulates a disc. A title's game reads its contents from the Wii's NAND instead.
constexpr char kTitleNotice[] =
    "note: WiiWare titles rip in archive mode only. WSF's model of the SDK's sound player plays\n"
    "      the sounds instead of the game's code. The DSP still runs the game's AX microcode.\n";

constexpr char kUsage[] =
    "usage: wsfrip [options] <input> [<input> ...]\n"
    "\n"
    "Rips each input into a .wsflib and a .miniwsf per sound. The output folder is next to the\n"
    "input and has the input's name with \"_wsf\" added. An input is a Wii disc image (ISO, NKit,\n"
    "RVZ, WIA, GCZ, WBFS, CISO or WDF), an extracted disc, an unpacked WiiWare title or a sound\n"
    "archive (.brsar). The game partition of a disc image or extracted disc must be stored\n"
    "decrypted. NKit, RVZ and WIA images store it that way. An unpacked title is a folder with the\n"
    "title's TMD (.tmd) and its decrypted contents (.app). A title rips in archive mode only. A\n"
    "game whose sound library is JAudio2 rips in game mode only. A sound archive requires the\n"
    "main.dol of its game. The ripper takes AX's microcode and the build of the sound library from\n"
    "it. Give the main.dol with the archive or put it in the archive's folder. You can also drop\n"
    "files on the program. Sequences that make no sound, and those whose data isn't all there, are\n"
    "listed and skipped.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list a game's sound archives, or a JAudio2 game's sounds, and whether\n"
    "                       it has a driver profile, then stop\n"
    "  --mode game|archive  game mode runs the game's code and requires a driver profile for the\n"
    "                       game. It's the default when a profile exists. Archive mode plays the\n"
    "                       sound archives with WSF's model of the SDK's sound player.\n"
    "  --microcode FILE     the main.dol of the game that the sound archive comes from, for AX's\n"
    "                       microcode and the build of the sound library in archive mode\n"
    "  --archive PATH       rip only this sound archive of a game (its path on the disc)\n"
    "  --only REGEX         rip only the sounds whose label matches\n"
    "  --bgm                rip only the music: sequences labelled as BGM, or that play on a player\n"
    "                       named for music (BGM, MUSIC, SONG or FANFARE). In a JAudio2 game, the\n"
    "                       sounds labelled as BGM or in the section named BGM.\n"
    "  --no-length          skip length analysis and omit length and fade tags. The analysis also\n"
    "                       finds the sequences that make no sound, and the JAudio2 sounds that\n"
    "                       can't start. Without it, only the sequences at a volume of 0 are\n"
    "                       skipped.\n"
    "  --jobs N             the number of length analyses that run at once (default: the number\n"
    "                       of CPU threads, and at most 8 in a 32-bit build)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (the game defaults to the profile's name or else the game's title;\n"
    "                       for a sound archive, to its file name without the extension)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

// What WSF's model reads from a game's main.dol: AX's microcode and the game's build of NW4R SND. The model follows
// that build. Archive mode plays the model, and the length analyses of both modes run it. The main.dol's driver
// profile, if it has one, lets the messages point to game mode.
struct GameCode
{
    ntdlib::Microcode microcode;
    nw4r::SndBuild build;
    const rip::GameProfile* profile = nullptr;
};

// RipArchive's result when the model can't drive the game's AX microcode.
constexpr int kMicrocodeRejected = -2;

// The rip frame's options, and --microcode.
struct Options : xsflib::cli::RipOptions
{
    // The main.dol from --microcode and the main.dol among the inputs: their paths, and the microcode, the build of
    // NW4R SND and the profile read from each. Only loose sound archives use them.
    std::string microcode_file, input_microcode_file;
    std::optional<GameCode> microcode, input_microcode;
};

struct Job
{
    uint32_t index;
    std::string label;
    std::optional<rip::Timing> timing;
    std::optional<rip::SoundData> data; // game mode: what the driver loads for the sound
};

// A count and its noun: "1 sound", "2 sounds".
std::string Counted(std::size_t count, const std::string& noun)
{
    return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

// Length analysis runs every selected sequence through the model and AX's microcode on the DSP. The sounds are
// independent. The analysis spreads them over several threads.
void AnalyzeLengths(std::vector<Job>& jobs, const std::vector<uint8_t>& archive, const GameCode& code, unsigned threads)
{
    const rip::LengthAnalyzer analyzer(archive, code.microcode, code.build);
    const auto analyze = [&analyzer](Job& job)
    {
        job.timing = analyzer.Analyze(job.index);

        return true;
    };
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze,
                                {.out_of_memory = "out of memory during length analysis; try fewer --jobs"});
}

// The sequences that --only and --bgm pick.
std::vector<Job> SelectSounds(const ntdlib::rsar::SoundArchive& archive, const Options& o)
{
    std::vector<Job> jobs;
    const auto& sounds = archive.Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        // Streams are out of scope, and wave sounds aren't ripped yet.
        const auto index = static_cast<uint32_t>(i);
        if (sounds[i].type == ntdlib::rsar::SoundType::kSequence && std::regex_search(sounds[i].name, o.only) &&
            (!o.bgm || rip::IsMusic(archive, index)))
        {
            jobs.push_back({index, sounds[i].name, std::nullopt, std::nullopt});
        }
    }

    return jobs;
}

// Reports that --only or --bgm selected no sound. This is an error. Nothing is written.
int NoSoundMatches(const Options& o)
{
    if (o.only_text.empty())
    {
        std::fprintf(stderr, "error: no music sequences found (--bgm)\n");
    }
    else
    {
        std::fprintf(stderr, "error: no %s matches '%s'\n", o.bgm ? "music" : "sound", o.only_text.c_str());
    }

    return 1;
}

// Takes out the sequences whose volume in the archive is 0. They make no sound. The game's sound library multiplies
// everything that a sound plays by its volume. Returns the number taken out.
std::size_t SkipMuted(std::vector<Job>& jobs, const ntdlib::rsar::SoundArchive& archive)
{
    const auto muted = [&archive](const Job& job)
    {
        if (archive.Sounds()[job.index].volume != 0)
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: it makes no sound (volume 0)\n", job.label.c_str());

        return true;
    };
    return std::erase_if(jobs, muted);
}

// Takes out the sequences that the length analysis found never sound. Returns the number taken out.
template <typename J>
std::size_t SkipSilent(std::vector<J>& jobs)
{
    const auto silent = [](const J& job)
    {
        if (!job.timing || !job.timing->silent)
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());

        return true;
    };
    return std::erase_if(jobs, silent);
}

// Rips one sound archive in archive mode into `out_dir`. `names` is the registry of file names for `out_dir`. Adds the
// number of sounds that --only selected to `matched`. Returns the number of .miniwsf files written, kMicrocodeRejected
// if the model can't drive the game's AX microcode, or -1 on another error.
int RipArchive(const fs::path& out_dir, xsflib::rip::FileNames& names, const std::string& lib_base,
               const std::string& archive_path, const std::vector<uint8_t>& archive_bytes, const GameCode& code,
               const std::string& game_name, const Options& o, std::size_t& matched)
{
    std::optional<ntdlib::rsar::SoundArchive> archive;
    try
    {
        archive = ntdlib::rsar::SoundArchive::Load(archive_bytes, wsf::nw4r::kRsarOptions);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", archive_path.c_str(), e.what());
        return -1;
    }

    if (archive->Truncated())
    {
        std::fprintf(stderr, "warning: %s is truncated: sounds whose data lies past its end are skipped\n",
                     archive_path.c_str());
    }

    std::vector<Job> jobs = SelectSounds(*archive, o);
    matched += jobs.size();
    if (jobs.empty())
    {
        return 0;
    }

    if (!rip::MicrocodeWorks(code.microcode, *archive))
    {
        std::fprintf(stderr,
                     "error: %s: archive mode can't play this game. Its AX microcode rejects the command lists\n"
                     "       that WSF's model makes. The game links a build of AX that the model doesn't follow.\n",
                     archive_path.c_str());
        return kMicrocodeRejected;
    }

    // Sequences whose data isn't all in the archive would play with missing instruments.
    std::vector<Job> complete;
    std::size_t skipped = 0;
    for (Job& job : jobs)
    {
        const auto missing = rip::MissingData(*archive, job.index);
        if (missing.empty())
        {
            complete.push_back(std::move(job));
            continue;
        }

        skipped++;
        std::string what;
        for (std::size_t i = 0; i < missing.size(); i++)
        {
            what += (i ? ", " : "") + missing[i];
        }

        std::fprintf(stderr, "skipped %s: not in this archive: %s\n", job.label.c_str(), what.c_str());
    }

    // The sequences that make no sound are left out: those at a volume of 0, and those that the length analysis finds
    // never sound.
    std::size_t silent = SkipMuted(complete, *archive);
    if (o.lengths && !complete.empty())
    {
        AnalyzeLengths(complete, archive_bytes, code, o.jobs);
        silent += SkipSilent(complete);
    }

    const std::string skipped_text = xsflib::cli::SkippedText(skipped, "data outside the archive", silent, "sequence");
    if (complete.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", archive_path.c_str(), skipped_text.c_str());
        return 0;
    }

    // The library keeps only the data that the ripped sequences play. A game's archive can hold hundreds of megabytes
    // of sound effects and voices that a rip of its music doesn't play.
    std::vector<uint32_t> indices;
    for (const Job& job : complete)
    {
        indices.push_back(job.index);
    }

    const Tags common = xsflib::cli::RipTags(o, game_name, "wsf", "archive");
    const auto lib = rip::BuildArchiveLibrary(archive_path, rip::KeepSequenceData(*archive, indices), kMicrocodeName,
                                              code.microcode, code.build, common);
    const std::string lib_name = rip::LibraryName(out_dir, names, lib_base, lib);
    fs::create_directories(out_dir);
    if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(lib_name), lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return -1;
    }

    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    const auto build_mini = [&lib_name](const Job& job, const Tags& tags)
    {
        return rip::BuildArchiveMini(job.index, lib_name, tags);
    };
    const int count = xsflib::cli::WriteMinis(out_dir, names, complete, common, "wsf_sound", build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %s%s\n", archive_path.c_str(), Counted(count, "miniwsf file").c_str(),
                     skipped_text.c_str());
    }

    return count;
}

// Rips one of a game's sound archives in game mode into `out_dir`. `names` is the registry of file names for `out_dir`.
// Adds the number of sounds that --only selected to `matched`. Returns the number of .miniwsf files written, or -1 on
// an error.
int RipArchiveInGameMode(const fs::path& out_dir, xsflib::rip::FileNames& names, const ntdlib::disc::GameFiles& game,
                         const rip::GameProfile& profile, const std::string& archive_path,
                         const std::vector<uint8_t>& archive_bytes, const GameCode& code, const std::string& lib_base,
                         const std::string& game_name, const Options& o, std::size_t& matched)
{
    const auto archive = ntdlib::rsar::SoundArchive::Load(archive_bytes, wsf::nw4r::kRsarOptions);
    std::vector<Job> jobs = SelectSounds(archive, o);
    matched += jobs.size();
    if (jobs.empty())
    {
        return 0;
    }

    // The driver loads the groups that hold the sequence and its bank. A sequence that no group holds isn't ripped.
    std::vector<Job> playable;
    std::size_t skipped = 0;
    for (Job& job : jobs)
    {
        job.data = rip::FindSoundData(archive, job.index);
        if (job.data)
        {
            playable.push_back(std::move(job));
            continue;
        }

        skipped++;
        std::fprintf(stderr, "skipped %s: no group in the archive holds its sequence, or its bank\n",
                     job.label.c_str());
    }

    // The sequences that make no sound are left out, as in archive mode.
    std::size_t silent = SkipMuted(playable, archive);
    if (o.lengths && !playable.empty())
    {
        AnalyzeLengths(playable, archive_bytes, code, o.jobs);
        silent += SkipSilent(playable);
    }

    const std::string skipped_text = xsflib::cli::SkippedText(skipped, "no group holds them", silent, "sequence");
    if (playable.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", archive_path.c_str(), skipped_text.c_str());
        return 0;
    }

    const Tags common = xsflib::cli::RipTags(o, game_name, "wsf", "game");
    std::string error;
    const auto lib = rip::BuildLibrary(game, profile, archive_path, archive, common, &error);
    if (!lib)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return -1;
    }

    const std::string lib_name = rip::LibraryName(out_dir, names, lib_base, *lib);
    fs::create_directories(out_dir);
    if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(lib_name), *lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return -1;
    }

    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib->size());

    const auto build_mini = [&lib_name](const Job& job, const Tags& tags)
    {
        return rip::BuildMini(job.index, *job.data, lib_name, tags);
    };
    const int count = xsflib::cli::WriteMinis(out_dir, names, playable, common, "wsf_sound", build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %s%s\n", archive_path.c_str(), Counted(count, "miniwsf file").c_str(),
                     skipped_text.c_str());
    }

    return count;
}

// Lists a game's sound archives and their contents.
void ListArchives(const ntdlib::disc::GameFiles& game)
{
    const auto archives = game.SoundArchives();
    std::fprintf(stderr, "%s:\n", Counted(archives.size(), "sound archive").c_str());
    for (const auto& path : archives)
    {
        std::vector<uint8_t> bytes;
        if (!game.read_file(path, bytes))
        {
            std::fprintf(stderr, "  %s (unreadable)\n", path.c_str());
            continue;
        }

        try
        {
            const auto archive = ntdlib::rsar::SoundArchive::Load(bytes, wsf::nw4r::kRsarOptions);
            std::size_t sequences = 0, wave_sounds = 0, streams = 0;
            for (const auto& s : archive.Sounds())
            {
                sequences += s.type == ntdlib::rsar::SoundType::kSequence;
                wave_sounds += s.type == ntdlib::rsar::SoundType::kWaveSound;
                streams += s.type == ntdlib::rsar::SoundType::kStream;
            }

            std::fprintf(stderr, "  %s: %zu bytes, %s, %s, %s\n", path.c_str(), bytes.size(),
                         Counted(sequences, "sequence").c_str(), Counted(wave_sounds, "wave sound").c_str(),
                         Counted(streams, "stream").c_str());
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "  %s: %s\n", path.c_str(), e.what());
        }
    }
}

// The profile in `profiles` of another release of the game with `game_id`, or nullptr. A game's releases share the
// first three characters of their game IDs. The fourth gives the region.
template <typename Profile>
const Profile* ProfileOfOtherRelease(const std::vector<Profile>& profiles, const std::string& game_id)
{
    for (const Profile& profile : profiles)
    {
        if (game_id.size() >= 3 && profile.game_id.compare(0, 3, game_id, 0, 3) == 0)
        {
            return &profile;
        }
    }

    return nullptr;
}

// The names of `profiles`, separated by commas.
template <typename Profile>
std::string ProfileNames(const std::vector<Profile>& profiles)
{
    std::string names;
    for (const Profile& profile : profiles)
    {
        names += (names.empty() ? "" : ", ") + profile.name;
    }

    return names;
}

// Says where a profile comes from when it was derived from another release's (see rip::FindProfile).
void NoteDerivedProfile(const rip::GameProfile& profile)
{
    if (!profile.derived_from.empty())
    {
        std::fprintf(stderr,
                     "note: this profile comes from the one for %s. Its functions were found in this main.dol.\n",
                     profile.derived_from.c_str());
    }
}

// The driver profile of `dol`, whatever disc it comes from, or nullptr.
const rip::GameProfile* ProfileOfDol(const std::vector<uint8_t>& dol)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(dol));
    for (const rip::GameProfile& profile : rip::KnownProfiles())
    {
        if (sha1 == profile.dol_sha1)
        {
            return &profile;
        }
    }

    return nullptr;
}

// A JAudio2 game's sound to rip, and the wave groups that it plays, each (system << 16) | group.
struct JAudioJob
{
    uint32_t index;    // the sound's place in the sound table
    uint32_t sound_id; // the ID that the game passes to its sound library
    std::string label;
    std::optional<rip::Timing> timing;
    std::vector<uint32_t> waves;
    std::string start_error; // the player's message when the length pass can't start the sound
};

// Takes out the sounds that the length pass couldn't start with their wave groups loaded. No player could start their
// rips. Returns the number taken out.
std::size_t SkipUnstartable(std::vector<JAudioJob>& jobs)
{
    const auto unstartable = [](const JAudioJob& job)
    {
        if (job.start_error.empty())
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: it can't start with its wave groups loaded (%s)\n", job.label.c_str(),
                     job.start_error.c_str());

        return true;
    };
    return std::erase_if(jobs, unstartable);
}

// A JAudio2 game's sounds that --only and --bgm pick. Streams are out of scope. Music is a sound whose label contains
// BGM, or a sound of the section that the name table calls BGM. rip::IsMusic takes the same labels for NW4R SND.
std::vector<JAudioJob> SelectJAudioSounds(const std::vector<ntdlib::jaudio::SoundEntry>& sounds, const Options& o)
{
    std::vector<JAudioJob> jobs;
    for (const ntdlib::jaudio::SoundEntry& sound : sounds)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "0x%08x", sound.Id());
        const std::string label = sound.name.empty() ? id : sound.name;
        if (!ntdlib::jaudio::LayoutOf(sound.type).stream && std::regex_search(label, o.only) &&
            (!o.bgm || xsflib::AsciiLower(label).find("bgm") != std::string::npos || sound.section_name == "BGM"))
        {
            jobs.push_back({static_cast<uint32_t>(&sound - sounds.data()), sound.Id(), label, std::nullopt, {}, {}});
        }
    }

    return jobs;
}

// Adds the disc's paths of the AW files that hold `groups` to `paths`, each path once. Returns false after printing an
// error if one isn't on the disc.
bool AddAwFiles(const ntdlib::disc::GameFiles& game, const rip::JAudioFiles& files,
                const std::vector<ntdlib::jaudio::WaveSystem>& systems, std::span<const uint32_t> groups,
                std::vector<std::string>& paths)
{
    std::string missing;
    const std::optional<std::vector<std::string>> found = rip::FindAwFiles(game, files, systems, groups, &missing);
    if (!found)
    {
        std::fprintf(stderr, "error: the disc has no %s\n", missing.c_str());
        return false;
    }

    for (const std::string& path : *found)
    {
        if (std::find(paths.begin(), paths.end(), path) == paths.end())
        {
            paths.push_back(path);
        }
    }

    return true;
}

// Lists a JAudio2 game's sounds: how many there are, how many of them are streams, and the sections of the name table.
void ListJAudioSounds(const std::string& baa, const std::vector<ntdlib::jaudio::SoundEntry>& sounds)
{
    const auto is_stream = [](const ntdlib::jaudio::SoundEntry& sound)
    {
        return ntdlib::jaudio::LayoutOf(sound.type).stream;
    };
    const auto streams = std::count_if(sounds.begin(), sounds.end(), is_stream);
    std::fprintf(stderr, "%s: %s, %zu of them streams\n", baa.c_str(), Counted(sounds.size(), "sound").c_str(),
                 static_cast<std::size_t>(streams));

    std::map<std::string, std::size_t> sections;
    for (const ntdlib::jaudio::SoundEntry& sound : sounds)
    {
        sections[sound.section_name]++;
    }

    for (const auto& [name, count] : sections)
    {
        std::fprintf(stderr, "  section %s: %s\n", name.empty() ? "(no name)" : name.c_str(),
                     Counted(count, "sound").c_str());
    }
}

// Reports that WSF has no driver profile for a JAudio2 game. Only game mode plays JAudio2. With `list`, the report is
// --list's line. Otherwise the report is an error. Unless another release has a profile, the error names the games with
// profiles.
void ReportNoJAudioProfile(const std::string& game_id, bool list)
{
    const rip::JAudioProfile* other_release = ProfileOfOtherRelease(rip::KnownJAudioProfiles(), game_id);
    if (list)
    {
        std::fprintf(stderr, "no driver profile for this %s: JAudio2 games rip in game mode only\n",
                     other_release ? "release" : "game");
    }
    else if (other_release)
    {
        std::fprintf(stderr, "error: no driver profile for this release. JAudio2 games rip in game mode only.\n");
    }
    else
    {
        std::fprintf(stderr,
                     "error: no driver profile for this game (known: %s). JAudio2 games rip in game mode only.\n",
                     ProfileNames(rip::KnownJAudioProfiles()).c_str());
    }

    if (other_release)
    {
        std::fprintf(stderr, "note: WSF's driver profile is for %s.\n", other_release->name.c_str());
    }
}

// Game mode for a JAudio2 game: rips the sounds of its start-up archive into one set. Each sound first plays with only
// the wave groups that the game's audio set-up loads, and the waves its notes look up give its groups. The library then
// holds every group that a ripped sound plays, and each sound plays with its groups for the length analysis. `profile`
// is null for a game without a driver profile. Only --list works then.
int RipJAudioGame(const std::string& input, const fs::path& out_dir, const ntdlib::disc::GameFiles& game,
                  const rip::JAudioProfile* profile, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    const std::optional<rip::JAudioFiles> files = rip::FindJAudioFiles(game);
    if (!files)
    {
        std::fprintf(stderr, "error: the game has no JAudio2 start-up archive (.baa)\n");
        return 1;
    }

    // A rip checks its options and the profile before it parses the start-up archive. --archive is a usage error (exit
    // code 2) and comes first. A game without a profile can't be ripped at all. The profile's error therefore comes
    // before the one for --mode archive.
    if (!o.list)
    {
        if (!o.only_archive.empty())
        {
            std::fprintf(stderr,
                         "error: --archive selects one of a game's NW4R SND sound archives, and this game's sound "
                         "library is JAudio2\n");
            return 2;
        }
        if (!profile)
        {
            ReportNoJAudioProfile(game.GameId(), false);
            return 1;
        }
        if (o.mode == "archive")
        {
            std::fprintf(stderr,
                         "error: archive mode plays NW4R SND's sequences, and this game's sound library is JAudio2. "
                         "Leave out --mode archive.\n");
            return 1;
        }
    }

    std::vector<uint8_t> baa;
    if (!game.read_file(files->baa, baa))
    {
        std::fprintf(stderr, "error: can't read %s\n", files->baa.c_str());
        return 1;
    }

    // The sound table, with the names of its sections for --list and --bgm.
    const std::vector<ntdlib::jaudio::SoundEntry> sounds = ntdlib::jaudio::ReadSounds(baa, {.section_names = true});
    if (o.list)
    {
        ListJAudioSounds(files->baa, sounds);
        if (profile)
        {
            std::fprintf(stderr, "profile: %s (game mode, JAudio2)\n", profile->name.c_str());
        }
        else
        {
            ReportNoJAudioProfile(game.GameId(), true);
        }

        return 0;
    }

    std::vector<JAudioJob> jobs = SelectJAudioSounds(sounds, o);
    if (jobs.empty())
    {
        if (!o.only_text.empty() || o.bgm)
        {
            return NoSoundMatches(o);
        }

        std::fprintf(stderr, "%s: nothing to rip\n", files->baa.c_str());
        return 0;
    }

    // Every library holds the start-up archive, the RARC archives beside it and the groups that the audio set-up loads.
    const std::vector<ntdlib::jaudio::WaveSystem> systems = ntdlib::jaudio::ReadWaveSystems(baa);
    std::vector<std::string> paths = {files->baa};
    paths.insert(paths.end(), files->archives.begin(), files->archives.end());
    if (!AddAwFiles(game, *files, systems, profile->static_waves, paths))
    {
        return 1;
    }

    std::string error;
    const auto base = rip::BuildJAudioLibrary(game, *profile, paths, {}, &error);
    if (!base)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const auto discover = [&](JAudioJob& job)
    {
        std::set<ntdlib::jaudio::WaveUse> found;
        if (!rip::AnalyzeJAudioSound(*base, *profile, job.sound_id, {}, &found, true))
        {
            std::fprintf(stderr, "warning: the waves of %s can't be found; it plays without them\n", job.label.c_str());
            return false;
        }

        job.waves = ntdlib::jaudio::WaveGroups(systems, {found.begin(), found.end()});
        if (job.waves.size() > ntdlib::flipper::z2::kMaxWaves)
        {
            std::fprintf(stderr, "warning: %s plays waves from %zu groups; it loads the first %u\n", job.label.c_str(),
                         job.waves.size(), ntdlib::flipper::z2::kMaxWaves);
            job.waves.resize(ntdlib::flipper::z2::kMaxWaves);
        }

        // The pass gives no timing. False keeps the runner from reporting a failed length analysis.
        return false;
    };
    std::fprintf(stderr, "finding the wave groups of %s\n", Counted(jobs.size(), "sound").c_str());
    xsflib::cli::AnalyzeLengths(
        jobs, o.jobs, discover,
        {.out_of_memory = "out of memory while finding the waves; try fewer --jobs", .progress = "searched"});

    for (const JAudioJob& job : jobs)
    {
        if (!AddAwFiles(game, *files, systems, job.waves, paths))
        {
            return 1;
        }
    }

    const std::string game_name = o.game_name.empty() ? profile->name : o.game_name;
    const Tags common = xsflib::cli::RipTags(o, game_name, "wsf", "game");
    const auto lib = rip::BuildJAudioLibrary(game, *profile, paths, common, &error);
    if (!lib)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::size_t unstartable = 0;
    std::size_t silent = 0;
    if (o.lengths)
    {
        // A sound that can't start isn't analyzed: SkipUnstartable takes it out.
        const auto analyze = [&](JAudioJob& job)
        {
            std::set<ntdlib::jaudio::WaveUse> played;
            job.timing =
                rip::AnalyzeJAudioSound(*lib, *profile, job.sound_id, job.waves, &played, false, &job.start_error);
            if (!job.start_error.empty())
            {
                return false;
            }

            const std::vector<ntdlib::jaudio::WaveUse> waves(played.begin(), played.end());
            const std::size_t missing = ntdlib::jaudio::MissingWaves(systems, job.waves, waves);
            if (missing)
            {
                std::fprintf(stderr, "warning: %s looked up %s outside its wave groups\n", job.label.c_str(),
                             Counted(missing, "wave").c_str());
            }

            return true;
        };
        xsflib::cli::AnalyzeLengths(jobs, o.jobs, analyze,
                                    {.out_of_memory = "out of memory during length analysis; try fewer --jobs"});
        unstartable = SkipUnstartable(jobs);
        silent = SkipSilent(jobs);
    }

    const std::string skipped_text = xsflib::cli::SkippedText(unstartable, "can't start", silent, "sound");
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", files->baa.c_str(), skipped_text.c_str());
        return 0;
    }

    xsflib::rip::FileNames& names = xsflib::cli::NamesIn(registries, out_dir, ".wsflib", ".miniwsf");
    const std::string lib_name = rip::LibraryName(out_dir, names, game_name, *lib);
    fs::create_directories(out_dir);
    if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(lib_name), *lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib->size());

    const auto build_mini = [&lib_name](const JAudioJob& job, const Tags& tags)
    {
        return rip::BuildJAudioMini(job.sound_id, job.waves, lib_name, tags);
    };
    const int count = xsflib::cli::WriteMinis(out_dir, names, jobs, common, "wsf_sound", build_mini);
    if (count < 0)
    {
        return 1;
    }

    std::fprintf(stderr, "%s: %s%s\n", xsflib::rip::InputPath(input).filename().string().c_str(),
                 Counted(count, "miniwsf file").c_str(), skipped_text.c_str());
    if (count > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return 0;
}

// Rips a game image or folder: each of its sound archives, or the one that --archive names. Uses game mode if a driver
// profile exists. Otherwise uses archive mode. --mode overrides the choice. Each archive's folder takes its registry of
// file names from `registries` (see xsflib::cli::NamesIn). RipJAudioGame rips a JAudio2 game. Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o,
            xsflib::cli::FileNameRegistries& registries)
{
    ntdlib::disc::GameFiles game;
    if (const auto error = ntdlib::disc::LoadGame(fs::path(input), game, rip::DiscOptions()))
    {
        std::fprintf(stderr, "error: %s\n", error->c_str());
        return 1;
    }

    std::fprintf(stderr, "%s, \"%s\" (%s)\n", game.GameId().c_str(), game.Title().c_str(), game.source.c_str());

    // Only archive mode rips a title. The ripper looks up no driver profile for one. A disc with a JAudio2 profile is a
    // JAudio2 game. So is a disc that has no profile for NW4R SND and holds JAudio2's start-up archive. The sound
    // archives of NW4R SND on such a disc, like the Home Button menu's archive, aren't the game's music.
    const bool title = game.IsTitle();
    const std::optional<rip::JAudioProfile> jaudio = title ? std::nullopt : rip::FindJAudioProfile(game);
    const std::optional<rip::GameProfile> profile = title || jaudio ? std::nullopt : rip::FindProfile(game);
    if (jaudio || (!title && !profile && rip::FindJAudioFiles(game)))
    {
        return RipJAudioGame(input, out_dir, game, jaudio ? &*jaudio : nullptr, o, registries);
    }

    const rip::GameProfile* other_release =
        profile || title ? nullptr : ProfileOfOtherRelease(rip::KnownProfiles(), game.GameId());
    const std::optional<ntdlib::Microcode> microcode = ntdlib::FindMicrocode(game.dol);
    if (o.list)
    {
        ListArchives(game);
        if (title)
        {
            std::fprintf(stderr, "WiiWare titles rip in archive mode only\n");
        }
        else if (profile)
        {
            std::fprintf(stderr, "profile: %s (game mode)\n", profile->name.c_str());
            NoteDerivedProfile(*profile);
        }
        else if (other_release)
        {
            std::fprintf(stderr, "no driver profile for this release: archive mode only\n");
            std::fprintf(stderr, "note: WSF's driver profile is for %s.\n", other_release->name.c_str());
        }
        else
        {
            std::fprintf(stderr, "no driver profile for this game: archive mode only\n");
        }

        return 0;
    }

    if (title && o.mode == "game")
    {
        std::fprintf(stderr, "error: WiiWare titles rip in archive mode only. Leave out --mode game.\n");
        return 2;
    }

    // Without a sound archive of NW4R SND, a game that isn't a JAudio2 game holds nothing that WSF rips.
    const auto archives = game.SoundArchives();
    if (archives.empty())
    {
        std::fprintf(stderr, "error: the game has no NW4R SND sound archive (.brsar)\n");
        return 1;
    }

    if (!microcode)
    {
        std::fprintf(stderr, "error: the game's main.dol has no AX microcode\n");
        return 1;
    }

    const GameCode code{*microcode, ntdlib::FindSndBuild(game.dol), profile ? &*profile : nullptr};

    const std::string mode = o.mode.empty() ? (profile ? "game" : "archive") : o.mode;
    if (mode == "game" && !profile)
    {
        std::fprintf(stderr,
                     "error: no driver profile for this game (known: %s); --mode archive rips its sound archives "
                     "instead\n",
                     ProfileNames(rip::KnownProfiles()).c_str());
        return 1;
    }

    if (mode == "game")
    {
        NoteDerivedProfile(*profile);
    }

    if (mode == "archive" && title)
    {
        std::fprintf(stderr, "%s", kTitleNotice);
    }
    else if (mode == "archive")
    {
        if (other_release)
        {
            std::fprintf(stderr, "no driver profile for this release: ripping in archive mode\n");
            std::fprintf(stderr, "note: WSF's driver profile is for %s.\n", other_release->name.c_str());
        }
        else if (!profile)
        {
            std::fprintf(stderr, "no driver profile for this game: ripping in archive mode\n");
        }

        std::fprintf(stderr, "%s", profile ? kArchiveModeChosenNotice : kArchiveModeNotice);
    }

    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
        return 1;
    }

    const std::string game_name = o.game_name.empty() ? (profile ? profile->name : game.Title()) : o.game_name;
    const std::vector<std::string> folders = xsflib::rip::ArchiveFolders(archives);
    int total = 0;
    std::size_t matched = 0;
    bool failed = false; // an archive that can't be ripped doesn't stop the others
    bool rejected = false;
    for (std::size_t a = 0; a < archives.size(); a++)
    {
        const std::string& path = archives[a];
        if (!o.only_archive.empty() && path != o.only_archive)
        {
            continue;
        }

        std::vector<uint8_t> archive_bytes;
        if (!game.read_file(path, archive_bytes))
        {
            std::fprintf(stderr, "error: can't read %s\n", path.c_str());
            failed = true;
            continue;
        }

        // Each archive's set goes in a separate folder.
        const fs::path archive_dir = out_dir / folders[a];
        xsflib::rip::FileNames& names = xsflib::cli::NamesIn(registries, archive_dir, ".wsflib", ".miniwsf");
        const std::string lib_base = game_name + " " + folders[a];
        int n = 0;
        try
        {
            n = mode == "game"
                    ? RipArchiveInGameMode(archive_dir, names, game, *profile, path, archive_bytes, code, lib_base,
                                           game_name, o, matched)
                    : RipArchive(archive_dir, names, lib_base, path, archive_bytes, code, game_name, o, matched);
        }
        catch (const xsflib::FormatError& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
            n = -1;
        }

        if (n < 0)
        {
            failed = true;
            rejected |= n == kMicrocodeRejected;
            continue;
        }

        total += n;
    }

    if (rejected && profile)
    {
        std::fprintf(stderr, "note: game mode can rip this game. Leave out --mode archive.\n");
    }
    else if (rejected && other_release)
    {
        std::fprintf(stderr, "note: game mode can rip the release with the driver profile: %s.\n",
                     other_release->name.c_str());
    }

    if (matched == 0 && (!o.only_text.empty() || o.bgm) && !failed)
    {
        return NoSoundMatches(o);
    }

    std::fprintf(stderr, "%s: %s\n", xsflib::rip::InputPath(input).filename().string().c_str(),
                 Counted(total, "miniwsf file").c_str());
    if (total > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return failed ? 1 : 0;
}

// Reads AX's microcode and the build of NW4R SND from the main.dol at `path`, and finds its driver profile. Prints an
// error and returns nothing if the file can't be read or holds no AX microcode.
std::optional<GameCode> ReadGameCode(const std::string& path)
{
    std::vector<uint8_t> dol;
    if (!xsflib::psf::ReadWholeFile(path, dol))
    {
        std::fprintf(stderr, "error: can't read %s\n", path.c_str());
        return std::nullopt;
    }

    const std::optional<ntdlib::Microcode> microcode = ntdlib::FindMicrocode(dol);
    if (!microcode)
    {
        std::fprintf(stderr, "error: %s has no AX microcode\n", path.c_str());
        return std::nullopt;
    }

    return GameCode{*microcode, ntdlib::FindSndBuild(dol), ProfileOfDol(dol)};
}

// Rips a loose sound archive in archive mode into `out_dir`. `names` is the registry of file names for `out_dir`.
// Returns the exit code.
int RipLooseArchive(const std::string& input, const fs::path& out_dir, const Options& o, xsflib::rip::FileNames& names)
{
    const std::string file_name = xsflib::rip::InputPath(input).filename().string();
    if (o.mode == "game")
    {
        std::fprintf(stderr, "error: %s: game mode requires the game itself, not just its sound archive\n",
                     file_name.c_str());
        return 2;
    }
    if (!o.only_archive.empty())
    {
        std::fprintf(stderr, "error: %s is already a sound archive; --archive selects one of a game's sound archives\n",
                     input.c_str());
        return 2;
    }

    std::vector<uint8_t> archive_bytes;
    if (!xsflib::psf::ReadWholeFile(input, archive_bytes))
    {
        std::fprintf(stderr, "error: can't read %s\n", input.c_str());
        return 1;
    }

    // The main.dol comes from --microcode, then from the inputs, then from the archive's folder.
    std::string microcode_file = o.microcode_file.empty() ? o.input_microcode_file : o.microcode_file;
    std::optional<GameCode> code = o.microcode_file.empty() ? o.input_microcode : o.microcode;
    if (!code)
    {
        const fs::path beside = xsflib::rip::InputPath(input).parent_path() / "main.dol";
        std::error_code ec;
        if (fs::is_regular_file(beside, ec))
        {
            microcode_file = beside.string();
            code = ReadGameCode(microcode_file);
        }
    }

    if (o.list)
    {
        std::fprintf(stderr, "microcode: %s\n",
                     code ? microcode_file.c_str() : "none found; ripping requires the main.dol of the archive's game");
        std::fprintf(stderr, "%s: a sound archive (archive mode)\n", input.c_str());
        return 0;
    }

    if (!code)
    {
        std::fprintf(stderr,
                     "error: %s: a sound archive doesn't contain AX's microcode. Give the main.dol of the\n"
                     "       game it comes from with the archive (or with --microcode), or put that main.dol in\n"
                     "       the archive's folder. The rip stores a copy of the microcode.\n",
                     file_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "microcode: from %s\n", microcode_file.c_str());
    std::fprintf(stderr, "%s", kArchiveFileNotice);

    const std::string game_name = o.game_name.empty() ? xsflib::rip::NameOfInput(input) : o.game_name;
    std::size_t matched = 0;
    const int count = RipArchive(out_dir, names, game_name, file_name, archive_bytes, *code, game_name, o, matched);
    if (count == kMicrocodeRejected && code->profile)
    {
        std::fprintf(stderr, "note: game mode can rip %s. Rip its disc image or extracted folder instead.\n",
                     code->profile->name.c_str());
    }

    if (count < 0)
    {
        return 1;
    }

    if (matched == 0 && (!o.only_text.empty() || o.bgm))
    {
        return NoSoundMatches(o);
    }

    if (count > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return 0;
}

// True for a main.dol. The .dol extension identifies it. No disc image or sound archive uses that extension.
bool IsDol(const std::string& path)
{
    return xsflib::AsciiLower(fs::path(path).extension().string()) == ".dol";
}

// Removes the main.dol files from the inputs. Loose sound archives require the AX microcode and the build of NW4R SND
// in them. Returns 0, or the exit code.
int TakeDolInputs(std::vector<std::string>& inputs, Options& o)
{
    const auto dols_begin =
        std::stable_partition(inputs.begin(), inputs.end(), [](const auto& i) { return !IsDol(i); });
    const std::vector<std::string> dols(dols_begin, inputs.end());
    inputs.erase(dols_begin, inputs.end());

    if (dols.size() + !o.microcode_file.empty() > 1)
    {
        std::fprintf(stderr, "error: give only one main.dol\n");
        return 2;
    }

    if (!dols.empty())
    {
        o.input_microcode_file = dols[0];
        o.input_microcode = ReadGameCode(dols[0]);
        if (!o.input_microcode)
        {
            return 1;
        }
    }

    if (inputs.empty())
    {
        std::fprintf(stderr,
                     "error: %s is a main.dol. It only provides AX's microcode and the build of NW4R SND for sound "
                     "archives. Give it together with the archives.\n",
                     dols[0].c_str());
        return 2;
    }

    return 0;
}

// Rips one input. The output folders take their registries of file names from `registries`. Returns the exit code.
int RipInput(const std::string& input, const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    const std::string head = xsflib::rip::ReadHead(input, 4);
    if (head.starts_with("CSAR") || head.starts_with("FSAR"))
    {
        std::fprintf(stderr, "error: %s is a %s sound archive; WSF rips Wii sound archives (RSAR, .brsar)\n",
                     input.c_str(), head.starts_with("CSAR") ? "3DS" : "Wii U or Switch");
        return 1;
    }

    const fs::path out_dir = o.out_dir.empty() ? xsflib::rip::DefaultOutputDir(input, "_wsf") : fs::path(o.out_dir);
    if (head.starts_with("RSAR"))
    {
        return RipLooseArchive(input, out_dir, o, xsflib::cli::NamesIn(registries, out_dir, ".wsflib", ".miniwsf"));
    }

    return RipGame(input, out_dir, o, registries);
}

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::RipTool<Options> tool;
    tool.name = "wsfrip";
    tool.version = WSF_VERSION;
    tool.usage = kUsage;

    // Each analysis emulates 88 MiB of main memory. A 32-bit build has only 2 GiB of address space. It runs at most 8
    // analyses.
    tool.max_jobs_32bit = 8;

    tool.parse_option = [](const std::string& option, const std::function<std::string()>& next, Options& o)
    {
        if (option != "--microcode")
        {
            return -1;
        }

        o.microcode_file = next();

        return 0;
    };

    // The main.dol from --microcode, then the main.dol files among the inputs.
    tool.before_inputs = [](std::vector<std::string>& inputs, Options& o)
    {
        if (!o.microcode_file.empty())
        {
            o.microcode = ReadGameCode(o.microcode_file);
            if (!o.microcode)
            {
                return 1;
            }
        }

        return TakeDolInputs(inputs, o);
    };

    tool.rip_input = RipInput;

    return xsflib::cli::RunRipTool(tool, argc, argv);
}
