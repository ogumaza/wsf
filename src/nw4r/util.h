// SPDX-License-Identifier: MIT

// NW4R SND's utilities (Util): pitch, volume and pan ratios, the low-pass filter's cutoff, the Wii Remote's filter
// coefficients and the random numbers. Also the coefficients of the biquad filter that later builds add. Modelled on
// Wii Sports' main.dol (NW4R SND of 2007). A function that follows one of the game's routines gives its address. NW4R
// SND and the SDK are built without fused multiply-adds. The same single-precision operations in the same order
// therefore give the game's results.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace wsf::nw4r
{

// A build of NW4R SND, by the date in its version banner in main.dol ("<< NW4R    - SND \tfinal   build: Mar  8 2008
// 20:59:14 ..."), as YYYYMMDD. 0 means a build without a banner. The model follows Wii Sports' build (20070608). Where
// builds from 20080308 (Mario Kart Wii) on differ, it checks Since2008. Where builds from 20080620 (Wii Music) on
// differ again, it checks SinceJune2008. A build between two of these counts as the older kind because none has been
// compared with the model.
struct SndBuild
{
    uint32_t date = 0;

    bool Since2008() const
    {
        return date >= 20080308;
    }

    bool SinceJune2008() const
    {
        return date >= 20080620;
    }
};

// The build of NW4R SND that a main.dol links, from its banner.
SndBuild FindSndBuild(std::span<const uint8_t> dol);

// The random numbers (Util::CalcRandom, 0x8004ec58): x = x * 0x19660D + 0x3C6EF35F from 0x12345678, giving x's top 16
// bits. The game has one generator for the whole sound system.
class Random
{
public:
    uint16_t Next()
    {
        state_ = state_ * 0x19660D + 0x3C6EF35F;

        return static_cast<uint16_t>(state_ >> 16);
    }

private:
    uint32_t state_ = 0x12345678;
};

enum class PanCurve : uint8_t
{
    kSqrt = 0,
    kSinCos = 1,
    kLinear = 2,
};

// How a pan becomes a ratio (Util::PanInfo): the curve, whether the centre gives 1 (rather than the curve's value
// there), and whether ratios stop at 1 (rather than 2).
struct PanInfo
{
    PanCurve curve = PanCurve::kSqrt;
    bool center_zero = false;
    bool zero_clamp = false;
};

// 2^(pitch / 3072): `pitch` in 256ths of a semitone (Util::CalcPitchRatio, 0x8004e83c).
float CalcPitchRatio(int pitch);

// 10^(dB / 20), from a table in 0.1 dB steps, for dB clamped to -90.4..6.0 (Util::CalcVolumeRatio, 0x8004e9cc).
float CalcVolumeRatio(float db);

// The ratio for one side at a pan of -1 (left) to 1 (right) (Util::CalcPanRatio, 0x8004ea24).
float CalcPanRatio(float pan, const PanInfo& info);

// The ratio for a surround pan of 0 (front) to 2 (rear) (Util::CalcSurroundPanRatio, 0x8004eb04).
float CalcSurroundPanRatio(float pan, const PanInfo& info);

// The low-pass filter's cutoff in Hz for a scale of 0..1: 32000 * 2^(10 (scale - 1)) (Util::CalcLpfFreq,
// 0x8004eb98). The game computes the power with its C library's pow in double precision.
int CalcLpfFreq(float scale);

// The Wii Remote's biquad coefficients b0, b1, b2, a1 and a2 for a filter value, clamped to 0..127
// (Util::GetRemoteFilterCoefs, 0x8004ec04).
std::array<uint16_t, 5> RemoteFilterCoefs(int filter);

// The biquad filter's coefficients b0, b1, b2, a1 and a2 for a filter `type` at a `value` of 0 (mildest) to 1, in
// builds from June 2008 on. Type 1 is a low-pass filter, 2 a high-pass and 3 to 5 band-passes. Other types have no
// coefficients. Type 0 is no filter. For any other type it uses, the game registers a function. The model can't run
// that function.
std::optional<std::array<uint16_t, 5>> BiquadFilterCoefs(int type, float value);

} // namespace wsf::nw4r
