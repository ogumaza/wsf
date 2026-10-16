// SPDX-License-Identifier: MIT

// WSF ripper. For a game with NW4R SND, it builds a .wsflib for one of the game's sound archives and one .miniwsf per
// sequence. Game mode rips the game's code, for games with a driver profile. Archive mode rips a sound archive and AX's
// microcode for WSF's model of the SDK's sound player. A JAudio game (JAudio1 or JAudio2) rips only in game mode: one
// .wsflib with the game's code and sound files, and one .miniwsf per sound.

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

#include "nw4r/arrangement.h"
#include "nw4r/effect_setup.h"
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
// disc_containers reads (GCZ, CISO, WIA and RVZ), titles for the NAND, such as WiiWare games, from a folder with the
// title's TMD and decrypted contents, and the sound archives inside CRI's CPK archives.
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

// The effects that a game puts on the aux buses for the sounds of one of its sound archives, and the sends that it sets
// on some of those sounds (docs/wsf.md, "Effects"). `dol_sha1` names the main.dol whose effect profile gives them.
struct ArchiveEffects
{
    std::vector<nw4r::EffectSetup> effects;
    std::vector<nw4r::SoundSend> sends;
    std::string dol_sha1;
};

// Whether WSF has an effect profile for the main.dol with `dol_sha1`. An effect profile gives the effects that the game
// sets up for each of its sound archives. Only the release whose main.dol it was made from has one. Other games' rips
// play without effects.
bool HasEffectProfile(const std::string& dol_sha1);

// The effects of the game whose main.dol has `dol_sha1` for `archive`. The archive's file name in `archive_path` picks
// them. Empty for a game without an effect profile, and for an archive that the game plays without effects.
ArchiveEffects FindEffects(const std::string& dol_sha1, const std::string& archive_path,
                           const ntdlib::rsar::SoundArchive& archive);

// Game mode: builds the .wsflib for the sound archive at `archive_path` on the disc (no leading '/'). The library holds
// the game's code with main patched to branch to the driver, the driver, the partition's header and the archive. The
// driver sets up the effects of `effects` that a sound gets when no effect lists it, with the game's functions. Returns
// nullopt and sets `error` if main.dol isn't a DOL, the archive's path is longer than the driver supports, the archive
// can't be read, or the driver can't set up all of the effects.
std::optional<std::vector<uint8_t>> BuildLibrary(const ntdlib::disc::GameFiles& game, const GameProfile& profile,
                                                 const std::string& archive_path,
                                                 const ntdlib::rsar::SoundArchive& archive, const Tags& tags,
                                                 std::string* error = nullptr, const ArchiveEffects& effects = {});

// Game mode: a .miniwsf that plays sequence `index`, with `data`, from `lib_name`. It tells the driver whether the
// sound's variables pick the presets of `effects`, and the send that the game sets on the sound. For a sound that gets
// other effects than the library's, it gives the driver those effects.
std::vector<uint8_t> BuildMini(uint32_t index, const SoundData& data, const std::string& lib_name, Tags tags,
                               const ArchiveEffects& effects = {});

// The address of the JAudio2 driver's Z2Params (driver/jaudio.c): after the block's header of three words. Every
// JAudio2 driver's block starts the same way. After the header come the sound, its handle and its wave groups, at
// Z2Params' offsets. A .miniwsf and the length analysis therefore write and read them at the same addresses for every
// driver.
inline constexpr uint32_t kJAudioZ2Address = DriverBlock::kAddress + 0x0c;

// The OS functions that the JAudio2 driver's sleep calls (see driver/jaudio.c). The driver sleeps with them in a game
// that has no sleep of the calling thread.
struct JAudioAlarm
{
    uint32_t set_alarm = 0;          // OSSetAlarm
    uint32_t disable_interrupts = 0; // OSDisableInterrupts
    uint32_t restore_interrupts = 0; // OSRestoreInterrupts
    uint32_t suspend_thread = 0;     // OSSuspendThread
    uint32_t resume_thread = 0;      // OSResumeThread
};

// The JAudio2 functions and variables that the JAudio2 driver's functions for one wave group use (see
// driver/jaudio.c). The driver loads and erases a group with them in a game whose scene manager has no functions for
// one group.
struct JAudioWaveArcs
{
    uint32_t section_heap = 0; // the variable that holds JAUSectionHeap's instance
    uint32_t bank_table = 0;   // the offset in the section heap of two words: the wave banks' array, then its length
    uint32_t load = 0;         // JASWaveArc::load
    uint32_t load_tail = 0;    // JASWaveArc::loadTail
    uint32_t erase = 0;        // JASWaveArc::erase
};

// A JAudio game's sound library, by the sound files that the game reads at start-up. The ripper lists the game's
// sounds and finds the wave systems in those files.
enum class JAudioLibrary : uint8_t
{
    // JAudio2. The ripper finds the start-up archive (BAA) by its header. The archive's sound table lists the sounds.
    // The archive also holds the wave systems. The RARC archives beside it hold the sequences.
    kJAudio2,
    // An early JAudio1. The game calls it through C functions (Jac, Jaf, Jaq and Jam). Pikmin is one such game. The
    // profile gives the paths of the sound files (`sound_archive` and `sequence_archive`). The bank file holds the
    // instrument banks and the wave systems. main.dol holds the sequence index, at `sequence_index`. ReadJacSounds
    // lists the pieces of music.
    kJac,
    // A later JAudio1, with a start-up archive (AAF) that holds the sound table, the instrument banks and the wave
    // systems. New Play Control! Pikmin 2 is one such game. As for kJac, the profile gives the paths of the start-up
    // archive (`sound_archive`) and the sequence archive (`sequence_archive`), and FindJacFiles finds them. The game's
    // sound layer plays the music. ReadPikmin2Sounds lists Pikmin 2's sounds.
    kAaf,
};

// The game-mode drivers for JAudio games. Each is a source file in driver/. driver/build.py builds each into a blob in
// src/rip/.
enum class JAudioDriver : uint8_t
{
    // driver/jaudio.c: ntdlib's start-up for Z2, the sound layer that Twilight Princess puts on JAudio2. The profile's
    // `z2` and its Wii fields fill in the driver's block.
    kZ2,
    // driver/jungle_beat.c: New Play Control! Donkey Kong Jungle Beat's start-up of JAudio2, with the game's BGM
    // player. The profile's `params` fill in the driver's block.
    kJungleBeat,
    // driver/pikmin.c: the start-up of the JAudio1 in New Play Control! Pikmin. The driver calls the game's C functions
    // (kJac) and takes its parameters from `params`.
    kPikmin,
    // driver/pikmin2.c: New Play Control! Pikmin 2's sound layer on JAudio1. The profile's `params` fill in the
    // driver's block. They end with the driver's table of the game's pieces of music.
    kPikmin2,
    // driver/galaxy.c: the start-up of Super Mario Galaxy, whose audio system sits on JAudio2. The profile's `params`
    // fill in the driver's block.
    kGalaxy,
    // driver/galaxy2.c: the start-up of Super Mario Galaxy 2, through the game's file loader and audio wrapper. The
    // profile's `params` fill in the driver's block.
    kGalaxy2,
};

// A part of a Jac game's piece in parts (see JAudioProfile::parted_piece): its number, and the wave system and group
// that the game loads for it.
struct JacPart
{
    uint8_t number = 0;
    uint8_t system = 0;
    uint8_t group = 0;
};

// A JAudio game's driver profile. For driver/jaudio.c, ntdlib's Z2 start-up takes the sizes and the game's functions,
// and the driver adds the Wii's steps. Another driver takes its parameters from `params`. The length analysis and the
// wave discovery watch the functions in `watch`.
struct JAudioProfile
{
    std::string name;
    std::string game_id;
    std::string dol_sha1;
    // The sound library. A JAudio1 profile also gives the paths of the game's sound files on the disc and the address
    // of the sequence index in main.dol (see JAudioLibrary).
    JAudioLibrary library = JAudioLibrary::kJAudio2;
    std::string sound_archive = {};
    std::string sequence_archive = {};
    uint32_t sequence_index = 0;
    // A Jac game's piece in parts (0 for none), and the parts that the game plays. The sound library plays that piece
    // from its start-up. The game plays a part by writing the part's number to a port of the piece's root track. The
    // game loads the part's wave group first. ReadJacSounds lists the parts in place of the piece.
    uint32_t parted_piece = 0;
    std::vector<JacPart> parts = {};
    // The path of the name table (.asn) of an AAF game's sounds on the disc, or empty for none.
    std::string sound_names = {};
    uint32_t main;                      // patched to branch to the driver
    ntdlib::flipper::z2::Profile z2;    // the start-up's sizes and the game's functions
    uint32_t sleep;                     // the game's sleep of the calling thread for an OSTime, or 0
    uint32_t reset_data;                // the variable that holds the reset data's address
    uint32_t reset_flag;                // the reset data's byte that lets the audio set-up read the disc
    JAudioAlarm alarm = {};             // for a game whose sleep is 0
    JAudioWaveArcs wave_arcs = {};      // for a game whose z2.load_se_wave, z2.load_bgm_wave and z2.wave_status are 0
    uint32_t ios_version;               // the IOS that the game's TMD requests
    ntdlib::jaudio::GameWatch watch;    // what the length analysis and the wave discovery watch
    std::vector<uint32_t> static_waves; // the groups that the audio set-up loads itself: (system << 16) | group
    // The driver that the game's rips hold. driver/jaudio.c (kZ2) takes `z2` and the Wii's fields above.
    JAudioDriver driver = JAudioDriver::kZ2;
    // Another driver takes these words as its parameters, in the order of its block after the wave list.
    std::vector<uint32_t> params = {};
    // The sound files that the driver loads besides the start-up archive and the wave archives, by their paths on the
    // disc. A rip's library holds them. With none listed, it holds the RARC archives that FindJAudioFiles finds.
    std::vector<std::string> files = {};
};

const std::vector<JAudioProfile>& KnownJAudioProfiles();

// The JAudio2 profile for a game's main.dol, by the game's ID and the main.dol's SHA-1, or nothing.
std::optional<JAudioProfile> FindJAudioProfile(const ntdlib::disc::GameFiles& game);

// A JAudio2 game's sound files on the disc (paths without a leading '/'): its start-up archive (BAA), found by its
// header, and the RARC archives in the start-up archive's folder and below it, found by theirs. A start-up archive may
// be compressed with Yaz0. Its header is then the header of the decoded data.
struct JAudioFiles
{
    std::string baa;
    std::vector<std::string> archives;
};

// Finds the game's sound files. Returns nothing if the disc has no start-up archive.
std::optional<JAudioFiles> FindJAudioFiles(const ntdlib::disc::GameFiles& game);

// Reads the start-up archive at `path` on the disc into `data`, decoded from Yaz0 if it's compressed. Returns false if
// the file can't be read or decoded.
bool ReadStartUpArchive(const ntdlib::disc::GameFiles& game, const std::string& path, std::vector<uint8_t>& data);

// The disc's paths of the AW files that hold `groups`, each (system << 16) | group, in the start-up archive's folder or
// below it. Each path comes once, in the order of the files' names. If an AW file isn't there, returns nothing and sets
// `missing` to its name.
std::optional<std::vector<std::string>> FindAwFiles(const ntdlib::disc::GameFiles& game, const JAudioFiles& files,
                                                    const std::vector<ntdlib::jaudio::WaveSystem>& systems,
                                                    std::span<const uint32_t> groups, std::string* missing = nullptr);

// Game mode for a JAudio2 game: builds the .wsflib. It holds the game's code with main patched to branch to the
// profile's driver, the driver with the profile, the partition's header and the disc's files at `paths`. Returns
// nullopt and sets `error` if main.dol or a file can't be read, or if WSF has no such driver.
std::optional<std::vector<uint8_t>> BuildJAudioLibrary(const ntdlib::disc::GameFiles& game,
                                                       const JAudioProfile& profile,
                                                       const std::vector<std::string>& paths, const Tags& tags,
                                                       std::string* error = nullptr);

// Game mode for a JAudio2 game: a .miniwsf that plays sound `sound_id` from `lib_name`, with the wave groups `waves`
// loaded, each (system << 16) | group. Throws std::invalid_argument for more than ntdlib's 64 groups.
std::vector<uint8_t> BuildJAudioMini(uint32_t sound_id, std::span<const uint32_t> waves, const std::string& lib_name,
                                     Tags tags);

// A sound of a JAudio game that a rip can play. `id` is the ID that the game passes to the sound library. `section` is
// the sound's section in the name table, or empty. `music` is true if `--bgm` selects the sound, and `stream` is true
// if the sound plays a stream. `waves` lists the wave groups that the game loads for the sound, each as
// (system << 16) | group. The ripper finds the groups of a sound with an empty list by wave discovery.
struct JAudioSound
{
    uint32_t id = 0;
    std::string label;
    std::string section = {};
    bool music = false;
    bool stream = false;
    std::vector<uint32_t> waves = {};
};

// A Jac game's sound files (JAudioLibrary::kJac): the bank file that the profile names in `baa`, and the sequence
// archive in `archives`. Returns nothing if either file isn't on the disc.
std::optional<JAudioFiles> FindJacFiles(const ntdlib::disc::GameFiles& game, const JAudioProfile& profile);

// The wave systems of a Jac game's bank file. The file starts with the offset and the count of the wave systems'
// entries, then the same for the banks' entries. An entry gives a wave system's or a bank's offset and size in the
// file. A wave system's number is the entry's place in the list. A size of 0 means none. Throws xsflib::FormatError if
// the file or a wave system is damaged.
std::vector<ntdlib::jaudio::WaveSystem> ReadJacWaveSystems(std::span<const uint8_t> bank_file);

// A Jac game's pieces of music, from the sequence index in `dol` at the profile's `sequence_index`. The index is a
// BARC: a 32-byte header ("BARC", four bytes, a word, the number of files and the sequence archive's name), then a
// 32-byte entry for each file. An entry holds the file's name in up to 14 bytes, two bytes, two words, and the file's
// offset and size in the archive. Files 0 and 1 are the effects sequence and the system's events. The pieces are the
// other files that aren't empty. A piece's ID is 0x80000000 | the file's number. The label is the file's name without
// the extension. The game plays group 0 of the wave system with the piece's number. The profile's piece in parts gives
// a sound for each of the piece's parts in place of the piece. A part's ID is 0x80000000 | the part's number << 8 | the
// piece's number. A part's label is the piece's label, then "-" and the part's number. A part plays the wave group in
// the part's entry of the profile. Throws xsflib::FormatError if `dol` isn't a DOL, or if the index lies outside the
// DOL's sections, isn't a BARC or ends early.
std::vector<JAudioSound> ReadJacSounds(std::span<const uint8_t> dol, const JAudioProfile& profile);

// Pikmin 2's sounds (JAudioDriver::kPikmin2): its pieces of music, then the effects of the start-up archive's sound
// table. A piece is a file of the sequence archive, named without the extension. The sound table's sequences are left
// out: the game's sound layer gives them their data, and the pieces take their place. A piece's ID is 0x80000000 |
// the piece's number in the driver's table. Its wave groups are group 0 and the group that the game's scene loads, or
// none for a piece that the game never plays. An effect plays in the effects sequence. The name table `names` names it.
// Without a name there, the effect is named se<category>-<index>. Its wave groups are the profile's `static_waves`. The
// sequence archive may be compressed with Yaz0. Throws xsflib::FormatError if a file is damaged, or if the sequence
// archive lacks a piece's file.
std::vector<JAudioSound> ReadPikmin2Sounds(std::span<const uint8_t> aaf, std::span<const uint8_t> sequence_archive,
                                           std::span<const uint8_t> names, const JAudioProfile& profile);

// What the length analysis watches for sound `sound_id`: the profile's `watch`. Pikmin 2's AutoBgm pieces never repeat.
// Their root tracks' polls of the game stay moves (see GameWatch's `idle_in_calls`).
ntdlib::jaudio::GameWatch WatchOf(const JAudioProfile& profile, uint32_t sound_id);

// Archive mode: a .wsflib that holds the sound archive and AX's microcode as FILE chunks at the given paths. Its
// archive descriptor names the game's build of NW4R SND, and gives the effects. The model follows that build.
std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& microcode_path, const ntdlib::Microcode& microcode,
                                         nw4r::SndBuild build, const Tags& tags, const ArchiveEffects& effects = {});

// Archive mode: a .miniwsf that plays sequence `index` from `lib_name`.
std::vector<uint8_t> BuildArchiveMini(uint32_t index, const std::string& lib_name, Tags tags);

// Archive mode: a copy of the archive that holds only the data that the sequences at `indices` play. That data is each
// sequence's file, and its bank's file and wave data, in the items where SoundArchive::File finds them. The header, the
// SYMB block and the INFO block keep their sizes. The FILE block holds only those items, group by group. Items of a
// group that hold the same bytes, such as banks that share a wave archive, share one copy. The INFO block's groups and
// items give their new places. Every other item starts at the copy's end. SoundArchive::File then skips it. Each kept
// file is read back from the copy and compared with the archive's. Throws xsflib::FormatError if one differs, or if the
// groups' entries don't come before the FILE block. Only a hand-made archive could cause either.
std::vector<uint8_t> KeepSequenceData(const ntdlib::rsar::SoundArchive& archive, const std::vector<uint32_t>& indices);

// A player loads a program of 1 GiB at most (xsflib's kMaxProgramSize). For a larger program of `size` bytes, returns
// the rip's message. The message gives both sizes. Returns nothing for a program that a player loads.
std::optional<std::string> ProgramTooLarge(std::size_t size);

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
// microcode (see nw4r::ArchiveModel::RunFrameWithoutDsp). A frame then has sound while any note plays, even on a track
// that the arrangement silences. Analyze can be called from several threads at once.
class LengthAnalyzer
{
public:
    // The model follows the game's `build` of NW4R SND. Each sequence plays in `arrangement`, with `effects`. A
    // sequence with effects ends where their output falls silent.
    LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const ntdlib::Microcode& microcode, nw4r::SndBuild build,
                   const nw4r::Arrangement& arrangement = {}, const ArchiveEffects& effects = {});

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
