// SPDX-License-Identifier: MIT

// wsfrip's rips of the sound archives of NW4R SND (see archive_rip.h).

#include "cli/archive_rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/rsar/cut.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/music.h"
#include "ntdlib/rvl/snd/arrangement.h"
#include "ntdlib/rvl/snd/effect_setup.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "xsflib/cli/rip.h"
#include "xsflib/rip/file_names.h"

#include "cli/wsfrip.h"
#include "rip/profiles.h"
#include "rip/rip.h"
#include "wsf/format.h"
#include "wsf/models.h"

namespace wsf::cli
{
namespace
{

namespace fs = std::filesystem;

// The file name for AX's microcode in a rip. The microcode comes from the game's main.dol.
constexpr char kMicrocodeName[] = "ax_microcode.bin";

struct Job
{
    uint32_t index;
    std::string label;
    std::optional<rip::Timing> timing;
    std::optional<rip::SoundData> data; // game mode: what the driver loads for the sound

    // Archive mode: the sounds that the rip plays, in the order in which they start. A piece whose channels the game
    // plays as separate sequences has all of them (see rip::FindPieces), the first at `index`. Empty in game mode.
    std::vector<uint32_t> sounds;

    // The effect variant that the rip plays in (see nw4r::EffectVariant), by its place in the profile's variants, or -1
    // for the plain rip.
    int variant = -1;

    // The sounds that the rip plays: `index` alone in game mode.
    std::vector<uint32_t> Sounds() const
    {
        return sounds.empty() ? std::vector<uint32_t>{index} : sounds;
    }
};

// The rips of the jobs in `jobs` that play one sound of a variant of `effects`, for each variant in turn. Each gets the
// variant's name after its label.
std::vector<Job> VariantJobs(const std::vector<Job>& jobs, const nw4r::ArchiveEffects& effects)
{
    std::vector<Job> found;
    for (std::size_t v = 0; v < effects.variants.size(); v++)
    {
        const nw4r::EffectVariant& variant = effects.variants[v];
        for (const Job& job : jobs)
        {
            const std::vector<uint32_t> sounds = job.Sounds();
            if (sounds.size() == 1 &&
                std::find(variant.sounds.begin(), variant.sounds.end(), sounds.front()) != variant.sounds.end())
            {
                Job rip = job;
                rip.label += variant.name;
                rip.timing.reset();
                rip.variant = static_cast<int>(v);
                found.push_back(rip);
            }
        }
    }

    return found;
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
            (!o.bgm || ntdlib::rsar::IsMusic(archive, index)))
        {
            jobs.push_back({index, sounds[i].name, std::nullopt, std::nullopt, {}});
        }
    }

    return jobs;
}

// Archive mode: the sequences that --only and --bgm pick. A piece that the game plays as separate sequences makes one
// job (see rip::FindPieces). --only picks it by its name or by any of its sequences' labels, and --bgm by any of its
// sequences. A sequence of a piece makes no job of its own.
std::vector<Job> SelectArchiveSounds(const ntdlib::rsar::SoundArchive& archive, const Options& o)
{
    const auto& sounds = archive.Sounds();
    const std::vector<rip::Piece> pieces = rip::FindPieces(archive);
    std::vector<bool> in_piece(sounds.size(), false);
    for (const rip::Piece& piece : pieces)
    {
        for (const uint32_t sound : piece.sounds)
        {
            in_piece[sound] = true;
        }
    }

    std::vector<Job> jobs;
    const auto select =
        [&archive, &sounds, &o, &jobs](uint32_t index, const std::string& label, const std::vector<uint32_t>& members)
    {
        bool matches = std::regex_search(label, o.only);
        bool music = false;
        for (const uint32_t sound : members)
        {
            matches = matches || std::regex_search(sounds[sound].name, o.only);
            music = music || ntdlib::rsar::IsMusic(archive, sound);
        }

        if (matches && (!o.bgm || music))
        {
            jobs.push_back({index, label, std::nullopt, std::nullopt, members});
        }
    };

    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        // Streams are out of scope, and wave sounds aren't ripped yet. A piece's jobs come at its first sound.
        const auto index = static_cast<uint32_t>(i);
        if (sounds[i].type != ntdlib::rsar::SoundType::kSequence)
        {
            continue;
        }

        if (!in_piece[i])
        {
            select(index, sounds[i].name, {index});
            continue;
        }

        for (const rip::Piece& piece : pieces)
        {
            if (piece.sounds.front() == index)
            {
                select(index, piece.name, piece.sounds);
            }
        }
    }

    return jobs;
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
            const std::vector<std::string> missing = ntdlib::rsar::MissingData(archive, sound);
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

// Measures the rips of `variant_jobs` (see VariantJobs) and adds them to `jobs`, after the plain rips. Each variant's
// rips are measured with the variant's effects added to `effects`, and in the variant's arrangement. A variant without
// an arrangement takes `arrangement`. Returns the number of rips taken out.
std::size_t MeasureVariants(std::vector<Job>& jobs, const std::vector<Job>& variant_jobs,
                            const ntdlib::rsar::SoundArchive& archive, const std::vector<uint8_t>& archive_bytes,
                            const GameCode& code, const nw4r::Arrangement& arrangement,
                            const nw4r::ArchiveEffects& effects, const Options& o)
{
    std::size_t silent = 0;
    for (std::size_t v = 0; v < effects.variants.size(); v++)
    {
        std::vector<Job> of_variant;
        for (const Job& job : variant_jobs)
        {
            if (job.variant == static_cast<int>(v))
            {
                of_variant.push_back(job);
            }
        }

        const nw4r::EffectVariant& variant = effects.variants[v];
        const nw4r::ArchiveEffects with = nw4r::WithVariant(effects, variant);
        silent += MeasureSequences(of_variant, archive, archive_bytes, code, variant.arrangement.value_or(arrangement),
                                   with, o);
        jobs.insert(jobs.end(), of_variant.begin(), of_variant.end());
    }

    return silent;
}

// Loads the sound archive at `archive_path` on the disc, from `bytes`. Prints an error and returns nothing if it can't
// be read.
std::optional<ntdlib::rsar::SoundArchive> LoadArchive(const std::vector<uint8_t>& bytes,
                                                      const std::string& archive_path)
{
    std::optional<ntdlib::rsar::SoundArchive> archive;
    try
    {
        archive = ntdlib::rsar::SoundArchive::Load(bytes, wsf::nw4r::kRsarOptions);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", archive_path.c_str(), e.what());
        return std::nullopt;
    }

    return archive;
}

// The effects that the rips of the archive at `archive_path` play: those of the game's effect profile. --tracks
// replaces the tracks that the game plays of a sound. The tag does the same in the player. --tracks, --tempo and --var
// rip the arrangement that they give. The rips then leave out the variants that have an arrangement.
nw4r::ArchiveEffects EffectsToRip(const GameCode& code, const std::string& archive_path,
                                  const ntdlib::rsar::SoundArchive& archive, const Options& o)
{
    nw4r::ArchiveEffects effects = rip::FindEffects(code.effect_profile, archive_path, archive);
    if (o.Arranged())
    {
        const auto arranged = [](const nw4r::EffectVariant& variant)
        {
            return variant.arrangement.has_value();
        };
        std::erase_if(effects.variants, arranged);
    }

    return effects;
}

// Adds `sound` to `sounds` unless `sounds` holds it already.
void AddOnce(std::vector<uint32_t>& sounds, uint32_t sound)
{
    if (std::find(sounds.begin(), sounds.end(), sound) == sounds.end())
    {
        sounds.push_back(sound);
    }
}

// The sounds that `jobs` play, each once, in the order of the jobs. A sound's rips in its variants play the plain
// rip's data.
std::vector<uint32_t> SoundsOf(const std::vector<Job>& jobs)
{
    std::vector<uint32_t> indices;
    for (const Job& job : jobs)
    {
        for (const uint32_t sound : job.sounds)
        {
            AddOnce(indices, sound);
        }
    }

    return indices;
}

// The version of the archive descriptor that a set of `jobs` requires. A set whose minis play several sounds gets
// version 2. Players of WSF 1.5.0 and older refuse it.
uint32_t DescriptorVersion(const std::vector<Job>& jobs)
{
    const auto several = [](const Job& job)
    {
        return job.sounds.size() > 1;
    };

    return std::any_of(jobs.begin(), jobs.end(), several) ? ArchiveDescriptor::kListVersion
                                                          : ArchiveDescriptor::kVersion;
}

// What the minis of an archive's set have in common: the archive and its path on the disc, the game's code, the
// effects that the rips play, and the version of the library's descriptor.
struct MiniSettings
{
    const ntdlib::rsar::SoundArchive& archive;
    const std::string& archive_path;
    const GameCode& code;
    const nw4r::ArchiveEffects& effects;
    uint32_t version = 0;
};

// The mini of `job` for the library `lib_name`, with `tags`. The wsf_sound tag gives the labels of the job's sounds.
// The rip of a variant with effects or sends holds a descriptor with them. That descriptor takes the place of the
// library's. The mood effect and a send to the main mix require version 3. Players of WSF 1.7.0 and older refuse
// version 3. A variant's arrangement goes in the rip's tags.
std::vector<uint8_t> BuildMini(const MiniSettings& settings, const std::string& lib_name, const Job& job, Tags tags)
{
    std::string labels;
    for (const uint32_t sound : job.sounds)
    {
        labels += (labels.empty() ? "" : ", ") + settings.archive.Sounds()[sound].name;
    }

    tags["wsf_sound"] = labels;

    std::optional<ArchiveDescriptor> descriptor;
    if (job.variant >= 0)
    {
        const nw4r::EffectVariant& variant = settings.effects.variants[static_cast<std::size_t>(job.variant)];
        if (variant.arrangement)
        {
            for (const auto& [key, value] : ArrangementTags(*variant.arrangement))
            {
                tags[key] = value;
            }
        }

        if (!variant.setups.empty() || !variant.sends.empty())
        {
            const nw4r::ArchiveEffects with = nw4r::WithVariant(settings.effects, variant);
            const uint32_t version = RequiresMoodVersion(with) ? ArchiveDescriptor::kMoodVersion : settings.version;
            descriptor = rip::BuildArchiveDescriptor(settings.archive_path, kMicrocodeName, settings.code.microcode,
                                                     settings.code.build, with, version);
        }
    }

    // The variables that the game sets on a sound that rips alone, under those of --var or the variant's arrangement.
    // The length analysis plays the same.
    if (job.sounds.size() == 1)
    {
        rip::AddSoundVariables(settings.effects, job.sounds.front(), tags);
    }

    return rip::BuildArchiveMini(job.sounds, lib_name, std::move(tags), descriptor);
}

// The effects that game mode's rips of the archive at `archive_path` play. Game mode rips a sound in a variant whose
// arrangement gives only the tracks that play. The driver silences the other tracks. It does the same for a sound
// whose tracks the profile gives (see rip::BuildMini). It sets no effects, sends, tempo or variables of a variant. The
// rips leave out the variants that have any.
nw4r::ArchiveEffects GameModeEffects(const GameCode& code, const std::string& archive_path,
                                     const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects effects = rip::FindEffects(code.effect_profile, archive_path, archive);
    const auto tracks_only = [](const nw4r::EffectVariant& variant)
    {
        return variant.setups.empty() && variant.sends.empty() && variant.arrangement &&
               variant.arrangement->tempo_ratio == 1.0f && variant.arrangement->variables.empty();
    };
    std::erase_if(effects.variants, std::not_fn(tracks_only));

    return effects;
}

// The effects of `variant` in game mode. A variant's rip gives the driver the variant's tracks for its sound.
nw4r::ArchiveEffects WithVariantTracks(const nw4r::ArchiveEffects& effects, const nw4r::EffectVariant& variant)
{
    nw4r::ArchiveEffects found = nw4r::WithVariant(effects, variant);
    for (const uint32_t sound : variant.sounds)
    {
        found.tracks.push_back({sound, variant.arrangement->tracks});
    }

    return found;
}

// The effects of game mode's library: `effects` with the tracks of every variant. The library then holds the offsets
// that silence a track, even where the profile gives no sound's tracks.
nw4r::ArchiveEffects LibraryEffects(const nw4r::ArchiveEffects& effects)
{
    nw4r::ArchiveEffects library_effects = effects;
    for (const nw4r::EffectVariant& variant : effects.variants)
    {
        const nw4r::ArchiveEffects with = WithVariantTracks(effects, variant);
        library_effects.tracks.insert(library_effects.tracks.end(), with.tracks.begin(), with.tracks.end());
    }

    return library_effects;
}

} // namespace

int RipArchive(const fs::path& out_dir, xsflib::rip::FileNames& names, const std::string& lib_base,
               const std::string& archive_path, const std::vector<uint8_t>& archive_bytes, const GameCode& code,
               const std::string& game_name, const Options& o, std::size_t& matched)
{
    const std::optional<ntdlib::rsar::SoundArchive> archive = LoadArchive(archive_bytes, archive_path);
    if (!archive)
    {
        return -1;
    }

    // Archive mode skips the sounds whose data is past the end (SkipIncomplete). Game mode plays what the game finds.
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

    const nw4r::ArchiveEffects effects = EffectsToRip(code, archive_path, *archive, o);
    nw4r::ArchiveEffects analyzed = effects;
    if (o.tracks)
    {
        analyzed.tracks.clear();
    }

    // The rips of a sound in an effect variant follow its plain rips. A variant without an arrangement takes the
    // options' arrangement.
    const std::vector<Job> variant_jobs = VariantJobs(jobs, effects);
    std::size_t silent = MeasureSequences(jobs, *archive, archive_bytes, code, o.ToArrangement(), analyzed, o);
    silent += MeasureVariants(jobs, variant_jobs, *archive, archive_bytes, code, o.ToArrangement(), analyzed, o);

    const std::string skipped_text = xsflib::cli::SkippedText(skipped, "data outside the archive", silent, "sequence");
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", archive_path.c_str(), skipped_text.c_str());
        return 0;
    }

    // The library keeps only the data that the ripped sequences play. A game's archive can hold hundreds of megabytes
    // of sound effects and voices that a rip of its music doesn't play.
    const std::vector<uint32_t> indices = SoundsOf(jobs);
    const uint32_t version = DescriptorVersion(jobs);
    const Tags common = xsflib::cli::RipTags(o, game_name, "wsf", "archive");
    const auto lib =
        rip::BuildArchiveLibrary(archive_path, ntdlib::rsar::KeepSequenceData(*archive, indices, nw4r::kRsarOptions),
                                 kMicrocodeName, code.microcode, code.build, common, effects, version);

    // The arrangement's tags go in the minis. The library then stays the same for every arrangement of its sounds.
    // --suffix goes in each mini's title and file name. The wsf_sound tag keeps the sound's label, or the labels of a
    // piece's sequences.
    Tags mini_tags = common;
    o.AddArrangementTags(mini_tags);
    for (Job& job : jobs)
    {
        job.label += o.suffix;
    }

    const MiniSettings settings = {*archive, archive_path, code, effects, version};
    const auto build_mini = [&settings](const std::string& lib_name, const Job& job, Tags tags)
    {
        return BuildMini(settings, lib_name, job, std::move(tags));
    };
    return WriteSet(out_dir, names, lib_base, lib, jobs, mini_tags, build_mini, archive_path, skipped_text);
}

int RipArchiveInGameMode(const fs::path& out_dir, xsflib::rip::FileNames& names, const std::string& lib_base,
                         const ntdlib::disc::GameFiles& game, const rip::GameProfile& profile,
                         const std::string& archive_path, const std::vector<uint8_t>& archive_bytes,
                         const GameCode& code, const std::string& game_name, const Options& o, std::size_t& matched)
{
    const std::optional<ntdlib::rsar::SoundArchive> loaded = LoadArchive(archive_bytes, archive_path);
    if (!loaded)
    {
        return -1;
    }

    const ntdlib::rsar::SoundArchive& archive = *loaded;
    std::vector<Job> jobs = SelectSounds(archive, o);
    matched += jobs.size();
    if (jobs.empty())
    {
        return 0;
    }

    const std::size_t skipped = SkipUngrouped(jobs, archive);

    const nw4r::ArchiveEffects effects = GameModeEffects(code, archive_path, archive);
    const std::vector<Job> variant_jobs = VariantJobs(jobs, effects);
    std::size_t silent = MeasureSequences(jobs, archive, archive_bytes, code, {}, effects, o);
    silent += MeasureVariants(jobs, variant_jobs, archive, archive_bytes, code, {}, effects, o);
    const std::string skipped_text = xsflib::cli::SkippedText(skipped, "no group holds them", silent, "sequence");
    if (jobs.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", archive_path.c_str(), skipped_text.c_str());
        return 0;
    }

    // The library holds the data of the groups that the driver loads for the sounds. A sound's rips in its variants
    // play the plain rip's data.
    std::vector<uint32_t> indices;
    for (const Job& job : jobs)
    {
        AddOnce(indices, job.index);
    }

    const Tags common = xsflib::cli::RipTags(o, game_name, "wsf", "game");
    std::string error;
    const auto lib =
        rip::BuildLibrary(game, profile, archive_path, archive, indices, common, &error, LibraryEffects(effects));
    if (!lib)
    {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return -1;
    }

    // The wsf_sound tag gives the sound's label, without the name of a variant.
    const auto build_mini = [&archive, &effects, &code](const std::string& lib_name, const Job& job, Tags tags)
    {
        tags["wsf_sound"] = archive.Sounds()[job.index].name;
        if (job.variant < 0)
        {
            return rip::BuildMini(job.index, *job.data, lib_name, tags, effects, code.effect_profile);
        }

        const nw4r::EffectVariant& variant = effects.variants[static_cast<std::size_t>(job.variant)];
        return rip::BuildMini(job.index, *job.data, lib_name, tags, WithVariantTracks(effects, variant),
                              code.effect_profile);
    };
    return WriteSet(out_dir, names, lib_base, *lib, jobs, common, build_mini, archive_path, skipped_text);
}

} // namespace wsf::cli
