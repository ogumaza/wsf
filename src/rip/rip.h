// SPDX-License-Identifier: MIT

// WSF ripper. Builds a .wsflib for one of a game's sound archives and one .miniwsf per sequence. Game mode rips the
// game's code, for games with a driver profile. Archive mode rips a sound archive and AX's microcode for WSF's model of
// the SDK's sound player.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ax/microcode.h"
#include "disc/game.h"
#include "nw4r/util.h"
#include "rsar/formats.h"
#include "wsf/format.h"

namespace wsf::rip
{

// Game-specific addresses for the driver (see driver/driver.c): the game's functions in its main.dol.
struct GameProfile
{
    const char* name;
    const char* game_id;  // "RSPE01"
    const char* dol_sha1; // the main.dol that the addresses belong to
    uint32_t main;        // patched to branch to the driver
    uint32_t ai_init, ax_init, alloc, sleep_ticks, init_sound_system;
    uint32_t archive_ctor, archive_open, archive_load_info;
    uint32_t player_ctor, player_mem_size, player_strm_size, player_setup;
    uint32_t heap_ctor, heap_create, load_group, start_sound, update;
    uint32_t startable_offset; // SoundArchivePlayer's SoundStartable base
    uint32_t ios_version;      // the IOS that the game's TMD requests
    uint32_t init_flag;        // the sound system's flag byte, checked by Setup in 2008 builds, or 0
};

const std::vector<GameProfile>& KnownProfiles();

// The profile for a game's main.dol, or nullptr if it isn't supported.
const GameProfile* FindProfile(const disc::GameFiles& game);

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
std::optional<SoundData> FindSoundData(const rsar::SoundArchive& archive, uint32_t index);

// Game mode: builds the .wsflib for the sound archive at `archive_path` on the disc (no leading '/'). The library holds
// the game's code with main patched to branch to the driver, the driver, the partition's header and the archive.
// Returns nullopt and sets `error` if main.dol can't be read.
std::optional<std::vector<uint8_t>> BuildLibrary(const disc::GameFiles& game, const GameProfile& profile,
                                                 const std::string& archive_path, const rsar::SoundArchive& archive,
                                                 const Tags& tags, std::string* error = nullptr);

// Game mode: a .miniwsf that plays sequence `index`, with `data`, from `lib_name`.
std::vector<uint8_t> BuildMini(uint32_t index, const SoundData& data, const std::string& lib_name, Tags tags);

// Archive mode: a .wsflib that holds the sound archive and AX's microcode as FILE chunks at the given paths. Its
// archive descriptor names the game's build of NW4R SND. The model follows that build.
std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& microcode_path, const ax::Microcode& microcode,
                                         nw4r::SndBuild build, const Tags& tags);

// Archive mode: a .miniwsf that plays sequence `index` from `lib_name`.
std::vector<uint8_t> BuildArchiveMini(uint32_t index, const std::string& lib_name, Tags tags);

// Lists the data that sequence `index` requires but the archive doesn't hold. The sequence or bank may be in another
// file, or cut off by truncation. Empty when nothing is missing.
std::vector<std::string> MissingData(const rsar::SoundArchive& archive, uint32_t index);

// Whether the sound at `index` is music, by the rule that `wsfrip --bgm` uses. Music is a sequence whose label contains
// BGM, or one that plays on a music player. Games name their music players with BGM, MUSIC, SONG or FANFARE among the
// words separated by underscores (PLAYER_BGM, SONG_PLAYER). They do this even where the music's labels don't mention
// BGM. A player whose name also contains the word SE plays sound effects.
bool IsMusic(const rsar::SoundArchive& archive, uint32_t index);

// Filenames for one rip, valid on Windows, macOS and Linux and unique even on case-insensitive filesystems.
class FileNames
{
public:
    // "<base>.wsflib".
    std::string Library(const std::string& base);

    // "<index, four digits> <label>.miniwsf".
    std::string Mini(uint32_t index, const std::string& label);

private:
    std::string Unique(const std::string& stem, const std::string& extension);

    std::set<std::string> taken_; // the names used so far, in lowercase
};

// A sequence's length and fade in milliseconds.
struct Timing
{
    long long length_ms;
    long long fade_ms;
};

// Whether archive mode's model can drive `microcode`. The microcode must start and mix the model's first frame. Some
// games have older AX microcodes that accept other command lists. The model doesn't make those lists.
bool MicrocodeWorks(const ax::Microcode& microcode, const rsar::SoundArchive& archive);

// Works out how long sequences play by running them on the model with AX's microcode on the DSP. When the model can't
// drive the microcode, the analyzer runs the model alone. The model then follows the voices' positions in place of the
// microcode (see nw4r::ArchiveModel::RunFrameWithoutDsp). Analyze can be called from several threads at once.
class LengthAnalyzer
{
public:
    // The model follows the game's `build` of NW4R SND.
    LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const ax::Microcode& microcode, nw4r::SndBuild build);
    ~LengthAnalyzer();

    // A looping sequence gets two loops and a 10 s fade. Any other sequence runs to its end or to where it falls silent
    // for good, for at most 600 s. Returns nullopt when the length can't be worked out. No length tag is written then.
    // Throws std::bad_alloc when memory runs out.
    std::optional<Timing> Analyze(uint32_t sound_index) const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace wsf::rip
