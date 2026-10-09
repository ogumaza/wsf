// SPDX-License-Identifier: MIT

// Readers for the NintendoWare for Revolution sound formats: the sound archive (RSAR), and the sequences (RSEQ), banks
// (RBNK), wave archives (RWAR) and waves (RWAV) in it. The parsed data points into the source bytes. The source bytes
// must outlive it. Archives and their banks come in two generations. In RSAR 1.1, a bank (RBNK 1.1) describes its waves
// in a WAVE block. Its wave data is the raw samples. In RSAR 1.4, a bank (RBNK 1.2) has no WAVE block. Its wave data is
// an RWAR of RWAV files.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rsar/binary.h"

namespace wsf::rsar
{

enum class WaveFormat : uint8_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kAdpcm = 2,
};

// A DSP ADPCM channel's decoding state: the coefficients, the gain, and the predictor/scale and history at the start
// and at the loop start.
struct AdpcmInfo
{
    std::array<int16_t, 16> coefs{};
    uint16_t gain = 0;
    uint16_t pred_scale = 0;
    int16_t yn1 = 0;
    int16_t yn2 = 0;
    uint16_t loop_pred_scale = 0;
    int16_t loop_yn1 = 0;
    int16_t loop_yn2 = 0;
};

struct WaveChannel
{
    std::span<const uint8_t> data;     // the channel's samples, from its first to the end of the wave data
    AdpcmInfo adpcm;                   // ADPCM only
    std::array<uint32_t, 4> volumes{}; // front left, front right, rear left, rear right, as 8.24 fixed point
};

// A wave: an RWAV's INFO block, or a wave info in a version 1.1 bank's WAVE block.
struct Wave
{
    // Reads a wave info at `offset` in `info`. Its samples start at `wave_data` plus the info's data offset plus each
    // channel's offset.
    static Wave Parse(const Reader& info, size_t offset, std::span<const uint8_t> wave_data);

    // Reads an RWAV file. The samples are in the file's DATA block.
    static Wave ParseFile(std::span<const uint8_t> file);

    WaveFormat format = WaveFormat::kPcm16;
    bool loop = false;
    uint32_t sample_rate = 32000;

    // DSP addresses as the file stores them: in nibbles for ADPCM (two for each 8-byte frame's header), in samples for
    // PCM16 and in bytes for PCM8. The end is the last sample's address.
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;

    std::vector<WaveChannel> channels;
};

// RWAR: a wave archive, a table of RWAV files.
struct WaveArchive
{
    static WaveArchive Parse(std::span<const uint8_t> file);

    std::vector<std::span<const uint8_t>> waves; // each an RWAV file
};

// An instrument's region in a bank (its InstParam): what plays for a range of keys and velocities.
struct Region
{
    int32_t wave_index = 0; // in the bank's waves; a negative index plays nothing
    uint8_t attack = 127;
    uint8_t decay = 127;
    uint8_t sustain = 127;
    uint8_t release = 127;
    uint8_t hold = 0;
    uint8_t wave_data_location = 0; // 0: an index into the bank's waves
    uint8_t note_off_type = 0;      // 0: release, 1: ignore note-off
    uint8_t alternate_assign = 0;   // the region's key group
    uint8_t original_key = 60;
    uint8_t volume = 127;
    uint8_t pan = 64;
    uint8_t surround_pan = 0;
    float tune = 1.0f;
};

// RBNK: a bank of instruments, each a tree of key and velocity tables, and the waves its regions play.
class Bank
{
public:
    // The data types of a bank's references.
    enum : uint8_t
    {
        kNone = 0,
        kRegion = 1,
        kRangeTable = 2,
        kIndexTable = 3,
    };

    // Reads a bank and its waves from `wave_data`: the raw samples for a version 1.1 bank, an RWAR for a later one.
    static Bank Parse(std::span<const uint8_t> file, std::span<const uint8_t> wave_data);

    uint16_t Version() const
    {
        return version_;
    }

    size_t InstrumentCount() const
    {
        return instruments_.size();
    }

    // The region that plays a key and velocity on a program. The choice follows NW4R's bank reader of 2007. The
    // instrument's reference leads to a key table, then a velocity table, then a region. Each table is a range table
    // (the first entry whose upper key is at least the key) or an index table (by key from its lowest). Any other type
    // of reference, a key outside a table and a negative wave index play nothing.
    const Region* Find(int program, int key, int velocity) const;

    const std::vector<Wave>& Waves() const
    {
        return waves_;
    }

private:
    // A reference in the tree, resolved: a region (an index into regions_), a table (an index into tables_), or
    // nothing, with the data type it had.
    struct Node
    {
        uint8_t type = kNone;
        uint32_t index = 0;
    };

    // A range table (keys holds each entry's upper key) or an index table (keys holds its lowest and highest key).
    struct Table
    {
        uint8_t type = kRangeTable;
        std::vector<uint8_t> keys;
        std::vector<Node> entries;
    };

    const Node* Choose(const Node& node, int key) const;

    uint16_t version_ = 0;
    std::vector<Node> instruments_;
    std::vector<Table> tables_;
    std::vector<Region> regions_;
    std::vector<Wave> waves_;
};

// RSEQ: a sequence file. It can hold several sequences. Each starts at a label.
struct Sequence
{
    struct Label
    {
        std::string name;
        uint32_t offset; // into data
    };

    static Sequence Parse(std::span<const uint8_t> file);

    std::span<const uint8_t> data; // the commands
    std::vector<Label> labels;
};

enum class SoundType : uint8_t
{
    kInvalid = 0,
    kSequence = 1,
    kStream = 2,
    kWaveSound = 3,
};

struct SoundInfo
{
    std::string name; // from the symbols, or made up from the index (SEQ_12) when the archive has none
    uint32_t file_id = 0;
    uint32_t player_id = 0;
    uint8_t volume = 127;
    uint8_t player_priority = 64;
    SoundType type = SoundType::kInvalid;
    uint8_t remote_filter = 0;
    uint32_t user_params[2] = {};
    uint8_t pan_mode = 1;  // 0 dual, 1 balance (the default before version 1.2)
    uint8_t pan_curve = 0; // 0 square root (the default before version 1.2)

    // Sequence sounds: where in the file the sequence starts, its bank, the tracks it may use, and its notes' priority.
    // Wave sounds use allocate_track, channel_priority, release_priority_fix and sub_index.
    uint32_t data_offset = 0;
    uint32_t bank_id = 0;
    uint32_t allocate_track = 0;
    uint8_t channel_priority = 64;
    bool release_priority_fix = false; // false before version 1.3
    int32_t sub_index = 0;
};

struct BankInfo
{
    std::string name;
    uint32_t file_id = 0;
};

// A sound player: a channel of the game's sound system that sounds play on. One example is a player for music.
struct PlayerInfo
{
    std::string name;        // from the symbols, or made up from the index (PLAYER_2) when the archive has none
    uint8_t sound_count = 0; // the number of sounds it plays at once
    uint32_t heap_size = 0;
};

// A file in a group: its contents and its wave data. Banks and wave sound files have wave data.
struct FileData
{
    std::span<const uint8_t> file;
    std::span<const uint8_t> wave_data;
};

// RSAR: a sound archive. The object keeps the archive's bytes. The spans it returns stay valid as long as the object
// exists.
class SoundArchive
{
public:
    // A group of files that a game loads into memory together: their data, then their wave data.
    struct Group
    {
        struct Item
        {
            uint32_t file_id;
            uint32_t offset;
            uint32_t size;
            uint32_t wave_data_offset;
            uint32_t wave_data_size;
        };

        bool internal = false; // held in the archive, rather than in a file beside it
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t wave_data_offset = 0;
        uint32_t wave_data_size = 0;
        std::vector<Item> items;
    };

    // Reads an archive. Throws FormatError for anything that isn't a readable RSAR.
    static SoundArchive Load(std::vector<uint8_t> bytes);

    uint16_t Version() const
    {
        return version_;
    }

    const std::vector<SoundInfo>& Sounds() const
    {
        return sounds_;
    }

    const std::vector<BankInfo>& Banks() const
    {
        return banks_;
    }

    const std::vector<PlayerInfo>& Players() const
    {
        return players_;
    }

    std::optional<uint32_t> FindSound(const std::string& name) const;

    // A file's contents and wave data, from the first group that holds the file inside the archive. Returns nothing for
    // a file that only an external group or file holds, or that lies past the end of a truncated archive.
    std::optional<FileData> File(uint32_t file_id) const;

    const std::vector<Group>& Groups() const
    {
        return groups_;
    }

    // The size of the INFO block. A game loads this block to use the archive.
    uint32_t InfoSize() const
    {
        return info_size_;
    }

    // The archive is shorter than its header says, or groups lie past its end.
    bool Truncated() const
    {
        return truncated_;
    }

private:
    struct FilePosition
    {
        uint32_t group;
        uint32_t index;
    };

    std::vector<uint8_t> bytes_;
    uint16_t version_ = 0;
    std::vector<SoundInfo> sounds_;
    std::vector<BankInfo> banks_;
    std::vector<PlayerInfo> players_;
    std::vector<std::vector<FilePosition>> files_; // where each file is, by file id
    std::vector<Group> groups_;
    uint32_t info_size_ = 0;
    bool truncated_ = false;
};

} // namespace wsf::rsar
