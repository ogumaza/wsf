// SPDX-License-Identifier: MIT

// NW4R SND model tests, on the AX model with no DSP: the parameter blocks a note's channel sets up, the ramps from
// frame to frame, the release and the end of a channel, the voices NW4R SND drops by priority when all are in use, a
// short sequence played from an archive, what the builds of 2008 do differently, the loops the ripper's length
// analysis counts, snapshots of the model, and the end of a wave without the DSP.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ax/pb.h"
#include "ax/system.h"
#include "check.h"
#include "dsp/main_memory.h"
#include "nw4r/driver.h"
#include "nw4r/engine.h"
#include "nw4r/tables.h"
#include "nw4r/util.h"
#include "rsar/formats.h"
#include "rsar_files.h"

namespace
{

using namespace wsf;

// Builds of NW4R SND: those of Wii Sports, Mario Kart Wii and Wii Music.
constexpr nw4r::SndBuild kBuild2007{20070608};
constexpr nw4r::SndBuild kBuildMarch2008{20080308};
constexpr nw4r::SndBuild kBuildJune2008{20080620};

// An archive whose sequence sound has the tracks in the mask `tracks` and plays track 0 from the start of `commands`.
rsar::SoundArchive MakeArchive(const test::Bytes& commands, uint32_t tracks = 1)
{
    return rsar::SoundArchive::Load(test::MakeArchive(test::MakeSequence(commands), 0, tracks));
}

// AX, initialised as the game does before NW4R SND starts, with a compressor table of zeros.
ax::System& Initialised(ax::System& ax)
{
    ax.Init();

    return ax;
}

// The AX model and the engine on it, following `build` of NW4R SND. A frame runs as in the game: AX's interrupt, then
// the sound frame.
struct Model
{
    explicit Model(const test::Bytes& commands = {0xff}, nw4r::SndBuild build = {}, uint32_t tracks = 1)
        : archive(MakeArchive(commands, tracks)), engine{memory, Initialised(ax), archive, build}
    {
    }

    void RunFrame()
    {
        ax.StartFrame();
        ax.FinishFrame();
        engine.FrameProcess();
    }

    // The PB of the AX voice with `index`, as the microcode will read it.
    ax::Pb Pb(uint32_t index) const
    {
        return ax::LoadPb(memory.At(ax::layout::kPbs + index * 0x140, ax::kPbSize));
    }

    MainMemory memory;
    ax::System ax{memory};
    rsar::SoundArchive archive;
    nw4r::Engine engine;
};

// A second of a 32 kHz PCM16 mono wave that doesn't loop.
nw4r::WaveData MakeWave(MainMemory& memory)
{
    const std::vector<uint8_t> samples(64000, 0);
    nw4r::WaveData wave;
    wave.format = rsar::WaveFormat::kPcm16;
    wave.sample_rate = 32000;
    wave.loop_end = 32000;
    wave.channels[0].data = memory.Store(samples.data(), samples.size());

    return wave;
}

void TestNote()
{
    Model model;
    const nw4r::WaveData wave = MakeWave(model.memory);
    std::vector<nw4r::Channel::CallbackStatus> events;
    const auto on_event = [&events](nw4r::Channel*, nw4r::Channel::CallbackStatus status)
    {
        events.push_back(status);
    };

    // A note a fifth up, centred, at full volume. A bank's NoteOn starts notes this way during a sound frame.
    nw4r::Channel* channel = nw4r::Channel::AllocChannel(model.engine, 1, 64, on_event);
    Check(channel != nullptr, "a channel for a note");
    if (channel == nullptr)
    {
        return;
    }

    channel->SetKey(67);
    channel->SetOriginalKey(60);
    channel->Start(wave, -1, 0);
    model.engine.Channels().UpdateAllChannel();
    model.engine.Voices().UpdateAllVoices();

    // AX's first voice is its last. It gets the note's parameters with the next frame.
    model.ax.StartFrame();
    const ax::Pb pb = model.Pb(95);
    const auto center = static_cast<uint16_t>(32768.0f * nw4r::tables::kPanSqrt[128]);
    const uint32_t ratio = ax::Join(pb.src.ratio_hi, pb.src.ratio_lo);
    const uint32_t expected_ratio = static_cast<uint32_t>(65536.0f * nw4r::CalcPitchRatio(7 * 256));
    Check(pb.state == 1, "the note runs");
    Check(pb.mix.l == center && pb.mix.r == center && pb.mix.delta_l == 0 &&
              pb.MixerCtrl() == (ax::mixer::kL | ax::mixer::kR),
          "the note's mix");
    Check(pb.ve.current_volume == 32767 && pb.ve.current_delta == 0, "the note's volume");
    Check(ratio == expected_ratio && pb.src_select == 0 && pb.coef_select == 2, "the note's pitch and converter");
    Check(pb.addr.format == 10 && ax::Join(pb.addr.current_address_hi, pb.addr.current_address_lo) == kMem2Base / 2 &&
              ax::Join(pb.addr.end_address_hi, pb.addr.end_address_lo) == kMem2Base / 2 + 31999,
          "the note's samples");
    Check(ax::Join(pb.addr.loop_address_hi, pb.addr.loop_address_lo) == nw4r::kZeroBuffer / 2 && pb.addr.loop_flag == 0,
          "a note that doesn't loop goes on to the zero buffer");
    model.ax.FinishFrame();
    model.engine.FrameProcess();

    // At the default release rate, 127, the envelope falls to its floor at once. The channel stops in its next update.
    channel->Release();
    model.RunFrame();
    model.RunFrame();
    Check(!events.empty() && events.back() == nw4r::Channel::CallbackStatus::kStopped, "a released note stops");
    Check(model.engine.Channels().ActiveCount() == 0 && model.engine.Voices().PrioVoices().empty(),
          "the channel and voice freed");

    model.ax.StartFrame();
    Check(model.Pb(95).state == 0, "the AX voice stopped");
}

void TestRelease()
{
    Model model;
    const nw4r::WaveData wave = MakeWave(model.memory);
    nw4r::Channel* channel = nw4r::Channel::AllocChannel(model.engine, 1, 64, nullptr);
    channel->SetRelease(100);
    channel->Start(wave, -1, 0);
    model.RunFrame();
    model.RunFrame();

    // A slower release ramps the envelope down over each frame. The volume falls from where the last ramp ended.
    channel->Release();
    model.RunFrame();
    model.ax.StartFrame();
    const ax::Pb first = model.Pb(95);
    model.ax.FinishFrame();
    model.engine.FrameProcess();
    model.ax.StartFrame();
    const ax::Pb second = model.Pb(95);
    const int16_t delta = static_cast<int16_t>(first.ve.current_delta);
    Check(delta < 0, "the release ramps down");
    Check(second.ve.current_volume == static_cast<uint16_t>(first.ve.current_volume + delta * 96),
          "the next frame starts where the ramp ended");
    Check(model.engine.Voices().PrioVoices().front()->Priority() == 1, "a released voice drops to priority 1");
}

// The volume at which an AX voice starts a frame after its envelope lands at 74 and then aims for 0, under `build`. A
// step towards 0 would take the volume below 0 in a frame. The step is therefore 0. A 2007 build then leaves the volume
// where it was. A 2008 build lands it on the target (AxVoiceParamBlock::Sync). A released note's last frame does the
// same.
uint16_t VolumeAfterLastStep(nw4r::SndBuild build)
{
    Model model({0xff}, build);
    model.RunFrame();
    const nw4r::WaveData wave = MakeWave(model.memory);
    nw4r::AxVoice* voice = model.engine.AxVoices().AcquireAxVoice(31, nullptr);
    voice->Setup(wave.channels[0].data, ax::SampleFormat::kPcm16, 32000);
    voice->SetAddr(false, wave.channels[0].data, 0, 0, wave.loop_end);
    voice->SetVe(74.5f / 32767.0f, 74.5f / 32767.0f);
    voice->Sync();
    voice->SetVe(0.0f, 0.0f);
    voice->Sync();
    model.ax.StartFrame();

    return model.Pb(95).ve.current_volume;
}

void TestLastStep()
{
    const uint16_t older = VolumeAfterLastStep(kBuild2007);
    const uint16_t newer = VolumeAfterLastStep(kBuildMarch2008);

    Check(older == 74, "a 2007 build leaves the volume where it was (" + std::to_string(older) + ")");
    Check(newer == 0, "a 2008 build lands the volume on its target (" + std::to_string(newer) + ")");
}

// The build comes from main.dol's banner. A DOL without a banner gives none.
void TestSndBuild()
{
    const auto dol = [](const std::string& text)
    {
        std::vector<uint8_t> bytes(64, 0);
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.resize(bytes.size() + 64, 0);

        return bytes;
    };

    Check(nw4r::FindSndBuild(dol("<< NW4R    - SND \tfinal   build: Mar  8 2008 20:59:14 (0x4201_127) >>")).date ==
              20080308,
          "a build's date");
    Check(nw4r::FindSndBuild(dol("<< NW4R    - SND \trelease build: Nov 20 2008 03:40:59 (0x4199_60831) >>")).date ==
              20081120,
          "a release build's date");
    Check(nw4r::FindSndBuild(dol("<< NW4R    - G3D \tfinal   build: Jun  8 2007 11:17:15 (0x4199_60831) >>")).date == 0,
          "another library's banner");
    Check(nw4r::FindSndBuild(dol("no banner")).date == 0, "no banner");
}

void TestDrop()
{
    Model model;
    const nw4r::WaveData wave = MakeWave(model.memory);

    // 96 notes take every voice. The 97th, of higher priority, takes the oldest voice of the lowest priority.
    std::vector<int> dropped;
    for (int i = 0; i < 96; i++)
    {
        const auto on_event = [&dropped, i](nw4r::Channel*, nw4r::Channel::CallbackStatus status)
        {
            if (status == nw4r::Channel::CallbackStatus::kDrop)
            {
                dropped.push_back(i);
            }
        };

        nw4r::Channel* channel = nw4r::Channel::AllocChannel(model.engine, 1, i < 10 ? 32 : 64, on_event);
        Check(channel != nullptr, "a channel while voices are free");
        if (channel != nullptr)
        {
            channel->Start(wave, -1, 0);
        }
    }

    model.RunFrame();
    Check(nw4r::Channel::AllocChannel(model.engine, 1, 16, nullptr) == nullptr, "no voice for a lower priority");
    nw4r::Channel* channel = nw4r::Channel::AllocChannel(model.engine, 1, 100, nullptr);
    Check(channel != nullptr && dropped.size() == 1 && dropped[0] == 0, "the oldest lowest-priority note dropped");
}

void TestSequence()
{
    // Release 127, a note of 48 ticks (key 60, velocity 100), and the end. At the default tempo of 120 and timebase of
    // 48, 48 ticks last 500 ms, or about 167 frames.
    Model model({0xd3, 0x7f, 0x3c, 0x64, 0x30, 0xff});
    nw4r::SeqPlayer* player = model.engine.StartSequence(0);
    Check(player != nullptr, "the sequence starts");
    if (player == nullptr)
    {
        return;
    }

    model.RunFrame();
    model.ax.StartFrame();
    const ax::Pb pb = model.Pb(95);
    const float velocity = 100.0f / 127.0f;
    const float sound_volume = 90.0f / 127.0f;
    const float volume = velocity * velocity * (100.0f / 127.0f) * sound_volume;
    const auto expected_ve = static_cast<uint16_t>(static_cast<int>(32767.0f * volume));
    Check(pb.state == 1 && pb.addr.format == 0 && pb.addr.loop_flag == 1, "the note plays its looping ADPCM wave");
    Check(pb.ve.current_volume == expected_ve, "the note's volume: velocity squared, the region's and the sound's (" +
                                                   std::to_string(pb.ve.current_volume) + ", expected " +
                                                   std::to_string(expected_ve) + ")");
    model.ax.FinishFrame();
    model.engine.FrameProcess();

    // The sound ends when its note has been released and stopped, and its track has reached the end.
    int frames = 2;
    while (model.engine.IsBusy() && frames < 400)
    {
        model.RunFrame();
        frames++;
    }

    Check(!model.engine.IsBusy() && frames >= 167 && frames <= 170,
          "the sequence ends after its note (" + std::to_string(frames) + " frames)");
}

// The voices' PBs after the first frame of a sequence of `commands` played under `build`, and the model to play on.
struct Started
{
    Started(const test::Bytes& commands, nw4r::SndBuild build) : model(commands, build)
    {
        player = model.engine.StartSequence(0);
        pb = PbAfter(1);
    }

    // Runs `frames` frames, then returns the first voice's PB as the microcode reads it in the next.
    ax::Pb PbAfter(int frames)
    {
        for (int i = 0; i < frames; i++)
        {
            model.RunFrame();
        }

        model.ax.StartFrame();
        const ax::Pb next = model.Pb(95);
        model.ax.FinishFrame();
        model.engine.FrameProcess();

        return next;
    }

    // Runs frames until the sound ends, at most `frames` of them. Returns the number of frames run.
    int RunUntilEnd(int frames)
    {
        int ran = 0;
        while (model.engine.IsBusy() && ran < frames)
        {
            model.RunFrame();
            ran++;
        }

        return ran;
    }

    Model model;
    nw4r::SeqPlayer* player = nullptr;
    ax::Pb pb;
};

// The volume at which a note of `velocity` starts on instrument 0. The region's volume is 100 and the sound's volume is
// 90.
uint16_t ExpectedVolume(int velocity)
{
    const float scale = static_cast<float>(velocity) / 127.0f;

    return static_cast<uint16_t>(static_cast<int>(32767.0f * (scale * scale * (100.0f / 127.0f) * (90.0f / 127.0f))));
}

void TestSweepPitch()
{
    // A sweep of -64 over the note: two semitones down at the start in a 2007 build, one in a 2008 build.
    const test::Bytes commands = {0xe3, 0xff, 0xc0, 0x3c, 0x64, 0x30, 0xff};
    const Started older(commands, kBuild2007);
    const Started newer(commands, kBuildMarch2008);

    const double older_ratio = ax::Join(older.pb.src.ratio_hi, older.pb.src.ratio_lo) / 65536.0;
    const double newer_ratio = ax::Join(newer.pb.src.ratio_hi, newer.pb.src.ratio_lo) / 65536.0;
    Check(std::abs(older_ratio - std::pow(2.0, -2.0 / 12.0)) < 0.005,
          "a 2007 build sweeps from two semitones down (" + std::to_string(older_ratio) + ")");
    Check(std::abs(newer_ratio - std::pow(2.0, -1.0 / 12.0)) < 0.005,
          "a 2008 build sweeps from one semitone down (" + std::to_string(newer_ratio) + ")");
}

void TestTempo()
{
    // The fastest release, a tempo from variable 1 (which is -1) and a note of 48 ticks. A 2007 build races through the
    // sequence. A 2008 build stops the sequence and holds the note.
    const test::Bytes commands = {0xd3, 0x7f, 0xa1, 0xe1, 0x01, 0x3c, 0x64, 0x30, 0xff};
    Started older(commands, kBuild2007);
    Started newer(commands, kBuildMarch2008);

    const int older_frames = older.RunUntilEnd(300);
    const int newer_frames = newer.RunUntilEnd(300);

    Check(older_frames < 10, "a 2007 build plays a tempo of -1 at full speed (" + std::to_string(older_frames) + ")");
    Check(newer_frames == 300 && newer.model.engine.Channels().ActiveCount() == 1,
          "a 2008 build stops at a tempo of 0");
    Check(newer.player->WaitsForEver() && newer.player->WaitsForNotes(), "a sequence at a tempo of 0 waits for ever");
}

void TestVelocityRange()
{
    // From June 2008, a velocity range of 50 turns a velocity of 100 into 39. Earlier builds ignore it.
    const test::Bytes commands = {0xb3, 0x32, 0x3c, 0x64, 0x30, 0xff};
    const Started march(commands, kBuildMarch2008);
    const Started june(commands, kBuildJune2008);

    Check(march.pb.ve.current_volume == ExpectedVolume(100), "a build of March 2008 ignores the velocity range");
    Check(june.pb.ve.current_volume == ExpectedVolume(39),
          "a build of June 2008 scales the velocity (" + std::to_string(june.pb.ve.current_volume) + ")");
}

void TestEnvReset()
{
    // The slowest attack, then the bank's envelope again from June 2008. The bank's attack is instant.
    const test::Bytes commands = {0xd0, 0x00, 0xfb, 0x3c, 0x7f, 0x30, 0xff};
    const Started march(commands, kBuildMarch2008);
    const Started june(commands, kBuildJune2008);

    Check(march.pb.ve.current_volume < ExpectedVolume(127) / 2, "a build of March 2008 keeps the track's attack");
    Check(june.pb.ve.current_volume == ExpectedVolume(127), "a build of June 2008 takes the bank's attack again");
}

// Whether `pb`'s biquad filter is on with the coefficients in `row` of a table.
bool BiquadIs(const ax::Pb& pb, const uint16_t* row)
{
    return pb.biquad.on == ax::kBiquadOn && pb.biquad.b0 == row[0] && pb.biquad.b1 == row[1] &&
           pb.biquad.b2 == row[2] && pb.biquad.a1 == row[3] && pb.biquad.a2 == row[4];
}

void TestBiquad()
{
    // The high-pass filter (type 2) at 90 over 127 under a note of 96 ticks that doesn't wait, then the value 0 at tick
    // 24 and no filter at tick 48. At 90, the filter takes row 96 × 90 / 127 = 68 of its table.
    const test::Bytes commands = {0xc7, 0x00, 0xb4, 0x02, 0xb5, 0x5a, 0x3c, 0x64, 0x60, 0x80,
                                  0x18, 0xb5, 0x00, 0x80, 0x18, 0xb4, 0x00, 0x80, 0x18, 0xff};

    const Started march(commands, kBuildMarch2008);
    Started june(commands, kBuildJune2008);
    const ax::Pb at_0 = june.PbAfter(140);
    const ax::Pb off = june.PbAfter(90);

    Check(march.pb.biquad.on == 0, "a build of March 2008 ignores the biquad filter");
    Check(BiquadIs(june.pb, &nw4r::tables::kBiquadHpf[68 * 5]), "a build of June 2008 filters the note");
    Check(BiquadIs(at_0, &nw4r::tables::kBiquadHpf[0]), "the filter's value changes under the note");
    Check(off.biquad.on == 0 && off.state == 1, "type 0 turns the filter off under the note");

    // The band-passes take their rows at value × (2 - value). A type without a preset has no filter.
    const std::optional<std::array<uint16_t, 5>> band_pass = nw4r::BiquadFilterCoefs(3, 0.5f);

    Check(band_pass && (*band_pass)[0] == nw4r::tables::kBiquadBpf512[90 * 5] &&
              (*band_pass)[4] == nw4r::tables::kBiquadBpf512[90 * 5 + 4],
          "a band-pass at 0.5 takes row 121 × 0.75");
    Check(!nw4r::BiquadFilterCoefs(0, 0.5f) && !nw4r::BiquadFilterCoefs(6, 0.5f) && !nw4r::BiquadFilterCoefs(64, 0.5f),
          "types 0, 6 and 64 have no coefficients");
}

void TestIgnoreNoteOff()
{
    // The fastest release, a note of 4 ticks on instrument 6 (whose region ignores note-offs) and a long wait. A 2008
    // build keeps the note when its length runs out. A 2007 build releases it.
    const test::Bytes commands = {0xd3, 0x7f, 0x81, 0x06, 0x3c, 0x64, 0x04, 0x80, 0x60, 0xff};
    Started older(commands, kBuild2007);
    Started newer(commands, kBuildMarch2008);

    for (int i = 0; i < 40; i++)
    {
        older.model.RunFrame();
        newer.model.RunFrame();
    }

    Check(older.model.engine.Channels().ActiveCount() == 0, "a 2007 build releases the note at its end");
    Check(newer.model.engine.Channels().ActiveCount() == 1, "a 2008 build lets the note ignore its note-off");
}

void TestKeyGroup()
{
    // The fastest release, no note wait, and two long notes at once on instrument 7, in key group 1. In a 2008 build,
    // the second note releases the first.
    const test::Bytes commands = {0xd3, 0x7f, 0xc7, 0x00, 0x81, 0x07, 0x3c, 0x64,
                                  0x60, 0x40, 0x64, 0x60, 0x80, 0x60, 0xff};
    Started older(commands, kBuild2007);
    Started newer(commands, kBuildMarch2008);

    older.model.RunFrame();
    newer.model.RunFrame();

    Check(older.model.engine.Channels().ActiveCount() == 2, "a 2007 build plays both notes");
    Check(newer.model.engine.Channels().ActiveCount() == 1, "a 2008 build releases the key group's first note");
}

// The number of times a sequence of `commands`, with tracks 0 and 1, reports a loop to its player in 400 frames (1.2
// s).
int LoopsOf(const test::Bytes& commands)
{
    Model model(commands, {}, 3);
    nw4r::SeqPlayer* player = model.engine.StartSequence(0);
    int loops = 0;
    player->on_loop = [&loops]
    {
        loops++;
    };

    for (int i = 0; i < 400; i++)
    {
        model.RunFrame();
    }

    return loops;
}

void TestLoops()
{
    // A wait of 12 ticks (125 ms) repeated forever on track 0: by an endless loop, by a jump back, and by a jump back
    // inside a subroutine. A subroutine that draws a random value until it differs from the last jumps back that way.
    const int endless = LoopsOf({0xd4, 0x00, 0x80, 0x0c, 0xfc, 0xff});
    const int jump = LoopsOf({0x80, 0x0c, 0x89, 0x00, 0x00, 0x00});
    const int in_call = LoopsOf({0x8a, 0x00, 0x00, 0x05, 0xff, 0x80, 0x0c, 0x89, 0x00, 0x00, 0x05});

    // Track 0 opens track 1 and ends. Track 1 repeats a note of 12 ticks, or a wait of 12 ticks, forever.
    const int noted = LoopsOf({0x88, 0x01, 0x00, 0x00, 0x06, 0xff, 0xd4, 0x00, 0x3c, 0x64, 0x0c, 0xfc});
    const int silent = LoopsOf({0x88, 0x01, 0x00, 0x00, 0x06, 0xff, 0xd4, 0x00, 0x80, 0x0c, 0xfc});

    Check(endless >= 8 && jump >= 8, "an endless loop and a jump back on track 0 are loops (" +
                                         std::to_string(endless) + ", " + std::to_string(jump) + ")");
    Check(in_call == 0, "a jump back inside a subroutine isn't a loop");
    Check(noted >= 8,
          "once track 0 has ended, track 1's loops of notes are the sequence's (" + std::to_string(noted) + ")");
    Check(silent == 0, "a loop of waits on track 1 isn't the sequence's");
}

void TestHeldNotes()
{
    // Track 0 waits out a note of 10,000 ticks or of 9,000 ticks. The note of 10,000 ticks is held until the game stops
    // the sound.
    Started held({0x3c, 0x64, 0xce, 0x10, 0xff}, kBuild2007);
    Started long_note({0x3c, 0x64, 0xc6, 0x28, 0xff}, kBuild2007);

    held.RunUntilEnd(10);
    long_note.RunUntilEnd(10);

    // Track 0 plays a note of 10,000 ticks, or of 9,000, without waiting for it. The note decays at once to a full
    // sustain. The track then calls a subroutine that waits 12 ticks and jumps back forever.
    const int holding = LoopsOf({0xd1, 0x7f, 0xd2, 0x7f, 0xc7, 0x00, 0x3c, 0x64, 0xce, 0x10, 0x8a,
                                 0x00, 0x00, 0x0f, 0xff, 0x80, 0x0c, 0x89, 0x00, 0x00, 0x0f});
    const int not_held = LoopsOf({0xd1, 0x7f, 0xd2, 0x7f, 0xc7, 0x00, 0x3c, 0x64, 0xc6, 0x28, 0x8a,
                                  0x00, 0x00, 0x0f, 0xff, 0x80, 0x0c, 0x89, 0x00, 0x00, 0x0f});

    Check(held.player->WaitsForEver() && held.player->WaitsForNotes(),
          "a track that waits out a held note won't read on before the game stops the sound");
    Check(!long_note.player->WaitsForEver(), "a note of 9,000 ticks isn't a held one");
    Check(holding >= 8, "a subroutine that loops for ever while the track holds a note is the sequence's loop (" +
                            std::to_string(holding) + ")");
    Check(not_held == 0, "one that loops while the track holds a note of 9,000 ticks isn't");
}

// Runs `frames` frames. Returns the PBs that each frame hands the microcode, one frame after another.
std::vector<uint8_t> PbsOfFrames(Model& model, int frames)
{
    constexpr uint32_t kSize = ax::kVoiceCount * 0x140;
    std::vector<uint8_t> pbs;
    for (int i = 0; i < frames; i++)
    {
        model.RunFrame();
        const uint8_t* bytes = model.memory.At(ax::layout::kPbs, kSize);
        pbs.insert(pbs.end(), bytes, bytes + kSize);
    }

    return pbs;
}

void TestSnapshot()
{
    // Notes of different lengths and a release of 64. At the snapshot, one note is releasing, one is sounding and one
    // is still to come.
    Model model({0xd3, 0x40, 0x3c, 0x64, 0x30, 0x40, 0x64, 0x18, 0x43, 0x50, 0x60, 0xff});
    Check(model.engine.StartSequence(0) != nullptr, "the sequence for the snapshot starts");
    PbsOfFrames(model, 200);

    // Everything the frames change: the memory with its PBs, AX, and the engine with its players.
    const MainMemory::Snapshot memory = model.memory.Save();
    const ax::System ax = model.ax;
    const nw4r::Engine::Snapshot engine = model.engine.Save();
    const std::vector<uint8_t> first = PbsOfFrames(model, 300);
    const bool busy = model.engine.IsBusy();

    model.memory.Restore(memory);
    model.ax = ax;
    model.engine.Restore(engine);
    const std::vector<uint8_t> second = PbsOfFrames(model, 300);

    Check(first == second && model.engine.IsBusy() == busy, "the frames after a restored snapshot are the same");

    // A sequence started after the snapshot can't be undone.
    model.engine.StartSequence(0);
    bool refused = false;
    try
    {
        model.engine.Restore(engine);
    }
    catch (const std::runtime_error&)
    {
        refused = true;
    }

    Check(refused, "a snapshot from before another sequence started is refused");
}

void TestWaveEndWithoutDsp()
{
    // A note of 48 ticks (167 frames) on a wave that doesn't loop, played on the model without the DSP. Its channel
    // stops once the wave has played to its end, a frame or two in. The microcode would stop it at the same point.
    const rsar::SoundArchive archive =
        rsar::SoundArchive::Load(test::MakeArchive(test::MakeSequence({0x3c, 0x64, 0x30, 0xff}), 0, 1, false));
    auto model = std::make_unique<nw4r::ArchiveModel>();
    model->StartWithoutDsp(archive);
    Check(model->engine->StartSequence(0) != nullptr, "the sequence without the DSP starts");
    model->RunFrameWithoutDsp();
    Check(model->engine->Channels().ActiveCount() == 1, "the note starts");

    int frames = 1;
    while (model->engine->Channels().ActiveCount() > 0 && frames < 200)
    {
        model->RunFrameWithoutDsp();
        frames++;
    }

    Check(frames <= 3, "the note's channel stops when its wave ends (after " + std::to_string(frames) + " frames)");
}

} // namespace

int main()
{
    try
    {
        TestNote();
        TestRelease();
        TestLastStep();
        TestSndBuild();
        TestDrop();
        TestSequence();
        TestSweepPitch();
        TestTempo();
        TestVelocityRange();
        TestEnvReset();
        TestBiquad();
        TestIgnoreNoteOff();
        TestKeyGroup();
        TestLoops();
        TestHeldNotes();
        TestSnapshot();
        TestWaveEndWithoutDsp();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "exception: %s\n", e.what());

        return 1;
    }

    return failures == 0 ? 0 : 1;
}
