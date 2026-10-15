// SPDX-License-Identifier: MIT

// What the effects' models share: the games' sample rate, their runtime's conversion of a float to an unsigned integer,
// and the reverbs' range check, comb gains and line steps.

#pragma once

#include <cmath>
#include <cstdint>

namespace wsf::axfx
{

// The sample rate that the games' code converts times with.
constexpr float kSampleRate = 32000.0f;

// The runtime's conversion of a float to an unsigned integer (__cvt_fp2unsigned, 0x800b1c1c in Wii Sports, 0x804be79c
// in WarioWare: Smooth Moves): 0 below 0, all ones from 2^32 and for NaN, and truncated between.
inline uint32_t ToUnsigned(float value)
{
    if (value < 0.0f)
    {
        return 0;
    }

    if (!(value < 4294967296.0f))
    {
        return 0xffffffff;
    }

    return static_cast<uint32_t>(value);
}

// Whether a gain lies outside 0 to 1. A NaN gain doesn't. The games' comparisons let NaN through too.
inline bool OutOfRange(float value)
{
    return value < 0.0f || value > 1.0f;
}

// The gain of a reverb's comb `length` samples long that decays by 60 dB in `time` seconds. The games call MSL's pow
// (0x800c0e80 in Wii Sports, 0x804cb4dc in Smooth Moves) in double.
inline float CombCoef(uint32_t length, float time)
{
    const float exponent = -3.0f * static_cast<float>(length) / (kSampleRate * time);

    return static_cast<float>(std::pow(10.0, static_cast<double>(exponent)));
}

// Moves a line's position on by a sample. The position wraps to 0 at the line's length.
inline void Step(uint32_t& position, uint32_t length)
{
    position++;
    if (position >= length)
    {
        position = 0;
    }
}

} // namespace wsf::axfx
