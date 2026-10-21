// SPDX-License-Identifier: MIT

// The NW4R SND sound engine for one sound archive: the voice and channel managers on the AX model, the banks and the
// sequence players, and the sound frame that NW4R SND's sound thread runs after each AX frame
// (SoundThread::SoundThreadProc, main.dol 0x80047e68).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/dsp/dsp.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/length_hooks.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/types.h"

#include "ax/dsp_link.h"
#include "ax/pb.h"
#include "ax/system.h"
#include "nw4r/arrangement.h"
#include "nw4r/driver.h"
#include "nw4r/effect_setup.h"
#include "nw4r/effects.h"
#include "nw4r/sequence.h"
#include "nw4r/util.h"

namespace wsf::nw4r
{

// AxManager's zero buffer: 256 bytes of silence that a wave without a loop plays on into. This is the physical address
// that Wii Sports uses (in the .bss of its main.dol).
constexpr ntdlib::PAddr kZeroBuffer = 0x003d0640;
constexpr uint32_t kZeroBufferSize = 0x100;

// A bank, with its waves' samples copied into MEM2.
struct LoadedBank
{
    ntdlib::rsar::Bank bank;
    std::span<const uint8_t> wave_data; // in the archive's bytes
    ntdlib::PAddr wave_address = 0;     // where wave_data is in MEM2
};

// The engine for one sound archive (see the file's comment). The length analysis's hooks for the sequence's loops, for
// every track's passes and for each open of a track by another track come from ntdlib's LengthHooks.
class Engine : public ntdlib::nwsnd::LengthHooks
{
public:
    // The engine's state between sound frames, for a snapshot of the model: its pools, its players' state and the
    // random numbers. The snapshot points at this engine's objects and can only be restored to this engine.
    struct Snapshot
    {
        AxVoiceManager::Snapshot ax_voices;
        VoiceManager::Snapshot voices;
        ChannelManager channels;
        Random random;
        std::array<int16_t, 16> global_variables{};
        std::vector<SeqPlayer> players;
        std::size_t banks = 0; // the banks loaded by then
        Effects effects;
    };

    // The other hooks that the length analysis and the tests set (see SetCommandStartHook, SetVoiceParamHook and
    // SetChannelEventHook).
    using VoiceParamHook =
        std::function<void(ChannelId owner, const ntdlib::nwsnd::RevolutionPolicy::ChannelVoiceParam& param)>;
    using ChannelEventHook = std::function<void(const Channel& channel, Channel::CallbackStatus status)>;
    using CommandStartHook = std::function<void(int track, uint32_t offset)>;

    // Sets up NW4R SND's side of AX (AxManager::Init, 0x800313c4): the zero buffer, and the frame callback through
    // which AX's interrupt ends the voices' ramps (SoundThread::AxCallbackFunc, 0x80047c44) and runs the effects' part
    // (see SetEffects). `ax` must be initialised. `archive` must outlive the engine. The engine follows `build` of NW4R
    // SND.
    Engine(ntdlib::dsp::WiiMemory& memory, ax::System& ax, const ntdlib::rsar::SoundArchive& archive, SndBuild build);

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine();

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

    const ChannelManager& Channels() const
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

    ntdlib::PAddr ZeroBuffer() const
    {
        return kZeroBuffer;
    }

    // AxManager::GetOutputVolume (0x80031540): the master volume. Archive mode leaves it at 1.
    float OutputVolume() const
    {
        return 1.0f;
    }

    // SeqPlayer's global variables. Every player shares them. They're zero until the game sets one. In archive mode, an
    // arrangement can set them before its sequence starts (see StartSequence).
    std::array<int16_t, 16>& GlobalVariables()
    {
        return global_variables_;
    }

    // Sets up the effects that the game puts on the aux buses for `sound`, from a set's `setups` (see EffectsFor,
    // FollowsVariables and Effects::Set). Where the sound's variables pick an effect's presets, the engine reads the
    // variables of the sequence that starts first. Every sequence that the engine plays gets the effects. Each sequence
    // that starts later gets the sends that `sends` gives its sound (see SendsOf and SoundSends).
    void SetEffects(const std::vector<EffectSetup>& setups, const std::vector<SoundSend>& sends, uint32_t sound)
    {
        effects_.Set(EffectsFor(setups, sound), AxfxBuild(build_), FollowsVariables(setups, sound));
        sends_ = sends;
    }

    // Whether any effect is set up.
    bool HasEffects() const
    {
        return effects_.Any();
    }

    // The sends that the game sets through a sound's handle before the sound's first update (BasicSound::SetFxSend),
    // for the sequence of the player at `player` in the order of their starts. BasicSound::Update passes them to the
    // sound's player, and each track adds the player's sends to the track's sends (SeqTrack::UpdateChannelParam). A
    // player that the engine doesn't have gets none.
    std::array<float, ax::kAuxBusCount> SoundSends(int player) const
    {
        if (player < 0 || static_cast<std::size_t>(player) >= player_sends_.size())
        {
            return {};
        }

        return player_sends_[static_cast<std::size_t>(player)];
    }

    // Starts the sequence sound at `index` in the archive's sound table, from the next sound frame, in `arrangement`.
    // Returns its player, or nullptr if the sound isn't a sequence or its sequence or bank isn't in the archive. Throws
    // xsflib::FormatError for a sequence or bank that can't be read.
    SeqPlayer* StartSequence(uint32_t index, const Arrangement& arrangement = {});

    // A note on the bank in a sequence's bank slot `slot` of `banks` (SeqPlayer::NoteOn, 0x8003f3c8, through
    // SoundArchivePlayer's note-on callback, then Bank::NoteOn, 0x80035b68): the region for the note, and a channel
    // that starts its wave. The Wii has no bank select. Its sequences play the bank in slot 0. Returns nullptr if the
    // bank, the region or the wave isn't there, or no channel is free.
    Channel* NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info);

    // Passes a channel's callback to its owner's player (ChannelCallbackFunc's call through the track's pointer).
    void ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus status);

    // Passes a voice's event to the channel that owns it (VoiceCallbackFunc's call through the callback's data).
    void OnVoiceEvent(ChannelId owner, Voice::Status status);

    // The sound frame, after AX's frame (SoundThreadProc). Frees the voices that AX dropped. Updates AxManager, the
    // players and the channels. Advances the random numbers. Updates the voices.
    void FrameProcess();

    // Whether a sequence is still playing, or any note still sounds.
    bool IsBusy() const;

    // The length analysis's hook for each command that a track starts. ntdlib's sequence player calls OnCommandStart as
    // a track starts a command. The hook receives the track and the offset of the command's first byte, prefixes
    // included.
    void SetCommandStartHook(CommandStartHook hook)
    {
        on_command_start_ = std::move(hook);
    }

    void OnCommandStart(const SeqPlayer&, int track, uint32_t offset, uint32_t, bool)
    {
        if (on_command_start_)
        {
            on_command_start_(track, offset);
        }
    }

    // The counted loops that the length analysis takes as endless, each a track and the start of its loop's body (see
    // ntdlib's LoopsForEver). The players end every other counted loop as the game does.
    void SetEndlessLoops(std::set<std::pair<int, uint32_t>> loops)
    {
        endless_loops_ = std::move(loops);
    }

    bool LoopsForEver(const SeqPlayer&, int track, uint32_t address) const
    {
        return endless_loops_.contains({track, address});
    }

    // Hooks for tests. While set, the voice parameter hook receives the values that a channel gives its voice in each
    // call of Voice::SetParam (through OnVoiceParam), with the channel's place. The channel event hook receives each
    // callback from a channel, with its status, before the channel's player.
    void SetVoiceParamHook(VoiceParamHook hook)
    {
        on_voice_param_ = std::move(hook);
    }

    void SetChannelEventHook(ChannelEventHook hook)
    {
        on_channel_event_ = std::move(hook);
    }

    void OnVoiceParam(ChannelId owner, const ntdlib::nwsnd::RevolutionPolicy::ChannelVoiceParam& param) const
    {
        if (on_voice_param_)
        {
            on_voice_param_(owner, param);
        }
    }

    // Takes a snapshot between sound frames.
    Snapshot Save() const;

    // Restores a snapshot of this engine. The model's memory must be restored to the same moment. Throws
    // std::runtime_error if the engine has started a sequence or loaded a bank since the snapshot. A restore can't undo
    // those.
    void Restore(const Snapshot& snapshot);

private:
    // AX's frame callback: NotifyVoiceUpdate, then the effects' part.
    void OnAxFrame();

    // Returns the bank with `bank_id`. The first call loads the bank. Returns nullptr if the archive doesn't hold it.
    const LoadedBank* GetBank(uint32_t bank_id);

    // The wave parameters that WaveFileReader::ReadWaveParam (0x8004eca8) reads for a wave of a bank.
    WaveData ReadWaveParam(const LoadedBank& bank, const ntdlib::rsar::Wave& wave) const;

    ntdlib::dsp::WiiMemory& memory_;
    ax::System& ax_;
    const ntdlib::rsar::SoundArchive& archive_;
    AxVoiceManager ax_voices_;
    VoiceManager voices_;
    ChannelManager channels_;
    Random random_;
    SndBuild build_;
    std::array<int16_t, 16> global_variables_{};
    std::map<uint32_t, std::unique_ptr<LoadedBank>> banks_;
    std::vector<std::unique_ptr<ntdlib::rsar::Sequence>> sequences_;

    // The players, in the order they started. A channel's owner gives its player's place here.
    std::vector<std::unique_ptr<SeqPlayer>> players_;

    // The sends that the game sets on sounds (see SetEffects), and those of each player's sound, by the player's place.
    std::vector<SoundSend> sends_;
    std::vector<std::array<float, ax::kAuxBusCount>> player_sends_;

    Effects effects_;

    CommandStartHook on_command_start_;
    std::set<std::pair<int, uint32_t>> endless_loops_;
    VoiceParamHook on_voice_param_;
    ChannelEventHook on_channel_event_;
};

// Archive mode's whole stack for one sound archive: main memory, the DSP running the game's AX microcode, AX's CPU side
// and the engine. The player and the ripper's length analysis both build it this way.
struct ArchiveModel
{
    // A snapshot of the model between frames: its memory, the DSP, AX and the engine. The snapshot points at this
    // model's objects and can only be restored to this model.
    struct Snapshot
    {
        ntdlib::dsp::WiiMemory::Snapshot memory;
        ntdlib::dsp::Dsp<ntdlib::dsp::WiiMemory> dsp;
        ax::System ax;
        Engine::Snapshot engine;
    };

    // Starts AX and the engine on `archive`. AX boots `microcode` on the DSP before the engine starts. `archive` must
    // outlive the model. Returns false if the microcode didn't start.
    bool Start(const ntdlib::Microcode& microcode, const ntdlib::rsar::SoundArchive& archive);

    // One frame: AX's frame, which the microcode mixes, then the sound frame. Appends the frame's 96 stereo samples to
    // `output`, left then right. Returns false if the microcode stopped responding.
    bool RunFrame(std::vector<int16_t>& output);

    // Starts AX and the engine without the DSP. This follows a sequence that the game's microcode can't be made to mix.
    // Each RunFrameWithoutDsp runs AX's frame and the sound frame. In place of the microcode, it updates the voices'
    // positions. A wave that doesn't loop then ends as it would on the DSP. Nothing is mixed.
    void StartWithoutDsp(const ntdlib::rsar::SoundArchive& archive);
    void RunFrameWithoutDsp();

    // Takes a snapshot between frames, after Start. The snapshot shares its unchanged memory pages with `previous`.
    // Restore throws std::runtime_error for a snapshot that the engine can't return to (see Engine::Restore).
    Snapshot Save(const Snapshot* previous = nullptr) const;
    void Restore(const Snapshot& snapshot);

    ntdlib::dsp::WiiMemory memory;
    ntdlib::dsp::Dsp<ntdlib::dsp::WiiMemory> dsp{memory};
    ax::System ax{memory};
    ax::DspLink link{dsp};
    std::optional<Engine> engine;
    SndBuild build; // the build of NW4R SND that the engine follows, set before Start
};

} // namespace wsf::nw4r
