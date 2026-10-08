// SPDX-License-Identifier: MIT

// The layouts follow NW4R SND's readers as linked into Wii Sports (2007). They include the fields that later versions
// of the formats add or move.

#include "rsar/formats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "rsar/binary.h"

namespace wsf::rsar
{
namespace
{

// The maximum length read for a name in the string table.
constexpr size_t kMaxName = 1024;

// Checks a file's header (its magic and big-endian byte order mark) and returns its version.
uint16_t ReadHeader(const Reader& reader, const char* magic)
{
    if (!reader.Magic(0, magic))
    {
        throw FormatError(std::string("not an ") + magic + " file");
    }
    if (reader.U16(4) != 0xFEFF)
    {
        throw FormatError(std::string("the ") + magic + " file isn't big-endian (Wii files are)");
    }

    return reader.U16(6);
}

// Reads a wave info at `offset` whose channels' sample offsets count from `channel_base`.
Wave ReadWaveInfo(const Reader& info, size_t offset, std::span<const uint8_t> channel_base)
{
    Wave wave;
    const uint8_t format = info.U8(offset);
    if (format > 2)
    {
        throw FormatError("a wave has an unknown format (" + std::to_string(format) + ")");
    }

    wave.format = static_cast<WaveFormat>(format);
    wave.loop = info.U8(offset + 1) != 0;
    const uint8_t channels = info.U8(offset + 2);
    wave.sample_rate = (uint32_t{info.U8(offset + 3)} << 16) | info.U16(offset + 4);
    wave.loop_start = info.U32(offset + 8);
    wave.loop_end = info.U32(offset + 0xC);

    // Each channel: its samples' offset, its ADPCM info's offset (from the wave info, 0 for none) and its volumes.
    const size_t table = offset + info.U32(offset + 0x10);
    for (uint8_t c = 0; c < channels; ++c)
    {
        const size_t channel = offset + info.U32(table + c * 4u);
        WaveChannel out;
        const uint32_t samples = info.U32(channel);
        if (samples > channel_base.size())
        {
            throw FormatError("a wave's samples lie outside its wave data");
        }

        out.data = channel_base.subspan(samples);
        for (int v = 0; v < 4; ++v)
        {
            out.volumes[v] = info.U32(channel + 8 + v * 4u);
        }

        if (const uint32_t adpcm = info.U32(channel + 4))
        {
            const size_t a = offset + adpcm;
            for (int i = 0; i < 16; ++i)
            {
                out.adpcm.coefs[i] = info.S16(a + i * 2u);
            }

            out.adpcm.gain = info.U16(a + 0x20);
            out.adpcm.pred_scale = info.U16(a + 0x22);
            out.adpcm.yn1 = info.S16(a + 0x24);
            out.adpcm.yn2 = info.S16(a + 0x26);
            out.adpcm.loop_pred_scale = info.U16(a + 0x28);
            out.adpcm.loop_yn1 = info.S16(a + 0x2A);
            out.adpcm.loop_yn2 = info.S16(a + 0x2C);
        }

        wave.channels.push_back(out);
    }

    return wave;
}

} // namespace

Wave Wave::Parse(const Reader& info, size_t offset, std::span<const uint8_t> wave_data)
{
    const uint32_t data_offset = info.U32(offset + 0x14);
    if (data_offset > wave_data.size())
    {
        throw FormatError("a wave's samples lie outside its wave data");
    }

    return ReadWaveInfo(info, offset, wave_data.subspan(data_offset));
}

Wave Wave::ParseFile(std::span<const uint8_t> file)
{
    // An RWAV: an INFO block with the wave info, and a DATA block with the samples. The channels' offsets count from
    // the DATA block. The info's data offset points at the DATA block's header.
    const Reader reader(file);
    ReadHeader(reader, "RWAV");
    const uint32_t info = reader.U32(0x10);
    const uint32_t data = reader.U32(0x18);
    if (!reader.Magic(info, "INFO") || !reader.Magic(data, "DATA"))
    {
        throw FormatError("an RWAV file has no INFO or DATA block");
    }

    return ReadWaveInfo(reader, info + 8, reader.Sub(data + 8, file.size() - (data + 8)));
}

WaveArchive WaveArchive::Parse(std::span<const uint8_t> file)
{
    // A TABL block lists each wave's reference (from the start of the DATA block) and size.
    const Reader reader(file);
    ReadHeader(reader, "RWAR");
    const uint32_t table = reader.U32(0x10);
    const uint32_t data = reader.U32(0x18);
    if (!reader.Magic(table, "TABL") || !reader.Magic(data, "DATA"))
    {
        throw FormatError("an RWAR file has no TABL or DATA block");
    }

    WaveArchive archive;
    const uint32_t count = reader.U32(table + 8);
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t entry = table + 0xC + i * size_t{12};
        const DataRef ref = ReadRef(reader, entry);
        archive.waves.push_back(reader.Sub(data + size_t{ref.value}, reader.U32(entry + 8)));
    }

    return archive;
}

Bank Bank::Parse(std::span<const uint8_t> file, std::span<const uint8_t> wave_data)
{
    const Reader reader(file);
    Bank bank;
    bank.version_ = ReadHeader(reader, "RBNK");
    const uint32_t data = reader.U32(0x10);
    if (!reader.Magic(data, "DATA"))
    {
        throw FormatError("an RBNK file has no DATA block");
    }

    // The instrument table: a count, then a reference for each program, from the table's start. Tables and regions
    // that several references share are read once.
    const size_t base = data + 8;
    const uint32_t count = reader.U32(base);
    std::map<std::pair<uint8_t, uint32_t>, uint32_t> seen;
    const auto read_node = [&](size_t ref_offset, int depth, const auto& self) -> Node
    {
        const DataRef ref = ReadRef(reader, ref_offset);
        if (!ref.IsOffset() || ref.data_type == kNone)
        {
            return Node{kNone, 0};
        }
        if (ref.data_type != kRegion && (depth >= 2 || ref.data_type > kIndexTable))
        {
            // NW4R looks at most two levels deep: a key, then a velocity. It supports no other types.
            return Node{ref.data_type, 0};
        }
        if (const auto known = seen.find({ref.data_type, ref.value}); known != seen.end())
        {
            return Node{ref.data_type, known->second};
        }

        const size_t at = base + ref.value;
        if (ref.data_type == kRegion)
        {
            Region region;
            region.wave_index = reader.S32(at);
            region.attack = reader.U8(at + 4);
            region.decay = reader.U8(at + 5);
            region.sustain = reader.U8(at + 6);
            region.release = reader.U8(at + 7);
            region.hold = reader.U8(at + 8);
            region.wave_data_location = reader.U8(at + 9);
            region.note_off_type = reader.U8(at + 0xA);
            region.alternate_assign = reader.U8(at + 0xB);
            region.original_key = reader.U8(at + 0xC);
            region.pan = reader.U8(at + 0xE);
            region.surround_pan = reader.U8(at + 0xF);

            // In a bank before version 1.1, the game ignores a region's volume and tuning. It uses the defaults: full
            // volume and no tuning (Wii Sports, 0x80035ef0).
            if (bank.version_ >= 0x0101)
            {
                region.volume = reader.U8(at + 0xD);
                region.tune = reader.F32(at + 0x10);
            }

            bank.regions_.push_back(region);
            seen[{ref.data_type, ref.value}] = static_cast<uint32_t>(bank.regions_.size() - 1);

            return Node{kRegion, static_cast<uint32_t>(bank.regions_.size() - 1)};
        }

        // A range table: a count and each entry's upper key, then references from a multiple of 4 bytes on. An index
        // table: its lowest and highest key, two padding bytes, then a reference for each key.
        Table table;
        table.type = ref.data_type;
        size_t refs;
        size_t entries;
        if (ref.data_type == kRangeTable)
        {
            entries = reader.U8(at);
            for (size_t i = 0; i < entries; ++i)
            {
                table.keys.push_back(reader.U8(at + 1 + i));
            }

            refs = at + ((entries + 1 + 3) & ~size_t{3});
        }
        else
        {
            const uint8_t lowest = reader.U8(at);
            const uint8_t highest = reader.U8(at + 1);
            table.keys = {lowest, highest};
            entries = highest >= lowest ? size_t{highest} - lowest + 1 : 0;
            refs = at + 4;
        }

        const uint32_t index = static_cast<uint32_t>(bank.tables_.size());
        seen[{ref.data_type, ref.value}] = index;
        bank.tables_.push_back(table);
        for (size_t i = 0; i < entries; ++i)
        {
            const Node entry = self(refs + i * 8, depth + 1, self);
            bank.tables_[index].entries.push_back(entry);
        }

        return Node{ref.data_type, index};
    };

    for (uint32_t i = 0; i < count; ++i)
    {
        bank.instruments_.push_back(read_node(base + 4 + i * size_t{8}, 0, read_node));
    }

    // The waves. A version 1.1 bank's WAVE block holds a reference to each wave info (from the table's start). The
    // samples are in the wave data. A later bank's wave data is an RWAR.
    const uint32_t wave = reader.U16(0x0E) >= 2 ? reader.U32(0x18) : 0;
    if (wave && reader.Magic(wave, "WAVE"))
    {
        const size_t wave_base = wave + 8;
        const uint32_t waves = reader.U32(wave_base);
        for (uint32_t i = 0; i < waves; ++i)
        {
            const DataRef ref = ReadRef(reader, wave_base + 4 + i * size_t{8});
            bank.waves_.push_back(Wave::Parse(reader, wave_base + ref.value, wave_data));
        }
    }
    else if (!wave_data.empty())
    {
        for (const std::span<const uint8_t> rwav : WaveArchive::Parse(wave_data).waves)
        {
            bank.waves_.push_back(Wave::ParseFile(rwav));
        }
    }

    return bank;
}

const Bank::Node* Bank::Choose(const Node& node, int key) const
{
    switch (node.type)
    {
    case kRegion:
        return &node;
    case kRangeTable:
        {
            const Table& table = tables_[node.index];
            for (size_t i = 0; i < table.entries.size(); ++i)
            {
                if (key <= table.keys[i])
                {
                    return &table.entries[i];
                }
            }

            return nullptr;
        }

    case kIndexTable:
        {
            const Table& table = tables_[node.index];
            if (key < table.keys[0] || key > table.keys[1] || table.entries.empty())
            {
                return nullptr;
            }

            return &table.entries[static_cast<size_t>(key - table.keys[0])];
        }

    default:
        return nullptr;
    }
}

const Region* Bank::Find(int program, int key, int velocity) const
{
    if (program < 0 || static_cast<size_t>(program) >= instruments_.size())
    {
        return nullptr;
    }

    const Node* node = &instruments_[static_cast<size_t>(program)];
    if (node->type == kNone)
    {
        return nullptr;
    }

    for (const int split : {key, velocity})
    {
        if (node->type != kRegion)
        {
            node = Choose(*node, split);
            if (!node || node->type == kNone)
            {
                return nullptr;
            }
        }
    }

    if (node->type != kRegion || regions_[node->index].wave_index < 0)
    {
        return nullptr;
    }

    return &regions_[node->index];
}

Sequence Sequence::Parse(std::span<const uint8_t> file)
{
    // DATA holds the commands from an offset that it gives (from the block's start). LABL holds an offset for each
    // label (from LABL's payload). Each offset leads to the label's data offset, name length and name.
    const Reader reader(file);
    ReadHeader(reader, "RSEQ");
    const uint32_t data = reader.U32(0x10);
    const uint32_t data_size = reader.U32(0x14);
    if (!reader.Magic(data, "DATA") || data_size < 0xC)
    {
        throw FormatError("an RSEQ file has no DATA block");
    }

    Sequence sequence;
    const uint32_t commands = reader.U32(data + 8);
    if (commands > data_size)
    {
        throw FormatError("an RSEQ file's commands lie outside its DATA block");
    }

    sequence.data = reader.Sub(data + size_t{commands}, data_size - commands);
    const uint32_t labels = reader.U16(0x0E) >= 2 ? reader.U32(0x18) : 0;
    if (labels && reader.Magic(labels, "LABL"))
    {
        const size_t base = labels + 8;
        const uint32_t count = reader.U32(base);
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t label = base + reader.U32(base + 4 + i * size_t{4});
            const uint32_t length = reader.U32(label + 4);
            const std::span<const uint8_t> name = reader.Sub(label + 8, length);
            sequence.labels.push_back({std::string(name.begin(), name.end()), reader.U32(label)});
        }
    }

    return sequence;
}

SoundArchive SoundArchive::Load(std::vector<uint8_t> bytes)
{
    SoundArchive archive;
    archive.bytes_ = std::move(bytes);
    const Reader reader(archive.bytes_);
    if (reader.Magic(0, "CSAR") || reader.Magic(0, "FSAR"))
    {
        throw FormatError(reader.Magic(0, "CSAR") ? "a 3DS sound archive (CSAR), not a Wii one (RSAR)"
                                                  : "a Wii U or Switch sound archive (FSAR), not a Wii one (RSAR)");
    }

    archive.version_ = ReadHeader(reader, "RSAR");
    archive.truncated_ = reader.U32(8) > reader.Size();
    const uint32_t symbols = reader.U32(0x10);
    const uint32_t symbols_size = reader.U32(0x14);
    const uint32_t info = reader.U32(0x18);
    if (!reader.Magic(info, "INFO"))
    {
        throw FormatError("the sound archive has no INFO block");
    }

    archive.info_size_ = reader.U32(0x1c);

    // The names: a table of offsets (from SYMB's payload) to strings. An archive may come without them.
    const size_t symbol_base = symbols + size_t{8};
    std::vector<uint32_t> strings;
    if (symbols && symbols_size && reader.Magic(symbols, "SYMB"))
    {
        const size_t table = symbol_base + reader.U32(symbol_base);
        const uint32_t count = reader.U32(table);
        for (uint32_t i = 0; i < count; ++i)
        {
            strings.push_back(reader.U32(table + 4 + i * size_t{4}));
        }
    }

    const auto name = [&](uint32_t string_id) -> std::optional<std::string>
    {
        if (string_id >= strings.size())
        {
            return std::nullopt;
        }

        return reader.CString(symbol_base + strings[string_id], kMaxName);
    };

    // INFO's tables, each a count and references from INFO's payload.
    const size_t base = info + size_t{8};
    const auto table = [&](size_t ref_offset, const auto& read_entry)
    {
        const DataRef ref = ReadRef(reader, ref_offset);
        if (!ref.IsOffset())
        {
            return;
        }

        const size_t at = base + ref.value;
        const uint32_t count = reader.U32(at);
        for (uint32_t i = 0; i < count; ++i)
        {
            read_entry(ReadRef(reader, at + 4 + i * size_t{8}));
        }
    };

    // The sounds. Before version 1.1, the reference's data type gives the sound type. From 1.1, a field gives it.
    const auto read_sound = [&](const DataRef& ref)
    {
        const size_t at = base + ref.value;
        SoundInfo sound;
        sound.file_id = reader.U32(at + 4);
        sound.player_id = reader.U32(at + 8);
        sound.volume = reader.U8(at + 0x14);
        sound.player_priority = reader.U8(at + 0x15);
        const uint8_t type = archive.version_ >= 0x0101 ? reader.U8(at + 0x16) : ref.data_type;
        sound.type = type >= 1 && type <= 3 ? static_cast<SoundType>(type) : SoundType::kInvalid;
        sound.remote_filter = reader.U8(at + 0x17);
        sound.user_params[0] = reader.U32(at + 0x20);
        sound.user_params[1] = reader.U32(at + 0x24);
        if (archive.version_ >= 0x0102)
        {
            sound.pan_mode = reader.U8(at + 0x28);
            sound.pan_curve = reader.U8(at + 0x29);
        }

        const DataRef detail = ReadRef(reader, at + 0x18);
        const size_t d = base + detail.value;
        if (detail.IsOffset() && sound.type == SoundType::kSequence)
        {
            sound.data_offset = reader.U32(d);
            sound.bank_id = reader.U32(d + 4);
            sound.allocate_track = reader.U32(d + 8);
            sound.channel_priority = reader.U8(d + 0xC);
            sound.release_priority_fix = archive.version_ >= 0x0103 && reader.U8(d + 0xD) != 0;
        }
        else if (detail.IsOffset() && sound.type == SoundType::kWaveSound)
        {
            sound.sub_index = reader.S32(d);
            sound.allocate_track = reader.U32(d + 4);
            sound.channel_priority = reader.U8(d + 8);
            sound.release_priority_fix = archive.version_ >= 0x0103 && reader.U8(d + 9) != 0;
        }

        static const char* const kPrefixes[] = {"SOUND", "SEQ", "STRM", "WSD"};
        const size_t index = archive.sounds_.size();
        sound.name = name(reader.U32(at))
                         .value_or(std::string(kPrefixes[static_cast<int>(sound.type)]) + "_" + std::to_string(index));
        archive.sounds_.push_back(sound);
    };

    table(base, read_sound);

    const auto read_bank = [&](const DataRef& ref)
    {
        const size_t at = base + ref.value;
        BankInfo bank;
        bank.file_id = reader.U32(at + 4);
        bank.name = name(reader.U32(at)).value_or("BANK_" + std::to_string(archive.banks_.size()));
        archive.banks_.push_back(bank);
    };

    table(base + 8, read_bank);

    const auto read_player = [&](const DataRef& ref)
    {
        const size_t at = base + ref.value;
        PlayerInfo player;
        player.sound_count = reader.U8(at + 4);
        player.heap_size = reader.U32(at + 8);
        player.name = name(reader.U32(at)).value_or("PLAYER_" + std::to_string(archive.players_.size()));
        archive.players_.push_back(player);
    };

    table(base + 0x10, read_player);

    // The files, each with its positions in groups.
    const auto read_file = [&](const DataRef& ref)
    {
        const size_t at = base + ref.value;
        std::vector<FilePosition> positions;
        const auto read_position = [&](const DataRef& position)
        {
            positions.push_back({reader.U32(base + position.value), reader.U32(base + position.value + 4)});
        };

        table(at + 0x14, read_position);
        archive.files_.push_back(std::move(positions));
    };

    table(base + 0x18, read_file);

    // The groups. An external group has a file path and is stored outside the archive.
    const auto read_group = [&](const DataRef& ref)
    {
        const size_t at = base + ref.value;
        Group group;
        group.internal = !ReadRef(reader, at + 8).IsOffset();
        group.offset = reader.U32(at + 0x10);
        group.size = reader.U32(at + 0x14);
        group.wave_data_offset = reader.U32(at + 0x18);
        group.wave_data_size = reader.U32(at + 0x1C);
        const auto read_item = [&](const DataRef& item)
        {
            const size_t i = base + item.value;
            group.items.push_back(
                {reader.U32(i), reader.U32(i + 4), reader.U32(i + 8), reader.U32(i + 0xC), reader.U32(i + 0x10)});
        };

        table(at + 0x20, read_item);
        const uint64_t end =
            std::max(uint64_t{group.offset} + group.size, uint64_t{group.wave_data_offset} + group.wave_data_size);
        if (group.internal && end > reader.Size())
        {
            archive.truncated_ = true;
        }

        archive.groups_.push_back(std::move(group));
    };

    table(base + 0x20, read_group);

    return archive;
}

std::optional<uint32_t> SoundArchive::FindSound(const std::string& name) const
{
    for (size_t i = 0; i < sounds_.size(); ++i)
    {
        if (sounds_[i].name == name)
        {
            return static_cast<uint32_t>(i);
        }
    }

    return std::nullopt;
}

std::optional<FileData> SoundArchive::File(uint32_t file_id) const
{
    if (file_id >= files_.size())
    {
        return std::nullopt;
    }

    const auto inside = [this](uint64_t offset, uint64_t size)
    {
        return offset <= bytes_.size() && size <= bytes_.size() - offset;
    };

    for (const FilePosition& position : files_[file_id])
    {
        if (position.group >= groups_.size())
        {
            continue;
        }

        const Group& group = groups_[position.group];
        if (!group.internal || position.index >= group.items.size())
        {
            continue;
        }

        const Group::Item& item = group.items[position.index];
        const uint64_t file = uint64_t{group.offset} + item.offset;
        const uint64_t wave_data = uint64_t{group.wave_data_offset} + item.wave_data_offset;
        if (inside(file, item.size) && inside(wave_data, item.wave_data_size))
        {
            const std::span<const uint8_t> all(bytes_);

            return FileData{all.subspan(static_cast<size_t>(file), item.size),
                            all.subspan(static_cast<size_t>(wave_data), item.wave_data_size)};
        }
    }

    return std::nullopt;
}

} // namespace wsf::rsar
