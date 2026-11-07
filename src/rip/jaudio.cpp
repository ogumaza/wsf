// SPDX-License-Identifier: MIT

// The ripper's parts for games whose sound library is JAudio2, JAudio1 or Jac: the drivers' blocks, the games' sound
// files and sounds, Pikmin 2's pieces, and the library and minis. The length analysis and the wave discovery play a
// rip on the player (see rip/jaudio_analysis.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/flipper/wave_list.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/archive.h"
#include "ntdlib/jaudio/game_lengths.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio/wave_system.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"
#include "xsflib/text.h"

#include "rip/driver_blob.h"
#include "rip/galaxy2_driver_blob.h"
#include "rip/galaxy_driver_blob.h"
#include "rip/jaudio_driver_blob.h"
#include "rip/jungle_beat_driver_blob.h"
#include "rip/pikmin2_driver_blob.h"
#include "rip/pikmin_driver_blob.h"
#include "rip/rip.h"
#include "rip/sound_libraries.h"
#include "wsf/format.h"

namespace wsf::rip
{
namespace
{

// driver/jaudio.c's parameter block, from its address: the header, ntdlib's Z2Params, then the Wii's fields.
constexpr uint32_t kJAudioZ2Offset = kJAudioZ2Address - DriverBlock::kAddress;
constexpr uint32_t kJAudioWiiOffset = kJAudioZ2Offset + ntdlib::flipper::z2::kParamsSize;
constexpr uint32_t kJAudioSleepOffset = kJAudioWiiOffset;
constexpr uint32_t kJAudioRetraceTicksOffset = kJAudioWiiOffset + 4;
constexpr uint32_t kJAudioResetDataOffset = kJAudioWiiOffset + 8;
constexpr uint32_t kJAudioResetFlagOffset = kJAudioWiiOffset + 12;
constexpr uint32_t kJAudioAlarmOffset = kJAudioWiiOffset + 16;
constexpr uint32_t kJAudioWaveArcsOffset = kJAudioAlarmOffset + 20;

// The driver's replacements for a game that lacks the functions: the sleep, then the functions for one wave group.
constexpr uint32_t kJAudioDriverSleepOffset = kJAudioWaveArcsOffset + 20;
constexpr uint32_t kJAudioDriverLoadSeWaveOffset = kJAudioDriverSleepOffset + 4;
constexpr uint32_t kJAudioDriverLoadBgmWaveOffset = kJAudioDriverSleepOffset + 8;
constexpr uint32_t kJAudioDriverWaveStatusOffset = kJAudioDriverSleepOffset + 12;
constexpr uint32_t kJAudioDriverEraseBgmWaveOffset = kJAudioDriverSleepOffset + 16;
constexpr uint32_t kJAudioDriverEraseSeWaveOffset = kJAudioDriverSleepOffset + 20;

// The sound's start: the game's function and finishDSPFrame's count, then the driver's start.
constexpr uint32_t kJAudioGameStartSoundOffset = kJAudioDriverSleepOffset + 24;
constexpr uint32_t kJAudioFullRingCountOffset = kJAudioDriverSleepOffset + 28;
constexpr uint32_t kJAudioDriverStartSoundOffset = kJAudioDriverSleepOffset + 32;
static_assert(jaudio::kDriverParams == DriverBlock::kAddress && jaudio::kDriverBase == kDriverBase);

// Whether `data` starts with the four bytes of `magic`.
bool StartsWith(std::span<const uint8_t> data, const char* magic)
{
    return data.size() >= 4 && std::memcmp(data.data(), magic, 4) == 0;
}

// The folder of a path on the disc, with its '/' at the end, or "" for a file at the root.
std::string FolderOf(const std::string& path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// The words of a profile's parameters for a driver other than kZ2 start where the Z2 start-up's profile starts: after
// the wave list.
constexpr uint32_t kJAudioParamsOffset = kJAudioZ2Offset + ntdlib::flipper::z2::kProfileOffset;

// A JAudio driver's image: its blob, the address it's loaded at, its entry and its parameter block. Every driver's
// block is at DriverBlock::kAddress.
struct JAudioDriverImage
{
    std::span<const uint8_t> blob;
    uint32_t base;
    uint32_t entry;
    uint32_t params;
};

// The image of a profile's driver, or nothing for a driver that WSF doesn't have.
std::optional<JAudioDriverImage> ImageOf(JAudioDriver driver)
{
    switch (driver)
    {
    case JAudioDriver::kZ2:
        return JAudioDriverImage{jaudio::kDriverBlob, jaudio::kDriverBase, jaudio::kDriverEntry, jaudio::kDriverParams};
    case JAudioDriver::kJungleBeat:
        return JAudioDriverImage{jungle_beat::kDriverBlob, jungle_beat::kDriverBase, jungle_beat::kDriverEntry,
                                 jungle_beat::kDriverParams};
    case JAudioDriver::kPikmin:
        return JAudioDriverImage{pikmin::kDriverBlob, pikmin::kDriverBase, pikmin::kDriverEntry, pikmin::kDriverParams};
    case JAudioDriver::kPikmin2:
        return JAudioDriverImage{pikmin2::kDriverBlob, pikmin2::kDriverBase, pikmin2::kDriverEntry,
                                 pikmin2::kDriverParams};
    case JAudioDriver::kGalaxy:
        return JAudioDriverImage{galaxy::kDriverBlob, galaxy::kDriverBase, galaxy::kDriverEntry, galaxy::kDriverParams};
    case JAudioDriver::kGalaxy2:
        return JAudioDriverImage{galaxy2::kDriverBlob, galaxy2::kDriverBase, galaxy2::kDriverEntry,
                                 galaxy2::kDriverParams};
    }

    return std::nullopt;
}

// Fills in driver/jaudio.c's parameters in the driver's block at `params`: the profile's sizes and functions for
// ntdlib's start-up for Z2, and the Wii's fields.
void PutZ2DriverParams(uint8_t* params, const JAudioProfile& profile)
{
    const auto put = [params](uint32_t offset, uint32_t value)
    {
        xsflib::StoreBe32(params + offset, value);
    };

    const auto word = [params](uint32_t offset)
    {
        return xsflib::LoadBe32(params + offset);
    };

    // A game whose scene manager has no functions for one wave group gets the driver's functions in their place. A
    // game without a sleep of the calling thread gets the driver's sleep. The block gives the addresses of both. A game
    // with the functions runs the same instructions as before the driver had them.
    ntdlib::flipper::z2::Profile z2 = profile.z2;
    if (!z2.load_se_wave)
    {
        z2.load_se_wave = word(kJAudioDriverLoadSeWaveOffset);
        z2.load_bgm_wave = word(kJAudioDriverLoadBgmWaveOffset);
        z2.wave_status = word(kJAudioDriverWaveStatusOffset);
        z2.erase_bgm_wave = word(kJAudioDriverEraseBgmWaveOffset);
        z2.erase_se_wave = word(kJAudioDriverEraseSeWaveOffset);
    }

    // A game whose audio driver runs extra DSP frames at its start-up gets the driver's start of the sound. It waits
    // until those frames have run, then calls the game's start.
    if (profile.full_ring_count)
    {
        put(kJAudioGameStartSoundOffset, z2.start_sound);
        put(kJAudioFullRingCountOffset, profile.full_ring_count);
        z2.start_sound = word(kJAudioDriverStartSoundOffset);
    }

    const std::vector<uint32_t> words = z2.Words();
    for (std::size_t i = 0; i < words.size(); i++)
    {
        put(kJAudioZ2Offset + ntdlib::flipper::z2::kProfileOffset + static_cast<uint32_t>(i * 4), words[i]);
    }

    put(kJAudioSleepOffset, profile.sleep ? profile.sleep : word(kJAudioDriverSleepOffset));
    put(kJAudioRetraceTicksOffset, kFrameTicks);
    put(kJAudioResetDataOffset, profile.reset_data);
    put(kJAudioResetFlagOffset, profile.reset_flag);
    const uint32_t alarm[] = {profile.alarm.set_alarm, profile.alarm.disable_interrupts,
                              profile.alarm.restore_interrupts, profile.alarm.suspend_thread,
                              profile.alarm.resume_thread};
    for (std::size_t i = 0; i < std::size(alarm); i++)
    {
        put(kJAudioAlarmOffset + static_cast<uint32_t>(i * 4), alarm[i]);
    }

    const uint32_t wave_arcs[] = {profile.wave_arcs.section_heap, profile.wave_arcs.bank_table, profile.wave_arcs.load,
                                  profile.wave_arcs.load_tail, profile.wave_arcs.erase};
    for (std::size_t i = 0; i < std::size(wave_arcs); i++)
    {
        put(kJAudioWaveArcsOffset + static_cast<uint32_t>(i * 4), wave_arcs[i]);
    }
}

// How Pikmin 2's driver makes a piece's object: driver/pikmin2.c's PIECE_ values. Each is a way in which the game
// makes a piece's object.
enum class PieceKind : uint32_t
{
    kPlain = 0,       // a BgmSeq that only plays
    kMain = 1,        // an area's music: a JumpBgmSeq with the scene's directors
    kDirected = 2,    // a boss's music, or the music of 2-player battles
    kAuto = 3,        // a cave's music: an AutoBgm. Its conductor picks what the tracks play
    kBattle = 4,      // a DirectedBgm with the 2-player battle directors that the game gives battle_t
    kFree = 5,        // a DirectedBgm without directors
    kAutoFree = 6,    // an AutoBgm without directors or the arrangement of its melody
    kBigBoss = 7,     // the final boss's music, with the final boss's sound object in place
    kBigBossNear = 8, // the same, with the tension director on: a captain stands near the boss
};

// The JAInter::SoundInfo that the game gives a piece's object, in words: the flags, then the priority and the
// sequence's slot in the top two bytes, the pitch (1.0) and the volume in the top byte. The game's list of volumes
// (BgmList.txt) then sets the volume.
using SoundInfoWords = std::array<uint32_t, 4>;
constexpr SoundInfoWords kAreaInfo = {0x1f00, 127u << 24 | 1u << 16, 0x3f800000, 50u << 24}; // from initMainBgm
constexpr SoundInfoWords kBossInfo = {0, 127u << 24 | 2u << 16, 0x3f800000, 40u << 24};      // from initBossBgm
constexpr SoundInfoWords kExtraInfo = {0, 127u << 24 | 1u << 16, 0x3f800000, 50u << 24};     // from initAdditionalBgm
constexpr SoundInfoWords kKeyInfo = {0x1f00, 127u << 24 | 4u << 16, 0x3f800000, 35u << 24};  // the key's music

// The wave scene of a piece that the game never plays. The ripper finds the groups that hold its waves.
constexpr uint8_t kFindWaves = 0xff;

// One of Pikmin 2's pieces of music: its file in the sequence archive, how the game makes its object, the wave scene
// that the game loads for it (a group of wave system 0, besides group 0), an AutoBgm's conductor, the object's sound
// information, and the beat interval that the game gives the root track (0 keeps the default). A boss's music then has
// a start point: the part of the music that the boss starts. A variant of a piece plays it in another state of the
// game: with another conductor, or from another start point. Its label names its rips. A piece has no label, and its
// rips are named after its file.
struct Pikmin2Piece
{
    const char* sequence;
    PieceKind kind;
    uint8_t wave_scene;
    const char* conductor = "";
    SoundInfoWords info = kAreaInfo;
    uint16_t beat_interval = 0;
    uint8_t start_point = 0;
    const char* label = "";
};

// Pikmin 2's music: every file of the sequence archive but the effects sequence, se.bms, in the archive's order, then
// the variants. A piece's sound ID is 0x80000000 and its place here. The Wii's sequence archive has the GameCube's
// files, and the Wii's code makes the pieces' objects as the GameCube's does.
//
// The game makes most pieces' objects in PikSceneMgr::initMainBgm, initBossBgm and initAdditionalBgm. A piece's scene
// loads one of the wave system's 49 groups (wScene_*.aw) besides group 0. A cave floor's conductor comes from a list
// for each area (BgmList_*.txt) or from the challenge mode's list (ChallengeBgmList.txt). The conductor picks the
// phrases that the cave music's tracks play. Its name gives the floor's piece and wave group
// (ConductorList::getSeqAndWaveFromConductor, 0x803b3b3c). A piece plays with the first conductor of its family, such
// as caveconc_00_0.cnd and new_00_0.cnd. The game's code names caveconc_00_0.cnd itself, for a boss's floor. A boss
// starts its music when it appears, at a part of the music: the start point. The middle bosses start m_boss at their
// appearance (9). The final boss starts l_boss at its part for four weapons (1). A start point of 0 leaves the start to
// the scene. The music then starts at part 0, where JumpBgmSeq's constructor sets the part. The Waterwraith's chase
// starts m_boss there. At part 0, a track of l_boss jumps to itself without end.
//
// The variants. Each other conductor that the lists name gets a variant of its piece, with the wave group that its
// name gives. The bosses' actions jump to the other parts of their music (JumpBgmSeq::requestJumpBgmOnBeat). Each
// other part of m_boss and l_boss gets a variant that starts there and plays what follows. m_boss's parts 5 to 7 play
// in turn with part 3 for an attack. Nobody has found code in the game that asks for part 12 of l_boss. Its variant
// rips it, as the pieces that the game never plays rip. l_boss has no variant at part 0.
//
// The game makes l_boss only while the final boss's sound object exists (initBossBgm, 0x803b5740). Its directors then
// take tracks 14 and 15 (0x804f5d24) and mute them. Tracks 10 and 11 play in full. tentionD turns track 15 on while a
// captain stands within 300 units of the boss (0x804f2130), and only parts 4 and 11 hold notes on that track. Those two
// parts therefore get one more variant each, labelled with "-near". pikatkD turns track 14 on as Pikmin attack. That
// track holds one note, in parts 1 and 12, and gets no variant.
//
// Neither the game's code nor its lists of music start 14 of the files: 2pbattle, a_iron, a_zawinul01, caveboss,
// c_result, the six ff_ pieces, id_test, result and u_result. The game plays streams of the same names in place of
// c_result, result, u_result and the ff_ pieces. The calls that each sequence makes to the game choose its object.
// 2pbattle calls as battle_t does. a_iron and caveboss call as the caves' pieces do. They play with iron.cnd and
// caveboss.cnd. The game has no track map for them, and they play without directors. a_zawinul01 has no conductor of
// its own and plays with iron.cnd. id_test calls as a directed piece does, and plays without directors. The others
// only play. The ripper finds the wave groups of these pieces.
const Pikmin2Piece kPikmin2Pieces[] = {
    {"2pbattle.bms", PieceKind::kBattle, kFindWaves},
    {"a_iron.bms", PieceKind::kAutoFree, kFindWaves, "iron.cnd"},
    {"a_zawinul01.bms", PieceKind::kAutoFree, kFindWaves, "iron.cnd"},
    {"battle_t.bms", PieceKind::kDirected, 6},
    {"book.bms", PieceKind::kPlain, 36},
    {"caveboss.bms", PieceKind::kAutoFree, kFindWaves, "caveboss.cnd"},
    {"caveconc.bms", PieceKind::kAuto, 26, "caveconc_00_0.cnd"},
    {"caveglass.bms", PieceKind::kAuto, 23, "caveglass_00_0.cnd"},
    {"cavekeyget.bms", PieceKind::kAuto, 5, "cavekeyget.cnd", kKeyInfo},
    {"cavemetal.bms", PieceKind::kAuto, 10, "cavemetal_00_0.cnd"},
    {"caverelax.bms", PieceKind::kAuto, 28, "caverelax.cnd"},
    {"cavesoil.bms", PieceKind::kAuto, 22, "cavesoil_00_0.cnd"},
    {"cavetile.bms", PieceKind::kAuto, 24, "cavetile_00_0.cnd"},
    {"cavetsumiki.bms", PieceKind::kAuto, 25, "cavetsumiki_00_0.cnd"},
    {"comp_result.bms", PieceKind::kPlain, 19},
    {"c_menu.bms", PieceKind::kPlain, 20},
    {"c_result.bms", PieceKind::kPlain, kFindWaves},
    {"ff_annihi.bms", PieceKind::kPlain, kFindWaves},
    {"ff_doped.bms", PieceKind::kPlain, kFindWaves},
    {"ff_doping.bms", PieceKind::kPlain, kFindWaves},
    {"ff_keyget.bms", PieceKind::kPlain, kFindWaves},
    {"ff_playerdown.bms", PieceKind::kPlain, kFindWaves},
    {"ff_treasureget.bms", PieceKind::kPlain, kFindWaves},
    {"forest.bms", PieceKind::kMain, 2},
    {"f_result.bms", PieceKind::kPlain, 19},
    {"hiscore.bms", PieceKind::kPlain, 21, "", kExtraInfo},
    {"id_test.bms", PieceKind::kFree, kFindWaves},
    {"kuro_post.bms", PieceKind::kPlain, 48, "", kExtraInfo},
    {"kuro_pre.bms", PieceKind::kPlain, 48},
    {"last.bms", PieceKind::kMain, 4, "", kAreaInfo, 30},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 1},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 9},
    {"new_00.bms", PieceKind::kAuto, 37, "new_00_0.cnd"},
    {"new_01.bms", PieceKind::kAuto, 38, "new_01_0.cnd"},
    {"new_02.bms", PieceKind::kAuto, 39, "new_02_0.cnd"},
    {"new_03.bms", PieceKind::kAuto, 40, "new_03_0.cnd"},
    {"new_04.bms", PieceKind::kAuto, 41, "new_04_0.cnd"},
    {"new_05.bms", PieceKind::kAuto, 42, "new_05_0.cnd"},
    {"new_06.bms", PieceKind::kAuto, 43, "new_06_0.cnd"},
    {"new_07.bms", PieceKind::kAuto, 44, "new_07_0.cnd"},
    {"new_08.bms", PieceKind::kAuto, 45, "new_08_0.cnd"},
    {"n_tutorial.bms", PieceKind::kMain, 15},
    {"n_tutorial_1stday.bms", PieceKind::kPlain, 15, "", kExtraInfo},
    {"result.bms", PieceKind::kPlain, kFindWaves},
    {"u_result.bms", PieceKind::kPlain, kFindWaves},
    {"worldmap.bms", PieceKind::kPlain, 16},
    {"worldmap_intro.bms", PieceKind::kPlain, 16},
    {"yakushima.bms", PieceKind::kMain, 3, "", kAreaInfo, 30},
    {"caveconc.bms", PieceKind::kAuto, 31, "caveconc_05_0.cnd", kAreaInfo, 0, 0, "caveconc_05_0"},
    {"caveconc.bms", PieceKind::kAuto, 32, "caveconc_10_0.cnd", kAreaInfo, 0, 0, "caveconc_10_0"},
    {"cavemetal.bms", PieceKind::kAuto, 29, "cavemetal_05_0.cnd", kAreaInfo, 0, 0, "cavemetal_05_0"},
    {"cavemetal.bms", PieceKind::kAuto, 29, "cavemetal_05_4.cnd", kAreaInfo, 0, 0, "cavemetal_05_4"},
    {"cavemetal.bms", PieceKind::kAuto, 30, "cavemetal_10_0.cnd", kAreaInfo, 0, 0, "cavemetal_10_0"},
    {"cavemetal.bms", PieceKind::kAuto, 30, "cavemetal_10_4.cnd", kAreaInfo, 0, 0, "cavemetal_10_4"},
    {"cavesoil.bms", PieceKind::kAuto, 33, "cavesoil_05_0.cnd", kAreaInfo, 0, 0, "cavesoil_05_0"},
    {"cavesoil.bms", PieceKind::kAuto, 34, "cavesoil_10_0.cnd", kAreaInfo, 0, 0, "cavesoil_10_0"},
    {"cavesoil.bms", PieceKind::kAuto, 34, "cavesoil_10_3.cnd", kAreaInfo, 0, 0, "cavesoil_10_3"},
    {"new_00.bms", PieceKind::kAuto, 37, "new_00_1.cnd", kAreaInfo, 0, 0, "new_00_1"},
    {"new_08.bms", PieceKind::kAuto, 45, "new_08_1.cnd", kAreaInfo, 0, 0, "new_08_1"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 0, "m_boss-0"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 1, "m_boss-1"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 2, "m_boss-2"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 3, "m_boss-3"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 4, "m_boss-4"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 5, "m_boss-5"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 6, "m_boss-6"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 7, "m_boss-7"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 8, "m_boss-8"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 10, "m_boss-10"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 11, "m_boss-11"},
    {"m_boss.bms", PieceKind::kDirected, 0, "", kBossInfo, 60, 12, "m_boss-12"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 2, "l_boss-2"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 3, "l_boss-3"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 4, "l_boss-4"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 5, "l_boss-5"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 6, "l_boss-6"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 7, "l_boss-7"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 8, "l_boss-8"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 9, "l_boss-9"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 10, "l_boss-10"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 11, "l_boss-11"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 12, "l_boss-12"},
    {"l_boss.bms", PieceKind::kBigBoss, 35, "", kBossInfo, 60, 13, "l_boss-13"},
    {"l_boss.bms", PieceKind::kBigBossNear, 35, "", kBossInfo, 60, 4, "l_boss-4-near"},
    {"l_boss.bms", PieceKind::kBigBossNear, 35, "", kBossInfo, 60, 11, "l_boss-11-near"},
};

// The most pieces that driver/pikmin2.c's table holds (MAX_PIECES).
constexpr std::size_t kMaxPikmin2Pieces = 96;
static_assert(std::size(kPikmin2Pieces) <= kMaxPikmin2Pieces);

// The bytes that driver/pikmin2.c's Piece gives each file name.
constexpr std::size_t kPieceNameSize = 24;

} // namespace

std::vector<uint32_t> Pikmin2Parameters(std::vector<uint32_t> words)
{
    words.push_back(static_cast<uint32_t>(std::size(kPikmin2Pieces)));
    for (const Pikmin2Piece& piece : kPikmin2Pieces)
    {
        words.push_back(static_cast<uint32_t>(piece.kind) | uint32_t{piece.start_point} << 8 |
                        uint32_t{piece.beat_interval} << 16);
        words.insert(words.end(), piece.info.begin(), piece.info.end());
        for (const std::string_view name : {std::string_view(piece.sequence), std::string_view(piece.conductor)})
        {
            std::array<uint8_t, kPieceNameSize> bytes = {};
            std::copy_n(name.begin(), std::min(name.size(), bytes.size() - 1), bytes.begin());
            for (std::size_t i = 0; i < bytes.size(); i += 4)
            {
                words.push_back(xsflib::LoadBe32(bytes.data() + i));
            }
        }
    }

    return words;
}

namespace
{

// The piece of Pikmin 2's that sound `sound_id` plays, or null for another sound.
const Pikmin2Piece* PieceOf(const JAudioProfile& profile, uint32_t sound_id)
{
    const uint32_t index = sound_id & 0xffff;
    if (profile.driver != JAudioDriver::kPikmin2 ||
        (sound_id & ntdlib::jaudio::kAafTypeMask) != ntdlib::jaudio::kAafSequence || index >= std::size(kPikmin2Pieces))
    {
        return nullptr;
    }

    return &kPikmin2Pieces[index];
}

// A JAudio1 sound name table (.asn): a 16-byte header, then 32-byte records. A record holds a name in up to 28 bytes,
// then a word. The first 18 records name the sound table's categories: 16 of effects, the sequences and the streams.
// Each record after them names a sound, and its word is the sound's ID.
constexpr std::size_t kNamesHeader = 0x10;
constexpr std::size_t kNameRecordSize = 0x20;
constexpr std::size_t kNameSize = 0x1c;
constexpr std::size_t kNamedCategories = 18;

// The key under which an effect's name is found: its ID without bits 10 and 11. Those bits set how long the effect
// plays.
uint32_t NameKey(uint32_t id)
{
    return id & ~(ntdlib::jaudio::kAafToEnd | ntdlib::jaudio::kAafToEnd >> 1);
}

// The effects that a name table names, by NameKey. Throws xsflib::FormatError if the table is damaged.
std::map<uint32_t, std::string> ReadEffectNames(std::span<const uint8_t> table)
{
    if (table.size() < kNamesHeader || (table.size() - kNamesHeader) / kNameRecordSize < kNamedCategories)
    {
        throw xsflib::FormatError("the sound name table is damaged");
    }

    std::map<uint32_t, std::string> names;
    const xsflib::Reader r(table, true);
    for (std::size_t at = kNamesHeader + kNamedCategories * kNameRecordSize; at + kNameRecordSize <= table.size();
         at += kNameRecordSize)
    {
        const uint32_t id = r.U32(at + kNameSize);
        if ((id & ntdlib::jaudio::kAafTypeMask) == ntdlib::jaudio::kAafEffect)
        {
            names.try_emplace(NameKey(id), r.CString(at, kNameSize));
        }
    }

    return names;
}

// The text at `address` in `dol`, up to its terminating zero. Throws xsflib::FormatError if the text has no end.
std::string DolText(const ntdlib::Dol& dol, uint32_t address)
{
    const std::span<const uint8_t> bytes = dol.At(address);
    const auto end = std::find(bytes.begin(), bytes.end(), uint8_t{0});
    if (end == bytes.end())
    {
        throw xsflib::FormatError("main.dol's name of an effect at " + xsflib::Hex(address, 8) + " has no end");
    }

    return std::string(bytes.begin(), end);
}

// A Jac game's sound effects, from the catalogue in `dol` (see ReadJacSounds).
std::vector<JAudioSound> ReadJacEffects(const ntdlib::Dol& dol, const JAudioProfile& profile)
{
    if (!profile.effect_table)
    {
        return {};
    }

    constexpr std::size_t kRow = 20, kEventTypes = 8, kAction = 6;
    constexpr int32_t kSystem = -2, kPlayer = -1;
    const xsflib::Reader rows(dol.At(profile.effect_table), true);
    const xsflib::Reader offsets(dol.At(profile.event_offsets), true);
    if (rows.Size() < profile.effect_count * kRow || offsets.Size() < kEventTypes * 4)
    {
        throw xsflib::FormatError("main.dol's catalogue of effects runs past its section");
    }

    // An event type's first entry in the table of actions, and the effect that an entry plays.
    const xsflib::Reader actions(offsets.Sub(kEventTypes * 4, offsets.Size() - kEventTypes * 4), true);
    const auto first_entry = [&offsets](uint32_t type)
    {
        return offsets.U32(4 * std::size_t{type});
    };
    const auto effect_of = [&actions](uint32_t entry)
    {
        if (actions.Size() / kAction <= entry)
        {
            throw xsflib::FormatError("main.dol's table of actions runs past its section");
        }

        return actions.U16(std::size_t{entry} * kAction + 4);
    };

    std::vector<JAudioSound> effects;
    std::set<uint32_t> ids;
    const auto add = [&](uint32_t id, std::string label)
    {
        const std::vector<uint32_t>& left_out = profile.left_out_effects;
        if (std::find(left_out.begin(), left_out.end(), id) == left_out.end() && ids.insert(id).second)
        {
            effects.push_back({.id = id, .label = std::move(label), .section = "SE", .waves = profile.effect_groups});
        }
    };

    // Actions of events of different types can play the same effect. The first row that plays an effect names it.
    std::set<uint32_t> event_effects;
    for (uint32_t i = 0; i < profile.effect_count; i++)
    {
        const std::size_t row = std::size_t{i} * kRow;
        const uint32_t number = rows.U32(row + 4);
        std::string name = DolText(dol, rows.U32(row + 8));
        const auto kind = static_cast<int32_t>(rows.U32(row + 16));
        if (kind == kSystem || kind == kPlayer)
        {
            add(JacEffectId(kind == kSystem ? JacEffect::kSystem : JacEffect::kPlayer, number), std::move(name));
            continue;
        }

        if (kind < 1 || kind >= static_cast<int32_t>(kEventTypes))
        {
            throw xsflib::FormatError("main.dol's catalogue of effects has a row of unknown kind");
        }

        // An event of the sounding type starts another effect as the game creates it. The rip plays the effects of
        // that type on an event of the quiet type, by the same entry in the table of actions.
        auto type = static_cast<uint32_t>(kind);
        const uint32_t entry = first_entry(type) + number;
        if (!event_effects.insert(effect_of(entry)).second)
        {
            continue;
        }

        if (type == profile.sounding_event)
        {
            type = profile.quiet_event;
        }

        add(JacEffectId(JacEffect::kEvent, type << 12 | (entry - first_entry(type))), std::move(name));
    }

    // The effects that the game's code starts itself.
    for (const uint32_t id : profile.other_effects)
    {
        const auto kind = static_cast<JacEffect>(id >> 24 & 0x7f);
        std::string label = "formation";
        if (kind != JacEffect::kFormation)
        {
            const char* const word = kind == JacEffect::kSystem   ? "system"
                                     : kind == JacEffect::kPlayer ? "player"
                                                                  : "event";
            char text[32];
            std::snprintf(text, sizeof(text), "%s-%x", word, static_cast<unsigned>(id >> 8 & 0xffff));
            label = text;
        }

        add(id, std::move(label));
    }

    return effects;
}

} // namespace

std::optional<JAudioFiles> FindJAudio2Files(const ntdlib::disc::GameFiles& game)
{
    if (!game.read_file)
    {
        return std::nullopt;
    }

    // A start-up archive is a .baa file, or a .szs file that holds one compressed with Yaz0.
    std::optional<JAudioFiles> found;
    std::vector<uint8_t> data;
    for (const ntdlib::disc::GameFile& file : game.files)
    {
        const std::string path = xsflib::AsciiLower(file.path);
        if ((path.ends_with(".baa") || path.ends_with(".szs")) && ReadSoundFile(game, file.path, data) &&
            StartsWith(data, "AA_<"))
        {
            found = JAudioFiles{file.path, {}};
            break;
        }
    }
    if (!found)
    {
        return std::nullopt;
    }

    // The RARC archives in the start-up archive's folder and below it, compressed with Yaz0 or not.
    const std::string folder = FolderOf(found->baa);
    for (const ntdlib::disc::GameFile& file : game.files)
    {
        if (file.path.starts_with(folder) && xsflib::AsciiLower(file.path).ends_with(".arc") &&
            ReadSoundFile(game, file.path, data) && StartsWith(data, "RARC"))
        {
            found->archives.push_back(file.path);
        }
    }

    return found;
}

bool ReadSoundFile(const ntdlib::disc::GameFiles& game, const std::string& path, std::vector<uint8_t>& data)
{
    if (!game.read_file || !game.read_file(path, data))
    {
        return false;
    }

    if (ntdlib::jaudio::IsYaz0(data))
    {
        try
        {
            data = ntdlib::jaudio::DecodeYaz0(data);
        }
        catch (const xsflib::FormatError&)
        {
            return false;
        }
    }

    return true;
}

std::vector<std::string> LibraryFiles(const JAudioFiles& files, const JAudioProfile& profile)
{
    const std::vector<std::string>& others = profile.files.empty() ? files.archives : profile.files;
    std::vector<std::string> paths = {files.baa};
    paths.insert(paths.end(), others.begin(), others.end());

    return paths;
}

std::optional<std::vector<std::string>> FindAwFiles(const ntdlib::disc::GameFiles& game, const JAudioFiles& files,
                                                    const std::vector<ntdlib::jaudio::WaveSystem>& systems,
                                                    std::span<const uint32_t> groups, std::string* missing)
{
    const std::string folder = FolderOf(files.baa);
    std::vector<std::string> paths;
    for (const std::string& name : ntdlib::jaudio::GroupArchives(systems, groups))
    {
        const auto matches = [&folder, &name](const ntdlib::disc::GameFile& file)
        {
            return file.path.starts_with(folder) && file.path.ends_with("/" + name);
        };
        const auto file = std::find_if(game.files.begin(), game.files.end(), matches);
        if (file == game.files.end())
        {
            if (missing)
            {
                *missing = name;
            }

            return std::nullopt;
        }

        paths.push_back(file->path);
    }

    return paths;
}

std::optional<std::vector<uint8_t>> BuildJAudioLibrary(const ntdlib::disc::GameFiles& game,
                                                       const JAudioProfile& profile,
                                                       const std::vector<std::string>& paths, const Tags& tags,
                                                       std::string* error)
{
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(game.dol);
    if (!dol)
    {
        return LibraryError(error, "main.dol isn't a DOL");
    }

    const std::optional<JAudioDriverImage> image = ImageOf(profile.driver);
    if (!image || image->params != DriverBlock::kAddress)
    {
        return LibraryError(error, "WSF has no driver for this profile");
    }

    // The driver, with its parameters. driver/jaudio.c (kZ2) takes the profile and the Wii's fields. Another driver
    // takes the profile's words after the wave list. A .miniwsf fills in the sound and its wave groups.
    GameLibrary library{.driver = {image->blob.begin(), image->blob.end()},
                        .base = image->base,
                        .entry = image->entry,
                        .main = profile.main,
                        .ios_version = profile.ios_version,
                        .paths = paths,
                        .too_large_advice = "Rip fewer sounds at a time, with --only."};
    const uint32_t block = image->params - image->base;
    if (profile.driver == JAudioDriver::kZ2)
    {
        PutZ2DriverParams(library.driver.data() + block, profile);
    }
    else
    {
        if (block + kJAudioParamsOffset + profile.params.size() * 4 > library.driver.size())
        {
            return LibraryError(error, "the profile has more parameters than its driver's block holds");
        }

        for (std::size_t i = 0; i < profile.params.size(); i++)
        {
            xsflib::StoreBe32(library.driver.data() + block + kJAudioParamsOffset + i * 4, profile.params[i]);
        }
    }

    return BuildGameLibrary(game, *dol, library, tags, error);
}

std::vector<uint8_t> BuildJAudioMini(uint32_t sound_id, std::span<const uint32_t> waves, const std::string& lib_name,
                                     Tags tags, const ntdlib::flipper::z2::PieceState* state, int32_t luigi_word)
{
    uint8_t sound[4];
    xsflib::StoreBe32(sound, sound_id);
    ProgramBuilder program;
    program.AddMemory(DriverBlock::kAddress + kJAudioZ2Offset + ntdlib::flipper::z2::kSoundOffset, sound,
                      sizeof(sound));
    const std::vector<uint8_t> list = ntdlib::flipper::WaveList(waves);
    program.AddMemory(DriverBlock::kAddress + kJAudioZ2Offset + ntdlib::flipper::z2::kWaveCountOffset, list.data(),
                      list.size());
    if (const std::vector<uint8_t> words = state ? state->Bytes() : std::vector<uint8_t>(); !words.empty())
    {
        program.AddMemory(DriverBlock::kAddress + kJAudioZ2Offset + ntdlib::flipper::z2::kMutedChildrenOffset,
                          words.data(), words.size());
    }

    if (luigi_word >= 0)
    {
        uint8_t on[4];
        xsflib::StoreBe32(on, 1);
        program.AddMemory(DriverBlock::kAddress + kJAudioParamsOffset + static_cast<uint32_t>(luigi_word) * 4, on,
                          sizeof(on));
    }

    tags["_lib"] = lib_name;

    return xsflib::psf::WritePsf(kPsfVersionWsf, {}, program.Data(), tags);
}

std::vector<JAudioSound> ReadJAudio2Sounds(std::span<const uint8_t> baa)
{
    std::vector<JAudioSound> sounds;
    for (const ntdlib::jaudio::SoundEntry& entry : ntdlib::jaudio::ReadSounds(baa, {.section_names = true}))
    {
        char id[16];
        std::snprintf(id, sizeof(id), "0x%08x", entry.Id());
        std::string label = entry.name.empty() ? id : entry.name;
        const bool music = xsflib::AsciiLower(label).find("bgm") != std::string::npos || entry.section_name == "BGM";
        sounds.push_back({.id = entry.Id(),
                          .label = std::move(label),
                          .section = entry.section_name,
                          .music = music,
                          .stream = ntdlib::jaudio::LayoutOf(entry.type).stream});
    }

    return sounds;
}

std::optional<JAudioFiles> FindJAudio1Files(const ntdlib::disc::GameFiles& game, const JAudioProfile& profile)
{
    const auto on_disc = [&game](const std::string& path)
    {
        return std::any_of(game.files.begin(), game.files.end(),
                           [&path](const ntdlib::disc::GameFile& file) { return file.path == path; });
    };

    if (!on_disc(profile.sound_archive) || !on_disc(profile.sequence_archive))
    {
        return std::nullopt;
    }

    return JAudioFiles{profile.sound_archive, {profile.sequence_archive}};
}

std::optional<JAudioFiles> FindJAudioFiles(const ntdlib::disc::GameFiles& game, const JAudioProfile& profile,
                                           std::string* error)
{
    if (profile.library == JAudioLibrary::kJAudio2)
    {
        std::optional<JAudioFiles> files = FindJAudio2Files(game);
        if (!files && error)
        {
            *error = "the game has no JAudio2 start-up archive (.baa)";
        }

        return files;
    }

    std::optional<JAudioFiles> files = FindJAudio1Files(game, profile);
    if (!files && error)
    {
        *error = "the disc lacks " + profile.sound_archive + " or " + profile.sequence_archive;
    }

    return files;
}

std::vector<ntdlib::jaudio::WaveSystem> ReadJacWaveSystems(std::span<const uint8_t> bank_file)
{
    constexpr std::size_t kEntry = 8;
    const xsflib::Reader r(bank_file, true);
    const uint32_t list = r.U32(0);
    const uint32_t count = r.U32(4);
    if (count > bank_file.size() / kEntry)
    {
        throw xsflib::FormatError("the bank file lists more wave systems than it can hold");
    }

    std::vector<ntdlib::jaudio::WaveSystem> systems;
    for (uint32_t number = 0; number < count; number++)
    {
        const uint32_t offset = r.U32(list + number * kEntry);
        const uint32_t size = r.U32(list + number * kEntry + 4);
        if (size != 0)
        {
            systems.push_back(ntdlib::jaudio::ReadWaveSystem(r.Sub(offset, size), number));
        }
    }

    return systems;
}

std::vector<ntdlib::jaudio::WaveSystem> ReadWaveSystems(JAudioLibrary library, std::span<const uint8_t> start_up)
{
    switch (library)
    {
    case JAudioLibrary::kJAudio2:
        return ntdlib::jaudio::ReadWaveSystems(start_up);
    case JAudioLibrary::kJac:
        return ReadJacWaveSystems(start_up);
    case JAudioLibrary::kAaf:
        return ntdlib::jaudio::ReadAafWaveSystems(start_up);
    }

    return {};
}

std::vector<JAudioSound> ReadJacSounds(std::span<const uint8_t> dol, const JAudioProfile& profile)
{
    constexpr std::size_t kHeader = 0x20, kEntry = 0x20, kEntryNameSize = 14, kSizeOffset = 0x1c;

    // The index runs from the profile's address to the end of the section that holds it.
    const ntdlib::Dol parsed = ntdlib::Dol::ParseMain(dol);
    const std::span<const uint8_t> index = parsed.At(profile.sequence_index);
    const xsflib::Reader r(index, true);
    if (index.size() < kHeader || !r.Magic(0, "BARC"))
    {
        throw xsflib::FormatError("main.dol has no sequence index at " + xsflib::Hex(profile.sequence_index, 8));
    }

    // A sound's ID holds the piece's number in its low byte.
    constexpr uint32_t kMaxPieces = 0x100;
    const uint32_t count = r.U32(0xc);
    if (count > kMaxPieces)
    {
        throw xsflib::FormatError("main.dol's sequence index has " + std::to_string(count) + " files");
    }

    std::vector<JAudioSound> sounds;
    for (uint32_t number = kJacFirstPiece; number < count; number++)
    {
        const std::size_t entry = kHeader + std::size_t{number} * kEntry;
        if (r.U32(entry + kSizeOffset) == 0)
        {
            continue;
        }

        std::string label = r.CString(entry, kEntryNameSize);
        label = label.substr(0, label.rfind('.'));
        if (number != profile.parted_piece)
        {
            sounds.push_back({.id = ntdlib::jaudio::kAafSequence | number,
                              .label = std::move(label),
                              .music = true,
                              .waves = {number << 16}});
            continue;
        }

        for (const JacPart& part : profile.parts)
        {
            sounds.push_back({.id = ntdlib::jaudio::kAafSequence | uint32_t{part.number} << 8 | number,
                              .label = label + "-" + std::to_string(part.number),
                              .music = true,
                              .waves = {uint32_t{part.system} << 16 | part.group}});
        }
    }

    std::vector<JAudioSound> effects = ReadJacEffects(parsed, profile);
    sounds.insert(sounds.end(), std::make_move_iterator(effects.begin()), std::make_move_iterator(effects.end()));

    return sounds;
}

std::vector<JAudioSound> ReadPikmin2Sounds(std::span<const uint8_t> aaf, std::span<const uint8_t> sequence_archive,
                                           std::span<const uint8_t> names, const JAudioProfile& profile)
{
    std::vector<uint8_t> decoded;
    sequence_archive = ntdlib::jaudio::DecodeIfYaz0(sequence_archive, decoded);

    // The pieces, each with group 0 and its scene's group of wave system 0.
    const std::vector<ntdlib::jaudio::ArchiveFile> files = ntdlib::jaudio::ReadRarc(sequence_archive);
    std::vector<JAudioSound> sounds;
    for (const Pikmin2Piece& piece : kPikmin2Pieces)
    {
        const std::string_view name = piece.sequence;
        const auto is_piece = [name](const ntdlib::jaudio::ArchiveFile& file)
        {
            return file.name == name;
        };
        if (std::none_of(files.begin(), files.end(), is_piece))
        {
            throw xsflib::FormatError("the sequence archive lacks " + std::string(name));
        }

        const std::string_view label = *piece.label ? std::string_view(piece.label) : name.substr(0, name.rfind('.'));
        JAudioSound sound = {.id = ntdlib::jaudio::kAafSequence | static_cast<uint32_t>(sounds.size()),
                             .label = std::string(label),
                             .section = "BGM",
                             .music = true};
        if (piece.wave_scene != kFindWaves)
        {
            sound.waves = {0};
            if (piece.wave_scene != 0)
            {
                sound.waves.push_back(piece.wave_scene);
            }
        }

        sounds.push_back(std::move(sound));
    }

    // The effects, by their names in the name table, or else by their categories and indices.
    const std::map<uint32_t, std::string> named =
        names.empty() ? std::map<uint32_t, std::string>{} : ReadEffectNames(names);
    for (const ntdlib::jaudio::SoundEntry& entry : ntdlib::jaudio::ReadAafSounds(aaf))
    {
        if (entry.source == ntdlib::jaudio::SoundEntry::Source::kArchive)
        {
            continue;
        }

        const uint32_t id = entry.Id();
        const auto name = named.find(NameKey(id));
        char label[32];
        std::snprintf(label, sizeof(label), "se%u-%04u", static_cast<unsigned>(ntdlib::jaudio::AafCategory(id)),
                      static_cast<unsigned>(id & ntdlib::jaudio::kAafIndexMask));
        const auto start = std::find_if(profile.random_starts.begin(), profile.random_starts.end(),
                                        [id](const RandomStart& s) { return s.id == id; });
        const uint32_t draws = start != profile.random_starts.end() ? start->draws : 0;
        sounds.push_back({.id = id | draws << kRandomDrawsShift,
                          .label = name != named.end() ? name->second : label,
                          .section = "SE",
                          .waves = profile.static_waves});
    }

    return sounds;
}

namespace
{

// Gives the pieces whose tracks the game mutes as it starts them the children to mute (JAudioProfile::muted_starts). A
// start with a suffix becomes a sound after its piece. The sound's label is the piece's with the suffix, such as
// "Z2BGM_FIELD_LINK_DAY_riding", and it shares the piece's number.
void AddMutedStarts(const JAudioProfile& profile, std::vector<JAudioSound>& sounds)
{
    if (profile.muted_starts.empty())
    {
        return;
    }

    std::vector<JAudioSound> listed;
    for (JAudioSound& sound : sounds)
    {
        const std::size_t piece = listed.size();
        listed.push_back(std::move(sound));
        for (const ntdlib::jaudio::MutedStart& start : profile.muted_starts)
        {
            if (start.sound != listed[piece].id || listed[piece].stream)
            {
                continue;
            }

            if (start.suffix.empty())
            {
                listed[piece].piece_state.muted_children = start.muted_children;
                continue;
            }

            JAudioSound variant = listed[piece];
            variant.label += start.suffix;
            variant.piece_state.muted_children = start.muted_children;
            variant.shares_number = true;
            listed.push_back(std::move(variant));
        }
    }

    sounds = std::move(listed);
}

// Gives the pieces their states (JAudioProfile::piece_states). A state without a suffix is the piece's. A state with a
// suffix becomes a sound after the piece and its other starts. The sound's label is the piece's with the suffix, such
// as "Z2BGM_TOAL_VILLEGE_status1", and it shares the piece's number.
void AddPieceStates(const JAudioProfile& profile, std::vector<JAudioSound>& sounds)
{
    if (profile.piece_states.empty())
    {
        return;
    }

    std::vector<JAudioSound> listed;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const std::size_t piece = listed.size();
        listed.push_back(std::move(sounds[i]));
        if (listed[piece].stream || listed[piece].shares_number)
        {
            continue;
        }

        std::vector<JAudioSound> states;
        for (const ntdlib::jaudio::MusicState& state : profile.piece_states)
        {
            if (state.sound != listed[piece].id)
            {
                continue;
            }

            if (state.suffix.empty())
            {
                listed[piece].piece_state = ntdlib::flipper::z2::PieceStateOf(state);
                continue;
            }

            JAudioSound variant = listed[piece];
            variant.label += state.suffix;
            variant.piece_state = ntdlib::flipper::z2::PieceStateOf(state);
            variant.shares_number = true;
            states.push_back(std::move(variant));
        }

        // The piece's other starts stay right after it.
        while (i + 1 < sounds.size() && sounds[i + 1].shares_number)
        {
            listed.push_back(std::move(sounds[++i]));
        }

        listed.insert(listed.end(), std::make_move_iterator(states.begin()), std::make_move_iterator(states.end()));
    }

    sounds = std::move(listed);
}

} // namespace

std::optional<std::vector<JAudioSound>> ReadJAudioSounds(const ntdlib::disc::GameFiles& game, const JAudioFiles& files,
                                                         const JAudioProfile* profile,
                                                         std::span<const uint8_t> start_up, std::string* error)
{
    const JAudioLibrary library = SoundLibraryOf(profile);
    if (library == JAudioLibrary::kJAudio2)
    {
        std::vector<JAudioSound> sounds = ReadJAudio2Sounds(start_up);
        if (profile)
        {
            AddMutedStarts(*profile, sounds);
            AddPieceStates(*profile, sounds);
        }

        return sounds;
    }
    if (library == JAudioLibrary::kJac)
    {
        return ReadJacSounds(game.dol, *profile);
    }

    // Pikmin 2's sequence archive, and its name table.
    std::vector<uint8_t> sequences;
    std::vector<uint8_t> name_table;

    const auto read = [&game, error](const std::string& path, std::vector<uint8_t>& data)
    {
        if (game.read_file && game.read_file(path, data))
        {
            return true;
        }

        if (error)
        {
            *error = "can't read " + path;
        }

        return false;
    };

    if (!read(files.archives.front(), sequences) ||
        (!profile->sound_names.empty() && !read(profile->sound_names, name_table)))
    {
        return std::nullopt;
    }

    return ReadPikmin2Sounds(start_up, sequences, name_table, *profile);
}

ntdlib::jaudio::GameWatch WatchOf(const JAudioProfile& profile, uint32_t sound_id)
{
    // An AutoBgm piece never repeats. Once the piece's root track idles in its polling routine, the passes of the other
    // tracks would still look like a loop. The root track's polls therefore stay moves. They hold back every loop, and
    // the piece plays to the analysis's limit.
    ntdlib::jaudio::GameWatch watch = profile.watch;
    const Pikmin2Piece* piece = PieceOf(profile, sound_id);
    if (piece && (piece->kind == PieceKind::kAuto || piece->kind == PieceKind::kAutoFree))
    {
        watch.idle_in_calls = false;
    }

    return watch;
}

} // namespace wsf::rip
