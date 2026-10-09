// SPDX-License-Identifier: MIT

#include "nw4r/sequence.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "nw4r/engine.h"
#include "nw4r/util.h"

namespace wsf::nw4r
{
namespace
{

// The game parses a track's commands until one waits. It hangs on a sequence that loops without waiting. The model ends
// such a track instead.
constexpr int kParseLimit = 100000;

// The variables' value before anything sets them (DEFAULT_VARIABLE_VALUE).
constexpr int16_t kVariableDefault = -1;

// The minimum length of a held note, for the ripper's length analysis: 200 beats at the default timebase, or 100 s at
// 120 BPM. A sound that plays until the game stops it gives its notes such lengths so that the game stops them first.
// Music's notes are far shorter.
constexpr int32_t kHeldTicks = 9600;

// The commands (MmlParser's MML_*).
enum Command : uint32_t
{
    kWait = 0x80,
    kSetProgram = 0x81,
    kOpenTrack = 0x88,
    kJump = 0x89,
    kCall = 0x8a,
    kRandomParam = 0xa0,
    kVariableParam = 0xa1,
    kExecIf = 0xa2,
    kTimeParam = 0xa3, // 2008 builds' time prefixes
    kTimeRandomParam = 0xa4,
    kTimeVariableParam = 0xa5,
    kSetTimebase = 0xb0,
    kSetHold = 0xb1,          // 2008 builds
    kSetMonophonic = 0xb2,    // 2008 builds
    kSetVelocityRange = 0xb3, // builds from June 2008
    kSetBiquadType = 0xb4,    // builds from June 2008
    kSetBiquadValue = 0xb5,   // builds from June 2008
    kSetPan = 0xc0,
    kSetTrackVolume = 0xc1,
    kSetPlayerVolume = 0xc2,
    kSetTranspose = 0xc3,
    kSetPitchBend = 0xc4,
    kSetBendRange = 0xc5,
    kSetPriority = 0xc6,
    kSetNoteWait = 0xc7,
    kSetTie = 0xc8,
    kSetPortamento = 0xc9,
    kSetLfoDepth = 0xca,
    kSetLfoSpeed = 0xcb,
    kSetLfoTarget = 0xcc,
    kSetLfoRange = 0xcd,
    kSetPortaSpeed = 0xce,
    kSetPortaTime = 0xcf,
    kSetAttack = 0xd0,
    kSetDecay = 0xd1,
    kSetSustain = 0xd2,
    kSetRelease = 0xd3,
    kLoopStart = 0xd4,
    kSetTrackVolume2 = 0xd5,
    kPrintVar = 0xd6,
    kSetSurroundPan = 0xd7,
    kSetLpfFreq = 0xd8,
    kSetFxSendA = 0xd9,
    kSetFxSendB = 0xda,
    kSetMainSend = 0xdb,
    kSetInitPan = 0xdc,
    kSetMute = 0xdd,
    kSetFxSendC = 0xde,
    kSetDamper = 0xdf,
    kSetLfoDelay = 0xe0,
    kSetTempo = 0xe1,
    kSetSweepPitch = 0xe3,
    kExCommand = 0xf0,
    kEnvReset = 0xfb, // builds from June 2008
    kLoopEnd = 0xfc,
    kReturn = 0xfd,
    kAllocTrack = 0xfe,
    kEnd = 0xff,
};

// The extended commands (0xf0 and a second byte): arithmetic and comparisons on a variable, and a user procedure.
enum ExCommand : uint32_t
{
    kExArithmetic = 0x80,
    kExSet = 0x80,
    kExAdd = 0x81,
    kExSubtract = 0x82,
    kExMultiply = 0x83,
    kExDivide = 0x84,
    kExShift = 0x85,
    kExRandom = 0x86,
    kExAnd = 0x87,
    kExOr = 0x88,
    kExXor = 0x89,
    kExNot = 0x8a,
    kExModulo = 0x8b,
    kExLogic = 0x90,
    kExEqual = 0x90,
    kExGreaterEqual = 0x91,
    kExGreater = 0x92,
    kExLessEqual = 0x93,
    kExLess = 0x94,
    kExNotEqual = 0x95,
    kExUserProc = 0xe0,
};

// `a * b` with the wraparound of the game's 32-bit multiply.
int32_t WrapMultiply(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

// A shift like PowerPC's slw and sraw. Amounts from 32 to 63 shift everything out.
int32_t ShiftLeft(int32_t value, int32_t amount)
{
    return (amount & 0x3f) >= 32 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(value) << (amount & 0x1f));
}

int32_t ShiftRight(int32_t value, int32_t amount)
{
    return (amount & 0x3f) >= 32 ? (value < 0 ? -1 : 0) : value >> (amount & 0x1f);
}

} // namespace

SeqTrack::SeqTrack(SeqPlayer& player, int index) : player_(&player), index_(index)
{
    variables_.fill(kVariableDefault);
}

void SeqTrack::DetachChannels()
{
    for (Channel* channel = channels_; channel != nullptr; channel = channel->NextTrackChannel())
    {
        Channel::FreeChannel(channel);
    }
}

void SeqTrack::SetSeqData(std::span<const uint8_t> data, uint32_t offset)
{
    data_ = data;
    current_ = offset;
}

void SeqTrack::Open()
{
    open_ = true;
}

void SeqTrack::Close()
{
    ReleaseAllChannel(-1);
    FreeAllChannel();
    open_ = false;
}

int SeqTrack::ParseNextTick(bool do_note_on)
{
    if (!open_)
    {
        return 0;
    }

    // In a 2008 build, the moving settings advance by a tick before the track continues. In an older build they don't
    // move.
    volume_.Update();
    pan_.Update();
    surround_pan_.Update();

    // A track waiting for its notes to end keeps waiting while any note plays.
    if (note_finish_wait_)
    {
        if (channels_ != nullptr)
        {
            return 1;
        }

        note_finish_wait_ = false;
    }

    if (wait_ > 0 && --wait_ > 0)
    {
        return 1;
    }

    for (int count = 0; wait_ == 0 && !note_finish_wait_; count++)
    {
        if (count == kParseLimit || RunCommand(do_note_on) == ParseResult::kFinish)
        {
            return -1;
        }
    }

    return 1;
}

void SeqTrack::UpdateChannelLength()
{
    if (!open_)
    {
        return;
    }

    for (Channel* channel = channels_; channel != nullptr; channel = channel->NextTrackChannel())
    {
        if (channel->Length() > 0)
        {
            channel->SetLength(channel->Length() - 1);
        }

        UpdateChannelRelease(channel);
        if (!channel->IsAutoUpdateSweep())
        {
            channel->UpdateSweep(1);
        }
    }
}

void SeqTrack::UpdateChannelParam()
{
    if (!open_ || channels_ == nullptr)
    {
        return;
    }

    // Like the velocity, the volumes are squared. The pan ranges of the player and the track are 1. The game's other
    // settings (pitch, pan, filter and sends of the track and the player) are 0 or 1. The model leaves them out. It
    // also leaves out the player's biquad filter (from June 2008). That filter replaces the tracks' filters when the
    // game gives a sound one.
    const float track_volume = static_cast<float>(volume_.Get()) / 127.0f;
    const float track_volume2 = static_cast<float>(volume2_) / 127.0f;
    const float player_volume = static_cast<float>(player_->volume_) / 127.0f;
    float volume = 1.0f;
    volume *= track_volume * track_volume;
    volume *= track_volume2 * track_volume2;
    volume *= player_volume * player_volume;
    volume *= player_->player_volume_;

    const float pitch = static_cast<float>(pitch_bend_) / 128.0f * static_cast<float>(bend_range_);
    float pan = 0.0f;
    pan += std::clamp(static_cast<float>(pan_.Get()) / 63.0f, -1.0f, 1.0f);
    float surround_pan = 0.0f;
    surround_pan += std::clamp(static_cast<float>(surround_pan_.Get()) / 63.0f, 0.0f, 2.0f);
    float lpf_freq = 0.0f;
    lpf_freq += static_cast<float>(lpf_freq_ - 64) / 64.0f;
    float main_send = 0.0f;
    main_send += static_cast<float>(main_send_) / 127.0f - 1.0f;
    std::array<float, 3> fx_send{};
    for (int i = 0; i < 3; i++)
    {
        fx_send[i] += static_cast<float>(fx_send_[i]) / 127.0f;
    }

    for (Channel* channel = channels_; channel != nullptr; channel = channel->NextTrackChannel())
    {
        channel->SetUserVolume(volume);
        channel->SetUserPitch(pitch);
        channel->SetUserPitchRatio(1.0f);
        channel->SetUserPan(pan);
        channel->SetUserSurroundPan(surround_pan);
        channel->SetUserLpfFreq(lpf_freq);
        channel->SetBiquadFilter(biquad_type_, biquad_value_);
        channel->SetMainSend(main_send);
        for (int i = 0; i < 3; i++)
        {
            channel->SetFxSend(i, fx_send[i]);
        }

        channel->SetLfoParam(lfo_param_);
        channel->SetLfoTarget(static_cast<Channel::LfoTarget>(lfo_target_));
    }
}

void SeqTrack::ReleaseAllChannel(int release)
{
    UpdateChannelParam();
    for (Channel* channel = channels_; channel != nullptr; channel = channel->NextTrackChannel())
    {
        if (channel->IsActive())
        {
            if (release >= 0)
            {
                channel->SetRelease(static_cast<uint8_t>(release));
            }

            channel->Release();
        }
    }
}

void SeqTrack::FreeAllChannel()
{
    for (Channel* channel = channels_; channel != nullptr; channel = channel->NextTrackChannel())
    {
        Channel::FreeChannel(channel);
    }

    channels_ = nullptr;
}

SeqTrack::SoundState SeqTrack::State() const
{
    SoundState state{};
    state.program = program_;
    state.lfo_param = lfo_param_;
    state.lfo_target = lfo_target_;
    state.sweep_pitch = sweep_pitch_;
    state.volume2 = volume2_;
    state.pitch_bend = pitch_bend_;
    state.bend_range = bend_range_;
    state.init_pan = init_pan_;
    state.transpose = transpose_;
    state.priority = priority_;
    state.porta_key = porta_key_;
    state.porta_time = porta_time_;
    state.envelope = {attack_, decay_, sustain_, release_};
    state.hold = hold_;
    state.velocity_range = velocity_range_;
    state.biquad_type = biquad_type_;
    state.biquad_value = biquad_value_;
    state.main_send = main_send_;
    state.fx_send = fx_send_;
    state.lpf_freq = lpf_freq_;
    state.mute = mute_;
    state.porta = porta_;
    state.damper = damper_;
    state.monophonic = monophonic_;
    state.note_wait = note_wait_;
    state.tie = tie_;

    std::size_t at = 0;
    const auto move = [&state, &at](const auto& value)
    {
        state.moves[at++] = value.Get();
        state.moves[at++] = value.target;
        state.moves[at++] = value.duration - value.counter;
    };

    move(volume_);
    move(pan_);
    move(surround_pan_);

    state.newest = channels_;
    state.timebase = player_->timebase_;
    state.tempo = player_->tempo_;
    state.player_volume = player_->volume_;

    return state;
}

SeqTrack::ParseResult SeqTrack::RunCommand(bool do_note_on)
{
    if (!player_->on_loop)
    {
        return Parse(do_note_on);
    }

    const uint64_t now = ++player_->commands_run_;
    run_.resize(data_.size() + 1);
    run_[std::min<std::size_t>(current_, data_.size())] = now;
    const SoundState before = State();
    const ParseResult result = Parse(do_note_on);
    if (State() != before)
    {
        MarkChange();
    }

    return result;
}

SeqTrack::ParseResult SeqTrack::Parse(bool do_note_on)
{
    // MmlParser::Parse (0x8003b8b8). A command may be conditional on the last comparison. Its argument may come from a
    // random range or a variable instead of the command. Builds from 2008 also take a time for a command of 0xb0-0xdf,
    // after its argument (Mario Kart Wii, 0x80096240). The time comes from the command, a random range or a variable.
    const bool build_2008 = player_->GetEngine().Build().Since2008();
    bool exec = true;
    bool use_arg_type = false;
    ArgType arg_type = ArgType::kU8;
    std::optional<ArgType> time_type;
    held_wait_ = false;
    uint32_t command = ReadByte();
    conditional_ = command == kExecIf;
    if (conditional_)
    {
        command = ReadByte();
        exec = cmp_flag_;
    }

    if (build_2008 && command >= kTimeParam && command <= kTimeVariableParam)
    {
        static constexpr ArgType kTimeTypes[] = {ArgType::kS16, ArgType::kRandom, ArgType::kVariable};
        time_type = kTimeTypes[command - kTimeParam];
        command = ReadByte();
    }

    if (command == kRandomParam)
    {
        command = ReadByte();
        arg_type = ArgType::kRandom;
        use_arg_type = true;
    }

    if (command == kVariableParam)
    {
        command = ReadByte();
        arg_type = ArgType::kVariable;
        use_arg_type = true;
    }

    const auto arg = [&](ArgType type)
    {
        return ReadArg(use_arg_type ? arg_type : type);
    };

    // A note: its key, velocity and length.
    if (!(command & 0x80))
    {
        const uint8_t velocity = ReadByte();
        const int32_t length = arg(ArgType::kVmidi);
        int key = static_cast<int>(command) + transpose_;
        if (!exec)
        {
            return ParseResult::kContinue;
        }

        key = std::clamp(key, 0, 127);
        noted_at_ = player_->commands_run_;
        holds_ = length >= kHeldTicks;
        if (!mute_ && do_note_on && NoteOn(key, velocity, length > 0 ? length : -1, tie_))
        {
            MarkChange();
        }

        if (note_wait_)
        {
            wait_ = length;
            held_wait_ = holds_;
            if (length == 0)
            {
                note_finish_wait_ = true;
            }
        }

        return ParseResult::kContinue;
    }

    int32_t arg1 = 0;
    int32_t arg2 = 0;
    switch (command & 0xf0)
    {
    case 0x80:
        switch (command)
        {
        case kWait:
            {
                const int32_t time = arg(ArgType::kVmidi);
                if (exec)
                {
                    wait_ = time;
                }
                break;
            }

        case kSetProgram:
            arg1 = arg(ArgType::kVmidi);
            if (exec)
            {
                CommandProc(command, arg1, arg2);
            }
            break;

        case kOpenTrack:
            {
                const uint8_t track = ReadByte();
                const uint32_t offset = Read24();
                if (exec)
                {
                    CommandProc(command, track, static_cast<int32_t>(offset));
                }
                break;
            }

        case kJump:
        case kCall:
            {
                const uint32_t offset = Read24();
                if (exec)
                {
                    CommandProc(command, static_cast<int32_t>(offset), arg2);
                }
                break;
            }

        default:
            break;
        }
        break;

    case 0xb0:
    case 0xc0:
    case 0xd0:
        {
            // A byte, read as a byte whatever the argument's source. It's signed for transpose and pitch bend. A prefix
            // can give a time for a volume or a pan to move over. Without a prefix, the time is 0: the value changes at
            // once.
            const auto value = static_cast<uint8_t>(arg(ArgType::kU8));
            if (time_type)
            {
                arg2 = ReadArg(*time_type);
            }

            if (!exec)
            {
                break;
            }

            arg1 = command == kSetTranspose || command == kSetPitchBend ? static_cast<int8_t>(value) : value;
            CommandProc(command, arg1, arg2);
            break;
        }

    case 0x90:
        if (exec)
        {
            CommandProc(command, arg1, arg2);
        }
        break;

    case 0xe0:
        arg1 = static_cast<int16_t>(arg(ArgType::kS16));
        if (exec)
        {
            CommandProc(command, arg1, arg2);
        }
        break;

    case 0xf0:
        switch (command)
        {
        case kAllocTrack:
            Read16();
            break;

        case kEnd:
            return ParseResult::kFinish;

        case kExCommand:
            {
                // An extended command. The game then falls through to run the 0xf0 command itself. That command does
                // nothing.
                const uint32_t ex = ReadByte();
                switch (ex & 0xf0)
                {
                case kExUserProc:
                    arg1 = static_cast<int16_t>(arg(ArgType::kS16));
                    if (exec)
                    {
                        CommandProc((command << 8) + ex, arg1, arg2);
                    }
                    break;

                case kExArithmetic:
                case kExLogic:
                    arg1 = ReadByte();
                    arg2 = static_cast<int16_t>(arg(ArgType::kS16));
                    if (exec)
                    {
                        CommandProc((command << 8) + ex, arg1, arg2);
                    }
                    break;

                default:
                    break;
                }
                break;
            }

        default:
            if (exec)
            {
                CommandProc(command, arg1, arg2);
            }
            break;
        }
        break;

    default:
        break;
    }

    return ParseResult::kContinue;
}

int32_t SeqTrack::PanArgument(int32_t value) const
{
    return player_->engine_->Build().October2006() && value <= 1 ? value + 1 : value;
}

void SeqTrack::CommandProc(uint32_t command, int32_t arg1, int32_t arg2)
{
    // MmlParser::CommandProc (0x8003bee8).
    SeqPlayer& player = *player_;
    if (command <= 0xff)
    {
        switch (command)
        {
        case kSetTempo:
            // A 2008 build keeps the tempo within 0-1023 (Mario Kart Wii, 0x80096924). A variable's default of -1 then
            // stops the sequence instead of racing it.
            player.tempo_ =
                static_cast<uint16_t>(player.GetEngine().Build().Since2008() ? std::clamp(arg1, 0, 1023) : arg1);
            break;

        case kSetTimebase:
            player.timebase_ = static_cast<uint8_t>(arg1);
            break;

        case kSetHold:
            // Only a 2008 build supports the hold and monophony (Mario Kart Wii, 0x80096b90 and 0x80096be8). An older
            // build ignores the commands.
            if (player.GetEngine().Build().Since2008())
            {
                hold_ = static_cast<int16_t>(arg1 & 0xff);
            }
            break;

        case kSetMonophonic:
            if (player.GetEngine().Build().Since2008())
            {
                monophonic_ = arg1 != 0;
                if (monophonic_)
                {
                    ReleaseAllChannel(-1);
                    FreeAllChannel();
                }
            }
            break;

        case kSetVelocityRange:
            // From June 2008, the notes' velocities are scaled by this over 127 (Wii Music, 0x800b95c4).
            if (player.GetEngine().Build().SinceJune2008())
            {
                velocity_range_ = static_cast<uint8_t>(arg1);
            }
            break;

        case kSetBiquadType:
            // From June 2008, the notes' biquad filter (Wii Music, 0x800b9870) and its value over 127 (0x800b9878).
            if (player.GetEngine().Build().SinceJune2008())
            {
                biquad_type_ = static_cast<uint8_t>(arg1);
            }
            break;

        case kSetBiquadValue:
            if (player.GetEngine().Build().SinceJune2008())
            {
                biquad_value_ = static_cast<float>(arg1) / 127.0f;
            }
            break;

        case kSetProgram:
            if (arg1 < 0x10000)
            {
                program_ = arg1 & 0xffff;
            }
            break;

        case kSetMute:
            SetMute(arg1);
            break;

        case kSetTrackVolume:
            volume_.SetTarget(static_cast<uint8_t>(arg1), static_cast<int16_t>(arg2));
            break;

        case kSetTrackVolume2:
            volume2_ = static_cast<uint8_t>(arg1);
            break;

        case kSetPlayerVolume:
            player.volume_ = static_cast<uint8_t>(arg1);
            break;

        case kSetTranspose:
            transpose_ = static_cast<int8_t>(arg1);
            break;

        case kSetPitchBend:
            pitch_bend_ = static_cast<int8_t>(arg1);
            break;

        case kSetBendRange:
            bend_range_ = static_cast<uint8_t>(arg1);
            break;

        case kSetPan:
            pan_.SetTarget(static_cast<int8_t>(PanArgument(arg1) - 64), static_cast<int16_t>(arg2));
            break;

        case kSetInitPan:
            init_pan_ = static_cast<int8_t>(PanArgument(arg1) - 64);
            break;

        case kSetSurroundPan:
            surround_pan_.SetTarget(static_cast<int8_t>(PanArgument(arg1)), static_cast<int16_t>(arg2));
            break;

        case kSetPriority:
            priority_ = static_cast<uint8_t>(arg1);
            break;

        case kSetNoteWait:
            note_wait_ = arg1 != 0;
            break;

        case kSetPortaTime:
            porta_time_ = static_cast<uint8_t>(arg1);
            break;

        case kSetLfoDepth:
            lfo_param_.depth = static_cast<float>(static_cast<uint8_t>(arg1)) / 128.0f;
            break;

        case kSetLfoSpeed:
            lfo_param_.speed = static_cast<float>(static_cast<uint8_t>(arg1)) * (100.0f / 256.0f);
            break;

        case kSetLfoTarget:
            lfo_target_ = static_cast<uint8_t>(arg1);
            break;

        case kSetLfoRange:
            lfo_param_.range = static_cast<uint8_t>(arg1);
            break;

        case kSetLfoDelay:
            lfo_param_.delay = static_cast<uint32_t>(arg1 * 5);
            break;

        case kSetSweepPitch:
            // In 32nds of a semitone, or 64ths in a 2008 build (Mario Kart Wii, 0x80096b58).
            sweep_pitch_ = static_cast<float>(arg1) / (player.GetEngine().Build().Since2008() ? 64.0f : 32.0f);
            break;

        case kSetAttack:
            attack_ = static_cast<uint8_t>(arg1);
            break;

        case kSetDecay:
            decay_ = static_cast<uint8_t>(arg1);
            break;

        case kSetSustain:
            sustain_ = static_cast<uint8_t>(arg1);
            break;

        case kSetRelease:
            release_ = static_cast<uint8_t>(arg1);
            break;

        case kSetDamper:
            damper_ = static_cast<uint8_t>(arg1) >= 64;
            break;

        case kSetTie:
            tie_ = arg1 != 0;
            ReleaseAllChannel(-1);
            FreeAllChannel();
            break;

        case kSetPortamento:
            porta_key_ = static_cast<uint8_t>(arg1 + transpose_);
            porta_ = true;
            break;

        case kSetPortaSpeed:
            porta_ = arg1 != 0;
            break;

        case kSetLpfFreq:
            lpf_freq_ = static_cast<uint8_t>(arg1);
            break;

        case kSetFxSendA:
            fx_send_[0] = static_cast<uint8_t>(arg1);
            break;

        case kSetFxSendB:
            fx_send_[1] = static_cast<uint8_t>(arg1);
            break;

        case kSetFxSendC:
            fx_send_[2] = static_cast<uint8_t>(arg1);
            break;

        case kSetMainSend:
            main_send_ = static_cast<uint8_t>(arg1);
            break;

        case kOpenTrack:
            {
                // Another track starts at an offset in the same sequence.
                SeqTrack* track = player.Track(arg1);
                if (track != nullptr && track != this)
                {
                    track->Close();
                    track->SetSeqData(data_, static_cast<uint32_t>(arg2));
                    track->Open();
                    MarkChange(); // the new track's sound is this pass's change
                }
                break;
            }

        case kJump:
            {
                // A jump back inside a subroutine isn't a loop of the sequence. An example is a subroutine that draws a
                // random value again until it differs from the last. The exception is a subroutine that can't be left
                // while the track holds a note that sounds until the game stops it. Such a subroutine may bend a held
                // note's pitch forever.
                const auto target = static_cast<uint32_t>(arg1);
                const bool in_call = std::any_of(called_.begin(), called_.begin() + call_depth_, std::identity());
                const bool holding = !conditional_ && HoldsSustainedNote();
                if (target < current_ && target < run_.size() && run_[target] != 0 && (!in_call || holding))
                {
                    CountLoop(target);
                }

                current_ = target;
                break;
            }

        case kCall:
            if (call_depth_ < call_stack_.size())
            {
                called_[call_depth_] = true;
                call_stack_[call_depth_++] = current_;
                current_ = static_cast<uint32_t>(arg1);
            }
            break;

        case kReturn:
            if (call_depth_ > 0)
            {
                current_ = call_stack_[--call_depth_];
            }
            break;

        case kEnvReset:
            // From June 2008: the notes take their envelopes from the bank again (Wii Music, 0x800b9788).
            if (player.GetEngine().Build().SinceJune2008())
            {
                attack_ = 0xff;
                decay_ = 0xff;
                sustain_ = 0xff;
                release_ = 0xff;
                hold_ = 0xff;
            }
            break;

        case kLoopStart:
            if (call_depth_ < call_stack_.size())
            {
                called_[call_depth_] = false;
                call_stack_[call_depth_] = current_;
                loop_count_[call_depth_] = static_cast<uint8_t>(arg1);
                call_depth_++;
            }
            break;

        case kLoopEnd:
            {
                // A count of 0 loops forever. On the main track, that's a loop of the sequence.
                if (call_depth_ == 0)
                {
                    break;
                }

                uint8_t count = loop_count_[call_depth_ - 1];
                if (count != 0 && --count == 0)
                {
                    call_depth_--;
                }
                else
                {
                    if (count == 0)
                    {
                        CountLoop(call_stack_[call_depth_ - 1]);
                    }

                    loop_count_[call_depth_ - 1] = count;
                    current_ = call_stack_[call_depth_ - 1];
                }
                break;
            }

        default:
            break;
        }

        return;
    }

    // An extended command on a variable.
    const uint32_t ex = command & 0xff;
    int16_t* variable = nullptr;
    if ((ex & 0xf0) == kExArithmetic || (ex & 0xf0) == kExLogic)
    {
        variable = Variable(arg1);
        if (variable == nullptr)
        {
            return;
        }
    }

    if ((ex & 0xf0) == kExLogic)
    {
        MarkRead(arg1);
    }

    switch (ex)
    {
    case kExSet:
        *variable = static_cast<int16_t>(arg2);
        break;

    case kExAdd:
        *variable = static_cast<int16_t>(*variable + arg2);
        break;

    case kExSubtract:
        *variable = static_cast<int16_t>(*variable - arg2);
        break;

    case kExMultiply:
        *variable = static_cast<int16_t>(WrapMultiply(*variable, arg2));
        break;

    case kExDivide:
        if (arg2 != 0)
        {
            *variable = static_cast<int16_t>(*variable / arg2);
        }
        break;

    case kExShift:
        *variable = static_cast<int16_t>(arg2 >= 0 ? ShiftLeft(*variable, arg2) : ShiftRight(*variable, -arg2));
        break;

    case kExRandom:
        {
            // A random number from 0 to the argument, or from its negative to 0.
            const bool minus = arg2 < 0;
            if (minus)
            {
                arg2 = static_cast<int16_t>(-arg2);
            }

            int32_t random = player.GetEngine().Rng().Next();
            drew_at_ = player.commands_run_;
            random = WrapMultiply(random, arg2 + 1) >> 16;
            *variable = static_cast<int16_t>(minus ? -random : random);
            break;
        }

    case kExAnd:
        *variable = static_cast<int16_t>(*variable & arg2);
        break;

    case kExOr:
        *variable = static_cast<int16_t>(*variable | arg2);
        break;

    case kExXor:
        *variable = static_cast<int16_t>(*variable ^ arg2);
        break;

    case kExNot:
        *variable = static_cast<int16_t>(~static_cast<uint16_t>(arg2));
        break;

    case kExModulo:
        if (arg2 != 0)
        {
            *variable = static_cast<int16_t>(*variable % arg2);
        }
        break;

    case kExEqual:
        cmp_flag_ = *variable == arg2;
        break;

    case kExGreaterEqual:
        cmp_flag_ = *variable >= arg2;
        break;

    case kExGreater:
        cmp_flag_ = *variable > arg2;
        break;

    case kExLessEqual:
        cmp_flag_ = *variable <= arg2;
        break;

    case kExLess:
        cmp_flag_ = *variable < arg2;
        break;

    case kExNotEqual:
        cmp_flag_ = *variable != arg2;
        break;

    default:
        break;
    }
}

int32_t SeqTrack::ReadArg(ArgType type)
{
    // MmlParser::ReadArg (0x8003c638).
    switch (type)
    {
    case ArgType::kU8:
        return ReadByte();

    case ArgType::kS16:
        return Read16();

    case ArgType::kVmidi:
        return ReadVar();

    case ArgType::kVariable:
        {
            // The game leaves the argument unset for an index past the track's variables. The model gives 0.
            const int index = ReadByte();
            MarkRead(index);
            const int16_t* variable = Variable(index);

            return variable != nullptr ? *variable : 0;
        }

    case ArgType::kRandom:
        {
            const auto min = static_cast<int16_t>(Read16());
            const auto max = static_cast<int16_t>(Read16());
            int32_t random = player_->GetEngine().Rng().Next();
            drew_at_ = player_->commands_run_;
            random = WrapMultiply(random, max - min + 1) >> 16;

            return random + min;
        }
    }

    return 0;
}

uint8_t SeqTrack::ReadByte()
{
    // Past the end of the sequence, the track reads the end command.
    if (current_ >= data_.size())
    {
        current_ = static_cast<uint32_t>(data_.size()) + 1;

        return kEnd;
    }

    return data_[current_++];
}

uint16_t SeqTrack::Read16()
{
    const uint16_t high = ReadByte();

    return static_cast<uint16_t>(high << 8 | ReadByte());
}

uint32_t SeqTrack::Read24()
{
    uint32_t value = ReadByte();
    value = value << 8 | ReadByte();

    return value << 8 | ReadByte();
}

int32_t SeqTrack::ReadVar()
{
    // A variable-length number, seven bits a byte, most significant first.
    uint32_t value = 0;
    uint8_t byte = 0;
    do
    {
        byte = ReadByte();
        value = value << 7 | (byte & 0x7f);
    } while (byte & 0x80);

    return static_cast<int32_t>(value);
}

int16_t* SeqTrack::Variable(int index)
{
    if (index < 32)
    {
        return player_->Variable(index);
    }
    if (index < 48)
    {
        return &variables_[index - 32];
    }

    return nullptr;
}

bool SeqTrack::NoteOn(int key, int velocity, int32_t length, bool tie)
{
    // NoteOn (0x80040bc0). In a tie, the last note takes the new key and velocity. Otherwise the bank starts a note. On
    // a monophonic track, the last note also takes the new length (Mario Kart Wii, 0x8009be98). If that note is being
    // released, it stops and the bank starts the new note instead. From June 2008, the velocity is first scaled by the
    // track's velocity range over 127 (Wii Music, 0x800bee00). It can then exceed 127.
    SeqPlayer& player = *player_;
    Channel* channel = nullptr;
    velocity = velocity * velocity_range_ / 127;
    const float velocity_volume = static_cast<float>(velocity) / 127.0f;
    const Channel* newest = channels_;
    const bool same =
        newest != nullptr && newest->Key() == key && newest->InitVolume() == velocity_volume * velocity_volume;
    if (tie)
    {
        channel = channels_;
        if (channel != nullptr)
        {
            channel->SetKey(key);
            const float init_volume = static_cast<float>(velocity) / 127.0f;
            channel->SetInitVolume(init_volume * init_volume);
        }
    }

    if (monophonic_)
    {
        channel = channels_;
        if (channel != nullptr && channel->IsRelease())
        {
            channel->Stop();
            channel = nullptr;
        }
        else if (channel != nullptr)
        {
            channel->SetKey(key);
            const float init_volume = static_cast<float>(velocity) / 127.0f;
            channel->SetInitVolume(init_volume * init_volume);
            channel->SetLength(length);
        }
    }

    if (channel == nullptr)
    {
        NoteOnInfo info;
        info.program = program_;
        info.key = key;
        info.velocity = velocity;
        info.length = tie ? -1 : length;
        info.init_pan = init_pan_;
        info.priority = player.priority_ + priority_;
        info.callback = [this](Channel* ended, Channel::CallbackStatus status)
        {
            OnChannelEvent(ended, status);
        };

        channel = player.NoteOn(info);
        if (channel == nullptr)
        {
            return false;
        }

        // A 2008 build's note releases the track's notes of its key group (Mario Kart Wii, 0x8009bfcc).
        if (channel->KeyGroup() > 0)
        {
            for (Channel* other = channels_; other != nullptr; other = other->NextTrackChannel())
            {
                if (other->KeyGroup() == channel->KeyGroup())
                {
                    other->Release();
                }
            }
        }

        AddChannel(channel);
    }

    if (attack_ != 0xff)
    {
        channel->SetAttack(attack_);
    }

    if (decay_ != 0xff)
    {
        channel->SetDecay(decay_);
    }

    if (sustain_ != 0xff)
    {
        channel->SetSustain(sustain_);
    }

    if (release_ != 0xff)
    {
        channel->SetRelease(release_);
    }

    if (hold_ <= 127)
    {
        channel->SetHold(hold_);
    }

    // The sweep runs over the note's length. With portamento, it runs over a time that grows with the square of the
    // portamento time and with the interval.
    float sweep_pitch = sweep_pitch_;
    if (porta_)
    {
        sweep_pitch += static_cast<float>(porta_key_ - key);
    }

    if (porta_time_ == 0)
    {
        channel->SetSweepParam(sweep_pitch, length, false);
    }
    else
    {
        int time = porta_time_ * porta_time_;
        time = static_cast<int>(static_cast<float>(time) * (sweep_pitch >= 0.0f ? sweep_pitch : -sweep_pitch));
        time >>= 5;
        time = (time << 2) + time;
        channel->SetSweepParam(sweep_pitch, time, true);
    }

    porta_key_ = static_cast<uint8_t>(key);
    channel->SetReleasePriorityFix(player.release_priority_fix_);
    channel->SetPanMode(player.pan_mode_);
    channel->SetPanCurve(player.pan_curve_);

    return channel != newest || !same || sweep_pitch != 0.0f;
}

void SeqTrack::SetMute(int mute)
{
    // SetMute (0x80040a28): off, no new notes, no new notes and release the old ones, or no new notes and stop the old
    // ones.
    switch (mute)
    {
    case 0:
        mute_ = false;
        break;

    case 1:
        mute_ = true;
        break;

    case 2:
        ReleaseAllChannel(-1);
        FreeAllChannel();
        mute_ = true;
        break;

    case 3:
        StopAllChannel();
        mute_ = true;
        break;

    default:
        break;
    }
}

void SeqTrack::StopAllChannel()
{
    for (Channel* channel = channels_; channel != nullptr;)
    {
        Channel* next = channel->NextTrackChannel();
        Channel::FreeChannel(channel);
        channel->Stop();
        channel = next;
    }

    channels_ = nullptr;
}

void SeqTrack::AddChannel(Channel* channel)
{
    channel->SetNextTrackChannel(channels_);
    channels_ = channel;
}

void SeqTrack::UpdateChannelRelease(Channel* channel)
{
    // A note whose length has run out gets a note-off (Mario Kart Wii, 0x8009b20c). Only a note in a 2008 build can
    // ignore it.
    if (channel->Length() == 0 && !channel->IsRelease() && !damper_)
    {
        channel->NoteOff();
    }
}

void SeqTrack::CountLoop(uint32_t target)
{
    if (!player_->on_loop)
    {
        return;
    }

    // The pass began when the track last ran `target`. A pass of an outer loop includes the passes of the inner loops
    // in it. A still pass also left the variables that tracks read with the values that the pass before left. The first
    // pass compares them with 0. A variable that no track reads doesn't count. The game may read it. The beats that a
    // timing track marks are an example.
    const uint64_t begun = target < run_.size() ? run_[target] : 0;
    std::array<int16_t, 48> variables{};
    for (int i = 0; i < static_cast<int>(variables.size()); i++)
    {
        const int16_t* variable = IsRead(i) ? Variable(i) : nullptr;
        variables[static_cast<std::size_t>(i)] = variable != nullptr ? *variable : 0;
    }

    idle_ = changed_at_ < begun;
    quiet_ = noted_at_ < begun;
    still_ = idle_ && drew_at_ < begun && variables == loop_variables_;
    loop_variables_ = variables;
    looped_at_ = player_->commands_run_;
    const bool alone = HoldsSustainedNote() && player_->OthersRest(index_);
    if (!idle_ && (index_ == 0 || (!quiet_ && player_->IsMainTrack(index_)) || alone))
    {
        player_->on_loop(index_, target);
    }
}

bool SeqTrack::WaitsOnEndlessNote() const
{
    if (!note_finish_wait_)
    {
        return false;
    }

    // A note of length 0 has a length of -1 and is never released. While the track waits, nothing else releases it.
    for (const Channel* channel = channels_; channel != nullptr; channel = channel->NextTrackChannel())
    {
        if (channel->Length() < 0 && channel->WaveLoops())
        {
            return true;
        }
    }

    return false;
}

bool SeqTrack::HasEndingNote() const
{
    if (holds_)
    {
        return false;
    }

    for (const Channel* channel = channels_; channel != nullptr; channel = channel->NextTrackChannel())
    {
        if (channel->Length() > 0)
        {
            return true;
        }
    }

    return false;
}

void SeqTrack::MarkChange()
{
    changed_at_ = player_->commands_run_;
    player_->changed_at_ = changed_at_;
}

void SeqTrack::MarkRead(int index)
{
    if (index >= 0 && index < 32)
    {
        player_->variables_read_ |= 1u << index;
    }
    else if (index >= 32 && index < 48)
    {
        variables_read_ = static_cast<uint16_t>(variables_read_ | 1u << (index - 32));
    }
}

bool SeqTrack::IsRead(int index) const
{
    if (index >= 0 && index < 32)
    {
        return (player_->variables_read_ >> index & 1) != 0;
    }
    if (index >= 32 && index < 48)
    {
        return (variables_read_ >> (index - 32) & 1) != 0;
    }

    return false;
}

bool SeqTrack::HoldsSustainedNote() const
{
    // A note of length 0 has a length of -1.
    for (const Channel* channel = channels_; channel != nullptr; channel = channel->NextTrackChannel())
    {
        const bool endless = holds_ || channel->Length() < 0;
        if (endless && channel->IsSustain() && channel->WaveLoops())
        {
            return true;
        }
    }

    return false;
}

void SeqTrack::OnChannelEvent(Channel* channel, Channel::CallbackStatus status)
{
    // ChannelCallbackFunc (0x80040930). A note that stopped or finished is freed. Every note that ends leaves the list.
    if (status == Channel::CallbackStatus::kStopped || status == Channel::CallbackStatus::kFinish)
    {
        Channel::FreeChannel(channel);
    }

    if (channels_ == channel)
    {
        channels_ = channel->NextTrackChannel();

        return;
    }

    for (Channel* it = channels_; it != nullptr && it->NextTrackChannel() != nullptr; it = it->NextTrackChannel())
    {
        if (it->NextTrackChannel() == channel)
        {
            it->SetNextTrackChannel(channel->NextTrackChannel());

            return;
        }
    }
}

SeqPlayer::SeqPlayer(Engine& engine, const rsar::SoundInfo& sound, const rsar::Sequence& sequence,
                     const LoadedBank* bank)
    : engine_(&engine), sound_(&sound), sequence_(&sequence), bank_(bank)
{
    local_variables_.fill(kVariableDefault);
}

SeqPlayer::~SeqPlayer()
{
    for (std::optional<SeqTrack>& track : tracks_)
    {
        if (track)
        {
            track->DetachChannels();
        }
    }
}

void SeqPlayer::Start()
{
    // Setup (0x8003e1f4): a track for each bit of the sound's track mask. PrepareSeqImpl then gives the player the
    // sound's settings. BasicSound::Update gives it the volume: the sound's volume over 127, times factors of 1.
    uint32_t flags = sound_->allocate_track;
    for (int i = 0; flags != 0; flags >>= 1, i++)
    {
        if (flags & 1)
        {
            tracks_[i].emplace(*this, i);
        }
    }

    active_ = true;
    priority_ = sound_->channel_priority;
    release_priority_fix_ = sound_->release_priority_fix;
    pan_mode_ = static_cast<PanMode>(sound_->pan_mode);
    pan_curve_ = sound_->pan_curve;
    player_volume_ = std::clamp(static_cast<float>(sound_->volume) / 127.0f, 0.0f, 1.0f);

    // SetSeqData (0x8003e488) and Start (0x8003e504).
    SeqTrack* first = Track(0);
    if (first != nullptr)
    {
        first->SetSeqData(sequence_->data, sound_->data_offset);
        first->Open();
    }

    started_ = true;
}

void SeqPlayer::Update()
{
    if (!active_ || !started_)
    {
        return;
    }

    if (engine_->Build().October2006())
    {
        UpdateTickOctober2006();
    }
    else
    {
        UpdateTick(3);
    }

    for (std::optional<SeqTrack>& track : tracks_)
    {
        if (track)
        {
            track->UpdateChannelParam();
        }
    }
}

float SeqPlayer::BaseTempo() const
{
    // The tempo ratio is 1.
    return 1.0f * static_cast<float>(timebase_ * tempo_) / 60000.0f;
}

int16_t* SeqPlayer::Variable(int index)
{
    // The global variables are zero until the game sets one. Archive mode never does.
    if (index < 16)
    {
        return &local_variables_[index];
    }
    if (index < 32)
    {
        return &engine_->GlobalVariables()[index - 16];
    }

    return nullptr;
}

bool SeqPlayer::IsSettled() const
{
    const auto settled = [](const SeqTrack& track)
    {
        return track.WaitsForNotes() || (track.IsStill() && !track.HasEndingNote());
    };

    return tempo_ == 0 || EveryOpenTrack(settled);
}

bool SeqPlayer::IsMainTrack(int index) const
{
    // A track before it keeps it from being the main track when that track's loops would count. Track 0's loops count
    // unless its passes are idle. A later track's loops count only when its passes play notes. A track that waits for
    // ever makes no more passes.
    for (int i = 0; i < index; i++)
    {
        const std::optional<SeqTrack>& track = tracks_[i];
        if (track && track->IsOpen() && !track->IsIdle() && !track->WaitsForEver() && (i == 0 || !track->IsQuiet()))
        {
            return false;
        }
    }

    return true;
}

bool SeqPlayer::OthersRest(int index) const
{
    for (int i = 0; i < static_cast<int>(tracks_.size()); i++)
    {
        const std::optional<SeqTrack>& track = tracks_[static_cast<std::size_t>(i)];
        if (i != index && track && track->IsOpen() && !track->IsIdle() && !track->WaitsForEver())
        {
            return false;
        }
    }

    return true;
}

bool SeqPlayer::IsStuck() const
{
    const auto stuck = [](const SeqTrack& track)
    {
        return track.WaitsForEver() || (track.IsStill() && track.IsQuiet());
    };

    return tempo_ == 0 || EveryOpenTrack(stuck);
}

bool SeqPlayer::IsMoving() const
{
    const auto still = [](const SeqTrack& track)
    {
        return !track.IsMoving();
    };

    return !EveryOpenTrack(still);
}

int SeqPlayer::ParseNextTick(bool do_note_on)
{
    // ParseNextTick (0x8003ec44): each track counts its notes down by a tick and parses its next tick. Returns 1 when
    // no track is open.
    bool active = false;
    for (int i = 0; i < 16; i++)
    {
        SeqTrack* track = Track(i);
        if (track == nullptr)
        {
            continue;
        }

        track->UpdateChannelLength();
        if (track->ParseNextTick(do_note_on) < 0)
        {
            CloseTrack(i);
        }
        else if (track->IsOpen())
        {
            active = true;
        }
    }

    return active ? 0 : 1;
}

void SeqPlayer::UpdateTick(int msec)
{
    // UpdateTick (0x8003eec4): runs the ticks that fall within the frame. The leftover part of a tick carries over.
    float tick_per_msec = BaseTempo();
    if (tick_per_msec == 0.0f)
    {
        return;
    }

    float rest_msec = static_cast<float>(msec);
    float next_msec = tick_fraction_ / tick_per_msec;
    while (next_msec < rest_msec)
    {
        rest_msec -= next_msec;
        if (ParseNextTick(true) != 0)
        {
            FinishPlayer();

            return;
        }

        tick_per_msec = BaseTempo();
        if (tick_per_msec == 0.0f)
        {
            return;
        }

        next_msec = 1.0f / tick_per_msec;
    }

    next_msec -= rest_msec;
    tick_fraction_ = next_msec * tick_per_msec;
}

void SeqPlayer::UpdateTickOctober2006()
{
    // The build of October 2006 (0x800e6588 in Smooth Moves' main.dol) counts time in 416ths of a tick, from 416 at the
    // start. Each frame, the player takes out the whole ticks first. It adds the tempo times the tempo ratio next. WSF
    // leaves that ratio at 1. The player then runs the ticks that it took out. The timebase plays no part: a tick lasts
    // 416 / tempo frames, 0.16% shorter than 48 ticks a beat would make it.
    int ticks = 0;
    while (tick_units_ >= 416)
    {
        ticks++;
        tick_units_ -= 416;
    }

    tick_units_ = static_cast<uint16_t>(tick_units_ + tempo_);
    for (; ticks > 0; ticks--)
    {
        if (ParseNextTick(true) != 0)
        {
            FinishPlayer();

            return;
        }
    }
}

void SeqPlayer::CloseTrack(int index)
{
    // CloseTrack: closes the track and returns it to the allocator.
    SeqTrack* track = Track(index);
    if (track != nullptr)
    {
        track->Close();
        tracks_[index].reset();
    }
}

void SeqPlayer::FinishPlayer()
{
    // FinishPlayer: stops the player and closes its tracks.
    started_ = false;
    active_ = false;
    for (int i = 0; i < 16; i++)
    {
        CloseTrack(i);
    }
}

Channel* SeqPlayer::NoteOn(const NoteOnInfo& info)
{
    // NoteOn (0x8003f3c8), through SoundArchivePlayer's note-on callback. It uses the sequence's bank, whatever bank
    // the track names.
    return bank_ != nullptr ? engine_->NoteOn(*bank_, info) : nullptr;
}

} // namespace wsf::nw4r
