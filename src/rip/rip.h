// SPDX-License-Identifier: MIT

// WSF ripper. For a game with NW4R SND, it builds a .wsflib for one of the game's sound archives and one .miniwsf per
// sequence. Game mode rips the game's code, for games with a driver profile. Archive mode rips a sound archive and AX's
// microcode for WSF's model of the SDK's sound player. A JAudio2 game rips only in game mode: one .wsflib with the
// game's code and sound files, and one .miniwsf per sound.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/game_lengths.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/length.h"

#include "nw4r/util.h"
#include "wsf/format.h"

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

// The functions of a profile: main, then those from ai_init to update.
constexpr std::size_t kProfileFunctions = 18;

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
    std::array<FunctionSignature, kProfileFunctions> signatures;
    std::string derived_from = {}; // the profile that a derived profile comes from (see FindProfile), or nothing
};

const std::vector<GameProfile>& KnownProfiles();

// The options with which WSF loads a game: the Wii, WSF's name in messages, the containers that ntdlib's
// disc_containers reads (GCZ, CISO, WIA and RVZ), and titles for the NAND, such as WiiWare games, from a folder with
// the title's TMD and decrypted contents.
ntdlib::disc::GameOptions DiscOptions();

// The profile for a game: the one for its main.dol, or else one derived from the profile of another release or revision
// of the game. A game's releases share the first three characters of their game IDs. Their main.dols link the same
// functions at other addresses (see DeriveProfile). Returns nothing if neither exists.
std::optional<GameProfile> FindProfile(const ntdlib::disc::GameFiles& game);

// Finds `source`'s functions in `dol` by their signatures. A small function can match in several places. The functions
// of a library move together. The offset of the nearest function that matched once therefore picks the place. Returns
// the profile with the addresses found, or nothing if a function isn't there. `source` must have no init flag. The
// flag's address can't be found this way.
std::optional<GameProfile> DeriveProfile(const GameProfile& source, std::span<const uint8_t> dol);

// What game mode loads to play a sequence: the groups that hold its sequence and its bank (with the bank's waves), and
// the size of the sound heap they require.
struct SoundData
{
    uint32_t group;
    std::optional<uint32_t> bank_group; // the group with the bank, when `group` lacks it
    uint32_t heap_size;
};

// Returns the groups that hold a sequence's data: one group with both its sequence and its bank, or else the first
// group that holds each. Returns nothing if no group in the archive holds one of them.
std::optional<SoundData> FindSoundData(const ntdlib::rsar::SoundArchive& archive, uint32_t index);

// Game mode: builds the .wsflib for the sound archive at `archive_path` on the disc (no leading '/'). The library holds
// the game's code with main patched to branch to the driver, the driver, the partition's header and the archive.
// Returns nullopt and sets `error` if main.dol isn't a DOL, the archive's path is longer than the driver supports, or
// the archive can't be read.
std::optional<std::vector<uint8_t>> BuildLibrary(const ntdlib::disc::GameFiles& game, const GameProfile& profile,
                                                 const std::string& archive_path,
                                                 const ntdlib::rsar::SoundArchive& archive, const Tags& tags,
                                                 std::string* error = nullptr);

// Game mode: a .miniwsf that plays sequence `index`, with `data`, from `lib_name`.
std::vector<uint8_t> BuildMini(uint32_t index, const SoundData& data, const std::string& lib_name, Tags tags);

// The address of the JAudio2 driver's Z2Params (driver/jaudio.c): after the block's header of three words.
inline constexpr uint32_t kJAudioZ2Address = DriverBlock::kAddress + 0x0c;

// A JAudio2 game's driver profile (driver/jaudio.c). ntdlib's Z2 start-up takes the sizes and the game's functions. The
// driver adds the Wii's steps. The length analysis and the wave discovery watch the functions in `watch`.
struct JAudioProfile
{
    std::string name;
    std::string game_id;
    std::string dol_sha1;
    uint32_t main;                      // patched to branch to the driver
    ntdlib::flipper::z2::Profile z2;    // the start-up's sizes and the game's functions
    uint32_t sleep;                     // the game's sleep of the calling thread for an OSTime
    uint32_t reset_data;                // the variable that holds the reset data's address
    uint32_t reset_flag;                // the reset data's byte that lets the audio set-up read the disc
    uint32_t ios_version;               // the IOS that the game's TMD requests
    ntdlib::jaudio::GameWatch watch;    // what the length analysis and the wave discovery watch
    std::vector<uint32_t> static_waves; // the groups that the audio set-up loads itself: (system << 16) | group
};

const std::vector<JAudioProfile>& KnownJAudioProfiles();

// The JAudio2 profile for a game's main.dol, by the game's ID and the main.dol's SHA-1, or nothing.
std::optional<JAudioProfile> FindJAudioProfile(const ntdlib::disc::GameFiles& game);

// A JAudio2 game's sound files on the disc (paths without a leading '/'): its start-up archive (BAA), found by its
// header, and the RARC archives in the start-up archive's folder and below it, found by theirs.
struct JAudioFiles
{
    std::string baa;
    std::vector<std::string> archives;
};

// Finds the game's sound files. Returns nothing if the disc has no start-up archive.
std::optional<JAudioFiles> FindJAudioFiles(const ntdlib::disc::GameFiles& game);

// The disc's paths of the AW files that hold `groups`, each (system << 16) | group, in the start-up archive's folder or
// below it. Each path comes once, in the order of the files' names. If an AW file isn't there, returns nothing and sets
// `missing` to its name.
std::optional<std::vector<std::string>> FindAwFiles(const ntdlib::disc::GameFiles& game, const JAudioFiles& files,
                                                    const std::vector<ntdlib::jaudio::WaveSystem>& systems,
                                                    std::span<const uint32_t> groups, std::string* missing = nullptr);

// Game mode for a JAudio2 game: builds the .wsflib. It holds the game's code with main patched to branch to the JAudio2
// driver, the driver with the profile, the partition's header and the disc's files at `paths`. Returns nullopt and sets
// `error` if main.dol or a file can't be read.
std::optional<std::vector<uint8_t>> BuildJAudioLibrary(const ntdlib::disc::GameFiles& game,
                                                       const JAudioProfile& profile,
                                                       const std::vector<std::string>& paths, const Tags& tags,
                                                       std::string* error = nullptr);

// Game mode for a JAudio2 game: a .miniwsf that plays sound `sound_id` from `lib_name`, with the wave groups `waves`
// loaded, each (system << 16) | group. Throws std::invalid_argument for more than ntdlib's 64 groups.
std::vector<uint8_t> BuildJAudioMini(uint32_t sound_id, std::span<const uint32_t> waves, const std::string& lib_name,
                                     Tags tags);

// Archive mode: a .wsflib that holds the sound archive and AX's microcode as FILE chunks at the given paths. Its
// archive descriptor names the game's build of NW4R SND. The model follows that build.
std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& microcode_path, const ntdlib::Microcode& microcode,
                                         nw4r::SndBuild build, const Tags& tags);

// Archive mode: a .miniwsf that plays sequence `index` from `lib_name`.
std::vector<uint8_t> BuildArchiveMini(uint32_t index, const std::string& lib_name, Tags tags);

// Archive mode: a copy of the archive that keeps only the data that the sequences at `indices` play. That data is each
// sequence's file, and its bank's file and wave data. The rest of the groups' data becomes zeros. Zeros compress to
// almost nothing. The headers and the blocks keep their sizes. Every offset still holds.
std::vector<uint8_t> KeepSequenceData(const ntdlib::rsar::SoundArchive& archive, const std::vector<uint32_t>& indices);

// The file name for `library` in `folder`, from `names` (see xsflib::rip::FileNames::Library). An earlier rip into the
// folder may have written a library of that name for other sounds or in the other mode. Replacing it would leave that
// rip's minis without their data. A name is therefore used only when nothing in the folder has it, or when its file
// reads back as the same bytes. Otherwise the next name is tried: "<base>_2.wsflib" and so on.
std::string LibraryName(const std::filesystem::path& folder, xsflib::rip::FileNames& names, const std::string& base,
                        std::span<const uint8_t> library);

// Lists the data that sequence `index` requires but the archive doesn't hold. The sequence or bank may be in another
// file, or cut off by truncation. Empty when nothing is missing.
std::vector<std::string> MissingData(const ntdlib::rsar::SoundArchive& archive, uint32_t index);

// Whether the sound at `index` is music, by the rule that `wsfrip --bgm` uses. Music is a sequence whose label contains
// BGM, or one that plays on a music player. Games name their music players with BGM, MUSIC, SONG or FANFARE among the
// words separated by underscores (PLAYER_BGM, SONG_PLAYER). They do this even where the music's labels don't mention
// BGM. A player whose name also contains the word SE plays sound effects.
bool IsMusic(const ntdlib::rsar::SoundArchive& archive, uint32_t index);

// A sequence's length and fade in milliseconds, and whether it never makes a sound.
using Timing = xsflib::rip::Timing;

// Whether archive mode's model can drive `microcode`. The microcode must start and mix the model's first frame. Some
// games have older AX microcodes that accept other command lists. The model doesn't make those lists.
bool MicrocodeWorks(const ntdlib::Microcode& microcode, const ntdlib::rsar::SoundArchive& archive);

// Works out how long sequences play by running them on the model with AX's microcode on the DSP. When the model can't
// drive the microcode, the analyzer runs the model alone. The model then follows the voices' positions in place of the
// microcode (see nw4r::ArchiveModel::RunFrameWithoutDsp). Analyze can be called from several threads at once.
class LengthAnalyzer
{
public:
    // The model follows the game's `build` of NW4R SND.
    LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const ntdlib::Microcode& microcode, nw4r::SndBuild build);

    ~LengthAnalyzer();

    // A looping sequence gets two loops and a 10 s fade. Any other sequence runs to its end or to where it falls silent
    // for good, for at most 600 s. A sequence that never makes a sound is marked silent and gets the 0.5 s that follows
    // a sequence's last sound. Returns nullopt when the length can't be worked out. No length tag is written then.
    // Throws std::bad_alloc when memory runs out.
    std::optional<Timing> Analyze(uint32_t sound_index) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace wsf::rip
