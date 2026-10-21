// SPDX-License-Identifier: MIT

// What the ripper's files share. rip.cpp holds the parts that every game shares, and profiles.cpp the games' profiles.
// The file of each sound library holds the rest: nw4r.cpp for NW4R SND, and jaudio.cpp for JAudio2, JAudio1 and Jac.
// The length analysis of a JAudio game plays the game's code on the player. jaudio_analysis.cpp holds it, in a separate
// library that links the player.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/flipper/machine.h"
#include "ntdlib/rsar/formats.h"

#include "axfx/axfx.h"
#include "nw4r/effect_setup.h"
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
// holds the disc's files at `paths`. A library too large for a player gets an error that ends with `too_large_advice`.
struct GameLibrary
{
    std::vector<uint8_t> driver;
    uint32_t base;
    uint32_t entry;
    uint32_t main;
    uint32_t ios_version;
    std::vector<std::string> paths;
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

// The effect profiles (profiles.cpp)

// The functions of a game with an effect profile that game mode's driver calls for the effects (driver/driver.c).
struct EffectClass
{
    nw4r::EffectType type;
    uint32_t ctor, mem_size, assign, set_param; // FxXxx::FxXxx, GetRequiredMemSize, AssignWorkBuffer and SetParam
    uint32_t start_up, shutdown, update_buffer; // the virtual StartUp, Shutdown and UpdateBuffer (for the player)
};

// A game's effect profile: the effects that the game sets up for the sounds of each of its archives, and what game
// mode's driver calls for them. Besides the classes' functions, the driver calls AXRegisterCallback, and AxManager's
// GetInstance, AppendEffect and ClearEffect. AXRegisterCallback returns the callback that it replaces. AxManager
// registers one. The driver calls that one first. For presets, the driver also calls SeqSoundHandle's constructor and
// destructor and SeqSound::ReadVariable. For a send on the sound, it writes the send at an offset in BasicSound. For
// the tracks that the game silences, it writes a volume of 0 into each track (SeqSound::SetTrackVolume): `seq_tracks`
// is the offset of the SeqPlayer's 16 track pointers in SeqSound, and `track_volume` the offset of the volume in
// SeqTrack. A game without presets, sends or silenced tracks leaves their words at 0.
struct EffectProfile
{
    const char* dol_sha1;

    // The effects for the sounds of the archive whose file name, in lower case, is `name`.
    nw4r::ArchiveEffects (*effects)(const std::string& name, const ntdlib::rsar::SoundArchive& archive);

    axfx::Build axfx_build; // the build of AXFX that the game links, for the player's models of the effects
    uint32_t ax_register_callback, ax_manager, append_effect, clear_effect;
    uint32_t seq_handle_ctor = 0, seq_handle_dtor = 0, read_variable = 0;
    uint32_t sound_sends = 0;
    uint32_t seq_tracks = 0, track_volume = 0;
    std::vector<EffectClass> classes;
};

// The effect profile of the main.dol with `dol_sha1`, or null if WSF has none.
const EffectProfile* FindEffectProfile(const std::string& dol_sha1);

// JAudio2, JAudio1 and Jac (jaudio.cpp)

// Pikmin 2's driver parameters: the profile's `words`, then the number of pieces and the pieces (driver/pikmin2.c's
// Piece).
std::vector<uint32_t> Pikmin2Parameters(std::vector<uint32_t> words);

} // namespace wsf::rip
