// SPDX-License-Identifier: MIT

// wsfrip's rips of the sound archives of NW4R SND (.brsar), in archive mode and in game mode. Archive mode plays an
// archive with WSF's model of the sound library. Game mode runs the game's code.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "xsflib/rip/file_names.h"

#include "cli/wsfrip.h"
#include "rip/profiles.h"

namespace wsf::cli
{

// RipArchive's result when the model can't drive the game's AX microcode.
constexpr int kMicrocodeRejected = -2;

// Rips one sound archive in archive mode into `out_dir`. `names` is the registry of file names for `out_dir`. Adds the
// number of sounds that --only selected to `matched`. Returns the number of .miniwsf files written, kMicrocodeRejected
// if the model can't drive the game's AX microcode, or -1 on another error.
int RipArchive(const std::filesystem::path& out_dir, xsflib::rip::FileNames& names, const std::string& lib_base,
               const std::string& archive_path, const std::vector<uint8_t>& archive_bytes, const GameCode& code,
               const std::string& game_name, const Options& o, std::size_t& matched);

// Rips one of a game's sound archives in game mode into `out_dir`. `names` is the registry of file names for `out_dir`.
// Adds the number of sounds that --only selected to `matched`. Returns the number of .miniwsf files written, or -1 on
// an error.
int RipArchiveInGameMode(const std::filesystem::path& out_dir, xsflib::rip::FileNames& names,
                         const std::string& lib_base, const ntdlib::disc::GameFiles& game,
                         const rip::GameProfile& profile, const std::string& archive_path,
                         const std::vector<uint8_t>& archive_bytes, const GameCode& code, const std::string& game_name,
                         const Options& o, std::size_t& matched);

} // namespace wsf::cli
