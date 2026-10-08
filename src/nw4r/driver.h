// SPDX-License-Identifier: MIT

// NW4R SND's driver layer: the AX voices (AxVoice), the voices that group them (Voice), the channels that play notes on
// voices (Channel) and their managers. The model follows Wii Sports' main.dol (see util.h). The game runs these in its
// sound thread and in AX's interrupt, under interrupt locks. The model makes the same calls in the same order within a
// frame. Archive mode plays sequences on the TV only. Each channel therefore has one output (a voice out count of 1) on
// the main line. The Wii Remote mixes and filters stay off.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

#include "ax/pb.h"
#include "ax/voice.h"
#include "common/common_types.h"
#include "nw4r/envelope.h"
#include "nw4r/util.h"
#include "rsar/formats.h"

namespace wsf::nw4r
{

class Engine;

// A wave as NW4R SND plays it (WaveData, from WaveFileReader::ReadWaveParam at main.dol 0x8004eca8). It includes the
// addresses of the samples in main memory.
struct WaveData
{
    struct Channel
    {
        PAddr data = 0;
        rsar::AdpcmInfo adpcm;
    };

    rsar::WaveFormat format = rsar::WaveFormat::kPcm16;
    bool loop = false;
    int channel_count = 1;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0; // in samples
    uint32_t loop_end = 0;   // in samples, past the last one played
    std::array<Channel, 2> channels;
};

// How the channels of a stereo wave pan (PanMode): together, each from its side (dual), or each held to its side
// (balance).
enum class PanMode : uint8_t
{
    kDual = 0,
    kBalance = 1,
};

// One AX voice playing one channel of a wave (AxVoice, with its AxVoiceParamBlock).
class AxVoice
{
public:
    enum class CallbackStatus
    {
        kCancel,
        kDropDsp,
    };

    using Callback = std::function<void(AxVoice* voice, CallbackStatus status)>;

    // The sample rate converter's settings (AxVoice::SrcType). With 4-tap, the polyphase coefficients are chosen
    // automatically for the pitch.
    enum class SrcType
    {
        kNone,
        kLinear,
        k4Tap8k,
        k4Tap12k,
        k4Tap16k,
        k4TapAuto,
    };

    // The mix volumes of AxVoice::MixParam: L, R and S for the main bus and each aux bus.
    struct MixParam
    {
        uint16_t l = 0;
        uint16_t r = 0;
        uint16_t s = 0;
        uint16_t aux_a_l = 0;
        uint16_t aux_a_r = 0;
        uint16_t aux_a_s = 0;
        uint16_t aux_b_l = 0;
        uint16_t aux_b_r = 0;
        uint16_t aux_b_s = 0;
        uint16_t aux_c_l = 0;
        uint16_t aux_c_r = 0;
        uint16_t aux_c_s = 0;

        bool operator==(const MixParam&) const = default;
    };

    explicit AxVoice(Engine& engine) : engine_(&engine)
    {
    }

    // Setup (0x8003265c): sets the wave whose samples this voice plays.
    void Setup(PAddr wave, ax::SampleFormat format, int sample_rate);

    // IsPlayFinished (0x800326dc): whether the voice has played to its end. At the end, the microcode leaves it in the
    // zero buffer.
    bool IsPlayFinished() const;

    // SetAddr (0x800332a0): sets where to start, loop and end, all in samples from `wave`.
    void SetAddr(bool loop, PAddr wave, uint32_t offset, uint32_t loop_start, uint32_t loop_end);

    // SetSrcType (0x80033654).
    void SetSrcType(SrcType type, float pitch);

    // SetAdpcm (0x80033734) and SetAdpcmLoop (0x80033884).
    void SetAdpcm(const rsar::AdpcmInfo& info, uint16_t pred_scale, int16_t yn1, int16_t yn2);
    void SetAdpcmLoop(const rsar::AdpcmInfo& info);

    // SetMix (0x8003395c): ramps the mix from where the last frame left it towards `mix` over the next frame. Returns
    // whether the mix changed. If it did, the voice must ramp again in the next frame.
    bool SetMix(const MixParam& mix);

    // SetSrc (0x80033fd0): sets the pitch as a ratio to the wave's sample rate.
    void SetSrc(float ratio, bool initial);

    // SetVe (0x80034178): sets the volume to reach by the end of the next frame. Also sets the starting volume if the
    // voice hasn't started.
    void SetVe(float volume, float init_volume);

    // SetLpf (0x80034254): sets the low-pass filter's cutoff in Hz. A cutoff of 16 kHz or above turns the filter off.
    void SetLpf(uint16_t freq);

    // SetBiquad (Wii Music, 0x800afcbc): sets the biquad filter of `type` at `value` (see BiquadFilterCoefs). A type
    // without coefficients turns the filter off.
    void SetBiquad(int type, float value);

    void SetPriority(uint32_t priority); // 0x8003307c
    void SetVoiceType(uint16_t type);    // 0x80033090
    void EnableRemote(bool enable);      // 0x800330f0

    // AxVoiceParamBlock::Sync (0x800346b0): sets the volume envelope's ramp for the next frame and passes the frame's
    // changes to AX.
    void Sync();

    void Run();  // AxVoiceParamBlock::SetVoiceStateRun
    void Stop(); // AxVoiceParamBlock::SetVoiceStateStop

    // ResetDelta (0x80033150): ends the ramps of the envelope and the mix where this frame takes them.
    void ResetDelta();

    bool IsAvailable() const
    {
        return vpb_ != nullptr;
    }

    bool IsRun() const
    {
        return IsAvailable() && vpb_->IsRunning();
    }

    // The voice's address in the units of its format, for `samples` from `base` (GetDspAddressBySample), and the
    // reverse (GetSampleByDspAddress, 0x80032fb4).
    static uint32_t DspAddressBySample(PAddr base, uint32_t samples, ax::SampleFormat format);
    static uint32_t SampleByDspAddress(PAddr base, uint32_t address, ax::SampleFormat format);

    // The ADPCM decoder's state after decoding `offset` samples of a channel from its start (CalcOffsetAdpcmParam,
    // 0x80034574).
    static void CalcOffsetAdpcmParam(const Engine& engine, PAddr data, uint32_t offset, const rsar::AdpcmInfo& info,
                                     uint16_t& pred_scale, int16_t& yn1, int16_t& yn2);

private:
    friend class AxVoiceManager;

    // AxVoiceParamBlock's setters that the voice uses.
    void SetVoiceMix(const ax::PbMix& mix, bool sync_now);
    void UpdateDelta();

    Engine* engine_;

    // AxVoiceParamBlock (AxVoice + 0x0).
    ax::Voice* vpb_ = nullptr;              // + 0x0
    uint32_t sync_ = 0;                     // + 0x4: changes to pass to AX at the next Sync
    uint16_t prev_volume_ = ax::kMaxVolume; // + 0x8: the volume envelope at the start of the next frame
    int16_t prev_delta_ = 0;                // + 0xa
    bool first_ve_update_ = false;          // + 0xc
    uint16_t volume_ = ax::kMaxVolume;      // + 0xe: the volume to reach

    PAddr wave_data_ = 0;                                // + 0x10: 0 for none
    ax::SampleFormat format_ = ax::SampleFormat::kPcm16; // + 0x14
    int sample_rate_ = 0;                                // + 0x18
    bool first_mix_update_ = false;                      // + 0x1c
    bool reserve_for_free_ = false;                      // + 0x1d
    MixParam mix_prev_;                                  // + 0x1e
    Callback callback_;                                  // + 0x38
};

// The pool of AxVoices (AxVoiceManager): 96 for AX's voices and 16 more. The extra ones let a voice that AX has dropped
// wait to be freed while its AX voice is reused.
class AxVoiceManager
{
public:
    static constexpr int kVoiceCount = ax::kVoiceCount + 16;

    // The pool's state, for a snapshot of the engine: a copy of each AxVoice, and the lists that hold them.
    struct Snapshot
    {
        std::vector<AxVoice> voices;
        std::list<AxVoice*> free;
        std::list<AxVoice*> active;
        std::list<AxVoice*> reserved;
    };

    explicit AxVoiceManager(Engine& engine);

    // The pool's state between sound frames, and its restoration. A snapshot can only be restored to the pool it came
    // from. Its lists point at that pool's AxVoices.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

    // AcquireAxVoice (0x800353d0): returns an AxVoice with an AX voice at `priority`, or nullptr.
    AxVoice* AcquireAxVoice(uint32_t priority, AxVoice::Callback callback);

    // FreeAxVoice (0x800356a0).
    void FreeAxVoice(AxVoice* voice);

    // ReserveForFreeAxVoice (0x8003576c): AX dropped the voice. Its owner is notified in the next frame.
    void ReserveForFreeAxVoice(AxVoice* voice);

    // FreeAllReservedAxVoice (0x80035800), the first step of the sound frame.
    void FreeAllReservedAxVoice();

private:
    AxVoice* Alloc();
    void Free(AxVoice* voice);

    Engine& engine_;
    std::array<std::unique_ptr<AxVoice>, kVoiceCount> pool_;
    std::list<AxVoice*> free_;
    std::list<AxVoice*> active_;
    std::list<AxVoice*> reserved_;
};

// A voice (Voice): the AX voices that play one note, one for each channel of its wave.
class Voice
{
public:
    enum class Status
    {
        kFinishWave,
        kCancel,
        kDropVoice, // dropped by NW4R SND's voice manager
        kDropDsp,   // dropped by AX
    };

    using Callback = std::function<void(Voice* voice, Status status)>;

    // The voice's changes still to make to its AX voices (Voice::SyncFlag).
    static constexpr uint16_t kSyncSrcInitial = 1u << 0;
    static constexpr uint16_t kSyncVoice = 1u << 1;
    static constexpr uint16_t kSyncSrc = 1u << 2;
    static constexpr uint16_t kSyncVe = 1u << 3;
    static constexpr uint16_t kSyncMix = 1u << 4;
    static constexpr uint16_t kSyncLpf = 1u << 5;
    static constexpr uint16_t kSyncBiquad = 1u << 8; // builds from June 2008

    static constexpr int kPriorityMax = 255;

    explicit Voice(Engine& engine) : engine_(&engine)
    {
    }

    // Acquire (0x8004c14c): takes an AX voice for each of `channels`. Drops lower-priority voices to make room.
    bool Acquire(int channels, int priority, Callback callback);

    // Free (0x8004c388).
    void Free();

    // StopFinished (0x8004bb24), Calc (0x8004bbc0) and Update (0x8004be10): the voice manager's three passes.
    void StopFinished();
    void Calc();
    void Update();

    // Setup (0x8004c454): sets the wave to play, starting `offset` samples in.
    void Setup(const WaveData& data, uint32_t offset);

    void Start(); // 0x8004c704
    void Stop();  // 0x8004c724

    void SetVolume(float volume);                // 0x8004c830
    void SetVeVolume(float target, float init);  // 0x8004c874
    void SetPitch(float pitch);                  // 0x8004c918
    void SetPanMode(PanMode mode);               // 0x8004c938
    void SetPanCurve(uint8_t curve);             // 0x8004c958
    void SetPan(float pan);                      // 0x8004c978
    void SetSurroundPan(float pan);              // 0x8004c998
    void SetLpfFreq(float freq);                 // 0x8004c9b8
    void SetBiquadFilter(int type, float value); // Wii Music, 0x800cc028
    void SetMainOutVolume(float volume);         // 0x8004ca54
    void SetMainSend(float send);                // 0x8004ca98
    void SetFxSend(int bus, float send);         // 0x8004cae0

    // SetPriority (0x8004cc14): re-sorts the voice. A releasing voice (priority 1) gets a lower AX priority.
    void SetPriority(int priority);

    // UpdateVoicesPriority (0x8004ccb4).
    void UpdateVoicesPriority();

    int Priority() const
    {
        return priority_;
    }

    int AxVoiceCount() const
    {
        return channel_count_;
    }

    bool IsActive() const
    {
        return active_;
    }

private:
    friend class VoiceManager;

    bool IsPlayFinished() const;
    void CalcAxSrc(bool initial);
    void CalcAxVe();
    bool CalcAxMix();
    void CalcAxLpf();
    void CalcAxBiquadFilter();
    void CalcMixParam(int channel, AxVoice::MixParam& mix) const;
    void SyncAxVoice();
    void RunAllAxVoice();
    void StopAllAxVoice();
    void ResetDelta();

    // AxVoiceCallbackFunc (0x8004d2a0): called when AX takes one of the voice's AX voices, or when NW4R SND cancels
    // one.
    void OnAxVoiceEvent(AxVoice* dropped, AxVoice::CallbackStatus status);

    Engine* engine_;
    std::array<AxVoice*, 2> ax_voices_{}; // + 0xc, one for each channel
    int channel_count_ = 0;               // + 0x9c
    Callback callback_;                   // + 0xa4
    bool active_ = false;                 // + 0xac
    bool starting_ = false;               // + 0xad
    bool started_ = false;                // + 0xae
    bool pause_ = false;                  // + 0xaf
    bool pausing_ = false;                // + 0xb0
    uint16_t sync_flag_ = 0;              // + 0xb1
    int priority_ = 0;                    // + 0xb4
    float pan_ = 0.0f;                    // + 0xb8
    float surround_pan_ = 0.0f;           // + 0xbc
    float lpf_freq_ = 1.0f;               // + 0xc0
    float main_out_volume_ = 1.0f;        // + 0xc8
    float main_send_ = 1.0f;              // + 0xcc
    std::array<float, 3> fx_send_{};      // + 0xd0
    float pitch_ = 1.0f;                  // + 0x10c
    float volume_ = 1.0f;                 // + 0x110
    float ve_init_volume_ = 0.0f;         // + 0x114
    float ve_target_volume_ = 1.0f;       // + 0x118
    PanMode pan_mode_ = PanMode::kDual;   // + 0x11c
    uint8_t pan_curve_ = 0;               // + 0x120
    uint8_t biquad_type_ = 0;             // builds from June 2008
    float biquad_value_ = 0.0f;           // builds from June 2008
};

// The voices in use, by priority (VoiceManager): 96, one for each AX voice.
class VoiceManager
{
public:
    static constexpr int kVoiceCount = ax::kVoiceCount;

    // The pool's state, for a snapshot of the engine: a copy of each Voice, and the lists that hold them.
    struct Snapshot
    {
        std::vector<Voice> voices;
        std::list<Voice*> free;
        std::list<Voice*> prio_list;
    };

    explicit VoiceManager(Engine& engine);

    // The pool's state between sound frames, and its restoration, as for AxVoiceManager.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

    // AllocVoice (0x8004e358): returns a voice for `channels` at `priority`. If all voices are in use, drops the
    // lowest-priority voice first.
    Voice* AllocVoice(int channels, int priority, Voice::Callback callback);

    // FreeVoice (0x8004e514).
    void FreeVoice(Voice* voice);

    // UpdateAllVoices (0x8004e5ac), the last step of the sound frame.
    void UpdateAllVoices();

    // ChangeVoicePriority (0x8004e6c4).
    void ChangeVoicePriority(Voice* voice);

    // NotifyVoiceUpdate (0x8004e65c): AX's frame callback. It ends each voice's ramps where the frame took them. The
    // sound frame may not set new ramps.
    void NotifyVoiceUpdate();

    // UpdateAllVoicesSync (0x8004e7cc): marks a change for every active voice.
    void UpdateAllVoicesSync(uint8_t sync_flag);

    // The voices in use, lowest priority first and oldest first among equals.
    const std::list<Voice*>& PrioVoices() const
    {
        return prio_list_;
    }

private:
    friend class Voice;

    // DropLowestPriorityVoice (inlined in AllocVoice): drops the lowest-priority voice unless it outranks `priority`.
    // Returns the number of AX voices that the dropped voice had, or 0 if no voice was dropped.
    int DropLowestPriorityVoice(int priority);
    void AppendVoiceList(Voice* voice);
    void RemoveVoiceList(Voice* voice);
    void UpdateEachVoicePriority(std::list<Voice*>::iterator begin);

    std::array<std::unique_ptr<Voice>, kVoiceCount> pool_;
    std::list<Voice*> free_;
    std::list<Voice*> prio_list_;
};

// A channel (Channel): one note of a wave, with its envelope, LFO and sweep, played on a voice.
class Channel
{
public:
    enum class CallbackStatus
    {
        kStopped,
        kDrop,
        kFinish,
        kCancel,
    };

    // The owner's callback. Callbacks are copied before they're called. An owner may free the channel (and clear its
    // callback) from inside one.
    using Callback = std::function<void(Channel* channel, CallbackStatus status)>;

    enum class LfoTarget : uint8_t
    {
        kPitch,
        kVolume,
        kPan,
    };

    explicit Channel(Engine& engine) : engine_(&engine)
    {
    }

    // AllocChannel (0x80038370): returns a channel on a new voice for `channels` channels of a wave, or nullptr.
    static Channel* AllocChannel(Engine& engine, int channels, int priority, Callback callback);

    // FreeChannel (0x8003856c): the channel's owner releases it. The owner gets no more callbacks from it.
    static void FreeChannel(Channel* channel);

    // InitParam (0x800377b0).
    void InitParam(Callback callback);

    // Update (0x800378c8): runs every frame. With `periodic` set, it advances the envelope, LFO and sweep by 3 ms.
    void Update(bool periodic);

    // Start (0x8003801c): plays `data` from `offset` samples in, for `length` ticks of the sequence (or -1).
    void Start(const WaveData& data, int length, uint32_t offset);

    // Release (0x8003809c): starts the release. Lowers the voice's priority unless release_priority_fix (from the
    // archive) is set.
    void Release();

    // NoteOff (Mario Kart Wii, 0x80090600): the release at the end of the note's length. A 2008 build skips it for a
    // note whose region ignores note-offs.
    void NoteOff();

    // Stop (0x80038104).
    void Stop();

    void SetAttack(int attack)
    {
        envelope_.SetAttack(attack);
    }

    void SetDecay(int decay)
    {
        envelope_.SetDecay(decay);
    }

    void SetSustain(int sustain)
    {
        envelope_.SetSustain(sustain);
    }

    void SetRelease(int release)
    {
        envelope_.SetRelease(release);
    }

    void SetHold(int hold)
    {
        envelope_.SetHold(hold);
    }

    // 2008 builds: the region's note-off type (1 ignores note-offs) and its alternate assign (Mario Kart Wii's
    // Bank::NoteOn, 0x8008d490). The alternate assign is a key group. A new note in the group releases the group's
    // other notes.
    void SetIgnoreNoteOff(bool ignore)
    {
        ignore_note_off_ = ignore;
    }

    int KeyGroup() const
    {
        return key_group_;
    }

    void SetKeyGroup(int key_group)
    {
        key_group_ = key_group;
    }

    bool IsRelease() const
    {
        return envelope_.GetStatus() == Envelope::Status::kRelease;
    }

    bool IsSustain() const
    {
        return envelope_.GetStatus() == Envelope::Status::kSustain;
    }

    // Whether the channel's wave loops. A looping wave sounds until the note is released. The ripper's length analysis
    // uses this.
    bool WaveLoops() const
    {
        return wave_loops_;
    }

    void SetLfoParam(const LfoParam& param)
    {
        lfo_.SetParam(param);
    }

    void SetLfoTarget(LfoTarget target)
    {
        lfo_target_ = target;
    }

    void SetReleasePriorityFix(bool fix)
    {
        release_priority_fix_ = fix;
    }

    bool IsActive() const
    {
        return active_;
    }

    bool IsAutoUpdateSweep() const
    {
        return auto_sweep_;
    }

    void SetUserVolume(float volume)
    {
        user_volume_ = volume;
    }

    void SetUserPitch(float pitch)
    {
        user_pitch_ = pitch;
    }

    void SetUserPitchRatio(float ratio)
    {
        user_pitch_ratio_ = ratio;
    }

    void SetUserPan(float pan)
    {
        user_pan_ = pan;
    }

    void SetUserSurroundPan(float pan)
    {
        user_surround_pan_ = pan;
    }

    void SetUserLpfFreq(float freq)
    {
        user_lpf_freq_ = freq;
    }

    // SetBiquadFilter (Wii Music, 0x800b4d30).
    void SetBiquadFilter(int type, float value)
    {
        biquad_type_ = static_cast<uint8_t>(type);
        biquad_value_ = value;
    }

    void SetMainSend(float send)
    {
        main_send_ = send;
    }

    void SetFxSend(int bus, float send)
    {
        fx_send_[bus] = send;
    }

    // UpdateSweep (0x8003820c) and SetSweepParam (0x8003822c).
    void UpdateSweep(int count);
    void SetSweepParam(float pitch, int time, bool auto_update);

    void SetInitVolume(float volume)
    {
        init_volume_ = volume;
    }

    void SetInitPan(float pan)
    {
        init_pan_ = pan;
    }

    void SetInitSurroundPan(float pan)
    {
        init_surround_pan_ = pan;
    }

    void SetTune(float tune)
    {
        tune_ = tune;
    }

    void SetKey(int key)
    {
        key_ = key;
    }

    void SetOriginalKey(int key)
    {
        original_key_ = key;
    }

    int Length() const
    {
        return length_;
    }

    void SetLength(int length)
    {
        length_ = length;
    }

    void SetPanMode(PanMode mode)
    {
        pan_mode_ = mode;
    }

    void SetPanCurve(uint8_t curve)
    {
        pan_curve_ = curve;
    }

    Channel* NextTrackChannel() const
    {
        return next_link_;
    }

    void SetNextTrackChannel(Channel* channel)
    {
        next_link_ = channel;
    }

private:
    friend class ChannelManager;

    // A value that moves to a target over a number of frames (MoveValue<u8, u16>), for the silence fade.
    struct SilenceVolume
    {
        uint8_t origin = 255; // + 0x0
        uint8_t target = 255; // + 0x1
        uint16_t frame = 0;   // + 0x2
        uint16_t counter = 0; // + 0x4

        uint8_t Value() const;
    };

    float SweepValue() const;

    // VoiceCallbackFunc (0x80038244).
    void OnVoiceEvent(Voice* voice, Voice::Status status);

    Engine* engine_;
    Envelope envelope_;                        // + 0x0
    Lfo lfo_;                                  // + 0x18
    LfoTarget lfo_target_ = LfoTarget::kPitch; // + 0x30
    bool pause_ = false;                       // + 0x31
    bool active_ = false;                      // + 0x32
    bool alloc_ = false;                       // + 0x33
    bool auto_sweep_ = true;                   // + 0x34
    bool release_priority_fix_ = false;        // + 0x35
    float user_volume_ = 1.0f;                 // + 0x38
    float user_pitch_ratio_ = 1.0f;            // + 0x3c
    float user_pan_ = 0.0f;                    // + 0x40
    float user_surround_pan_ = 0.0f;           // + 0x44
    float user_lpf_freq_ = 0.0f;               // + 0x48
    float main_out_volume_ = 1.0f;             // + 0x54
    float main_send_ = 0.0f;                   // + 0x58
    std::array<float, 3> fx_send_{};           // + 0x5c
    float user_pitch_ = 0.0f;                  // + 0x98
    float sweep_pitch_ = 0.0f;                 // + 0x9c
    int sweep_counter_ = 0;                    // + 0xa0
    int sweep_length_ = 0;                     // + 0xa4
    float init_volume_ = 1.0f;                 // + 0xa8
    float init_pan_ = 0.0f;                    // + 0xac
    float init_surround_pan_ = 0.0f;           // + 0xb0
    float tune_ = 1.0f;                        // + 0xb4
    SilenceVolume silence_volume_;             // + 0xb8
    int key_ = 60;                             // + 0xc0
    int original_key_ = 60;                    // + 0xc4
    int length_ = 0;                           // + 0xc8
    PanMode pan_mode_ = PanMode::kDual;        // + 0xcc
    uint8_t pan_curve_ = 0;                    // + 0xd0
    Callback callback_;                        // + 0xd4
    Voice* voice_ = nullptr;                   // + 0xdc
    Channel* next_link_ = nullptr;             // + 0xe0
    bool ignore_note_off_ = false;             // 2008 builds
    int key_group_ = 0;                        // 2008 builds
    uint8_t biquad_type_ = 0;                  // builds from June 2008
    float biquad_value_ = 0.0f;                // builds from June 2008
    bool wave_loops_ = false;                  // the wave's loop flag
};

// The pool of channels and the channels in use (ChannelManager): 97, one for each AX voice and one more.
class ChannelManager
{
public:
    static constexpr int kChannelCount = ax::kVoiceCount + 1;

    // The pool's state, for a snapshot of the engine: a copy of each Channel, and the lists that hold them.
    struct Snapshot
    {
        std::vector<Channel> channels;
        std::list<Channel*> free;
        std::list<Channel*> active;
    };

    explicit ChannelManager(Engine& engine);

    // The pool's state between sound frames, and its restoration, as for AxVoiceManager.
    Snapshot Save() const;
    void Restore(const Snapshot& snapshot);

    // Alloc and Free (inlined in AllocChannel and elsewhere). The pool is a stack. The channels in use form a list in
    // the order they were taken.
    Channel* Alloc();
    void Free(Channel* channel);

    // UpdateAllChannel (0x8003775c).
    void UpdateAllChannel();

    // The number of channels in use.
    int ActiveCount() const
    {
        return static_cast<int>(channels_.size());
    }

    // Whether any channel in use is still changing. A channel is changing while its envelope is in its attack, hold,
    // decay or release. After the release, the channel ends by itself.
    bool AnyChanging() const;

private:
    std::array<std::unique_ptr<Channel>, kChannelCount> pool_;
    std::list<Channel*> free_;
    std::list<Channel*> channels_;
};

} // namespace wsf::nw4r
