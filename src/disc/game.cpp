// SPDX-License-Identifier: MIT

// Loads a Wii game for ripping (see game.h). WiiBrew's "Wii disc" page documents the disc layout: the partition table,
// a partition's header, the hashed sectors and the file system.

#include "disc/game.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/bytes.h"
#include "disc/image.h"

namespace wsf::disc
{
namespace
{

namespace fs = std::filesystem;

constexpr uint32_t kWiiMagic = 0x5D1C9EA3;
constexpr uint32_t kGameCubeMagic = 0xC2339F3D;
constexpr uint64_t kSector = 0x8000;     // a sector on the disc, with its hashes
constexpr uint64_t kSectorData = 0x7C00; // the data in a sector, without the hashes
constexpr uint64_t kHashes = 0x400;      // the hashes at the start of a sector

// The largest system file or file read into memory.
constexpr uint64_t kMaxFileSize = 1ull << 30;

// The message for an image whose game partition is encrypted.
constexpr char kEncrypted[] =
    "the game partition is encrypted. WSF requires a decrypted partition. NKit, RVZ and WIA images store it that way. "
    "Convert this image to RVZ with Dolphin (right-click the game, then Convert File) and use the RVZ. The conversion "
    "keeps all the data";

bool IsAscii(uint8_t c)
{
    return c < 0x80;
}

// Converts a Shift-JIS name from the disc to UTF-8. Bytes that aren't ASCII or half-width katakana become %XX.
std::string DecodeName(const uint8_t* name, size_t size)
{
    std::string out;
    for (size_t i = 0; i < size; ++i)
    {
        const uint8_t c = name[i];
        if (IsAscii(c))
        {
            out += static_cast<char>(c);
        }
        else if (c >= 0xA1 && c <= 0xDF)
        {
            // U+FF61 to U+FF9F, three bytes in UTF-8.
            const uint32_t code = 0xFF61 + (c - 0xA1u);
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else
        {
            static const char kDigits[] = "0123456789ABCDEF";
            out += '%';
            out += kDigits[c >> 4];
            out += kDigits[c & 15];
        }
    }

    return out;
}

std::string AsciiLower(std::string text)
{
    for (char& c : text)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return text;
}

// A file in the game partition's file system.
struct FstEntry
{
    uint64_t offset; // in the partition's data
    uint32_t size;
};

// Parses a Wii file system table (fst.bin) into its files. The table holds 12-byte entries, then their names. Each
// entry starts with a type byte and a 24-bit name offset. A file's entry then gives the file's offset / 4 and its size.
// A directory's entry gives its parent and the index after its last entry.
std::map<std::string, FstEntry> ParseFst(const std::vector<uint8_t>& fst)
{
    if (fst.size() < 12 || fst[0] != 1)
    {
        throw Error("the game partition's file system is damaged");
    }

    const uint32_t count = LoadBe32(fst.data() + 8);
    if (!count || count > fst.size() / 12)
    {
        throw Error("the game partition's file system is damaged");
    }

    const size_t names = size_t{count} * 12;
    const auto name = [&](uint32_t index)
    {
        const size_t start = names + (LoadBe32(fst.data() + index * 12) & 0xFFFFFF);
        const auto end =
            std::find(fst.begin() + static_cast<std::ptrdiff_t>(std::min(start, fst.size())), fst.end(), 0);
        if (start >= fst.size() || end == fst.end())
        {
            throw Error("the game partition's file system is damaged");
        }

        return DecodeName(fst.data() + start, static_cast<size_t>(end - fst.begin()) - start);
    };

    // Each directory ends at the index in its entry. The stack holds the open directories and their paths.
    std::map<std::string, FstEntry> files;
    std::vector<std::pair<uint32_t, std::string>> open = {{count, ""}};
    for (uint32_t i = 1; i < count; ++i)
    {
        while (i >= open.back().first)
        {
            open.pop_back();
        }

        const uint8_t* entry = fst.data() + i * 12;
        if (entry[0])
        {
            const uint32_t end = LoadBe32(entry + 8);
            if (end <= i || end > open.back().first)
            {
                throw Error("the game partition's file system is damaged");
            }

            open.push_back({end, open.back().second + name(i) + "/"});
        }
        else
        {
            files[open.back().second + name(i)] = {uint64_t{LoadBe32(entry + 4)} * 4, LoadBe32(entry + 8)};
        }
    }

    return files;
}

// The game partition of a Wii disc image in the form that the game reads: decrypted and without the hashes.
class GamePartition
{
public:
    explicit GamePartition(std::unique_ptr<Image> image) : image_(std::move(image))
    {
        uint8_t header[0x400];
        image_->Read(0, header, sizeof(header));
        if (LoadBe32(header + 0x18) != kWiiMagic)
        {
            if (LoadBe32(header + 0x1C) == kGameCubeMagic)
            {
                throw Error("it's a GameCube disc, and WSF is for Wii discs");
            }

            throw Error("it isn't a Wii disc image");
        }

        nkit_ = std::equal(header + 0x200, header + 0x208, "NKIT v01");
        const uint64_t partition = FindGamePartition();
        uint8_t partition_header[0x2C0];
        image_->Read(partition, partition_header, sizeof(partition_header));
        const uint32_t tmd_size = LoadBe32(partition_header + 0x2A4);
        if (tmd_size > 0x10000)
        {
            throw Error("the game partition's TMD is invalid");
        }

        tmd_.resize(tmd_size);
        image_->Read(partition + uint64_t{LoadBe32(partition_header + 0x2A8)} * 4, tmd_.data(), tmd_size);

        // WIA and RVZ store the partition's data decrypted. Other images store it as on the disc. The data is encrypted
        // unless the disc header turns encryption off (byte 0x61). It's in hashed sectors unless the header turns
        // hashes off (byte 0x60). NKit images turn these off. Wiimm's tools leave both bytes at 0 in images whose
        // partitions they decrypt. A partition that the header calls encrypted therefore still counts as decrypted if
        // its data starts with a Wii disc header. Encrypted data starts that way with a chance of one in four billion.
        data_offset_ = partition + uint64_t{LoadBe32(partition_header + 0x2B8)} * 4;
        stored_ = image_->OpenPartitionData(data_offset_);
        hashed_ = !header[0x60];
        if (!stored_ && !header[0x61])
        {
            uint8_t first[0x20];
            image_->Read(data_offset_ + (hashed_ ? kHashes : 0), first, sizeof(first));
            if (LoadBe32(first + 0x18) != kWiiMagic)
            {
                throw Error(kEncrypted);
            }
        }

        uint8_t boot[0x20];
        Read(0, boot, sizeof(boot));
        if (LoadBe32(boot + 0x18) != kWiiMagic)
        {
            throw Error("the game partition is damaged, or encrypted although the disc header says it isn't");
        }
    }

    std::string Source() const
    {
        return (nkit_ ? "NKit " : "") + image_->Format();
    }

    const std::vector<uint8_t>& Tmd() const
    {
        return tmd_;
    }

    void Read(uint64_t offset, uint8_t* out, size_t size)
    {
        if (stored_)
        {
            stored_->Read(offset, out, size);

            return;
        }
        if (!hashed_)
        {
            image_->Read(data_offset_ + offset, out, size);

            return;
        }

        while (size)
        {
            const uint64_t sector = offset / kSectorData;
            const size_t within = static_cast<size_t>(offset % kSectorData);
            const size_t n = static_cast<size_t>(std::min<uint64_t>(size, kSectorData - within));
            image_->Read(data_offset_ + sector * kSector + kHashes + within, out, n);
            out += n;
            offset += n;
            size -= n;
        }
    }

    std::vector<uint8_t> Read(uint64_t offset, uint64_t size)
    {
        if (size > kMaxFileSize)
        {
            throw Error("a file in the game partition is too large");
        }

        std::vector<uint8_t> data(static_cast<size_t>(size));
        Read(offset, data.data(), data.size());

        return data;
    }

private:
    // The offset of the first game partition (type 0) in the four groups of the partition table.
    uint64_t FindGamePartition()
    {
        uint8_t table[0x20];
        image_->Read(0x40000, table, sizeof(table));
        for (int group = 0; group < 4; ++group)
        {
            const uint32_t count = LoadBe32(table + group * 8);
            if (count > 64)
            {
                throw Error("the partition table is invalid");
            }

            std::vector<uint8_t> entries(size_t{count} * 8);
            image_->Read(uint64_t{LoadBe32(table + group * 8 + 4)} * 4, entries.data(), entries.size());
            for (uint32_t i = 0; i < count; ++i)
            {
                if (LoadBe32(entries.data() + i * 8 + 4) == 0)
                {
                    return uint64_t{LoadBe32(entries.data() + i * 8)} * 4;
                }
            }
        }

        throw Error("the disc has no game partition");
    }

    std::unique_ptr<Image> image_;
    std::unique_ptr<PartitionData> stored_; // the partition's data, if the image stores it separately
    uint64_t data_offset_ = 0;
    bool hashed_ = true;
    bool nkit_ = false;
    std::vector<uint8_t> tmd_;
};

// The size of a DOL: the end of the section that ends last in the file. The header lists 7 text and 11 data sections as
// offsets, then addresses, then sizes.
uint64_t DolSize(const uint8_t* header)
{
    uint64_t size = 0x100;
    for (int i = 0; i < 18; ++i)
    {
        size = std::max<uint64_t>(size, uint64_t{LoadBe32(header + i * 4)} + LoadBe32(header + 0x90 + i * 4));
    }

    return size;
}

void LoadImage(const fs::path& path, GameFiles& out)
{
    auto partition = std::make_shared<GamePartition>(OpenImage(path));
    out.source = partition->Source();
    out.tmd = partition->Tmd();

    // The system files: the header, bi2, the apploader, main.dol and the file system table. The apploader starts with a
    // 0x20-byte header that gives its size and the size of its trailer. The disc header gives the offsets of main.dol
    // and the file system table divided by 4.
    out.boot = partition->Read(0, 0x440);
    out.bi2 = partition->Read(0x440, 0x2000);
    const std::vector<uint8_t> apploader = partition->Read(0x2440, 0x20);
    out.apploader =
        partition->Read(0x2440, 0x20 + uint64_t{LoadBe32(apploader.data() + 0x14)} + LoadBe32(apploader.data() + 0x18));
    const uint64_t dol_offset = uint64_t{LoadBe32(out.boot.data() + 0x420)} * 4;
    const std::vector<uint8_t> dol_header = partition->Read(dol_offset, 0x100);
    out.dol = partition->Read(dol_offset, DolSize(dol_header.data()));
    out.fst = partition->Read(uint64_t{LoadBe32(out.boot.data() + 0x424)} * 4,
                              uint64_t{LoadBe32(out.boot.data() + 0x428)} * 4);

    auto files = std::make_shared<std::map<std::string, FstEntry>>(ParseFst(out.fst));
    out.files.clear();
    for (const auto& [name, entry] : *files)
    {
        out.files.push_back({name, entry.size});
    }

    out.read_file = [partition, files](const std::string& name, std::vector<uint8_t>& data)
    {
        const auto file = files->find(name);
        if (file == files->end())
        {
            return false;
        }

        data = partition->Read(file->second.offset, file->second.size);

        return true;
    };
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& data)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }

    file.seekg(0, std::ios::end);
    const auto size = static_cast<uint64_t>(file.tellg());
    if (size > kMaxFileSize)
    {
        return false;
    }

    file.seekg(0);
    data.resize(static_cast<size_t>(size));

    return !size ||
           static_cast<bool>(file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)));
}

void LoadFolder(const fs::path& root, GameFiles& out)
{
    out.source = "extracted disc";
    const fs::path sys = root / "sys";
    if (!ReadHostFile(sys / "boot.bin", out.boot) || out.boot.size() < 0x440 ||
        !ReadHostFile(sys / "main.dol", out.dol) || !ReadHostFile(sys / "fst.bin", out.fst))
    {
        throw Error("the extracted disc must contain sys/boot.bin, sys/main.dol and sys/fst.bin");
    }
    if (LoadBe32(out.boot.data() + 0x18) != kWiiMagic)
    {
        throw Error(LoadBe32(out.boot.data() + 0x1C) == kGameCubeMagic
                        ? "it's a GameCube disc, and WSF is for Wii discs"
                        : "sys/boot.bin isn't a Wii disc's header");
    }

    // Only game mode uses these files. A folder may leave them out.
    ReadHostFile(sys / "bi2.bin", out.bi2);
    ReadHostFile(sys / "apploader.img", out.apploader);
    ReadHostFile(root / "tmd.bin", out.tmd);

    // The files are those in the folder. There may be fewer than the file system table lists.
    const fs::path files = root / "files";
    std::error_code error;
    out.files.clear();
    for (auto it = fs::recursive_directory_iterator(files, error); !error && it != fs::recursive_directory_iterator();
         it.increment(error))
    {
        if (it->is_regular_file(error))
        {
            const std::u8string name = it->path().lexically_relative(files).generic_u8string();
            out.files.push_back({std::string(name.begin(), name.end()), it->file_size(error)});
        }
    }

    std::sort(out.files.begin(), out.files.end(), [](const GameFile& a, const GameFile& b) { return a.path < b.path; });
    out.read_file = [files](const std::string& name, std::vector<uint8_t>& data)
    {
        return ReadHostFile(files / fs::path(std::u8string(name.begin(), name.end())), data);
    };
}

} // namespace

std::string GameFiles::GameId() const
{
    return boot.size() >= 6 ? std::string(boot.begin(), boot.begin() + 6) : std::string();
}

std::string GameFiles::Title() const
{
    if (boot.size() < 0x60)
    {
        return std::string();
    }

    const auto end = std::find(boot.begin() + 0x20, boot.begin() + 0x60, 0);

    return DecodeName(boot.data() + 0x20, static_cast<size_t>(end - (boot.begin() + 0x20)));
}

std::vector<std::string> GameFiles::SoundArchives() const
{
    std::vector<std::string> archives;
    for (const GameFile& file : files)
    {
        if (AsciiLower(file.path).ends_with(".brsar"))
        {
            archives.push_back(file.path);
        }
    }

    return archives;
}

std::optional<std::string> LoadGame(const std::filesystem::path& path, GameFiles& out)
{
    try
    {
        std::error_code error;
        if (fs::is_directory(path, error))
        {
            const fs::path data = path / "DATA";
            LoadFolder(fs::is_regular_file(path / "sys" / "main.dol", error) ? path : data, out);
        }
        else
        {
            LoadImage(path, out);
        }
    }
    catch (const std::exception& e)
    {
        return e.what();
    }

    return std::nullopt;
}

} // namespace wsf::disc
