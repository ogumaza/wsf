// SPDX-License-Identifier: MIT

// The NW4R SND sound engine for one sound archive: the voice and channel managers on the AX model, the banks and the
// sequence players, and the sound frame that NW4R SND's sound thread runs after each AX frame
// (SoundThread::SoundThreadProc, main.dol 0x80047e68).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ax/dsp_link.h"
#include "ax/microcode.h"
#include "ax/system.h"
#include "common/common_types.h"
#include "dsp/dsp.h"
#include "dsp/main_memory.h"
#include "nw4r/driver.h"
#include "nw4r/sequence.h"
#include "nw4r/util.h"
#include "rsar/formats.h"

namespace wsf::nw4r
{

// AxManager's zero buffer: 256 bytes of silence that a wave without a loop plays on into. This is the physical address
// that Wii Sports uses (in the .bss of its main.dol).
constexpr PAddr kZeroBuffer = 0x003d0640;
constexpr uint32_t kZeroBufferSize = 0x100;

// A bank, with its waves' samples copied into MEM2.
struct LoadedBank
{
    rsar::Bank bank;
    std::span<const uint8_t> wave_data; // in the archive's bytes
    PAddr wave_address = 0;             // where wave_data is in MEM2
};

class Engine
{
public:
    // The engine's state between sound frames, for a snapshot of the model: its pools, its players' state and the
    // random numbers. The snapshot points at this engine's objects and can only be restored to this engine.
    struct Snapshot
    {
        AxVoiceManager::Snapshot ax_voices;
        VoiceManager::Snapshot voices;
        ChannelManager::Snapshot channels;
        Random random;
        std::array<int16_t, 16> global_variables{};
        std::vector<SeqPlayer::Snapshot> players;
        std::size_t banks = 0; // the banks loaded by then
    };

    // Sets up NW4R SND's side of AX (AxManager::Init, 0x800313c4): the zero buffer, and the frame callback through
    // which AX's interrupt ends the voices' ramps (SoundThread::AxCallbackFunc, 0x80047c44). `ax` must be initialised.
    // `archive` must outlive the engine. The engine follows `build` of NW4R SND.
    Engine(MainMemory& memory, ax::System& ax, const rsar::SoundArchive& archive, SndBuild build);

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine();

    MainMemory& Memory()
    {
        return memory_;
    }

    const MainMemory& Memory() const
    {
        return memory_;
    }

    ax::System& Ax()
    {
        return ax_;
    }

    AxVoiceManager& AxVoices()
    {
        return ax_voices_;
    }

    VoiceManager& Voices()
    {
        return voices_;
    }

    ChannelManager& Channels()
    {
        return channels_;
    }

    Random& Rng()
    {
        return random_;
    }

    SndBuild Build() const
    {
        return build_;
    }

    PAddr ZeroBuffer() const
    {
        return kZeroBuffer;
    }

    // AxManager::GetOutputVolume (0x80031540): the master volume. Archive mode leaves it at 1.
    float OutputVolume() const
    {
        return 1.0f;
    }

    // SeqPlayer's global variables. Every player shares them. They're zero until the game sets one. Archive mode never
    // sets them.
    std::array<int16_t, 16>& GlobalVariables()
    {
        return global_variables_;
    }

    // Starts the sequence sound at `index` in the archive's sound table, from the next sound frame. Returns its player,
    // or nullptr if the sound isn't a sequence or its sequence or bank isn't in the archive. Throws rsar::FormatError
    // for a sequence or bank that can't be read.
    SeqPlayer* StartSequence(uint32_t index);

    // Bank::NoteOn (0x80035b68): finds the region for the note and starts a channel on its wave.
    Channel* NoteOn(const LoadedBank& bank, const NoteOnInfo& info);

    // The sound frame, after AX's frame (SoundThreadProc). Frees the voices that AX dropped. Updates AxManager, the
    // players, the channels and the voices. Advances the random numbers.
    void FrameProcess();

    // Whether a sequence is still playing, or any note still sounds.
    bool IsBusy() const;

    // Takes a snapshot between sound frames.
    Snapshot Save() const;

    // Restores a snapshot of this engine. The model's memory must be restored to the same moment. Throws
    // std::runtime_error if the engine has started a sequence or loaded a bank since the snapshot. A restore can't undo
    // those.
    void Restore(const Snapshot& snapshot);

private:
    // Returns the bank with `bank_id`. The first call loads the bank. Returns nullptr if the archive doesn't hold it.
    const LoadedBank* GetBank(uint32_t bank_id);

    // The wave parameters that WaveFileReader::ReadWaveParam (0x8004eca8) reads for a wave of a bank.
    WaveData ReadWaveParam(const LoadedBank& bank, const rsar::Wave& wave) const;

    MainMemory& memory_;
    ax::System& ax_;
    const rsar::SoundArchive& archive_;
    AxVoiceManager ax_voices_;
    VoiceManager voices_;
    ChannelManager channels_;
    Random random_;
    SndBuild build_;
    std::array<int16_t, 16> global_variables_{};
    std::map<uint32_t, std::unique_ptr<LoadedBank>> banks_;
    std::vector<std::unique_ptr<rsar::Sequence>> sequences_;

    // Declared last so that the players are destroyed first. The players detach their tracks' channels while those
    // channels still exist.
    std::vector<std::unique_ptr<SeqPlayer>> players_;
};

// Archive mode's whole stack for one sound archive: main memory, the DSP running the game's AX microcode, AX's CPU side
// and the engine. The player and the ripper's length analysis both build it this way.
struct ArchiveModel
{
    // A snapshot of the model between frames: its memory, the DSP, AX and the engine. The snapshot points at this
    // model's objects and can only be restored to this model.
    struct Snapshot
    {
        MainMemory::Snapshot memory;
        dsp::Dsp dsp;
        ax::System ax;
        Engine::Snapshot engine;
    };

    // Starts `microcode` on the DSP, then AX and the engine on `archive`. `archive` must outlive the model. Returns
    // false if the microcode didn't start.
    bool Start(const ax::Microcode& microcode, const rsar::SoundArchive& archive);

    // One frame: AX's frame, which the microcode mixes, then the sound frame. Appends the frame's 96 stereo samples to
    // `output`, left then right. Returns false if the microcode stopped responding.
    bool RunFrame(std::vector<int16_t>& output);

    // Starts AX and the engine without the DSP. This follows a sequence that the game's microcode can't be made to mix.
    // Each RunFrameWithoutDsp runs AX's frame and the sound frame. In place of the microcode, it updates the voices'
    // positions. A wave that doesn't loop then ends as it would on the DSP. Nothing is mixed.
    void StartWithoutDsp(const rsar::SoundArchive& archive);
    void RunFrameWithoutDsp();

    // Takes a snapshot between frames, after Start. The snapshot shares its unchanged memory pages with `previous`.
    // Restore throws std::runtime_error for a snapshot that the engine can't return to (see Engine::Restore).
    Snapshot Save(const Snapshot* previous = nullptr) const;
    void Restore(const Snapshot& snapshot);

    MainMemory memory;
    dsp::Dsp dsp{memory};
    ax::System ax{memory};
    ax::DspLink link{dsp};
    std::optional<Engine> engine;
    SndBuild build; // the build of NW4R SND that the engine follows, set before Start
};

} // namespace wsf::nw4r
