// SPDX-License-Identifier: MIT

// Archive mode (see archive_player.h), on xsflib's ArchiveSource.

#include "wsf/archive_player.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/bytes.h"
#include "xsflib/player/archive_source.h"

#include "nw4r/arrangement.h"
#include "nw4r/effect_setup.h"
#include "nw4r/engine.h"
#include "nw4r/rsar_options.h"
#include "nw4r/util.h"

namespace wsf
{
namespace
{

// WSF's steps of archive mode: the set's archive and AX's microcode, the model of NW4R SND and AX, and each sequence in
// the arrangement that the tags and the descriptor give.
struct ArchiveTraits
{
    using Set = const LoadedSet;
    using Archive = ntdlib::rsar::SoundArchive;
    using Model = nw4r::ArchiveModel;
    using Snapshot = nw4r::ArchiveModel::Snapshot;

    struct Prepared
    {
        const ArchiveDescriptor* descriptor = nullptr;
        const std::vector<uint8_t>* archive = nullptr;
        ntdlib::Microcode microcode;
        std::vector<uint32_t> sounds;                // in the order in which they start
        std::vector<nw4r::Arrangement> arrangements; // one for each sound, from the tags and the descriptor's tracks
    };

    static constexpr bool kKeepArchive = false;

    static std::optional<std::string> Prepare(Set& set, Prepared& prepared)
    {
        if (!set.archive)
        {
            return "not an archive-mode set";
        }

        const ArchiveDescriptor& d = *set.archive;
        prepared.descriptor = &d;
        prepared.archive = set.File(d.archive_path);
        if (!prepared.archive)
        {
            return "the sound archive (" + d.archive_path + ") is missing";
        }

        const std::vector<uint8_t>* microcode_bytes = set.File(d.microcode_path);
        if (!microcode_bytes || microcode_bytes->size() != ntdlib::kMicrocodeWords * 2)
        {
            return "AX's microcode (" + d.microcode_path + ") is missing";
        }

        prepared.sounds = SoundsToPlay(set);
        if (prepared.sounds.empty())
        {
            return "no sequence selected";
        }

        if (prepared.sounds.size() > kMaxSounds)
        {
            return std::to_string(prepared.sounds.size()) + " sounds selected; a set plays at most " +
                   std::to_string(kMaxSounds) + " together";
        }

        // The tracks that the game plays of each sound, unless the tags give others. The tags apply to every sound.
        prepared.arrangements.clear();
        for (const uint32_t sound : prepared.sounds)
        {
            prepared.arrangements.push_back(ArrangementFromTags(set.tags, nw4r::TracksOf(d.effects.tracks, sound)));
        }

        prepared.microcode.address = d.microcode_address;
        for (std::size_t i = 0; i < ntdlib::kMicrocodeWords; i++)
        {
            prepared.microcode.words.push_back(xsflib::LoadBe16(&(*microcode_bytes)[i * 2]));
        }

        return std::nullopt;
    }

    static std::optional<std::string> LoadArchive(Set&, const Prepared& prepared, std::optional<Archive>& archive)
    {
        archive = ntdlib::rsar::SoundArchive::Load(*prepared.archive, wsf::nw4r::kRsarOptions);

        return std::nullopt;
    }

    static std::unique_ptr<Model> MakeModel(const Prepared& prepared, const Archive&)
    {
        auto model = std::make_unique<Model>();
        model->build = nw4r::SndBuild{prepared.descriptor->snd_build};

        return model;
    }

    static std::optional<std::string> StartModel(Model& model, Archive& archive, const Prepared& prepared)
    {
        if (!model.Start(prepared.microcode, archive))
        {
            return "AX's microcode didn't start";
        }

        return std::nullopt;
    }

    static std::optional<std::string> StartSequence(Model& model, Set&, const Prepared& prepared)
    {
        // The effects that the game puts on the aux buses for the first sound, and the sends that it sets on each
        // sound. The first AX frame after the sequences have started appends the effects. Every sequence starts before
        // the first frame, in the list's order (docs/wsf.md, "Sounds that play together").
        const nw4r::ArchiveEffects& effects = prepared.descriptor->effects;
        model.engine->SetEffects(effects.setups, effects.sends, prepared.sounds.front());
        for (std::size_t i = 0; i < prepared.sounds.size(); i++)
        {
            if (!model.engine->StartSequence(prepared.sounds[i], prepared.arrangements[i]))
            {
                return "sound " + std::to_string(prepared.sounds[i]) +
                       " isn't a sequence, or its data isn't in the archive";
            }
        }

        return std::nullopt;
    }

    static bool RunFrame(Model& model, std::vector<int16_t>& out)
    {
        return model.engine && model.RunFrame(out);
    }

    static bool Busy(const Model& model)
    {
        return model.engine && model.engine->IsBusy();
    }

    static Snapshot Save(Model& model, const Snapshot* previous)
    {
        return model.Save(previous);
    }

    static void Restore(Model& model, const Snapshot& snapshot)
    {
        model.Restore(snapshot);
    }
};

} // namespace

struct ArchivePlayer::State
{
    xsflib::player::ArchiveSource<ArchiveTraits> source;
};

struct ArchivePlayer::Snapshot
{
    std::shared_ptr<ArchiveTraits::Snapshot> model;
};

ArchivePlayer::ArchivePlayer() : state_(std::make_unique<State>())
{
}

ArchivePlayer::~ArchivePlayer() = default;

std::optional<std::string> ArchivePlayer::Start(const LoadedSet& set)
{
    output_.clear();

    return state_->source.Start(set, output_);
}

bool ArchivePlayer::RunFrame()
{
    return state_->source.RunFrame(output_);
}

bool ArchivePlayer::Busy() const
{
    return state_->source.Busy();
}

std::shared_ptr<ArchivePlayer::Snapshot> ArchivePlayer::Save(const Snapshot* previous) const
{
    return std::make_shared<Snapshot>(Snapshot{state_->source.Save(previous ? previous->model.get() : nullptr)});
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    state_->source.Restore(*snapshot.model);
}

} // namespace wsf
