// SPDX-License-Identifier: MIT

// wsfrip: builds WSF sets (.wsflib + .miniwsf files) from Wii games and sound archives.
//
// usage: wsfrip [options] <input> [<input> ...]
//   <input>          a Wii disc image with a decrypted game partition (NKit ISO or GCZ, RVZ, WIA, or an ISO, GCZ,
//                    WBFS, CISO or WDF made from one), an extracted disc, or a sound archive (.brsar). A main.dol among
//                    the inputs isn't ripped. The sound archives among the inputs take AX's microcode from it.
//   -o, --output DIR the folder for the rips (default: a folder next to each input, named after the input with "_wsf"
//                    added)
//   --list           list the game's sound archives and whether it has a driver profile, then stop
//   --mode game|archive
//                    game mode runs the game's code and requires a driver profile for the game (src/rip/rip.cpp). It's
//                    the default when a profile exists. Archive mode plays the sound archives with WSF's model of the
//                    SDK's sound player instead.
//   --microcode FILE a game's main.dol, for AX's microcode. Archive mode on a sound archive reads the microcode from
//                    this file. By default the microcode comes from a main.dol given with the archive, or else from
//                    one in the archive's folder.
//   --archive PATH   rip only this sound archive of a game (its path on the disc)
//   --only REGEX     rip only sounds whose label matches (ECMAScript regex)
//   --bgm            rip only the music (rip::IsMusic): sequences labelled as BGM, or that play on a music player
//   --no-length      skip length analysis (no length or fade tags)
//   --jobs N         the number of length analyses run in parallel (default: the number of CPU threads, at most 8 in
//                    a 32-bit build)
//   --game NAME      game tag (default: the profile's name, or else the game's title, or the archive's name)
//   --artist NAME, --year YEAR, --copyright TEXT, --by NAME (wsfby)
//   --version        print the version
//
// Each of a game's sound archives gets a separate folder in the output folder. The folder is named after the archive.
// Where two of the game's archives have the same name, the names of the folders above them are added. The inputs are
// ripped one after another. An input that fails doesn't stop the rest. Files dropped on the program become inputs.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "ax/microcode.h"
#include "cli/console.h"
#include "common/ascii.h"
#include "disc/game.h"
#include "rip/rip.h"
#include "rsar/formats.h"
#include "wsf/format.h"

using namespace wsf;
namespace fs = std::filesystem;

namespace
{

// The file name for AX's microcode in a rip. The microcode comes from the game's main.dol.
constexpr char kMicrocodeName[] = "ax_microcode.bin";

constexpr char kArchiveModeNotice[] =
    "note: in archive mode, WSF's model of the SDK's sound player plays the sounds instead of the\n"
    "      game's code. The DSP still runs the game's AX microcode. For exact rips of a game with a\n"
    "      driver profile, rip the game itself.\n";

constexpr char kUsage[] =
    "usage: wsfrip [options] <input> [<input> ...]\n"
    "\n"
    "Rips each input into a .wsflib and a .miniwsf per sound. The output folder is next to the\n"
    "input and has the input's name with \"_wsf\" added. An input is a Wii disc image (ISO, NKit,\n"
    "RVZ, WIA, GCZ, WBFS, CISO or WDF), an extracted disc or a sound archive (.brsar). The game\n"
    "partition of a disc image or extracted disc must be stored decrypted. NKit, RVZ and WIA\n"
    "images store it that way. A sound archive requires AX's microcode from a game's main.dol.\n"
    "Give a main.dol with the archive or put one in the archive's folder. You can also drop files\n"
    "on the program.\n"
    "\n"
    "options:\n"
    "  -o, --output DIR     put the rips in DIR\n"
    "  --list               list a game's sound archives and whether it has a driver profile,\n"
    "                       then stop\n"
    "  --mode game|archive  game mode runs the game's code and requires a driver profile for the\n"
    "                       game. It's the default when a profile exists. Archive mode plays the\n"
    "                       sound archives with WSF's model of the SDK's sound player.\n"
    "  --microcode FILE     a game's main.dol, for AX's microcode in archive mode\n"
    "  --archive PATH       rip only this sound archive of a game (its path on the disc)\n"
    "  --only REGEX         rip only the sounds whose label matches\n"
    "  --bgm                rip only the music: sequences labelled as BGM, or that play on a\n"
    "                       player named for music (BGM, MUSIC, SONG or FANFARE)\n"
    "  --no-length          skip length analysis and omit length and fade tags\n"
    "  --jobs N             the number of length analyses that run at once (default: the number\n"
    "                       of CPU threads, and at most 8 in a 32-bit build)\n"
    "  --game NAME, --artist NAME, --year YEAR, --copyright TEXT, --by NAME\n"
    "                       tags (the game defaults to the profile's name, or else the game's title)\n"
    "  --version            show the version\n"
    "  -h, --help           show this help\n";

bool WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    f.close();

    return !f.fail();
}

// What archive mode reads from a game's main.dol: AX's microcode and the game's build of NW4R SND. The model follows
// that build.
struct GameCode
{
    ax::Microcode microcode;
    nw4r::SndBuild build;
};

struct Options
{
    std::regex only{".*"};
    std::string only_text; // the pattern given with --only, or empty
    std::string out_dir, mode, only_archive, game_name, artist, year, copyright, by = "wsfrip";
    bool list = false, lengths = true, bgm = false;
    unsigned jobs = 0;

    // The microcode from --microcode, and the microcode from a main.dol among the inputs. Only loose sound archives use
    // them.
    std::string microcode_file, input_microcode_file;
    std::optional<GameCode> microcode, input_microcode;
};

Tags CommonTags(const Options& o, const std::string& game_name, const char* mode)
{
    Tags common;
    common["game"] = game_name;

    if (!o.artist.empty())
    {
        common["artist"] = o.artist;
    }

    if (!o.year.empty())
    {
        common["year"] = o.year;
    }

    if (!o.copyright.empty())
    {
        common["copyright"] = o.copyright;
    }

    common["wsfby"] = o.by;
    common["wsf_mode"] = mode;
    common["utf8"] = "1"; // tags are UTF-8 (PSF convention)

    return common;
}

struct Job
{
    uint32_t index;
    std::string label;
    std::optional<rip::Timing> timing;
    std::optional<rip::SoundData> data; // game mode: what the driver loads for the sound
};

// Length analysis runs every selected sequence through the model and AX's microcode on the DSP. The sounds are
// independent. The analysis spreads them over several threads.
void AnalyzeLengths(std::vector<Job>& jobs, const std::vector<uint8_t>& archive, const GameCode& code, unsigned threads)
{
    const rip::LengthAnalyzer analyzer(archive, code.microcode, code.build);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> out_of_memory{false};
    const auto worker = [&]
    {
        for (std::size_t i; (i = next++) < jobs.size();)
        {
            try
            {
                jobs[i].timing = analyzer.Analyze(jobs[i].index);
            }
            catch (const std::bad_alloc&)
            {
                out_of_memory = true;
                next = jobs.size(); // the other threads take no more jobs
                return;
            }

            if (!jobs[i].timing)
            {
                std::fprintf(stderr, "warning: length analysis failed for %s; no length tag written\n",
                             jobs[i].label.c_str());
            }

            const std::size_t n = ++done;
            if (n % 100 == 0 || n == jobs.size())
            {
                std::fprintf(stderr, "  analyzed %zu of %zu\n", n, jobs.size());
            }
        }
    };

    threads = std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(jobs.size())));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; t++)
    {
        // If another thread can't start (the system is out of threads or memory), the threads already running do the
        // work.
        try
        {
            pool.emplace_back(worker);
        }
        catch (const std::exception&)
        {
            break;
        }
    }

    worker();

    for (auto& t : pool)
    {
        t.join();
    }

    if (out_of_memory)
    {
        throw std::runtime_error("out of memory during length analysis; try fewer --jobs");
    }
}

// Returns the input's absolute path, without the trailing separator of a folder given as "game/" or ".".
fs::path InputPath(const std::string& input)
{
    const fs::path path = fs::absolute(input).lexically_normal();
    return path.has_filename() ? path : path.parent_path();
}

// The name of `input`: the image's or archive's file name without its extension, or the folder's name.
std::string NameOfInput(const std::string& input)
{
    const fs::path path = InputPath(input);
    return fs::is_directory(path) ? path.filename().string() : path.stem().string();
}

// Returns the output folder for `input` when -o isn't given. The folder is next to the input and named after it with
// "_wsf" added.
fs::path DefaultOutputDir(const std::string& input)
{
    return InputPath(input).parent_path() / (NameOfInput(input) + "_wsf");
}

// Returns the output folders for the sets of a game's sound archives, from the archives' paths on the disc. Each folder
// is the archive's name without its extension. Archives whose names are the same, ignoring case, also get the names of
// as many parent folders as it takes to tell them apart. Sound/BGM/sound.brsar and Sound/SE/sound.brsar go in BGM_sound
// and SE_sound.
std::vector<std::string> ArchiveFolders(const std::vector<std::string>& paths)
{
    // The parts of each path from the archive up: its name without the extension, then its parent folders.
    std::vector<std::vector<std::string>> parts;
    for (const auto& path : paths)
    {
        std::vector<std::string> p{fs::path(path).stem().string()};
        for (fs::path folder = fs::path(path).parent_path(); folder.has_filename(); folder = folder.parent_path())
        {
            p.push_back(folder.filename().string());
        }

        parts.push_back(std::move(p));
    }

    // In each round, archives whose folders clash take one more part.
    std::vector<std::size_t> used(paths.size(), 1);
    std::vector<std::string> folders(paths.size());
    for (bool clash = true; clash;)
    {
        std::map<std::string, std::vector<std::size_t>> by_name;
        for (std::size_t i = 0; i < paths.size(); i++)
        {
            folders[i] = parts[i][0];
            for (std::size_t j = 1; j < used[i]; j++)
            {
                folders[i] = parts[i][j] + "_" + folders[i];
            }

            by_name[AsciiLower(folders[i])].push_back(i);
        }

        clash = false;
        for (const auto& [name, archives] : by_name)
        {
            for (const std::size_t i : archives)
            {
                if (archives.size() > 1 && used[i] < parts[i].size())
                {
                    used[i]++;
                    clash = true;
                }
            }
        }
    }

    return folders;
}

// The sequences that --only and --bgm pick.
std::vector<Job> SelectSounds(const rsar::SoundArchive& archive, const Options& o)
{
    std::vector<Job> jobs;
    const auto& sounds = archive.Sounds();
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        // Streams are out of scope, and wave sounds aren't ripped yet.
        const auto index = static_cast<uint32_t>(i);
        if (sounds[i].type == rsar::SoundType::kSequence && std::regex_search(sounds[i].name, o.only) &&
            (!o.bgm || rip::IsMusic(archive, index)))
        {
            jobs.push_back({index, sounds[i].name, std::nullopt, std::nullopt});
        }
    }

    return jobs;
}

// Writes one .miniwsf per job. `build` makes the file and the tags for a job.
int WriteMinis(const fs::path& out_dir, rip::FileNames& names, const std::vector<Job>& jobs, const Tags& common,
               const std::function<std::vector<uint8_t>(const Job&, const Tags&)>& build)
{
    int count = 0;
    for (const Job& job : jobs)
    {
        Tags tags = common;
        tags["title"] = job.label;
        tags["wsf_sound"] = job.label;
        if (job.timing)
        {
            tags["length"] = FormatTime(job.timing->length_ms);
            tags["fade"] = FormatTime(job.timing->fade_ms);
        }

        const std::string name = names.Mini(job.index, job.label);
        if (!WriteFile(out_dir / name, build(job, tags)))
        {
            std::fprintf(stderr, "error: can't write %s\n", name.c_str());
            return -1;
        }

        count++;
        std::fprintf(stderr, "%s%s%s\n", name.c_str(), tags.count("length") ? "  length " : "",
                     tags.count("length") ? tags["length"].c_str() : "");
    }

    return count;
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

// Prints the rip's output folder as the last line about the input.
void ReportOutput(const fs::path& out_dir)
{
    std::fprintf(stderr, "output: %s\n", fs::absolute(out_dir).lexically_normal().string().c_str());
}

// Rips one sound archive in archive mode. Adds the number of sounds that --only selected to `matched`. Returns the
// number of .miniwsf files written, or -1 on an error.
int RipArchive(const fs::path& out_dir, const std::string& lib_base, const std::string& archive_path,
               const std::vector<uint8_t>& archive_bytes, const GameCode& code, const std::string& game_name,
               const Options& o, std::size_t& matched)
{
    std::optional<rsar::SoundArchive> archive;
    try
    {
        archive = rsar::SoundArchive::Load(archive_bytes);
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
                     "       that WSF's model makes. The game's AX is older or newer than the model's AX.\n",
                     archive_path.c_str());
        return -1;
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

    if (complete.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip (%zu sequences skipped)\n", archive_path.c_str(), skipped);
        return 0;
    }

    rip::FileNames names;
    const Tags common = CommonTags(o, game_name, "archive");
    const std::string lib_name = names.Library(lib_base);
    const auto lib =
        rip::BuildArchiveLibrary(archive_path, archive_bytes, kMicrocodeName, code.microcode, code.build, common);
    fs::create_directories(out_dir);
    if (!WriteFile(out_dir / lib_name, lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return -1;
    }

    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    if (o.lengths)
    {
        AnalyzeLengths(complete, archive_bytes, code, o.jobs);
    }

    const auto build_mini = [&lib_name](const Job& job, const Tags& tags)
    {
        return rip::BuildArchiveMini(job.index, lib_name, tags);
    };
    const int count = WriteMinis(out_dir, names, complete, common, build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %d miniwsf files; %zu sequences skipped (data outside the archive)\n",
                     archive_path.c_str(), count, skipped);
    }

    return count;
}

// Rips one of a game's sound archives in game mode. Returns the number of .miniwsf files written, or -1 on an error.
int RipArchiveInGameMode(const fs::path& out_dir, const disc::GameFiles& game, const rip::GameProfile& profile,
                         const std::string& archive_path, const std::vector<uint8_t>& archive_bytes,
                         const GameCode& code, const std::string& lib_base, const std::string& game_name,
                         const Options& o, std::size_t& matched)
{
    const auto archive = rsar::SoundArchive::Load(archive_bytes);
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

    if (playable.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip (%zu sequences skipped)\n", archive_path.c_str(), skipped);
        return 0;
    }

    rip::FileNames names;
    const Tags common = CommonTags(o, game_name, "game");
    const std::string lib_name = names.Library(lib_base);
    std::string error;
    const auto lib = rip::BuildLibrary(game, profile, archive_path, archive, common, &error);
    if (!lib)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return -1;
    }

    fs::create_directories(out_dir);
    if (!WriteFile(out_dir / lib_name, *lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return -1;
    }

    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib->size());

    if (o.lengths)
    {
        AnalyzeLengths(playable, archive_bytes, code, o.jobs);
    }

    const auto build_mini = [&lib_name](const Job& job, const Tags& tags)
    {
        return rip::BuildMini(job.index, *job.data, lib_name, tags);
    };
    const int count = WriteMinis(out_dir, names, playable, common, build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %d miniwsf files; %zu sequences skipped\n", archive_path.c_str(), count, skipped);
    }

    return count;
}

// Lists a game's sound archives and their contents.
void ListArchives(const disc::GameFiles& game)
{
    const auto archives = game.SoundArchives();
    std::fprintf(stderr, "%zu sound archive(s):\n", archives.size());
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
            const auto archive = rsar::SoundArchive::Load(bytes);
            std::size_t sequences = 0, wave_sounds = 0, streams = 0;
            for (const auto& s : archive.Sounds())
            {
                sequences += s.type == rsar::SoundType::kSequence;
                wave_sounds += s.type == rsar::SoundType::kWaveSound;
                streams += s.type == rsar::SoundType::kStream;
            }
            std::fprintf(stderr, "  %s: %zu bytes, %zu sequences, %zu wave sounds, %zu streams\n", path.c_str(),
                         bytes.size(), sequences, wave_sounds, streams);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "  %s: %s\n", path.c_str(), e.what());
        }
    }
}

// Rips a game image or folder: each of its sound archives, or the one that --archive names. Uses game mode if a driver
// profile exists. Otherwise uses archive mode. --mode overrides the choice. Returns the exit code.
int RipGame(const std::string& input, const fs::path& out_dir, const Options& o)
{
    disc::GameFiles game;
    if (const auto error = disc::LoadGame(fs::path(input), game))
    {
        std::fprintf(stderr, "error: %s\n", error->c_str());
        return 1;
    }

    std::fprintf(stderr, "%s, \"%s\" (%s)\n", game.GameId().c_str(), game.Title().c_str(), game.source.c_str());
    const rip::GameProfile* profile = rip::FindProfile(game);
    const std::optional<ax::Microcode> microcode = ax::FindMicrocode(game.dol);
    if (o.list)
    {
        ListArchives(game);
        std::fprintf(stderr, "%s\n",
                     profile ? (std::string("profile: ") + profile->name + " (game mode)").c_str()
                             : "no driver profile for this game: archive mode only");
        return 0;
    }

    if (!microcode)
    {
        std::fprintf(stderr, "error: the game's main.dol has no AX microcode\n");
        return 1;
    }

    const GameCode code{*microcode, nw4r::FindSndBuild(game.dol)};

    const std::string mode = o.mode.empty() ? (profile ? "game" : "archive") : o.mode;
    if (mode == "game" && !profile)
    {
        std::fprintf(stderr, "error: no driver profile for this game (known: ");
        for (std::size_t i = 0; i < rip::KnownProfiles().size(); i++)
        {
            std::fprintf(stderr, "%s%s", i ? ", " : "", rip::KnownProfiles()[i].name);
        }
        std::fprintf(stderr, "); --mode archive rips its sound archives instead\n");
        return 1;
    }

    if (mode == "archive")
    {
        if (!profile)
        {
            std::fprintf(stderr, "no driver profile for this game: ripping in archive mode\n");
        }

        std::fprintf(stderr, "%s", kArchiveModeNotice);
    }

    const auto archives = game.SoundArchives();
    if (!o.only_archive.empty() && std::find(archives.begin(), archives.end(), o.only_archive) == archives.end())
    {
        std::fprintf(stderr, "error: the game has no sound archive %s (--list shows them)\n", o.only_archive.c_str());
        return 1;
    }

    const std::string game_name = o.game_name.empty() ? (profile ? profile->name : game.Title()) : o.game_name;
    const std::vector<std::string> folders = ArchiveFolders(archives);
    int total = 0;
    std::size_t matched = 0;
    bool failed = false; // an archive that can't be ripped doesn't stop the others
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
        const std::string lib_base = game_name + " " + folders[a];
        int n = 0;
        try
        {
            n = mode == "game" ? RipArchiveInGameMode(archive_dir, game, *profile, path, archive_bytes, code, lib_base,
                                                      game_name, o, matched)
                               : RipArchive(archive_dir, lib_base, path, archive_bytes, code, game_name, o, matched);
        }
        catch (const rsar::FormatError& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
            n = -1;
        }

        if (n < 0)
        {
            failed = true;
            continue;
        }

        total += n;
    }

    if (matched == 0 && (!o.only_text.empty() || o.bgm) && !failed)
    {
        return NoSoundMatches(o);
    }

    std::fprintf(stderr, "%s: %d miniwsf files\n", InputPath(input).filename().string().c_str(), total);
    if (total > 0)
    {
        ReportOutput(out_dir);
    }

    return failed ? 1 : 0;
}

// Reads AX's microcode and the build of NW4R SND from the main.dol at `path`.
std::optional<GameCode> ReadGameCode(const std::string& path)
{
    std::vector<uint8_t> dol;
    if (!ReadWholeFile(path, dol))
    {
        return std::nullopt;
    }

    const std::optional<ax::Microcode> microcode = ax::FindMicrocode(dol);
    if (!microcode)
    {
        return std::nullopt;
    }

    return GameCode{*microcode, nw4r::FindSndBuild(dol)};
}

// Rips a loose sound archive in archive mode. Returns the exit code.
int RipLooseArchive(const std::string& input, const fs::path& out_dir, const Options& o)
{
    const std::string file_name = InputPath(input).filename().string();
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
    if (!ReadWholeFile(input, archive_bytes))
    {
        std::fprintf(stderr, "error: can't read %s\n", input.c_str());
        return 1;
    }

    // The microcode comes from --microcode, then from a main.dol among the inputs, then from the archive's folder.
    std::string microcode_file = o.microcode_file.empty() ? o.input_microcode_file : o.microcode_file;
    std::optional<GameCode> code = o.microcode_file.empty() ? o.input_microcode : o.microcode;
    if (!code)
    {
        const fs::path beside = InputPath(input).parent_path() / "main.dol";
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
                     code ? microcode_file.c_str() : "none found; ripping requires --microcode");
        std::fprintf(stderr, "%s: a sound archive (archive mode)\n", input.c_str());
        return 0;
    }

    if (!code)
    {
        std::fprintf(stderr,
                     "error: %s: a sound archive doesn't contain AX's microcode. Give a game's main.dol with\n"
                     "       the archive (or with --microcode) or put one in the archive's folder. The rip\n"
                     "       stores a copy of the microcode.\n",
                     file_name.c_str());
        return 1;
    }

    std::fprintf(stderr, "microcode: from %s\n", microcode_file.c_str());
    std::fprintf(stderr, "%s", kArchiveModeNotice);

    const std::string game_name = o.game_name.empty() ? NameOfInput(input) : o.game_name;
    std::size_t matched = 0;
    const int count = RipArchive(out_dir, game_name, file_name, archive_bytes, *code, game_name, o, matched);
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
        ReportOutput(out_dir);
    }

    return 0;
}

// Returns the first 0x20 bytes of a file. These bytes distinguish a sound archive or a DOL from a disc image. Returns
// an empty string for a folder or for a file that can't be read.
std::string ReadHead(const std::string& path)
{
    std::string head;
    std::error_code ec;
    if (fs::is_regular_file(path, ec))
    {
        std::ifstream f(path, std::ios::binary);
        head.resize(0x20);
        f.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(f.gcount()));
    }

    return head;
}

// True for a main.dol. The .dol extension identifies it. No disc image or sound archive uses that extension.
bool IsDol(const std::string& path)
{
    return AsciiLower(fs::path(path).extension().string()) == ".dol";
}

// Reads the options and inputs. Returns 0 to go on, -1 when there's nothing more to do (after --help or --version), or
// the exit code.
int ParseArgs(int argc, char** argv, Options& o, std::vector<std::string>& inputs)
{
    std::string only;
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];

        bool missing_value = false;
        const auto next = [&]() -> std::string
        {
            if (i + 1 >= argc)
            {
                missing_value = true;
                return "";
            }

            return argv[++i];
        };

        if (a == "-h" || a == "--help")
        {
            std::fputs(kUsage, stdout);
            return -1;
        }
        else if (a == "--version")
        {
            std::puts("wsfrip " WSF_VERSION);
            return -1;
        }
        else if (a == "-o" || a == "--output")
        {
            o.out_dir = next();
        }
        else if (a == "--list")
        {
            o.list = true;
        }
        else if (a == "--mode")
        {
            o.mode = next();
        }
        else if (a == "--microcode")
        {
            o.microcode_file = next();
        }
        else if (a == "--archive")
        {
            o.only_archive = next();
        }
        else if (a == "--only")
        {
            only = next();
        }
        else if (a == "--bgm")
        {
            o.bgm = true;
        }
        else if (a == "--no-length")
        {
            o.lengths = false;
        }
        else if (a == "--jobs")
        {
            const std::string v = next();
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end || n < 1 || n > 1024)
            {
                std::fprintf(stderr, "error: --jobs takes a number of threads from 1 to 1024, not '%s'\n", v.c_str());
                return 2;
            }

            o.jobs = static_cast<unsigned>(n);
        }
        else if (a == "--game")
        {
            o.game_name = next();
        }
        else if (a == "--artist")
        {
            o.artist = next();
        }
        else if (a == "--year")
        {
            o.year = next();
        }
        else if (a == "--copyright")
        {
            o.copyright = next();
        }
        else if (a == "--by")
        {
            o.by = next();
        }
        else if (a.size() > 1 && a[0] == '-')
        {
            std::fprintf(stderr, "error: unknown option %s (see --help)\n", a.c_str());
            return 2;
        }
        else
        {
            inputs.push_back(a);
        }

        if (missing_value)
        {
            std::fprintf(stderr, "error: %s needs a value\n", a.c_str());
            return 2;
        }
    }

    if (inputs.empty())
    {
        std::fprintf(stderr, "%s", kUsage);
        return 2;
    }

    if (!o.mode.empty() && o.mode != "game" && o.mode != "archive")
    {
        std::fprintf(stderr, "error: --mode must be game or archive\n");
        return 2;
    }

    // Report a pattern that doesn't compile before anything is written.
    if (!only.empty())
    {
        try
        {
            o.only = std::regex(only);
            o.only_text = only;
        }
        catch (const std::regex_error& e)
        {
            std::fprintf(stderr, "error: --only: %s\n", e.what());
            return 2;
        }
    }

    // Each analysis emulates 88 MiB of main memory. A 32-bit build has only 2 GiB of address space. It runs at most 8
    // analyses.
    if (o.jobs == 0)
    {
        constexpr unsigned kMaxJobs32 = 8;
        o.jobs = std::max(1u, std::thread::hardware_concurrency());
        if constexpr (sizeof(void*) < 8)
        {
            o.jobs = std::min(o.jobs, kMaxJobs32);
        }
    }

    return 0;
}

// Removes the main.dol files from the inputs. The sound archives take AX's microcode from them. Returns 0, or the exit
// code.
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
            std::fprintf(stderr, "error: %s has no AX microcode\n", dols[0].c_str());
            return 1;
        }
    }

    if (inputs.empty())
    {
        std::fprintf(stderr,
                     "error: %s is a main.dol. It only provides microcode for sound archives. Give it together "
                     "with the archives\n",
                     dols[0].c_str());
        return 2;
    }

    return 0;
}

// Rips one input. Returns the exit code.
int RipInput(const std::string& input, const Options& o)
{
    const std::string head = ReadHead(input);
    if (head.starts_with("CSAR") || head.starts_with("FSAR"))
    {
        std::fprintf(stderr, "error: %s is a %s sound archive; WSF rips Wii sound archives (RSAR, .brsar)\n",
                     input.c_str(), head.starts_with("CSAR") ? "3DS" : "Wii U or Switch");
        return 1;
    }

    const fs::path out_dir = o.out_dir.empty() ? DefaultOutputDir(input) : fs::path(o.out_dir);
    if (head.starts_with("RSAR"))
    {
        return RipLooseArchive(input, out_dir, o);
    }

    return RipGame(input, out_dir, o);
}

int Run(int argc, char** argv)
{
    Options o;
    std::vector<std::string> inputs;
    if (const int parsed = ParseArgs(argc, argv, o, inputs))
    {
        return parsed < 0 ? 0 : parsed;
    }

    // Check all paths before writing anything. Dropped files always exist. This check catches typos, and output paths
    // passed as positional arguments instead of with -o.
    for (std::size_t i = 0; i < inputs.size(); i++)
    {
        std::error_code ec;
        if (!fs::exists(inputs[i], ec))
        {
            std::fprintf(stderr, "error: %s: no such file or folder%s\n", inputs[i].c_str(),
                         i ? " (-o names the output folder)" : "");
            return 1;
        }
    }

    if (!o.microcode_file.empty())
    {
        o.microcode = ReadGameCode(o.microcode_file);
        if (!o.microcode)
        {
            std::fprintf(stderr, "error: %s has no AX microcode\n", o.microcode_file.c_str());
            return 1;
        }
    }

    if (const int taken = TakeDolInputs(inputs, o))
    {
        return taken;
    }

    // Rip each input once and continue after errors. Return the highest exit code.
    std::set<fs::path> done;
    int result = 0;
    for (std::size_t i = 0; i < inputs.size(); i++)
    {
        if (inputs.size() > 1)
        {
            std::fprintf(stderr, "%s%s:\n", i ? "\n" : "", InputPath(inputs[i]).filename().string().c_str());
        }

        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(inputs[i], ec);
        if (!done.insert(ec ? InputPath(inputs[i]) : canonical).second)
        {
            std::fprintf(stderr, "skipped duplicate input\n");
            continue;
        }

        try
        {
            result = std::max(result, RipInput(inputs[i], o));
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", inputs[i].c_str(), e.what());
            result = std::max(result, 1);
        }
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    SetUpConsole();

    // Report unhandled errors, including allocation failures.
    int result = 1;
    try
    {
        result = Run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
    }

    KeepConsoleOpen();

    return result;
}
