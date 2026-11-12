// SPDX-License-Identifier: MIT

// wsfrip's rips of the games whose sound library is JAudio (see jaudio_games.h).

#include "cli/jaudio_games.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/flipper/wave_list.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/wave_system.h"
#include "xsflib/cli/rip.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"

#include "cli/wsfrip.h"
#include "rip/jaudio.h"
#include "rip/jaudio_analysis.h"
#include "rip/rip.h"
#include "wsf/format.h"

namespace wsf::cli
{
namespace
{

namespace fs = std::filesystem;

// A JAudio game's sound to rip, and the wave groups that it plays, each (system << 16) | group.
struct JAudioJob
{
    uint32_t index;    // the sound's place in the game's list of sounds
    uint32_t sound_id; // the ID that the game passes to its sound library
    std::string label;
    std::string sound_label; // the label of the sound that the job plays, for the wsf_sound tag
    std::optional<rip::Timing> timing;
    std::vector<uint32_t> waves;
    std::string start_error; // the player's message when the length pass can't start the sound

    // The state that the driver gives a piece (rip::JAudioSound::piece_state).
    ntdlib::flipper::z2::PieceState piece_state;

    bool luigi = false;    // the sound's Luigi variant (rip::JAudioProfile::luigi_word)
    bool as_mario = false; // a Luigi variant that looks up the same waves as its sound

    // The seconds that the search for the sound's waves took, or infinity if no search played the sound
    // (DiscoverWaves). The length analysis starts the sounds whose search took longest first.
    double search_seconds = std::numeric_limits<double>::infinity();
};

// Takes out the Luigi variants that look up the same waves as their sounds (JAudioJob::as_mario). Their rips would
// play as the sounds' rips do. Returns the number taken out.
std::size_t SkipSameAsMario(std::vector<JAudioJob>& jobs)
{
    return std::erase_if(jobs, [](const JAudioJob& job) { return job.as_mario; });
}

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

// A JAudio game's sounds that --only and --bgm pick. Streams are out of scope. A sound's number is its place in the
// list without the sounds that share the number before them (rip::JAudioSound::shares_number). A sound of the profile's
// group for Luigi (rip::JAudioProfile::luigi_word) is followed by its Luigi variant, labelled with "_Luigi", with the
// same number. --only picks each of the two by its label.
std::vector<JAudioJob> SelectJAudioSounds(const std::vector<rip::JAudioSound>& sounds,
                                          const rip::JAudioProfile* profile, const Options& o)
{
    std::vector<JAudioJob> jobs;
    uint32_t index = 0;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const rip::JAudioSound& sound = sounds[i];
        if (i > 0 && !sound.shares_number)
        {
            index++;
        }

        if (sound.stream || (o.bgm && !sound.music))
        {
            continue;
        }

        const std::string& sound_label = sound.sound_label.empty() ? sound.label : sound.sound_label;
        if (std::regex_search(sound.label, o.only))
        {
            jobs.push_back(
                {index, sound.id, sound.label, sound_label, std::nullopt, sound.waves, {}, sound.piece_state});
        }

        const std::string luigi = sound.label + "_Luigi";
        if (profile && profile->luigi_word >= 0 && (sound.id >> 16) == profile->luigi_group &&
            std::regex_search(luigi, o.only))
        {
            jobs.push_back(
                {index, sound.id, luigi, sound_label, std::nullopt, sound.waves, {}, sound.piece_state, true});
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
        if (!rip::FindJAudioWaves(*base, profile, job.sound_id, found, &job.piece_state, job.luigi))
        {
            std::fprintf(stderr, "warning: the waves of %s can't be found; it plays without them\n", job.label.c_str());
            return false;
        }

        // A Luigi variant that looks up the same waves as its sound plays as the sound does. It's left out
        // (SkipSameAsMario).
        if (job.luigi)
        {
            std::set<ntdlib::jaudio::WaveUse> mario;
            if (rip::FindJAudioWaves(*base, profile, job.sound_id, mario, &job.piece_state) && mario == found)
            {
                job.as_mario = true;
                return false;
            }
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
    const auto timed = [&discover](JAudioJob& job)
    {
        const bool searches = job.waves.empty();
        const auto start = std::chrono::steady_clock::now();
        const bool result = discover(job);
        if (searches)
        {
            job.search_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }

        return result;
    };
    std::fprintf(stderr, "finding the wave groups of %s\n", Counted(undiscovered, "sound").c_str());
    xsflib::cli::AnalyzeLengths(
        jobs, threads, timed,
        {.out_of_memory = "out of memory while finding the waves; try fewer --jobs", .progress = "searched"});

    return true;
}

// Length analysis plays every sound in `jobs` on the rip's library `lib`, with its wave groups loaded. The sounds whose
// wave search took longest start first. A sound that can't start isn't analyzed: SkipUnstartable takes it out. A sound
// that looks up waves outside its groups gets a warning.
void AnalyzeLengths(std::vector<JAudioJob>& jobs, const std::vector<uint8_t>& lib, const rip::JAudioProfile& profile,
                    const std::vector<ntdlib::jaudio::WaveSystem>& systems, unsigned threads)
{
    const auto analyze = [&](JAudioJob& job)
    {
        std::set<ntdlib::jaudio::WaveUse> played;
        job.timing = rip::AnalyzeJAudioSound(lib, profile, job.sound_id, job.waves, &played, &job.start_error,
                                             &job.piece_state, job.luigi);
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
    xsflib::cli::AnalysisSettings settings = {.out_of_memory =
                                                  "out of memory during length analysis; try fewer --jobs"};
    for (const JAudioJob& job : jobs)
    {
        settings.costs.push_back(job.search_seconds);
    }

    xsflib::cli::AnalyzeLengths(jobs, threads, analyze, settings);
}

// The sounds of a JAudio game. `baa` gets the start-up archive, or a Jac game's bank file. Prints an error and returns
// nothing if the file can't be read or its sounds can't be listed.
std::optional<std::vector<rip::JAudioSound>> ReadGameSounds(const ntdlib::disc::GameFiles& game,
                                                             const rip::JAudioFiles& files,
                                                             const rip::JAudioProfile* profile,
                                                             std::vector<uint8_t>& baa)
{
    if (!rip::ReadSoundFile(game, files.baa, baa))
    {
        std::fprintf(stderr, "error: can't read %s\n", files.baa.c_str());
        return std::nullopt;
    }

    std::string error;
    std::optional<std::vector<rip::JAudioSound>> sounds = rip::ReadJAudioSounds(game, files, profile, baa, &error);
    if (!sounds)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
    }

    return sounds;
}

// The files of the library for `jobs`, in `paths`. Every library holds the sound files (see rip::LibraryFiles) and the
// groups that the audio set-up loads, then the groups that the jobs play. A Luigi variant that plays Mario's waves is
// left out. Returns false after an error.
bool FindLibraryFiles(std::vector<JAudioJob>& jobs, const ntdlib::disc::GameFiles& game, const rip::JAudioFiles& files,
                      const rip::JAudioProfile& profile, const std::vector<ntdlib::jaudio::WaveSystem>& systems,
                      unsigned threads, std::vector<std::string>& paths)
{
    paths = rip::LibraryFiles(files, profile);
    if (!AddAwFiles(game, files, systems, profile.static_waves, paths))
    {
        return false;
    }

    if (!DiscoverWaves(jobs, game, profile, paths, systems, threads))
    {
        return false;
    }

    if (const std::size_t same = SkipSameAsMario(jobs))
    {
        std::fprintf(stderr, "left out %s: they play Mario's waves\n", Counted(same, "Luigi variant").c_str());
    }

    for (const JAudioJob& job : jobs)
    {
        if (!AddAwFiles(game, files, systems, job.waves, paths))
        {
            return false;
        }
    }

    return true;
}

// With length analysis, measures the lengths of `jobs` on the library `lib` and leaves out the sounds that can't start
// or never sound. Returns the text that names the sounds left out.
std::string MeasureLengths(std::vector<JAudioJob>& jobs, const std::vector<uint8_t>& lib,
                           const rip::JAudioProfile& profile, const std::vector<ntdlib::jaudio::WaveSystem>& systems,
                           const Options& o)
{
    std::size_t unstartable = 0;
    std::size_t silent = 0;
    if (o.lengths)
    {
        AnalyzeLengths(jobs, lib, profile, systems, o.jobs);
        unstartable = SkipUnstartable(jobs);
        silent = SkipSilent(jobs);
    }

    return xsflib::cli::SkippedText(unstartable, "can't start", silent, "sound");
}

} // namespace

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

    std::vector<uint8_t> baa;
    const std::optional<std::vector<rip::JAudioSound>> sounds = ReadGameSounds(game, files, profile, baa);
    if (!sounds)
    {
        return 1;
    }

    if (o.list)
    {
        ListJAudioGame(game.GameId(), files, *sounds, profile);
        return 0;
    }

    std::vector<JAudioJob> jobs = SelectJAudioSounds(*sounds, profile, o);
    if (jobs.empty())
    {
        if (!o.only_text.empty() || o.bgm)
        {
            return NoSoundMatches(o);
        }

        std::fprintf(stderr, "%s: nothing to rip\n", files.baa.c_str());
        return 0;
    }

    const std::vector<ntdlib::jaudio::WaveSystem> systems = rip::ReadWaveSystems(profile->library, baa);
    std::vector<std::string> paths;
    if (!FindLibraryFiles(jobs, game, files, *profile, systems, o.jobs, paths))
    {
        return 1;
    }

    const std::string game_name = o.game_name.empty() ? profile->name : o.game_name;
    const Tags common = xsflib::cli::RipTags(o, game_name, "wsf", "game");
    std::string error;
    const auto lib = rip::BuildJAudioLibrary(game, *profile, paths, common, &error);
    if (!lib)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    const std::string skipped_text = MeasureLengths(jobs, *lib, *profile, systems, o);
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", files.baa.c_str(), skipped_text.c_str());
        return 0;
    }

    xsflib::rip::FileNames& names = xsflib::cli::NamesIn(registries, out_dir, ".wsflib", ".miniwsf");
    const auto build_mini = [profile](const std::string& lib_name, const JAudioJob& job, Tags tags)
    {
        tags["wsf_sound"] = job.sound_label;
        return rip::BuildJAudioMini(job.sound_id, job.waves, lib_name, tags, &job.piece_state,
                                    job.luigi ? profile->luigi_word : -1);
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

} // namespace wsf::cli
