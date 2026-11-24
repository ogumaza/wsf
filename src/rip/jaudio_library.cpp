// SPDX-License-Identifier: MIT

// The library and the minis of a JAudio rip: the drivers' images, with the parameters that each driver's block takes,
// and the .miniwsf of each sound.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/flipper/z2_params.h"
#include "ntdlib/jaudio/pikmin2_pieces.h"
#include "xsflib/binary.h"
#include "xsflib/psf/psf.h"

#include "rip/driver_blob.h"
#include "rip/galaxy2_driver_blob.h"
#include "rip/galaxy_driver_blob.h"
#include "rip/jaudio.h"
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

// The driver's replacements for a game without the functions: the sleep, then the functions for one wave group.
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

// The offset of a profile's parameter words in the driver block.
constexpr uint32_t kJAudioParamsOffset = kJAudioParamsAddress - DriverBlock::kAddress;

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
        put(kJAudioParamsOffset + static_cast<uint32_t>(i * 4), words[i]);
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

} // namespace

std::vector<uint32_t> Pikmin2Parameters(std::vector<uint32_t> words)
{
    ntdlib::jaudio::AppendPikmin2Pieces(words);

    return words;
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
        // Donkey Kong Jungle Beat's driver takes the table of its chord layers after the profile's words.
        std::vector<uint32_t> params = profile.params;
        if (profile.driver == JAudioDriver::kJungleBeat)
        {
            ntdlib::jaudio::AppendChordLayers(profile.chord_layers, params);
        }

        if (block + kJAudioParamsOffset + params.size() * 4 > library.driver.size())
        {
            return LibraryError(error, "the profile has more parameters than its driver's block holds");
        }

        for (std::size_t i = 0; i < params.size(); i++)
        {
            xsflib::StoreBe32(library.driver.data() + block + kJAudioParamsOffset + i * 4, params[i]);
        }
    }

    return BuildGameLibrary(game, *dol, library, tags, error);
}

std::vector<uint8_t> BuildJAudioMini(uint32_t sound_id, std::span<const uint32_t> waves, const std::string& lib_name,
                                     Tags tags, const ntdlib::flipper::z2::PieceState* state, int32_t luigi_word)
{
    ProgramBuilder program;
    for (const ntdlib::flipper::z2::PieceState::Memory& write :
         ntdlib::flipper::z2::MiniWrites(kJAudioZ2Address, sound_id, waves, state))
    {
        program.AddMemory(write.address, write.data.data(), write.data.size());
    }

    if (luigi_word >= 0)
    {
        uint8_t on[4];
        xsflib::StoreBe32(on, 1);
        program.AddMemory(kJAudioParamsAddress + static_cast<uint32_t>(luigi_word) * 4, on, sizeof(on));
    }

    tags["_lib"] = lib_name;

    return xsflib::psf::WritePsf(kPsfVersionWsf, {}, program.Data(), tags);
}

} // namespace wsf::rip
