// SPDX-License-Identifier: MIT

// Archive mode (docs/wsf.md): one sequence from a sound archive. WSF's model of the SDK's sound player (nw4r::snd and
// AX's CPU side, src/nw4r and src/ax) plays it. The game's AX microcode runs on the emulated DSP. Used when a rip is
// made from a sound archive rather than from the game's code.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wsf/format.h"

namespace wsf
{

class ArchivePlayer
{
public:
    // A snapshot of the model between frames, without the output. It can only be restored to this player before its
    // next Start.
    struct Snapshot;

    ArchivePlayer();
    ~ArchivePlayer();

    // Loads the sound archive and the microcode named by the set's archive descriptor. Starts the DSP and the selected
    // sound. Returns an error message on failure.
    std::optional<std::string> Start(const LoadedSet& set);

    // Runs one AX frame (96 output samples). Returns false if the microcode stopped responding.
    bool RunFrame();

    // False once the sequence has ended and no voice is sounding.
    bool Busy() const;

    // Takes a snapshot after Start, and restores one. A snapshot shares its unchanged memory pages with `previous`.
    // Restore throws std::runtime_error for a snapshot that the model can't return to.
    std::shared_ptr<Snapshot> Save(const Snapshot* previous) const;
    void Restore(const Snapshot& snapshot);

    // The DSP's output (interleaved L, R), appended by RunFrame.
    std::vector<int16_t>& Output()
    {
        return output_;
    }

private:
    struct State;

    std::unique_ptr<State> state_;
    std::vector<int16_t> output_;
};

} // namespace wsf
