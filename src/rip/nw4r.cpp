// SPDX-License-Identifier: MIT

// The ripper's parts for games whose sound library is NW4R SND: game mode's driver block, library and minis, archive
// mode's library and minis with the data that they keep, the checks of an archive's sounds, and the length analysis on
// WSF's model of NW4R SND and AX.

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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/reference.h"
#include "ntdlib/sound_build.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"
#include "xsflib/sha1.h"

#include "ax/pb.h"
#include "ax/system.h"
#include "nw4r/arrangement.h"
#include "nw4r/effect_setup.h"
#include "nw4r/engine.h"
#include "nw4r/rsar_options.h"
#include "nw4r/sequence.h"
#include "nw4r/util.h"
#include "rip/driver_blob.h"
#include "rip/rip.h"
#include "rip/sound_libraries.h"
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

// The driver's settings besides the length of a frame (kFrameTicks): nw4r::snd's default thread priorities, and room
// for each object that the driver creates.
constexpr uint32_t kSoundThreadPriority = 4;
constexpr uint32_t kDvdThreadPriority = 3;
constexpr uint32_t kObjectSize = 0x2000;

// Room in the sound heap beyond a group's data: the heap's block headers and alignment.
constexpr uint32_t kHeapSlack = 0x10000;

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

// The driver's parameter block up to the end of the effects, for a sound without a send, with `setups` filled in. They
// are effects of the game whose main.dol has `dol_sha1`. `sends` says whether the game sets sends on sounds. Returns
// nullopt if the driver can't set the effects up.
std::optional<std::array<uint8_t, kParamsEffectsEnd>> EffectsBlock(const std::string& dol_sha1,
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

    const EffectProfile* profile = FindEffectProfile(dol_sha1);
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
        const nw4r::EffectSetup& effect = setups[i];
        const auto found = std::find_if(profile->classes.begin(), profile->classes.end(),
                                        [&effect](const EffectClass& c) { return c.type == effect.type; });
        const auto words = static_cast<uint32_t>(nw4r::EffectParamWords(effect.type));
        const auto sets = static_cast<uint32_t>(std::max<std::size_t>(effect.presets.size(), 1));
        if (found == profile->classes.end() || first + words * sets > kDriverEffectWords ||
            (!effect.presets.empty() && profile->read_variable == 0))
        {
            return std::nullopt;
        }

        const uint32_t at = kParamsEffectsOffset + static_cast<uint32_t>(i) * DriverBlock::kEffectSize;
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

// Fills in driver/driver.c's parameters in the driver's block at `params`: the settings, the archive at `archive_path`,
// the game's profile, the effects and the silenced tracks. Returns nothing, or the error: the driver's block doesn't
// have the layout that the ripper expects, or the driver can't set up the game's effects or silence its tracks.
std::optional<std::string> PutDriverParams(uint8_t* params, const GameProfile& profile, const std::string& archive_path,
                                           const ntdlib::rsar::SoundArchive& archive, const std::string& dol_sha1,
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

    // The driver's layout must be the one that these offsets describe: its block has no bus for the sound's send.
    if (xsflib::LoadBe32(params + kParamsSoundEffectsOffset + 4) != kNoBus)
    {
        return "the driver's parameter block doesn't have the layout that the ripper expects";
    }

    // The driver must be able to set up every effect. A .miniwsf gives the driver the effects of a sound that gets
    // others (see BuildMini). The library holds the functions for the effects, and the effects of a sound that no
    // effect lists.
    const bool sends = !effects.sends.empty();
    const auto all = EffectsBlock(dol_sha1, effects.setups, sends);
    const auto unlisted = EffectsBlock(dol_sha1, nw4r::EffectsFor(effects.setups, nw4r::kUnlistedSound), sends);
    if (!all || !unlisted)
    {
        return "the driver can't set up this game's effects";
    }

    std::copy(all->begin() + kParamsEffectFunctionsOffset, all->begin() + kParamsEffectCountOffset,
              params + kParamsEffectFunctionsOffset);
    std::copy(unlisted->begin() + kParamsEffectCountOffset, unlisted->end(), params + kParamsEffectCountOffset);

    // The tracks that the game silences on some sounds. A .miniwsf gives the driver its sound's (see BuildMini).
    if (!effects.tracks.empty())
    {
        const EffectProfile* effect_profile = FindEffectProfile(dol_sha1);
        if (!effect_profile || effect_profile->seq_tracks == 0)
        {
            return "the driver can't silence this game's tracks";
        }

        put(kParamsSeqTracksOffset, effect_profile->seq_tracks);
        put(kParamsTrackVolumeOffset, effect_profile->track_volume);
    }

    return std::nullopt;
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
                            xsflib::Sha1::Hex(xsflib::Sha1::Of(game.dol)), effects))
    {
        return LibraryError(error, *problem);
    }

    return BuildGameLibrary(game, *dol, library, tags, error);
}

std::vector<uint8_t> BuildMini(uint32_t index, const SoundData& data, const std::string& lib_name, Tags tags,
                               const nw4r::ArchiveEffects& effects, const std::string& dol_sha1)
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
        block = EffectsBlock(dol_sha1, nw4r::EffectsFor(effects.setups, index), !effects.sends.empty());
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

    tags["_lib"] = lib_name;

    return xsflib::psf::WritePsf(kPsfVersionWsf, {}, program.Data(), tags);
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

namespace
{

// An RSAR's FILE block keeps each file at a multiple of 32 bytes.
constexpr std::size_t kFileAlignment = 0x20;

// The largest archive whose offsets fit RSAR's 32-bit fields.
constexpr std::size_t kMaxArchiveSize = 0xFFFFFFFF;

// The bytes at the archive's start that DvdSoundArchive::LoadFileHeader reads: the header's 0x28 bytes, rounded up to a
// multiple of 32.
constexpr uint64_t kHeaderRead = 0x40;

// Pads `data` with zeros to a multiple of kFileAlignment.
void PadToFileAlignment(std::vector<uint8_t>& data)
{
    data.resize((data.size() + kFileAlignment - 1) / kFileAlignment * kFileAlignment, 0);
}

// The fields of an RSAR that KeepSequenceData and KeepLoadedGroups read, by their offsets. KeepSequenceData rewrites
// some of them.
struct RsarLayout
{
    // The header: the archive's size, the INFO block's offset and size, and the FILE block's offset and size.
    static constexpr std::size_t kArchiveSize = 0x08;
    static constexpr std::size_t kInfoBlock = 0x18;
    static constexpr std::size_t kInfoBlockSize = 0x1c;
    static constexpr std::size_t kFileBlock = 0x20;
    static constexpr std::size_t kFileBlockSize = 0x24;

    // A block's header: its magic, then its size. The INFO block's references count from the end of its header.
    static constexpr std::size_t kBlockSize = 0x04;
    static constexpr std::size_t kBlockHeader = 0x08;

    // A reference (ntdlib::rsar::Reference), and the reference to the groups' table in the INFO block. A table holds
    // a count, then a reference to each entry.
    static constexpr std::size_t kReference = 0x08;
    static constexpr std::size_t kGroupTable = 0x20;

    // A group's entry: the offset and size of its files in the archive, the same for their wave data, the reference
    // to the table of its items, and the size of the entry.
    static constexpr std::size_t kGroupOffset = 0x10;
    static constexpr std::size_t kGroupSize = 0x14;
    static constexpr std::size_t kGroupWaveDataOffset = 0x18;
    static constexpr std::size_t kGroupWaveDataSize = 0x1c;
    static constexpr std::size_t kGroupItems = 0x20;
    static constexpr std::size_t kGroupEntrySize = 0x28;

    // An item's entry: the offset of its file from its group's files, that of its wave data from the group's wave
    // data, and the size of the entry.
    static constexpr std::size_t kItemOffset = 0x04;
    static constexpr std::size_t kItemWaveDataOffset = 0x0c;
    static constexpr std::size_t kItemEntrySize = 0x14;
};

// Where an archive's INFO block holds a group's entry and its items' entries (SoundArchiveFileReader::ReadGroupInfo,
// main.dol 0x80042ac4, and ReadGroupItemInfo, 0x80042bd4).
struct GroupEntries
{
    std::size_t group = 0;
    std::vector<std::size_t> items;
};

// The groups' entries, in the order of SoundArchive::Groups. The INFO block's payload starts with references to its
// tables. The groups' table is the fifth. A table holds a count, then a reference to each entry. The references count
// from the payload.
std::vector<GroupEntries> FindGroupEntries(const xsflib::Reader& r)
{
    const std::size_t base = std::size_t{r.U32(RsarLayout::kInfoBlock)} + RsarLayout::kBlockHeader;
    const auto entries = [&r, base](std::size_t table_ref)
    {
        std::vector<std::size_t> found;
        const ntdlib::rsar::Reference table = ntdlib::rsar::ReadRef(r, table_ref);
        if (table.IsOffset())
        {
            const std::size_t at = base + table.offset;
            const uint32_t count = r.U32(at);
            for (uint32_t i = 0; i < count; i++)
            {
                found.push_back(base + ntdlib::rsar::ReadRef(r, at + 4 + i * RsarLayout::kReference).offset);
            }
        }

        return found;
    };

    std::vector<GroupEntries> groups;
    for (const std::size_t group : entries(base + RsarLayout::kGroupTable))
    {
        groups.push_back({group, entries(group + RsarLayout::kGroupItems)});
    }

    return groups;
}

// True if `entries` hold each of `groups` and its items, and lie before the FILE block at `file_block`.
bool EntriesBeforeFiles(const std::vector<GroupEntries>& entries,
                        const std::vector<ntdlib::rsar::SoundArchive::Group>& groups, std::size_t file_block)
{
    if (entries.size() != groups.size())
    {
        return false;
    }

    for (std::size_t g = 0; g < groups.size(); g++)
    {
        if (entries[g].items.size() != groups[g].items.size() ||
            entries[g].group + RsarLayout::kGroupEntrySize > file_block)
        {
            return false;
        }

        for (const std::size_t item : entries[g].items)
        {
            if (item + RsarLayout::kItemEntrySize > file_block)
            {
                return false;
            }
        }
    }

    return true;
}

// The files that the model reads for the sequences at `indices`: each sequence's file, and its bank's file and wave
// data (Engine::StartSequence and Engine::GetBank).
std::vector<uint32_t> SequenceFiles(const ntdlib::rsar::SoundArchive& archive, const std::vector<uint32_t>& indices)
{
    std::vector<uint32_t> files;
    for (const uint32_t index : indices)
    {
        if (index >= archive.Sounds().size())
        {
            continue;
        }

        const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[index];
        files.push_back(sound.file_id);
        if (sound.bank < archive.Banks().size())
        {
            files.push_back(archive.Banks()[sound.bank].file_id);
        }
    }

    return files;
}

// For each group, which of its items SoundArchive::File reads for `files`. File returns spans into the archive's bytes.
// An item whose file and wave data have those offsets and sizes holds them. Like File, the test ignores the item's
// file ID.
std::vector<std::vector<bool>> ItemsRead(const ntdlib::rsar::SoundArchive& archive, const std::vector<uint32_t>& files)
{
    const std::vector<ntdlib::rsar::SoundArchive::Group>& groups = archive.Groups();
    std::vector<std::vector<bool>> read(groups.size());
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        read[g].resize(groups[g].items.size());
    }

    const auto offset = [start = archive.Bytes().data()](std::span<const uint8_t> data)
    {
        return static_cast<uint64_t>(data.data() - start);
    };

    for (const uint32_t file_id : files)
    {
        const std::optional<ntdlib::rsar::FileData> file = archive.File(file_id);
        for (std::size_t g = 0; file && g < groups.size(); g++)
        {
            for (std::size_t i = 0; i < groups[g].items.size(); i++)
            {
                const ntdlib::rsar::SoundArchive::Group::Item& item = groups[g].items[i];
                read[g][i] = read[g][i] ||
                             (groups[g].internal && uint64_t{groups[g].offset} + item.offset == offset(file->file) &&
                              item.size == file->file.size() &&
                              uint64_t{groups[g].wave_data_offset} + item.wave_data_offset == offset(file->wave_data) &&
                              item.wave_data_size == file->wave_data.size());
            }
        }
    }

    return read;
}

// Where a group's kept items lie in the copy that KeepSequenceData makes: the group's files, then their wave data.
struct GroupPlace
{
    std::size_t offset = 0, size = 0, wave_data_offset = 0, wave_data_size = 0;
    std::vector<std::size_t> files, waves; // each kept item's offsets, from the group's two offsets
};

// Appends each group's kept items to `copy`: their files, then their wave data, each at a multiple of 32 bytes. Items
// of a group that hold the same bytes share one copy of them. Banks often share a wave archive. A group outside the
// archive keeps its place.
std::vector<GroupPlace> AppendKeptItems(const ntdlib::rsar::SoundArchive& archive,
                                        const std::vector<std::vector<bool>>& kept, std::vector<uint8_t>& copy)
{
    // The offsets and sizes of a group's bytes that the copy holds, and where each copy starts from the group's offset.
    using Copies = std::map<std::pair<uint64_t, uint32_t>, std::size_t>;

    // Appends the bytes at `offset` in the archive, unless `copies` holds them already. Returns where they start from
    // `base` in the copy.
    const std::span<const uint8_t> all = archive.Bytes();
    const auto append = [&all, &copy](Copies& copies, uint64_t offset, uint32_t size, std::size_t base)
    {
        const auto [at, added] = copies.try_emplace({offset, size}, 0);
        if (added)
        {
            PadToFileAlignment(copy);
            at->second = copy.size() - base;
            const std::span<const uint8_t> data = all.subspan(static_cast<std::size_t>(offset), size);
            copy.insert(copy.end(), data.begin(), data.end());
        }

        return at->second;
    };

    const std::vector<ntdlib::rsar::SoundArchive::Group>& groups = archive.Groups();
    std::vector<GroupPlace> places(groups.size());
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        if (!groups[g].internal)
        {
            continue;
        }

        const ntdlib::rsar::SoundArchive::Group& group = groups[g];
        GroupPlace& place = places[g];
        place.files.resize(group.items.size());
        place.waves.resize(group.items.size());
        PadToFileAlignment(copy);
        place.offset = copy.size();
        Copies files;
        for (std::size_t i = 0; i < group.items.size(); i++)
        {
            if (kept[g][i])
            {
                place.files[i] =
                    append(files, uint64_t{group.offset} + group.items[i].offset, group.items[i].size, place.offset);
            }
        }

        place.size = copy.size() - place.offset;
        PadToFileAlignment(copy);
        place.wave_data_offset = copy.size();
        Copies waves;
        for (std::size_t i = 0; i < group.items.size(); i++)
        {
            if (kept[g][i])
            {
                place.waves[i] = append(waves, uint64_t{group.wave_data_offset} + group.items[i].wave_data_offset,
                                        group.items[i].wave_data_size, place.wave_data_offset);
            }
        }

        place.wave_data_size = copy.size() - place.wave_data_offset;
    }

    PadToFileAlignment(copy);

    return places;
}

// Writes the copy's size into its header and its FILE block's header, and each group's place and each item's into its
// INFO block. An item that isn't kept starts at the copy's end.
void WritePlaces(const ntdlib::rsar::SoundArchive& archive, const std::vector<std::vector<bool>>& kept,
                 const std::vector<GroupEntries>& entries, const std::vector<GroupPlace>& places,
                 std::size_t file_block, std::vector<uint8_t>& copy)
{
    const auto put = [&copy](std::size_t at, std::size_t value)
    {
        xsflib::StoreBe32(&copy[at], static_cast<uint32_t>(value));
    };

    const std::size_t end = copy.size();
    put(RsarLayout::kArchiveSize, end);
    put(RsarLayout::kFileBlockSize, end - file_block);
    put(file_block + RsarLayout::kBlockSize, end - file_block);
    for (std::size_t g = 0; g < places.size(); g++)
    {
        if (!archive.Groups()[g].internal)
        {
            continue;
        }

        const GroupPlace& place = places[g];
        put(entries[g].group + RsarLayout::kGroupOffset, place.offset);
        put(entries[g].group + RsarLayout::kGroupSize, place.size);
        put(entries[g].group + RsarLayout::kGroupWaveDataOffset, place.wave_data_offset);
        put(entries[g].group + RsarLayout::kGroupWaveDataSize, place.wave_data_size);
        for (std::size_t i = 0; i < entries[g].items.size(); i++)
        {
            put(entries[g].items[i] + RsarLayout::kItemOffset, kept[g][i] ? place.files[i] : end - place.offset);
            put(entries[g].items[i] + RsarLayout::kItemWaveDataOffset,
                kept[g][i] ? place.waves[i] : end - place.wave_data_offset);
        }
    }
}

// Whether two files hold the same bytes, wave data included, or neither is there.
bool SameFile(const std::optional<ntdlib::rsar::FileData>& a, const std::optional<ntdlib::rsar::FileData>& b)
{
    return a.has_value() == b.has_value() &&
           (!a || (std::ranges::equal(a->file, b->file) && std::ranges::equal(a->wave_data, b->wave_data)));
}

} // namespace

std::vector<uint8_t> KeepSequenceData(const ntdlib::rsar::SoundArchive& archive, const std::vector<uint32_t>& indices)
{
    // The copy keeps the archive up to its files: the header, the SYMB and INFO blocks and the FILE block's header. The
    // groups' entries must lie there.
    const std::span<const uint8_t> all = archive.Bytes();
    const xsflib::Reader r(all, true);
    const std::size_t file_block = r.U32(RsarLayout::kFileBlock);
    const std::size_t files_start = file_block + RsarLayout::kBlockHeader;
    const std::vector<GroupEntries> entries = FindGroupEntries(r);
    if (files_start > all.size() || !EntriesBeforeFiles(entries, archive.Groups(), file_block))
    {
        throw xsflib::FormatError("the archive's tables of groups don't come before its files");
    }

    // The FILE block then holds the items that the model reads.
    const std::vector<uint32_t> files = SequenceFiles(archive, indices);
    const std::vector<std::vector<bool>> kept = ItemsRead(archive, files);
    std::vector<uint8_t> copy(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(files_start));
    const std::vector<GroupPlace> places = AppendKeptItems(archive, kept, copy);
    if (copy.size() > kMaxArchiveSize)
    {
        throw xsflib::FormatError("the files that the sequences play are too large for a sound archive");
    }

    WritePlaces(archive, kept, entries, places, file_block, copy);

    // The model reads each sequence's files from the copy. They must hold the archive's bytes.
    const ntdlib::rsar::SoundArchive cut = ntdlib::rsar::SoundArchive::Load(copy, nw4r::kRsarOptions);
    for (const uint32_t file_id : files)
    {
        if (!SameFile(archive.File(file_id), cut.File(file_id)))
        {
            throw xsflib::FormatError("the files that the sequences play can't be cut out of the archive");
        }
    }

    return copy;
}

std::vector<uint8_t> KeepLoadedGroups(const ntdlib::rsar::SoundArchive& archive, const std::vector<uint32_t>& indices)
{
    // The groups that the driver loads for the sequences.
    const std::vector<ntdlib::rsar::SoundArchive::Group>& groups = archive.Groups();
    std::vector<bool> loaded(groups.size());
    for (const uint32_t index : indices)
    {
        if (const std::optional<SoundData> data = FindSoundData(archive, index))
        {
            loaded[data->group] = true;
            loaded[data->bank_group.value_or(data->group)] = true;
        }
    }

    // The ranges that the driver reads: the header's first bytes, the INFO block, then each loaded group's files and
    // wave data. The loader skips wave data of no bytes.
    const std::span<const uint8_t> all = archive.Bytes();
    const xsflib::Reader r(all, true);
    const uint64_t info = r.U32(RsarLayout::kInfoBlock);
    std::vector<std::pair<uint64_t, uint64_t>> read = {{0, kHeaderRead},
                                                       {info, info + r.U32(RsarLayout::kInfoBlockSize)}};
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        if (!loaded[g])
        {
            continue;
        }

        // A stream of a group of no bytes reads on to the archive's end (DvdSoundArchive's DvdFileStream). The copy
        // then keeps the whole archive.
        if (groups[g].size == 0)
        {
            return {all.begin(), all.end()};
        }

        read.emplace_back(groups[g].offset, uint64_t{groups[g].offset} + groups[g].size);
        if (groups[g].wave_data_size != 0)
        {
            read.emplace_back(groups[g].wave_data_offset,
                              uint64_t{groups[g].wave_data_offset} + groups[g].wave_data_size);
        }
    }

    // The copy keeps those ranges, rounded out to multiples of 32 bytes. The FILE block's other data becomes zeros.
    std::vector<uint8_t> copy(all.begin(), all.end());
    const uint64_t file_block = r.U32(RsarLayout::kFileBlock);
    const uint64_t end = std::min<uint64_t>(file_block + r.U32(RsarLayout::kFileBlockSize), copy.size());
    uint64_t at = file_block + RsarLayout::kBlockHeader;
    const auto zero_until = [&copy, &at, end](uint64_t stop)
    {
        if (at < std::min(stop, end))
        {
            std::fill(copy.begin() + static_cast<std::ptrdiff_t>(at),
                      copy.begin() + static_cast<std::ptrdiff_t>(std::min(stop, end)), uint8_t{0});
        }
    };

    std::sort(read.begin(), read.end());
    for (const auto& [start, stop] : read)
    {
        zero_until(start / kFileAlignment * kFileAlignment);
        at = std::max(at, (stop + kFileAlignment - 1) / kFileAlignment * kFileAlignment);
    }

    zero_until(end);

    return copy;
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

std::vector<Piece> FindPieces(const ntdlib::rsar::SoundArchive& archive)
{
    static constexpr std::array<std::string_view, 4> kSuffixes = {"_S1", "_S2", "_TR", "_NO"};

    const auto& sounds = archive.Sounds();
    std::map<std::string, uint32_t, std::less<>> sequences;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        if (sounds[i].type == ntdlib::rsar::SoundType::kSequence)
        {
            sequences.emplace(sounds[i].name, static_cast<uint32_t>(i));
        }
    }

    // Each piece starts at its first channel's sequence.
    std::vector<Piece> pieces;
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        const std::string& label = sounds[i].name;
        if (sounds[i].type != ntdlib::rsar::SoundType::kSequence || label.size() <= kSuffixes[0].size() ||
            !label.ends_with(kSuffixes[0]))
        {
            continue;
        }

        Piece piece{label.substr(0, label.size() - kSuffixes[0].size()), {}};
        for (const std::string_view suffix : kSuffixes)
        {
            const auto found = sequences.find(piece.name + std::string(suffix));
            if (found == sequences.end())
            {
                break;
            }

            piece.sounds.push_back(found->second);
        }

        if (piece.sounds.size() == kSuffixes.size() && piece.sounds.front() == i)
        {
            pieces.push_back(std::move(piece));
        }
    }

    return pieces;
}

std::optional<Timing> TimingOfPiece(const std::vector<std::optional<Timing>>& timings)
{
    if (timings.empty() || std::any_of(timings.begin(), timings.end(), [](const auto& t) { return !t; }))
    {
        return std::nullopt;
    }

    std::optional<Timing> piece;
    for (const std::optional<Timing>& timing : timings)
    {
        if (timing->silent)
        {
            continue;
        }

        if (!piece)
        {
            piece = timing;
            continue;
        }

        piece->length_ms = std::max(piece->length_ms, timing->length_ms);
        piece->fade_ms = std::max(piece->fade_ms, timing->fade_ms);
    }

    return piece ? piece : timings.front();
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

using ntdlib::nwsnd::kTrackCount;

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
        : model_(model), player_(player), dsp_(dsp), tracks_(tracks)
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

    // Sets the hooks for the analysis's loops and for every track's passes. Only the passes that changed the sound
    // count. A track that another track opens again also makes a pass as it starts its part again. The first open
    // isn't a pass. On the Wii an open keeps the track's wait (SeqTrack::Open, 0x8003ffcc). The pass therefore counts
    // at the track's next command. Two opens during one wait make one pass.
    template <typename Function>
    void OnLoop(Function function)
    {
        model_.engine->SetLoopHook(std::move(function));
        const auto count_pass = [this](int track, uint32_t)
        {
            const auto* looping = player_.Track(track);
            if (looping && !looping->IsIdle())
            {
                ++passes_[static_cast<std::size_t>(track)];
            }
        };
        model_.engine->SetTrackLoopHook(count_pass);

        const auto mark_open = [this](int, int opened, uint32_t)
        {
            const auto index = static_cast<std::size_t>(opened);
            if (opened_[index])
            {
                reopened_[index] = true;
            }

            opened_[index] = true;
        };
        model_.engine->SetOpenTrackHook(mark_open);

        const auto count_start = [this](int track, uint32_t)
        {
            const auto index = static_cast<std::size_t>(track);
            if (reopened_[index])
            {
                ++passes_[index];
                reopened_[index] = false;
            }
        };
        model_.engine->SetCommandStartHook(count_start);
    }

    // Whether every open track other than `track` has made kLengthLoops passes that changed the sound. A track whose
    // last pass changed nothing, that waits for ever or that plays at a volume of 0 doesn't hold the fade back.
    bool OthersLooped(int track) const
    {
        for (int i = 0; i < kTrackCount; ++i)
        {
            const auto* other = player_.Track(i);
            const bool plays = (tracks_ & (1u << i)) != 0;
            if (i != track && other && other->IsOpen() && plays && !other->IsIdle() &&
                !other->WaitsForEver(*model_.engine) &&
                passes_[static_cast<std::size_t>(i)] < xsflib::rip::kLengthLoops)
            {
                return false;
            }
        }

        return true;
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
    std::array<int, kTrackCount> passes_{};    // each track's passes that changed the sound
    std::array<bool, kTrackCount> opened_{};   // whether another track has opened the track
    std::array<bool, kTrackCount> reopened_{}; // whether the track's next command starts its part again after an open
};

// The outermost counted loop that each open track of `player` is in: the track and the start of the loop's body. A
// track in no counted loop gives none.
std::set<std::pair<int, uint32_t>> OutermostCountedLoops(const nw4r::SeqPlayer& player)
{
    std::set<std::pair<int, uint32_t>> loops;
    for (int i = 0; i < kTrackCount; ++i)
    {
        const auto* track = player.Track(i);
        if (!track || !track->IsOpen())
        {
            continue;
        }

        for (const auto& entry : track->CallStack())
        {
            if (entry.is_loop && entry.loop_count != 0)
            {
                loops.insert({i, entry.address});
                break;
            }
        }
    }

    return loops;
}

// Measures a sequence in `arrangement` on the model (xsflib::rip::MeasureLength), with the effects of `effects_sound`.
// With `dsp` set, the microcode runs on the DSP. Otherwise the model runs without the DSP. The counted loops in
// `endless_loops` loop for ever.
std::optional<Timing> MeasureSequence(const ntdlib::rsar::SoundArchive& archive, const ntdlib::Microcode& microcode,
                                      nw4r::SndBuild build, uint32_t sound_index, uint32_t effects_sound,
                                      const nw4r::Arrangement& arrangement, const nw4r::ArchiveEffects& effects,
                                      bool dsp, const std::set<std::pair<int, uint32_t>>& endless_loops = {})
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
    // measure reaches the limit with tracks in counted loops, it runs again with each track's outermost counted loop
    // endless. Their passes then count as those of a loop of count 0 do. Up to the limit, the tracks play the same.
    if (timing && endless_loops.empty() && length_model.Frames() >= rules.max_frames)
    {
        if (const auto running = OutermostCountedLoops(*player); !running.empty())
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
    // The tracks that the game plays of the sound narrow the arrangement's.
    nw4r::Arrangement arrangement = impl_->arrangement;
    arrangement.tracks = static_cast<uint16_t>(arrangement.tracks & nw4r::TracksOf(impl_->effects.tracks, sound_index));

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
