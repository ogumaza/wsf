// SPDX-License-Identifier: MIT

// What a game can set through a sequence sound's handle before the sequence's first tick: the tracks that play, the
// tempo ratio and the variables. Games use them to play one sequence in several arrangements. WSF's tags give them in
// archive mode (docs/wsf.md, "Tags").

#pragma once

#include <cstdint>

#include "xsflib/psf/psf.h"

namespace wsf::nw4r
{

struct Arrangement
{
    // The tracks that play, bit N for track N. The others get a track volume of 0 (SeqPlayer::SetTrackVolume,
    // 0x8003e8b0 in Wii Sports' main.dol).
    uint16_t tracks = 0xffff;

    // The tempo ratio, by which the clock multiplies the tempo (SeqPlayer::SetTempoRatio, 0x8003e818).
    float tempo_ratio = 1.0f;

    // The variables, by number: 0 to 15 are the player's variables, and 16 to 31 the global ones.
    xsflib::psf::Variables variables;
};

} // namespace wsf::nw4r
