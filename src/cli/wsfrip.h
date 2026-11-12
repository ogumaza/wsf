// SPDX-License-Identifier: MIT

// The parts of wsfrip that the rips of each kind of input share: the game code that WSF's model reads, the options,
// what a rip does when --only or --bgm selects no sound, the drop of the sounds that make none, the writing of a set,
// and the lookups and notes for driver profiles.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/rvl/snd/arrangement.h"
#include "ntdlib/rvl/snd/util.h"
#include "ntdlib/sound_build.h"
#include "xsflib/cli/rip.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/file_names.h"

#include "rip/profiles.h"
#include "wsf/format.h"
#include "wsf/models.h"

namespace wsf::cli
{

// What WSF's model reads from a game's main.dol: AX's microcode and the game's build of NW4R SND. The model follows
// that build. Archive mode plays the model, and the length analyses of both modes run it. The game's effect profile
// gives the effects that both modes play.
struct GameCode
{
    ntdlib::Microcode microcode;
    nw4r::SndBuild build;
    std::string dol_sha1; // the main.dol's SHA-1, for the driver profile and the notes
    std::optional<rip::EffectProfile> effect_profile;
};

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

    // Whether --tracks, --tempo or --var was given.
    bool Arranged() const
    {
        return tracks || tempo || !variables.empty();
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

// A count and its noun: "1 sound", "2 sounds".
inline std::string Counted(std::size_t count, const std::string& noun)
{
    return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

// Reports that --only or --bgm selected no sound. This is an error. Nothing is written.
inline int NoSoundMatches(const Options& o)
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

// Writes a rip's set into `out_dir`: the library `lib` (see xsflib::cli::WriteLibrary), then a mini for each of `jobs`
// with the tags `tags`. `build(lib_name, job, tags)` makes a job's mini. Then prints how many minis the rip of `source`
// wrote, and `skipped_text` (see xsflib::cli::SkippedText). Returns that number, or -1 after printing an error.
template <typename J, typename Build>
int WriteSet(const std::filesystem::path& out_dir, xsflib::rip::FileNames& names, const std::string& lib_base,
             const std::vector<uint8_t>& lib, const std::vector<J>& jobs, const Tags& tags, const Build& build,
             const std::string& source, const std::string& skipped_text)
{
    const std::optional<std::string> lib_name = xsflib::cli::WriteLibrary(out_dir, names, lib_base, lib);
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
inline void NoteOtherRelease(const std::string& profile_name)
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

} // namespace wsf::cli
