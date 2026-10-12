// SPDX-License-Identifier: MIT

// WSF ripper (see rip.h).

#include "rip/rip.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/disc/containers.h"
#include "ntdlib/dol.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/ascii.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/rip/length.h"
#include "xsflib/sha1.h"

#include "ax/pb.h"
#include "ax/system.h"
#include "nw4r/engine.h"
#include "nw4r/rsar_options.h"
#include "nw4r/sequence.h"
#include "rip/driver_blob.h"
#include "rip/jaudio_driver_blob.h"
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

std::optional<std::vector<uint8_t>> BuildLibrary(const ntdlib::disc::GameFiles& game, const GameProfile& profile,
                                                 const std::string& archive_path,
                                                 const ntdlib::rsar::SoundArchive& archive, const Tags& tags,
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

std::vector<uint8_t> BuildMini(uint32_t index, const SoundData& data, const std::string& lib_name, Tags tags)
{
    uint8_t patch[12];
    xsflib::StoreBe32(&patch[0], index);
    xsflib::StoreBe32(&patch[4], data.group);
    xsflib::StoreBe32(&patch[8], data.heap_size);
    ProgramBuilder program;
    program.AddMemory(DriverBlock::kAddress + kParamsSoundOffset, patch, sizeof(patch));

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

} // namespace

const std::vector<JAudioProfile>& KnownJAudioProfiles()
{
    // Addresses in the game's main.dol. Every function's address was checked against its code. The sizes and offsets
    // are the game's: its machine start-up gives the heaps and the ARAM manager, its audio set-up the audio heap, and
    // its sound objects the analysis's offsets. The game keeps its archive heap in MEM2's root heap. The audio set-up
    // loads four wave groups itself.
    static const std::vector<JAudioProfile> kProfiles = {
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

    std::optional<JAudioFiles> found;
    std::vector<uint8_t> data;
    for (const ntdlib::disc::GameFile& file : game.files)
    {
        if (xsflib::AsciiLower(file.path).ends_with(".baa") && game.read_file(file.path, data) &&
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

    const std::vector<uint32_t> words = profile.z2.Words();
    for (std::size_t i = 0; i < words.size(); i++)
    {
        put(kJAudioZ2Offset + ntdlib::flipper::z2::kProfileOffset + static_cast<uint32_t>(i * 4), words[i]);
    }

    put(kJAudioSleepOffset, profile.sleep);
    put(kJAudioRetraceTicksOffset, kFrameTicks);
    put(kJAudioResetDataOffset, profile.reset_data);
    put(kJAudioResetFlagOffset, profile.reset_flag);

    program.AddMemory(jaudio::kDriverBase, driver);

    // main branches to the driver.
    uint8_t branch[4];
    xsflib::StoreBe32(branch, 0x48000000 | ((jaudio::kDriverEntry - profile.main) & 0x03fffffc));
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

std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& microcode_path, const ntdlib::Microcode& microcode,
                                         nw4r::SndBuild build, const Tags& tags)
{
    ArchiveDescriptor descriptor;
    descriptor.archive_path = archive_path;
    descriptor.microcode_path = microcode_path;
    descriptor.microcode_address = microcode.address;
    descriptor.snd_build = build.date;

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

        return model_.RunFrame(samples_);
    }

    bool Sounded() const
    {
        return dsp_ ? xsflib::rip::Audible(samples_) : model_.engine->Channels().ActiveCount() > 0;
    }

    // The model has no effects.
    bool Ringing() const
    {
        return false;
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
    nw4r::ArchiveModel& model_;
    nw4r::SeqPlayer& player_;
    bool dsp_;
    std::vector<int16_t> samples_;
};

// Measures a sequence on the model (xsflib::rip::MeasureLength). With `dsp` set, the microcode runs on the DSP.
// Otherwise the model runs without the DSP.
std::optional<Timing> MeasureSequence(const ntdlib::rsar::SoundArchive& archive, const ntdlib::Microcode& microcode,
                                      nw4r::SndBuild build, uint32_t sound_index, bool dsp)
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

    nw4r::SeqPlayer* player = model->engine->StartSequence(sound_index);
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
    bool dsp = true;
};

LengthAnalyzer::LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const ntdlib::Microcode& microcode,
                               nw4r::SndBuild build)
    : impl_(new Impl{ntdlib::rsar::SoundArchive::Load(archive_bytes, wsf::nw4r::kRsarOptions), microcode, build})
{
    impl_->dsp = MicrocodeWorks(microcode, impl_->archive);
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    // A sequence that the model can't play, such as one with a damaged bank, gets no length (xsflib::rip::TryMeasure).
    const auto measure = [this, sound_index]
    {
        return MeasureSequence(impl_->archive, impl_->microcode, impl_->build, sound_index, impl_->dsp);
    };
    return xsflib::rip::TryMeasure(measure);
}

} // namespace wsf::rip
