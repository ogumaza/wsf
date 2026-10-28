// SPDX-License-Identifier: MIT

// What the ripper's files share. rip.cpp holds the parts that every game shares, and profiles.cpp the games' profiles.
// The file of each sound library holds the rest: nw4r.cpp for NW4R SND, and jaudio.cpp for JAudio2, JAudio1 and Jac.
// The length analysis of a JAudio game plays the game's code on the player. jaudio_analysis.cpp holds it, in a separate
// library that links the player.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/flipper/machine.h"

#include "wsf/format.h"

namespace wsf::rip
{

// What every game shares (rip.cpp)

// The ticks of the time base in a frame of the video, a 60th of a second. The time base runs at a quarter of the
// 243 MHz bus clock. NW4R SND's driver calls SoundArchivePlayer::Update once a frame. The JAudio drivers take the same
// time for a frame.
inline constexpr uint32_t kFrameTicks = ntdlib::flipper::kWiiBusClockHz / 4 / 60;

// What differs between the two kinds of game-mode library, for the NW4R driver (BuildLibrary) and for JAudio's drivers
// (BuildJAudioLibrary). The driver's image has its parameters filled in. It loads at `base`, and the game's `main`
// branches to its `entry`. For a game without a TMD, the IOS is the one that the game's profile gives. The library
// holds the disc's files at `paths`. Then it holds `files`: each is a path on the disc and the bytes that the library
// holds for that file. A library too large for a player gets an error that ends with `too_large_advice`.
struct GameLibrary
{
    std::vector<uint8_t> driver;
    uint32_t base;
    uint32_t entry;
    uint32_t main;
    uint32_t ios_version;
    std::vector<std::string> paths;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    std::string too_large_advice;
};

// Builds a game-mode library. It holds the game's code and data from `dol`, section by section, with the game's main
// patched to branch to the driver. Then it holds the driver, the partition's header and the disc's files. The process
// starts at main.dol's entry. The driver writes the status to DriverBlock. The IOS is the low word of the system
// version in the game's TMD, or `library.ios_version` for a game without a TMD. Returns nothing and sets `error` if a
// file can't be read, or if the program is larger than a player loads.
std::optional<std::vector<uint8_t>> BuildGameLibrary(const ntdlib::disc::GameFiles& game, const ntdlib::Dol& dol,
                                                     const GameLibrary& library, const Tags& tags, std::string* error);

// A library that can't be built: sets `*error` to `message` unless `error` is null, and returns nothing.
std::nullopt_t LibraryError(std::string* error, const std::string& message);

// JAudio2, JAudio1 and Jac (jaudio.cpp)

// Pikmin 2's driver parameters: the profile's `words`, then the number of pieces and the pieces (driver/pikmin2.c's
// Piece).
std::vector<uint32_t> Pikmin2Parameters(std::vector<uint32_t> words);

} // namespace wsf::rip
