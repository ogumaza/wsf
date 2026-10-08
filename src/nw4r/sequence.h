// SPDX-License-Identifier: MIT

// NW4R SND's sequence playback: SeqTrack with its MML parser (MmlSeqTrack, MmlParser) and SeqPlayer. Modelled on Wii
// Sports' main.dol (see util.h). A field comment gives the field's offset in the game's object. TP is the track's
// parser parameters, at track + 0x50.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "nw4r/driver.h"
#include "nw4r/envelope.h"
#include "rsar/formats.h"

namespace wsf::nw4r
{

class Engine;
class SeqPlayer;
struct LoadedBank;

// MoveValue: a track setting that moves linearly from its current value to a target over a number of ticks. The volume
// and the pans move this way in builds from 2008. In Mario Kart Wii, MmlParser::CommandProc (0x800968d0) sets the
// moves. SeqTrack::ParseNextTick (0x8009b280) counts the ticks. Older builds set a value at once. The model treats that
// as a move over no ticks.
template <typename T>
struct MoveValue
{
    T Get() const
    {
        if (counter >= duration)
        {
            return target;
        }

        return static_cast<T>(start + (target - start) * counter / duration);
    }

    void SetTarget(T value, int16_t ticks)
    {
        start = Get();
        target = value;
        duration = ticks;
        counter = 0;
    }

    void Update()
    {
        if (counter < duration)
        {
            counter++;
        }
    }

    bool Moving() const
    {
        return counter < duration;
    }

    T start{};
    T target{};
    int16_t duration = 0;
    int16_t counter = 0;
};

// A note's request to the bank (NoteOnInfo).
struct NoteOnInfo
{
    int program = 0;
    int key = 60;
    int velocity = 127;
    int length = -1; // in ticks, or -1 for a note that lasts until it's released
    int init_pan = 0;
    int priority = 64;
    Channel::Callback callback;
};

class SeqTrack
{
public:
    SeqTrack(SeqPlayer& player, int index);

    // Removes the callbacks of the track's channels before the track is destroyed. The callbacks then can't call into
    // it.
    void DetachChannels();

    void SetSeqData(std::span<const uint8_t> data, uint32_t offset); // SetSeqData
    void Open();                                                     // Open

    // Close: releases the track's notes and frees them.
    void Close();

    bool IsOpen() const
    {
        return open_;
    }

    // Whether the track won't continue while its notes sound. That's the case when it waits for its notes to end (after
    // a note of length 0 with note wait on) and some still sound. It's also the case when it waits forever. A track
    // whose notes have all ended continues at its next tick.
    bool WaitsForNotes() const
    {
        return (note_finish_wait_ && channels_ != nullptr) || WaitsForEver();
    }

    // Whether the track will never continue, or not before the game stops the sound. That's the case when it waits for
    // a negative number of ticks. A note or wait whose length comes from a variable left at its default of -1 does
    // that. It's also the case when the track waits out a held note (see kHeldTicks).
    bool WaitsForEver() const
    {
        return wait_ < 0 || held_wait_;
    }

    // Whether the track's volume or pan is moving to a new value.
    bool IsMoving() const
    {
        return volume_.Moving() || pan_.Moving() || surround_pan_.Moving();
    }

    // ParseNextTick: runs the track's commands up to its next wait. Returns -1 when the track ends, 0 for a closed
    // track and 1 otherwise.
    int ParseNextTick(bool do_note_on);

    // UpdateChannelLength: counts the notes' lengths down by a tick. Releases the notes that run out.
    void UpdateChannelLength();

    // UpdateChannelParam (0x80046d0c): passes the track's volume, pitch, pan and sends to its notes.
    void UpdateChannelParam();

    // ReleaseAllChannel and FreeAllChannel.
    void ReleaseAllChannel(int release);
    void FreeAllChannel();

private:
    enum class ArgType
    {
        kU8,
        kS16,
        kVmidi,
        kRandom,
        kVariable,
    };

    enum class ParseResult
    {
        kContinue,
        kFinish,
    };

    ParseResult Parse(bool do_note_on);                             // MmlParser::Parse
    void CommandProc(uint32_t command, int32_t arg1, int32_t arg2); // MmlParser::CommandProc
    int32_t ReadArg(ArgType type);                                  // MmlParser::ReadArg
    uint8_t ReadByte();
    uint16_t Read16();
    uint32_t Read24();
    int32_t ReadVar();

    // MmlParser::GetVariablePtr: variables 0-15 belong to the player, 16-31 are global and 32-47 belong to the track.
    int16_t* Variable(int index);

    Channel* NoteOn(int key, int velocity, int32_t length, bool tie); // NoteOn
    void SetMute(int mute);                                           // SetMute
    void StopAllChannel();
    void AddChannel(Channel* channel);
    void UpdateChannelRelease(Channel* channel);

    // ChannelCallbackFunc: called when a note ends or is dropped.
    void OnChannelEvent(Channel* channel, Channel::CallbackStatus status);

    // Called when the track goes back to the start of a loop. That counts as a loop of the sequence
    // (SeqPlayer::on_loop) if this is track 0. Once track 0 has ended, it counts if this is the lowest-numbered open
    // track and it played a note during the loop. A loop that only sets values or waits while other tracks play isn't
    // part of the music.
    void CountLoop();

    // Whether the track holds a held note (see kHeldTicks) on a looping wave, settled at its envelope's sustain level.
    bool HoldsSustainedNote() const;

    SeqPlayer* player_; // a pointer so that a track can be copied
    int index_ = 0;     // + 0x4
    bool open_ = false; // + 0x5

    // ParserTrackParam.
    std::span<const uint8_t> data_;
    uint32_t current_ = 0;                // TP + 0x4, as an offset into data_
    int32_t wait_ = 0;                    // TP + 0x8
    bool mute_ = false;                   // TP + 0xc
    bool note_finish_wait_ = false;       // TP + 0xe
    bool porta_ = false;                  // TP + 0xf
    bool damper_ = false;                 // TP + 0x10
    int program_ = 0;                     // TP + 0x18
    LfoParam lfo_param_;                  // TP + 0x1c
    uint8_t lfo_target_ = 0;              // TP + 0x2c
    float sweep_pitch_ = 0.0f;            // TP + 0x30
    MoveValue<uint8_t> volume_{127, 127}; // TP + 0x34
    uint8_t volume2_ = 127;               // TP + 0x35
    int8_t pitch_bend_ = 0;               // TP + 0x36
    uint8_t bend_range_ = 2;              // TP + 0x37
    MoveValue<int8_t> pan_;               // TP + 0x38
    int8_t init_pan_ = 0;                 // TP + 0x39
    MoveValue<int8_t> surround_pan_;      // TP + 0x3a
    int8_t transpose_ = 0;                // TP + 0x3b
    uint8_t priority_ = 64;               // TP + 0x3c
    uint8_t porta_key_ = 60;              // TP + 0x3d
    uint8_t porta_time_ = 0;              // TP + 0x3e
    uint8_t attack_ = 0xff;               // TP + 0x3f
    uint8_t decay_ = 0xff;                // TP + 0x40
    uint8_t sustain_ = 0xff;              // TP + 0x41
    uint8_t release_ = 0xff;              // TP + 0x42
    int16_t hold_ = 0xff;                 // 2008 builds: the env_hold command's override of the notes' hold
    bool monophonic_ = false;             // 2008 builds: each note takes over the last one's channel
    uint8_t velocity_range_ = 127;        // builds from June 2008: the notes' velocities are scaled by this over 127
    uint8_t biquad_type_ = 0;             // builds from June 2008: the notes' biquad filter (see BiquadFilterCoefs)
    float biquad_value_ = 0.0f;           // builds from June 2008
    uint8_t main_send_ = 127;             // TP + 0x43
    std::array<uint8_t, 3> fx_send_{};    // TP + 0x44
    uint8_t lpf_freq_ = 64;               // TP + 0x47

    std::vector<bool> run_;             // the commands that the track has run, while the player has on_loop set
    bool noted_ = false;                // whether the track has played a note since its last loop
    std::array<bool, 3> called_{};      // which entries of call_stack_ came from a call rather than a loop_start
    bool conditional_ = false;          // whether the command being run is conditional
    bool holds_ = false;                // whether the track's last note is a held one (see kHeldTicks)
    bool held_wait_ = false;            // whether the track waits out a held note
    std::array<int16_t, 16> variables_; // + 0x98
    Channel* channels_ = nullptr;       // + 0xbc, newest first

    // MmlParserParam (MmlSeqTrack + 0xc0).
    bool cmp_flag_ = true;
    bool note_wait_ = true;
    bool tie_ = false;
    std::array<uint8_t, 3> loop_count_{};
    uint8_t call_depth_ = 0;
    std::array<uint32_t, 3> call_stack_{};
};

// A SeqPlayer's state between sound frames. A snapshot of the engine copies it (see SeqPlayer::Save). The tracks are
// held in place. Each track stays at one address for the player's lifetime. The callbacks of its channels point at it,
// in a snapshot as in the player.
class SeqPlayerState
{
protected:
    std::array<std::optional<SeqTrack>, 16> tracks_;
    std::array<int16_t, 16> local_variables_;
    bool active_ = false;
    bool started_ = false;
    float tick_fraction_ = 0.0f;
    uint32_t tick_counter_ = 0;

    // ParserPlayerParam.
    uint8_t volume_ = 127;
    uint8_t priority_ = 64;
    uint8_t timebase_ = 48;
    uint16_t tempo_ = 120;

    // The sound's settings: its volume (from BasicPlayer, set by BasicSound::Update) and the rest from the archive.
    float player_volume_ = 1.0f;
    bool release_priority_fix_ = false;
    PanMode pan_mode_ = PanMode::kDual;
    uint8_t pan_curve_ = 0;
};

// A sequence sound's player (SeqPlayer), with the settings the sound archive gives it (SoundArchivePlayer's
// PrepareSeqImpl and BasicSound::Update).
class SeqPlayer : private SeqPlayerState
{
public:
    // The player's state as Save captures it.
    using Snapshot = SeqPlayerState;

    // A player for `sound` that plays `sequence` with `bank`. All three must outlive the player.
    SeqPlayer(Engine& engine, const rsar::SoundInfo& sound, const rsar::Sequence& sequence, const LoadedBank* bank);

    SeqPlayer(const SeqPlayer&) = delete;
    SeqPlayer& operator=(const SeqPlayer&) = delete;
    ~SeqPlayer();

    // Setup, SetSeqData and Start: allocates the tracks that the sound requests and starts the first at the sound's
    // offset.
    void Start();

    // Update (0x80045d50), called from the sound thread every frame. Runs the ticks that fall within the frame's 3 ms,
    // then updates the notes' parameters.
    void Update();

    bool IsActive() const
    {
        return active_;
    }

    // Whether any note of the sound is still playing.
    bool HasChannels() const;

    // Whether no open track will continue while the notes sound, or the tempo is 0. The sequence then plays on
    // unchanged while the notes sound. The ripper's length analysis uses this.
    bool WaitsForNotes() const;

    // Whether no track will ever continue: every open track waits forever, or the tempo is 0.
    bool WaitsForEver() const;

    // Whether any open track's volume or pan is moving to a new value.
    bool IsMoving() const;

    // The player's state between sound frames, and its restoration. A snapshot can only be restored to the player it
    // came from.
    Snapshot Save() const
    {
        return *this;
    }

    void Restore(const Snapshot& snapshot)
    {
        SeqPlayerState::operator=(snapshot);
    }

    // Called when the main track jumps back to a command it has already run, or to the start of a loop that repeats
    // forever. That's a loop of the sequence, for the ripper's length analysis. The main track is track 0. Once track 0
    // has ended, the lowest-numbered open track is the main track. Its loops count only when it played a note during
    // the loop. A jump back into code that the track hasn't run isn't a loop. Sequences that share their start make
    // such jumps.
    std::function<void()> on_loop;

private:
    friend class SeqTrack;

    Engine& GetEngine()
    {
        return *engine_;
    }

    SeqTrack* Track(int index)
    {
        return index >= 0 && index < 16 && tracks_[index] ? &*tracks_[index] : nullptr;
    }

    // The tempo in ticks per millisecond (GetBaseTempo).
    float BaseTempo() const;

    // Whether track `index` is the main track: no track before it is open. The main track's loops are loops of the
    // sequence.
    bool IsMainTrack(int index) const;

    int16_t* Variable(int index); // GetVariablePtr
    int ParseNextTick(bool do_note_on);
    void UpdateTick(int msec);
    void CloseTrack(int index);
    void FinishPlayer();
    Channel* NoteOn(const NoteOnInfo& info);

    // Whether there's an open track and `test` holds for every open track.
    template <typename Test>
    bool EveryOpenTrack(Test test) const
    {
        bool open = false;
        for (const std::optional<SeqTrack>& track : tracks_)
        {
            if (!track || !track->IsOpen())
            {
                continue;
            }

            if (!test(*track))
            {
                return false;
            }

            open = true;
        }

        return open;
    }

    Engine* engine_;
    const rsar::SoundInfo* sound_;
    const rsar::Sequence* sequence_;
    const LoadedBank* bank_;
};

} // namespace wsf::nw4r
