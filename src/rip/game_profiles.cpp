// SPDX-License-Identifier: MIT

// NW4R SND's driver profiles of the games that WSF rips in game mode, with the signatures that find their functions in
// another release (see profiles.h).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/rvl/snd/game_setups.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "rip/dol_search.h"
#include "rip/profiles.h"

namespace wsf::rip
{
namespace
{

// The main.dol files of the six games with driver profiles (ntdlib/rvl/snd/game_setups.h).
using nw4r::kCityFolkSha1;
using nw4r::kMarioKartSha1;
using nw4r::kSmoothMovesSha1;
using nw4r::kWiiMusicSha1;
using nw4r::kWiiPlaySha1;
using nw4r::kWiiSportsSha1;

// The profile's functions with signatures, in the order of the signatures.
constexpr std::array<uint32_t GameProfile::*, kProfileFunctions> kProfileFields = {
    &GameProfile::ai_init,
    &GameProfile::ax_init,
    &GameProfile::alloc,
    &GameProfile::sleep_ticks,
    &GameProfile::init_sound_system,
    &GameProfile::archive_ctor,
    &GameProfile::archive_open,
    &GameProfile::archive_load_info,
    &GameProfile::player_ctor,
    &GameProfile::player_mem_size,
    &GameProfile::player_strm_size,
    &GameProfile::player_setup,
    &GameProfile::heap_ctor,
    &GameProfile::heap_create,
    &GameProfile::load_group,
    &GameProfile::start_sound,
    &GameProfile::update,
};

// The place of SoundArchivePlayer::Setup among the profile's functions.
constexpr std::size_t kSetupFunction = 11;
static_assert(kProfileFields[kSetupFunction] == &GameProfile::player_setup);

// Words of PowerPC code: blr, and the opcode and the AA and LK bits of b and bl, which kBranchBits selects.
constexpr uint32_t kBlr = 0x4e800020;
constexpr uint32_t kBranchBits = 0xfc000003;
constexpr uint32_t kB = 0x48000000;
constexpr uint32_t kBl = 0x48000001;

// The word at the load address `address` in `dol`, if a section holds all four bytes.
std::optional<uint32_t> WordAt(const ntdlib::Dol& dol, uint32_t address)
{
    const std::span<const uint8_t> bytes = dol.At(address);
    if (bytes.size() < 4)
    {
        return std::nullopt;
    }

    return xsflib::LoadBe32(bytes.data());
}

// The function that the word `word` at `address` calls, if it's a relative bl.
std::optional<uint32_t> CallTarget(std::optional<uint32_t> word, uint32_t address)
{
    if (!word || (*word & kBranchBits) != kBl)
    {
        return std::nullopt;
    }

    const int32_t offset = (static_cast<int32_t>(*word << 6) >> 6) & ~3;

    return address + static_cast<uint32_t>(offset);
}

// The base of the small data area, which CodeWarrior's start-up code puts in r13. The entry point calls
// __init_registers, which loads r13 with lis, then ori or addi. Returns nothing if none of the functions that the entry
// point's first 400 words call does that in its first 40 words.
std::optional<uint32_t> SmallDataBase(const ntdlib::Dol& dol)
{
    constexpr uint32_t kR13 = 13;
    for (uint32_t i = 0; i < 400; i++)
    {
        const uint32_t at = dol.Entry() + 4 * i;
        const std::optional<uint32_t> callee = CallTarget(WordAt(dol, at), at);
        for (uint32_t j = 0; callee && j < 40; j++)
        {
            const std::optional<uint32_t> high = WordAt(dol, *callee + 4 * j);
            const std::optional<uint32_t> low = WordAt(dol, *callee + 4 * j + 4);
            if (!high || !low)
            {
                break;
            }

            const uint32_t low_opcode = *low >> 26;
            const bool loads_high = *high >> 26 == 15 && (*high >> 21 & 31) == kR13 && (*high >> 16 & 31) == 0;
            const bool adds_low =
                (low_opcode == 14 || low_opcode == 24) && (*low >> 21 & 31) == kR13 && (*low >> 16 & 31) == kR13;
            if (loads_high && adds_low)
            {
                const uint32_t base = *high << 16;
                return low_opcode == 24 ? base | (*low & 0xffff)
                                        : base + static_cast<uint32_t>(static_cast<int16_t>(*low & 0xffff));
            }
        }
    }

    return std::nullopt;
}

// The game's main. __start, at the entry point, ends with bl main, then b exit. The function after it starts within
// 400 words, and the first blr lies in that function. Returns the last bl before the first blr that a b follows, or
// nothing if no blr comes in the first 400 words.
std::optional<uint32_t> FindMain(const ntdlib::Dol& dol)
{
    std::optional<uint32_t> main;
    for (uint32_t i = 0; i < 400; i++)
    {
        const uint32_t at = dol.Entry() + 4 * i;
        const std::optional<uint32_t> word = WordAt(dol, at);
        const std::optional<uint32_t> next = WordAt(dol, at + 4);
        if (!word || !next)
        {
            return std::nullopt;
        }

        if (*word == kBlr)
        {
            return main;
        }

        const std::optional<uint32_t> callee = CallTarget(word, at);
        if (callee && (*next & kBranchBits) == kB)
        {
            main = callee;
        }
    }

    return std::nullopt;
}

// The address of the sound system's flag that SoundArchivePlayer::Setup at `setup` checks in a 2008 build of NW4R SND
// (GameProfile::init_flag). Setup's first `size` bytes call IsInitializedSoundSystem, which reads the flag relative to
// r13: lbz r3, d(r13), then blr. Returns nothing if no call there reaches such a function.
std::optional<uint32_t> FindInitFlag(const ntdlib::Dol& dol, uint32_t setup, uint32_t size)
{
    constexpr uint32_t kLbzR3FromR13 = 0x886d0000;
    const std::optional<uint32_t> base = SmallDataBase(dol);
    for (uint32_t at = setup; base && at < setup + size; at += 4)
    {
        const std::optional<uint32_t> callee = CallTarget(WordAt(dol, at), at);
        const std::optional<uint32_t> load = callee ? WordAt(dol, *callee) : std::nullopt;
        if (load && (*load & 0xffff0000) == kLbzR3FromR13 && WordAt(dol, *callee + 4) == kBlr)
        {
            return *base + static_cast<uint32_t>(static_cast<int16_t>(*load & 0xffff));
        }
    }

    return std::nullopt;
}

} // namespace

const std::vector<GameProfile>& KnownProfiles()
{
    // The function addresses in each game's main.dol. Wii Play links the same builds of NW4R SND and the SDK as Wii
    // Sports, at other addresses. The 2008 builds of NW4R SND in Mario Kart Wii, Wii Music and Animal Crossing have the
    // same functions, with two differences. SoundStartable::detail_StartSound takes the start info as its third
    // argument. The driver passes null for it. SoundArchivePlayer::Setup first checks the sound system's flag
    // (init_flag).
    static const std::vector<GameProfile> kProfiles = {
        {
            .name = "Wii Sports (USA, Rev 1)",
            .game_id = "RSPE01",
            .dol_sha1 = kWiiSportsSha1,
            .main = 0x80183ee4,
            .ai_init = 0x80103680,
            .ax_init = 0x80104450,
            .alloc = 0x800edc08,
            .sleep_ticks = 0x800f5550,
            .init_sound_system = 0x80047550,
            .archive_ctor = 0x800388e4,
            .archive_open = 0x800389b0,
            .archive_load_info = 0x80038cd8,
            .player_ctor = 0x80043a7c,
            .player_mem_size = 0x80044060,
            .player_strm_size = 0x80044190,
            .player_setup = 0x80043e64,
            .heap_ctor = 0x80045bac,
            .heap_create = 0x80045c64,
            .load_group = 0x80045578,
            .start_sound = 0x80047400,
            .update = 0x8004460c,
            .startable_offset = 0x0c,
            .ios_version = 21,
            .init_flag = 0,
            .signatures = {{
                {0x180, 0x9421ffe0, 0x25fcee24901ac57a},
                {0x54, 0x9421fff0, 0x60936706004ec277},
                {0x34, 0x800d0000, 0xfad1a78efc06182b},
                {0xb4, 0x9421ffb0, 0x7abf305a62b303b7},
                {0x68, 0x9421ffe0, 0x8e7a5abce8d5cb95},
                {0x4c, 0x9421fff0, 0xc7c03c4ee59b1b07},
                {0x160, 0x9421fe80, 0xd3bca4c736f1857e},
                {0x88, 0x9421ffe0, 0x0781f7e8942b6274},
                {0x12c, 0x9421fff0, 0xefb2299afec43230},
                {0x130, 0x9421ffc0, 0x0b056a926bd17fc0},
                {0x4c, 0x9421ffd0, 0x74b85e088a4c1ee5},
                {0xd8, 0x9421ffa0, 0xec36ba63135c5c14},
                {0x48, 0x9421fff0, 0x9358d2ebe15d59cf},
                {0x80, 0x38630000, 0x41e934a477c3a901},
                {0x168, 0x9421fdc0, 0x2420f0b2abacdba3},
                {0x6c, 0x9421fff0, 0x339b90648f261e8a},
                {0x398, 0x9421edb0, 0x867fcef783e7b781},
            }},
        },
        {
            .name = "Wii Play (USA, Rev 1)",
            .game_id = "RHAE01",
            .dol_sha1 = kWiiPlaySha1,
            .main = 0x80183b1c,
            .ai_init = 0x80103068,
            .ax_init = 0x80103e38,
            .alloc = 0x800ed890,
            .sleep_ticks = 0x800f51d8,
            .init_sound_system = 0x80047674,
            .archive_ctor = 0x8003887c,
            .archive_open = 0x80038948,
            .archive_load_info = 0x80038c70,
            .player_ctor = 0x80043ba0,
            .player_mem_size = 0x80044184,
            .player_strm_size = 0x800442b4,
            .player_setup = 0x80043f88,
            .heap_ctor = 0x80045cd0,
            .heap_create = 0x80045d88,
            .load_group = 0x8004569c,
            .start_sound = 0x80047524,
            .update = 0x80044730,
            .startable_offset = 0x0c,
            .ios_version = 21,
            .init_flag = 0,
            .signatures = {{
                {0x180, 0x9421ffe0, 0x25fcee24901ac57a},
                {0x54, 0x9421fff0, 0x60936706004ec277},
                {0x34, 0x800d0000, 0xfad1a78efc06182b},
                {0xb4, 0x9421ffb0, 0x7abf305a62b303b7},
                {0x68, 0x9421ffe0, 0x8e7a5abce8d5cb95},
                {0x4c, 0x9421fff0, 0xc7c03c4ee59b1b07},
                {0x160, 0x9421fe80, 0xd3bca4c736f1857e},
                {0x88, 0x9421ffe0, 0x0781f7e8942b6274},
                {0x12c, 0x9421fff0, 0xefb2299afec43230},
                {0x130, 0x9421ffc0, 0x0b056a926bd17fc0},
                {0x4c, 0x9421ffd0, 0x74b85e088a4c1ee5},
                {0xd8, 0x9421ffa0, 0xec36ba63135c5c14},
                {0x48, 0x9421fff0, 0x9358d2ebe15d59cf},
                {0x80, 0x38630000, 0x41e934a477c3a901},
                {0x168, 0x9421fdc0, 0x2420f0b2abacdba3},
                {0x6c, 0x9421fff0, 0x339b90648f261e8a},
                {0x398, 0x9421edb0, 0x867fcef783e7b781},
            }},
        },
        {
            .name = "WarioWare: Smooth Moves (USA)",
            .game_id = "RODE01",
            .dol_sha1 = kSmoothMovesSha1,
            .main = 0x80007cb4,
            .ai_init = 0x8002dfa0,
            .ax_init = 0x8002e3f0,
            .alloc = 0x80009f10,
            .sleep_ticks = 0x8001266c,
            .init_sound_system = 0x800ed7e4,
            .archive_ctor = 0x800e127c,
            .archive_open = 0x800e1348,
            .archive_load_info = 0x800e1670,
            .player_ctor = 0x800e9abc,
            .player_mem_size = 0x800e9ff8,
            .player_strm_size = 0x800ea128,
            .player_setup = 0x800e9f28,
            .heap_ctor = 0x800ec3dc,
            .heap_create = 0x800ec494,
            .load_group = 0x800eb174,
            .start_sound = 0x800ed694,
            .update = 0x800ea554,
            .startable_offset = 0x0c,
            .ios_version = 9,
            .init_flag = 0,
            .signatures = {{
                {0x180, 0x9421ffe0, 0x25fcee24901ac57a},
                {0x40, 0x9421fff0, 0x50ffca50e67c275d},
                {0x34, 0x800d0000, 0xfad1a78efc06182b},
                {0xb4, 0x9421ffb0, 0x7abf305a62b303b7},
                {0xe8, 0x9421fff0, 0x099bc18647c6a238},
                {0x4c, 0x9421fff0, 0xc7c03c4ee59b1b07},
                {0x160, 0x9421fe80, 0xd3bca4c736f1857e},
                {0x88, 0x9421ffe0, 0x0781f7e8942b6274},
                {0x14c, 0x9421fff0, 0x52ddf5fe747838a6},
                {0x130, 0x9421ffc0, 0xb2416a556048d714},
                {0x4c, 0x9421ffd0, 0x74b85e088a4c1ee5},
                {0xd0, 0x9421ffa0, 0x1ad0f211fb2054ec},
                {0x48, 0x9421fff0, 0x9358d2ebe15d59cf},
                {0x80, 0x38630000, 0x41e934a477c3a901},
                {0x168, 0x9421fdc0, 0x2420f0b2abacdba3},
                {0x6c, 0x9421fff0, 0x339b90648f261e8a},
                {0x378, 0x9421edb0, 0xb5a7cfb4b16209f0},
            }},
        },
        {
            .name = "Mario Kart Wii (USA)",
            .game_id = "RMCE01",
            .dol_sha1 = kMarioKartSha1,
            .main = 0x8000b610,
            .ai_init = 0x80124010,
            .ax_init = 0x80124de0,
            .alloc = 0x801a1064,
            .sleep_ticks = 0x801aac08,
            .init_sound_system = 0x800a40b0,
            .archive_ctor = 0x80090f20,
            .archive_open = 0x80090ff0,
            .archive_load_info = 0x800912f0,
            .player_ctor = 0x800a0460,
            .player_mem_size = 0x800a0ab0,
            .player_strm_size = 0x800a0bf0,
            .player_setup = 0x800a0870,
            .heap_ctor = 0x800a2e30,
            .heap_create = 0x800a2ef0,
            .load_group = 0x800a2810,
            .start_sound = 0x800a3de0,
            .update = 0x800a10e0,
            .startable_offset = 0x0c,
            .ios_version = 36,
            .init_flag = 0x80381e88, // SoundSystem's initialized flag
            .signatures = {{
                {0x180, 0x9421ffe0, 0x25fcee24901ac57a},
                {0x54, 0x9421fff0, 0x60936706004ec277},
                {0x34, 0x800d0000, 0xfad1a78efc06182b},
                {0xb4, 0x9421ffb0, 0x7abf305a62b303b7},
                {0x48, 0x9421ffe0, 0xbbeabbb2087e5770},
                {0x4c, 0x9421fff0, 0x7adb49e8d8126387},
                {0x13c, 0x9421fe80, 0x1829aaf48992bd84},
                {0x88, 0x9421ffe0, 0x0781f7e8942b6274},
                {0x12c, 0x9421fff0, 0xefb2299afec43230},
                {0x13c, 0x9421ffc0, 0x8f04719f78f020b3},
                {0x4c, 0x9421ffd0, 0x74b85e088a4c1ee5},
                {0x104, 0x9421ffa0, 0xb7531c0cd0254cb4},
                {0x48, 0x9421fff0, 0x9358d2ebe15d59cf},
                {0x90, 0x38630000, 0x7941552587452861},
                {0x168, 0x9421fdc0, 0x2420f0b2abacdba3},
                {0x60, 0x9421fff0, 0xaa8d2659b6fa0dcc},
                {0x3bc, 0x9421edb0, 0xa1e88141f4372022},
            }},
        },
        {
            .name = "Wii Music (USA)",
            .game_id = "R64E01",
            .dol_sha1 = kWiiMusicSha1,
            .main = 0x804dd25c,
            .ai_init = 0x800e5f54,
            .ax_init = 0x800e6d24,
            .alloc = 0x8015af88,
            .sleep_ticks = 0x80162cb0,
            .init_sound_system = 0x800c5b2c,
            .archive_ctor = 0x800b542c,
            .archive_open = 0x800b5504,
            .archive_load_info = 0x800b5804,
            .player_ctor = 0x800c2270,
            .player_mem_size = 0x800c2890,
            .player_strm_size = 0x800c29c8,
            .player_setup = 0x800c2660,
            .heap_ctor = 0x800c4b74,
            .heap_create = 0x800c4c2c,
            .load_group = 0x800c45d0,
            .start_sound = 0x800c5a0c,
            .update = 0x800c2ea4,
            .startable_offset = 0x0c,
            .ios_version = 35,
            .init_flag = 0x80698230, // SoundSystem's initialized flag
            .signatures = {{
                {0x180, 0x9421ffe0, 0x25fcee24901ac57a},
                {0x54, 0x9421fff0, 0x60936706004ec277},
                {0x34, 0x800d0000, 0xfad1a78efc06182b},
                {0xb4, 0x9421ffb0, 0x7abf305a62b303b7},
                {0x48, 0x9421ffe0, 0xbbeabbb2087e5770},
                {0x4c, 0x9421fff0, 0xc7c03c4ee59b1b07},
                {0x14c, 0x9421fe80, 0xe9daeffb06a38bd9},
                {0x88, 0x9421ffe0, 0x0781f7e8942b6274},
                {0x134, 0x9421fff0, 0x6ab429ed207f563c},
                {0x138, 0x9421ffc0, 0x6b866f91e5beb1b3},
                {0x4c, 0x9421ffd0, 0x74b85e088a4c1ee5},
                {0x104, 0x9421ffa0, 0x0423269b0abd84e4},
                {0x48, 0x9421fff0, 0x9358d2ebe15d59cf},
                {0x80, 0x38630000, 0x41e934a477c3a901},
                {0x168, 0x9421fdc0, 0x2420f0b2abacdba3},
                {0x60, 0x9421fff0, 0xaa8d2659b6fa0dcc},
                {0x3bc, 0x9421edb0, 0x8aa28a1f29cfc7c2},
            }},
        },
        {
            .name = "Animal Crossing: City Folk (USA)",
            .game_id = "RUUE01",
            .dol_sha1 = kCityFolkSha1,
            .main = 0x800cddec,
            .ai_init = 0x80373b54,
            .ax_init = 0x80374924,
            .alloc = 0x8037be34,
            .sleep_ticks = 0x80385520,
            .init_sound_system = 0x8026e148,
            .archive_ctor = 0x802618b8,
            .archive_open = 0x80261990,
            .archive_load_info = 0x80261c44,
            .player_ctor = 0x8026abb0,
            .player_mem_size = 0x8026b0a4,
            .player_strm_size = 0x8026b1dc,
            .player_setup = 0x8026afa0,
            .heap_ctor = 0x8026d310,
            .heap_create = 0x8026d3c8,
            .load_group = 0x8026cddc,
            .start_sound = 0x8026e040,
            .update = 0x8026b6b8,
            .startable_offset = 0x0c,
            .ios_version = 38,
            .init_flag = 0x8074ef38, // SoundSystem's initialized flag
            .signatures = {{
                {0x180, 0x9421ffe0, 0x25fcee24901ac57a},
                {0x54, 0x9421fff0, 0x60936706004ec277},
                {0x34, 0x800d0000, 0xfad1a78efc06182b},
                {0xb4, 0x9421ffb0, 0x7abf305a62b303b7},
                {0x48, 0x9421ffe0, 0xbbeabbb2087e5770},
                {0x4c, 0x9421fff0, 0xc7c03c4ee59b1b07},
                {0x14c, 0x9421fe80, 0xe9daeffb06a38bd9},
                {0x88, 0x9421ffe0, 0x0781f7e8942b6274},
                {0x134, 0x9421fff0, 0x6ab429ed207f563c},
                {0x138, 0x9421ffc0, 0x6b866f91e5beb1b3},
                {0x4c, 0x9421ffd0, 0x74b85e088a4c1ee5},
                {0x104, 0x9421ffa0, 0x0423269b0abd84e4},
                {0x48, 0x9421fff0, 0x9358d2ebe15d59cf},
                {0x80, 0x38630000, 0x41e934a477c3a901},
                {0x168, 0x9421fdc0, 0x2420f0b2abacdba3},
                {0x60, 0x9421fff0, 0xaa8d2659b6fa0dcc},
                {0x3bc, 0x9421edb0, 0x8aa28a1f29cfc7c2},
            }},
        },
    };
    return kProfiles;
}

bool SameGame(const std::string& game_id, const std::string& other_id)
{
    return other_id.size() >= 3 && game_id.compare(0, 3, other_id, 0, 3) == 0;
}

std::optional<GameProfile> FindProfile(const ntdlib::disc::GameFiles& game)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(game.dol));
    for (const GameProfile& profile : KnownProfiles())
    {
        if (game.GameId() == profile.game_id && sha1 == profile.dol_sha1)
        {
            return profile;
        }
    }

    // Another release or a revision of a game with a profile.
    for (const GameProfile& source : KnownProfiles())
    {
        if (!SameGame(source.game_id, game.GameId()))
        {
            continue;
        }

        std::optional<GameProfile> derived = DeriveProfile(source, game.dol);
        if (derived)
        {
            derived->name = game.Title() + " (" + game.GameId() + ")";
            derived->game_id = game.GameId();
            derived->dol_sha1 = sha1;
            derived->derived_from = source.name;

            return derived;
        }
    }

    return std::nullopt;
}

std::optional<GameProfile> DeriveProfile(const GameProfile& source, std::span<const uint8_t> dol_bytes)
{
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(dol_bytes);
    if (!dol)
    {
        return std::nullopt;
    }

    std::array<uint32_t, kProfileFunctions> addresses;
    for (std::size_t i = 0; i < kProfileFunctions; i++)
    {
        addresses[i] = source.*kProfileFields[i];
    }

    const std::optional<std::vector<uint32_t>> found = FindFunctions(*dol, addresses, source.signatures);
    const std::optional<uint32_t> main = FindMain(*dol);
    if (!found || !main)
    {
        return std::nullopt;
    }

    GameProfile derived = source;
    derived.main = *main;
    for (std::size_t i = 0; i < kProfileFunctions; i++)
    {
        derived.*kProfileFields[i] = (*found)[i];
    }

    // A 2008 build's Setup checks the sound system's flag. Its code gives the flag's address.
    if (source.init_flag != 0)
    {
        const std::optional<uint32_t> flag =
            FindInitFlag(*dol, derived.player_setup, source.signatures[kSetupFunction].size);
        if (!flag)
        {
            return std::nullopt;
        }

        derived.init_flag = *flag;
    }

    return derived;
}

} // namespace wsf::rip
