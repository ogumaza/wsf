// SPDX-License-Identifier: MIT

// NW4R SND's utilities (Util): pan ratios, the low-pass filter's cutoff and the random numbers. Also the coefficients
// of the biquad filter that later builds add. Modelled on Wii Sports' main.dol (NW4R SND of 2007). A function that
// follows one of the game's routines gives its address. NW4R SND and the SDK are built without fused multiply-adds. The
// same single-precision operations in the same order therefore give the game's results. The header also defines
// SndBuild, the build of NW4R SND that a main.dol links. The checks for the builds of November 2008 and September 2009
// are here too.

#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sound_build.h"

#include "axfx/util.h"

namespace wsf::nw4r
{

// The build of NW4R SND that a main.dol links (ntdlib/sound_build.h).
using SndBuild = ntdlib::SndBuild;

// Builds from November 2008 on take the low-pass filter's cutoff from a table (see CalcLpfFreq). The earliest build
// seen with the table is of November 20 2008. The latest build seen without it is of September 19 2008. The builds
// between those dates haven't been checked. They count as the older kind.
inline bool SinceNovember2008(const SndBuild& build)
{
    return build.date >= 20081120;
}

// Builds from September 2009 on let a voice's volume and envelope go above full level (see Voice::SetVolume). The
// earliest build seen that does is of September 24 2009. The latest build seen that doesn't is of November 20 2008. The
// builds between those dates haven't been checked. They count as the older kind.
inline bool SinceSeptember2009(const SndBuild& build)
{
    return build.date >= 20090924;
}

// Converts a float to an unsigned integer like the game's runtime (__cvt_fp2unsigned, 0x800b1c1c): 0 below 0, all ones
// from 2^32 (and for NaN), and truncated between.
using axfx::ToUnsigned;

// The random numbers (Util::CalcRandom, 0x8004ec58): x = x * 0x19660D + 0x3C6EF35F from 0x12345678. A draw gives x's
// top 16 bits. The game has one generator for the whole sound system.
using Random = ntdlib::nwsnd::Random;

// How a pan becomes a ratio (Util::PanInfo): the curve, whether the centre gives 1 (rather than the curve's value
// there), and whether ratios stop at 1 (rather than 2).
using PanInfo = ntdlib::nwsnd::PanInfo;

// The ratio for one side at a pan of -1 (left) to 1 (right), in the Wii's stereo mix (Util::CalcPanRatio, 0x8004ea24).
inline float CalcPanRatio(float pan, const PanInfo& info)
{
    return ntdlib::nwsnd::CalcPanRatio<ntdlib::nwsnd::RevolutionPolicy>(pan, info, ntdlib::nwsnd::OutputMode::kStereo);
}

// The low-pass filter's cutoff in Hz for a scale of 0..1: 32000 * 2^(10 (scale - 1)) (Util::CalcLpfFreq, 0x8004eb98).
// The game computes the power with its C library's pow in double precision. Builds from November 2008 on take the
// cutoff from a table instead. The Wii U's library (CafePolicy) reads the table in the same way: 80 Hz below
// 0.13561438, 16000 Hz from 0.9, and steps a thirtieth apart between them. These builds hold the scale to 0 and 1 here,
// not in Voice::SetLpfFreq. The model holds it in both places. The cutoffs come out the same.
inline uint16_t CalcLpfFreq(float scale, const SndBuild& build)
{
    if (SinceNovember2008(build))
    {
        return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::CafePolicy>(scale);
    }

    return ntdlib::nwsnd::CalcLpfFreq<ntdlib::nwsnd::RevolutionPolicy>(scale);
}

// The biquad filter's coefficients b0, b1, b2, a1 and a2 for a filter `type` at a `value` of 0 (mildest) to 1, in
// builds from June 2008 on. AX takes them as unsigned 16-bit values. Type 1 is a low-pass filter, 2 a high-pass and 3
// to 5 band-passes (ntdlib's BiquadPresetCoefficients). Other types have no coefficients. Type 0 is no filter. For any
// other type it uses, the game registers a function. The model can't run that function.
inline std::optional<std::array<uint16_t, 5>> BiquadFilterCoefs(int type, float value)
{
    const int16_t* row = ntdlib::nwsnd::BiquadPresetCoefficients<ntdlib::nwsnd::RevolutionPolicy>(type, value);
    if (row == nullptr)
    {
        return std::nullopt;
    }

    return std::array<uint16_t, 5>{static_cast<uint16_t>(row[0]), static_cast<uint16_t>(row[1]),
                                   static_cast<uint16_t>(row[2]), static_cast<uint16_t>(row[3]),
                                   static_cast<uint16_t>(row[4])};
}

} // namespace wsf::nw4r
