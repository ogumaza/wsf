// SPDX-License-Identifier: MIT

// Archive mode (see archive_player.h).

#include "wsf/archive_player.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ax/microcode.h"
#include "common/bytes.h"
#include "nw4r/engine.h"
#include "rsar/formats.h"

namespace wsf
{

struct ArchivePlayer::State
{
    std::optional<rsar::SoundArchive> archive; // declared first so that it outlives the model's engine
    nw4r::ArchiveModel model;
};

struct ArchivePlayer::Snapshot
{
    nw4r::ArchiveModel::Snapshot model;
};

ArchivePlayer::ArchivePlayer() = default;

ArchivePlayer::~ArchivePlayer() = default;

namespace
{

const std::vector<uint8_t>* FindFile(const LoadedSet& set, const std::string& path)
{
    const std::vector<uint8_t>* found = nullptr;
    for (const auto& f : set.files)
    {
        if (f.path == path)
        {
            found = &f.data; // later chunks replace earlier ones
        }
    }

    return found;
}

} // namespace

std::optional<std::string> ArchivePlayer::Start(const LoadedSet& set)
{
    if (!set.archive)
    {
        return "not an archive-mode set";
    }

    const ArchiveDescriptor& d = *set.archive;
    const std::vector<uint8_t>* archive_bytes = FindFile(set, d.archive_path);
    if (!archive_bytes)
    {
        return "the sound archive (" + d.archive_path + ") is missing";
    }

    const std::vector<uint8_t>* microcode_bytes = FindFile(set, d.microcode_path);
    if (!microcode_bytes || microcode_bytes->size() != ax::kMicrocodeWords * 2)
    {
        return "AX's microcode (" + d.microcode_path + ") is missing";
    }

    if (!set.sound)
    {
        return "no sequence selected";
    }

    ax::Microcode microcode;
    microcode.address = d.microcode_address;
    for (std::size_t i = 0; i < ax::kMicrocodeWords; i++)
    {
        microcode.words.push_back(LoadBe16(&(*microcode_bytes)[i * 2]));
    }

    output_.clear();
    state_ = std::make_unique<State>();
    State& s = *state_;
    s.archive = rsar::SoundArchive::Load(*archive_bytes);
    s.model.build = nw4r::SndBuild{d.snd_build};
    if (!s.model.Start(microcode, *s.archive))
    {
        return "AX's microcode didn't start";
    }

    if (!s.model.engine->StartSequence(*set.sound))
    {
        return "sound " + std::to_string(*set.sound) + " isn't a sequence, or its data isn't in the archive";
    }

    return std::nullopt;
}

bool ArchivePlayer::RunFrame()
{
    return state_ && state_->model.engine && state_->model.RunFrame(output_);
}

bool ArchivePlayer::Busy() const
{
    return state_ && state_->model.engine && state_->model.engine->IsBusy();
}

std::shared_ptr<ArchivePlayer::Snapshot> ArchivePlayer::Save(const Snapshot* previous) const
{
    return std::make_shared<Snapshot>(Snapshot{state_->model.Save(previous ? &previous->model : nullptr)});
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    state_->model.Restore(snapshot.model);
}

} // namespace wsf
