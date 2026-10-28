// SPDX-License-Identifier: MIT

// The profiles of the games that WSF rips (see rip.h): NW4R SND's driver profiles, with the signatures that find their
// functions in another release, the effect profiles, and the driver profiles of the JAudio games.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/rsar/formats.h"
#include "xsflib/ascii.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "axfx/axfx.h"
#include "nw4r/effect_setup.h"
#include "rip/rip.h"
#include "rip/sound_libraries.h"

namespace wsf::rip
{
namespace
{

// The main.dol files of the games with profiles. Each of them has a driver profile (see KnownProfiles) and an effect
// profile (see KnownEffectProfiles).
constexpr char kWiiSportsSha1[] = "8bb422971b88b5551a37de98db69557df7b46637";
constexpr char kWiiPlaySha1[] = "0da5e7e51135219f580ad011d1b635bc83569bb9";
constexpr char kSmoothMovesSha1[] = "ab32545dc32724f1e4eeeecc05189af722b8addd";
constexpr char kCityFolkSha1[] = "c87967b9ac0a943ba04ac16d875f5f79b4c46516";
constexpr char kMarioKartSha1[] = "1b367209919fcc225c74a155dc46f29c5d64b9a2";
constexpr char kWiiMusicSha1[] = "a9ab9a8a8f14aa58d9bf4e05d72d29115ec589ab";

// The main programs of five channels: content 1 of each title. Each title stores it uncompressed. Each channel has an
// effect profile. A title rips in archive mode only. No channel has a driver profile.
constexpr char kWiiShopChannelSha1[] = "aecf1c655b67c6412998cbc71bf58eba2e4edf03";
constexpr char kMiiChannelSha1[] = "5e0325b32438065bbc33fb82b755a250d93a9f9b";
constexpr char kPhotoChannelSha1[] = "b739b39949d3f3baada78171ef2d3bf36d478669";
constexpr char kForecastChannelSha1[] = "3c3bdec3e81441325e093c49380163e9d744597f";
constexpr char kNewsChannelSha1[] = "1bb643fa5f5e11930849b5efd514f08d30ead975";

// The profile's functions, in the order of their signatures.
constexpr std::array<uint32_t GameProfile::*, kProfileFunctions> kProfileFields = {
    &GameProfile::main,         &GameProfile::ai_init,         &GameProfile::ax_init,
    &GameProfile::alloc,        &GameProfile::sleep_ticks,     &GameProfile::init_sound_system,
    &GameProfile::archive_ctor, &GameProfile::archive_open,    &GameProfile::archive_load_info,
    &GameProfile::player_ctor,  &GameProfile::player_mem_size, &GameProfile::player_strm_size,
    &GameProfile::player_setup, &GameProfile::heap_ctor,       &GameProfile::heap_create,
    &GameProfile::load_group,   &GameProfile::start_sound,     &GameProfile::update,
};

// Opcodes of addi, addis and the D-form loads and stores. The linker may fill in their immediates.
bool IsDForm(uint32_t opcode)
{
    return opcode == 14 || opcode == 15 || (opcode >= 32 && opcode <= 55);
}

// A word of code with the fields that the linker fills in cleared: a branch's target, and the immediate of an addis or
// of a load, store or add that doesn't address the stack (r1).
uint32_t MaskedWord(uint32_t word)
{
    const uint32_t opcode = word >> 26;
    if (opcode == 18)
    {
        return word & 0xfc000003;
    }
    if (IsDForm(opcode) && (opcode == 15 || (word >> 16 & 31) != 1))
    {
        return word & 0xffff0000;
    }

    return word;
}

// The FNV-1a hash of `code`'s masked words, in big-endian order.
uint64_t MaskedHash(std::span<const uint8_t> code)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (std::size_t i = 0; i + 4 <= code.size(); i += 4)
    {
        const uint32_t word = MaskedWord(xsflib::LoadBe32(&code[i]));
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            hash ^= word >> shift & 0xff;
            hash *= 0x100000001b3;
        }
    }

    return hash;
}

// Finds functions of another main.dol in `dol` by their signatures: for each address in `addresses`, the place in `dol`
// of the function with the signature at the same index in `signatures`. A small function can match in several places.
// The functions of a library move together. The offset of the nearest function that matched once therefore picks the
// place. Returns the places, or nothing if a function isn't there.
std::optional<std::vector<uint32_t>> FindFunctions(const ntdlib::Dol& dol, std::span<const uint32_t> addresses,
                                                   std::span<const FunctionSignature> signatures)
{
    // The places whose code matches each function's signature.
    std::vector<std::vector<uint32_t>> places(addresses.size());
    for (const ntdlib::Dol::Section& section : dol.Sections())
    {
        for (std::size_t at = 0; at + 4 <= section.data.size(); at += 4)
        {
            const uint32_t first = MaskedWord(xsflib::LoadBe32(&section.data[at]));
            for (std::size_t i = 0; i < addresses.size(); i++)
            {
                const FunctionSignature& signature = signatures[i];
                if (first == signature.first && at + signature.size <= section.data.size() &&
                    MaskedHash(section.data.subspan(at, signature.size)) == signature.hash)
                {
                    places[i].push_back(section.address + static_cast<uint32_t>(at));
                }
            }
        }
    }

    // How far each function that matched once moved.
    std::vector<std::optional<int64_t>> moved(addresses.size());
    for (std::size_t i = 0; i < addresses.size(); i++)
    {
        if (places[i].empty())
        {
            return std::nullopt;
        }

        if (places[i].size() == 1)
        {
            moved[i] = int64_t{places[i][0]} - addresses[i];
        }
    }

    std::vector<uint32_t> found(addresses.size());
    for (std::size_t i = 0; i < addresses.size(); i++)
    {
        // A function that matched in several places moved as far as the nearest function that matched once.
        const uint32_t address = addresses[i];
        const auto distance = [addresses, address](std::size_t k)
        {
            return std::abs(int64_t{addresses[k]} - address);
        };

        std::optional<std::size_t> nearest;
        for (std::size_t j = 0; j < addresses.size(); j++)
        {
            if (moved[j] && (!nearest || distance(j) < distance(*nearest)))
            {
                nearest = j;
            }
        }
        if (!nearest)
        {
            return std::nullopt;
        }

        const int64_t offset = moved[i] ? *moved[i] : *moved[*nearest];
        const auto place = std::find(places[i].begin(), places[i].end(), static_cast<uint32_t>(address + offset));
        if (place == places[i].end())
        {
            return std::nullopt;
        }

        found[i] = *place;
    }

    return found;
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
                {0x34, 0x9421fff0, 0x4d2783dea7611fe9},
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
                {0x34, 0x9421fff0, 0x4d2783dea7611fe9},
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
                {0x1a0, 0x9421fff0, 0x94a7ddd954409b2c},
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
                {0x3c, 0x48000000, 0x8f13a0d9937e24b3},
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
                {0x34, 0x9421fff0, 0x4d2783dea7611fe9},
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
                {0xb4, 0x542b06fe, 0x7028186cbbedc369},
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
        if (!SameGame(source.game_id, game.GameId()) || source.init_flag != 0)
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
    if (!found)
    {
        return std::nullopt;
    }

    GameProfile derived = source;
    for (std::size_t i = 0; i < kProfileFunctions; i++)
    {
        derived.*kProfileFields[i] = (*found)[i];
    }

    return derived;
}

namespace
{

// An effect's parameters as the words of its parameter structure: floats as their bits.
nw4r::EffectParams Words(std::initializer_list<float> values)
{
    nw4r::EffectParams params{};
    std::size_t i = 0;
    for (const float value : values)
    {
        params[i++] = std::bit_cast<uint32_t>(value);
    }

    return params;
}

nw4r::EffectSetup Fixed(int bus, nw4r::EffectType type, const nw4r::EffectParams& params)
{
    nw4r::EffectSetup setup;
    setup.bus = bus;
    setup.type = type;
    setup.params = params;

    return setup;
}

// FxReverbHi's parameters: preDelayTime, fusedTime, coloration, damping, crossTalk and outGain.
nw4r::EffectSetup ReverbHi(float pre_delay, float time, float coloration, float damping, float crosstalk, float out)
{
    return Fixed(0, nw4r::EffectType::kReverbHi, Words({pre_delay, time, coloration, damping, crosstalk, out}));
}

// FxChorus's default parameters (FxChorus::FxChorus, 0x80039d88): delayTime 10 ms, depth 0.5 and rate 1 Hz.
nw4r::EffectSetup DefaultChorus(int bus)
{
    return Fixed(bus, nw4r::EffectType::kChorus, Words({10.0f, 0.5f, 1.0f}));
}

// The modes and gains of the reverbs of 2008: earlyMode, preDelayTimeMax, fusedMode, earlyGain and fusedGain.
struct Reverb2008
{
    uint32_t early_mode;
    float pre_delay_time_max;
    uint32_t fused_mode;
    float early_gain;
    float fused_gain;
};

// Puts a 2008 reverb's outGain, modes and gains into `params`, at the places that `words` gives.
void PutReverb2008(nw4r::EffectParams& params, const nw4r::Reverb2008Words& words, float out, const Reverb2008& more)
{
    const auto put = [&params](int word, uint32_t value)
    {
        params[static_cast<std::size_t>(word)] = value;
    };

    put(words.out_gain, std::bit_cast<uint32_t>(out));
    put(words.early_mode, more.early_mode);
    put(words.pre_delay_time_max, std::bit_cast<uint32_t>(more.pre_delay_time_max));
    put(words.fused_mode, more.fused_mode);
    put(words.early_gain, std::bit_cast<uint32_t>(more.early_gain));
    put(words.fused_gain, std::bit_cast<uint32_t>(more.fused_gain));
}

// FxReverbHi of 2008 on bus A: preDelayTime, fusedTime, coloration, damping, crossTalk, outGain, then the modes and
// gains.
nw4r::EffectSetup ReverbHi2008(float pre_delay, float time, float coloration, float damping, float crosstalk, float out,
                               const Reverb2008& more)
{
    nw4r::EffectParams params = Words({pre_delay, time, coloration, damping, crosstalk});
    PutReverb2008(params, nw4r::kReverbHi2008Words, out, more);

    return Fixed(0, nw4r::EffectType::kReverbHi2008, params);
}

// FxReverbStd of 2008 on bus A: preDelayTime, fusedTime, coloration, damping, outGain, then the modes and gains.
nw4r::EffectSetup ReverbStd2008(float pre_delay, float time, float coloration, float damping, float out,
                                const Reverb2008& more)
{
    nw4r::EffectParams params = Words({pre_delay, time, coloration, damping});
    PutReverb2008(params, nw4r::kReverbStd2008Words, out, more);

    return Fixed(0, nw4r::EffectType::kReverbStd2008, params);
}

// The sounds of `archive` whose names `matches` accepts.
template <typename Matches>
std::vector<uint32_t> SoundsWhere(const ntdlib::rsar::SoundArchive& archive, Matches matches)
{
    std::vector<uint32_t> sounds;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (matches(archive.Sounds()[i].name))
        {
            sounds.push_back(i);
        }
    }

    return sounds;
}

// The sounds of `archive` with one of `names`.
std::vector<uint32_t> SoundsNamed(const ntdlib::rsar::SoundArchive& archive,
                                  std::initializer_list<std::string_view> names)
{
    const auto is_named = [names](const std::string& sound)
    {
        return std::find(names.begin(), names.end(), sound) != names.end();
    };
    return SoundsWhere(archive, is_named);
}

// The file name of the archive at `path`, in lower case.
std::string ArchiveFileName(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    return xsflib::AsciiLower(slash == std::string::npos ? path : path.substr(slash + 1));
}

// The effects of Wii Sports. The menus have NW4R SND's default reverb on bus A (RPSndAudioMgr::setDefaultFx,
// 0x801b6d00, with the parameters at 0x8037ae68). Each sport sets bus A again as its scene starts. Tennis (0x80214e40),
// baseball (0x8024e7dc) and bowling (0x803153c0) keep that reverb and add FxChorus's default on bus B. Golf
// (0x8028e664) and boxing (0x802ee324) have reverbs with other times and levels.
nw4r::ArchiveEffects WiiSportsEffects(const std::string& name, const ntdlib::rsar::SoundArchive&)
{
    nw4r::ArchiveEffects found;
    const nw4r::EffectSetup menu = ReverbHi(0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f);
    if (name == "rp_common_sports.brsar")
    {
        found.setups = {menu};
    }
    else if (name == "rp_tns_sound.brsar" || name == "rp_bsb_sound.brsar" || name == "rp_bow_sound.brsar")
    {
        found.setups = {menu, DefaultChorus(1)};
    }
    else if (name == "rp_gol_sound.brsar")
    {
        found.setups = {ReverbHi(0.02f, 1.2f, 0.6f, 0.4f, 0.1f, 0.7f)};
    }
    else if (name == "rp_box_sound.brsar")
    {
        found.setups = {ReverbHi(0.02f, 2.2f, 0.6f, 0.4f, 0.1f, 0.5f)};
    }

    return found;
}

// The effects of Wii Play. Every scene has the same effects (setDefaultFx, 0x801b3588, with the parameters at
// 0x803182c0): a reverb on bus A, a chorus on bus B and a delay on bus C.
nw4r::ArchiveEffects WiiPlayEffects(const std::string& name, const ntdlib::rsar::SoundArchive&)
{
    nw4r::ArchiveEffects found;
    if (name.starts_with("rp_"))
    {
        found.setups = {ReverbHi(0.03f, 1.5f, 0.0f, 0.1f, 0.2f, 0.5f),
                        Fixed(1, nw4r::EffectType::kChorus, Words({5.0f, 1.0f, 0.6f})),
                        Fixed(2, nw4r::EffectType::kDelay, Words({40.0f, 0.0f, 0.6f}))};
    }

    return found;
}

// The effects of WarioWare: Smooth Moves. The start-up of the sound object (0x80100198) puts a delay on bus B
// (parameters at 0x804da1e4) and a chorus on bus C (0x804da25c). The music's variables pick bus A's reverb from the 10
// rows at 0x804da11c: preDelay, time, coloration, damping and mix.
nw4r::ArchiveEffects SmoothMovesEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    if (name == "sound_data_jp.brsar")
    {
        nw4r::EffectSetup reverb;
        reverb.type = nw4r::EffectType::kReverbStd;
        reverb.presets = {Words({0.02f, 3.0f, 0.6f, 0.4f, 0.25f}), Words({0.02f, 1.5f, 0.6f, 0.4f, 0.25f}),
                          Words({0.02f, 3.0f, 0.6f, 0.4f, 0.25f}), Words({0.03f, 3.0f, 0.5f, 0.4f, 0.5f}),
                          Words({0.02f, 1.5f, 0.6f, 0.4f, 0.25f}), Words({0.03f, 2.0f, 0.6f, 0.4f, 0.3f}),
                          Words({0.08f, 3.0f, 0.6f, 0.4f, 0.25f}), Words({0.04f, 3.0f, 0.6f, 0.4f, 0.25f}),
                          Words({0.04f, 3.0f, 0.6f, 0.4f, 0.25f}), Words({0.08f, 3.0f, 0.6f, 0.4f, 0.25f})};
        for (uint32_t i = 0; i < archive.Sounds().size(); i++)
        {
            if (IsMusic(archive, i))
            {
                reverb.sounds.push_back(i);
            }
        }

        found.setups = {Fixed(1, nw4r::EffectType::kDelay, Words({160.0f, 0.4f, 1.0f})),
                        Fixed(2, nw4r::EffectType::kChorus, Words({10.0f, 0.5f, 1.0f})), reverb};
    }

    return found;
}

// What Animal Crossing: City Folk sets up for its sounds. The sound object's start-up (0x801ce58c) puts FxReverbHi on
// bus A with the parameters of 2008: those of the older builds, then earlyMode, preDelayTimeMax, fusedMode, earlyGain
// and fusedGain. The start of the music (0x801cbbbc) sends 0.25 of the outdoor and town music to bus A, and 0.28 of
// K.K. Slider's songs.
nw4r::ArchiveEffects CityFolkEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    if (name == "rv_forest.brsar")
    {
        found.setups = {ReverbHi2008(0.01f, 0.5f, 0.6f, 0.4f, 0.1f, 1.0f, {4, 0.05f, 2, 0.3f, 0.3f})};

        const auto send = [&found, &archive](uint32_t first, uint32_t last, float value)
        {
            for (uint32_t i = first; i <= last && i < archive.Sounds().size(); i++)
            {
                found.sends.push_back({i, 0, value});
            }
        };

        send(0x1a11, 0x1a28, 0.25f); // BGM_OUTDOOR_00 to 23
        send(0x1aa3, 0x1aa6, 0.25f); // BGM_TOWN_*
        send(0x1a29, 0x1a77, 0.28f); // BGM_KK00 to 78

        // The outdoor and town music and BGM_MUSEUM play some of their tracks. As one of them starts, the game
        // (0x801cc23c) gives SeqSound::SetTrackVolume (0x80267774) a volume of 0 for the tracks outside a mask. Each
        // piece of outdoor and town music has three masks by the weather (0x8007e000), in the tables at 0x8047e9f0 and
        // 0x8047ea90: the first for weathers 0 to 2, the others for 3 and 4, and 5 and 6. A rip plays the first. The
        // town music also silences track 15 (0x801cbc28). BGM_MUSEUM's masks go by the room (0x8047ea80). A rip plays
        // the mask of the state that the start of the music picks by default (0x801cb9bc).
        static constexpr uint16_t kOutdoor[] = {0x00ff, 0x07ff, 0x007f, 0x03ff, 0x009f, 0x002f, 0x05ff, 0x09ff,
                                                0x447f, 0x009f, 0x04ff, 0x13ff, 0x00ff, 0x00ff, 0x03ff, 0x007f,
                                                0x07ff, 0x02ff, 0x077f, 0x01ff, 0x007f, 0x1f7f, 0x19ff, 0x003f};
        static constexpr uint16_t kTown[] = {0x99ff, 0x07ff, 0x01ff, 0x003f};

        const auto play = [&found, &archive](uint32_t sound, uint16_t tracks)
        {
            if (sound < archive.Sounds().size())
            {
                found.tracks.push_back({sound, tracks});
            }
        };

        for (uint32_t i = 0; i < std::size(kOutdoor); i++)
        {
            play(0x1a11 + i, kOutdoor[i]); // BGM_OUTDOOR_00 to 23
        }
        for (uint32_t i = 0; i < std::size(kTown); i++)
        {
            play(0x1aa3 + i, kTown[i] & 0x7fff); // BGM_TOWN_EARLY, _DAY, _NIGHT and _MIDNIGHT
        }
        play(0x1a86, 0x00e7); // BGM_MUSEUM
    }

    return found;
}

// The effects of Mario Kart Wii. Only bus A has an effect: the standard reverb of 2008 that AudioFx::create
// (0x802119f4) makes for type 4. As each scene starts, code in the game's REL module sets it up from one set of
// parameters. A race picks the set by course (.text + 0x2027dc). Mushroom Gorge and the N64 courses Mario Raceway,
// Sherbet Land and DK's Jungle Parkway get a shorter pre-delay and less damping. The names of their sounds start with
// SE_CN_KNK_, SE_CR_64M_, SE_CR_64S_ and SE_CR_64D_. A menu picks the set by the sound type of its section (.text +
// 0x2021c0). The type is the group of the section's music. SEQ_O_SELECT_CH (group 2) and SEQ_O_EARTH (group 3) get a
// lower outGain. SEQ_O_RANKING_CH (group 4) gets another reverb. Other sounds get the set as it is. Only six of the
// archive's sequences send anything to bus A: these three, SEQ_O_OPTION_CH and two sounds of Sherbet Land.
nw4r::ArchiveEffects MarioKartEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    if (name != "revo_kart.brsar")
    {
        return {};
    }

    nw4r::ArchiveEffects found;
    const auto is_course_sound = [](const std::string& sound)
    {
        return sound.starts_with("SE_CN_KNK_") || sound.starts_with("SE_CR_64M_") || sound.starts_with("SE_CR_64S_") ||
               sound.starts_with("SE_CR_64D_");
    };

    nw4r::EffectSetup courses = ReverbStd2008(0.015f, 3.0f, 0.6f, 0.35f, 1.0f, {5, 0.02f, 5, 0.0f, 1.0f});
    courses.sounds = SoundsWhere(archive, is_course_sound);
    nw4r::EffectSetup menus = ReverbStd2008(0.02f, 3.0f, 0.6f, 0.4f, 0.73f, {5, 0.02f, 0, 0.0f, 1.0f});
    menus.sounds = SoundsNamed(archive, {"SEQ_O_SELECT_CH", "SEQ_O_EARTH"});
    nw4r::EffectSetup ranking = ReverbStd2008(0.025f, 1.5f, 0.5f, 0.5f, 1.0f, {5, 0.025f, 0, 0.0f, 0.5f});
    ranking.sounds = SoundsNamed(archive, {"SEQ_O_RANKING_CH"});
    found.setups = {ReverbStd2008(0.02f, 3.0f, 0.6f, 0.4f, 1.0f, {5, 0.02f, 0, 0.0f, 1.0f}), courses, menus, ranking};

    return found;
}

// The effects of Wii Music. Each scene sets the buses from a row of the table at 0x8059f978 (0x803cddc0). In every
// scene but Mii Maestro's, bus A has the standard reverb of 2008 with row 0 of the parameters at 0x8059fe20. Mii
// Maestro has FxReverbHi with row 2 of the parameters at 0x8059fd58. The names of its sounds start with RP_OCS_. The
// game sets the longest pre-delay of both to 0.09 (0x803cdfb4). Some scenes put a reverb on bus B too. No sequence
// sends anything to bus B there. The game sets sends on the instruments of a jam session by stage. A rip doesn't know
// the stage and leaves those sends out.
nw4r::ArchiveEffects WiiMusicEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    if (!name.starts_with("rp_"))
    {
        return {};
    }

    nw4r::ArchiveEffects found;
    const auto is_orchestra_sound = [](const std::string& sound)
    {
        return sound.starts_with("RP_OCS_");
    };

    nw4r::EffectSetup orchestra = ReverbHi2008(0.02f, 1.2f, 0.5f, 0.3f, 0.1f, 0.9f, {5, 0.09f, 4, 0.5f, 1.0f});
    orchestra.sounds = SoundsWhere(archive, is_orchestra_sound);
    found.setups = {ReverbStd2008(0.0f, 1.3f, 0.5f, 0.5f, 1.0f, {5, 0.09f, 4, 0.5f, 1.0f}), orchestra};

    return found;
}

// The effects of the Wii Shop Channel. Its sound start-up (0x80023aa0) puts FxReverbHi of 2008 on bus A (0x80023b80)
// with the parameters that 0x800239e0 sets: no pre-delay, fusedTime 2.5, coloration 0.5, no damping or crossTalk and an
// outGain of 1. The channel clears the buses as it pauses its sounds (0x800241a0) and sets the reverb up again as they
// resume (0x80024230). It sets no send on a sound.
nw4r::ArchiveEffects WiiShopChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive&)
{
    nw4r::ArchiveEffects found;
    if (name == "iplsound.brsar")
    {
        found.setups = {ReverbHi2008(0.0f, 2.5f, 0.5f, 0.0f, 0.0f, 1.0f, {5, 0.0f, 0, 0.0f, 1.0f})};
    }

    return found;
}

// The variants of `music`, SEQ_TMP_BGM, in the Mii Channel's other states. 0x80018018 sets the music's state by the
// jump table at 0x80233a20. The channel's code names none of its scenes but the root scene (0x80206548). Its scene
// factory (0x80010484) gives each scene a number. A variant's name gives a scene's number, or the sound that the code
// plays with the change of state:
// - " scene 4": track 10 alone. Scene 4 sets it (0x8002c2d8).
// - " menu out": tracks 9 and 11. The code lets track 11 play as it plays SEQ_DOOL_MENU_OUT. With
//   SEQ_DOOL_INTO_MENU, it silences track 11 again. Neither change touches tracks 9 and 10. The rip therefore adds
//   track 11 to the tracks from boot.
// - " scenes 1 and 3": none of the three. Scenes 1 and 3 set it (0x80011b30 and 0x8001755c).
// - " plaza speed up": every track but 10 and 11, at a tempo ratio of 1.0878 (0x8001862c). The code sets the ratio as
//   it plays SEQ_PLAZA_SPEED_UP. It sets the ratio back to 1 with SEQ_PLAZA_SPEED_DOWN.
// The channel also turns the music down to a volume of 0.4 and back up (0x8001868c and 0x800186e4). An arrangement
// holds no volume. That state therefore has no rip.
std::vector<nw4r::EffectVariant> MiiScenes(uint32_t music)
{
    const auto scene = [music](const char* name, uint16_t tracks, float tempo_ratio)
    {
        nw4r::Arrangement arrangement;
        arrangement.tracks = tracks;
        arrangement.tempo_ratio = tempo_ratio;

        return nw4r::EffectVariant{
            .name = name, .sounds = {music}, .setups = {}, .sends = {}, .arrangement = arrangement};
    };

    return {scene(" scene 4", 0xf5ff, 1.0f), scene(" menu out", 0xfbff, 1.0f), scene(" scenes 1 and 3", 0xf1ff, 1.0f),
            scene(" plaza speed up", 0xf3ff, 1.0878f)};
}

// The effects of the Mii Channel. The sound object's start-up (0x80017ff4) puts FxReverbHi on bus A through EGG's
// AudioFxMgr (0x80151424). The parameters at 0x80206878 give no pre-delay, fusedTime 3, coloration 0.5, no damping or
// crossTalk and an outGain of 0.25. Each of the channel's four starters of sound effects (0x80018bb4, 0x80018cf8,
// 0x80018ddc and 0x80019474) sends 0.2 of its sound to bus A (BasicSound::SetFxSend, 0x800f73a0). The music,
// SEQ_TMP_BGM, starts without a send (0x80018a2c). As the music starts, the channel silences its tracks 9, 10 and 11
// (SeqSoundHandle::SetTrackSilence, 0x800fdf0c). The scenes then let some of them play (see MiiScenes). A rip plays
// what the channel plays from boot: every track but 10 and 11. The music also rips once in each of its other states.
nw4r::ArchiveEffects MiiChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    if (name == "nigaoesound.brsar")
    {
        found.setups = {ReverbHi(0.0f, 3.0f, 0.5f, 0.0f, 0.0f, 0.25f)};
        for (uint32_t i = 0; i < archive.Sounds().size(); i++)
        {
            if (archive.Sounds()[i].name == "SEQ_TMP_BGM")
            {
                found.tracks.push_back({i, 0xf3ff});
                found.variants = MiiScenes(i);
            }
            else
            {
                found.sends.push_back({i, 0, 0.2f});
            }
        }
    }

    return found;
}

// The Photo Channel 1.1, the Forecast Channel and the News Channel share a sound manager. As each channel starts, the
// sound manager puts FxReverbHi on bus C with preDelayTime 0.1, fusedTime 2.1, coloration 0.6, damping 0.4, crossTalk
// 0.1 and outGain 0.2 (AppendEffect is called at 0x80048a0c in the Photo Channel 1.1, 0x80041b60 in the Forecast
// Channel and 0x8004ee00 in the News Channel). Nothing changes the reverb later. The music sends to bus C from its
// sequences. The sound manager also puts an effect that the channel defines on bus B. That effect starts off. Nothing
// sends to bus B. Only the Photo Channel turns it on. While a photo with a mood is shown, the music of the photos
// (BGM_MAIN and BGM_MAIN_KEIJI) moves from the main mix and bus C to bus B (0x80049614). Both pieces also play without
// a mood. Their plain rips play the channel as it starts, with nothing on bus B. Each piece also rips once with each
// mood.
constexpr float kChannelPreDelay = 0.1f;
constexpr float kChannelTime = 2.1f;
constexpr float kChannelColoration = 0.6f;
constexpr float kChannelDamping = 0.4f;
constexpr float kChannelCrosstalk = 0.1f;
constexpr float kChannelOut = 0.2f;

// The modes that the Photo Channel's moods 1 to 4 set (0x800212c8).
constexpr std::array<uint32_t, 4> kMoodModes = {1, 4, 3, 2};

// The variants of the Photo Channel's pieces with a mood. A photo's record gives its mood at +0x18c. While the photo is
// shown with BGM_MAIN or BGM_MAIN_KEIJI, 0x80020d64 turns the effect on bus B on. 0x8002123c sets the mood's mode.
// 0x80049614 sets the music's main send to -1, its send to bus C to -1 and its send to bus B to 1. The music then plays
// through the effect alone.
std::vector<nw4r::EffectVariant> PhotoMoods(const ntdlib::rsar::SoundArchive& archive)
{
    const std::vector<uint32_t> photos = SoundsNamed(archive, {"BGM_MAIN", "BGM_MAIN_KEIJI"});
    std::vector<nw4r::EffectVariant> moods;
    for (std::size_t mood = 0; mood < kMoodModes.size(); mood++)
    {
        nw4r::EffectSetup effect = Fixed(1, nw4r::EffectType::kPhotoMood, {kMoodModes[mood]});
        effect.sounds = photos;
        nw4r::EffectVariant variant{
            .name = " mood " + std::to_string(mood + 1), .sounds = photos, .setups = {effect}, .sends = {}};
        for (const uint32_t sound : photos)
        {
            variant.sends.push_back({sound, nw4r::kMainSendBus, -1.0f});
            variant.sends.push_back({sound, 2, -1.0f});
            variant.sends.push_back({sound, 1, 1.0f});
        }

        moods.push_back(variant);
    }

    return moods;
}

// The Photo Channel's NW4R SND of 2008 reads five more words. They keep the defaults that the sound manager's
// constructor gives them (0x8004b9a8). FxReverbHi::SetParam then bounds the pre-delay by preDelayTimeMax, 0.02.
nw4r::ArchiveEffects PhotoChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive& archive)
{
    nw4r::ArchiveEffects found;
    if (name == "rev_photo.brsar")
    {
        nw4r::EffectSetup reverb = ReverbHi2008(kChannelPreDelay, kChannelTime, kChannelColoration, kChannelDamping,
                                                kChannelCrosstalk, kChannelOut, {5, 0.02f, 0, 0.0f, 1.0f});
        reverb.bus = 2;
        found.setups = {reverb};
        found.variants = PhotoMoods(archive);
    }

    return found;
}

// The older FxReverbHi of the Forecast Channel and the News Channel, for the channel's archive.
nw4r::ArchiveEffects OlderChannelEffects(const std::string& name, const char* archive_name)
{
    nw4r::ArchiveEffects found;
    if (name == archive_name)
    {
        found.setups = {Fixed(2, nw4r::EffectType::kReverbHi,
                              Words({kChannelPreDelay, kChannelTime, kChannelColoration, kChannelDamping,
                                     kChannelCrosstalk, kChannelOut}))};
    }

    return found;
}

nw4r::ArchiveEffects ForecastChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive&)
{
    return OlderChannelEffects(name, "rev_wtr.brsar");
}

nw4r::ArchiveEffects NewsChannelEffects(const std::string& name, const ntdlib::rsar::SoundArchive&)
{
    return OlderChannelEffects(name, "rev_news.brsar");
}

// The addresses of `profile`'s functions, in the order of its signatures: AXRegisterCallback, AxManager's GetInstance,
// AppendEffect and ClearEffect, SeqSoundHandle's constructor and destructor and SeqSound::ReadVariable where the
// profile has them, then each class's functions.
std::vector<uint32_t*> EffectFunctions(EffectProfile& profile)
{
    std::vector<uint32_t*> functions = {&profile.ax_register_callback, &profile.ax_manager, &profile.append_effect,
                                        &profile.clear_effect};
    for (uint32_t* function : {&profile.seq_handle_ctor, &profile.seq_handle_dtor, &profile.read_variable})
    {
        if (*function != 0)
        {
            functions.push_back(function);
        }
    }

    for (EffectClass& effect_class : profile.classes)
    {
        functions.insert(functions.end(),
                         {&effect_class.ctor, &effect_class.mem_size, &effect_class.assign, &effect_class.set_param,
                          &effect_class.start_up, &effect_class.shutdown, &effect_class.update_buffer});
    }

    return functions;
}

const std::vector<EffectProfile>& KnownEffectProfiles()
{
    static const std::vector<EffectProfile> kProfiles = {
        {
            .dol_sha1 = kWiiSportsSha1,
            .effects = WiiSportsEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x80106260,
            .ax_manager = 0x800312e8,
            .append_effect = 0x80031d5c,
            .clear_effect = 0x80031f60,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x8003a668, 0x8003a6f4, 0x8003a720, 0x8003a84c, 0x8003a730,
                     0x8003a7ec, 0x8003aa38},
                    {nw4r::EffectType::kChorus, 0x80039d88, 0x80039dfc, 0x80039e28, 0x80039f54, 0x80039e38, 0x80039ef4,
                     0x8003a0c0},
                },
        },
        {
            .dol_sha1 = kWiiPlaySha1,
            .effects = WiiPlayEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x80105c48,
            .ax_manager = 0x800312e0,
            .append_effect = 0x80031d54,
            .clear_effect = 0x80031f58,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x8003a600, 0x8003a68c, 0x8003a6b8, 0x8003a7e4, 0x8003a6c8,
                     0x8003a784, 0x8003a9d0},
                    {nw4r::EffectType::kChorus, 0x80039d20, 0x80039d94, 0x80039dc0, 0x80039eec, 0x80039dd0, 0x80039e8c,
                     0x8003a058},
                    {nw4r::EffectType::kDelay, 0x8003a118, 0x8003a28c, 0x8003a2b8, 0x8003a3e4, 0x8003a2c8, 0x8003a384,
                     0x8003a540},
                },
        },
        {
            .dol_sha1 = kSmoothMovesSha1,
            .game_id = "RODE01",
            .effects = SmoothMovesEffects,
            .axfx_build = axfx::Build::kOctober2006,
            .ax_register_callback = 0x8002ff80,
            .ax_manager = 0x800d97e8,
            .append_effect = 0x800da878,
            .clear_effect = 0x800daa7c,
            .seq_handle_ctor = 0x800e6d68,
            .seq_handle_dtor = 0x800feef0,
            .read_variable = 0x800e6cbc,
            .classes =
                {
                    {nw4r::EffectType::kReverbStd, 0x800e31d8, 0x800e325c, 0x800e3288, 0x800e33ac, 0x800e3298,
                     0x800e334c, 0x800e3538},
                    {nw4r::EffectType::kChorus, 0x800e2514, 0x800e2588, 0x800e25b4, 0x800e26d8, 0x800e25c4, 0x800e2678,
                     0x800e2820},
                    {nw4r::EffectType::kDelay, 0x800e28e0, 0x800e2a34, 0x800e2a60, 0x800e2b84, 0x800e2a70, 0x800e2b24,
                     0x800e2cbc},
                },
            // AXRegisterCallback and SeqSoundHandle's destructor match in several places. AXInit places the first. The
            // game's function that reads the music's variables (0x800fee28, which driver.c follows) places the second.
            .anchors = {0x8002e3f0, 0x800fee28},
            .signatures =
                {
                    {0x44, 0x9421fff0, 0xa0d7e48a180ad991},  {0x64, 0x9421fff0, 0xc6bcc4e1821f0a4e},
                    {0x204, 0x9421ffc0, 0x9d5cc845ad848986}, {0xc8, 0x9421ffe0, 0xe2ec3a9c84a610f3},
                    {0x108, 0x9421ffe0, 0x3ab8381b7a6d4019}, {0x54, 0x9421fff0, 0x8c27b2db545ed4aa},
                    {0x50, 0x9421fff0, 0xe4d13d3b9e57f7e0},  {0x84, 0x9421ffd0, 0xe7af0ca778fbb3b0},
                    {0x2c, 0x9421fff0, 0x03071cb3942a82a7},  {0xc4, 0x38630000, 0x68cbcd27cd639700},
                    {0x18c, 0x9421fff0, 0xc5bafced5b1e6603}, {0xb4, 0x9421ffe0, 0x9e48aa8c140d1b40},
                    {0x60, 0x9421fff0, 0x8995c55a93d3a9c2},  {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},
                    {0x74, 0x9421ffe0, 0x4242d47254e35f43},  {0x2c, 0x9421fff0, 0x03071cb3942a82a7},
                    {0xc4, 0x38630000, 0x68cbcd27cd639700},  {0x148, 0x9421ffe0, 0x9fa6a8ea745a3664},
                    {0xb4, 0x9421ffe0, 0x9e48aa8c140d1b40},  {0x60, 0x9421fff0, 0x8995c55a93d3a9c2},
                    {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},  {0x154, 0x9421ffe0, 0x875ce753b7fed176},
                    {0x2c, 0x9421fff0, 0x03071cb3942a82a7},  {0xc4, 0x38630000, 0x68cbcd27cd639700},
                    {0x138, 0x9421fff0, 0x3d63a8a733f6c736}, {0xb4, 0x9421ffe0, 0x9e48aa8c140d1b40},
                    {0x60, 0x9421fff0, 0x8995c55a93d3a9c2},  {0x44, 0x9421ffe0, 0xa0c96b3a5b882892},
                    {0x40, 0x9421fff0, 0x50ffca50e67c275d},  {0xc8, 0x9421ffe0, 0x535d001042662a33},
                },
        },
        {
            .dol_sha1 = kCityFolkSha1,
            .effects = CityFolkEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x80376748,
            .ax_manager = 0x8025914c,
            .append_effect = 0x80259b98,
            .clear_effect = 0x80259d9c,
            .sound_sends = 0xd0,
            .seq_tracks = 0x22c,
            .track_volume = 0x8,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi2008, 0x80262d10, 0x80262de0, 0x80262e34, 0x80262fe8, 0x80262e44,
                     0x80262f6c, 0x8026333c},
                },
        },
        {
            .dol_sha1 = kMarioKartSha1,
            .effects = MarioKartEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x80126c04,
            .ax_manager = 0x80088750,
            .append_effect = 0x80089170,
            .clear_effect = 0x80089360,
            .classes =
                {
                    {nw4r::EffectType::kReverbStd2008, 0x80095080, 0x80095150, 0x800951b0, 0x80095380, 0x800951d0,
                     0x80095300, 0x800956a0},
                },
        },
        {
            .dol_sha1 = kWiiMusicSha1,
            .effects = WiiMusicEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x800e8b48,
            .ax_manager = 0x800ac63c,
            .append_effect = 0x800ad130,
            .clear_effect = 0x800ad334,
            .classes =
                {
                    {nw4r::EffectType::kReverbStd2008, 0x800b7f24, 0x800b7fe8, 0x800b8014, 0x800b813c, 0x800b8024,
                     0x800b80d8, 0x800b83d4},
                    {nw4r::EffectType::kReverbHi2008, 0x800b71a8, 0x800b7278, 0x800b72a4, 0x800b73cc, 0x800b72b4,
                     0x800b7368, 0x800b7698},
                },
        },
        // The channels. A title rips in archive mode only. The addresses for game mode's driver are those of the
        // channel's functions.
        {
            .dol_sha1 = kWiiShopChannelSha1,
            .effects = WiiShopChannelEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x801ec8e0,
            .ax_manager = 0x801824e0,
            .append_effect = 0x80182f70,
            .clear_effect = 0x80183160,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi2008, 0x8018d6b0, 0x8018d780, 0x8018d7b0, 0x8018d900, 0x8018d7d0,
                     0x8018d890, 0x8018dbd0},
                },
        },
        {
            .dol_sha1 = kMiiChannelSha1,
            .effects = MiiChannelEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x801a4dd8,
            .ax_manager = 0x800f0e60,
            .append_effect = 0x800f1e40,
            .clear_effect = 0x800f205c,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x800f9ac0, 0x800f9b4c, 0x800f9b78, 0x800f9c9c, 0x800f9b88,
                     0x800f9c3c, 0x800f9e5c},
                },
        },
        {
            .dol_sha1 = kPhotoChannelSha1,
            .effects = PhotoChannelEffects,
            .axfx_build = axfx::Build::kSeptember2008,
            .ax_register_callback = 0x8016e90c,
            .ax_manager = 0x801169c4,
            .append_effect = 0x801174b8,
            .clear_effect = 0x801176bc,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi2008, 0x80120854, 0x80120924, 0x80120950, 0x80120a78, 0x80120960,
                     0x80120a14, 0x80120d44},
                },
        },
        {
            .dol_sha1 = kForecastChannelSha1,
            .effects = ForecastChannelEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x8010d1cc,
            .ax_manager = 0x800b618c,
            .append_effect = 0x800b6a6c,
            .clear_effect = 0x800b6c70,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x800bcaa4, 0x800bcb30, 0x800bcb5c, 0x800bcc6c, 0x800bcb6c,
                     0x800bcc18, 0x800bce48},
                },
        },
        {
            .dol_sha1 = kNewsChannelSha1,
            .effects = NewsChannelEffects,
            .axfx_build = axfx::Build::kDecember2006,
            .ax_register_callback = 0x80125d34,
            .ax_manager = 0x800cec58,
            .append_effect = 0x800cf538,
            .clear_effect = 0x800cf73c,
            .classes =
                {
                    {nw4r::EffectType::kReverbHi, 0x800d5570, 0x800d55fc, 0x800d5628, 0x800d5738, 0x800d5638,
                     0x800d56e4, 0x800d5914},
                },
        },
    };
    return kProfiles;
}

} // namespace

std::optional<EffectProfile> FindEffectProfile(std::span<const uint8_t> dol, const std::string& game_id)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(dol));
    for (const EffectProfile& profile : KnownEffectProfiles())
    {
        if (sha1 == profile.dol_sha1)
        {
            return profile;
        }
    }

    // Another release of a game with a profile.
    for (const EffectProfile& source : KnownEffectProfiles())
    {
        if (!SameGame(source.game_id, game_id))
        {
            continue;
        }

        std::optional<EffectProfile> derived = DeriveEffectProfile(source, dol);
        if (derived)
        {
            derived->dol_sha1 = sha1;
            derived->game_id = game_id;

            return derived;
        }
    }

    return std::nullopt;
}

std::optional<EffectProfile> DeriveEffectProfile(const EffectProfile& source, std::span<const uint8_t> dol_bytes)
{
    // The functions to find: the profile's, then its anchors.
    EffectProfile derived = source;
    std::vector<uint32_t*> functions = EffectFunctions(derived);
    for (uint32_t& anchor : derived.anchors)
    {
        functions.push_back(&anchor);
    }

    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(dol_bytes);
    if (source.signatures.size() != functions.size() || !dol)
    {
        return std::nullopt;
    }

    std::vector<uint32_t> addresses;
    for (const uint32_t* function : functions)
    {
        addresses.push_back(*function);
    }

    const std::optional<std::vector<uint32_t>> found = FindFunctions(*dol, addresses, source.signatures);
    if (!found)
    {
        return std::nullopt;
    }

    for (std::size_t i = 0; i < functions.size(); i++)
    {
        *functions[i] = (*found)[i];
    }

    return derived;
}

nw4r::ArchiveEffects FindEffects(const std::optional<EffectProfile>& profile, const std::string& archive_path,
                                 const ntdlib::rsar::SoundArchive& archive)
{
    return profile ? profile->effects(ArchiveFileName(archive_path), archive) : nw4r::ArchiveEffects{};
}

const std::vector<JAudioProfile>& KnownJAudioProfiles()
{
    // Addresses in the games' main.dol files.
    static const std::vector<JAudioProfile> kProfiles = {
        // The Legend of Zelda: Twilight Princess. The sizes and offsets are the game's: its machine start-up gives the
        // heaps and the ARAM manager, its audio set-up the audio heap, and its sound objects the analysis's offsets.
        // The game keeps its archive heap in MEM2's root heap. The audio set-up loads four wave groups itself.
        {
            .name = "The Legend of Zelda: Twilight Princess (USA, Rev 2)",
            .game_id = "RZDE01",
            .dol_sha1 = "5a99acb37b98e19682924502ee14a6d03d536c69",
            .main = 0x80008d04,
            .z2 =
                {
                    .frame_retraces = 2,
                    .max_heaps = 1,
                    .system_heap_size = 0x800000,
                    .aram_audio_size = 0xb00000,
                    .aram_graph_size = 0xffffffff,
                    .aram_priorities = 8 << 16 | 7 << 8 | 6,
                    .aram_buffer_size = 0x2000,
                    .command_heap_size = 0x1000,
                    .archive_heap_size = 0xf20c00,
                    .zelda_heap_margin = 0x10000,
                    .audio_heap_size = 0x169000,
                    .vi_init = 0,
                    .vi_wait_for_retrace = 0,
                    .get_current_thread = 0x803384c0,
                    .get_thread_priority = 0x80339754,
                    .create_root_heap = 0x802cd5bc,
                    .create_exp_heap = 0x802cd688,
                    .create_solid_heap = 0x802cf184,
                    .create_aram = 0x802d096c,
                    .set_aram_buffer = 0x802d296c,
                    .create_thread_switch = 0x802d00b8,
                    .create_command_heap = 0x800149f8,
                    .create_archive_heap = 0x80014a28,
                    .archive_heap_parent = 0x8052112c, // JKRHeap::sRootHeap2
                    .create_assert_heap = 0x80014930,
                    .create_zelda_heap = 0x800149c8,
                    .get_free_size = 0x802cd050,
                    .become_current_heap = 0x802cce18,
                    .system_heap = 0x80521120,
                    .alloc = 0x802cd4fc,
                    .thread_ctor = 0x802cfd98,
                    .create_disc_thread = 0x8001ad3c,
                    .audio_heap = 0x80520784,
                    .audio_setup = 0x8000973c,
                    .init_flag = 0x80520780,
                    .audio_frame = 0x802af680,
                    .load_se_wave = 0x8029cb7c,
                    .load_bgm_wave = 0x8029cbe8,
                    .wave_status = 0x8029caa8,
                    .audio_manager = 0x80431cac,
                    .start_sound = 0x802af854,
                    .erase_bgm_wave = 0x8029ca40,
                    .erase_se_wave = 0x8029c9d8,
                    .first_scene_wave_count = 2,
                    .first_scene_waves = {(1u << 16) | 0, (0u << 16) | 88},
                    .room_wave_count = 1,
                    .room_waves = {(0u << 16) | 0},
                },
            .sleep = 0x8025b100,      // JFWDisplay::threadSleep
            .reset_data = 0x805208d0, // mDoRst::mResetData
            .reset_flag = 0x12,
            .ios_version = 9,
            .watch =
                {
                    .parse_command = 0x8027b080,
                    .jump_handler = 0x80279bf4,
                    // JASSeqParser's handler for 0xC8 (0x80279c0c) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x80279c38,
                    .sequence_vtable = 0x8040d934,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x8040d888, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8027d94c, 0x8027ddf4},
                    .wave_bank_table = 0x80521038,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0, 0 << 16 | 88},
        },
        {
            // A later build of Twilight Princess's engine. The game's main.dol has no JFWDisplay::threadSleep. The
            // driver's sleep stands in for that function, with `alarm`. The game's scene manager has no function that
            // loads or erases one wave group. The driver's functions stand in for those, with `wave_arcs`. The game
            // runs at 30 frames a second: each scene sets the display's tick rate to a 30th of a second (0x8010231c).
            // The audio set-up loads SE group 0 (4.8 MiB), BGM group 0 and the Home Button menu's group. When a
            // piece's BGM groups don't fit in the wave heap beside SE group 0, the driver erases SE group 0.
            .name = "Link's Crossbow Training (USA, Rev 1)",
            .game_id = "RZPE01",
            .dol_sha1 = "2d3fe2c34ce4d2e470573846198ad93f07e4ac1e",
            .main = 0x800082d0,
            .z2 =
                {
                    .frame_retraces = 2,
                    .max_heaps = 1,
                    .system_heap_size = 0x800000,
                    .aram_audio_size = 0xb00000,
                    .aram_graph_size = 0xffffffff,
                    .aram_priorities = 8 << 16 | 7 << 8 | 6,
                    .aram_buffer_size = 0x2000,
                    .command_heap_size = 0x1000,
                    .archive_heap_size = 0x1620c00,
                    .zelda_heap_margin = 0x10000,
                    .audio_heap_size = 0x260800,
                    .vi_init = 0,
                    .vi_wait_for_retrace = 0,
                    .get_current_thread = 0x801da1a8,
                    .get_thread_priority = 0x801db43c,
                    .create_root_heap = 0x8017eda8,
                    .create_exp_heap = 0x8017ef20,
                    .create_solid_heap = 0x80180fec,
                    .create_aram = 0x801826c8,
                    .set_aram_buffer = 0x801844e8,
                    .create_thread_switch = 0x80181e38,
                    .create_command_heap = 0x800156ac,
                    .create_archive_heap = 0x800156dc,
                    .archive_heap_parent = 0x803e0eac, // JKRHeap::sRootHeap2
                    .create_assert_heap = 0x800155e4,
                    .create_zelda_heap = 0x8001567c,
                    .get_free_size = 0x8017d730,
                    .become_current_heap = 0x8017c834,
                    .system_heap = 0x803e0ea0,
                    .alloc = 0x8017e2d4,
                    .thread_ctor = 0x80181c70,
                    .create_disc_thread = 0x8001c098,
                    .audio_heap = 0x803e0694,
                    .audio_setup = 0x8000909c,
                    .init_flag = 0x803e0690,
                    .audio_frame = 0x8016f070,
                    .load_se_wave = 0,
                    .load_bgm_wave = 0,
                    .wave_status = 0,
                    .audio_manager = 0x802ed170,
                    .start_sound = 0x8016f288,
                    .erase_bgm_wave = 0,
                    .erase_se_wave = 0,
                    .first_scene_wave_count = 0,
                    .first_scene_waves = {},
                    .room_wave_count = 1,
                    .room_waves = {(0u << 16) | 0},
                },
            .sleep = 0,
            .reset_data = 0x803e07a8,
            .reset_flag = 0x12,
            .alarm =
                {
                    .set_alarm = 0x801d19ec,
                    .disable_interrupts = 0x801d68fc,
                    .restore_interrupts = 0x801d6924,
                    .suspend_thread = 0x801dafdc,
                    .resume_thread = 0x801dad44,
                },
            .wave_arcs =
                {
                    .section_heap = 0x803e0e20,
                    .bank_table = 0xf4,
                    .load = 0x801407bc,
                    .load_tail = 0x80140894,
                    .erase = 0x8014096c,
                },
            .ios_version = 21,
            .watch =
                {
                    .parse_command = 0x8013c0ec,
                    .jump_handler = 0x8013aca0,
                    // JASSeqParser's handler for 0xC8 takes the jump here, with the track in r30 and the arguments in
                    // r31.
                    .conditional_jump = 0x8013ad98,
                    .sequence_vtable = 0x802c61b8,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x802c6028,
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8013ead0, 0x8013f0f4},
                    .wave_bank_table = 0x803e0e20,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0},
        },
        // As in the GameCube's Donkey Kong Jungle Beat, the sound library is JAudio2 of Twilight Princess's generation,
        // under the game's audio system. The start-up archive is compressed with Yaz0. The game loads it, decoded,
        // into its game heap. It mounts the sequence archive and the chord archive in its audio heap, and loads the
        // chord database there. Its 65 wave banks hold one group each. The sound library loads bank 2 with the
        // start-up archive, and the game loads the others per stage. The game's BGM player starts the music. It sets
        // each track of a BGM by tables in the game's code, and the driver starts music the same way. A chord effect
        // takes its keys from the chords of the music that plays. Its rip plays it over a stage's music with the
        // music's tracks off. The parameters follow driver/jungle_beat.c's Params from frame_ticks.
        {
            .name = "New Play Control! Donkey Kong Jungle Beat (USA)",
            .game_id = "R49E01",
            .dol_sha1 = "cdd123faa94ac8cff7c6c96c1de00bb42e5e3eeb",
            .main = 0x80226e00,
            .ios_version = 53,
            .watch =
                {
                    .parse_command = 0x802d0e60,
                    .jump_handler = 0x802cf880,
                    // JASSeqParser's handler for 0xC8 (0x802cf8a0) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x802cf8cc,
                    .sequence_vtable = 0x804658c8,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x80465708, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x802d3c00, 0x802d4190},
                    .wave_bank_table = 0x805035a8,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {2 << 16 | 0},
            .driver = JAudioDriver::kJungleBeat,
            .params =
                {
                    kFrameTicks,          // frame_ticks: the game runs at 60 frames a second
                    0xd80000,             // aram_audio_size
                    0xffffffff,           // aram_graph_size: the rest of the game's "ARAM"
                    8 << 16 | 7 << 8 | 6, // aram_priorities
                    0x18,                 // heaps_size
                    0x803299cc,           // DVDInit
                    0x80228700,           // create_root_heap: JKRExpHeap::createRoot(1, true)
                    0x8027df20,           // operator new
                    0x80228710,           // heaps_ctor: the game heap, the audio heap and the "ARAM" in MEM2
                    0x80502f18,           // heaps
                    0x8027d970,           // JKRHeap::becomeCurrentHeap
                    0x80503134,           // JKRHeap::sCurrentHeap
                    0x80281e40,           // JKRAram::create
                    0x80315974,           // OSSleepTicks
                    0x8027df90,           // operator delete
                    0x80328efc,           // DVDConvertPathToEntrynum
                    0x802885c0,           // JKRDvdRipper::loadToMainRAM
                    0x802843c0,           // JKRArchive::mount
                    0x8045b858,           // "/AudioRes/Donkey.szs"
                    0x8045b8d8,           // "/AudioRes/Seqs/JaiSeq.arc"
                    0x8045b958,           // "/AudioRes/Info/JaiChord.arc"
                    0x8045b9d8,           // "/AudioRes/Info/ChordSeDataBase.csd"
                    0x80265550,           // create_audio_system
                    0x80266cf0,           // the audio system's frame process
                    0x80502b88,           // speaker_manager: the Wii Remote speaker's manager
                    0x1060,               // speaker_offset: the audio system's pointer to it
                    0x805035a8,           // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0xfc,                 // wave_banks_offset
                    0x802e1190,           // JAUSection::loadWaveArc with a mask of archives
                    0x4c,                 // wave_arc_state_offset
                    1,                    // music_section: music's sound IDs are 0x01xxxxxx
                    0xd18,                // music_player_offset: the audio system's BGM player
                    0x803ab6b0,           // music_tables: each BGM's tables of track states
                    0x803acc44,           // music_states: the second table's states
                    0x80279b20,           // set_music_state: the BGM player's function for a state of the second table
                    0xd10,                // chord_offset: the audio system's chord effects
                    164,                  // chord_records: the database's count says 218
                    0x8027c050,           // start_chord_effect
                    0xd14,                // stage_offset: the audio system's stage object
                    0x80277590,           // start_stage_music
                    0x80279270,           // set_track_states: the BGM player's function for a code for each track
                    0x818,                // rhythm_offset: the audio system's rhythm
                    0x80270260,           // rhythm_frame: the rhythm's frame process
                    0x50042,              // swapped_effect: JA_SE_IST_AUTO_COMP_ALTO_SAX
                    0x50063,              // swaps: JA_SE_IST_AUTO_COMP_E_PIANO_T
                    0x2c,                 // at stage 0x2c (TropicalCoast)
                    0x50062,              // JA_SE_IST_AUTO_COMP_E_PIANO
                    0x35,                 // at stage 0x35 (SwimRace)
                    0x70,                 // combo_effect: JA_SE_EAT_BIND_COMBO
                    0x802678b0,           // start_with_count: the audio system's start of a sound with a count
                    0,                    // music: written by the driver
                },
            .background_sound = 50, // the driver's `music`
            .track_parent_offset = 0x12c,
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/ChordSeDataBase.csd"},
        },
        // New Play Control! Pikmin (USA). The game's sound library is the early JAudio1 of the GameCube release. The
        // game calls the library through C functions (Jac, Jaf, Jaq and Jam). The game's code holds the sequence index.
        // The game plays a piece of music by the piece's number in the index. The piece plays in one of two slots, with
        // group 0 of the wave system of the same number. The disc has no map. Each function in main.dol was matched
        // with the function at the same place in the GameCube release's sound library. The sizes come from the game's
        // start-up, System::Initialise (0x800dc878). The parameters follow driver/pikmin.c's Params from frame_ticks.
        {
            .name = "New Play Control! Pikmin (USA)",
            .game_id = "R9IE01",
            .dol_sha1 = "551306334183a633d0fde434a930b98809433b70",
            .library = JAudioLibrary::kJac,
            .sound_archive = "pikmin1/US/dataDir/SndData/Banks/pikibank.bx",
            .sequence_archive = "pikmin1/US/dataDir/SndData/Seqs/pikiseq.arc",
            .sequence_index = 0x80316bc0,
            // The music of the cutscenes: demobgm, in parts. Jac_InitBgm (0x8001bf60) starts demobgm when the sound
            // library starts. A cutscene's entry in the table at 0x803165a4 gives the part to play. __Prepare_BGM
            // (0x8001e8a0) loads the part's group of wave system 13. Part 2 plays bank 14. Its waves are in the
            // diary's wave system, 14. Parts 3 and 4 play part 5's tracks without a wave group. No cutscene plays them.
            // The table and the parts hold the GameCube release's data.
            .parted_piece = 13,
            .parts = {{1, 13, 7}, {2, 14, 0}, {5, 13, 3}, {6, 13, 4}, {7, 13, 5}, {8, 13, 6}, {9, 13, 1}, {10, 13, 8}},
            .main = 0x80006de0,
            .ios_version = 33,
            .watch =
                {
                    .parse_command = 0x80016068, // the head of the loop over a track's commands in Jam_SeqmainNote
                    .jump_handler = 0x800147b0,  // Cmd_CallF, for the jump (0xC8) and the call (0xC4), at the jump
                    .command_track_register = 20,
                    .jump_track_register = 3,
                    .jump_target_register = 30,
                    .depth_offset = 8,
                    .relative_positions = true,
                    .wait_offset = 0x8c,
                    .note_wait_offset = 0,
                    .jaudio1 = true,
                    // The driver's word `sound` points at a word that holds the sound's root track.
                    .parameters_offset = 0,
                    .parameters_track_offset = 0,
                    .parent_offset = 0x40,
                    .children_offset = 0x44,
                },
            // Jac_Start loads group 0 of wave system 0. That system holds the effects.
            .static_waves = {0 << 16 | 0},
            .driver = JAudioDriver::kPikmin,
            .params =
                {
                    kFrameTicks, // frame_ticks: 60 polls a second. The sound library's threads play the music.
                    0x80000,     // audio_heap_size: as System::Initialise gives Jac_Start
                    0x20,        // audio_heap_align
                    0x80000,     // disc_buffer_size: the game's system object gives 0x2000 units of 64 bytes
                    0x90801600,  // aram_arena_lo: MEM2's arena from here, above the ARAM emulation's 8 MB
                    0x980000,    // aram_size: as System::Initialise gives Jac_Start
                    0x20,        // aram_align
                    0x802ff267,  // sound_path: "/pikmin1/US/dataDir/SndData/"
                    0,           // music_slot
                    4,           // music_handle: the slot's root sequence (Jac_PlayBgm plays slot n with handle n + 4)
                    0x30000,     // part_track: the name that demobgm's root track takes. Jac_StartDemo writes to it.
                    0x80058224,  // sleep_ticks: OSSleepTicks
                    0x8004e7e8,  // alloc_mem1: OSAllocFromMEM1ArenaLo
                    0x8004e7d8,  // set_mem2_arena_lo: OSSetMEM2ArenaLo
                    0x8004e81c,  // alloc_mem2: OSAllocFromMEM2ArenaLo
                    0x8001a4e0,  // set_aram_base: the sound library's setter. System::Initialise calls it.
                    0x8001a360,  // start: Jac_Start
                    0x8001a5e0,  // add_disc_buffer: Jac_AddDVDBuffer
                    0x8049e1a4,  // boot_ok: the flag that __BootSoundOK (0x8001a300) sets
                    0x8001c160,  // ready_bgm: Jac_ReadyBgm
                    0x80010580,  // collect_aram: Collect_AramMotherHeap
                    0x8000f3a0,  // set_wave_scene: WaveScene_Set
                    0x80009c60,  // check_pass: DVDT_CheckPass
                    0x8001c200,  // play_bgm: Jac_PlayBgm
                    0x8001f220,  // handle_to_sequence: Jaf_HandleToSeq
                    0x800133c0,  // get_track_handle: Jam_GetTrackHandle
                    0x80013040,  // write_port: Jam_WritePortAppDirect
                },
        },
        // New Play Control! Pikmin 2 (USA), the GameCube's Pikmin 2 built for the Wii. Its sound library is a later
        // JAudio1, under the game's sound layer (PSSystem, PSGame and PSM). The start-up archive is an AAF. The game
        // starts its music by file name, through the objects of its sound scenes (driver/pikmin2.c). The sound table
        // lists its effects, and PSound.asn names them. Wave system 0 has 49 groups, one .aw each. wScene_0.aw holds
        // most effects and stays loaded, and a scene loads at most two others. Wave system 1 holds the Home Button
        // menu's sounds. The global scene loads it too. The game keeps its files in the disc's folder pikmin2. The
        // parameters follow driver/pikmin2.c's Params from frame_ticks to menu_waves. The table of pieces follows them.
        {
            .name = "New Play Control! Pikmin 2 (USA)",
            .game_id = "R92E01",
            .dol_sha1 = "d69f9083b7c47665a4f701099ec029176f2e3aeb",
            .library = JAudioLibrary::kAaf,
            .sound_archive = "pikmin2/AudioRes/PSound.aaf",
            .sequence_archive = "pikmin2/AudioRes/Seqs/Seq.arc",
            .sound_names = "pikmin2/AudioRes/PSound.asn",
            .main = 0x80497f08,
            .ios_version = 33,
            .watch =
                {
                    .parse_command = 0x8005025c, // the head of the loop over a track's commands in the sequence parser
                    .jump_handler = 0x8004ec34,  // the jump in the sequence parser's handler for 0xC8
                    .command_track_register = 30,
                    .jump_track_register = 31,
                    .jump_target_register = 25,
                    .reader_offset = 0xc,
                    .depth_offset = 0x18,
                    .interrupt_offset = 0x50, // JASSeqCtrl's saved position, +0x44 in the sequence control
                    .idle_in_calls = true,    // the root tracks poll the game once a beat in a call that never returns
                    .wait_offset = 0x14,
                    .wave_handle_functions = {0x8004c140, 0x8004d7a4}, // the two wave banks' getWaveHandle
                    .wave_bank_table = 0x8066b9e4,                     // JASWaveBankMgr::sWaveBank
                    .jaudio1 = true,
                    .parameters_offset = 0x48,
                    .parameters_inline = true,
                    .parameters_track_offset = 0x2c4,
                    .effects_sequence = 0x8066bb68, // JAInter::SeMgr::seHandle
                    .effect_slot_offset = 0x14,     // JAISound's track number, after its JSULink and its vtable
                    .parent_offset = 0x2f8,
                    .children_offset = 0x2fc,
                },
            .static_waves = {0 << 16 | 0, 1 << 16 | 0},
            .driver = JAudioDriver::kPikmin2,
            .params = Pikmin2Parameters({
                2 * kFrameTicks,      // frame_ticks: the game's areas and caves run at 30 frames a second
                1,                    // max_heaps
                0xa0000,              // system_heap_size
                0xc00000,             // aram_audio_size
                0x700000,             // aram_graph_size
                8 << 16 | 7 << 8 | 6, // aram_priorities
                0x7c,                 // thread_size
                0x200000,             // heap_margin
                0xd4800,              // file_heap_size: the game's heap for 2D resources
                0x1c,                 // factory_size
                0xc00000,             // sound_memory
                0x38,                 // scene_info_size
                17,                   // scene_type: the challenge mode's menu
                0x24,                 // battle_directors_size
                0xb8,                 // directed_bgm_size
                0x350,                // auto_bgm_size
                0x80117308,           // DVDInit
                0x80116cc0,           // DVDChangeDir
                0x80558eba,           // game_folder: "pikmin2"
                0x800f0a14,           // JFWDisplay::threadSleep
                0x8013539c,           // OSGetCurrentThread
                0x801338e8,           // OSInitMutex
                0x80084768,           // create_mem1_root_heap
                0x800846e8,           // JKRExpHeap::createRoot: MEM2's root heap
                0x800847e8,           // JKRExpHeap::create
                0x80089320,           // JKRSolidHeap::create
                0x80087d64,           // JKRHeap::getFreeSize
                0x80087c08,           // JKRHeap::becomeSystemHeap
                0x80087c18,           // JKRHeap::becomeCurrentHeap
                0x80087c28,           // JKRHeap::destroy
                0x800894f4,           // JKRSolidHeap::adjustSize
                0x80088948,           // operator new
                0x80089d04,           // JKRThread::JKRThread
                0x8007c4f8,           // JKRAram::create
                0x80086374,           // JKRFileCache::mount
                0x805593f9,           // audio_folder: "/pikmin2/AudioRes"
                0x8008759c,           // JKRFileLoader::getGlbResource
                0x8055940b,           // start_up_archive: "PSound.aaf"
                0x803b43a8,           // factory_ctor
                0x805ccaec,           // factory_vtable
                0x804f67e8,           // make_se
                0x803b43dc,           // new_sound_system
                0x8066cea0,           // scene_manager
                0x8066ce70,           // system_interface
                0x803b89d0,           // main_loop
                0x803b5628,           // new_bgm_seq
                0x804df7c8,           // new_main_bgm
                0x804df8b0,           // new_directed_bgm
                0x804e1994,           // new_auto_bgm
                0x804dfa78,           // new_game_scene
                0x803c0838,           // append_seq
                0x803c0718,           // adapt_child_scene
                0x803b4b24,           // scene_info_ctor
                0x803b2120,           // directed_bgm_ctor
                0x803bb1b8,           // auto_bgm_ctor
                0x804f5f70,           // battle_directors_ctor
                0x803c2390,           // adapt_directors
                0x803b2b84,           // start_jump_bgm
                0x8066ce80,           // conductor_heap
                0x803b85f8,           // play_system_se
                0x8066ca98,           // play_data
                0x8066ce98,           // rappas
                0x803bce28,           // load_wave_scene: PSSystem::WaveScene::load
                1 << 16 | 0,          // menu_waves: the Home Button menu's wave group
            }),
            .files = {"pikmin2/AudioRes/Seqs/Seq.arc", "pikmin2/AudioRes/Conductor.arc", "pikmin2/AudioRes/Key.arc",
                      "pikmin2/user/Totaka/BgmList.txt", "pikmin2/user/Totaka/StreamList.txt",
                      "pikmin2/user/Totaka/trackMap_Cond_T.txt", "pikmin2/user/Totaka/trackMap_Seq_T.txt",
                      "pikmin2/user/Wakai/trackMap_Cond_W.txt", "pikmin2/user/Wakai/trackMap_Seq_W.txt"},
        },
        {
            // The game's start-up gives the sizes of ARAM and the heaps' object. The audio system loads wave bank 7
            // itself, and the scene manager loads banks 0 to 6, 8 and 11 before the first scene. The game loads the
            // others per stage and per scenario. Every bank of those holds one group.
            .name = "Super Mario Galaxy (USA)",
            .game_id = "RMGE01",
            .dol_sha1 = "9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd",
            .main = 0x80399510, // main's call of DVDInit, after main sets up the quantization registers
            .ios_version = 33,
            .watch =
                {
                    .parse_command = 0x8048dc98,
                    .jump_handler = 0x8048c7f4,
                    // JASSeqParser's handler for 0xC8 (0x8048c80c) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x8048c838,
                    .sequence_vtable = 0x805da424,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x805da368, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x80490564, 0x80490a8c},
                    .wave_bank_table = 0x806a2c28,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {7 << 16 | 0, 0 << 16 | 0, 1 << 16 | 0, 2 << 16 | 0, 3 << 16 | 0, 4 << 16 | 0, 5 << 16 | 0,
                             6 << 16 | 0, 8 << 16 | 0, 11 << 16 | 0},
            .driver = JAudioDriver::kGalaxy,
            // driver/galaxy.c's block after the wave list, word by word.
            .params =
                {
                    kFrameTicks, // the game runs its audio frame process at 60 Hz
                    0xe00000,    // ARAM for the sound library
                    0xffffffff,  // ARAM for graphics: the rest
                    8 << 16 | 7 << 8 | 3,
                    0x28,       // HeapMemoryWatcher's size
                    0x804bfd58, // DVDInit
                    0x8039ed34, // HeapMemoryWatcher::createRootHeap
                    0x80409af8, // operator new
                    0x8039eee4, // HeapMemoryWatcher's constructor
                    0x806a245c, // SingletonHolder<HeapMemoryWatcher>::sInstance
                    0x0,        // the stationed heap in MEM1
                    0x24,       // the audio system's heap
                    0x8040963c, // JKRHeap::becomeCurrentHeap
                    0x8040d1b8, // JKRAram::create
                    0x804ac2b4, // OSSleepTicks
                    0x80409b40, // operator delete
                    0x804bef5c, // DVDConvertPathToEntrynum
                    0x804134ac, // JKRDvdRipper::loadToMainRAM
                    0x8040f528, // JKRArchive::mount
                    0x80551608, // "/AudioRes/SMR.szs"
                    0x8055161a, // "/AudioRes/Seqs/JaiSeq.arc"
                    0x80551634, // "/AudioRes/Info/JaiChord.arc"
                    0x80551650, // "/AudioRes/Info/JaiMe.arc"
                    0x80551669, // "/AudioRes/Info/JaiRemixSeq.arc"
                    0x80026158, // AudNewAudSystem
                    0x80026c34, // AudSystem::frameWork
                    0x132c,     // AudSystem's SpkSystem
                    0x9f8,      // AudSystem's AudSceneMgr
                    0x800316fc, // AudSceneMgr::isLoadDoneSystemInit
                    0x8003170c, // AudSceneMgr::loadStaticResource
                    0x800317a0, // AudSceneMgr::isLoadDoneStaticResource
                    0x806a2c28, // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0x8049cfac, // JAUSection::loadWaveArc(bank, mask)
                    0x8049dadc, // JAUSectionHeap::isWaveLoaded
                    1,          // the section of music sound IDs
                    0x8002a72c, // AudWrap::startStageBgm
                    0x80026d54, // AudSystem::startSound
                    0x806a2de8, // THD_TIME_TO_DIMMING, VI's threshold
                    0x80027358, // AudSystem::initSceneVolume
                },
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/JaiMe.arc",
                      "AudioRes/Info/JaiRemixSeq.arc"},
        },
        {
            // The game loads its sound files and its audio data tables through its file loader. The audio system
            // loads wave bank 7 itself. The driver loads no stationed waves: each rip lists the wave groups that its
            // sound uses.
            .name = "Super Mario Galaxy 2 (USA)",
            .game_id = "SB4E01",
            .dol_sha1 = "7d79810889824edd01c4ff02624713060a69818c",
            .main = 0x804b6410, // main's call of DVDInit, after main sets up the quantization registers
            .ios_version = 56,
            .watch =
                {
                    .parse_command = 0x8058a040,
                    .jump_handler = 0x80588a60,
                    // JASSeqParser's handler for 0xC8 (0x80588a80) takes the jump here, with the track in r30 and the
                    // arguments in r31.
                    .conditional_jump = 0x80588aac,
                    .sequence_vtable = 0x8070bdb0,
                    .sequence_track_offset = 0xa8,
                    .effect_vtable = 0x8070bd08, // the JAISound (JAISe's) that the handle holds
                    .effect_track_offset = 0xb8,
                    .wait_offset = 0x40,
                    .note_wait_offset = 0x51,
                    .wave_handle_functions = {0x8058cb00, 0x8058d0a0},
                    .wave_bank_table = 0x807d6568,
                    .wave_bank_table_offset = 0xfc,
                    .wave_bank_count_offset = 0xf8,
                },
            .static_waves = {7 << 16 | 0},
            .driver = JAudioDriver::kGalaxy2,
            // driver/galaxy2.c's block after the wave list, word by word.
            .params =
                {
                    kFrameTicks, // the game runs its audio frame process at 60 Hz
                    0xdb0000,    // ARAM for the sound library
                    0xffffffff,  // ARAM for graphics: the rest
                    8 << 16 | 7 << 8 | 3,
                    0x30,       // HeapMemoryWatcher's size
                    0x20000,    // FileRipper's buffer
                    0x3c,       // GameSystem's size
                    0x44,       // GameSystemObjHolder's size
                    0x34,       // AudSystemWrapper's size
                    0x21,       // the game's number for US English
                    0x805d1e10, // DVDInit
                    0x804bcda0, // HeapMemoryWatcher::createRootHeap
                    0x80501850, // operator new
                    0x804bcf90, // HeapMemoryWatcher's constructor
                    0x807d0dac, // SingletonHolder<HeapMemoryWatcher>::sInstance
                    0x0,        // the stationed heap in MEM1
                    0x4,        // the stationed heap in MEM2
                    0x2c,       // the audio system's heap
                    0x805012e0, // JKRHeap::becomeCurrentHeap
                    0x804b1f00, // FileRipper::setup
                    0x80505280, // JKRAram::create
                    0x804b90b0, // GameSystemObjHolder::initDvd
                    0x807d0da4, // SingletonHolder<GameSystem>::sInstance
                    0x20,       // GameSystem's GameSystemObjHolder
                    0x38,       // the holder's language
                    0x20,       // the holder's AudSystemWrapper
                    0x804ae050, // AudSystemWrapper's constructor
                    0x804ae0f0, // AudSystemWrapper::requestResourceForInitialize
                    0x804ae1c0, // AudSystemWrapper::createAudioSystem
                    0x805bdcc0, // OSSleepTicks
                    0x80078b20, // AudSystem::frameWork
                    0x9a8,      // AudSystem's SpkSystem
                    0x804ae340, // AudSystemWrapper::isLoadDoneWaveDataAtSystemInit
                    0x804ae370, // the wrapper's step after that bank loads
                    0x80089480, // the reader of BgmParam.arc
                    0x80083ce0, // the reader of MultiBgmInfo.arc
                    0x8008a120, // the reader of StageBgmInfo.arc
                    0x807d6568, // JASGlobalInstance<JAUSectionHeap>::sInstance
                    0x80599de0, // JAUSection::loadWaveArc(bank, mask)
                    0x8059a8f0, // JAUSectionHeap::isWaveLoaded
                    1,          // the section of music sound IDs
                    0x8007c8d0, // AudWrap::startStageBgm
                    0x80078cc0, // AudSystem::startSound
                    0x807d67a0, // THD_TIME_TO_DIMMING, VI's threshold
                    0x800793c0, // AudSystem::initSceneVolume
                },
            // The archives that the audio wrapper requests, then the three tables that the driver has the audio
            // system read. ActionSound.arc in the same folder is read only for the particle effects.
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/JaiMe.arc",
                      "AudioRes/Info/JaiRemixSeq.arc", "AudioRes/Info/SoundIdToInstList.arc",
                      "AudioRes/SpkRes/SpkRes.arc", "AudioRes/Info/BgmParam.arc", "AudioRes/Info/MultiBgmInfo.arc",
                      "AudioRes/Info/StageBgmInfo.arc"},
        },
    };
    return kProfiles;
}

std::optional<JAudioProfile> FindJAudioProfile(const ntdlib::disc::GameFiles& game)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(game.dol));
    for (const JAudioProfile& profile : KnownJAudioProfiles())
    {
        if (game.GameId() == profile.game_id && sha1 == profile.dol_sha1)
        {
            return profile;
        }
    }

    return std::nullopt;
}

JAudioLibrary SoundLibraryOf(const JAudioProfile* profile)
{
    return profile ? profile->library : JAudioLibrary::kJAudio2;
}

} // namespace wsf::rip
