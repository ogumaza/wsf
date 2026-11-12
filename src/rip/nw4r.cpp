// SPDX-License-Identifier: MIT

// The ripper's parts for games whose sound library is NW4R SND: the driver block, library and minis of game mode, the
// library and minis of archive mode, the pieces that play together, and the length analysis on the model of NW4R SND
// and AX.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/nwsnd/length_model.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/rsar/cut.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/snd/arrangement.h"
#include "ntdlib/rvl/snd/effect_setup.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/rvl/snd/sequence.h"
#include "ntdlib/rvl/snd/util.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/sound_pieces.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"

#include "rip/driver_blob.h"
#include "rip/rip.h"
#include "rip/sound_libraries.h"
#include "wsf/format.h"
#include "wsf/models.h"

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

// The effects (see EffectProfile): the functions and the offset of a sound's sends, the number of effects, what a
// .miniwsf patches for its sound (whether the variables pick the presets, the send's bus and the send), the effects and
// their parameter words. The block ends with the build of AXFX and the effects' classes for the player. A .miniwsf
// whose sound gets other effects than the library's patches everything from the number of effects to the end.
constexpr uint32_t kParamsEffectFunctionsOffset = 0xbc;
constexpr uint32_t kParamsEffectCountOffset = DriverBlock::kEffectCountOffset;
constexpr uint32_t kParamsSoundEffectsOffset = 0xe0;
constexpr uint32_t kParamsEffectsOffset = DriverBlock::kEffectsOffset;
constexpr uint32_t kParamsEffectWordsOffset = 0x16c;
constexpr uint32_t kDriverEffectWords = 64;
constexpr uint32_t kParamsEffectsEnd = DriverBlock::kNativeOffset + DriverBlock::kMaxEffects * DriverBlock::kNativeSize;
static_assert(kParamsEffectsOffset + DriverBlock::kMaxEffects * DriverBlock::kEffectSize == kParamsEffectWordsOffset &&
              kParamsEffectWordsOffset + kDriverEffectWords * 4 == DriverBlock::kAxfxBuildOffset);
constexpr uint32_t kNoBus = 0xffffffff;

// The tracks that the game silences on the sound (see EffectProfile): the offsets of the SeqPlayer's track pointers
// in SeqSound and of the volume in SeqTrack, then the tracks, which a .miniwsf patches.
constexpr uint32_t kParamsSeqTracksOffset = kParamsEffectsEnd;
constexpr uint32_t kParamsTrackVolumeOffset = kParamsEffectsEnd + 4;
constexpr uint32_t kParamsSilencedTracksOffset = kParamsEffectsEnd + 8;

// The variables that the game sets on the sound (see EffectProfile): the offset of the SeqPlayer's local variables in
// SeqSound, then the variables that are set, a bit for each, and their values, which a .miniwsf patches. The driver
// writes local variables only.
constexpr uint32_t kParamsSeqVariablesOffset = kParamsEffectsEnd + 12;
constexpr uint32_t kParamsSetVariablesOffset = kParamsEffectsEnd + 16;
constexpr uint32_t kParamsVariablesOffset = kParamsEffectsEnd + 20;
constexpr int kDriverVariables = 16;
static_assert(kParamsVariablesOffset == kParamsSetVariablesOffset + 4);

// The driver's settings besides the length of a frame (kFrameTicks): nw4r::snd's default thread priorities, and room
// for each object that the driver creates.
constexpr uint32_t kSoundThreadPriority = 4;
constexpr uint32_t kDvdThreadPriority = 3;
constexpr uint32_t kObjectSize = 0x2000;

// Room in the sound heap beyond a group's data: the heap's block headers and alignment.
constexpr uint32_t kHeapSlack = 0x10000;

// A piece that a channel plays as a base sequence and a layer: the piece's name and the two sequences' labels. A piece
// with an empty layer is the base alone.
struct LayeredPiece
{
    std::string_view name;
    std::string_view base;
    std::string_view layer;
};

// The Forecast and News channels start their pieces on separate players. A fader sets each piece's volume every frame,
// to targets that each view sets: 0 or 1.
// - The News Channel (USA, version 7) plays NEW_BGM_NEWS (labelled SMF_bgm_news_anytime) in every view but the slide
//   show. The news screens add NEW_BGM_READ (read_only) at 1 (0x80030720). The archive gives NEW_BGM_READ a volume of
//   0. The news screens therefore sound as NEW_BGM_NEWS alone. The globe adds NEW_BGM_CIKYU (cikyu_only) at 1, with
//   READ at 0 (0x80030650). The fader is at 0x8002fb34. The slide show's pieces play alone (0x80030854).
// - The Forecast Channel (USA, version 7) starts WTR_BGM_MORNING_YOHOU and WTR_BGM_MORNING_CIKYU together, and plays
//   both at 1 on the globe (0x8000f4b8). The forecasts take the layer to 0 (0x8000ed20). At night the pieces are
//   WTR_BGM_NIGHT_YOHOU and WTR_BGM_NIGHT_CIKYU. The fader is at 0x8000dec4. The other pieces play alone.
// Each pair has one tempo. A layer never plays without its base.
constexpr std::array<LayeredPiece, 6> kLayeredPieces = {{
    {"NEW_BGM_NEWS", "NEW_BGM_NEWS", ""},
    {"NEW_BGM_NEWS globe", "NEW_BGM_NEWS", "NEW_BGM_CIKYU"},
    {"WTR_BGM_MORNING_YOHOU", "WTR_BGM_MORNING_YOHOU", ""},
    {"WTR_BGM_MORNING_YOHOU globe", "WTR_BGM_MORNING_YOHOU", "WTR_BGM_MORNING_CIKYU"},
    {"WTR_BGM_NIGHT_YOHOU", "WTR_BGM_NIGHT_YOHOU", ""},
    {"WTR_BGM_NIGHT_YOHOU globe", "WTR_BGM_NIGHT_YOHOU", "WTR_BGM_NIGHT_CIKYU"},
}};

} // namespace

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

// Puts effect `i` of the driver's parameter block, `effect`, in `block`, with the functions of its class in
// `profile`. Its parameters, or each of its presets, go in the effects' words from word `first` on. `first` then
// gives the next free word. Returns false if the profile has no class for the effect, the words run out, or the effect
// has presets and the game reads no variables.
bool PutEffect(std::array<uint8_t, kParamsEffectsEnd>& block, const EffectProfile& profile,
               const nw4r::EffectSetup& effect, uint32_t i, uint32_t& first)
{
    const auto put = [&block](uint32_t offset, uint32_t value)
    {
        xsflib::StoreBe32(&block[offset], value);
    };

    const auto found = std::find_if(profile.classes.begin(), profile.classes.end(),
                                    [&effect](const EffectClass& c) { return c.type == effect.type; });
    const auto words = static_cast<uint32_t>(nw4r::EffectParamWords(effect.type));
    const auto sets = static_cast<uint32_t>(std::max<std::size_t>(effect.presets.size(), 1));
    if (found == profile.classes.end() || first + words * sets > kDriverEffectWords ||
        (!effect.presets.empty() && profile.read_variable == 0))
    {
        return false;
    }

    const uint32_t at = kParamsEffectsOffset + i * DriverBlock::kEffectSize;
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

    static_assert(std::size(effect_words) * 4 == DriverBlock::kEffectSize);

    const uint32_t native_at = DriverBlock::kNativeOffset + i * DriverBlock::kNativeSize;
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

    return true;
}

// The driver's parameter block up to the end of the effects, for a sound without a send, with `setups` filled in. They
// are effects of the game with the effect profile `profile`. `sends` says whether the game sets sends on sounds.
// Returns nullopt if the driver can't set the effects up.
std::optional<std::array<uint8_t, kParamsEffectsEnd>> EffectsBlock(const std::optional<EffectProfile>& profile,
                                                                   const std::vector<nw4r::EffectSetup>& setups,
                                                                   bool sends)
{
    std::array<uint8_t, kParamsEffectsEnd> block{};
    const auto put = [&block](uint32_t offset, uint32_t value)
    {
        xsflib::StoreBe32(&block[offset], value);
    };

    put(kParamsSoundEffectsOffset + 4, kNoBus);
    if (setups.empty())
    {
        return block;
    }

    if (!profile || setups.size() > DriverBlock::kMaxEffects)
    {
        return std::nullopt;
    }

    const uint32_t function_words[] = {
        profile->ax_register_callback, profile->ax_manager,      profile->append_effect, profile->clear_effect,
        profile->seq_handle_ctor,      profile->seq_handle_dtor, profile->read_variable, profile->sound_sends};
    for (std::size_t i = 0; i < std::size(function_words); i++)
    {
        put(kParamsEffectFunctionsOffset + static_cast<uint32_t>(i * 4), function_words[i]);
    }

    static_assert(kParamsEffectFunctionsOffset + std::size(function_words) * 4 == kParamsEffectCountOffset);
    put(kParamsEffectCountOffset, static_cast<uint32_t>(setups.size()));
    put(DriverBlock::kAxfxBuildOffset, static_cast<uint32_t>(profile->axfx_build));

    uint32_t first = 0;
    for (std::size_t i = 0; i < setups.size(); i++)
    {
        if (!PutEffect(block, *profile, setups[i], static_cast<uint32_t>(i), first))
        {
            return std::nullopt;
        }
    }

    if (sends && profile->sound_sends == 0)
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

// Puts the functions and the init flag of the game's driver profile `profile` in the driver's block at `params`.
void PutProfileFunctions(uint8_t* params, const GameProfile& profile)
{
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
        xsflib::StoreBe32(params + kParamsProfileOffset + i * 4, functions[i]);
    }

    static_assert(kParamsProfileOffset + std::size(functions) * 4 == kParamsBankGroupOffset);
    xsflib::StoreBe32(params + kParamsInitFlagOffset, profile.init_flag);
}

// Puts the offsets that the driver uses to silence tracks and set variables in the driver's block at `params`, from
// `effect_profile`, when `effects` has tracks or variables. Returns nothing, or the error: the driver can't silence
// the game's tracks or set its variables.
std::optional<std::string> PutTrackAndVariableOffsets(uint8_t* params,
                                                      const std::optional<EffectProfile>& effect_profile,
                                                      const nw4r::ArchiveEffects& effects)
{
    // The tracks that the game silences on some sounds. A .miniwsf gives the driver its sound's (see BuildMini).
    if (!effects.tracks.empty())
    {
        if (!effect_profile || effect_profile->seq_tracks == 0)
        {
            return "the driver can't silence this game's tracks";
        }

        xsflib::StoreBe32(params + kParamsSeqTracksOffset, effect_profile->seq_tracks);
        xsflib::StoreBe32(params + kParamsTrackVolumeOffset, effect_profile->track_volume);
    }

    // The variables that the game sets on some sounds. A .miniwsf gives the driver its sound's (see BuildMini). The
    // driver writes local variables (0 to 15) alone.
    if (!effects.variables.empty())
    {
        const auto local = [](const nw4r::SoundVariables& entry)
        {
            return std::all_of(entry.variables.begin(), entry.variables.end(), [](const auto& variable)
                               { return variable.first >= 0 && variable.first < kDriverVariables; });
        };
        if (!effect_profile || effect_profile->seq_variables == 0 ||
            !std::all_of(effects.variables.begin(), effects.variables.end(), local))
        {
            return "the driver can't set this game's variables";
        }

        xsflib::StoreBe32(params + kParamsSeqVariablesOffset, effect_profile->seq_variables);
    }

    return std::nullopt;
}

// Fills in driver/driver.c's parameters in the driver's block at `params`: the settings, the archive at `archive_path`,
// the game's profile, the effects, the silenced tracks and the offset for the variables. The functions for the effects
// come from `effect_profile`. Returns nothing, or the error: the driver's block doesn't have the layout that the ripper
// expects, or the driver can't set up the game's effects, silence its tracks or set its variables.
std::optional<std::string> PutDriverParams(uint8_t* params, const GameProfile& profile, const std::string& archive_path,
                                           const ntdlib::rsar::SoundArchive& archive,
                                           const std::optional<EffectProfile>& effect_profile,
                                           const nw4r::ArchiveEffects& effects)
{
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

    PutProfileFunctions(params, profile);

    // The driver's layout must be the one that these offsets describe: its block has no bus for the sound's send.
    if (xsflib::LoadBe32(params + kParamsSoundEffectsOffset + 4) != kNoBus)
    {
        return "the driver's parameter block doesn't have the layout that the ripper expects";
    }

    // The driver must be able to set up every effect. A .miniwsf gives the driver the effects of a sound that gets
    // others (see BuildMini). The library holds the functions for the effects, and the effects of a sound that no
    // effect lists.
    const bool sends = !effects.sends.empty();
    const auto all = EffectsBlock(effect_profile, effects.setups, sends);
    const auto unlisted = EffectsBlock(effect_profile, nw4r::EffectsFor(effects.setups, nw4r::kUnlistedSound), sends);
    if (!all || !unlisted)
    {
        return "the driver can't set up this game's effects";
    }

    std::copy(all->begin() + kParamsEffectFunctionsOffset, all->begin() + kParamsEffectCountOffset,
              params + kParamsEffectFunctionsOffset);
    std::copy(unlisted->begin() + kParamsEffectCountOffset, unlisted->end(), params + kParamsEffectCountOffset);

    return PutTrackAndVariableOffsets(params, effect_profile, effects);
}

} // namespace

std::optional<std::vector<uint8_t>> BuildLibrary(const ntdlib::disc::GameFiles& game, const GameProfile& profile,
                                                 const std::string& archive_path,
                                                 const ntdlib::rsar::SoundArchive& archive,
                                                 const std::vector<uint32_t>& indices, const Tags& tags,
                                                 std::string* error, const nw4r::ArchiveEffects& effects)
{
    const std::optional<ntdlib::Dol> dol = ntdlib::Dol::Parse(game.dol);
    if (!dol)
    {
        return LibraryError(error, "main.dol isn't a DOL");
    }
    if (archive_path.size() + 1 >= kParamsPathSize)
    {
        return LibraryError(error, "the sound archive's path is longer than the driver supports");
    }

    // The driver, with its parameters. A .miniwsf fills in the sound. In the archive, only the groups that the driver
    // loads keep their data.
    GameLibrary library{.driver = {std::begin(kDriverBlob), std::end(kDriverBlob)},
                        .base = kDriverBase,
                        .entry = kDriverEntry,
                        .main = profile.main,
                        .ios_version = profile.ios_version,
                        .files = {{archive_path, KeepLoadedGroups(archive, indices)}},
                        .too_large_advice =
                            "A game-mode library keeps the whole sound archive's size. Add --mode archive "
                            "to rip only the data that the sequences play."};
    if (const std::optional<std::string> problem =
            PutDriverParams(library.driver.data() + (kDriverParams - kDriverBase), profile, archive_path, archive,
                            FindEffectProfile(game.dol, game.GameId()), effects))
    {
        return LibraryError(error, *problem);
    }

    return BuildGameLibrary(game, *dol, library, tags, error);
}

std::vector<uint8_t> BuildMini(uint32_t index, const SoundData& data, const std::string& lib_name, Tags tags,
                               const nw4r::ArchiveEffects& effects, const std::optional<EffectProfile>& effect_profile)
{
    uint8_t patch[12];
    xsflib::StoreBe32(&patch[0], index);
    xsflib::StoreBe32(&patch[4], data.group);
    xsflib::StoreBe32(&patch[8], data.heap_size);
    ProgramBuilder program;
    program.AddMemory(DriverBlock::kAddress + kParamsSoundOffset, patch, sizeof(patch));

    // Whether the sound's variables pick the presets, and the send that the game sets on the sound.
    const bool follows = nw4r::FollowsVariables(effects.setups, index);
    const auto send = std::find_if(effects.sends.begin(), effects.sends.end(),
                                   [index](const nw4r::SoundSend& s) { return s.sound == index; });
    uint8_t sound_effects[12];
    xsflib::StoreBe32(&sound_effects[0], follows ? 1 : 0);
    xsflib::StoreBe32(&sound_effects[4], send != effects.sends.end() ? static_cast<uint32_t>(send->bus) : kNoBus);
    xsflib::StoreBe32(&sound_effects[8], send != effects.sends.end() ? std::bit_cast<uint32_t>(send->send) : 0);

    // A sound that gets other effects than the library's patches the block from the number of effects to its end. That
    // covers the words above too.
    std::optional<std::array<uint8_t, kParamsEffectsEnd>> block;
    if (GetsOtherEffects(effects.setups, index))
    {
        block = EffectsBlock(effect_profile, nw4r::EffectsFor(effects.setups, index), !effects.sends.empty());
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

    // The tracks that the game silences on the sound.
    if (const uint16_t tracks = nw4r::TracksOf(effects.tracks, index); tracks != nw4r::kAllTracks)
    {
        uint8_t silenced[4];
        xsflib::StoreBe32(silenced, static_cast<uint16_t>(~tracks));
        program.AddMemory(DriverBlock::kAddress + kParamsSilencedTracksOffset, silenced, sizeof(silenced));
    }

    // The variables that the game sets on the sound: the bits of those that are set, then the 16 values.
    if (const xsflib::psf::Variables variables = nw4r::VariablesOf(effects.variables, index); !variables.empty())
    {
        uint8_t set_variables[4 + kDriverVariables * 2] = {};
        uint32_t set = 0;
        for (const auto& [number, value] : variables)
        {
            if (number >= 0 && number < kDriverVariables)
            {
                set |= 1u << number;
                xsflib::StoreBe16(&set_variables[4 + number * 2], static_cast<uint16_t>(value));
            }
        }

        xsflib::StoreBe32(&set_variables[0], set);
        program.AddMemory(DriverBlock::kAddress + kParamsSetVariablesOffset, set_variables, sizeof(set_variables));
    }

    tags["_lib"] = lib_name;

    return xsflib::psf::WritePsf(kPsfVersionWsf, {}, program.Data(), tags);
}

void AddSoundVariables(const nw4r::ArchiveEffects& effects, uint32_t sound, Tags& tags)
{
    xsflib::psf::Variables variables = ArrangementFromTags(tags).variables;
    variables.merge(nw4r::VariablesOf(effects.variables, sound));
    if (!variables.empty())
    {
        tags[kVariablesTag] = xsflib::psf::FormatVariables(variables);
    }
}

ArchiveDescriptor BuildArchiveDescriptor(const std::string& archive_path, const std::string& microcode_path,
                                         const ntdlib::Microcode& microcode, nw4r::SndBuild build,
                                         const nw4r::ArchiveEffects& effects, uint32_t version)
{
    ArchiveDescriptor descriptor;
    descriptor.version = version;
    descriptor.archive_path = archive_path;
    descriptor.microcode_path = microcode_path;
    descriptor.microcode_address = microcode.address;
    descriptor.snd_build = build.date;
    descriptor.effects = effects;
    descriptor.effects.variants.clear();

    return descriptor;
}

std::vector<uint8_t> BuildArchiveLibrary(const std::string& archive_path, const std::vector<uint8_t>& archive,
                                         const std::string& microcode_path, const ntdlib::Microcode& microcode,
                                         nw4r::SndBuild build, const Tags& tags, const nw4r::ArchiveEffects& effects,
                                         uint32_t version)
{
    const ArchiveDescriptor descriptor =
        BuildArchiveDescriptor(archive_path, microcode_path, microcode, build, effects, version);

    std::vector<uint8_t> words(microcode.words.size() * 2);
    for (std::size_t i = 0; i < microcode.words.size(); i++)
    {
        xsflib::StoreBe16(&words[i * 2], microcode.words[i]);
    }

    ProgramBuilder program;
    program.AddFile(archive_path, archive);
    program.AddFile(microcode_path, words);
    if (const std::optional<std::string> too_large = ProgramTooLarge(program.Data().size()))
    {
        throw xsflib::FormatError(*too_large + " Rip fewer sequences at a time, such as with --bgm or --only.");
    }

    return xsflib::psf::WritePsf(kPsfVersionWsf, descriptor.Serialize(), program.Data(), tags);
}

std::vector<uint8_t> BuildArchiveMini(uint32_t index, const std::string& lib_name, Tags tags)
{
    return BuildArchiveMini(std::vector<uint32_t>{index}, lib_name, std::move(tags));
}

std::vector<uint8_t> BuildArchiveMini(const std::vector<uint32_t>& indices, const std::string& lib_name, Tags tags,
                                      const std::optional<ArchiveDescriptor>& descriptor)
{
    // A list of one sound gives the same chunk as AddSound.
    ProgramBuilder program;
    program.AddSounds(indices);

    tags["_lib"] = lib_name;

    return xsflib::psf::WritePsf(kPsfVersionWsf, descriptor ? descriptor->Serialize() : std::vector<uint8_t>{},
                                 program.Data(), tags);
}

std::vector<uint8_t> KeepLoadedGroups(const ntdlib::rsar::SoundArchive& archive, const std::vector<uint32_t>& indices)
{
    // The groups that the driver loads for the sequences.
    std::vector<bool> loaded(archive.Groups().size());
    for (const uint32_t index : indices)
    {
        if (const std::optional<SoundData> data = FindSoundData(archive, index))
        {
            loaded[data->group] = true;
            loaded[data->bank_group.value_or(data->group)] = true;
        }
    }

    return ntdlib::rsar::KeepGroups(archive, loaded);
}

std::vector<Piece> FindPieces(const ntdlib::rsar::SoundArchive& archive)
{
    std::vector<Piece> pieces = ntdlib::FindSoundPieces(archive, ntdlib::kMegaMan9Channels);

    // The sequences by label. A later sequence with an earlier one's label doesn't replace it.
    const auto& sounds = archive.Sounds();
    std::map<std::string, uint32_t, std::less<>> sequences;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        if (sounds[i].type == ntdlib::rsar::SoundType::kSequence)
        {
            sequences.emplace(sounds[i].name, static_cast<uint32_t>(i));
        }
    }

    // A layered piece requires each of its sequences.
    for (const LayeredPiece& layered : kLayeredPieces)
    {
        const auto base = sequences.find(layered.base);
        const auto layer = sequences.find(layered.layer);
        if (base == sequences.end() || (!layered.layer.empty() && layer == sequences.end()))
        {
            continue;
        }

        Piece piece{std::string(layered.name), {base->second}};
        if (!layered.layer.empty())
        {
            piece.sounds.push_back(layer->second);
        }

        pieces.push_back(std::move(piece));
    }

    const auto by_first_sound = [](const Piece& a, const Piece& b)
    {
        return a.sounds.front() < b.sounds.front();
    };
    std::stable_sort(pieces.begin(), pieces.end(), by_first_sound);

    return pieces;
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
// than silence. Without the DSP, a frame has sound if any note plays. The model also counts each track's passes that
// change the sound, and each start of a track that another track opens again. A looping sequence's fade then waits for
// the tracks that loop at other lengths than the main track (OthersLooped).
class LengthModel
{
public:
    // `tracks` gives the tracks that play, a bit for each (nw4r::Arrangement). The others play at a volume of 0.
    LengthModel(nw4r::ArchiveModel& model, nw4r::SeqPlayer& player, bool dsp, uint16_t tracks)
        : model_(model), player_(player), dsp_(dsp), tracks_(tracks), passes_(*model.engine, player)
    {
    }

    bool RunFrame()
    {
        ++frames_;
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

    // The frames run so far.
    uint64_t Frames() const
    {
        return frames_;
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

    // Sets the hooks for the loops of the analysis and for the passes of every track
    // (ntdlib::nwsnd::TrackPasses::SetHooks).
    template <typename Function>
    void OnLoop(Function function)
    {
        model_.engine->SetLoopHook(std::move(function));
        passes_.SetHooks();
    }

    // Whether every other track that plays has made kLengthLoops passes that changed the sound
    // (ntdlib::nwsnd::TrackPasses::OthersLooped). A track that plays at a volume of 0 doesn't hold the fade back.
    bool OthersLooped(int track) const
    {
        return passes_.OthersLooped(track, tracks_);
    }

private:
    static constexpr int kRingingFrames = 334; // 1 s

    nw4r::ArchiveModel& model_;
    nw4r::SeqPlayer& player_;
    bool dsp_;
    uint16_t tracks_;
    std::vector<int16_t> samples_;
    uint64_t frames_ = 0;
    int ringing_frames_ = 0;

    // On the Wii an open keeps the track's wait (SeqTrack::Open, 0x8003ffcc).
    ntdlib::nwsnd::TrackPasses<nw4r::Engine, nw4r::SeqPlayer, true> passes_;
};

// Measures a sequence in `arrangement` on the model (xsflib::rip::MeasureLength), with the effects of `effects_sound`.
// With `dsp` set, the microcode runs on the DSP. Otherwise the model runs without the DSP. The counted loops in
// `endless_loops` loop for ever.
std::optional<Timing> MeasureSequence(const ntdlib::rsar::SoundArchive& archive, const ntdlib::Microcode& microcode,
                                      nw4r::SndBuild build, uint32_t sound_index, uint32_t effects_sound,
                                      const nw4r::Arrangement& arrangement, const nw4r::ArchiveEffects& effects,
                                      bool dsp, const ntdlib::nwsnd::CountedLoopSet& endless_loops = {})
{
    auto model = std::make_unique<nw4r::ArchiveModel>();
    model->build = build;
    if (dsp)
    {
        if (!model->Start(microcode, archive))
        {
            return std::nullopt;
        }

        // The effects process the microcode's mix of the aux buses. Without the DSP, nothing reaches them.
        model->engine->SetEffects(effects.setups, effects.sends, effects_sound);
    }
    else
    {
        model->StartWithoutDsp(archive);
    }

    model->engine->SetEndlessLoops(endless_loops);
    nw4r::SeqPlayer* player = model->engine->StartSequence(sound_index, arrangement);
    if (!player)
    {
        return std::nullopt;
    }

    LengthModel length_model(*model, *player, dsp, arrangement.tracks);
    const xsflib::rip::LengthRules rules = LengthRules();
    const std::optional<Timing> timing = xsflib::rip::MeasureLength(length_model, rules);

    // A track can repeat its part in a counted loop that the game plays for longer than the limit, such as 127 passes
    // of a whole piece. The loop's passes don't count. The fade then waits for that track until the limit. When the
    // measure reaches the limit with tracks in counted loops, it runs again with each track's outermost loop endless
    // where that loop is a counted one (ntdlib::nwsnd::OutermostCountedLoops). The passes of those loops then count as
    // those of a loop of count 0 do. A track whose outermost loop is endless keeps its inner loops. It then plays as in
    // the game. Up to the limit, the tracks play the same.
    if (timing && endless_loops.empty() && length_model.Frames() >= rules.max_frames)
    {
        if (const auto running = ntdlib::nwsnd::OutermostCountedLoops(*player); !running.empty())
        {
            return MeasureSequence(archive, microcode, build, sound_index, effects_sound, arrangement, effects, dsp,
                                   running);
        }
    }

    return timing;
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
    nw4r::ArchiveEffects effects;
    bool dsp = true;
};

LengthAnalyzer::LengthAnalyzer(const std::vector<uint8_t>& archive_bytes, const ntdlib::Microcode& microcode,
                               nw4r::SndBuild build, const nw4r::Arrangement& arrangement,
                               const nw4r::ArchiveEffects& effects)
    : impl_(new Impl{ntdlib::rsar::SoundArchive::Load(archive_bytes, wsf::nw4r::kRsarOptions), microcode, build,
                     arrangement, effects})
{
    impl_->dsp = MicrocodeWorks(microcode, impl_->archive);
}

LengthAnalyzer::~LengthAnalyzer() = default;

std::optional<Timing> LengthAnalyzer::Analyze(uint32_t sound_index, std::optional<uint32_t> effects_sound) const
{
    // The tracks that the game plays of the sound narrow the arrangement's. The variables that the game sets on the
    // sound fill in those that the arrangement doesn't set.
    nw4r::Arrangement arrangement = impl_->arrangement;
    arrangement.tracks = static_cast<uint16_t>(arrangement.tracks & nw4r::TracksOf(impl_->effects.tracks, sound_index));
    arrangement.variables.merge(nw4r::VariablesOf(impl_->effects.variables, sound_index));

    // A sequence that the model can't play, such as one with a damaged bank, gets no length (xsflib::rip::TryMeasure).
    const uint32_t effects_of = effects_sound.value_or(sound_index);
    const auto measure = [this, sound_index, effects_of, &arrangement]
    {
        return MeasureSequence(impl_->archive, impl_->microcode, impl_->build, sound_index, effects_of, arrangement,
                               impl_->effects, impl_->dsp);
    };
    return xsflib::rip::TryMeasure(measure);
}

} // namespace wsf::rip
