// SPDX-License-Identifier: MIT

// wsfrip's rips of the games whose sound library is JAudio (JAudio1 or JAudio2). Such a game rips in game mode only.

#pragma once

#include <filesystem>
#include <string>

#include "ntdlib/disc/game.h"
#include "xsflib/cli/rip.h"

#include "cli/wsfrip.h"
#include "rip/jaudio.h"

namespace wsf::cli
{

// Game mode for a JAudio game: rips the sounds of its start-up archive, or of a Jac game's sequence index, into one
// set. A JAudio2 sound first plays with only the wave groups that the game's audio set-up loads, and the waves its
// notes look up give its groups. ReadJacSounds gives the group of each piece of a Jac game. ReadPikmin2Sounds lists the
// sounds of New Play Control! Pikmin 2 with their groups, apart from the pieces that the game never plays. Wave
// discovery finds the groups of those pieces. The library then holds every group that a ripped sound plays, and each
// sound plays with its groups for the length analysis. `files` are the game's sound files. `profile` is null for a
// JAudio2 game without a driver profile. Only --list works then.
int RipJAudioGame(const std::string& input, const std::filesystem::path& out_dir, const ntdlib::disc::GameFiles& game,
                  const rip::JAudioFiles& files, const rip::JAudioProfile* profile, const Options& o,
                  xsflib::cli::FileNameRegistries& registries);

} // namespace wsf::cli
