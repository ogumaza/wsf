// SPDX-License-Identifier: MIT

#include "nw4r/driver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ax/pb.h"
#include "ax/system.h"
#include "ax/voice.h"
#include "nw4r/engine.h"
#include "nw4r/util.h"

namespace wsf::nw4r
{
namespace
{

// The AX priorities that NW4R SND gives its voices: the highest for a voice of priority 255, one above the middle for
// the rest, and the middle for a voice in its release. A voice that must drop another to get its AX voice requests one
// more than usual.
constexpr uint32_t kAxPriorityTop = ax::kPriorityMax;
constexpr uint32_t kAxPriorityNormal = ax::kPriorityMax / 2 + 1;
constexpr uint32_t kAxPriorityRelease = ax::kPriorityMax / 2;
constexpr uint32_t kAxPriorityDrop = ax::kPriorityMax / 2 + 2;

// The zero buffer's length in the units of each format: 256 bytes.
uint32_t ZeroBufferUnits(ax::SampleFormat format)
{
    switch (format)
    {
    case ax::SampleFormat::kAdpcm:
        return 0x200;
    case ax::SampleFormat::kPcm8:
        return 0x100;
    default:
        return 0x80;
    }
}

ax::SampleFormat ToAxFormat(rsar::WaveFormat format)
{
    switch (format)
    {
    case rsar::WaveFormat::kPcm16:
        return ax::SampleFormat::kPcm16;
    case rsar::WaveFormat::kPcm8:
        return ax::SampleFormat::kPcm8;
    default:
        return ax::SampleFormat::kAdpcm;
    }
}

// The ramp that moves a mix volume from `from` to `to` over a frame (CalcAxvpbDelta).
int MixDelta(uint16_t from, uint16_t to)
{
    return (to - from) / ax::kSamplesPerFrame;
}

// Converts a float to an unsigned integer like the game's runtime (__cvt_fp2unsigned, 0x800b1c1c): 0 below 0, all ones
// from 2^32 (and for NaN), and truncated between.
uint32_t ToUnsigned(float value)
{
    if (value < 0.0f)
    {
        return 0;
    }
    if (!(value < 4294967296.0f))
    {
        return 0xffffffff;
    }
    if (value < 2147483648.0f)
    {
        return static_cast<uint32_t>(static_cast<int32_t>(value));
    }

    return static_cast<uint32_t>(static_cast<int32_t>(value - 2147483648.0f)) + 0x80000000u;
}

// A mix gain as AX's volume (CalcMixVolume). In AX's volume, 0x8000 is 1.
uint16_t MixVolume(float volume)
{
    return static_cast<uint16_t>(std::min<uint32_t>(0xffff, ToUnsigned(static_cast<float>(ax::kMaxVolume) * volume)));
}

// The pan curves of the sound archive (PanCurve) and how Util computes each.
PanInfo PanInfoFor(uint8_t curve)
{
    switch (curve)
    {
    case 1:
        return {PanCurve::kSqrt, true, false};
    case 2:
        return {PanCurve::kSqrt, true, true};
    case 3:
        return {PanCurve::kSinCos, false, false};
    case 4:
        return {PanCurve::kSinCos, true, false};
    case 5:
        return {PanCurve::kSinCos, true, true};
    case 6:
        return {PanCurve::kLinear, false, false};
    case 7:
        return {PanCurve::kLinear, true, false};
    case 8:
        return {PanCurve::kLinear, true, true};
    default:
        return {PanCurve::kSqrt, false, false};
    }
}

// The ADPCM decoder's step for one nibble (DecodeDspAdpcm, 0x800505c0), on the PB's ADPCM block.
void DecodeDspAdpcm(ax::PbAdpcm& adpcm, uint8_t nibble)
{
    const auto yn1 = static_cast<int16_t>(adpcm.yn1);
    const auto yn2 = static_cast<int16_t>(adpcm.yn2);
    const auto scale = static_cast<int16_t>(1 << (adpcm.pred_scale & 0xf));
    const auto bits = static_cast<int16_t>(nibble << 12);
    const int index = adpcm.pred_scale >> 4;
    const auto coef0 = static_cast<int16_t>(adpcm.a[index * 2]);
    const auto coef1 = static_cast<int16_t>(adpcm.a[index * 2 + 1]);

    int32_t sample = coef0 * yn1;
    sample += coef1 * yn2;
    sample += scale * (bits >> 1);
    sample >>= 10;
    sample += 1;
    sample >>= 1;
    sample = std::clamp(sample, -32768, 32767);

    adpcm.yn2 = adpcm.yn1;
    adpcm.yn1 = static_cast<uint16_t>(sample);
}

} // namespace

void AxVoice::Setup(PAddr wave, ax::SampleFormat format, int sample_rate)
{
    wave_data_ = wave;
    format_ = format;
    sample_rate_ = sample_rate;
    mix_prev_ = MixParam{};
    first_mix_update_ = true;
}

bool AxVoice::IsPlayFinished() const
{
    if (wave_data_ == 0)
    {
        return false;
    }

    const uint32_t address =
        IsAvailable() ? ax::Join(vpb_->pb.addr.current_address_hi, vpb_->pb.addr.current_address_lo) : 0;
    const uint32_t begin = DspAddressBySample(engine_->ZeroBuffer(), 0, format_);

    return begin <= address && address < begin + ZeroBufferUnits(format_);
}

void AxVoice::SetAddr(bool loop, PAddr wave, uint32_t offset, uint32_t loop_start, uint32_t loop_end)
{
    if (!IsAvailable())
    {
        return;
    }

    // Starting past the end plays a sample of silence. A wave that doesn't loop continues into the zero buffer. The
    // microcode stops it there.
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t loop_address = 0;
    const PAddr zero = engine_->ZeroBuffer();
    if (offset > loop_end)
    {
        loop = false;
        start = DspAddressBySample(zero, 0, format_);
        loop_address = DspAddressBySample(zero, 0, format_);
        end = DspAddressBySample(zero, 1, format_);
    }
    else
    {
        loop_address = loop ? DspAddressBySample(wave, loop_start, format_) : DspAddressBySample(zero, 0, format_);
        start = DspAddressBySample(wave, offset, format_);
        end = DspAddressBySample(wave, loop_end - 1, format_);
    }

    ax::PbAddr address;
    address.loop_flag = loop ? 1 : 0;
    address.format = static_cast<uint16_t>(format_);
    address.loop_address_hi = static_cast<uint16_t>(loop_address >> 16);
    address.loop_address_lo = static_cast<uint16_t>(loop_address);
    address.end_address_hi = static_cast<uint16_t>(end >> 16);
    address.end_address_lo = static_cast<uint16_t>(end);
    address.current_address_hi = static_cast<uint16_t>(start >> 16);
    address.current_address_lo = static_cast<uint16_t>(start);
    vpb_->SetAddr(address);
}

void AxVoice::SetSrcType(SrcType type, float pitch)
{
    if (!IsAvailable())
    {
        return;
    }

    // The automatic choice takes the coefficients for the lowest cutoff that the rate requires. The more the wave is
    // sped up, the lower the cutoff.
    if (type == SrcType::k4TapAuto)
    {
        const float ratio = pitch * static_cast<float>(sample_rate_) / static_cast<float>(ax::kSampleRate);
        if (ratio > 4.0f / 3.0f)
        {
            type = SrcType::k4Tap8k;
        }
        else if (ratio > 1.0f)
        {
            type = SrcType::k4Tap12k;
        }
        else
        {
            type = SrcType::k4Tap16k;
        }
    }

    // AxVoiceParamBlock::SetVoiceSrcType (0x80034af8).
    ax::Pb& pb = vpb_->pb;
    switch (type)
    {
    case SrcType::kNone:
        pb.src_select = 2;
        break;

    case SrcType::kLinear:
        pb.src_select = 1;
        break;

    case SrcType::k4Tap8k:
        pb.src_select = 0;
        pb.coef_select = 0;
        break;

    case SrcType::k4Tap12k:
        pb.src_select = 0;
        pb.coef_select = 1;
        break;

    case SrcType::k4Tap16k:
        pb.src_select = 0;
        pb.coef_select = 2;
        break;

    default:
        break;
    }

    sync_ |= ax::sync::kSelect;
}

void AxVoice::SetAdpcm(const rsar::AdpcmInfo& info, uint16_t pred_scale, int16_t yn1, int16_t yn2)
{
    if (!IsAvailable())
    {
        return;
    }

    // PCM16 and PCM8 decode with no prediction and a gain that scales them to 16 bits.
    ax::PbAdpcm adpcm;
    switch (format_)
    {
    case ax::SampleFormat::kAdpcm:
        for (int i = 0; i < 16; i++)
        {
            adpcm.a[i] = static_cast<uint16_t>(info.coefs[i]);
        }

        adpcm.gain = info.gain;
        adpcm.pred_scale = pred_scale;
        adpcm.yn1 = static_cast<uint16_t>(yn1);
        adpcm.yn2 = static_cast<uint16_t>(yn2);
        break;

    case ax::SampleFormat::kPcm16:
        adpcm.gain = 0x800;
        break;

    case ax::SampleFormat::kPcm8:
        adpcm.gain = 0x100;
        break;
    }

    vpb_->pb.adpcm = adpcm;
    sync_ |= ax::sync::kAdpcm;
}

void AxVoice::SetAdpcmLoop(const rsar::AdpcmInfo& info)
{
    if (!IsAvailable())
    {
        return;
    }

    ax::PbAdpcmLoop loop;
    if (format_ == ax::SampleFormat::kAdpcm)
    {
        loop.loop_pred_scale = info.loop_pred_scale;
        loop.loop_yn1 = static_cast<uint16_t>(info.loop_yn1);
        loop.loop_yn2 = static_cast<uint16_t>(info.loop_yn2);
    }

    vpb_->pb.adpcm_loop = loop;
    sync_ |= ax::sync::kAdpcmLoop;
}

bool AxVoice::SetMix(const MixParam& mix)
{
    if (!IsAvailable())
    {
        return false;
    }

    // The build of October 2006 sets the mix at once, with no ramp (CalcMixParam, 0x800dd970 in Smooth Moves' main.dol,
    // and AXSetVoiceMix).
    if (engine_->Build().October2006())
    {
        ax::PbMix pb_mix;
        pb_mix.l = mix.l;
        pb_mix.r = mix.r;
        pb_mix.s = mix.s;
        pb_mix.aux_a_l = mix.aux_a_l;
        pb_mix.aux_a_r = mix.aux_a_r;
        pb_mix.aux_a_s = mix.aux_a_s;
        pb_mix.aux_b_l = mix.aux_b_l;
        pb_mix.aux_b_r = mix.aux_b_r;
        pb_mix.aux_b_s = mix.aux_b_s;
        pb_mix.aux_c_l = mix.aux_c_l;
        pb_mix.aux_c_r = mix.aux_c_r;
        pb_mix.aux_c_s = mix.aux_c_s;
        SetVoiceMix(pb_mix, true);

        return false;
    }

    // A voice that hasn't started jumps straight to its mix. A playing voice ramps from where it is.
    if (first_mix_update_ || !IsRun())
    {
        mix_prev_ = mix;
        first_mix_update_ = false;
    }

    const bool need_update = mix_prev_ != mix;

    // Each volume starts where the last frame's ramp left it. The ramp moves it towards the target over the frame. A
    // volume reaches its target when the ramp would overshoot, or when the target is 0.
    const auto ramp = [](uint16_t& prev, uint16_t target, uint16_t& volume, uint16_t& delta)
    {
        volume = prev;
        const int step = MixDelta(prev, target);
        delta = static_cast<uint16_t>(step);
        if (target == 0 || step == 0)
        {
            prev = target;
        }
        else
        {
            prev = static_cast<uint16_t>(prev + step * ax::kSamplesPerFrame);
        }
    };

    ax::PbMix pb_mix;
    ramp(mix_prev_.l, mix.l, pb_mix.l, pb_mix.delta_l);
    ramp(mix_prev_.r, mix.r, pb_mix.r, pb_mix.delta_r);
    ramp(mix_prev_.s, mix.s, pb_mix.s, pb_mix.delta_s);
    ramp(mix_prev_.aux_a_l, mix.aux_a_l, pb_mix.aux_a_l, pb_mix.delta_aux_a_l);
    ramp(mix_prev_.aux_a_r, mix.aux_a_r, pb_mix.aux_a_r, pb_mix.delta_aux_a_r);
    ramp(mix_prev_.aux_a_s, mix.aux_a_s, pb_mix.aux_a_s, pb_mix.delta_aux_a_s);
    ramp(mix_prev_.aux_b_l, mix.aux_b_l, pb_mix.aux_b_l, pb_mix.delta_aux_b_l);
    ramp(mix_prev_.aux_b_r, mix.aux_b_r, pb_mix.aux_b_r, pb_mix.delta_aux_b_r);
    ramp(mix_prev_.aux_b_s, mix.aux_b_s, pb_mix.aux_b_s, pb_mix.delta_aux_b_s);
    ramp(mix_prev_.aux_c_l, mix.aux_c_l, pb_mix.aux_c_l, pb_mix.delta_aux_c_l);
    ramp(mix_prev_.aux_c_r, mix.aux_c_r, pb_mix.aux_c_r, pb_mix.delta_aux_c_r);
    ramp(mix_prev_.aux_c_s, mix.aux_c_s, pb_mix.aux_c_s, pb_mix.delta_aux_c_s);
    SetVoiceMix(pb_mix, false);

    return need_update;
}

void AxVoice::SetSrc(float ratio, bool initial)
{
    if (!IsAvailable())
    {
        return;
    }

    // The ratio to AX's 32 kHz, as 16.16 fixed point. A voice that starts also starts its converter from rest.
    const float dsp_ratio = ratio * static_cast<float>(sample_rate_) / static_cast<float>(ax::kSampleRate);
    const uint32_t bits = ToUnsigned(65536.0f * dsp_ratio);
    ax::Pb& pb = vpb_->pb;
    if (initial)
    {
        pb.src = ax::PbSrc{};
        pb.src.ratio_hi = static_cast<uint16_t>(bits >> 16);
        pb.src.ratio_lo = static_cast<uint16_t>(bits);
        sync_ &= ~ax::sync::kSrcRatio;
        sync_ |= ax::sync::kSrc;
    }
    else
    {
        pb.src.ratio_hi = static_cast<uint16_t>(bits >> 16);
        pb.src.ratio_lo = static_cast<uint16_t>(bits);
        if (!(sync_ & ax::sync::kSrc))
        {
            sync_ |= ax::sync::kSrcRatio;
        }
    }
}

void AxVoice::SetVe(float volume, float init_volume)
{
    if (!IsAvailable())
    {
        return;
    }

    // AxVoiceParamBlock::SetVoiceVe: the first call also sets where the envelope starts.
    if (first_ve_update_)
    {
        prev_volume_ = static_cast<uint16_t>(static_cast<int32_t>(32767.0f * init_volume));
        first_ve_update_ = false;
    }

    volume_ = static_cast<uint16_t>(static_cast<int32_t>(32767.0f * volume));
}

void AxVoice::SetVeOctober2006(uint16_t volume, int16_t delta)
{
    if (!IsAvailable())
    {
        return;
    }

    vpb_->pb.ve = {volume, static_cast<uint16_t>(delta)};
    vpb_->sync |= ax::sync::kVe;
}

void AxVoice::SetLpf(uint16_t freq)
{
    if (!IsAvailable())
    {
        return;
    }

    // Off, new coefficients for a filter that's on, or on from rest. The game turns the filter off with a block whose
    // coefficients it leaves unset. The microcode ignores them while the filter is off. The model writes zeros.
    ax::Pb& pb = vpb_->pb;
    if (freq >= 16000)
    {
        pb.lpf = ax::PbLpf{};
        sync_ |= ax::sync::kLpf;
    }
    else if (pb.lpf.on == ax::kLpfOn)
    {
        const ax::LpfCoefs coefs = ax::CalcLpfCoefs(freq);
        pb.lpf.a0 = coefs.a0;
        pb.lpf.b0 = coefs.b0;
        sync_ |= ax::sync::kLpfCoefs;
    }
    else
    {
        const ax::LpfCoefs coefs = ax::CalcLpfCoefs(freq);
        pb.lpf = {ax::kLpfOn, 0, coefs.a0, coefs.b0};
        sync_ |= ax::sync::kLpf;
    }
}

void AxVoice::SetBiquad(int type, float value)
{
    if (!IsAvailable())
    {
        return;
    }

    // Off, new coefficients for a filter that's on, or on from rest, as for the low-pass filter. The game's block for
    // off leaves the coefficients unset. The model writes zeros.
    const std::optional<std::array<uint16_t, 5>> coefs = BiquadFilterCoefs(type, value);
    ax::Pb& pb = vpb_->pb;
    if (!coefs)
    {
        pb.biquad = ax::PbBiquad{};
        sync_ |= ax::sync::kBiquad;
    }
    else if (pb.biquad.on == ax::kBiquadOn)
    {
        pb.biquad.b0 = (*coefs)[0];
        pb.biquad.b1 = (*coefs)[1];
        pb.biquad.b2 = (*coefs)[2];
        pb.biquad.a1 = (*coefs)[3];
        pb.biquad.a2 = (*coefs)[4];
        sync_ |= ax::sync::kBiquadCoefs;
    }
    else
    {
        pb.biquad = {ax::kBiquadOn, 0, 0, 0, 0, (*coefs)[0], (*coefs)[1], (*coefs)[2], (*coefs)[3], (*coefs)[4]};
        sync_ |= ax::sync::kBiquad;
    }
}

void AxVoice::SetPriority(uint32_t priority)
{
    if (IsAvailable())
    {
        engine_->Ax().SetVoicePriority(vpb_, priority);
    }
}

void AxVoice::SetVoiceType(uint16_t type)
{
    if (!IsAvailable())
    {
        return;
    }

    vpb_->pb.type = type;
    sync_ |= ax::sync::kType;
}

void AxVoice::EnableRemote(bool enable)
{
    if (!IsAvailable())
    {
        return;
    }

    vpb_->pb.remote = enable ? 1 : 0;
    sync_ |= ax::sync::kRemote;
}

void AxVoice::Sync()
{
    if (!IsAvailable())
    {
        return;
    }

    // The build of October 2006 sets the envelope in Voice::CalcAxVe. Only the other changes go to AX here.
    if (engine_->Build().October2006())
    {
        vpb_->sync |= sync_;
        sync_ = 0;

        return;
    }

    // The envelope starts the frame at the volume that the model computed for the end of the last frame. It ramps by a
    // whole step per sample towards the target. The step is rounded towards whichever value comes closer. The game's
    // alternative step is 1, or -1 when the step is 0.
    ax::PbVe& ve = vpb_->pb.ve;
    ve.current_volume = prev_volume_;
    const auto delta_in = static_cast<int16_t>((volume_ - prev_volume_) / ax::kSamplesPerFrame);
    const int16_t delta_out = delta_in + (delta_in > 0 ? 1 : 0) != 0 ? 1 : -1;
    const int predicted_in = prev_volume_ + delta_in * ax::kSamplesPerFrame;
    const int predicted_out = prev_volume_ + delta_out * ax::kSamplesPerFrame;
    int16_t delta = std::abs(volume_ - predicted_in) < std::abs(volume_ - predicted_out) ? delta_in : delta_out;

    // A step of 0, after one, lands on the target at once. The ramp mustn't take the volume out of 0 to 32767 by the
    // end of the frame. Builds from 2008 keep the step in range first (Mario Kart Wii, AxVoiceParamBlock::Sync,
    // 0x8008bd60). A step that the range cuts to 0 then lands on the target too. A releasing note's last frame shows
    // this.
    const auto land = [&]
    {
        if (delta == 0 && prev_delta_ == 0)
        {
            ve.current_volume = volume_;
        }
    };

    const auto keep_in_range = [&]
    {
        const int next_volume = prev_volume_ + delta * ax::kSamplesPerFrame;
        if (next_volume < 0)
        {
            delta = static_cast<int16_t>(-prev_volume_ / ax::kSamplesPerFrame);
        }
        else if (next_volume > 32767)
        {
            delta = static_cast<int16_t>((32767 - prev_volume_) / ax::kSamplesPerFrame);
        }
    };

    if (engine_->Build().Since2008())
    {
        keep_in_range();
        land();
    }
    else
    {
        land();
        keep_in_range();
    }

    ve.current_delta = static_cast<uint16_t>(delta);
    sync_ &= ~ax::sync::kVeDelta;
    sync_ |= ax::sync::kVe;
    prev_volume_ = ve.current_volume;
    prev_delta_ = delta;
    vpb_->sync |= sync_;
    sync_ = 0;
}

void AxVoice::Run()
{
    if (IsAvailable())
    {
        vpb_->SetState(ax::VoiceState::kRun);
    }
}

void AxVoice::Stop()
{
    if (IsRun())
    {
        vpb_->SetState(ax::VoiceState::kStop);
    }
}

uint32_t AxVoice::DspAddressBySample(PAddr base, uint32_t samples, ax::SampleFormat format)
{
    switch (format)
    {
    case ax::SampleFormat::kAdpcm:
        return samples / ax::kAdpcmSamplesPerFrame * ax::kAdpcmNibblesPerFrame + samples % ax::kAdpcmSamplesPerFrame +
               base * 2 + 2;
    case ax::SampleFormat::kPcm8:
        return base + samples;
    case ax::SampleFormat::kPcm16:
        return base / 2 + samples;
    }

    return 0;
}

uint32_t AxVoice::SampleByDspAddress(PAddr base, uint32_t address, ax::SampleFormat format)
{
    switch (format)
    {
    case ax::SampleFormat::kAdpcm:
        {
            const uint32_t nibbles = address - base * 2;

            return nibbles % ax::kAdpcmNibblesPerFrame +
                   nibbles / ax::kAdpcmNibblesPerFrame * ax::kAdpcmSamplesPerFrame - 2;
        }

    case ax::SampleFormat::kPcm8:
        return address - base;

    case ax::SampleFormat::kPcm16:
        return address - base / 2;
    }

    return 0;
}

void AxVoice::CalcOffsetAdpcmParam(const Engine& engine, PAddr data, uint32_t offset, const rsar::AdpcmInfo& info,
                                   uint16_t& pred_scale, int16_t& yn1, int16_t& yn2)
{
    // Decode up to `offset` to find the decoder's state there. Read each frame's header on the way.
    ax::PbAdpcm adpcm;
    for (int i = 0; i < 16; i++)
    {
        adpcm.a[i] = static_cast<uint16_t>(info.coefs[i]);
    }

    adpcm.gain = info.gain;
    adpcm.pred_scale = info.pred_scale;
    adpcm.yn1 = static_cast<uint16_t>(info.yn1);
    adpcm.yn2 = static_cast<uint16_t>(info.yn2);

    uint32_t address = DspAddressBySample(data, 0, ax::SampleFormat::kAdpcm);
    const uint32_t end = DspAddressBySample(data, offset, ax::SampleFormat::kAdpcm);
    while (address < end)
    {
        if (address % ax::kAdpcmNibblesPerFrame == 0)
        {
            adpcm.pred_scale = *engine.Memory().At(address / 2);
            address += 2;
        }

        const uint8_t byte = *engine.Memory().At(address / 2);
        DecodeDspAdpcm(adpcm, address % 2 != 0 ? byte & 0xf : byte >> 4);
        address++;
    }

    pred_scale = adpcm.pred_scale;
    yn1 = static_cast<int16_t>(adpcm.yn1);
    yn2 = static_cast<int16_t>(adpcm.yn2);
}

void AxVoice::SetVoiceMix(const ax::PbMix& mix, bool sync_now)
{
    // AxVoiceParamBlock::SetVoiceMix (0x80034880). The mixer control has a bit for each volume that isn't 0. It also
    // has a ramp bit for each ramp that isn't 0.
    namespace m = ax::mixer;
    uint32_t ctrl = 0;
    const auto flag = [&ctrl](uint16_t value, uint32_t bits)
    {
        if (value != 0)
        {
            ctrl |= bits;
        }
    };

    flag(mix.l, m::kL);
    flag(mix.delta_l, m::kDelta | m::kL);
    flag(mix.r, m::kR);
    flag(mix.delta_r, m::kDelta | m::kR);
    flag(mix.aux_a_l, m::kAuxAL);
    flag(mix.delta_aux_a_l, m::kDeltaA | m::kAuxAL);
    flag(mix.aux_a_r, m::kAuxAR);
    flag(mix.delta_aux_a_r, m::kDeltaA | m::kAuxAR);
    flag(mix.aux_b_l, m::kAuxBL);
    flag(mix.delta_aux_b_l, m::kDeltaB | m::kAuxBL);
    flag(mix.aux_b_r, m::kAuxBR);
    flag(mix.delta_aux_b_r, m::kDeltaB | m::kAuxBR);
    flag(mix.aux_c_l, m::kAuxCL);
    flag(mix.delta_aux_c_l, m::kDeltaC | m::kAuxCL);
    flag(mix.aux_c_r, m::kAuxCR);
    flag(mix.delta_aux_c_r, m::kDeltaC | m::kAuxCR);
    flag(mix.s, m::kS);
    flag(mix.delta_s, m::kDeltaS | m::kS);
    flag(mix.aux_a_s, m::kAuxAS);
    flag(mix.delta_aux_a_s, m::kDeltaAS | m::kAuxAS);
    flag(mix.aux_b_s, m::kAuxBS);
    flag(mix.delta_aux_b_s, m::kDeltaBS | m::kAuxBS);
    flag(mix.aux_c_s, m::kAuxCS);
    flag(mix.delta_aux_c_s, m::kDeltaCS | m::kAuxCS);

    vpb_->pb.mix = mix;
    vpb_->pb.SetMixerCtrl(ctrl);
    if (sync_now)
    {
        vpb_->sync |= ax::sync::kMix | ax::sync::kMixerCtrl;
    }
    else
    {
        sync_ |= ax::sync::kMix | ax::sync::kMixerCtrl;
    }
}

void AxVoice::UpdateDelta()
{
    // AxVoiceParamBlock::UpdateDelta: the envelope ends its ramp where the frame took it.
    if (!IsAvailable())
    {
        return;
    }

    prev_volume_ =
        static_cast<uint16_t>(prev_volume_ + static_cast<int16_t>(vpb_->pb.ve.current_delta) * ax::kSamplesPerFrame);
    vpb_->pb.ve.current_volume = prev_volume_;
    vpb_->pb.ve.current_delta = 0;
    vpb_->sync |= ax::sync::kVe;
}

void AxVoice::ResetDelta()
{
    if (!IsAvailable())
    {
        return;
    }

    // The envelope and the mix stop their ramps where they are.
    UpdateDelta();
    ax::PbMix mix;
    mix.l = mix_prev_.l;
    mix.r = mix_prev_.r;
    mix.aux_a_l = mix_prev_.aux_a_l;
    mix.aux_a_r = mix_prev_.aux_a_r;
    mix.aux_b_l = mix_prev_.aux_b_l;
    mix.aux_b_r = mix_prev_.aux_b_r;
    mix.aux_c_l = mix_prev_.aux_c_l;
    mix.aux_c_r = mix_prev_.aux_c_r;
    mix.s = mix_prev_.s;
    mix.aux_a_s = mix_prev_.aux_a_s;
    mix.aux_b_s = mix_prev_.aux_b_s;
    mix.aux_c_s = mix_prev_.aux_c_s;
    SetVoiceMix(mix, true);
}

AxVoiceManager::AxVoiceManager(Engine& engine) : engine_(engine)
{
    for (std::unique_ptr<AxVoice>& voice : pool_)
    {
        voice = std::make_unique<AxVoice>(engine);
        free_.push_back(voice.get());
    }
}

AxVoiceManager::Snapshot AxVoiceManager::Save() const
{
    Snapshot snapshot{{}, free_, active_, reserved_};
    snapshot.voices.reserve(pool_.size());
    for (const std::unique_ptr<AxVoice>& voice : pool_)
    {
        snapshot.voices.push_back(*voice);
    }

    return snapshot;
}

void AxVoiceManager::Restore(const Snapshot& snapshot)
{
    for (std::size_t i = 0; i < pool_.size(); i++)
    {
        *pool_[i] = snapshot.voices[i];
    }

    free_ = snapshot.free;
    active_ = snapshot.active;
    reserved_ = snapshot.reserved;
}

AxVoice* AxVoiceManager::AcquireAxVoice(uint32_t priority, AxVoice::Callback callback)
{
    AxVoice* voice = Alloc();
    if (voice == nullptr)
    {
        return nullptr;
    }

    // AX calls this when it takes the voice away (AxVoice::VoiceCallback, 0x80032f58).
    const auto on_drop = [this, voice](ax::Voice*)
    {
        voice->vpb_ = nullptr;
        voice->sync_ = 0;
        ReserveForFreeAxVoice(voice);
    };

    ax::Voice* vpb = engine_.Ax().AcquireVoice(priority, on_drop);
    if (vpb == nullptr)
    {
        Free(voice);

        return nullptr;
    }

    // AxVoiceParamBlock::Set (0x80034854).
    voice->vpb_ = vpb;
    voice->sync_ = 0;
    voice->first_ve_update_ = true;
    voice->volume_ = ax::kMaxVolume;
    voice->prev_volume_ = ax::kMaxVolume;
    voice->prev_delta_ = 0;
    voice->callback_ = std::move(callback);

    return voice;
}

void AxVoiceManager::FreeAxVoice(AxVoice* voice)
{
    if (voice->IsAvailable())
    {
        engine_.Ax().FreeVoice(voice->vpb_);
    }

    Free(voice);
}

void AxVoiceManager::ReserveForFreeAxVoice(AxVoice* voice)
{
    voice->reserve_for_free_ = true;
    active_.remove(voice);
    reserved_.push_back(voice);
}

void AxVoiceManager::FreeAllReservedAxVoice()
{
    while (!reserved_.empty())
    {
        AxVoice* voice = reserved_.front();
        if (voice->callback_)
        {
            const AxVoice::Callback callback = voice->callback_;
            callback(voice, AxVoice::CallbackStatus::kDropDsp);
        }

        FreeAxVoice(voice);
    }
}

AxVoice* AxVoiceManager::Alloc()
{
    // AX reuses its voices as soon as it drops them. Free the AxVoices of the dropped voices first.
    FreeAllReservedAxVoice();
    if (free_.empty())
    {
        return nullptr;
    }

    AxVoice* voice = free_.front();
    free_.pop_front();
    *voice = AxVoice(engine_);
    active_.push_back(voice);

    return voice;
}

void AxVoiceManager::Free(AxVoice* voice)
{
    // The game reads the reserve flag after destroying the voice. Destroying the voice leaves the flag unchanged.
    if (voice->reserve_for_free_)
    {
        reserved_.remove(voice);
    }
    else
    {
        active_.remove(voice);
    }

    free_.push_back(voice);
}

bool Voice::Acquire(int channels, int priority, Callback callback)
{
    channels = std::clamp(channels, 1, 2);

    // Take an AX voice for each channel. When AX has none free, check whether dropping NW4R SND voices of no higher
    // priority would free enough. Then request an AX voice at a priority high enough to steal one.
    const uint32_t ax_priority = priority == kPriorityMax ? kAxPriorityTop : kAxPriorityNormal;
    AxVoiceManager& manager = engine_->AxVoices();
    std::array<AxVoice*, 2> table{};
    const auto on_event = [this](AxVoice* dropped, AxVoice::CallbackStatus status)
    {
        OnAxVoiceEvent(dropped, status);
    };

    for (int i = 0; i < channels; i++)
    {
        AxVoice* voice = manager.AcquireAxVoice(ax_priority, on_event);
        if (voice == nullptr)
        {
            int rest = channels - i;
            for (const Voice* other : engine_->Voices().PrioVoices())
            {
                if (priority < other->Priority())
                {
                    break;
                }

                rest -= other->AxVoiceCount();
                if (rest <= 0)
                {
                    break;
                }
            }

            if (rest > 0)
            {
                for (int j = 0; j < i; j++)
                {
                    manager.FreeAxVoice(table[j]);
                }

                return false;
            }

            voice = manager.AcquireAxVoice(ax_priority == kAxPriorityTop ? kAxPriorityTop : kAxPriorityDrop, on_event);
        }

        if (voice == nullptr)
        {
            for (int j = 0; j < i; j++)
            {
                manager.FreeAxVoice(table[j]);
            }

            return false;
        }

        table[i] = voice;
    }

    for (int i = 0; i < channels; i++)
    {
        table[i]->SetPriority(ax_priority);
        ax_voices_[i] = table[i];
    }

    // InitParam (0x8004ba80).
    channel_count_ = channels;
    callback_ = std::move(callback);
    sync_flag_ = 0;
    pause_ = false;
    started_ = false;
    volume_ = 1.0f;
    ve_init_volume_ = 0.0f;
    ve_target_volume_ = 1.0f;
    lpf_freq_ = 1.0f;
    biquad_type_ = 0;
    biquad_value_ = 0.0f;
    pan_ = 0.0f;
    surround_pan_ = 0.0f;
    main_out_volume_ = 1.0f;
    main_send_ = 1.0f;
    fx_send_ = {};
    pitch_ = 1.0f;
    pan_mode_ = PanMode::kDual;
    pan_curve_ = 0;
    first_ve_update_ = true;
    prev_ve_volume_ = 1.0f;
    active_ = true;

    return true;
}

void Voice::Free()
{
    if (!active_)
    {
        return;
    }

    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            engine_->AxVoices().FreeAxVoice(ax_voices_[i]);
            ax_voices_[i] = nullptr;
        }
    }

    channel_count_ = 0;
    engine_->Voices().FreeVoice(this);
    active_ = false;
}

void Voice::StopFinished()
{
    if (active_ && started_ && IsPlayFinished())
    {
        if (callback_)
        {
            const Callback callback = callback_;
            callback(this, Status::kFinishWave);
        }

        started_ = false;
        starting_ = false;
    }
}

void Voice::Calc()
{
    if (!starting_)
    {
        return;
    }

    if (sync_flag_ & kSyncSrc)
    {
        CalcAxSrc(false);
        sync_flag_ &= ~kSyncSrc;
    }

    if (sync_flag_ & kSyncVe)
    {
        if (!CalcAxVe())
        {
            sync_flag_ &= ~kSyncVe;
        }
    }

    if (sync_flag_ & kSyncMix)
    {
        if (!CalcAxMix())
        {
            sync_flag_ &= ~kSyncMix;
        }
    }

    if (sync_flag_ & kSyncLpf)
    {
        CalcAxLpf();
        sync_flag_ &= ~kSyncLpf;
    }

    if (sync_flag_ & kSyncBiquad)
    {
        CalcAxBiquadFilter();
        sync_flag_ &= ~kSyncBiquad;
    }
}

void Voice::Update()
{
    if (!active_)
    {
        return;
    }

    // A voice starts with its converter at rest. It runs from this frame.
    if ((sync_flag_ & kSyncSrcInitial) && starting_ && !started_)
    {
        CalcAxSrc(true);
        RunAllAxVoice();
        started_ = true;
        sync_flag_ &= ~(kSyncSrcInitial | kSyncSrc);
    }

    if (started_)
    {
        if ((sync_flag_ & kSyncVoice) && starting_)
        {
            if (pause_)
            {
                StopAllAxVoice();
            }
            else
            {
                RunAllAxVoice();
            }

            sync_flag_ &= ~kSyncVoice;
        }

        SyncAxVoice();
    }
}

void Voice::Setup(const WaveData& data, uint32_t offset)
{
    const ax::SampleFormat format = ToAxFormat(data.format);
    for (int i = 0; i < channel_count_; i++)
    {
        AxVoice* voice = ax_voices_[i];
        if (voice == nullptr)
        {
            continue;
        }

        // A start part way into an ADPCM wave requires the decoder's state there.
        const WaveData::Channel& channel = data.channels[i];
        uint16_t pred_scale = channel.adpcm.pred_scale;
        int16_t yn1 = channel.adpcm.yn1;
        int16_t yn2 = channel.adpcm.yn2;
        if (format == ax::SampleFormat::kAdpcm)
        {
            AxVoice::CalcOffsetAdpcmParam(*engine_, channel.data, offset, channel.adpcm, pred_scale, yn1, yn2);
        }

        voice->Setup(channel.data, format, static_cast<int>(data.sample_rate));
        voice->SetAddr(data.loop, channel.data, offset, data.loop_start, data.loop_end);
        if (format == ax::SampleFormat::kAdpcm)
        {
            voice->SetAdpcm(channel.adpcm, pred_scale, yn1, yn2);
            voice->SetAdpcmLoop(channel.adpcm);
        }

        voice->SetSrcType(AxVoice::SrcType::k4TapAuto, pitch_);
        voice->SetVoiceType(0);
    }

    pause_ = false;
    starting_ = false;
    started_ = false;
    sync_flag_ |= kSyncLpf | kSyncMix | kSyncVe;
}

void Voice::Start()
{
    starting_ = true;
    pause_ = false;
    sync_flag_ |= kSyncSrcInitial;
}

void Voice::Stop()
{
    if (started_)
    {
        StopAllAxVoice();
        started_ = false;
    }

    pause_ = false;
    starting_ = false;
}

void Voice::SetVolume(float volume)
{
    volume = std::clamp(volume, 0.0f, 1.0f);
    if (volume != volume_)
    {
        volume_ = volume;
        sync_flag_ |= kSyncVe;
    }
}

void Voice::SetVeVolume(float target, float init)
{
    target = std::clamp(target, 0.0f, 1.0f);
    init = std::clamp(init, 0.0f, 1.0f);
    if (init == ve_init_volume_ && target == ve_target_volume_)
    {
        return;
    }

    ve_init_volume_ = init;
    ve_target_volume_ = target;
    sync_flag_ |= kSyncVe;
}

void Voice::SetPitch(float pitch)
{
    if (pitch != pitch_)
    {
        pitch_ = pitch;
        sync_flag_ |= kSyncSrc;
    }
}

void Voice::SetPanMode(PanMode mode)
{
    if (mode != pan_mode_)
    {
        pan_mode_ = mode;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetPanCurve(uint8_t curve)
{
    if (curve != pan_curve_)
    {
        pan_curve_ = curve;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetPan(float pan)
{
    if (pan != pan_)
    {
        pan_ = pan;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetSurroundPan(float pan)
{
    if (pan != surround_pan_)
    {
        surround_pan_ = pan;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetLpfFreq(float freq)
{
    freq = std::clamp(freq, 0.0f, 1.0f);
    if (freq != lpf_freq_)
    {
        lpf_freq_ = freq;
        sync_flag_ |= kSyncLpf;
    }
}

void Voice::SetBiquadFilter(int type, float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    if (type != biquad_type_ || value != biquad_value_)
    {
        biquad_type_ = static_cast<uint8_t>(type);
        biquad_value_ = value;
        sync_flag_ |= kSyncBiquad;
    }
}

void Voice::SetMainOutVolume(float volume)
{
    volume = std::clamp(volume, 0.0f, 1.0f);
    if (volume != main_out_volume_)
    {
        main_out_volume_ = volume;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetMainSend(float send)
{
    // The main send is relative: 0 is the full send.
    send = std::clamp(send + 1.0f, 0.0f, 1.0f);
    if (send != main_send_)
    {
        main_send_ = send;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetFxSend(int bus, float send)
{
    send = std::clamp(send, 0.0f, 1.0f);
    if (send != fx_send_[bus])
    {
        fx_send_[bus] = send;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetPriority(int priority)
{
    priority_ = priority;
    engine_->Voices().ChangeVoicePriority(this);
    if (priority_ != 1)
    {
        return;
    }

    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->SetPriority(kAxPriorityRelease);
        }
    }
}

void Voice::UpdateVoicesPriority()
{
    if (priority_ == 1)
    {
        return;
    }

    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->SetPriority(kAxPriorityNormal);
        }
    }
}

bool Voice::IsPlayFinished() const
{
    return active_ && ax_voices_[0] != nullptr && ax_voices_[0]->IsPlayFinished();
}

void Voice::CalcAxSrc(bool initial)
{
    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->SetSrc(pitch_, initial);
        }
    }
}

bool Voice::CalcAxVe()
{
    // CalcAxVe (0x8004d044): the voice's volume, scaled by the master volume, at both ends of the frame's ramp.
    float volume = 1.0f;
    volume *= volume_;
    volume *= engine_->OutputVolume();
    if (engine_->Build().October2006())
    {
        // The build of October 2006 (0x800dd140 in Smooth Moves' main.dol) sets the AX voices' envelopes itself. The
        // ramp starts at the volume where the last ramp ended. Once the voice runs, that volume comes from the last
        // frame. The ramp steps by the difference over the frame's 96 samples. The step is rounded towards 0. The next
        // ramp starts at the target.
        const bool running = ax_voices_[0] != nullptr && ax_voices_[0]->IsRun();
        const float start = first_ve_update_ || !running ? volume : prev_ve_volume_;
        const auto init = static_cast<uint16_t>(static_cast<int32_t>(32767.0f * (start * ve_init_volume_)));
        const auto end = static_cast<uint16_t>(static_cast<int32_t>(32767.0f * (volume * ve_target_volume_)));
        const auto delta = static_cast<int16_t>((end - init) / ax::kSamplesPerFrame);
        for (int i = 0; i < channel_count_; i++)
        {
            if (ax_voices_[i] != nullptr)
            {
                ax_voices_[i]->SetVeOctober2006(init, delta);
            }
        }

        prev_ve_volume_ = volume;
        first_ve_update_ = false;
        ve_init_volume_ = ve_target_volume_;

        return end != init;
    }

    const float target = volume * ve_target_volume_;
    const float init = volume * ve_init_volume_;
    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->SetVe(target, init);
        }
    }

    return false;
}

bool Voice::CalcAxMix()
{
    // CalcAxMix (0x8004d134). The voice plays on the main line only. The Wii Remotes stay off.
    bool next_update = false;
    for (int i = 0; i < channel_count_; i++)
    {
        AxVoice* voice = ax_voices_[i];
        if (voice == nullptr)
        {
            continue;
        }

        AxVoice::MixParam mix;
        CalcMixParam(i, mix);
        next_update |= voice->SetMix(mix);
        voice->EnableRemote(false);
    }

    return next_update;
}

void Voice::CalcAxLpf()
{
    const int freq = CalcLpfFreq(lpf_freq_);
    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->SetLpf(static_cast<uint16_t>(freq));
        }
    }
}

void Voice::CalcAxBiquadFilter()
{
    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->SetBiquad(biquad_type_, biquad_value_);
        }
    }
}

void Voice::CalcMixParam(int channel, AxVoice::MixParam& mix) const
{
    // CalcMixParam (0x8004d590), for the stereo output mode on the main line. Each channel of a stereo wave pans from
    // its side. The archive's balance mode holds each channel to its side instead.
    const float main_volume = main_out_volume_;
    const float main = main_volume * main_send_;
    const float fx_a = main_volume * fx_send_[0];
    const float fx_b = main_volume * fx_send_[1];
    const float fx_c = main_volume * fx_send_[2];

    // The build of October 2006 (0x800dd970 in Smooth Moves' main.dol) has no pan curves and no balance mode. It pans
    // by the square root curve, and each channel of a stereo wave pans from its side.
    const bool october = engine_->Build().October2006();
    const PanInfo info = PanInfoFor(october ? 0 : pan_curve_);
    float left = 0.0f;
    float right = 0.0f;
    if (channel_count_ > 1 && pan_mode_ == PanMode::kBalance && !october)
    {
        if (channel == 0)
        {
            left = CalcPanRatio(pan_, info);
        }
        else
        {
            right = CalcPanRatio(-pan_, info);
        }
    }
    else
    {
        float voice_pan = 0.0f;
        if (channel_count_ == 2)
        {
            voice_pan = channel == 0 ? -1.0f : 1.0f;
        }

        const float pan = pan_ + voice_pan;
        left = CalcPanRatio(pan, info);
        right = CalcPanRatio(-pan, info);
    }

    mix.l = MixVolume(main * left);
    mix.r = MixVolume(main * right);
    mix.s = MixVolume(0.0f);
    mix.aux_a_l = MixVolume(fx_a * left);
    mix.aux_a_r = MixVolume(fx_a * right);
    mix.aux_a_s = MixVolume(0.0f);
    mix.aux_b_l = MixVolume(fx_b * left);
    mix.aux_b_r = MixVolume(fx_b * right);
    mix.aux_b_s = MixVolume(0.0f);
    mix.aux_c_l = MixVolume(fx_c * left);
    mix.aux_c_r = MixVolume(fx_c * right);
    mix.aux_c_s = MixVolume(0.0f);
}

void Voice::SyncAxVoice()
{
    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->Sync();
        }
    }
}

void Voice::RunAllAxVoice()
{
    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->Run();
        }
    }
}

void Voice::StopAllAxVoice()
{
    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->Stop();
        }
    }
}

void Voice::ResetDelta()
{
    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            ax_voices_[i]->ResetDelta();
        }
    }
}

void Voice::OnAxVoiceEvent(AxVoice* dropped, AxVoice::CallbackStatus status)
{
    // AxVoiceCallbackFunc (0x8004d2a0). Stop and free the voice's other AX voices. AX has freed the one it dropped. A
    // cancelled one is freed here.
    const bool drop = status == AxVoice::CallbackStatus::kDropDsp;
    for (int i = 0; i < channel_count_; i++)
    {
        AxVoice* voice = ax_voices_[i];
        if (voice == nullptr)
        {
            continue;
        }

        if (voice == dropped)
        {
            if (!drop)
            {
                engine_->AxVoices().FreeAxVoice(voice);
            }
        }
        else
        {
            voice->Stop();
            engine_->AxVoices().FreeAxVoice(voice);
        }

        ax_voices_[i] = nullptr;
    }

    pause_ = false;
    starting_ = false;
    channel_count_ = 0;
    if (drop)
    {
        Free();
    }

    if (callback_)
    {
        const Callback callback = callback_;
        callback(this, drop ? Status::kDropDsp : Status::kCancel);
    }
}

VoiceManager::VoiceManager(Engine& engine)
{
    for (std::unique_ptr<Voice>& voice : pool_)
    {
        voice = std::make_unique<Voice>(engine);
        free_.push_back(voice.get());
    }
}

VoiceManager::Snapshot VoiceManager::Save() const
{
    Snapshot snapshot{{}, free_, prio_list_};
    snapshot.voices.reserve(pool_.size());
    for (const std::unique_ptr<Voice>& voice : pool_)
    {
        snapshot.voices.push_back(*voice);
    }

    return snapshot;
}

void VoiceManager::Restore(const Snapshot& snapshot)
{
    for (std::size_t i = 0; i < pool_.size(); i++)
    {
        *pool_[i] = snapshot.voices[i];
    }

    free_ = snapshot.free;
    prio_list_ = snapshot.prio_list;
}

Voice* VoiceManager::AllocVoice(int channels, int priority, Voice::Callback callback)
{
    if (free_.empty() && DropLowestPriorityVoice(priority) == 0)
    {
        return nullptr;
    }

    Voice* voice = free_.front();
    if (!voice->Acquire(channels, priority, std::move(callback)))
    {
        return nullptr;
    }

    voice->priority_ = priority & Voice::kPriorityMax;
    AppendVoiceList(voice);
    UpdateEachVoicePriority(std::find(prio_list_.begin(), prio_list_.end(), voice));

    return voice;
}

void VoiceManager::FreeVoice(Voice* voice)
{
    RemoveVoiceList(voice);
}

void VoiceManager::UpdateAllVoices()
{
    // Like the game's safe iteration, each pass goes over a copy of the list. A voice may free itself.
    const auto for_each = [this](void (Voice::*step)())
    {
        for (auto it = prio_list_.begin(); it != prio_list_.end();)
        {
            Voice* voice = *it++;
            (voice->*step)();
        }
    };

    for_each(&Voice::StopFinished);
    for_each(&Voice::Calc);
    for_each(&Voice::Update);
}

void VoiceManager::ChangeVoicePriority(Voice* voice)
{
    RemoveVoiceList(voice);
    AppendVoiceList(voice);
    UpdateEachVoicePriority(std::find(prio_list_.begin(), prio_list_.end(), voice));
}

void VoiceManager::NotifyVoiceUpdate()
{
    for (auto it = prio_list_.begin(); it != prio_list_.end();)
    {
        Voice* voice = *it++;
        voice->ResetDelta();
    }
}

int VoiceManager::DropLowestPriorityVoice(int priority)
{
    if (!free_.empty())
    {
        return 0;
    }

    Voice* voice = prio_list_.front();
    if (voice->Priority() > priority)
    {
        return 0;
    }

    const int dropped = voice->AxVoiceCount();
    voice->Stop();
    voice->Free();
    if (voice->callback_)
    {
        const Voice::Callback callback = voice->callback_;
        callback(voice, Voice::Status::kDropVoice);
    }

    return dropped;
}

void VoiceManager::AppendVoiceList(Voice* voice)
{
    // Insert after the last voice of no higher priority. The list runs from the lowest priority, oldest first.
    free_.remove(voice);
    auto it = prio_list_.end();
    while (it != prio_list_.begin() && (*std::prev(it))->Priority() > voice->Priority())
    {
        --it;
    }

    prio_list_.insert(it, voice);
}

void VoiceManager::RemoveVoiceList(Voice* voice)
{
    prio_list_.remove(voice);
    free_.push_back(voice);
}

void VoiceManager::UpdateEachVoicePriority(std::list<Voice*>::iterator begin)
{
    for (auto it = begin; it != prio_list_.end(); ++it)
    {
        if ((*it)->Priority() <= 1)
        {
            return;
        }
        if ((*it)->Priority() != Voice::kPriorityMax)
        {
            (*it)->UpdateVoicesPriority();
        }
    }
}

Channel* Channel::AllocChannel(Engine& engine, int channels, int priority, Callback callback)
{
    Channel* channel = engine.Channels().Alloc();
    channel->alloc_ = true;
    const auto on_voice = [channel](Voice* voice, Voice::Status status)
    {
        channel->OnVoiceEvent(voice, status);
    };

    Voice* voice = engine.Voices().AllocVoice(channels, priority, on_voice);
    if (voice == nullptr)
    {
        engine.Channels().Free(channel);

        return nullptr;
    }

    channel->voice_ = voice;
    channel->InitParam(std::move(callback));

    return channel;
}

void Channel::FreeChannel(Channel* channel)
{
    if (channel != nullptr)
    {
        channel->callback_ = nullptr;
    }
}

void Channel::InitParam(Callback callback)
{
    next_link_ = nullptr;
    callback_ = std::move(callback);
    pause_ = false;
    auto_sweep_ = true;
    release_priority_fix_ = false;
    ignore_note_off_ = false;
    key_group_ = 0;
    length_ = 0;
    key_ = 60;
    original_key_ = 60;
    init_volume_ = 1.0f;
    init_pan_ = 0.0f;
    init_surround_pan_ = 0.0f;
    tune_ = 1.0f;
    user_volume_ = 1.0f;
    user_pitch_ = 0.0f;
    user_pitch_ratio_ = 1.0f;
    user_pan_ = 0.0f;
    user_surround_pan_ = 0.0f;
    user_lpf_freq_ = 0.0f;
    biquad_type_ = 0;
    biquad_value_ = 0.0f;
    main_out_volume_ = 1.0f;
    main_send_ = 0.0f;
    fx_send_ = {};
    silence_volume_ = SilenceVolume{};
    sweep_pitch_ = 0.0f;
    sweep_length_ = 0;
    sweep_counter_ = 0;
    envelope_.Init();
    lfo_.SetParam(LfoParam{});
    lfo_target_ = LfoTarget::kPitch;
    pan_mode_ = PanMode::kDual;
    pan_curve_ = 0;
}

void Channel::Update(bool periodic)
{
    if (!active_)
    {
        return;
    }
    if (pause_)
    {
        periodic = false;
    }

    // The values at the start of the frame: the volume, and the envelope (with a volume LFO) where the ramp starts.
    const float lfo_value = lfo_.Value();
    if (silence_volume_.counter < silence_volume_.frame)
    {
        silence_volume_.counter++;
    }

    float volume = 1.0f;
    volume *= init_volume_;
    volume *= user_volume_;
    volume *= static_cast<float>(silence_volume_.Value()) / 255.0f;

    float ve_init_volume = 1.0f;
    ve_init_volume *= CalcVolumeRatio(envelope_.Value());
    if (lfo_target_ == LfoTarget::kVolume)
    {
        ve_init_volume *= CalcVolumeRatio(6.0f * lfo_value);
    }

    // A released note stops once it's silent. A channel with an owner falls silent when its envelope reaches 0. A
    // channel without an owner falls silent when its volume times its envelope reaches 0. The build of October 2006
    // (0x800e039c in Smooth Moves' main.dol) always takes the volume times the envelope.
    if (envelope_.GetStatus() == Envelope::Status::kRelease)
    {
        const bool owner = callback_ && !engine_->Build().October2006();
        if (owner ? ve_init_volume == 0.0f : volume * ve_init_volume == 0.0f)
        {
            Stop();

            return;
        }
    }

    // The pitch in semitones from the key, the sweep, the bend and a pitch LFO.
    float cent = 0.0f;
    cent += static_cast<float>(key_ - original_key_);
    cent += SweepValue();
    cent += user_pitch_;
    if (lfo_target_ == LfoTarget::kPitch)
    {
        cent += lfo_value;
    }

    float pitch_ratio = 1.0f;
    pitch_ratio *= tune_;
    pitch_ratio *= user_pitch_ratio_;
    float pitch = CalcPitchRatio(static_cast<int>(256.0f * cent));
    pitch *= pitch_ratio;

    float pan = 0.0f;
    pan += init_pan_;
    pan += user_pan_;
    if (lfo_target_ == LfoTarget::kPan)
    {
        pan += lfo_value;
    }

    float surround_pan = 0.0f;
    surround_pan += init_surround_pan_;
    surround_pan += user_surround_pan_;
    float lpf_freq = 1.0f;
    lpf_freq += user_lpf_freq_;
    float main_out_volume = 1.0f;
    main_out_volume *= main_out_volume_;
    float main_send = 0.0f;
    main_send += main_send_;
    std::array<float, 3> fx_send{};
    for (int i = 0; i < 3; i++)
    {
        fx_send[i] += fx_send_[i];
    }

    // Advance by the frame's 3 ms. The envelope there (with a volume LFO) is where the ramp ends.
    if (periodic)
    {
        if (auto_sweep_)
        {
            UpdateSweep(3);
        }

        lfo_.Update(3);
        envelope_.Update(3);
    }

    const float next_lfo_value = lfo_.Value();
    float ve_target_volume = 1.0f;
    ve_target_volume *= CalcVolumeRatio(envelope_.Value());
    if (lfo_target_ == LfoTarget::kVolume)
    {
        ve_target_volume *= CalcVolumeRatio(6.0f * next_lfo_value);
    }

    if (voice_ == nullptr)
    {
        return;
    }

    voice_->SetPanMode(pan_mode_);
    voice_->SetPanCurve(pan_curve_);
    voice_->SetVolume(volume);
    voice_->SetVeVolume(ve_target_volume, ve_init_volume);
    voice_->SetPitch(pitch);
    voice_->SetPan(pan);
    voice_->SetSurroundPan(surround_pan);
    voice_->SetLpfFreq(lpf_freq);
    voice_->SetBiquadFilter(biquad_type_, biquad_value_);
    voice_->SetMainOutVolume(main_out_volume);
    voice_->SetMainSend(main_send);
    for (int i = 0; i < 3; i++)
    {
        voice_->SetFxSend(i, fx_send[i]);
    }
}

void Channel::Start(const WaveData& data, int length, uint32_t offset)
{
    length_ = length;
    wave_loops_ = data.loop;
    lfo_.Reset();
    envelope_.Reset();
    sweep_counter_ = 0;
    voice_->Setup(data, offset);
    voice_->Start();
    active_ = true;
}

void Channel::Release()
{
    if (envelope_.GetStatus() != Envelope::Status::kRelease)
    {
        if (voice_ != nullptr && !release_priority_fix_)
        {
            voice_->SetPriority(1);
        }

        envelope_.SetStatus(Envelope::Status::kRelease);
    }

    pause_ = false;
}

void Channel::NoteOff()
{
    if (ignore_note_off_)
    {
        return;
    }

    Release();
}

void Channel::Stop()
{
    if (voice_ == nullptr)
    {
        return;
    }

    voice_->Stop();
    voice_->Free();
    voice_ = nullptr;
    pause_ = false;
    active_ = false;
    if (alloc_)
    {
        alloc_ = false;
        engine_->Channels().Free(this);
    }

    if (callback_)
    {
        const Callback callback = callback_;
        callback(this, CallbackStatus::kStopped);
    }
}

void Channel::UpdateSweep(int count)
{
    sweep_counter_ = std::min(sweep_counter_ + count, sweep_length_);
}

void Channel::SetSweepParam(float pitch, int time, bool auto_update)
{
    sweep_pitch_ = pitch;
    sweep_length_ = time;
    auto_sweep_ = auto_update;
    sweep_counter_ = 0;
}

uint8_t Channel::SilenceVolume::Value() const
{
    if (counter >= frame)
    {
        return target;
    }

    return static_cast<uint8_t>(origin + counter * (target - origin) / frame);
}

float Channel::SweepValue() const
{
    // GetSweepValue (inlined in Update): the sweep's pitch. The pitch falls to 0 over the sweep's length.
    if (sweep_pitch_ == 0.0f || sweep_counter_ >= sweep_length_)
    {
        return 0.0f;
    }

    float sweep = sweep_pitch_;
    sweep *= static_cast<float>(sweep_length_ - sweep_counter_);
    sweep /= static_cast<float>(sweep_length_);

    return sweep;
}

void Channel::OnVoiceEvent(Voice* voice, Voice::Status status)
{
    // A voice that finished or was cancelled is freed here. A dropped voice has been freed already.
    CallbackStatus channel_status = CallbackStatus::kDrop;
    switch (status)
    {
    case Voice::Status::kFinishWave:
        channel_status = CallbackStatus::kFinish;
        voice->Free();
        break;

    case Voice::Status::kCancel:
        channel_status = CallbackStatus::kCancel;
        voice->Free();
        break;

    case Voice::Status::kDropVoice:
    case Voice::Status::kDropDsp:
        channel_status = CallbackStatus::kDrop;
        break;
    }

    if (callback_)
    {
        const Callback callback = callback_;
        callback(this, channel_status);
    }

    voice_ = nullptr;
    pause_ = false;
    active_ = false;
    alloc_ = false;
    engine_->Channels().Free(this);
}

ChannelManager::ChannelManager(Engine& engine)
{
    // The pool is a stack built from the end of its memory. The first channel out is the last in the block.
    for (std::unique_ptr<Channel>& channel : pool_)
    {
        channel = std::make_unique<Channel>(engine);
        free_.push_front(channel.get());
    }
}

ChannelManager::Snapshot ChannelManager::Save() const
{
    Snapshot snapshot{{}, free_, channels_};
    snapshot.channels.reserve(pool_.size());
    for (const std::unique_ptr<Channel>& channel : pool_)
    {
        snapshot.channels.push_back(*channel);
    }

    return snapshot;
}

void ChannelManager::Restore(const Snapshot& snapshot)
{
    for (std::size_t i = 0; i < pool_.size(); i++)
    {
        *pool_[i] = snapshot.channels[i];
    }

    free_ = snapshot.free;
    channels_ = snapshot.active;
}

Channel* ChannelManager::Alloc()
{
    Channel* channel = free_.front();
    free_.pop_front();
    channels_.push_back(channel);

    return channel;
}

void ChannelManager::Free(Channel* channel)
{
    channels_.remove(channel);
    free_.push_front(channel);
}

void ChannelManager::UpdateAllChannel()
{
    // Like the game's safe iteration, go over a copy. A channel may stop and free itself.
    for (auto it = channels_.begin(); it != channels_.end();)
    {
        Channel* channel = *it++;
        channel->Update(true);
    }
}

bool ChannelManager::AnyChanging() const
{
    const auto changing = [](const Channel* channel)
    {
        return !channel->IsSustain();
    };

    return std::any_of(channels_.begin(), channels_.end(), changing);
}

} // namespace wsf::nw4r
