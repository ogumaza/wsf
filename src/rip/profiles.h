// SPDX-License-Identifier: MIT

// The profiles of the Wii games that WSF rips with NW4R SND: the driver profiles of game mode, with the signatures that
// find their functions in another release (game_profiles.cpp), and the effect profiles (effect_profiles.cpp).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/axfx/axfx.h"
#include "ntdlib/rvl/snd/effect_setup.h"

#include "wsf/models.h"

namespace wsf::rip
{

// A function's code, to find the function in another main.dol: its length up to its first blr, its first word and an
// FNV-1a hash of its words, each with the fields that the linker fills in cleared (branch targets, and the immediates
// of addis and of the loads, stores and adds that don't address the stack).
struct FunctionSignature
{
    uint32_t size = 0;
    uint32_t first = 0;
    uint64_t hash = 0;
};

// The functions of a profile that have signatures: those from ai_init to update. main has none (see DeriveProfile).
constexpr std::size_t kProfileFunctions = 17;

// Game-specific addresses for the driver (see driver/driver.c): the game's functions in its main.dol.
struct GameProfile
{
    std::string name;
    std::string game_id;  // "RSPE01"
    std::string dol_sha1; // the main.dol that the addresses belong to
    uint32_t main;        // patched to branch to the driver
    uint32_t ai_init, ax_init, alloc, sleep_ticks, init_sound_system;
    uint32_t archive_ctor, archive_open, archive_load_info;
    uint32_t player_ctor, player_mem_size, player_strm_size, player_setup;
    uint32_t heap_ctor, heap_create, load_group, start_sound, update;
    uint32_t startable_offset; // SoundArchivePlayer's SoundStartable base
    uint32_t ios_version;      // the IOS that the game's TMD requests
    uint32_t init_flag;        // the sound system's flag byte, checked by Setup in 2008 builds, or 0
    std::array<FunctionSignature, kProfileFunctions> signatures; // those of ai_init to update
    std::string derived_from = {}; // the profile that a derived profile comes from (see FindProfile), or nothing
};

const std::vector<GameProfile>& KnownProfiles();

// The options with which WSF loads a game: the Wii, WSF's name in messages, the containers that ntdlib's
// disc_containers reads (GCZ, CISO, WIA and RVZ), titles for the NAND, such as WiiWare games, from a folder with the
// title's TMD and decrypted contents, and the sound archives inside CRI's CPK archives.
ntdlib::disc::GameOptions DiscOptions();

// Whether the game IDs `game_id` and `other_id` belong to releases of one game. A game's releases share the first three
// characters of their game IDs. The fourth gives the region.
bool SameGame(const std::string& game_id, const std::string& other_id);

// The profile for a game: the one for its main.dol, or else one derived from the profile of another release or revision
// of the game (see SameGame). The main.dols of a game's releases link the same functions at other addresses (see
// DeriveProfile). Returns nothing if neither profile exists.
std::optional<GameProfile> FindProfile(const ntdlib::disc::GameFiles& game);

// Finds `source`'s functions in `dol` by their signatures. A small function can match in several places. The functions
// of a library move together. The offset of the nearest function that matched once therefore picks the place. Returns
// the profile with the addresses found, or nothing if a function isn't there. A game's main differs between releases.
// It comes from the start-up code instead: __start calls main, then branches to exit. For a source with an init flag,
// the flag's address comes from the code of Setup that was found: Setup calls a function that reads the flag relative
// to r13, and the start-up code gives r13.
std::optional<GameProfile> DeriveProfile(const GameProfile& source, std::span<const uint8_t> dol);

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
// SeqTrack. For the variables that the game sets on the sound, it writes each into the sound's player
// (SeqSound::WriteVariable): `seq_variables` is the offset of the SeqPlayer's 16 local variables in SeqSound. A game
// without presets, sends, silenced tracks or variables leaves their words at 0.
struct EffectProfile
{
    std::string dol_sha1;

    // The game ID of the release whose main.dol the profile was made from, for the game's other releases (see
    // DeriveEffectProfile). Empty in a profile that no other release takes.
    std::string game_id = {};

    // The effects for the sounds of the archive whose file name, in lower case, is `name`.
    nw4r::ArchiveEffects (*effects)(const std::string& name, const ntdlib::rsar::SoundArchive& archive);

    axfx::Build axfx_build; // the build of AXFX that the game links, for the player's models of the effects
    uint32_t ax_register_callback, ax_manager, append_effect, clear_effect;
    uint32_t seq_handle_ctor = 0, seq_handle_dtor = 0, read_variable = 0;
    uint32_t sound_sends = 0;
    uint32_t seq_tracks = 0, track_volume = 0;
    uint32_t seq_variables = 0;
    std::vector<EffectClass> classes;

    // For the game's other releases (see DeriveEffectProfile). The anchors are functions that the driver doesn't call.
    // Each one places the functions of its library that match in several places. The signatures follow the functions:
    // those from ax_register_callback to read_variable, without the ones at 0, then each class's in the order of its
    // fields, then the anchors.
    std::vector<uint32_t> anchors = {};
    std::vector<FunctionSignature> signatures = {};
};

// The effect profile for the main.dol `dol` of the game with the ID `game_id`: the one made from that main.dol, or else
// one derived from the profile of another release of the game (see SameGame and DeriveEffectProfile). Returns nothing
// if neither exists. Without a game ID, as for a loose main.dol, only the first is found. Other games' rips play
// without effects.
std::optional<EffectProfile> FindEffectProfile(std::span<const uint8_t> dol, const std::string& game_id);

// Finds `source`'s functions in `dol` by their signatures. DeriveProfile finds a driver profile's functions the same
// way. The nearest of the functions and anchors that matched once places a function that matches in several places.
// Returns the profile with the addresses found, or nothing if `source` has no signatures or a function isn't there.
std::optional<EffectProfile> DeriveEffectProfile(const EffectProfile& source, std::span<const uint8_t> dol);

// The effects of the game with the effect profile `profile` for `archive`. The archive's file name in `archive_path`
// picks them. Empty for a game without an effect profile, and for an archive that the game plays without effects.
nw4r::ArchiveEffects FindEffects(const std::optional<EffectProfile>& profile, const std::string& archive_path,
                                 const ntdlib::rsar::SoundArchive& archive);

} // namespace wsf::rip
