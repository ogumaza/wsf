// SPDX-License-Identifier: MIT

// WSF ripper (see rip.h).

#include "rip/rip.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/disc/containers.h"
#include "ntdlib/dol.h"
#include "ntdlib/jaudio/aaf.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"
#include "xsflib/sha1.h"
#include "xsflib/text.h"

#include "ax/pb.h"
#include "ax/system.h"
#include "axfx/axfx.h"
#include "nw4r/effect_setup.h"
#include "nw4r/engine.h"
#include "nw4r/rsar_options.h"
#include "nw4r/sequence.h"
#include "rip/driver_blob.h"
#include "rip/galaxy2_driver_blob.h"
#include "rip/galaxy_driver_blob.h"
#include "rip/jaudio_driver_blob.h"
#include "rip/jungle_beat_driver_blob.h"
#include "rip/pikmin2_driver_blob.h"
#include "rip/pikmin_driver_blob.h"
#include "wsf/format.h"

namespace wsf::rip
{
namespace
{

// The driver's parameter block (driver/driver.c), from its start: the fields that a .miniwsf patches, then the rest.
constexpr uint32_t kParamsSoundOffset = DriverBlock::kSoundOffset; // sound, group, heap size
constexpr uint32_t kParamsFrameTicksOffset = 0x18;
constexpr uint32_t kParamsInfoSizeOffset = 0x1c;
constexpr uint32_t kParamsPrioritiesOffset = 0x20;
constexpr uint32_t kParamsPathOffset = 0x28;
constexpr uint32_t kParamsPathSize = 64;
constexpr uint32_t kParamsProfileOffset = 0x68;
constexpr uint32_t kParamsBankGroupOffset = 0xb4; // the group with the sound's bank, when it isn't the sound's group
constexpr uint32_t kParamsInitFlagOffset = 0xb8;  // the sound system's flag, for a 2008 build's Setup
// The effects (see EffectFunctions): the functions and the offset of a sound's sends, the number of effects, what a
// .miniwsf patches for its sound (whether the variables pick the presets, the send's bus and the send), the effects and
// their parameter words. The block ends with the build of AXFX and the effects' classes for the player. A .miniwsf
// whose sound gets other effects than the library's patches everything from the number of effects to the end.
constexpr uint32_t kParamsEffectFunctionsOffset = 0xbc;
constexpr uint32_t kParamsEffectCountOffset = 0xdc;
constexpr uint32_t kParamsSoundEffectsOffset = 0xe0;
constexpr uint32_t kParamsEffectsOffset = 0xec;
constexpr uint32_t kParamsEffectWordsOffset = 0x16c;
constexpr uint32_t kDriverEffects = 4;
constexpr uint32_t kDriverEffectWords = 64;
constexpr uint32_t kParamsEffectsEnd = DriverBlock::kNativeOffset + DriverBlock::kMaxEffects * DriverBlock::kNativeSize;
static_assert(kParamsEffectCountOffset == DriverBlock::kEffectCountOffset &&
              kParamsEffectsOffset == DriverBlock::kEffectsOffset && kDriverEffects == DriverBlock::kMaxEffects &&
              kParamsEffectWordsOffset + kDriverEffectWords * 4 == DriverBlock::kAxfxBuildOffset);
constexpr uint32_t kNoBus = 0xffffffff;

// The driver's settings: SoundArchivePlayer::Update 60 times a second, nw4r::snd's default thread priorities, and room
// for each object that the driver creates. The time base runs at a quarter of the 243 MHz bus clock. The JAudio2 driver
// takes the same frame time for a video frame.
constexpr uint32_t kFrameTicks = 243'000'000 / 4 / 60;
constexpr uint32_t kSoundThreadPriority = 4;
constexpr uint32_t kDvdThreadPriority = 3;
constexpr uint32_t kObjectSize = 0x2000;

// Room in the sound heap beyond a group's data: the heap's block headers and alignment.
constexpr uint32_t kHeapSlack = 0x10000;

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
            "Wii Sports (USA, Rev 1)",
            "RSPE01",
            "8bb422971b88b5551a37de98db69557df7b46637",
            0x80183ee4, // main
            0x80103680,
            0x80104450,
            0x800edc08,
            0x800f5550,
            0x80047550,
            0x800388e4,
            0x800389b0,
            0x80038cd8,
            0x80043a7c,
            0x80044060,
            0x80044190,
            0x80043e64,
            0x80045bac,
            0x80045c64,
            0x80045578,
            0x80047400,
            0x8004460c,
            0x0c,
            21,
            0,
            {{
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
            "Wii Play (USA, Rev 1)",
            "RHAE01",
            "0da5e7e51135219f580ad011d1b635bc83569bb9",
            0x80183b1c, // main
            0x80103068,
            0x80103e38,
            0x800ed890,
            0x800f51d8,
            0x80047674,
            0x8003887c,
            0x80038948,
            0x80038c70,
            0x80043ba0,
            0x80044184,
            0x800442b4,
            0x80043f88,
            0x80045cd0,
            0x80045d88,
            0x8004569c,
            0x80047524,
            0x80044730,
            0x0c,
            21,
            0,
            {{
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
            "WarioWare: Smooth Moves (USA)",
            "RODE01",
            "ab32545dc32724f1e4eeeecc05189af722b8addd",
            0x80007cb4, // main
            0x8002dfa0,
            0x8002e3f0,
            0x80009f10,
            0x8001266c,
            0x800ed7e4,
            0x800e127c,
            0x800e1348,
            0x800e1670,
            0x800e9abc,
            0x800e9ff8,
            0x800ea128,
            0x800e9f28,
            0x800ec3dc,
            0x800ec494,
            0x800eb174,
            0x800ed694,
            0x800ea554,
            0x0c,
            9,
            0,
            {{
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
            "Mario Kart Wii (USA)",
            "RMCE01",
            "1b367209919fcc225c74a155dc46f29c5d64b9a2",
            0x8000b610, // main
            0x80124010,
            0x80124de0,
            0x801a1064,
            0x801aac08,
            0x800a40b0,
            0x80090f20,
            0x80090ff0,
            0x800912f0,
            0x800a0460,
            0x800a0ab0,
            0x800a0bf0,
            0x800a0870,
            0x800a2e30,
            0x800a2ef0,
            0x800a2810,
            0x800a3de0,
            0x800a10e0,
            0x0c,
            36,
            0x80381e88, // SoundSystem's initialized flag
            {{
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
            "Wii Music (USA)",
            "R64E01",
            "a9ab9a8a8f14aa58d9bf4e05d72d29115ec589ab",
            0x804dd25c, // main
            0x800e5f54,
            0x800e6d24,
            0x8015af88,
            0x80162cb0,
            0x800c5b2c,
            0x800b542c,
            0x800b5504,
            0x800b5804,
            0x800c2270,
            0x800c2890,
            0x800c29c8,
            0x800c2660,
            0x800c4b74,
            0x800c4c2c,
            0x800c45d0,
            0x800c5a0c,
            0x800c2ea4,
            0x0c,
            35,
            0x80698230, // SoundSystem's initialized flag
            {{
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
            "Animal Crossing: City Folk (USA)",
            "RUUE01",
            "c87967b9ac0a943ba04ac16d875f5f79b4c46516",
            0x800cddec, // main
            0x80373b54,
            0x80374924,
            0x8037be34,
            0x80385520,
            0x8026e148,
            0x802618b8,
            0x80261990,
            0x80261c44,
            0x8026abb0,
            0x8026b0a4,
            0x8026b1dc,
            0x8026afa0,
            0x8026d310,
            0x8026d3c8,
            0x8026cddc,
            0x8026e040,
            0x8026b6b8,
            0x0c,
            38,
            0x8074ef38, // SoundSystem's initialized flag
            {{
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

ntdlib::disc::GameOptions DiscOptions()
{
    ntdlib::disc::GameOptions options{ntdlib::disc::Console::kWii, "WSF"};
    options.image.open_container = ntdlib::disc::OpenContainer;
    options.titles = true;
    options.cpk_sound_archives = true;

    return options;
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

    // Another release or a revision of a game with a profile. A game's releases share the first three characters of
    // their game IDs.
    for (const GameProfile& source : KnownProfiles())
    {
        if (source.game_id.compare(0, 3, game.GameId(), 0, 3) != 0 || source.init_flag != 0)
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

    // The places whose code matches each function's signature.
    std::array<std::vector<uint32_t>, kProfileFunctions> places;
    for (const ntdlib::Dol::Section& section : dol->Sections())
    {
        for (std::size_t at = 0; at + 4 <= section.data.size(); at += 4)
        {
            const uint32_t first = MaskedWord(xsflib::LoadBe32(&section.data[at]));
            for (std::size_t i = 0; i < kProfileFunctions; i++)
            {
                const FunctionSignature& signature = source.signatures[i];
                if (first == signature.first && at + signature.size <= section.data.size() &&
                    MaskedHash(section.data.subspan(at, signature.size)) == signature.hash)
                {
                    places[i].push_back(section.address + static_cast<uint32_t>(at));
                }
            }
        }
    }

    // How far each function that matched once moved.
    std::array<std::optional<int64_t>, kProfileFunctions> moved;
    for (std::size_t i = 0; i < kProfileFunctions; i++)
    {
        if (places[i].empty())
        {
            return std::nullopt;
        }

        if (places[i].size() == 1)
        {
            moved[i] = int64_t{places[i][0]} - source.*kProfileFields[i];
        }
    }

    GameProfile derived = source;
    for (std::size_t i = 0; i < kProfileFunctions; i++)
    {
        // A function that matched in several places moved as far as the nearest function that matched once.
        const uint32_t address = source.*kProfileFields[i];
        const auto distance = [&source, address](std::size_t k)
        {
            return std::abs(int64_t{source.*kProfileFields[k]} - address);
        };

        std::optional<std::size_t> nearest;
        for (std::size_t j = 0; j < kProfileFunctions; j++)
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

        derived.*kProfileFields[i] = *place;
    }

    return derived;
}

std::optional<SoundData> FindSoundData(const ntdlib::rsar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size())
    {
        return std::nullopt;
    }

    const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[index];
    if (sound.bank >= archive.Banks().size())
    {
        return std::nullopt;
    }

    // The groups that SoundArchivePlayer::LoadGroup accepts: all but the last in the table. The last group holds the
    // files that no other group does. SoundArchiveFileReader::GetGroupCount (main.dol 0x80042dd0) counts one fewer than
    // the table.
    const std::size_t count = archive.Groups().empty() ? 0 : archive.Groups().size() - 1;

    // The first group from `first` that holds the file, or `count` for none.
    const auto group_with = [&archive, count](uint32_t file_id, std::size_t first)
    {
        std::size_t g = first;
        for (; g < count; g++)
        {
            const ntdlib::rsar::SoundArchive::Group& group = archive.Groups()[g];
            const auto holds = [file_id](const auto& item)
            {
                return item.file_id == file_id;
            };
            if (group.internal && std::any_of(group.items.begin(), group.items.end(), holds))
            {
                break;
            }
        }

        return g;
    };

    const auto data_size = [&archive](std::size_t g)
    {
        return archive.Groups()[g].size + archive.Groups()[g].wave_data_size;
    };

    // A group with both the sequence and its bank, or else the first group with each.
    const uint32_t bank_file = archive.Banks()[sound.bank].file_id;
    for (std::size_t g = group_with(sound.file_id, 0); g < count; g = group_with(sound.file_id, g + 1))
    {
        if (group_with(bank_file, g) == g)
        {
            return SoundData{static_cast<uint32_t>(g), std::nullopt, data_size(g) + kHeapSlack};
        }
    }

    const std::size_t sequence_group = group_with(sound.file_id, 0);
    const std::size_t bank_group = group_with(bank_file, 0);
    if (sequence_group == count || bank_group == count)
    {
        return std::nullopt;
    }

    return SoundData{static_cast<uint32_t>(sequence_group), static_cast<uint32_t>(bank_group),
                     data_size(sequence_group) + data_size(bank_group) + kHeapSlack};
}

namespace
{

// The main.dol files with effect profiles (see FindEffects).
constexpr char kWiiSportsSha1[] = "8bb422971b88b5551a37de98db69557df7b46637";
constexpr char kWiiPlaySha1[] = "0da5e7e51135219f580ad011d1b635bc83569bb9";
constexpr char kSmoothMovesSha1[] = "ab32545dc32724f1e4eeeecc05189af722b8addd";
constexpr char kCityFolkSha1[] = "c87967b9ac0a943ba04ac16d875f5f79b4c46516";
constexpr char kMarioKartSha1[] = "1b367209919fcc225c74a155dc46f29c5d64b9a2";
constexpr char kWiiMusicSha1[] = "a9ab9a8a8f14aa58d9bf4e05d72d29115ec589ab";

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

// FxReverbHi of 2008 on bus A: preDelayTime, fusedTime, coloration, damping, crossTalk, outGain, then the modes and
// gains.
nw4r::EffectSetup ReverbHi2008(float pre_delay, float time, float coloration, float damping, float crosstalk, float out,
                               const Reverb2008& more)
{
    nw4r::EffectParams params = Words({pre_delay, time, coloration, damping, crosstalk, out, 0.0f,
                                       more.pre_delay_time_max, 0.0f, more.early_gain, more.fused_gain});
    params[6] = more.early_mode;
    params[8] = more.fused_mode;

    return Fixed(0, nw4r::EffectType::kReverbHi2008, params);
}

// FxReverbStd of 2008 on bus A: preDelayTime, fusedTime, coloration, damping, outGain, earlyMode, preDelayTimeMax,
// fusedMode, earlyGain and fusedGain.
nw4r::EffectSetup ReverbStd2008(float pre_delay, float time, float coloration, float damping, float out,
                                const Reverb2008& more)
{
    nw4r::EffectParams params = Words({pre_delay, time, coloration, damping, out, 0.0f, more.pre_delay_time_max, 0.0f,
                                       more.early_gain, more.fused_gain});
    params[5] = more.early_mode;
    params[7] = more.fused_mode;

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

// The functions of a game with an effect profile that game mode's driver calls for the effects (driver/driver.c).
struct EffectClass
{
    nw4r::EffectType type;
    uint32_t ctor, mem_size, assign, set_param; // FxXxx::FxXxx, GetRequiredMemSize, AssignWorkBuffer and SetParam
    uint32_t start_up, shutdown, update_buffer; // the virtual StartUp, Shutdown and UpdateBuffer (for the player)
};

// The other functions that the driver calls: AXRegisterCallback, and AxManager's GetInstance, AppendEffect and
// ClearEffect. AXRegisterCallback returns the callback that it replaces. AxManager registers one. The driver calls that
// one first. For presets, the driver also calls SeqSoundHandle's constructor and destructor and SeqSound::ReadVariable.
// For a send on the sound, it writes the send at an offset in BasicSound.
struct EffectFunctions
{
    const char* dol_sha1;
    axfx::Build axfx_build; // the build of AXFX that the game links, for the player's models of the effects
    uint32_t ax_register_callback, ax_manager, append_effect, clear_effect;
    uint32_t seq_handle_ctor, seq_handle_dtor, read_variable;
    uint32_t sound_sends;
    std::vector<EffectClass> classes;
};

const std::vector<EffectFunctions>& KnownEffectFunctions()
{
    static const std::vector<EffectFunctions> kFunctions = {
        {
            kWiiSportsSha1,
            axfx::Build::kDecember2006,
            0x80106260,
            0x800312e8,
            0x80031d5c,
            0x80031f60,
            0,
            0,
            0,
            0,
            {
                {nw4r::EffectType::kReverbHi, 0x8003a668, 0x8003a6f4, 0x8003a720, 0x8003a84c, 0x8003a730, 0x8003a7ec,
                 0x8003aa38},
                {nw4r::EffectType::kChorus, 0x80039d88, 0x80039dfc, 0x80039e28, 0x80039f54, 0x80039e38, 0x80039ef4,
                 0x8003a0c0},
            },
        },
        {
            kWiiPlaySha1,
            axfx::Build::kDecember2006,
            0x80105c48,
            0x800312e0,
            0x80031d54,
            0x80031f58,
            0,
            0,
            0,
            0,
            {
                {nw4r::EffectType::kReverbHi, 0x8003a600, 0x8003a68c, 0x8003a6b8, 0x8003a7e4, 0x8003a6c8, 0x8003a784,
                 0x8003a9d0},
                {nw4r::EffectType::kChorus, 0x80039d20, 0x80039d94, 0x80039dc0, 0x80039eec, 0x80039dd0, 0x80039e8c,
                 0x8003a058},
                {nw4r::EffectType::kDelay, 0x8003a118, 0x8003a28c, 0x8003a2b8, 0x8003a3e4, 0x8003a2c8, 0x8003a384,
                 0x8003a540},
            },
        },
        {
            kSmoothMovesSha1,
            axfx::Build::kOctober2006,
            0x8002ff80,
            0x800d97e8,
            0x800da878,
            0x800daa7c,
            0x800e6d68,
            0x800feef0,
            0x800e6cbc,
            0,
            {
                {nw4r::EffectType::kReverbStd, 0x800e31d8, 0x800e325c, 0x800e3288, 0x800e33ac, 0x800e3298, 0x800e334c,
                 0x800e3538},
                {nw4r::EffectType::kChorus, 0x800e2514, 0x800e2588, 0x800e25b4, 0x800e26d8, 0x800e25c4, 0x800e2678,
                 0x800e2820},
                {nw4r::EffectType::kDelay, 0x800e28e0, 0x800e2a34, 0x800e2a60, 0x800e2b84, 0x800e2a70, 0x800e2b24,
                 0x800e2cbc},
            },
        },
        {
            kCityFolkSha1,
            axfx::Build::kSeptember2008,
            0x80376748,
            0x8025914c,
            0x80259b98,
            0x80259d9c,
            0,
            0,
            0,
            0xd0,
            {
                {nw4r::EffectType::kReverbHi2008, 0x80262d10, 0x80262de0, 0x80262e34, 0x80262fe8, 0x80262e44,
                 0x80262f6c, 0x8026333c},
            },
        },
        {
            kMarioKartSha1,
            axfx::Build::kSeptember2008,
            0x80126c04,
            0x80088750,
            0x80089170,
            0x80089360,
            0,
            0,
            0,
            0,
            {
                {nw4r::EffectType::kReverbStd2008, 0x80095080, 0x80095150, 0x800951b0, 0x80095380, 0x800951d0,
                 0x80095300, 0x800956a0},
            },
        },
        {
            kWiiMusicSha1,
            axfx::Build::kSeptember2008,
            0x800e8b48,
            0x800ac63c,
            0x800ad130,
            0x800ad334,
            0,
            0,
            0,
            0,
            {
                {nw4r::EffectType::kReverbStd2008, 0x800b7f24, 0x800b7fe8, 0x800b8014, 0x800b813c, 0x800b8024,
                 0x800b80d8, 0x800b83d4},
                {nw4r::EffectType::kReverbHi2008, 0x800b71a8, 0x800b7278, 0x800b72a4, 0x800b73cc, 0x800b72b4,
                 0x800b7368, 0x800b7698},
            },
        },
    };

    return kFunctions;
}

const EffectFunctions* FindEffectFunctions(const std::string& dol_sha1)
{
    for (const EffectFunctions& functions : KnownEffectFunctions())
    {
        if (dol_sha1 == functions.dol_sha1)
        {
            return &functions;
        }
    }

    return nullptr;
}

} // namespace

bool HasEffectProfile(const std::string& dol_sha1)
{
    return FindEffectFunctions(dol_sha1) != nullptr;
}

ArchiveEffects FindEffects(const std::string& dol_sha1, const std::string& archive_path,
                           const ntdlib::rsar::SoundArchive& archive)
{
    const std::string name = ArchiveFileName(archive_path);
    ArchiveEffects found;
    if (dol_sha1 == kWiiSportsSha1)
    {
        // The menus have NW4R SND's default reverb on bus A (RPSndAudioMgr::setDefaultFx, 0x801b6d00, with the
        // parameters at 0x8037ae68). Each sport sets bus A again as its scene starts. Tennis (0x80214e40), baseball
        // (0x8024e7dc) and bowling (0x803153c0) keep that reverb and add FxChorus's default on bus B. Golf (0x8028e664)
        // and boxing (0x802ee324) have reverbs with other times and levels.
        const nw4r::EffectSetup menu = ReverbHi(0.02f, 3.0f, 0.6f, 0.4f, 0.1f, 1.0f);
        if (name == "rp_common_sports.brsar")
        {
            found.effects = {menu};
        }
        else if (name == "rp_tns_sound.brsar" || name == "rp_bsb_sound.brsar" || name == "rp_bow_sound.brsar")
        {
            found.effects = {menu, DefaultChorus(1)};
        }
        else if (name == "rp_gol_sound.brsar")
        {
            found.effects = {ReverbHi(0.02f, 1.2f, 0.6f, 0.4f, 0.1f, 0.7f)};
        }
        else if (name == "rp_box_sound.brsar")
        {
            found.effects = {ReverbHi(0.02f, 2.2f, 0.6f, 0.4f, 0.1f, 0.5f)};
        }
    }
    else if (dol_sha1 == kWiiPlaySha1)
    {
        // Every scene has the same effects (setDefaultFx, 0x801b3588, with the parameters at 0x803182c0): a reverb on
        // bus A, a chorus on bus B and a delay on bus C.
        if (name.starts_with("rp_"))
        {
            found.effects = {ReverbHi(0.03f, 1.5f, 0.0f, 0.1f, 0.2f, 0.5f),
                             Fixed(1, nw4r::EffectType::kChorus, Words({5.0f, 1.0f, 0.6f})),
                             Fixed(2, nw4r::EffectType::kDelay, Words({40.0f, 0.0f, 0.6f}))};
        }
    }
    else if (dol_sha1 == kSmoothMovesSha1)
    {
        // The start-up of the sound object (0x80100198) puts a delay on bus B (parameters at 0x804da1e4) and a chorus
        // on bus C (0x804da25c). The music's variables pick bus A's reverb from the 10 rows at 0x804da11c: preDelay,
        // time, coloration, damping and mix.
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

            found.effects = {Fixed(1, nw4r::EffectType::kDelay, Words({160.0f, 0.4f, 1.0f})),
                             Fixed(2, nw4r::EffectType::kChorus, Words({10.0f, 0.5f, 1.0f})), reverb};
        }
    }
    else if (dol_sha1 == kCityFolkSha1)
    {
        // The sound object's start-up (0x801ce58c) puts FxReverbHi on bus A with the parameters of 2008: those of the
        // older builds, then earlyMode, preDelayTimeMax, fusedMode, earlyGain and fusedGain. The start of the music
        // (0x801cbbbc) sends 0.25 of the outdoor and town music to bus A, and 0.28 of K.K. Slider's songs.
        if (name == "rv_forest.brsar")
        {
            found.effects = {ReverbHi2008(0.01f, 0.5f, 0.6f, 0.4f, 0.1f, 1.0f, {4, 0.05f, 2, 0.3f, 0.3f})};
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
        }
    }
    else if (dol_sha1 == kMarioKartSha1 && name == "revo_kart.brsar")
    {
        // Only bus A has an effect: the standard reverb of 2008 that AudioFx::create (0x802119f4) makes for type 4.
        // As each scene starts, code in the game's REL module sets it up from one set of parameters. A race picks the
        // set by course (.text + 0x2027dc). Mushroom Gorge and the N64 courses Mario Raceway, Sherbet Land and DK's
        // Jungle Parkway get a shorter pre-delay and less damping. The names of their sounds start with SE_CN_KNK_,
        // SE_CR_64M_, SE_CR_64S_ and SE_CR_64D_. A menu picks the set by the sound type of its section
        // (.text + 0x2021c0). The type is the group of the section's music. SEQ_O_SELECT_CH (group 2) and SEQ_O_EARTH
        // (group 3) get a lower outGain. SEQ_O_RANKING_CH (group 4) gets another reverb. Other sounds get the set as
        // it is. Only six of the archive's sequences send anything to bus A: these three, SEQ_O_OPTION_CH and two
        // sounds of Sherbet Land.
        const auto is_course_sound = [](const std::string& sound)
        {
            return sound.starts_with("SE_CN_KNK_") || sound.starts_with("SE_CR_64M_") ||
                   sound.starts_with("SE_CR_64S_") || sound.starts_with("SE_CR_64D_");
        };

        nw4r::EffectSetup courses = ReverbStd2008(0.015f, 3.0f, 0.6f, 0.35f, 1.0f, {5, 0.02f, 5, 0.0f, 1.0f});
        courses.sounds = SoundsWhere(archive, is_course_sound);
        nw4r::EffectSetup menus = ReverbStd2008(0.02f, 3.0f, 0.6f, 0.4f, 0.73f, {5, 0.02f, 0, 0.0f, 1.0f});
        menus.sounds = SoundsNamed(archive, {"SEQ_O_SELECT_CH", "SEQ_O_EARTH"});
        nw4r::EffectSetup ranking = ReverbStd2008(0.025f, 1.5f, 0.5f, 0.5f, 1.0f, {5, 0.025f, 0, 0.0f, 0.5f});
        ranking.sounds = SoundsNamed(archive, {"SEQ_O_RANKING_CH"});
        found.effects = {ReverbStd2008(0.02f, 3.0f, 0.6f, 0.4f, 1.0f, {5, 0.02f, 0, 0.0f, 1.0f}), courses, menus,
                         ranking};
    }
    else if (dol_sha1 == kWiiMusicSha1 && name.starts_with("rp_"))
    {
        // Each scene sets the buses from a row of the table at 0x8059f978 (0x803cddc0). In every scene but Mii
        // Maestro's, bus A has the standard reverb of 2008 with row 0 of the parameters at 0x8059fe20. Mii Maestro has
        // FxReverbHi with row 2 of the parameters at 0x8059fd58. The names of its sounds start with RP_OCS_. The game
        // sets the longest pre-delay of both to 0.09 (0x803cdfb4). Some scenes put a reverb on bus B too. No sequence
        // sends anything to bus B there. The game sets sends on the instruments of a jam session by stage. A rip
        // doesn't know the stage and leaves those sends out.
        const auto is_orchestra_sound = [](const std::string& sound)
        {
            return sound.starts_with("RP_OCS_");
        };

        nw4r::EffectSetup orchestra = ReverbHi2008(0.02f, 1.2f, 0.5f, 0.3f, 0.1f, 0.9f, {5, 0.09f, 4, 0.5f, 1.0f});
        orchestra.sounds = SoundsWhere(archive, is_orchestra_sound);
        found.effects = {ReverbStd2008(0.0f, 1.3f, 0.5f, 0.5f, 1.0f, {5, 0.09f, 4, 0.5f, 1.0f}), orchestra};
    }

    found.dol_sha1 = dol_sha1;

    return found;
}

namespace
{

// Fills in `setups`, effects of the game whose main.dol has `dol_sha1`, in the driver's parameter block `params`.
// `sends` says whether the game sets sends on sounds. Returns why the driver can't set the effects up, if it can't.
std::optional<std::string> PutEffects(uint8_t* params, const std::string& dol_sha1,
                                      const std::vector<nw4r::EffectSetup>& setups, bool sends)
{
    const auto put = [params](uint32_t offset, uint32_t value)
    {
        xsflib::StoreBe32(params + offset, value);
    };

    // The driver's layout must be the one that these offsets describe: its block has no bus for the sound's send.
    if (xsflib::LoadBe32(params + kParamsSoundEffectsOffset + 4) != kNoBus)
    {
        return "the driver's parameter block doesn't have the layout that the ripper expects";
    }

    if (setups.empty())
    {
        return std::nullopt;
    }

    const EffectFunctions* functions = FindEffectFunctions(dol_sha1);
    if (!functions || setups.size() > kDriverEffects)
    {
        return "the driver can't set up this game's effects";
    }

    const uint32_t function_words[] = {
        functions->ax_register_callback, functions->ax_manager,      functions->append_effect, functions->clear_effect,
        functions->seq_handle_ctor,      functions->seq_handle_dtor, functions->read_variable, functions->sound_sends};
    for (std::size_t i = 0; i < std::size(function_words); i++)
    {
        put(kParamsEffectFunctionsOffset + static_cast<uint32_t>(i * 4), function_words[i]);
    }

    static_assert(kParamsEffectFunctionsOffset + std::size(function_words) * 4 == kParamsEffectCountOffset);
    put(kParamsEffectCountOffset, static_cast<uint32_t>(setups.size()));
    put(DriverBlock::kAxfxBuildOffset, static_cast<uint32_t>(functions->axfx_build));
    uint32_t first = 0;
    for (std::size_t i = 0; i < setups.size(); i++)
    {
        const nw4r::EffectSetup& effect = setups[i];
        const auto found = std::find_if(functions->classes.begin(), functions->classes.end(),
                                        [&effect](const EffectClass& c) { return c.type == effect.type; });
        const auto words = static_cast<uint32_t>(nw4r::EffectParamWords(effect.type));
        const auto sets = static_cast<uint32_t>(std::max<std::size_t>(effect.presets.size(), 1));
        if (found == functions->classes.end() || first + words * sets > kDriverEffectWords ||
            (!effect.presets.empty() && functions->read_variable == 0))
        {
            return "the driver can't set up this game's effects";
        }

        const uint32_t at = kParamsEffectsOffset + static_cast<uint32_t>(i * 32);
        const uint32_t effect_words[] = {static_cast<uint32_t>(effect.bus),
                                         found->ctor,
                                         found->mem_size,
                                         found->assign,
                                         found->set_param,
                                         words,
                                         static_cast<uint32_t>(effect.presets.size()),
                                         first};
        for (std::size_t w = 0; w < std::size(effect_words); w++)
        {
            put(at + static_cast<uint32_t>(w * 4), effect_words[w]);
        }

        const uint32_t native_at = DriverBlock::kNativeOffset + static_cast<uint32_t>(i) * DriverBlock::kNativeSize;
        put(native_at, static_cast<uint32_t>(effect.type));
        put(native_at + 4, found->start_up);
        put(native_at + 8, found->shutdown);
        put(native_at + 12, found->update_buffer);

        const auto put_params = [&](const nw4r::EffectParams& set)
        {
            for (uint32_t w = 0; w < words; w++)
            {
                put(kParamsEffectWordsOffset + (first++) * 4, set[w]);
            }
        };

        if (effect.presets.empty())
        {
            put_params(effect.params);
        }

        for (const nw4r::EffectParams& preset : effect.presets)
        {
            put_params(preset);
        }
    }

    if (sends && functions->sound_sends == 0)
    {
        return "the driver can't set up this game's effects";
    }

    return std::nullopt;
}

// The driver's parameter block up to the end of the effects, with `setups` filled in (see PutEffects) and a sound
// without a send. Returns nullopt if the driver can't set the effects up.
std::optional<std::array<uint8_t, kParamsEffectsEnd>> EffectsBlock(const std::string& dol_sha1,
                                                                   const std::vector<nw4r::EffectSetup>& setups,
                                                                   bool sends)
{
    std::array<uint8_t, kParamsEffectsEnd> block{};
    xsflib::StoreBe32(&block[kParamsSoundEffectsOffset + 4], kNoBus);
    if (PutEffects(block.data(), dol_sha1, setups, sends))
    {
        return std::nullopt;
    }

    return block;
}

// Whether the game sets up other effects for `sound` than for a sound that no effect lists.
bool GetsOtherEffects(const std::vector<nw4r::EffectSetup>& setups, uint32_t sound)
{
    for (std::size_t i = 0; i < setups.size(); i++)
    {
        if (nw4r::SetUpFor(setups, i, sound) != nw4r::SetUpFor(setups, i, nw4r::kUnlistedSound))
        {
            return true;
        }
    }

    return false;
}

} // namespace

std::optional<std::vector<uint8_t>> BuildLibrary(const ntdlib::disc::GameFiles& game, const GameProfile& profile,
                                                 const std::string& archive_path,
                                                 const ntdlib::rsar::SoundArchive& archive, const Tags& tags,
                                                 std::string* error, const ArchiveEffects& effects)
{
    const auto fail = [error](const std::string& message) -> std::optional<std::vector<uint8_t>>
    {
        if (error)
        {
            *error = message;
        }

        return std::nullopt;
    };

    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(game.dol);
    if (!dol)
    {
        return fail("main.dol isn't a DOL");
    }
    if (archive_path.size() + 1 >= kParamsPathSize)
    {
        return fail("the sound archive's path is longer than the driver supports");
    }

    // The game's code and data, section by section.
    ProgramBuilder program;
    for (const ntdlib::Dol::Section& section : dol->Sections())
    {
        program.AddMemory(section.address, section.data.data(), section.data.size());
    }

    // The driver, with its parameters: the settings, the archive and the game's profile. A .miniwsf fills in the sound.
    std::vector<uint8_t> driver(std::begin(kDriverBlob), std::end(kDriverBlob));
    uint8_t* params = driver.data() + (kDriverParams - kDriverBase);
    const auto put = [params](uint32_t offset, uint32_t value)
    {
        xsflib::StoreBe32(params + offset, value);
    };

    put(kParamsFrameTicksOffset, kFrameTicks);
    put(kParamsInfoSizeOffset, archive.InfoSize());
    put(kParamsPrioritiesOffset, kSoundThreadPriority);
    put(kParamsPrioritiesOffset + 4, kDvdThreadPriority);

    const std::string dvd_path = "/" + archive_path;
    std::memcpy(params + kParamsPathOffset, dvd_path.c_str(), dvd_path.size() + 1);

    const uint32_t functions[] = {
        profile.ai_init,
        profile.ax_init,
        profile.alloc,
        profile.sleep_ticks,
        profile.init_sound_system,
        profile.archive_ctor,
        profile.archive_open,
        profile.archive_load_info,
        profile.player_ctor,
        profile.player_mem_size,
        profile.player_strm_size,
        profile.player_setup,
        profile.heap_ctor,
        profile.heap_create,
        profile.load_group,
        profile.start_sound,
        profile.update,
        profile.startable_offset,
        kObjectSize,
    };
    for (std::size_t i = 0; i < std::size(functions); i++)
    {
        put(kParamsProfileOffset + static_cast<uint32_t>(i * 4), functions[i]);
    }
    static_assert(kParamsProfileOffset + std::size(functions) * 4 == kParamsBankGroupOffset);
    put(kParamsInitFlagOffset, profile.init_flag);

    // The driver must be able to set up every effect. The library holds the effects of a sound that no effect lists. A
    // .miniwsf gives the driver the effects of a sound that gets others (see BuildMini).
    const std::string dol_sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(game.dol));
    const bool sends = !effects.sends.empty();
    if (const std::optional<std::string> problem = PutEffects(params, dol_sha1, effects.effects, sends))
    {
        return fail(*problem);
    }

    const auto block = EffectsBlock(dol_sha1, nw4r::EffectsFor(effects.effects, nw4r::kUnlistedSound), sends);
    if (!block)
    {
        return fail("the driver can't set up this game's effects");
    }

    std::copy(block->begin() + kParamsEffectCountOffset, block->end(), params + kParamsEffectCountOffset);

    program.AddMemory(kDriverBase, driver);

    // main branches to the driver.
    uint8_t branch[4];
    xsflib::StoreBe32(branch, 0x48000000 | ((kDriverEntry - profile.main) & 0x03fffffc));
    program.AddMemory(profile.main, branch, sizeof(branch));

    // The disc: the partition's header, and the archive.
    std::vector<uint8_t> bytes;
    if (!game.read_file || !game.read_file(archive_path, bytes))
    {
        return fail("can't read " + archive_path + " from the disc");
    }

    program.AddFile("sys/boot.bin", game.boot);
    program.AddFile(archive_path, bytes);

    ProcessDescriptor d;
    d.entry = dol->Entry();
    d.status_address = DriverBlock::kAddress + DriverBlock::kStatusOffset;
    d.ios_version = game.tmd.size() >= 0x18c ? xsflib::LoadBe32(&game.tmd[0x188]) : profile.ios_version;

    return xsflib::psf::WritePsf(kPsfVersionWsf, d.Serialize(), program.Data(), tags);
}

std::vector<uint8_t> BuildMini(uint32_t index, const SoundData& data, const std::string& lib_name, Tags tags,
                               const ArchiveEffects& effects)
{
    uint8_t patch[12];
    xsflib::StoreBe32(&patch[0], index);
    xsflib::StoreBe32(&patch[4], data.group);
    xsflib::StoreBe32(&patch[8], data.heap_size);
    ProgramBuilder program;
    program.AddMemory(DriverBlock::kAddress + kParamsSoundOffset, patch, sizeof(patch));

    // Whether the sound's variables pick the presets, and the send that the game sets on the sound.
    const bool follows = nw4r::FollowsVariables(effects.effects, index);
    const auto send = std::find_if(effects.sends.begin(), effects.sends.end(),
                                   [index](const nw4r::SoundSend& s) { return s.sound == index; });
    uint8_t sound_effects[12];
    xsflib::StoreBe32(&sound_effects[0], follows ? 1 : 0);
    xsflib::StoreBe32(&sound_effects[4], send != effects.sends.end() ? static_cast<uint32_t>(send->bus) : kNoBus);
    xsflib::StoreBe32(&sound_effects[8], send != effects.sends.end() ? std::bit_cast<uint32_t>(send->send) : 0);

    // A sound that gets other effects than the library's patches the block from the number of effects to its end. That
    // covers the words above too.
    std::optional<std::array<uint8_t, kParamsEffectsEnd>> block;
    if (GetsOtherEffects(effects.effects, index))
    {
        block = EffectsBlock(effects.dol_sha1, nw4r::EffectsFor(effects.effects, index), !effects.sends.empty());
    }

    if (block)
    {
        std::copy(std::begin(sound_effects), std::end(sound_effects), block->begin() + kParamsSoundEffectsOffset);
        program.AddMemory(DriverBlock::kAddress + kParamsEffectCountOffset, block->data() + kParamsEffectCountOffset,
                          kParamsEffectsEnd - kParamsEffectCountOffset);
    }
    else if (follows || send != effects.sends.end())
    {
        program.AddMemory(DriverBlock::kAddress + kParamsSoundEffectsOffset, sound_effects, sizeof(sound_effects));
    }

    if (data.bank_group)
    {
        uint8_t bank_group[4];
        xsflib::StoreBe32(bank_group, *data.bank_group);
        program.AddMemory(DriverBlock::kAddress + kParamsBankGroupOffset, bank_group, sizeof(bank_group));
    }

    tags["_lib"] = lib_name;

    return xsflib::psf::WritePsf(kPsfVersionWsf, {}, program.Data(), tags);
}

namespace
{

// The JAudio2 driver's parameter block (driver/jaudio.c), from its address: the header, ntdlib's Z2Params, then the
// Wii's fields.
constexpr uint32_t kJAudioZ2Offset = kJAudioZ2Address - DriverBlock::kAddress;
constexpr uint32_t kJAudioWiiOffset =
    kJAudioZ2Offset + ntdlib::flipper::z2::kProfileOffset + sizeof(ntdlib::flipper::z2::Profile);
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

// A JAudio2 driver's image: its blob, the address it's loaded at, its entry and its parameter block. Every driver's
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

// How Pikmin 2's driver makes a piece's object: driver/pikmin2.c's PIECE_ values. Each is a way in which the game
// makes a piece's object.
enum class PieceKind : uint32_t
{
    kPlain = 0,    // a BgmSeq that only plays
    kMain = 1,     // an area's music: a JumpBgmSeq with the scene's directors
    kDirected = 2, // a boss's music, or the music of 2-player battles
    kAuto = 3,     // a cave's music: an AutoBgm. Its conductor picks what the tracks play
    kBattle = 4,   // a DirectedBgm with the 2-player battle directors that the game gives battle_t
    kFree = 5,     // a DirectedBgm without directors
    kAutoFree = 6, // an AutoBgm without directors or the arrangement of its melody
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
// a start point: the part of the music that the boss starts.
struct Pikmin2Piece
{
    const char* sequence;
    PieceKind kind;
    uint8_t wave_scene;
    const char* conductor = "";
    SoundInfoWords info = kAreaInfo;
    uint16_t beat_interval = 0;
    uint8_t start_point = 0;
};

// Pikmin 2's music: every file of the sequence archive but the effects sequence, se.bms, in the archive's order. A
// piece's sound ID is 0x80000000 and its place here. The Wii's sequence archive has the GameCube's files, and the
// Wii's code makes the pieces' objects as the GameCube's does.
//
// The game makes most pieces' objects in PikSceneMgr::initMainBgm, initBossBgm and initAdditionalBgm. A piece's scene
// loads one of the wave system's 49 groups (wScene_*.aw) besides group 0. A cave floor's conductor comes from a list
// for each area (BgmList_*.txt). The conductor picks the phrases that the cave music's tracks play. Its name gives the
// floor's wave group. A rip plays one conductor: the first of each family, such as caveconc_00_0.cnd and new_00_0.cnd.
// The game's code names caveconc_00_0.cnd itself, for a boss's floor. A boss starts its music when it appears, at a
// part of the music: the start point. The middle bosses start m_boss at their appearance (9). The final boss starts
// l_boss at its part for four weapons (1). At start point 0, a track of l_boss jumps to itself without end.
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
    {"l_boss.bms", PieceKind::kDirected, 35, "", kBossInfo, 60, 1},
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
};

// The bytes that driver/pikmin2.c's Piece gives each file name.
constexpr std::size_t kPieceNameSize = 24;

// Pikmin 2's driver parameters: the profile's `words`, then the number of pieces and the pieces (driver/pikmin2.c's
// Piece).
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

} // namespace

const std::vector<JAudioProfile>& KnownJAudioProfiles()
{
    // Addresses in the games' main.dol files.
    static const std::vector<JAudioProfile> kProfiles = {
        // The Legend of Zelda: Twilight Princess. Every function's address was checked against its code. The sizes and
        // offsets are the game's: its machine start-up gives the heaps and the ARAM manager, its audio set-up the audio
        // heap, and its sound objects the analysis's offsets. The game keeps its archive heap in MEM2's root heap. The
        // audio set-up loads four wave groups itself.
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
        // each track of a BGM by tables in the game's code, and the driver starts music the same way. The parameters
        // follow driver/jungle_beat.c's Params from frame_ticks.
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
                },
            .files = {"AudioRes/Seqs/JaiSeq.arc", "AudioRes/Info/JaiChord.arc", "AudioRes/Info/ChordSeDataBase.csd"},
        },
        // New Play Control! Pikmin (USA). The game's sound library is the early JAudio1 of the GameCube release. The
        // game calls the library through C functions (Jac, Jaf, Jaq and Jam). The game's code holds the sequence index.
        // The game plays a piece of music by the piece's number in the index. The piece plays in one of two slots, with
        // group 0 of the wave system of the same number. The disc has no map. Each function in main.dol was matched
        // with the function at the same place in the GameCube release's sound library. Each match was checked against
        // the code. The sizes come from the game's start-up, System::Initialise (0x800dc878). The parameters follow
        // driver/pikmin.c's Params from frame_ticks.
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
            .z2 = {},
            .sleep = 0,
            .reset_data = 0,
            .reset_flag = 0,
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
            .z2 = {},
            .sleep = 0,
            .reset_data = 0,
            .reset_flag = 0,
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
            .z2 = {},
            .sleep = 0,
            .reset_data = 0,
            .reset_flag = 0,
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

std::optional<JAudioFiles> FindJAudioFiles(const ntdlib::disc::GameFiles& game)
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
        if ((path.ends_with(".baa") || path.ends_with(".szs")) && ReadStartUpArchive(game, file.path, data) &&
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
        if (!file.path.starts_with(folder) || !xsflib::AsciiLower(file.path).ends_with(".arc") ||
            !game.read_file(file.path, data))
        {
            continue;
        }

        bool archive = StartsWith(data, "RARC");
        if (!archive && ntdlib::jaudio::IsYaz0(data))
        {
            try
            {
                archive = StartsWith(ntdlib::jaudio::DecodeYaz0(data), "RARC");
            }
            catch (const xsflib::FormatError&)
            {
            }
        }
        if (archive)
        {
            found->archives.push_back(file.path);
        }
    }

    return found;
}

bool ReadStartUpArchive(const ntdlib::disc::GameFiles& game, const std::string& path, std::vector<uint8_t>& data)
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
    const auto fail = [error](const std::string& message) -> std::optional<std::vector<uint8_t>>
    {
        if (error)
        {
            *error = message;
        }

        return std::nullopt;
    };

    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(game.dol);
    if (!dol)
    {
        return fail("main.dol isn't a DOL");
    }

    // The game's code and data, section by section.
    ProgramBuilder program;
    for (const ntdlib::Dol::Section& section : dol->Sections())
    {
        program.AddMemory(section.address, section.data.data(), section.data.size());
    }

    // The driver, with the profile and the Wii's fields. A .miniwsf fills in the sound and its wave groups.
    std::vector<uint8_t> driver(std::begin(jaudio::kDriverBlob), std::end(jaudio::kDriverBlob));
    uint8_t* params = driver.data() + (jaudio::kDriverParams - jaudio::kDriverBase);
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

    // Another driver takes the place of driver/jaudio.c, with the profile's words after the wave list.
    const std::optional<JAudioDriverImage> image = ImageOf(profile.driver);
    if (!image || image->params != DriverBlock::kAddress)
    {
        return fail("WSF has no driver for this profile");
    }

    if (profile.driver != JAudioDriver::kZ2)
    {
        driver.assign(image->blob.begin(), image->blob.end());
        const uint32_t block = image->params - image->base;
        if (block + kJAudioParamsOffset + profile.params.size() * 4 > driver.size())
        {
            return fail("the profile has more parameters than its driver's block holds");
        }

        for (std::size_t i = 0; i < profile.params.size(); i++)
        {
            xsflib::StoreBe32(driver.data() + block + kJAudioParamsOffset + i * 4, profile.params[i]);
        }
    }

    program.AddMemory(image->base, driver);

    // main branches to the driver.
    uint8_t branch[4];
    xsflib::StoreBe32(branch, 0x48000000 | ((image->entry - profile.main) & 0x03fffffc));
    program.AddMemory(profile.main, branch, sizeof(branch));

    // The disc: the partition's header, and the sound files.
    program.AddFile("sys/boot.bin", game.boot);
    std::vector<uint8_t> bytes;
    for (const std::string& path : paths)
    {
        if (!game.read_file || !game.read_file(path, bytes))
        {
            return fail("can't read " + path + " from the disc");
        }

        program.AddFile(path, bytes);
    }

    ProcessDescriptor d;
    d.entry = dol->Entry();
    d.status_address = DriverBlock::kAddress + DriverBlock::kStatusOffset;
    d.ios_version = game.tmd.size() >= 0x18c ? xsflib::LoadBe32(&game.tmd[0x188]) : profile.ios_version;

    return xsflib::psf::WritePsf(kPsfVersionWsf, d.Serialize(), program.Data(), tags);
}

std::vector<uint8_t> BuildJAudioMini(uint32_t sound_id, std::span<const uint32_t> waves, const std::string& lib_name,
                                     Tags tags)
{
    uint8_t sound[4];
    xsflib::StoreBe32(sound, sound_id);
    ProgramBuilder program;
    program.AddMemory(DriverBlock::kAddress + kJAudioZ2Offset + ntdlib::flipper::z2::kSoundOffset, sound,
                      sizeof(sound));
    const std::vector<uint8_t> list = ntdlib::flipper::z2::WaveList(waves);
    program.AddMemory(DriverBlock::kAddress + kJAudioZ2Offset + ntdlib::flipper::z2::kWaveCountOffset, list.data(),
                      list.size());

    tags["_lib"] = lib_name;

    return xsflib::psf::WritePsf(kPsfVersionWsf, {}, program.Data(), tags);
}

std::optional<JAudioFiles> FindJacFiles(const ntdlib::disc::GameFiles& game, const JAudioProfile& profile)
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

std::vector<JAudioSound> ReadJacSounds(std::span<const uint8_t> dol, const JAudioProfile& profile)
{
    constexpr std::size_t kHeader = 0x20, kEntry = 0x20, kNameSize = 14, kSizeOffset = 0x1c;
    constexpr uint32_t kFirstPiece = 2;
    const std::optional<ntdlib::Dol> parsed = ntdlib::Dol::Parse(dol);
    if (!parsed)
    {
        throw xsflib::FormatError("main.dol isn't a DOL");
    }

    // The index runs from the profile's address to the end of the section that holds it.
    std::span<const uint8_t> index;
    for (const ntdlib::Dol::Section& section : parsed->Sections())
    {
        if (profile.sequence_index >= section.address && profile.sequence_index - section.address < section.data.size())
        {
            index = section.data.subspan(profile.sequence_index - section.address);
        }
    }

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
    for (uint32_t number = kFirstPiece; number < count; number++)
    {
        const std::size_t entry = kHeader + std::size_t{number} * kEntry;
        if (r.U32(entry + kSizeOffset) == 0)
        {
            continue;
        }

        std::string label = r.CString(entry, kNameSize);
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

    return sounds;
}

std::vector<JAudioSound> ReadPikmin2Sounds(std::span<const uint8_t> aaf, std::span<const uint8_t> sequence_archive,
                                           std::span<const uint8_t> names, const JAudioProfile& profile)
{
    std::vector<uint8_t> decoded;
    if (ntdlib::jaudio::IsYaz0(sequence_archive))
    {
        decoded = ntdlib::jaudio::DecodeYaz0(sequence_archive);
        sequence_archive = decoded;
    }

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

        JAudioSound sound = {.id = ntdlib::jaudio::kAafSequence | static_cast<uint32_t>(sounds.size()),
                             .label = std::string(name.substr(0, name.rfind('.'))),
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
        sounds.push_back({.id = id,
                          .label = name != named.end() ? name->second : label,
                          .section = "SE",
                          .waves = profile.static_waves});
    }

    return sounds;
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

std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& microcode_path, const ntdlib::Microcode& microcode,
                                         nw4r::SndBuild build, const Tags& tags, const ArchiveEffects& effects)
{
    ArchiveDescriptor descriptor;
    descriptor.archive_path = archive_path;
    descriptor.microcode_path = microcode_path;
    descriptor.microcode_address = microcode.address;
    descriptor.snd_build = build.date;
    descriptor.effects = effects.effects;
    descriptor.sends = effects.sends;

    std::vector<uint8_t> words(microcode.words.size() * 2);
    for (std::size_t i = 0; i < microcode.words.size(); i++)
    {
        xsflib::StoreBe16(&words[i * 2], microcode.words[i]);
    }

    ProgramBuilder program;
    program.AddFile(archive_path, archive);
    program.AddFile(microcode_path, words);

    return xsflib::psf::WritePsf(kPsfVersionWsf, descriptor.Serialize(), program.Data(), tags);
}

std::vector<uint8_t> BuildArchiveMini(uint32_t index, const std::string& lib_name, Tags tags)
{
    ProgramBuilder program;
    program.AddSound(index);

    tags["_lib"] = lib_name;

    return xsflib::psf::WritePsf(kPsfVersionWsf, {}, program.Data(), tags);
}

std::vector<uint8_t> KeepSequenceData(const ntdlib::rsar::SoundArchive& archive, const std::vector<uint32_t>& indices)
{
    const std::span<const uint8_t> all = archive.Bytes();
    std::vector<uint8_t> kept(all.begin(), all.end());

    // Clears `size` bytes from `offset`, up to the copy's end at most.
    const auto clear = [&kept](uint64_t offset, uint64_t size)
    {
        if (offset < kept.size())
        {
            const uint64_t end = std::min<uint64_t>(offset + size, kept.size());
            std::fill(kept.begin() + static_cast<std::ptrdiff_t>(offset),
                      kept.begin() + static_cast<std::ptrdiff_t>(end), uint8_t{0});
        }
    };

    // The groups that the archive holds: their files, then their wave data.
    for (const ntdlib::rsar::SoundArchive::Group& group : archive.Groups())
    {
        if (group.internal)
        {
            clear(group.offset, group.size);
            clear(group.wave_data_offset, group.wave_data_size);
        }
    }

    // Copies a file and its wave data back from the spans that File returns.
    const auto keep = [&](std::span<const uint8_t> data)
    {
        if (!data.empty())
        {
            std::copy(data.begin(), data.end(), kept.begin() + (data.data() - all.data()));
        }
    };
    const auto keep_file = [&](uint32_t file_id)
    {
        if (const std::optional<ntdlib::rsar::FileData> file = archive.File(file_id))
        {
            keep(file->file);
            keep(file->wave_data);
        }
    };

    // The data that the model reads for each sequence: its file, and its bank's file and wave data.
    for (const uint32_t index : indices)
    {
        if (index >= archive.Sounds().size())
        {
            continue;
        }

        const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[index];
        keep_file(sound.file_id);
        if (sound.bank < archive.Banks().size())
        {
            keep_file(archive.Banks()[sound.bank].file_id);
        }
    }

    return kept;
}

namespace
{

// True if the file at `path` holds exactly `data`. The file is compared in blocks of 1 MiB. A library can be hundreds
// of megabytes.
bool FileHolds(const std::filesystem::path& path, std::span<const uint8_t> data)
{
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != data.size() || ec)
    {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    std::vector<char> block(std::size_t{1} << 20);
    for (std::size_t at = 0; at < data.size(); at += block.size())
    {
        const std::size_t size = std::min(block.size(), data.size() - at);
        if (!file.read(block.data(), static_cast<std::streamsize>(size)) ||
            std::memcmp(block.data(), data.data() + at, size) != 0)
        {
            return false;
        }
    }

    return true;
}

} // namespace

std::string LibraryName(const std::filesystem::path& folder, xsflib::rip::FileNames& names, const std::string& base,
                        std::span<const uint8_t> library)
{
    std::string name = names.Library(base);
    std::error_code ec;
    while (std::filesystem::exists(folder / xsflib::PathFromUtf8(name), ec) &&
           !FileHolds(folder / xsflib::PathFromUtf8(name), library))
    {
        name = names.Library(base);
    }

    return name;
}

std::vector<std::string> MissingData(const ntdlib::rsar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size())
    {
        return {"the sound itself"};
    }

    const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[index];
    std::vector<std::string> missing;
    if (!archive.File(sound.file_id))
    {
        missing.push_back("its sequence data");
    }

    if (sound.bank >= archive.Banks().size())
    {
        missing.push_back("its bank");
    }
    else if (!archive.File(archive.Banks()[sound.bank].file_id))
    {
        const std::string& name = archive.Banks()[sound.bank].name;
        missing.push_back("bank " + (name.empty() ? "#" + std::to_string(sound.bank) : name));
    }

    return missing;
}

bool IsMusic(const ntdlib::rsar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size() || archive.Sounds()[index].type != ntdlib::rsar::SoundType::kSequence)
    {
        return false;
    }

    const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[index];
    if (xsflib::AsciiLower(sound.name).find("bgm") != std::string::npos)
    {
        return true;
    }

    if (sound.player_id >= archive.Players().size())
    {
        return false;
    }

    // The player's name, word by word, each word without its trailing digits (PLAYER_BGM1).
    bool music = false;
    bool effects = false;
    const std::string player = xsflib::AsciiLower(archive.Players()[sound.player_id].name);
    for (std::size_t start = 0; start <= player.size();)
    {
        const std::size_t end = std::min(player.find('_', start), player.size());
        std::string word = player.substr(start, end - start);
        word.erase(word.find_last_not_of("0123456789") + 1);
        music = music || word == "bgm" || word == "music" || word == "song" || word == "fanfare";
        effects = effects || word == "se";
        start = end + 1;
    }

    return music && !effects;
}

namespace
{

// The length analysis's rules. The model runs in 3 ms AX frames. xsflib's defaults count in frames of that length. They
// give two loops and a 10 s fade, or the end, for at most 600 s. A sequence that stays busy without a sound for 180 s
// has ended. No music rests that long. Some sound effects come close. WSF counts a loop's point, a loop's sound and a
// steady sequence's loop from the frame that runs them. A sequence that ends does so at its last sound.
xsflib::rip::LengthRules LengthRules()
{
    xsflib::rip::LengthRules rules;
    rules.loop_offset = 0;
    rules.sound_offset = 0;
    rules.steady_offset = 0;
    rules.end_at_idle_start = false;

    return rules;
}

// The model of NW4R SND and AX for the length analysis. With the DSP, a frame has sound if it mixed anything louder
// than silence. Without the DSP, a frame has sound if any note plays.
class LengthModel
{
public:
    LengthModel(nw4r::ArchiveModel& model, nw4r::SeqPlayer& player, bool dsp)
        : model_(model), player_(player), dsp_(dsp)
    {
    }

    bool RunFrame()
    {
        samples_.clear();
        if (!dsp_)
        {
            model_.RunFrameWithoutDsp();

            return true;
        }

        if (!model_.RunFrame(samples_))
        {
            return false;
        }

        ringing_frames_ = xsflib::rip::Audible(samples_) ? kRingingFrames : std::max(ringing_frames_ - 1, 0);

        return true;
    }

    bool Sounded() const
    {
        return dsp_ ? xsflib::rip::Audible(samples_) : model_.engine->Channels().ActiveCount() > 0;
    }

    // Effects ring on after the notes have ended. A delay's echo can return after a quiet gap. The analysis therefore
    // waits for a second without sound before a sequence with effects ends.
    bool Ringing() const
    {
        return model_.engine->HasEffects() && ringing_frames_ > 0;
    }

    std::size_t VoiceCount() const
    {
        return model_.ax.VoiceCount();
    }

    xsflib::rip::VoicePlace Voice(std::size_t index) const
    {
        const ax::Pb pb = model_.ax.ReadPb(static_cast<uint32_t>(index));
        return {pb.state == static_cast<uint16_t>(ax::VoiceState::kRun), pb.addr.loop_flag != 0,
                ax::Join(pb.addr.current_address_hi, pb.addr.current_address_lo)};
    }

    bool Active() const
    {
        return player_.IsActive();
    }

    bool Settled() const
    {
        return player_.IsSettled(*model_.engine);
    }

    bool Stuck() const
    {
        return player_.IsStuck(*model_.engine);
    }

    bool Moving() const
    {
        return player_.IsMoving();
    }

    uint64_t ChangedAt() const
    {
        return player_.ChangedAt();
    }

    bool Busy() const
    {
        return model_.engine->IsBusy();
    }

    bool ChannelsChanging() const
    {
        return model_.engine->Channels().AnyChanging();
    }

    int ActiveChannels() const
    {
        return model_.engine->Channels().ActiveCount();
    }

    template <typename Function>
    void OnLoop(Function function)
    {
        model_.engine->SetLoopHook(std::move(function));
    }

private:
    static constexpr int kRingingFrames = 334; // 1 s

    nw4r::ArchiveModel& model_;
    nw4r::SeqPlayer& player_;
    bool dsp_;
    std::vector<int16_t> samples_;
    int ringing_frames_ = 0;
};

// Measures a sequence in `arrangement` on the model (xsflib::rip::MeasureLength). With `dsp` set, the microcode runs on
// the DSP. Otherwise the model runs without the DSP.
std::optional<Timing> MeasureSequence(const ntdlib::rsar::SoundArchive& archive, const ntdlib::Microcode& microcode,
                                      nw4r::SndBuild build, uint32_t sound_index, const nw4r::Arrangement& arrangement,
                                      const ArchiveEffects& effects, bool dsp)
{
    auto model = std::make_unique<nw4r::ArchiveModel>();
    model->build = build;
    if (!dsp)
    {
        model->StartWithoutDsp(archive);
    }
    else if (!model->Start(microcode, archive))
    {
        return std::nullopt;
    }
    else
    {
        // Without the DSP, nothing reaches the effects.
        model->engine->SetEffects(nw4r::EffectsFor(effects.effects, sound_index),
                                  nw4r::FollowsVariables(effects.effects, sound_index),
                                  nw4r::SendsOf(effects.sends, sound_index));
    }

    nw4r::SeqPlayer* player = model->engine->StartSequence(sound_index, arrangement);
    if (!player)
    {
        return std::nullopt;
    }

    LengthModel length_model(*model, *player, dsp);
    return xsflib::rip::MeasureLength(length_model, LengthRules());
}

} // namespace

bool MicrocodeWorks(const ntdlib::Microcode& microcode, const ntdlib::rsar::SoundArchive& archive)
{
    auto model = std::make_unique<nw4r::ArchiveModel>();
    std::vector<int16_t> samples;
    return model->Start(microcode, archive) && model->RunFrame(samples);
}

struct LengthAnalyzer::Impl
{
    ntdlib::rsar::SoundArchive archive;
    ntdlib::Microcode microcode;
    nw4r::SndBuild build;
    nw4r::Arrangement arrangement;
    ArchiveEffects effects;
    bool dsp = true;
};

LengthAnalyzer::LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const ntdlib::Microcode& microcode,
                               nw4r::SndBuild build, const nw4r::Arrangement& arrangement,
                               const ArchiveEffects& effects)
    : impl_(new Impl{ntdlib::rsar::SoundArchive::Load(archive_bytes, wsf::nw4r::kRsarOptions), microcode, build,
                     arrangement, effects})
{
    impl_->dsp = MicrocodeWorks(microcode, impl_->archive);
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    // A sequence that the model can't play, such as one with a damaged bank, gets no length (xsflib::rip::TryMeasure).
    const auto measure = [this, sound_index]
    {
        return MeasureSequence(impl_->archive, impl_->microcode, impl_->build, sound_index, impl_->arrangement,
                               impl_->effects, impl_->dsp);
    };
    return xsflib::rip::TryMeasure(measure);
}

} // namespace wsf::rip
