// SPDX-License-Identifier: MIT

// WSF ripper (see rip.h).

#include "rip/rip.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ax/microcode.h"
#include "ax/pb.h"
#include "ax/system.h"
#include "common/ascii.h"
#include "common/bytes.h"
#include "common/dol.h"
#include "common/sha1.h"
#include "nw4r/engine.h"
#include "nw4r/sequence.h"
#include "rip/driver_blob.h"
#include "rsar/formats.h"
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
// for each object that the driver creates. The time base runs at a quarter of the 243 MHz bus clock.
constexpr uint32_t kFrameTicks = 243'000'000 / 4 / 60;
constexpr uint32_t kSoundThreadPriority = 4;
constexpr uint32_t kDvdThreadPriority = 3;
constexpr uint32_t kObjectSize = 0x2000;

// Room in the sound heap beyond a group's data: the heap's block headers and alignment.
constexpr uint32_t kHeapSlack = 0x10000;

// The length analysis counts in 3 ms AX frames. It gives two loops and a 10 s fade, or the end, for at most 600 s. A
// sequence that stays busy without a sound for 180 s has ended. No music rests that long. Some sound effects come
// close.
constexpr int kLoops = 2;
constexpr int kFadeSeconds = 10;
constexpr int kMaxSeconds = 600;
constexpr int kSilentSeconds = 180;
constexpr double kFrameMs = 3.0;
constexpr uint64_t kMaxFrames = kMaxSeconds * 1000 / 3;
constexpr uint64_t kSilentFrames = kSilentSeconds * 1000 / 3;

// The loudest sample that still counts as silence: 8 of 32,767, or -72 dB. That's far below anything audible. A note
// can hold at such a level forever after it has died away.
constexpr int kSilenceLevel = 8;

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
        const uint32_t word = MaskedWord(LoadBe32(&code[i]));
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

std::optional<GameProfile> FindProfile(const disc::GameFiles& game)
{
    const std::string sha1 = Sha1::Hex(Sha1::Of(game.dol));
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
    const std::optional<Dol> dol = Dol::Parse(dol_bytes);
    if (!dol)
    {
        return std::nullopt;
    }

    // The places whose code matches each function's signature.
    std::array<std::vector<uint32_t>, kProfileFunctions> places;
    for (const Dol::Section& section : dol->Sections())
    {
        for (std::size_t at = 0; at + 4 <= section.data.size(); at += 4)
        {
            const uint32_t first = MaskedWord(LoadBe32(&section.data[at]));
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

std::optional<SoundData> FindSoundData(const rsar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size())
    {
        return std::nullopt;
    }

    const rsar::SoundInfo& sound = archive.Sounds()[index];
    if (sound.bank_id >= archive.Banks().size())
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
            const rsar::SoundArchive::Group& group = archive.Groups()[g];
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
    const uint32_t bank_file = archive.Banks()[sound.bank_id].file_id;
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

std::optional<std::vector<uint8_t>> BuildLibrary(const disc::GameFiles& game, const GameProfile& profile,
                                                 const std::string& archive_path, const rsar::SoundArchive& archive,
                                                 const Tags& tags, std::string* error)
{
    const auto fail = [error](const std::string& message) -> std::optional<std::vector<uint8_t>>
    {
        if (error)
        {
            *error = message;
        }

        return std::nullopt;
    };

    const std::optional<Dol> dol = Dol::Parse(game.dol);
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
    for (const Dol::Section& section : dol->Sections())
    {
        program.AddMemory(section.address, section.data.data(), section.data.size());
    }

    // The driver, with its parameters: the archive and the game's profile. A .miniwsf fills in the sound.
    std::vector<uint8_t> driver(std::begin(kDriverBlob), std::end(kDriverBlob));
    uint8_t* params = driver.data() + (kDriverParams - kDriverBase);
    const auto put = [params](uint32_t offset, uint32_t value)
    {
        StoreBe32(params + offset, value);
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
    StoreBe32(branch, 0x48000000 | ((kDriverEntry - profile.main) & 0x03fffffc));
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
    d.ios_version = game.tmd.size() >= 0x18c ? LoadBe32(&game.tmd[0x188]) : profile.ios_version;

    return WritePsf(kPsfVersionWsf, d.Serialize(), program.Data(), tags);
}

std::vector<uint8_t> BuildMini(uint32_t index, const SoundData& data, const std::string& lib_name, Tags tags)
{
    uint8_t patch[12];
    StoreBe32(&patch[0], index);
    StoreBe32(&patch[4], data.group);
    StoreBe32(&patch[8], data.heap_size);
    ProgramBuilder program;
    program.AddMemory(DriverBlock::kAddress + kParamsSoundOffset, patch, sizeof(patch));
    if (data.bank_group)
    {
        uint8_t bank_group[4];
        StoreBe32(bank_group, *data.bank_group);
        program.AddMemory(DriverBlock::kAddress + kParamsBankGroupOffset, bank_group, sizeof(bank_group));
    }

    tags["_lib"] = lib_name;

    return WritePsf(kPsfVersionWsf, {}, program.Data(), tags);
}

std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& microcode_path, const ax::Microcode& microcode,
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
        StoreBe16(&words[i * 2], microcode.words[i]);
    }

    ProgramBuilder program;
    program.AddFile(archive_path, archive);
    program.AddFile(microcode_path, words);

    return WritePsf(kPsfVersionWsf, descriptor.Serialize(), program.Data(), tags);
}

std::vector<uint8_t> BuildArchiveMini(uint32_t index, const std::string& lib_name, Tags tags)
{
    ProgramBuilder program;
    program.AddSound(index);

    tags["_lib"] = lib_name;

    return WritePsf(kPsfVersionWsf, {}, program.Data(), tags);
}

std::vector<std::string> MissingData(const rsar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size())
    {
        return {"the sound itself"};
    }

    const rsar::SoundInfo& sound = archive.Sounds()[index];
    std::vector<std::string> missing;
    if (!archive.File(sound.file_id))
    {
        missing.push_back("its sequence data");
    }

    if (sound.bank_id >= archive.Banks().size())
    {
        missing.push_back("its bank");
    }
    else if (!archive.File(archive.Banks()[sound.bank_id].file_id))
    {
        const std::string& name = archive.Banks()[sound.bank_id].name;
        missing.push_back("bank " + (name.empty() ? "#" + std::to_string(sound.bank_id) : name));
    }

    return missing;
}

bool IsMusic(const rsar::SoundArchive& archive, uint32_t index)
{
    if (index >= archive.Sounds().size() || archive.Sounds()[index].type != rsar::SoundType::kSequence)
    {
        return false;
    }

    const rsar::SoundInfo& sound = archive.Sounds()[index];
    if (AsciiLower(sound.name).find("bgm") != std::string::npos)
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
    const std::string player = AsciiLower(archive.Players()[sound.player_id].name);
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

// Measures a sequence on the model. With `dsp` set, the microcode runs on the DSP. Otherwise the model runs without the
// DSP. The sequence plays until it loops kLoops times, ends or falls silent for good, or for kMaxSeconds.
std::optional<Timing> MeasureSequence(const rsar::SoundArchive& archive, const ax::Microcode& microcode,
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

    nw4r::Engine& engine = *model->engine;
    nw4r::SeqPlayer* player = engine.StartSequence(sound_index);
    if (!player)
    {
        return std::nullopt;
    }

    // A sequence that never makes a sound is silent and gets only the tail, however it ends or loops. It may still play
    // notes that make no sound.
    bool sounded = false;
    const auto heard = [&sounded](Timing timing)
    {
        return sounded ? timing : Timing{500, 0, true};
    };

    // Loops that go round before the first sound don't count. An example is a track that changes a value over and over
    // while a note fades in or before another track starts. A loop of another track, or one that starts elsewhere,
    // starts the count again. The loops counted so far don't repeat what plays now. That happens when track 0 makes a
    // pass that changes the sound before a later track plays the music, or when a track moves on to the loop that it
    // plays for good.
    int loops_seen = 0;
    std::pair<int, uint32_t> loop{-1, 0};
    uint64_t frame = 0;
    uint64_t first_loop_frame = 0;
    uint64_t loop_frame = 0;
    player->on_loop = [&](int track, uint32_t target)
    {
        if (!sounded)
        {
            return;
        }

        if (std::pair{track, target} != loop)
        {
            loops_seen = 0;
            loop = {track, target};
        }

        loops_seen++;
        if (loops_seen == 1)
        {
            first_loop_frame = frame;
        }

        if (loops_seen == kLoops)
        {
            loop_frame = frame;
        }
    };

    // A sequence is steady when each open track waits for notes that never end (held notes on looping waves), will
    // never continue, or won't continue before the game stops the sound. A track may also poll a variable once a tick
    // and change nothing (SeqTrack::IsStill). Nothing else may change either. No note's envelope may be in its attack,
    // hold, decay or release. A released note ends by itself. Its track can then continue. No track's volume or pan may
    // be moving. A steady sequence plays on unchanged apart from the positions of the waves. It loops when the waves
    // do. The code follows each running voice's passes back to its wave's loop start in every frame. A steady sequence
    // has looped when every voice has passed kLoops times since the sound last changed. A track's change, a moving
    // envelope or value, and a voice that starts or stops all change the sound. A track that polls may only show that
    // it's still after a long pass. The waves' passes during that pass count.
    std::array<uint32_t, ax::kVoiceCount> positions{};
    std::array<bool, ax::kVoiceCount> running{};
    std::array<bool, ax::kVoiceCount> looping{};
    std::array<std::array<uint64_t, kLoops>, ax::kVoiceCount> passes{}; // each voice's latest passes, the newest first
    uint64_t changed = 0;                                               // the frame of the sound's last change
    uint64_t change_order = 0;                                          // the player's ChangedAt in that frame
    const auto follow_sound = [&]
    {
        for (uint32_t v = 0; v < model->ax.VoiceCount(); v++)
        {
            const ax::Pb pb = model->ax.ReadPb(v);
            const bool runs = pb.state == static_cast<uint16_t>(ax::VoiceState::kRun);
            const uint32_t position = ax::Join(pb.addr.current_address_hi, pb.addr.current_address_lo);
            if (runs != running[v])
            {
                changed = frame;
            }
            else if (runs && position < positions[v])
            {
                std::copy_backward(passes[v].begin(), passes[v].end() - 1, passes[v].end());
                passes[v][0] = frame;
            }

            running[v] = runs;
            looping[v] = pb.addr.loop_flag != 0;
            positions[v] = position;
        }

        if (engine.Channels().AnyChanging() || player->IsMoving() || player->ChangedAt() != change_order)
        {
            changed = frame;
        }

        change_order = player->ChangedAt();
    };

    const auto waves_looped = [&]
    {
        bool any = false;
        for (uint32_t v = 0; v < model->ax.VoiceCount(); v++)
        {
            if (!running[v])
            {
                continue;
            }

            if (!looping[v] || passes[v].back() <= changed)
            {
                return false;
            }

            any = true;
        }

        return any;
    };

    std::vector<int16_t> samples;
    uint64_t idle = 0;
    uint64_t last_sound = 0;
    for (; frame < kMaxFrames; frame++)
    {
        samples.clear();
        if (!dsp)
        {
            model->RunFrameWithoutDsp();
        }
        else if (!model->RunFrame(samples))
        {
            return std::nullopt;
        }

        // The frame has sound if it mixed anything louder than silence. Without the DSP, it has sound if any note
        // plays.
        const auto audible = [](int16_t sample)
        {
            return sample > kSilenceLevel || sample < -kSilenceLevel;
        };

        const bool sound =
            dsp ? std::any_of(samples.begin(), samples.end(), audible) : engine.Channels().ActiveCount() > 0;
        if (sound)
        {
            sounded = true;
            last_sound = frame;
        }

        // A loop that completed without a sound isn't music. An example is a wait that a track repeats once its notes
        // have ended. The sequence ended at its last sound.
        if (loops_seen >= kLoops && last_sound < first_loop_frame)
        {
            return heard(Timing{static_cast<long long>(static_cast<double>(last_sound + 1) * kFrameMs + 500.5), 0});
        }

        if (loops_seen >= kLoops)
        {
            return heard(Timing{static_cast<long long>(static_cast<double>(loop_frame) * kFrameMs + 0.5),
                                static_cast<long long>(kFadeSeconds * 1000)});
        }

        follow_sound();
        const bool steady = player->IsActive() && player->IsSettled() && changed < frame;
        if (steady && waves_looped())
        {
            // A sequence that has made no sound since its sound last changed ended at its last sound. A held note at a
            // volume of 0 is an example.
            if (last_sound < changed)
            {
                return heard(Timing{static_cast<long long>(static_cast<double>(last_sound + 1) * kFrameMs + 500.5), 0});
            }

            return heard(Timing{static_cast<long long>(static_cast<double>(frame) * kFrameMs + 0.5),
                                static_cast<long long>(kFadeSeconds * 1000)});
        }

        // A sequence that stays busy without a sound for kSilentSeconds ended where it fell silent. An example is music
        // that has ended while a track without notes runs on.
        if (!sound && engine.IsBusy() && frame - last_sound >= kSilentFrames)
        {
            break;
        }

        // A sequence that will never change again, or won't before the game stops it, has ended when its last note has.
        // Its tracks may wait forever or poll a variable that only the game sets.
        const bool stuck = player->IsActive() && player->IsStuck() && engine.Channels().ActiveCount() == 0;
        if (!engine.IsBusy() || stuck)
        {
            if (++idle > 10)
            {
                break;
            }
        }
        else
        {
            idle = 0;
        }
    }
    if (frame >= kMaxFrames)
    {
        return heard(Timing{static_cast<long long>(kMaxSeconds * 1000), static_cast<long long>(kFadeSeconds * 1000)});
    }

    // The sequence ended on its own. Its length runs to the end of its last sound's frame plus a short tail, with no
    // fade. Every path that ends a sequence counts from the end of that frame. Only silence follows the last sound. The
    // sequence may take a long time to end: a track may hold a note at a volume of 0 for minutes.
    return heard(Timing{static_cast<long long>(static_cast<double>(last_sound + 1) * kFrameMs + 500.5), 0});
}

} // namespace

bool MicrocodeWorks(const ax::Microcode& microcode, const rsar::SoundArchive& archive)
{
    auto model = std::make_unique<nw4r::ArchiveModel>();
    std::vector<int16_t> samples;

    return model->Start(microcode, archive) && model->RunFrame(samples);
}

struct LengthAnalyzer::Impl
{
    rsar::SoundArchive archive;
    ax::Microcode microcode;
    nw4r::SndBuild build;
    bool dsp = true;
};

LengthAnalyzer::LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const ax::Microcode& microcode,
                               nw4r::SndBuild build)
    : impl_(new Impl{rsar::SoundArchive::Load(archive_bytes), microcode, build})
{
    impl_->dsp = MicrocodeWorks(microcode, impl_->archive);
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index) const
{
    // A sequence that the model can't play, such as one with a damaged bank, gets no length. Running out of memory
    // isn't caused by the sequence. std::bad_alloc goes to the caller.
    try
    {
        return MeasureSequence(impl_->archive, impl_->microcode, impl_->build, sound_index, impl_->dsp);
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

namespace
{

// ASCII letters, digits, '_', '-' and '.' are kept. Everything else, including spaces, becomes '_'. Unlike
// std::isalnum, this doesn't depend on the C locale.
std::string SafeName(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                          c == '-' || c == '.';
        out += keep ? c : '_';
    }

    return out;
}

// Windows reserves these device names, whatever follows the first dot of a file name.
bool ReservedOnWindows(const std::string& stem)
{
    const std::string name = AsciiLower(stem);
    const bool numbered =
        name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) && name[3] >= '0' && name[3] <= '9';
    return name == "con" || name == "prn" || name == "aux" || name == "nul" || numbered;
}

} // namespace

std::string FileNames::Library(const std::string& base)
{
    std::string name = SafeName(base);
    const std::size_t stem_length = std::min(name.find('.'), name.size());
    if (ReservedOnWindows(name.substr(0, stem_length)))
    {
        name.insert(stem_length, "_");
    }

    return Unique(name, ".wsflib");
}

std::string FileNames::Mini(uint32_t index, const std::string& label)
{
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%04u ", static_cast<unsigned>(index));

    return Unique(prefix + SafeName(label), ".miniwsf");
}

std::string FileNames::Unique(const std::string& stem, const std::string& extension)
{
    std::string name = stem + extension;
    for (int n = 2; !taken_.insert(AsciiLower(name)).second; n++)
    {
        name = stem + "_" + std::to_string(n) + extension;
    }

    return name;
}

} // namespace wsf::rip
