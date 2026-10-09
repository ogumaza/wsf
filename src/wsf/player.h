// SPDX-License-Identifier: MIT

// WSF player core. Loads a .wsflib/.miniwsf set and produces its audio. Game mode runs the game's code, including its
// sound code and OS, on an emulated Wii. Archive mode uses WSF's model of the SDK's sound player. Both modes run the
// game's AX microcode on the emulated DSP. Portable C++ with no platform audio or UI. The CLI renderer and the
// foobar2000 component use it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "wsf/format.h"

namespace wsf
{
namespace wii
{

class FileDisc;
class System;

} // namespace wii

class ArchivePlayer;

class Player
{
public:
    enum class State
    {
        kIdle,
        kPlaying,
        kFinished,
        kError
    };

    // The rate players report, the audio interface's DMA rate: 32,000 Hz.
    static constexpr int kSampleRate = kPlayerSampleRate;

    // A snapshot of the player between emulation steps, with the audio that the player has rendered but not returned
    // yet. Unchanged memory pages are shared with the snapshot taken before it. The snapshot points at this player's
    // emulation and can only be restored to this player before the next Start.
    struct Snapshot;

    Player();
    ~Player();

    // Loads a file and its libraries, without starting emulation.
    bool Load(const std::string& path, const FileReader& reader);

    // Loads an already assembled set.
    bool Load(LoadedSet set);

    // Game mode: boots the emulated Wii and runs it until the sound starts (the silence before is skipped). Archive
    // mode: starts the DSP and the sound. Can be called again to restart from the beginning.
    bool Start();

    // Renders up to `frames` stereo frames (interleaved L, R) into `out`. Returns the number of frames written. It's
    // less than requested only after an error.
    std::size_t Render(int16_t* out, std::size_t frames);

    State GetState() const
    {
        return state_;
    }

    // Returns the frame count at the end of the emulation step that first reported the sound finished, independent of
    // Render's requested block size. Only meaningful in State::kFinished.
    uint64_t FinishedFrame() const
    {
        return finished_frame_;
    }

    const std::string& Error() const
    {
        return error_;
    }

    const Tags& GetTags() const
    {
        return set_.tags;
    }

    // Tagged length and fade in milliseconds (-1 if absent).
    long long LengthMs() const;
    long long FadeMs() const;

    // Tagged volume (linear gain, 1.0 if absent).
    double Volume() const;

    // The rip's files that the player loaded.
    const LoadedSet& GetLoadedSet() const
    {
        return set_;
    }

    // Takes a snapshot that shares pages with `previous` (ideally the latest snapshot). Returns nullptr when the player
    // isn't playing.
    std::shared_ptr<const Snapshot> Save(const Snapshot* previous = nullptr);

    // Restores `snapshot`, including any audio waiting to be handed out. Returns false if the snapshot belongs to an
    // earlier run of the player or to another player, or if the emulation can't go back to it (see Error()).
    bool Restore(const Snapshot& snapshot);

private:
    bool Fail(const std::string& message);
    bool StartImpl();
    uint32_t DriverStatus() const;
    bool Step();
    std::vector<int16_t>& Output();

    LoadedSet set_;
    std::unique_ptr<wii::FileDisc> disc_; // declared before the system that reads it
    std::unique_ptr<wii::System> system_;
    std::vector<int16_t> system_output_;
    std::unique_ptr<ArchivePlayer> archive_;
    State state_ = State::kIdle;
    std::string error_;
    uint64_t run_ = 0;            // counts Start calls so that a snapshot stays with the run it was taken in
    std::size_t consumed_ = 0;    // samples of Output() already handed out (two per frame)
    uint64_t dropped_ = 0;        // samples dropped from the front of Output() since the sound started
    uint64_t finished_frame_ = 0; // see FinishedFrame
};

} // namespace wsf
