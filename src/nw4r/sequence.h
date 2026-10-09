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
    // that. It's also the case when the track waits out a held note (see kHeldTicks), or waits for its notes to end
    // while one of them never will (see WaitsOnEndlessNote).
    bool WaitsForEver() const
    {
        return wait_ < 0 || held_wait_ || WaitsOnEndlessNote();
    }

    // Whether the track waits for its notes to end while one of them never will. That note has no length and plays a
    // looping wave. The track continues only once all its notes have ended.
    bool WaitsOnEndlessNote() const;

    // Whether the track's volume or pan is moving to a new value.
    bool IsMoving() const
    {
        return volume_.Moving() || pan_.Moving() || surround_pan_.Moving();
    }

    // Whether the track's last pass of a loop changed nothing in the sound and no change has come since. The ripper's
    // length analysis follows this while the player has on_loop set. A track that polls or counts a variable once a
    // tick makes such idle passes. See CountLoop.
    bool IsIdle() const
    {
        return idle_ && changed_at_ < looped_at_;
    }

    // Whether the track is idle, its last pass left the variables that tracks read as the pass before did, and it has
    // drawn no random number since that pass began. Such a track does the same on every pass until the game changes a
    // variable.
    bool IsStill() const
    {
        return IsIdle() && still_ && drew_at_ < looped_at_;
    }

    // Whether the track's last pass of a loop played no note. A track after track 0 whose passes play no notes doesn't
    // keep a later track from being the main track (see SeqPlayer::IsMainTrack). A still track that plays notes isn't
    // stuck. Once its notes end, its next pass starts a new one.
    bool IsQuiet() const
    {
        return quiet_;
    }

    // Whether a note of the track will end when its length runs out. A held note (see kHeldTicks) doesn't count. The
    // game stops it first.
    bool HasEndingNote() const;

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

    // What a command can change in the track's sound, for the ripper's length analysis: the track's settings, its
    // moving values' places and targets, its newest note, and the player's tempo and volume.
    struct SoundState
    {
        int program;
        LfoParam lfo_param;
        uint8_t lfo_target;
        float sweep_pitch;
        uint8_t volume2;
        int8_t pitch_bend;
        uint8_t bend_range;
        int8_t init_pan;
        int8_t transpose;
        uint8_t priority;
        uint8_t porta_key;
        uint8_t porta_time;
        std::array<uint8_t, 4> envelope;
        int16_t hold;
        uint8_t velocity_range;
        uint8_t biquad_type;
        float biquad_value;
        uint8_t main_send;
        std::array<uint8_t, 3> fx_send;
        uint8_t lpf_freq;
        bool mute;
        bool porta;
        bool damper;
        bool monophonic;
        bool note_wait;
        bool tie;
        std::array<int32_t, 9> moves; // the value, target and ticks to go of the volume, pan and surround pan
        const Channel* newest;
        uint8_t timebase;
        uint16_t tempo;
        uint8_t player_volume;

        bool operator==(const SoundState&) const = default;
    };

    SoundState State() const;

    // Parse, with what the ripper's length analysis follows while the player has on_loop set: each command's place in
    // the order that the player's tracks run them, and whether the command changes the track's sound.
    ParseResult RunCommand(bool do_note_on);

    ParseResult Parse(bool do_note_on);                             // MmlParser::Parse
    void CommandProc(uint32_t command, int32_t arg1, int32_t arg2); // MmlParser::CommandProc
    int32_t ReadArg(ArgType type);                                  // MmlParser::ReadArg
    uint8_t ReadByte();
    uint16_t Read16();
    uint32_t Read24();
    int32_t ReadVar();

    // The argument of a pan, starting pan or surround pan command. The build of October 2006 raises a value of 0 or 1
    // by one (0x800e417c in Smooth Moves' main.dol).
    int32_t PanArgument(int32_t value) const;

    // MmlParser::GetVariablePtr: variables 0-15 belong to the player, 16-31 are global and 32-47 belong to the track.
    int16_t* Variable(int index);

    // NoteOn. Returns whether the note changes the sound, for the ripper's length analysis. A note changes it unless it
    // continues the newest note at the same key and velocity, with no sweep. Only a tie or a monophonic note continues
    // the newest note.
    bool NoteOn(int key, int velocity, int32_t length, bool tie);

    void SetMute(int mute); // SetMute
    void StopAllChannel();
    void AddChannel(Channel* channel);
    void UpdateChannelRelease(Channel* channel);

    // ChannelCallbackFunc: called when a note ends or is dropped.
    void OnChannelEvent(Channel* channel, Channel::CallbackStatus status);

    // Called when the track goes back to `target`, the start of a loop. The pass that ended there began at the track's
    // last run of `target`. A pass that changed nothing in the sound is idle. A pass that only polls or counts a
    // variable is an example. An idle pass is never a loop of the sequence (SeqPlayer::on_loop). Any other pass is one
    // if this is track 0. It's also one if this is the main track (SeqPlayer::IsMainTrack) and it played a note during
    // the pass. A pass that played no note is one too while the track holds a note that sounds until the game stops it
    // (HoldsSustainedNote) and every other open track rests (SeqPlayer::OthersRest). The pass then changes that note,
    // such as its volume or pitch.
    void CountLoop(uint32_t target);

    // The command being run changed the track's sound.
    void MarkChange();

    // The track read variable `index` in a command's argument or in a comparison. IsRead returns whether a track has
    // read variable `index` so far. Any track counts for the player's and the global variables. Only this track counts
    // for the track's variables.
    void MarkRead(int index);
    bool IsRead(int index) const;

    // Whether the track holds a note that sounds until the game stops it on a looping wave, settled at its envelope's
    // sustain level. Such a note is a held note (see kHeldTicks) or a note of length 0.
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

    // What the ripper's length analysis follows while the player has on_loop set. Times are counts of the commands that
    // the player's tracks have run (SeqPlayerState::commands_run_).
    std::vector<uint64_t> run_;                // when the track last ran each command, or 0 if it hasn't
    uint64_t changed_at_ = 0;                  // when the track last changed the sound
    uint64_t drew_at_ = 0;                     // when the track last drew a random number
    uint64_t noted_at_ = 0;                    // when the track last played a note
    uint64_t looped_at_ = 0;                   // when the track last went back to the start of a loop
    uint16_t variables_read_ = 0;              // the track's variables that it has read, a bit each
    std::array<int16_t, 48> loop_variables_{}; // the variables that tracks read, at the last loop (0 for the others)
    bool idle_ = false;                        // whether the track's last pass of a loop changed nothing in the sound
    bool still_ = false; // whether that pass was idle, drew no random number and left the variables as the one before
    bool quiet_ = false; // whether that pass played no note

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
    uint16_t tick_units_ = 416; // the build of October 2006: time in 416ths of a tick (see UpdateTickOctober2006)

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

    // What the ripper's length analysis follows while on_loop is set: the commands that the tracks have run, when a
    // track last changed the sound (SeqPlayer::ChangedAt), and the player's and the global variables that a track has
    // read, a bit each.
    uint64_t commands_run_ = 0;
    uint64_t changed_at_ = 0;
    uint32_t variables_read_ = 0;
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

    // Whether no open track will change the sound while the notes sound, or the tempo is 0. Each open track waits for
    // its notes to end or waits forever. A track may also only make still passes (SeqTrack::IsStill), with no note that
    // will run out of length. The sequence then plays on unchanged while the notes sound. The ripper's length analysis
    // uses this.
    bool IsSettled() const;

    // Whether no open track will ever change the sound again, or the tempo is 0. Each open track waits forever or only
    // makes still passes that play no notes.
    bool IsStuck() const;

    // Whether any open track's volume or pan is moving to a new value.
    bool IsMoving() const;

    // When a track last changed the sound. The time is a count of the commands that the tracks have run while on_loop
    // is set. The ripper's length analysis uses this. A track changes the sound when it plays a note, changes a setting
    // or a moving value, or opens a track. 0 if no track has.
    uint64_t ChangedAt() const
    {
        return changed_at_;
    }

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

    // Called when a pass of track `track` that changed the sound ends with a jump back to `target`, or at the end of a
    // loop that repeats forever and starts at `target`. The jump must go to a command that the track has already run.
    // That's a loop of the sequence, for the ripper's length analysis. The track is track 0, or the main track if it
    // played a note during the pass (see IsMainTrack). A later track also counts if it holds a note that sounds until
    // the game stops it while every other open track rests (see OthersRest). A jump back into code that the track
    // hasn't run isn't a loop. Sequences that share their start make such jumps.
    std::function<void(int track, uint32_t target)> on_loop;

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

    // Whether track `index` is the main track: every track before it is closed or idle (SeqTrack::IsIdle), or comes
    // after track 0 and plays no notes in its passes (SeqTrack::IsQuiet). The main track's loops are loops of the
    // sequence.
    bool IsMainTrack(int index) const;

    // Whether every open track but track `index` is idle (SeqTrack::IsIdle) or waits forever (SeqTrack::WaitsForEver).
    bool OthersRest(int index) const;

    int16_t* Variable(int index); // GetVariablePtr
    int ParseNextTick(bool do_note_on);
    void UpdateTick(int msec);
    void UpdateTickOctober2006();
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
