// SPDX-License-Identifier: MIT

// Rips of the JAudio games (JAudio1 and JAudio2). They rip only in game mode: one .wsflib with the game code and sound
// files, and one .miniwsf per sound. The games' driver profiles (jaudio_profiles.cpp), their sound files, their sounds,
// and the library and minis of a rip (jaudio.cpp).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/game_lengths.h"
#include "ntdlib/jaudio/jac_effects.h"
#include "ntdlib/jaudio/jungle_beat_states.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/jaudio/z2_starts.h"
#include "ntdlib/jaudio/z2_states.h"

#include "wsf/format.h"

namespace wsf::rip
{

// The address of Z2Params in driver/jaudio.c's block: after the block's header of three words. Every JAudio driver's
// block starts the same way. After the header come the sound, its handle and its wave groups, at Z2Params' offsets. A
// .miniwsf and the length analysis therefore write and read them at the same addresses for every driver.
inline constexpr uint32_t kJAudioZ2Address = DriverBlock::kAddress + 0x0c;

// The address of the words of a profile's parameters: where Z2Params' profile starts, after the wave list. A driver
// other than kZ2 reads its parameters there too.
inline constexpr uint32_t kJAudioParamsAddress = kJAudioZ2Address + ntdlib::flipper::z2::kProfileOffset;

// The OS functions that driver/jaudio.c's sleep calls. The driver sleeps with them in a game that has no sleep of the
// calling thread.
struct JAudioAlarm
{
    uint32_t set_alarm = 0;          // OSSetAlarm
    uint32_t disable_interrupts = 0; // OSDisableInterrupts
    uint32_t restore_interrupts = 0; // OSRestoreInterrupts
    uint32_t suspend_thread = 0;     // OSSuspendThread
    uint32_t resume_thread = 0;      // OSResumeThread
};

// The JAudio2 functions and variables that driver/jaudio.c's functions for one wave group use. The driver loads and
// erases a group with them in a game whose scene manager has no functions for one group.
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
    // archive (`sound_archive`) and the sequence archive (`sequence_archive`), and FindJAudio1Files finds them. The
    // game's sound layer plays the music. ReadPikmin2Sounds lists Pikmin 2's sounds.
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

// A Jac game's parts and effects (ntdlib/jaudio/jac_effects.h).
using ntdlib::jaudio::JacEffect;
using ntdlib::jaudio::JacEffectId;
using ntdlib::jaudio::JacPart;
using ntdlib::jaudio::kJacFirstPiece;

// The first bit of an effect's value that gives the random numbers that the driver draws before it starts the effect
// (JAudioProfile::random_starts). New Play Control! Pikmin 2's effect IDs leave bits 16 to 30 clear.
inline constexpr uint32_t kRandomDrawsShift = 20;

// An effect that ends at once on some draws of the sound library's random numbers, and the draws from the generator's
// first state that come before the first draw on which it plays (see JAudioProfile::random_starts).
struct RandomStart
{
    uint32_t id; // the effect's sound ID
    uint8_t draws;
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

    // A Jac game's catalogue of sound effects in its code (none if `jac_effects.table` is 0), the effects that the
    // game's code starts itself, and the effects that the ripper leaves out (ntdlib::jaudio::ListJacEffects).
    ntdlib::jaudio::JacEffectRules jac_effects = {};

    // The wave groups that a Jac game loads for its effects, each (system << 16) | group. They are an effect's groups.
    std::vector<uint32_t> effect_groups = {};

    // The path of the name table (.asn) of an AAF game's sounds on the disc, or empty for none.
    std::string sound_names = {};

    uint32_t main;                        // patched to branch to the driver
    ntdlib::flipper::z2::Profile z2 = {}; // the start-up's sizes and the game's functions
    uint32_t sleep = 0;                   // the game's sleep of the calling thread for an OSTime, or 0
    uint32_t reset_data = 0;              // the variable that holds the reset data's address
    uint32_t reset_flag = 0;              // the reset data's byte that lets the audio set-up read the disc
    JAudioAlarm alarm = {};               // for a game whose sleep is 0
    JAudioWaveArcs wave_arcs = {};        // for a game whose z2.load_se_wave, z2.load_bgm_wave and z2.wave_status are 0
    uint32_t full_ring_count = 0;         // JASDriver::finishDSPFrame's count of full rings, or 0 (driver/jaudio.c)
    uint32_t ios_version;                 // the IOS that the game's TMD requests
    ntdlib::jaudio::GameWatch watch;      // what the length analysis and the wave discovery watch
    std::vector<uint32_t> static_waves;   // the groups that the audio set-up loads itself: (system << 16) | group

    // The driver that the game's rips hold. driver/jaudio.c (kZ2) takes `z2` and the Wii's fields above.
    JAudioDriver driver = JAudioDriver::kZ2;

    // Another driver takes these words as its parameters, in the order of its block after the wave list.
    std::vector<uint32_t> params = {};

    // A driver that plays a sound beside the ripped one writes that sound's JAISound to the word of `params` at index
    // `background_sound`. The word holds 0 while the driver plays no such sound. The length analysis leaves out that
    // sound's tracks. -1 for a driver without such a word. A JASTrack holds its parent at `track_parent_offset`. New
    // Play Control! Donkey Kong Jungle Beat's chord effects play over the game's music (see driver/jungle_beat.c).
    int32_t background_sound = -1;
    uint32_t track_parent_offset = 0;

    // A driver that can start a sound with the player as Luigi reads a word of `params` at index `luigi_word`. The
    // .miniwsf of a Luigi variant sets it to 1. The sounds of `luigi_group` (the section in the high byte and the group
    // in the low byte) get the variants, labelled with "_Luigi". -1 for a driver without such a word. In Luigi mode,
    // the player's voices in the Galaxy games play Luigi's waves.
    int32_t luigi_word = -1;
    uint16_t luigi_group = 0;

    // The sound files that the driver loads besides the start-up archive and the wave archives, by their paths on the
    // disc. A rip's library holds them. With none listed, it holds the archives that FindJAudioFiles finds.
    std::vector<std::string> files = {};

    // The effects that draw a random number and end at once on some draws, for a driver that draws numbers before the
    // start (driver/pikmin2.c). From the generator's first state, the first draw ends each of them. The list of sounds
    // gives such an effect with its draws at kRandomDrawsShift of its ID. Its label stays the same.
    std::vector<RandomStart> random_starts = {};

    // The pieces whose tracks the game mutes as it starts them, for driver/jaudio.c with the music manager in `z2`.
    // ReadJAudioSounds gives each piece the children to mute (JAudioSound::piece_state). A start with a suffix becomes
    // a sound after its piece, named with the suffix. Other profiles have none.
    std::span<const ntdlib::jaudio::MutedStart> muted_starts = {};

    // The states in which the game plays a piece once it has started, and the states that its scenes start more pieces
    // in, for the same driver. A state without a suffix gives its piece the state (JAudioSound::piece_state). A state
    // with a suffix becomes a sound after its piece, named with the suffix. Other profiles have none.
    std::span<const ntdlib::jaudio::MusicState> piece_states = {};

    // The states that the game's code sets for its music during play, for driver/jungle_beat.c. ReadJAudioSounds puts a
    // piece's states after the piece. Each state shares the piece's number. Other profiles have none.
    std::span<const ntdlib::jaudio::JungleBeatState> layer_states = {};

    // The chord effects that the game plays beside a piece of music while an object keeps them playing, for
    // driver/jungle_beat.c. ReadJAudioSounds puts a piece's layers after the piece and its states. Each layer shares the
    // piece's number. The driver's parameters end with the layers' stages and records.
    std::span<const ntdlib::jaudio::JungleBeatChordLayer> chord_layers = {};
};

const std::vector<JAudioProfile>& KnownJAudioProfiles();

// The JAudio profile for a game's main.dol, by the game's ID and the main.dol's SHA-1, or nothing.
std::optional<JAudioProfile> FindJAudioProfile(const ntdlib::disc::GameFiles& game);

// The sound library of a JAudio game with the profile `profile`: the one that the profile names, or JAudio2 for a game
// without a profile. Without a profile, the ripper finds only a JAudio2 game's sound files (see FindJAudio2Files).
JAudioLibrary SoundLibraryOf(const JAudioProfile* profile);

// A JAudio game's sound files on the disc, by their paths without a leading '/'. `baa` is the start-up archive, or a
// Jac game's bank file. `archives` holds the other sound archives. FindJAudio2Files finds a JAudio2 game's files.
// FindJAudio1Files finds a JAudio1 game's.
struct JAudioFiles
{
    std::string baa;
    std::vector<std::string> archives;
};

// Finds a JAudio2 game's sound files: its start-up archive (BAA), found by its header, and the RARC archives in the
// start-up archive's folder and below it, found by theirs. A start-up archive may be compressed with Yaz0. Its header
// is then the header of the decoded data. Returns nothing if the disc has no start-up archive.
std::optional<JAudioFiles> FindJAudio2Files(const ntdlib::disc::GameFiles& game);

// Reads the sound file at `path` on the disc into `data` as the game reads it: decoded from Yaz0 if it's compressed.
// Returns false if the file can't be read or decoded.
bool ReadSoundFile(const ntdlib::disc::GameFiles& game, const std::string& path, std::vector<uint8_t>& data);

// The disc's files that every library of a JAudio game holds besides its AW files: the start-up archive, then the other
// sound files that the driver loads. The other files are the profile's `files`, or else the RARC archives beside the
// start-up archive. For a Jac game, the bank file takes the start-up archive's place, and the sequence archive is among
// the other files.
std::vector<std::string> LibraryFiles(const JAudioFiles& files, const JAudioProfile& profile);

// The disc's paths of the AW files that hold `groups`, each (system << 16) | group, in the start-up archive's folder or
// below it. Each path comes once, in the order of the files' names. If an AW file isn't there, returns nothing and sets
// `missing` to its name.
std::optional<std::vector<std::string>> FindAwFiles(const ntdlib::disc::GameFiles& game, const JAudioFiles& files,
                                                    const std::vector<ntdlib::jaudio::WaveSystem>& systems,
                                                    std::span<const uint32_t> groups, std::string* missing = nullptr);

// Game mode for a JAudio game: builds the .wsflib. It holds the game's code with main patched to branch to the
// profile's driver, the driver with the profile, the partition's header and the disc's files at `paths`. Returns
// nullopt and sets `error` if main.dol isn't a DOL, WSF has no such driver, the profile has more parameters than the
// driver's block holds, a file can't be read, or the library is larger than a player loads.
std::optional<std::vector<uint8_t>> BuildJAudioLibrary(const ntdlib::disc::GameFiles& game,
                                                       const JAudioProfile& profile,
                                                       const std::vector<std::string>& paths, const Tags& tags,
                                                       std::string* error = nullptr);

// Game mode for a JAudio game: a .miniwsf that plays sound `sound_id` from `lib_name`, with the wave groups `waves`
// loaded, each (system << 16) | group. `state`, if given, is the state that driver/jaudio.c gives the piece after it
// starts the sound (JAudioSound::piece_state). With a `luigi_word` of 0 or more, the .miniwsf sets that word of the
// profile's `params` to 1: it plays the sound's Luigi variant (JAudioProfile::luigi_word). Throws std::invalid_argument
// for more than ntdlib's 64 groups.
std::vector<uint8_t> BuildJAudioMini(uint32_t sound_id, std::span<const uint32_t> waves, const std::string& lib_name,
                                     Tags tags, const ntdlib::flipper::z2::PieceState* state = nullptr,
                                     int32_t luigi_word = -1);

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

    // The state that the driver gives the piece after it starts the sound: the children of track 0 that it mutes or
    // plays at a volume between 0 and 1, and the status on port 9 (JAudioProfile::muted_starts and piece_states). Then
    // whether the sound's files take the number of the sound before it: a piece's second start or another state.
    ntdlib::flipper::z2::PieceState piece_state = {};
    bool shares_number = false;

    // The label of the sound that a variant plays, for the wsf_sound tag, or empty for the sound itself.
    std::string sound_label = {};
};

// A JAudio2 game's sounds, from the start-up archive's sound table, with the names of the table's sections. A sound
// without a name is labelled with its ID. Music is a sound whose label contains BGM, or a sound of the section that
// the name table calls BGM. ntdlib::rsar::IsMusic takes the same labels for NW4R SND. Throws xsflib::FormatError if
// the archive is damaged.
std::vector<JAudioSound> ReadJAudio2Sounds(std::span<const uint8_t> baa);

// A JAudio1 game's sound files (JAudioLibrary::kJac and kAaf), at the paths that the profile gives: a Jac game's bank
// file or an AAF start-up archive in `baa`, and the sequence archive in `archives`. Returns nothing if either file
// isn't on the disc.
std::optional<JAudioFiles> FindJAudio1Files(const ntdlib::disc::GameFiles& game, const JAudioProfile& profile);

// The sound files of a JAudio game with a profile, found by the function for the profile's library: FindJAudio1Files
// for a JAudio1 game, or FindJAudio2Files for a JAudio2 game. Returns nothing and sets `error` if the disc lacks them.
std::optional<JAudioFiles> FindJAudioFiles(const ntdlib::disc::GameFiles& game, const JAudioProfile& profile,
                                           std::string* error = nullptr);

// The wave systems of a Jac game's bank file. The file starts with the offset and the count of the wave systems'
// entries, then the same for the banks' entries. An entry gives a wave system's or a bank's offset and size in the
// file. A wave system's number is the entry's place in the list. A size of 0 means none. Throws xsflib::FormatError if
// the file or a wave system is damaged.
std::vector<ntdlib::jaudio::WaveSystem> ReadJacWaveSystems(std::span<const uint8_t> bank_file);

// The wave systems of a game whose sound library is `library`, from `start_up`: JAudio2's start-up archive, a Jac
// game's bank file (ReadJacWaveSystems) or an AAF start-up archive. Throws xsflib::FormatError if the file is damaged.
std::vector<ntdlib::jaudio::WaveSystem> ReadWaveSystems(JAudioLibrary library, std::span<const uint8_t> start_up);

// A Jac game's sounds: its pieces of music, from the sequence index in `dol` at the profile's `sequence_index`, then
// its sound effects. The index is a BARC: a 32-byte header ("BARC", four bytes, a word, the number of files and the
// sequence archive's name), then a 32-byte entry for each file. An entry holds the file's name in up to 14 bytes, two
// bytes, two words, and the file's offset and size in the archive. Files 0 and 1 are the effects sequence and the
// system's events. The pieces are the other files that aren't empty. A piece's ID is 0x80000000 | the file's number.
// The label is the file's name without the extension. The game plays group 0 of the wave system with the piece's
// number. The profile's piece in parts gives a sound for each of the piece's parts in place of the piece. A part's ID
// is 0x80000000 | the part's number << 8 | the piece's number. A part's label is the piece's label, then "-" and the
// part's number. A part plays the wave group in the part's entry of the profile.
// The effects are the rows of the profile's catalogue of effects, then its other effects, without its left-out
// effects. A row's name labels its effect. Actions of events of different types can play the same effect. The first
// row that plays an effect names it. An event of the sounding type starts another effect as the game creates it. The
// rip plays that type's effects on an event of the quiet type, by the same entry of the table of actions. An other
// effect is labelled by its kind and its number in hexadecimal, as system-19 or player-800c. The formation's sound is
// labelled formation. An effect plays the profile's `effect_groups`. Throws xsflib::FormatError if `dol` isn't a DOL,
// if the index lies outside the DOL's sections, isn't a BARC or ends early, or if the catalogue of effects or the table
// of actions runs past its section or has a row of an unknown kind.
std::vector<JAudioSound> ReadJacSounds(std::span<const uint8_t> dol, const JAudioProfile& profile);

// Pikmin 2's sounds (JAudioDriver::kPikmin2): its pieces of music and their variants, then the effects of the start-up
// archive's sound table. A piece is a file of the sequence archive, named without the extension. A variant plays a
// piece with another conductor or from another part. It is named after the conductor or the part. The sound table's
// sequences are left out: the game's sound layer gives them their data, and the pieces take their place. A piece's ID
// is 0x80000000 | the piece's number in the driver's table. Its wave groups are group 0 and the group that the game's
// scene loads, or none for a piece that the game never plays. An effect plays in the effects sequence. The name table
// `names` names it. Without a name there, the effect is named se<category>-<index>. Its wave groups are the profile's
// `static_waves`. The sequence archive may be compressed with Yaz0. Throws xsflib::FormatError if a file is damaged, or
// if the sequence archive lacks a piece's file.
std::vector<JAudioSound> ReadPikmin2Sounds(std::span<const uint8_t> aaf, std::span<const uint8_t> sequence_archive,
                                           std::span<const uint8_t> names, const JAudioProfile& profile);

// A JAudio game's sounds, read by the function for its sound library: ReadJAudio2Sounds from the start-up archive
// `start_up`, ReadJacSounds from main.dol, or ReadPikmin2Sounds from the AAF start-up archive in `start_up`, the
// sequence archive in `files` and the profile's name table. `profile` is null for a JAudio2 game without one. Returns
// nothing and sets `error` if a file can't be read. Throws xsflib::FormatError if a file is damaged.
std::optional<std::vector<JAudioSound>> ReadJAudioSounds(const ntdlib::disc::GameFiles& game, const JAudioFiles& files,
                                                         const JAudioProfile* profile,
                                                         std::span<const uint8_t> start_up,
                                                         std::string* error = nullptr);

// What the length analysis watches for sound `sound_id`: the profile's `watch`. Pikmin 2's AutoBgm pieces never repeat.
// Their root tracks' polls of the game stay moves (see GameWatch's `idle_in_calls`).
ntdlib::jaudio::GameWatch WatchOf(const JAudioProfile& profile, uint32_t sound_id);

} // namespace wsf::rip
