// SPDX-License-Identifier: MIT

// Game mode's length analysis and wave discovery for a JAudio game (JAudio1 or JAudio2). A set of a rip's library and a
// mini file plays on WSF's player, and ntdlib's AnalyzeGameSound watches the game's code while the sound plays
// (ntdlib/jaudio/game_lengths.h).

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/jaudio/wave_system.h"
#include "xsflib/rip/length.h"

#include "rip/rip.h"

namespace wsf::rip
{

// Plays sound `sound_id` of `library`, a .wsflib from BuildJAudioLibrary, with the wave groups `waves` loaded, and
// works out its length. The waves that the sound's notes look up go in `found` if it isn't null. With `lookups_sound`,
// each lookup counts as a sound: wave discovery plays the sound with no wave group loaded. Returns nothing if the set
// can't start or a render fails. When the set can't start, `start_error` gets the player's message if it isn't null.
std::optional<xsflib::rip::Timing> AnalyzeJAudioSound(const std::vector<uint8_t>& library, const JAudioProfile& profile,
                                                      uint32_t sound_id, std::span<const uint32_t> waves,
                                                      std::set<ntdlib::jaudio::WaveUse>* found = nullptr,
                                                      bool lookups_sound = false, std::string* start_error = nullptr);

} // namespace wsf::rip
