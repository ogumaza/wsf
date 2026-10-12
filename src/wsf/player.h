// SPDX-License-Identifier: MIT

// WSF player core. Loads a .wsflib/.miniwsf set and produces its audio. Game mode runs the game's code, including its
// sound code and OS, on an emulated Wii. Archive mode uses WSF's model of the SDK's sound player. Both modes run the
// game's AX microcode on the emulated DSP. Portable C++ with no platform audio or UI. The CLI renderer and the
// foobar2000 component use it.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/flipper/file_disc.h"
#include "xsflib/player/playback.h"
#include "xsflib/player/player.h"

#include "wsf/format.h"

namespace wsf
{
namespace wii
{

class System;

} // namespace wii

class ArchivePlayer;

class Player : public xsflib::player::PlayerBase<Player, LoadedSet>
{
public:
    // The source's part of a snapshot: the emulated Wii's state in game mode, or the model's in archive mode. Unchanged
    // memory pages are shared with the snapshot taken before it. A snapshot can only be restored to the player that
    // took it, before its next Start, with the audio that the player had rendered but not returned yet.
    struct SourceSnapshot;
    using Snapshot = xsflib::player::Snapshot<SourceSnapshot>;

    // The rate players report, the audio interface's DMA rate: 32,000 Hz.
    static constexpr int kSampleRate = kPlayerSampleRate;

    Player();
    ~Player();

    // Game mode's emulated Wii after a start, for an analysis that watches the game's code while the sound plays. Null
    // in archive mode.
    wii::System* GameSystem()
    {
        return system_.get();
    }

private:
    friend class xsflib::player::PlayerBase<Player, LoadedSet>;

    // The error for a set with nothing to play, and the start of the message for a start or a step that threw.
    static constexpr const char* kNotPlayable = "no process or archive descriptor";
    static constexpr const char* kStartFailure = "emulation failed: ";
    static constexpr const char* kStepFailure = "emulation failed: ";

    static std::optional<std::string> LoadFile(const std::string& path, const FileReader& reader, Set& set)
    {
        return LoadSet(path, reader, set);
    }

    void Unload();
    bool Playable() const;

    // Game mode: boots the emulated Wii and runs it until the sound starts (the silence before is skipped). Archive
    // mode: starts the DSP and the sound.
    bool StartSource();

    bool RunStep();

    // Whether the sound has finished: the driver reports status 2.
    bool Finished() const;

    std::vector<int16_t>& Output();
    uint32_t DriverStatus() const;
    uint64_t EmulatedCycles() const;
    std::shared_ptr<const SourceSnapshot> SaveSource(const SourceSnapshot* previous);
    bool CanRestore(const SourceSnapshot* source) const;
    void RestoreSource(const SourceSnapshot& source);

    std::unique_ptr<ntdlib::flipper::FileDisc> disc_; // declared before the system that reads it
    std::unique_ptr<wii::System> system_;
    std::vector<int16_t> system_output_;
    std::unique_ptr<ArchivePlayer> archive_;
};

// Playback of a WSF set: xsflib's playback for WSF's Player.
using Playback = xsflib::player::Playback<Player>;
using PlaybackOptions = xsflib::player::PlaybackOptions<Player>;

} // namespace wsf
