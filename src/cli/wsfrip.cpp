// SPDX-License-Identifier: MIT

// wsfrip: builds WSF sets (.wsflib + .miniwsf files) from Wii games and sound archives.
//
// usage: wsfrip [options] <input> [<input> ...]
//   <input>          a Wii disc image with a decrypted game partition (NKit ISO or GCZ, RVZ, WIA, or an ISO, GCZ, WBFS,
//                    CISO or WDF made from one), an extracted disc, an unpacked WiiWare title (a folder with the
//                    title's TMD and decrypted contents), or a sound archive (.brsar). A title rips in archive mode
//                    only. A game whose sound library is JAudio (JAudio1 or JAudio2) rips in game mode only, and
//                    requires a driver profile.
//                    A main.dol among the inputs isn't ripped. The sound archives among the inputs take AX's microcode
//                    and the build of NW4R SND from it.
//   -o, --output DIR the folder for the rips (default: a folder next to each input, named after the input with "_wsf"
//                    added)
//   --list           list the game's sound archives, or a JAudio game's sounds (and a JAudio2 game's sections), and
//                    whether it has a driver profile, then stop
//   --mode game|archive
//                    game mode runs the game's code and requires a driver profile for the game (src/rip/profiles.cpp).
//                    It's the default when a profile exists. Archive mode plays the sound archives with WSF's model of
//                    the SDK's sound player instead.
//   --microcode PATH the game code for a sound archive: the main.dol of the game that the archive comes from, the
//                    game's main program compressed with the CX library (a WiiWare title's content 1), or the game's
//                    extracted disc or unpacked title. Archive mode on a sound archive reads AX's microcode and the
//                    build of NW4R SND from it. By default they come from a main.dol given with the archive, or else
//                    from one in the archive's folder. Either may be compressed too.
//   --archive PATH   rip only this sound archive of a game (its path on the disc). A JAudio game has none.
//   --tracks LIST    archive mode: play only these tracks (0 to 15, and ranges such as 8-14), and write them in the
//                    wsf_tracks tag. The other tracks get a track volume of 0.
//   --tempo RATIO    archive mode: multiply the tempo by RATIO (0.25 to 4), and write it in the wsf_tempo tag
//   --var N=V        archive mode: set variable N (0 to 15 the player's, 16 to 31 the global ones) to V before each
//                    sequence starts, and write it in the wsf_var tag; repeat it for more variables
//   --suffix TEXT    archive mode: add TEXT to each rip's title and file name. Rips of one sound in several
//                    arrangements can then share a folder.
//   --only REGEX     rip only sounds whose label matches (ECMAScript regex)
//   --bgm            rip only the music (rip::IsMusic): sequences labelled as BGM, or that play on a music player. In a
//                    JAudio2 game, the sounds labelled as BGM or in the section named BGM. In a JAudio1 game, every
//                    piece of music.
//   --no-length      skip length analysis (no length or fade tags, only the sequences at a volume of 0 are found to
//                    make no sound, and every JAudio sound is taken to start)
//   --jobs N         the number of length analyses run in parallel (default: the number of CPU threads, at most 8 in a
//                    32-bit build)
//   --game NAME      game tag (default: the profile's name, or else the game's title, or the archive's name)
//   --artist NAME, --year YEAR, --copyright TEXT, --by NAME (wsfby)
//   --version        print the version
//
// Each of a game's sound archives gets a separate folder in the output folder. The folder is named after the archive.
// Where two of the game's archives have the same name, the names of the folders above them are added. A JAudio game's
// sounds make one set, in the output folder. Sequences whose data isn't all there are listed and skipped. So are those
// that make no sound: those whose volume in the archive is 0, and those that the length analysis finds never sound. The
// inputs are ripped one after another. An input that fails doesn't stop the rest. Files dropped on the program become
// inputs.

#include <algorithm>
#include <cstddef>
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

#include "ntdlib/disc/cx.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/flipper/wave_list.h"
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
#include "xsflib/text.h"

#include "nw4r/arrangement.h"
#include "nw4r/effect_setup.h"
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

// The most that a compressed main program may decompress to: the Wii's memory, 24 MiB of MEM1 and 64 MiB of MEM2.
constexpr std::size_t kMaxMainProgramSize = std::size_t{88} << 20;

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

// What --list says, and what a rip's error says, about a game with neither kind of sound archive that WSF reads.
// Such a game holds nothing that WSF rips.
constexpr char kNoSoundArchive[] =
    "the game has no sound archive that WSF reads: no NW4R SND archive (.brsar) and no JAudio2 start-up archive";

constexpr char kUsage[] =
    "usage: wsfrip [options] <input> [<input> ...]\n"
    "\n"
    "Rips each input into a .wsflib and a .miniwsf per sound. The output folder is next to the\n"
    "input and has the input's name with \"_wsf\" added. An input is a Wii disc image (ISO, NKit,\n"
    "RVZ, WIA, GCZ, WBFS, CISO or WDF), an extracted disc, an unpacked WiiWare title or a sound\n"
    "archive (.brsar). The game partition of a disc image or extracted disc must be stored\n"
    "decrypted. NKit, RVZ and WIA images store it that way. An unpacked title is a folder with the\n"
    "title's TMD (.tmd) and its decrypted contents (.app). A title rips in archive mode only. A\n"
    "game whose sound library is JAudio rips in game mode only. A sound archive requires the\n"
    "main.dol of its game. The ripper takes AX's microcode and the build of the sound library from\n"
    "it. Give the main.dol with the archive or put it in the archive's folder. A WiiWare game has\n"
    "no main.dol. Give its unpacked title with --microcode instead. You can also drop files on the\n"
    "program. Sequences that make no sound, and those whose data isn't all there, are listed and\n"
    "skipped.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list a game's sound archives, or a JAudio game's sounds, and whether\n"
    "                       it has a driver profile, then stop\n"
    "  --mode game|archive  game mode runs the game's code and requires a driver profile for the\n"
    "                       game. It's the default when a profile exists. Archive mode plays the\n"
    "                       sound archives with WSF's model of the SDK's sound player.\n"
    "  --microcode PATH     the game code for a sound archive, for AX's microcode and the build of\n"
    "                       the sound library in archive mode: the main.dol of the archive's\n"
    "                       game, the game's compressed main program (a WiiWare title's content\n"
    "                       1), or the game's extracted disc or unpacked title\n"
    "  --archive PATH       rip only this sound archive of a game (its path on the disc)\n"
    "  --tracks LIST        archive mode: play only these tracks (0-15, and ranges such as 8-14).\n"
    "                       The other tracks are silent. Writes the wsf_tracks tag.\n"
    "  --tempo RATIO        archive mode: multiply the tempo by RATIO (0.25 to 4). Writes the\n"
    "                       wsf_tempo tag.\n"
    "  --var N=V            archive mode: set variable N (0-15 the player's, 16-31 the global\n"
    "                       ones) to V before each sequence starts. N and V can be hex (0x...).\n"
    "                       Repeat it for more variables. Writes the wsf_var tag.\n"
    "  --suffix TEXT        archive mode: add TEXT to each rip's title and file name. Rips of a\n"
    "                       sound in several arrangements can then share a folder.\n"
    "  --only REGEX         rip only the sounds whose label matches\n"
    "  --bgm                rip only the music: sequences labelled as BGM, or that play on a player\n"
    "                       named for music (BGM, MUSIC, SONG or FANFARE). In a JAudio2 game, the\n"
    "                       sounds labelled as BGM or in the section named BGM. In a JAudio1\n"
    "                       game, the pieces of music.\n"
    "  --no-length          skip length analysis and omit length and fade tags. The analysis also\n"
    "                       finds the sequences that make no sound, and the JAudio sounds that\n"
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
// that build. Archive mode plays the model, and the length analyses of both modes run it.
struct GameCode
{
    ntdlib::Microcode microcode;
    nw4r::SndBuild build;
    std::string dol_sha1; // the main.dol's SHA-1, for the game's profiles
};

// What WSF's model reads from `dol`, or nothing if `dol` holds no AX microcode.
std::optional<GameCode> GameCodeOf(const std::vector<uint8_t>& dol)
{
    const std::optional<ntdlib::Microcode> microcode = ntdlib::FindMicrocode(dol);
    if (!microcode)
    {
        return std::nullopt;
    }

    return GameCode{*microcode, ntdlib::FindSndBuild(dol), xsflib::Sha1::Hex(xsflib::Sha1::Of(dol))};
}

// Notes once for each game in a run that WSF has no effect profile for it (see rip::HasEffectProfile).
void NoteNoEffectProfile(const GameCode& code)
{
    static std::set<std::string> noted;
    if (!rip::HasEffectProfile(code.dol_sha1) && noted.insert(code.dol_sha1).second)
    {
        std::fprintf(stderr,
                     "note: WSF has no effect profile for this game: its rips play without the game's reverb\n"
                     "      and other effects\n");
    }
}

// RipArchive's result when the model can't drive the game's AX microcode.
constexpr int kMicrocodeRejected = -2;

// The rip frame's options, --microcode, and the options that arrange the sequences in archive mode.
struct Options : xsflib::cli::RipOptions
{
    // The main.dol from --microcode or from the inputs: its path, and what WSF's model reads from it. Only loose sound
    // archives use them.
    std::string microcode_file;
    std::optional<GameCode> microcode;

    // --tracks, --tempo (its text and its ratio), --var and --suffix.
    std::optional<uint16_t> tracks;
    std::string tempo_text;
    std::optional<float> tempo;
    xsflib::psf::Variables variables;
    std::string suffix;

    // The options among --tracks, --tempo, --var and --suffix that were given, with the verb for a message: "--tempo
    // is", "--tracks and --tempo are". Empty if none was.
    std::string ArchiveOptionsGiven() const
    {
        std::vector<std::string> given;
        if (tracks)
        {
            given.emplace_back("--tracks");
        }
        if (tempo)
        {
            given.emplace_back("--tempo");
        }
        if (!variables.empty())
        {
            given.emplace_back("--var");
        }
        if (!suffix.empty())
        {
            given.emplace_back("--suffix");
        }

        std::string text;
        for (std::size_t i = 0; i < given.size(); i++)
        {
            text += (i == 0 ? "" : i + 1 == given.size() ? " and " : ", ") + given[i];
        }

        return given.empty() ? text : text + (given.size() == 1 ? " is" : " are");
    }

    // The arrangement that --tracks, --tempo and --var give.
    nw4r::Arrangement ToArrangement() const
    {
        nw4r::Arrangement arrangement;
        arrangement.tracks = tracks.value_or(arrangement.tracks);
        arrangement.tempo_ratio = tempo.value_or(arrangement.tempo_ratio);
        arrangement.variables = variables;

        return arrangement;
    }

    // The tags that --tracks, --tempo and --var write into each mini.
    void AddArrangementTags(Tags& tags) const
    {
        if (tracks)
        {
            tags[kTracksTag] = FormatTracks(*tracks);
        }
        if (tempo)
        {
            tags[kTempoTag] = tempo_text;
        }
        if (!variables.empty())
        {
            tags[kVariablesTag] = xsflib::psf::FormatVariables(variables);
        }
    }
};

struct Job
{
    uint32_t index;
    std::string label;
    std::optional<rip::Timing> timing;
    std::optional<rip::SoundData> data; // game mode: what the driver loads for the sound

    // Archive mode: the sounds that the rip plays, in the order in which they start. A piece whose channels the game
    // plays as separate sequences has all of them (see rip::FindPieces), the first at `index`. Empty in game mode.
    std::vector<uint32_t> sounds;

    // The sounds that the rip plays: `index` alone in game mode.
    std::vector<uint32_t> Sounds() const
    {
        return sounds.empty() ? std::vector<uint32_t>{index} : sounds;
    }
};

// A count and its noun: "1 sound", "2 sounds".
std::string Counted(std::size_t count, const std::string& noun)
{
    return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

// Length analysis runs every selected sequence through the model and AX's microcode on the DSP, in `arrangement`. The
// sounds are independent. The analysis spreads them over several threads. Each sequence of a piece is measured alone,
// with the effects of the piece's first sound. The piece then gets the latest end (rip::TimingOfPiece).
void AnalyzeLengths(std::vector<Job>& jobs, const ntdlib::rsar::SoundArchive& archive,
                    const std::vector<uint8_t>& archive_bytes, const GameCode& code,
                    const nw4r::Arrangement& arrangement, const nw4r::ArchiveEffects& effects, unsigned threads)
{
    struct Measure
    {
        uint32_t index;
        uint32_t effects_sound;
        std::string label; // for the warning of a failed analysis
        std::optional<rip::Timing> timing;
    };

    std::vector<Measure> measures;
    for (const Job& job : jobs)
    {
        const std::vector<uint32_t> sounds = job.Sounds();
        for (const uint32_t sound : sounds)
        {
            const std::string& label = sounds.size() > 1 ? archive.Sounds()[sound].name : job.label;
            measures.push_back({sound, sounds.front(), label, std::nullopt});
        }
    }

    const rip::LengthAnalyzer analyzer(archive_bytes, code.microcode, code.build, arrangement, effects);
    const auto analyze = [&analyzer](Measure& measure)
    {
        measure.timing = analyzer.Analyze(measure.index, measure.effects_sound);

        return true;
    };
    xsflib::cli::AnalyzeLengths(measures, threads, analyze,
                                {.out_of_memory = "out of memory during length analysis; try fewer --jobs"});

    std::size_t next = 0;
    for (Job& job : jobs)
    {
        std::vector<std::optional<rip::Timing>> timings;
        for (std::size_t i = job.Sounds().size(); i > 0; i--)
        {
            timings.push_back(measures[next++].timing);
        }

        job.timing = rip::TimingOfPiece(timings);
    }
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
            jobs.push_back({index, sounds[i].name, std::nullopt, std::nullopt, {}});
        }
    }

    return jobs;
}

// Archive mode: the sequences that --only and --bgm pick. A piece whose channels the game plays as separate sequences
// makes one job (see rip::FindPieces). --only picks it by its name or by any of its sequences' labels, and --bgm by any
// of its sequences.
std::vector<Job> SelectArchiveSounds(const ntdlib::rsar::SoundArchive& archive, const Options& o)
{
    const auto& sounds = archive.Sounds();
    const std::vector<rip::Piece> pieces = rip::FindPieces(archive);
    std::vector<const rip::Piece*> piece_of(sounds.size(), nullptr);
    for (const rip::Piece& piece : pieces)
    {
        for (const uint32_t sound : piece.sounds)
        {
            piece_of[sound] = &piece;
        }
    }

    std::vector<Job> jobs;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        // Streams are out of scope, and wave sounds aren't ripped yet. A piece's job comes at its first sound.
        const auto index = static_cast<uint32_t>(i);
        const rip::Piece* piece = piece_of[i];
        if (sounds[i].type != ntdlib::rsar::SoundType::kSequence || (piece && piece->sounds.front() != index))
        {
            continue;
        }

        const std::string label = piece ? piece->name : sounds[i].name;
        const std::vector<uint32_t> members = piece ? piece->sounds : std::vector<uint32_t>{index};
        bool matches = std::regex_search(label, o.only);
        bool music = false;
        for (const uint32_t sound : members)
        {
            matches = matches || std::regex_search(sounds[sound].name, o.only);
            music = music || rip::IsMusic(archive, sound);
        }

        if (matches && (!o.bgm || music))
        {
            jobs.push_back({index, label, std::nullopt, std::nullopt, members});
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

// Takes out the sequences whose volume in the archive is 0, and the pieces whose sequences all have a volume of 0. They
// make no sound. The game's sound library multiplies everything that a sound plays by its volume. Returns the number
// taken out.
std::size_t SkipMuted(std::vector<Job>& jobs, const ntdlib::rsar::SoundArchive& archive)
{
    const auto muted = [&archive](const Job& job)
    {
        const std::vector<uint32_t> sounds = job.Sounds();
        if (std::any_of(sounds.begin(), sounds.end(),
                        [&archive](uint32_t sound) { return archive.Sounds()[sound].volume != 0; }))
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

// Takes out the sequences whose data isn't all in the archive, and the pieces with such a sequence. They would play
// with missing instruments or channels. Returns the number taken out.
std::size_t SkipIncomplete(std::vector<Job>& jobs, const ntdlib::rsar::SoundArchive& archive)
{
    const auto incomplete = [&archive](const Job& job)
    {
        const std::vector<uint32_t> sounds = job.Sounds();
        std::string what;
        for (const uint32_t sound : sounds)
        {
            const std::vector<std::string> missing = rip::MissingData(archive, sound);
            std::string list;
            for (std::size_t i = 0; i < missing.size(); i++)
            {
                list += (i ? ", " : "") + missing[i];
            }

            // A piece names each sequence whose data is missing.
            if (!list.empty() && sounds.size() > 1)
            {
                list = archive.Sounds()[sound].name + " (" + list + ")";
            }

            if (!list.empty())
            {
                what += (what.empty() ? "" : "; ") + list;
            }
        }

        if (what.empty())
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: not in this archive: %s\n", job.label.c_str(), what.c_str());

        return true;
    };
    return std::erase_if(jobs, incomplete);
}

// Game mode: finds what the driver loads for each sequence. The driver loads the groups that hold the sequence and its
// bank. Takes out the sequences that no group holds. Returns the number taken out.
std::size_t SkipUngrouped(std::vector<Job>& jobs, const ntdlib::rsar::SoundArchive& archive)
{
    for (Job& job : jobs)
    {
        job.data = rip::FindSoundData(archive, job.index);
    }

    const auto ungrouped = [](const Job& job)
    {
        if (job.data)
        {
            return false;
        }

        std::fprintf(stderr, "skipped %s: no group in the archive holds its sequence, or its bank\n",
                     job.label.c_str());

        return true;
    };
    return std::erase_if(jobs, ungrouped);
}

// Takes out the sequences at a volume of 0. With the length analysis on, measures the others in `arrangement` with
// `effects`, and takes out those that never sound. Returns the number taken out.
std::size_t MeasureSequences(std::vector<Job>& jobs, const ntdlib::rsar::SoundArchive& archive,
                             const std::vector<uint8_t>& archive_bytes, const GameCode& code,
                             const nw4r::Arrangement& arrangement, const nw4r::ArchiveEffects& effects,
                             const Options& o)
{
    std::size_t silent = SkipMuted(jobs, archive);
    if (o.lengths && !jobs.empty())
    {
        AnalyzeLengths(jobs, archive, archive_bytes, code, arrangement, effects, o.jobs);
        silent += SkipSilent(jobs);
    }

    return silent;
}

// Writes a rip's library `lib` into `out_dir`, with the name that LibraryName gives it from `base`. Returns the name.
// For a library that can't be written, prints the error and returns nothing.
std::optional<std::string> WriteLibrary(const fs::path& out_dir, xsflib::rip::FileNames& names, const std::string& base,
                                        const std::vector<uint8_t>& lib)
{
    const std::string lib_name = rip::LibraryName(out_dir, names, base, lib);
    fs::create_directories(out_dir);
    if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(lib_name), lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return std::nullopt;
    }

    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    return lib_name;
}

// Writes a rip's set into `out_dir`: the library `lib` (see WriteLibrary), then a mini for each of `jobs` with the tags
// `tags`. `build(lib_name, job, tags)` makes a job's mini. Then prints how many minis the rip of `source` wrote, and
// `skipped_text` (see xsflib::cli::SkippedText). Returns that number, or -1 after printing an error.
template <typename J, typename Build>
int WriteSet(const fs::path& out_dir, xsflib::rip::FileNames& names, const std::string& lib_base,
             const std::vector<uint8_t>& lib, const std::vector<J>& jobs, const Tags& tags, const Build& build,
             const std::string& source, const std::string& skipped_text)
{
    const std::optional<std::string> lib_name = WriteLibrary(out_dir, names, lib_base, lib);
    if (!lib_name)
    {
        return -1;
    }

    const auto build_mini = [&build, &lib_name](const J& job, Tags& mini_tags)
    {
        return build(*lib_name, job, mini_tags);
    };
    const int count = xsflib::cli::WriteMinis(out_dir, names, jobs, tags, "wsf_sound", build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %s%s\n", source.c_str(), Counted(count, "miniwsf file").c_str(),
                     skipped_text.c_str());
    }

    return count;
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

    std::vector<Job> jobs = SelectArchiveSounds(*archive, o);
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

    const std::size_t skipped = SkipIncomplete(jobs, *archive);

    // --tracks replaces the tracks that the game plays of a sound. The tag does the same in the player.
    const nw4r::ArchiveEffects effects = rip::FindEffects(code.dol_sha1, archive_path, *archive);
    nw4r::ArchiveEffects analyzed = effects;
    if (o.tracks)
    {
        analyzed.tracks.clear();
    }

    const std::size_t silent = MeasureSequences(jobs, *archive, archive_bytes, code, o.ToArrangement(), analyzed, o);
    const std::string skipped_text = xsflib::cli::SkippedText(skipped, "data outside the archive", silent, "sequence");
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", archive_path.c_str(), skipped_text.c_str());
        return 0;
    }

    // The library keeps only the data that the ripped sequences play. A game's archive can hold hundreds of megabytes
    // of sound effects and voices that a rip of its music doesn't play. A set whose minis play several sounds gets
    // version 2 of the archive descriptor. Players of WSF 1.5.0 and older refuse it.
    std::vector<uint32_t> indices;
    uint32_t version = ArchiveDescriptor::kVersion;
    for (const Job& job : jobs)
    {
        indices.insert(indices.end(), job.sounds.begin(), job.sounds.end());
        if (job.sounds.size() > 1)
        {
            version = ArchiveDescriptor::kListVersion;
        }
    }

    const Tags common = xsflib::cli::RipTags(o, game_name, "wsf", "archive");
    const auto lib = rip::BuildArchiveLibrary(archive_path, rip::KeepSequenceData(*archive, indices), kMicrocodeName,
                                              code.microcode, code.build, common, effects, version);

    // The arrangement's tags go in the minis. The library then stays the same for every arrangement of its sounds.
    // --suffix goes in each mini's title and file name. The wsf_sound tag keeps the sound's label, or the labels of a
    // piece's sequences.
    Tags mini_tags = common;
    o.AddArrangementTags(mini_tags);
    for (Job& job : jobs)
    {
        job.label += o.suffix;
    }

    const auto build_mini = [&archive](const std::string& lib_name, const Job& job, Tags tags)
    {
        std::string labels;
        for (const uint32_t sound : job.sounds)
        {
            labels += (labels.empty() ? "" : ", ") + archive->Sounds()[sound].name;
        }

        tags["wsf_sound"] = labels;
        return rip::BuildArchiveMini(job.sounds, lib_name, std::move(tags));
    };
    return WriteSet(out_dir, names, lib_base, lib, jobs, mini_tags, build_mini, archive_path, skipped_text);
}

// Rips one of a game's sound archives in game mode into `out_dir`. `names` is the registry of file names for `out_dir`.
// Adds the number of sounds that --only selected to `matched`. Returns the number of .miniwsf files written, or -1 on
// an error.
int RipArchiveInGameMode(const fs::path& out_dir, xsflib::rip::FileNames& names, const std::string& lib_base,
                         const ntdlib::disc::GameFiles& game, const rip::GameProfile& profile,
                         const std::string& archive_path, const std::vector<uint8_t>& archive_bytes,
                         const GameCode& code, const std::string& game_name, const Options& o, std::size_t& matched)
{
    const auto archive = ntdlib::rsar::SoundArchive::Load(archive_bytes, wsf::nw4r::kRsarOptions);
    std::vector<Job> jobs = SelectSounds(archive, o);
    matched += jobs.size();
    if (jobs.empty())
    {
        return 0;
    }

    const std::size_t skipped = SkipUngrouped(jobs, archive);
    const nw4r::ArchiveEffects effects = rip::FindEffects(code.dol_sha1, archive_path, archive);
    const std::size_t silent = MeasureSequences(jobs, archive, archive_bytes, code, {}, effects, o);
    const std::string skipped_text = xsflib::cli::SkippedText(skipped, "no group holds them", silent, "sequence");
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", archive_path.c_str(), skipped_text.c_str());
        return 0;
    }

    const Tags common = xsflib::cli::RipTags(o, game_name, "wsf", "game");
    std::string error;
    const auto lib = rip::BuildLibrary(game, profile, archive_path, archive, common, &error, effects);
    if (!lib)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return -1;
    }

    const auto build_mini = [&effects, &code](const std::string& lib_name, const Job& job, const Tags& tags)
    {
        return rip::BuildMini(job.index, *job.data, lib_name, tags, effects, code.dol_sha1);
    };
    return WriteSet(out_dir, names, lib_base, *lib, jobs, common, build_mini, archive_path, skipped_text);
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

// The profile in `profiles` of another release of the game with `game_id` (see rip::SameGame), or nullptr.
template <typename Profile>
const Profile* ProfileOfOtherRelease(const std::vector<Profile>& profiles, const std::string& game_id)
{
    for (const Profile& profile : profiles)
    {
        if (rip::SameGame(profile.game_id, game_id))
        {
            return &profile;
        }
    }

    return nullptr;
}

// Names the driver profile that WSF has for another release of the game (see ProfileOfOtherRelease).
void NoteOtherRelease(const std::string& profile_name)
{
    std::fprintf(stderr, "note: WSF's driver profile is for %s.\n", profile_name.c_str());
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

// The driver profile of the main.dol whose SHA-1 is `dol_sha1`, whatever disc it comes from, or nullptr.
const rip::GameProfile* ProfileOfDol(const std::string& dol_sha1)
{
    for (const rip::GameProfile& profile : rip::KnownProfiles())
    {
        if (dol_sha1 == profile.dol_sha1)
        {
            return &profile;
        }
    }

    return nullptr;
}

// A JAudio game's sound to rip, and the wave groups that it plays, each (system << 16) | group.
struct JAudioJob
{
    uint32_t index;    // the sound's place in the game's list of sounds
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

// A JAudio game's sounds that --only and --bgm pick. Streams are out of scope.
std::vector<JAudioJob> SelectJAudioSounds(const std::vector<rip::JAudioSound>& sounds, const Options& o)
{
    std::vector<JAudioJob> jobs;
    for (const rip::JAudioSound& sound : sounds)
    {
        if (!sound.stream && std::regex_search(sound.label, o.only) && (!o.bgm || sound.music))
        {
            jobs.push_back(
                {static_cast<uint32_t>(&sound - sounds.data()), sound.id, sound.label, std::nullopt, sound.waves, {}});
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

// The name that the messages give the family of a JAudio game's sound library. Jac and AAF are kinds of JAudio1.
const char* FamilyOf(const rip::JAudioProfile* profile)
{
    return rip::SoundLibraryOf(profile) == rip::JAudioLibrary::kJAudio2 ? "JAudio2" : "JAudio1";
}

// Lists a JAudio game's sounds, with the file that lists them: how many there are, how many of them are streams, and,
// with `with_sections`, the sections of the name table.
void ListJAudioSounds(const std::string& file, const std::vector<rip::JAudioSound>& sounds, bool with_sections)
{
    const auto is_stream = [](const rip::JAudioSound& sound)
    {
        return sound.stream;
    };
    const auto streams = std::count_if(sounds.begin(), sounds.end(), is_stream);
    std::fprintf(stderr, "%s: %s, %zu of them streams\n", file.c_str(), Counted(sounds.size(), "sound").c_str(),
                 static_cast<std::size_t>(streams));
    if (!with_sections)
    {
        return;
    }

    std::map<std::string, std::size_t> sections;
    for (const rip::JAudioSound& sound : sounds)
    {
        sections[sound.section]++;
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
        NoteOtherRelease(other_release->name);
    }
}

// The --list output for a JAudio game: its sounds (see ListJAudioSounds), then its profile, or why WSF has none. A Jac
// game's pieces are files of its sequence archive. They have no sections.
void ListJAudioGame(const std::string& game_id, const rip::JAudioFiles& files,
                    const std::vector<rip::JAudioSound>& sounds, const rip::JAudioProfile* profile)
{
    const bool jac = rip::SoundLibraryOf(profile) == rip::JAudioLibrary::kJac;
    ListJAudioSounds(jac ? files.archives.front() : files.baa, sounds, !jac);
    if (profile)
    {
        std::fprintf(stderr, "profile: %s (game mode, %s)\n", profile->name.c_str(), FamilyOf(profile));
    }
    else
    {
        ReportNoJAudioProfile(game_id, true);
    }
}

// Refuses a rip of a JAudio game for its options or the lack of a profile. The check comes before the rip parses the
// start-up archive. Returns the exit code after the error, or nothing for a rip that can go on. --archive is a usage
// error (exit code 2) and comes first. A game without a profile can't be ripped at all. The profile's error therefore
// comes before the one for --mode archive.
std::optional<int> RefuseJAudioRip(const Options& o, const rip::JAudioProfile* profile, const std::string& game_id)
{
    const char* const library = FamilyOf(profile);
    if (!o.only_archive.empty())
    {
        std::fprintf(stderr,
                     "error: --archive selects one of a game's NW4R SND sound archives, and this game's sound library "
                     "is %s\n",
                     library);
        return 2;
    }
    if (const std::string given = o.ArchiveOptionsGiven(); !given.empty())
    {
        std::fprintf(stderr, "error: %s for archive mode, and this game's sound library is %s\n", given.c_str(),
                     library);
        return 2;
    }
    if (!profile)
    {
        ReportNoJAudioProfile(game_id, false);
        return 1;
    }
    if (o.mode == "archive")
    {
        std::fprintf(stderr,
                     "error: archive mode plays NW4R SND's sequences, and this game's sound library is %s. Leave out "
                     "--mode archive.\n",
                     library);
        return 1;
    }

    return std::nullopt;
}

// Wave discovery, for the sounds in `jobs` with an empty list of groups: each plays on a library of the game's code and
// the files at `paths`, and the waves that its notes look up give its groups. Returns false after the error for a
// library that can't be built.
bool DiscoverWaves(std::vector<JAudioJob>& jobs, const ntdlib::disc::GameFiles& game, const rip::JAudioProfile& profile,
                   const std::vector<std::string>& paths, const std::vector<ntdlib::jaudio::WaveSystem>& systems,
                   unsigned threads)
{
    const auto undiscovered = static_cast<std::size_t>(
        std::count_if(jobs.begin(), jobs.end(), [](const JAudioJob& job) { return job.waves.empty(); }));
    if (undiscovered == 0)
    {
        return true;
    }

    std::string error;
    const auto base = rip::BuildJAudioLibrary(game, profile, paths, {}, &error);
    if (!base)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }

    const auto discover = [&](JAudioJob& job)
    {
        if (!job.waves.empty())
        {
            return false;
        }

        std::set<ntdlib::jaudio::WaveUse> found;
        if (!rip::FindJAudioWaves(*base, profile, job.sound_id, found))
        {
            std::fprintf(stderr, "warning: the waves of %s can't be found; it plays without them\n", job.label.c_str());
            return false;
        }

        job.waves = ntdlib::jaudio::WaveGroups(systems, {found.begin(), found.end()});
        if (job.waves.size() > ntdlib::flipper::kMaxWaves)
        {
            std::fprintf(stderr, "warning: %s plays waves from %zu groups; it loads the first %u\n", job.label.c_str(),
                         job.waves.size(), ntdlib::flipper::kMaxWaves);
            job.waves.resize(ntdlib::flipper::kMaxWaves);
        }

        // The pass gives no timing. False keeps the runner from reporting a failed length analysis.
        return false;
    };
    std::fprintf(stderr, "finding the wave groups of %s\n", Counted(undiscovered, "sound").c_str());
    xsflib::cli::AnalyzeLengths(
        jobs, threads, discover,
        {.out_of_memory = "out of memory while finding the waves; try fewer --jobs", .progress = "searched"});

    return true;
}

// Length analysis plays every sound in `jobs` on the rip's library `lib`, with its wave groups loaded. A sound that
// can't start isn't analyzed: SkipUnstartable takes it out. A sound that looks up waves outside its groups gets a
// warning.
void AnalyzeLengths(std::vector<JAudioJob>& jobs, const std::vector<uint8_t>& lib, const rip::JAudioProfile& profile,
                    const std::vector<ntdlib::jaudio::WaveSystem>& systems, unsigned threads)
{
    const auto analyze = [&](JAudioJob& job)
    {
        std::set<ntdlib::jaudio::WaveUse> played;
        job.timing = rip::AnalyzeJAudioSound(lib, profile, job.sound_id, job.waves, &played, &job.start_error);
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
    xsflib::cli::AnalyzeLengths(jobs, threads, analyze,
                                {.out_of_memory = "out of memory during length analysis; try fewer --jobs"});
}

// Game mode for a JAudio game: rips the sounds of its start-up archive, or of a Jac game's sequence index, into one
// set. A JAudio2 sound first plays with only the wave groups that the game's audio set-up loads, and the waves its
// notes look up give its groups. ReadJacSounds gives the group of each piece of a Jac game. ReadPikmin2Sounds lists the
// sounds of New Play Control! Pikmin 2 with their groups, apart from the pieces that the game never plays. Wave
// discovery finds the groups of those pieces. The library then holds every group that a ripped sound plays, and each
// sound plays with its groups for the length analysis. `files` are the game's sound files. `profile` is null for a
// JAudio2 game without a driver profile. Only --list works then.
int RipJAudioGame(const std::string& input, const fs::path& out_dir, const ntdlib::disc::GameFiles& game,
                  const rip::JAudioFiles& files, const rip::JAudioProfile* profile, const Options& o,
                  xsflib::cli::FileNameRegistries& registries)
{
    // A rip checks its options and the profile before it parses the start-up archive.
    if (!o.list)
    {
        if (const std::optional<int> refused = RefuseJAudioRip(o, profile, game.GameId()))
        {
            return *refused;
        }
    }

    // The start-up archive, or a Jac game's bank file.
    std::vector<uint8_t> baa;
    if (!rip::ReadSoundFile(game, files.baa, baa))
    {
        std::fprintf(stderr, "error: can't read %s\n", files.baa.c_str());
        return 1;
    }

    std::string error;
    const std::optional<std::vector<rip::JAudioSound>> sounds =
        rip::ReadJAudioSounds(game, files, profile, baa, &error);
    if (!sounds)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    if (o.list)
    {
        ListJAudioGame(game.GameId(), files, *sounds, profile);
        return 0;
    }

    std::vector<JAudioJob> jobs = SelectJAudioSounds(*sounds, o);
    if (jobs.empty())
    {
        if (!o.only_text.empty() || o.bgm)
        {
            return NoSoundMatches(o);
        }

        std::fprintf(stderr, "%s: nothing to rip\n", files.baa.c_str());
        return 0;
    }

    // Every library holds the sound files (see rip::LibraryFiles) and the groups that the audio set-up loads.
    const std::vector<ntdlib::jaudio::WaveSystem> systems = rip::ReadWaveSystems(profile->library, baa);
    std::vector<std::string> paths = rip::LibraryFiles(files, *profile);
    if (!AddAwFiles(game, files, systems, profile->static_waves, paths))
    {
        return 1;
    }

    if (!DiscoverWaves(jobs, game, *profile, paths, systems, o.jobs))
    {
        return 1;
    }

    for (const JAudioJob& job : jobs)
    {
        if (!AddAwFiles(game, files, systems, job.waves, paths))
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
        AnalyzeLengths(jobs, *lib, *profile, systems, o.jobs);
        unstartable = SkipUnstartable(jobs);
        silent = SkipSilent(jobs);
    }

    const std::string skipped_text = xsflib::cli::SkippedText(unstartable, "can't start", silent, "sound");
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", files.baa.c_str(), skipped_text.c_str());
        return 0;
    }

    xsflib::rip::FileNames& names = xsflib::cli::NamesIn(registries, out_dir, ".wsflib", ".miniwsf");
    const auto build_mini = [](const std::string& lib_name, const JAudioJob& job, const Tags& tags)
    {
        return rip::BuildJAudioMini(job.sound_id, job.waves, lib_name, tags);
    };
    const std::string source = xsflib::rip::InputPath(input).filename().string();
    const int count = WriteSet(out_dir, names, game_name, *lib, jobs, common, build_mini, source, skipped_text);
    if (count < 0)
    {
        return 1;
    }

    if (count > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return 0;
}

// The --list output for a game of NW4R SND: its sound archives, then how WSF rips them. `profile` is the game's driver
// profile, and `other_release` the profile of another release of the game, or null.
void ListGame(const ntdlib::disc::GameFiles& game, const rip::GameProfile* profile,
              const rip::GameProfile* other_release)
{
    ListArchives(game);
    if (game.SoundArchives().empty())
    {
        std::fprintf(stderr, "%s\n", kNoSoundArchive);
    }
    else if (game.IsTitle())
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
        NoteOtherRelease(other_release->name);
    }
    else
    {
        std::fprintf(stderr, "no driver profile for this game: archive mode only\n");
    }
}

// Notes how the game rips in `mode`, "game" or "archive", and why archive mode rips it.
void NoteMode(const std::string& mode, bool title, const rip::GameProfile* profile,
              const rip::GameProfile* other_release)
{
    if (mode == "game")
    {
        NoteDerivedProfile(*profile);
        return;
    }

    if (title)
    {
        std::fprintf(stderr, "%s", kTitleNotice);
        return;
    }

    if (other_release)
    {
        std::fprintf(stderr, "no driver profile for this release: ripping in archive mode\n");
        NoteOtherRelease(other_release->name);
    }
    else if (!profile)
    {
        std::fprintf(stderr, "no driver profile for this game: ripping in archive mode\n");
    }

    std::fprintf(stderr, "%s", profile ? kArchiveModeChosenNotice : kArchiveModeNotice);
}

// What RipArchives did with a game's sound archives.
struct ArchivesRipped
{
    int files = 0;           // the .miniwsf files written
    std::size_t matched = 0; // the sounds that --only and --bgm selected
    bool failed = false;     // whether an archive failed to rip
    bool rejected = false;   // whether the model couldn't drive the game's AX microcode
};

// Rips the game's sound archives `archives`, or the one that --archive names, in `mode`. Each archive's set goes in a
// folder of its own in `out_dir`. The folder takes its registry of file names from `registries`. An archive that fails
// doesn't stop the others.
ArchivesRipped RipArchives(const fs::path& out_dir, const ntdlib::disc::GameFiles& game,
                           const std::vector<std::string>& archives, const std::string& mode,
                           const rip::GameProfile* profile, const GameCode& code, const std::string& game_name,
                           const Options& o, xsflib::cli::FileNameRegistries& registries)
{
    ArchivesRipped ripped;
    const std::vector<std::string> folders = xsflib::rip::ArchiveFolders(archives);
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
            ripped.failed = true;
            continue;
        }

        const fs::path archive_dir = out_dir / folders[a];
        xsflib::rip::FileNames& names = xsflib::cli::NamesIn(registries, archive_dir, ".wsflib", ".miniwsf");
        const std::string lib_base = game_name + " " + folders[a];
        int n = 0;
        try
        {
            n = mode == "game"
                    ? RipArchiveInGameMode(archive_dir, names, lib_base, game, *profile, path, archive_bytes, code,
                                           game_name, o, ripped.matched)
                    : RipArchive(archive_dir, names, lib_base, path, archive_bytes, code, game_name, o, ripped.matched);
        }
        catch (const xsflib::FormatError& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
            n = -1;
        }

        if (n < 0)
        {
            ripped.failed = true;
            ripped.rejected |= n == kMicrocodeRejected;
            continue;
        }

        ripped.files += n;
    }

    return ripped;
}

// Rips a game image or folder: each of its sound archives, or the one that --archive names. Uses game mode if a driver
// profile exists. Otherwise uses archive mode. --mode overrides the choice. Each archive's folder takes its registry of
// file names from `registries` (see xsflib::cli::NamesIn). RipJAudioGame rips a JAudio game. Returns the exit code.
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

    // Only archive mode rips a title. The ripper looks up no driver profile for one. A disc with a JAudio profile is a
    // JAudio game. So is a disc that has no profile for NW4R SND and holds JAudio2's start-up archive. The sound
    // archives of NW4R SND on such a disc, like the Home Button menu's archive, aren't the game's music.
    const bool title = game.IsTitle();
    const std::optional<rip::JAudioProfile> jaudio = title ? std::nullopt : rip::FindJAudioProfile(game);
    if (jaudio)
    {
        std::string error;
        const std::optional<rip::JAudioFiles> files = rip::FindJAudioFiles(game, *jaudio, &error);
        if (!files)
        {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }

        return RipJAudioGame(input, out_dir, game, *files, &*jaudio, o, registries);
    }

    const std::optional<rip::GameProfile> found = title ? std::nullopt : rip::FindProfile(game);
    const rip::GameProfile* profile = found ? &*found : nullptr;
    if (!title && !profile)
    {
        if (const std::optional<rip::JAudioFiles> files = rip::FindJAudio2Files(game))
        {
            return RipJAudioGame(input, out_dir, game, *files, nullptr, o, registries);
        }
    }

    const rip::GameProfile* other_release =
        profile || title ? nullptr : ProfileOfOtherRelease(rip::KnownProfiles(), game.GameId());
    if (o.list)
    {
        ListGame(game, profile, other_release);
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
        std::fprintf(stderr, "error: %s\n", kNoSoundArchive);
        return 1;
    }

    const std::optional<GameCode> code = GameCodeOf(game.dol);
    if (!code)
    {
        std::fprintf(stderr, "error: the game's main.dol has no AX microcode\n");
        return 1;
    }

    const std::string mode = o.mode.empty() ? (profile ? "game" : "archive") : o.mode;
    if (const std::string given = o.ArchiveOptionsGiven(); mode == "game" && !given.empty())
    {
        std::fprintf(stderr, "error: %s for archive mode. Add --mode archive.\n", given.c_str());
        return 2;
    }

    if (mode == "game" && !profile)
    {
        std::fprintf(stderr,
                     "error: no driver profile for this game (known: %s); --mode archive rips its sound archives "
                     "instead\n",
                     ProfileNames(rip::KnownProfiles()).c_str());
        return 1;
    }

    NoteMode(mode, title, profile, other_release);

    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
        return 1;
    }

    NoteNoEffectProfile(*code);

    const std::string game_name = o.game_name.empty() ? (profile ? profile->name : game.Title()) : o.game_name;
    const ArchivesRipped ripped = RipArchives(out_dir, game, archives, mode, profile, *code, game_name, o, registries);
    if (ripped.rejected && profile)
    {
        std::fprintf(stderr, "note: game mode can rip this game. Leave out --mode archive.\n");
    }
    else if (ripped.rejected && other_release)
    {
        std::fprintf(stderr, "note: game mode can rip the release with the driver profile: %s.\n",
                     other_release->name.c_str());
    }

    if (ripped.matched == 0 && (!o.only_text.empty() || o.bgm) && !ripped.failed)
    {
        return NoSoundMatches(o);
    }

    std::fprintf(stderr, "%s: %s\n", xsflib::rip::InputPath(input).filename().string().c_str(),
                 Counted(ripped.files, "miniwsf file").c_str());
    if (ripped.files > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return ripped.failed ? 1 : 0;
}

// Reads AX's microcode and the build of NW4R SND from a game's code (see GameCodeOf). `path` is a main.dol, a main
// program compressed with the CX library (a WiiWare title's content 1), or a folder that holds the game: an extracted
// disc or an unpacked title. Prints an error and returns nothing if the code can't be read or holds no AX microcode.
std::optional<GameCode> ReadGameCode(const std::string& path)
{
    std::vector<uint8_t> dol;
    std::error_code ec;
    if (fs::is_directory(xsflib::rip::InputPath(path), ec))
    {
        ntdlib::disc::GameFiles game;
        if (const auto error = ntdlib::disc::LoadGame(xsflib::rip::InputPath(path), game, rip::DiscOptions()))
        {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), error->c_str());
            return std::nullopt;
        }

        dol = std::move(game.dol);
    }
    else if (!xsflib::psf::ReadWholeFile(path, dol))
    {
        std::fprintf(stderr, "error: can't read %s\n", path.c_str());
        return std::nullopt;
    }
    else if (ntdlib::disc::IsCx(dol))
    {
        try
        {
            dol = ntdlib::disc::DecompressCx(dol, kMaxMainProgramSize);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
            return std::nullopt;
        }
    }

    std::optional<GameCode> code = GameCodeOf(dol);
    if (!code)
    {
        std::fprintf(stderr, "error: %s has no AX microcode\n", path.c_str());
    }

    return code;
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

    // The main.dol comes from --microcode or the inputs, or else from the archive's folder.
    std::string microcode_file = o.microcode_file;
    std::optional<GameCode> code = o.microcode;
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
                     "       the archive's folder. For a WiiWare game, give its unpacked title with --microcode.\n"
                     "       The rip stores a copy of the microcode.\n",
                     file_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "microcode: from %s\n", microcode_file.c_str());
    std::fprintf(stderr, "%s", kArchiveFileNotice);
    NoteNoEffectProfile(*code);

    const std::string game_name = o.game_name.empty() ? xsflib::rip::NameOfInput(input) : o.game_name;
    std::size_t matched = 0;
    const int count = RipArchive(out_dir, names, game_name, file_name, archive_bytes, *code, game_name, o, matched);
    const rip::GameProfile* profile = ProfileOfDol(code->dol_sha1);
    if (count == kMicrocodeRejected && profile)
    {
        std::fprintf(stderr, "note: game mode can rip %s. Rip its disc image or extracted folder instead.\n",
                     profile->name.c_str());
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
// in them. A main.dol among the inputs takes the place of --microcode. Returns 0, or the exit code.
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
        o.microcode_file = dols[0];
        o.microcode = ReadGameCode(dols[0]);
        if (!o.microcode)
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
        if (option == "--microcode")
        {
            o.microcode_file = next();
        }
        else if (option == "--tracks")
        {
            const std::string value = next();
            o.tracks = ParseTracks(value);
            if (!o.tracks)
            {
                std::fprintf(stderr,
                             "error: --tracks takes track numbers from 0 to 15 and ranges such as 8-14, separated by "
                             "commas, not '%s'\n",
                             value.c_str());
                return 2;
            }
        }
        else if (option == "--tempo")
        {
            o.tempo_text = next();
            o.tempo = ParseTempo(o.tempo_text);
            if (!o.tempo)
            {
                std::fprintf(stderr, "error: --tempo takes a number from 0.25 to 4, such as 1.1, not '%s'\n",
                             o.tempo_text.c_str());
                return 2;
            }
        }
        else if (option == "--var")
        {
            const std::string value = next();
            const std::optional<xsflib::Assignment> assignment = xsflib::ParseAssignment(value);
            if (!assignment || assignment->index < 0 || assignment->index > 31 || assignment->value < INT16_MIN ||
                assignment->value > INT16_MAX)
            {
                std::fprintf(stderr,
                             "error: --var takes N=V, with N from 0 to 31 and V from -32768 to 32767, not '%s'\n",
                             value.c_str());
                return 2;
            }

            o.variables[static_cast<int>(assignment->index)] = static_cast<int16_t>(assignment->value);
        }
        else if (option == "--suffix")
        {
            o.suffix = next();
        }
        else
        {
            return -1;
        }

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
