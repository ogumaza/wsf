// SPDX-License-Identifier: MIT

#include "wii/file_disc.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/bytes.h"

namespace wsf::wii
{
namespace
{

constexpr std::size_t kHeaderSize = 0x440;
constexpr uint64_t kFstOffset = 0x2440; // where the apploader would be

// A directory of the file system, with its subdirectories and files by name.
struct Directory
{
    std::map<std::string, Directory> directories;
    std::map<std::string, std::size_t> files; // index into the files given
};

// The table's entries and their names, in the order of a walk of the tree. Each entry is 12 bytes: the kind and the
// name's offset, then a file's data offset / 4 and size, or a directory's parent and the index after its last entry.
class FstWriter
{
public:
    explicit FstWriter(const std::vector<uint64_t>& offsets, const std::vector<std::size_t>& sizes)
        : offsets_(offsets), sizes_(sizes)
    {
    }

    void Write(const Directory& directory, const std::string& name, uint32_t parent)
    {
        const auto index = static_cast<uint32_t>(entries_.size() / 12);
        Entry(true, name, parent, 0);
        for (const auto& [file_name, file] : directory.files)
        {
            Entry(false, file_name, static_cast<uint32_t>(offsets_[file] / 4), static_cast<uint32_t>(sizes_[file]));
        }

        for (const auto& [directory_name, child] : directory.directories)
        {
            Write(child, directory_name, index);
        }

        // The directory's last word: the index after its entries.
        StoreBe32(&entries_[index * 12 + 8], static_cast<uint32_t>(entries_.size() / 12));
    }

    std::vector<uint8_t> Table() const
    {
        std::vector<uint8_t> table = entries_;
        table.insert(table.end(), names_.begin(), names_.end());

        return table;
    }

private:
    void Entry(bool directory, const std::string& name, uint32_t first, uint32_t second)
    {
        uint8_t entry[12];
        StoreBe32(&entry[0], (directory ? 0x01000000u : 0) | static_cast<uint32_t>(names_.size()));
        StoreBe32(&entry[4], first);
        StoreBe32(&entry[8], second);
        entries_.insert(entries_.end(), entry, entry + 12);
        names_.insert(names_.end(), name.begin(), name.end());
        names_.push_back(0);
    }

    const std::vector<uint64_t>& offsets_;
    const std::vector<std::size_t>& sizes_;
    std::vector<uint8_t> entries_;
    std::vector<uint8_t> names_;
};

} // namespace

FileDisc::FileDisc(std::vector<uint8_t> header,
                   const std::vector<std::pair<std::string, std::span<const uint8_t>>>& files)
    : header_(std::move(header))
{
    header_.resize(kHeaderSize);

    // The tree of the files' paths.
    Directory root;
    for (std::size_t i = 0; i < files.size(); i++)
    {
        Directory* directory = &root;
        const std::string& path = files[i].first;
        std::size_t start = 0;
        for (std::size_t slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1)
        {
            directory = &directory->directories[path.substr(start, slash - start)];
        }
        directory->files[path.substr(start)] = i;
    }

    // The table's size doesn't depend on where the files go. A first pass with no offsets measures it. The files then
    // follow the table, 32-byte aligned.
    std::vector<uint64_t> offsets(files.size(), 0);
    std::vector<std::size_t> sizes;
    for (const auto& file : files)
    {
        sizes.push_back(file.second.size());
    }

    FstWriter measure(offsets, sizes);
    measure.Write(root, "", 0);
    uint64_t at = (kFstOffset + measure.Table().size() + 0x7fff) & ~uint64_t{0x7fff};
    for (std::size_t i = 0; i < files.size(); i++)
    {
        offsets[i] = at;
        files_.push_back({at, files[i].second});
        at = (at + files[i].second.size() + 0x1f) & ~uint64_t{0x1f};
    }

    FstWriter writer(offsets, sizes);
    writer.Write(root, "", 0);
    fst_ = writer.Table();
    fst_offset_ = kFstOffset;
    StoreBe32(&header_[0x424], static_cast<uint32_t>(fst_offset_ / 4));
    StoreBe32(&header_[0x428], static_cast<uint32_t>((fst_.size() + 3) / 4));
    StoreBe32(&header_[0x42c], static_cast<uint32_t>((fst_.size() + 3) / 4));
}

bool FileDisc::Read(uint64_t offset, void* out, std::size_t size)
{
    // The header, the table and the files, with zeros between them. Nothing comes after the last file.
    auto* bytes = static_cast<uint8_t*>(out);
    const uint64_t end = files_.empty() ? fst_offset_ + fst_.size() : files_.back().offset + files_.back().data.size();
    if (offset + size > std::max<uint64_t>(end, kHeaderSize))
    {
        return false;
    }

    std::memset(bytes, 0, size);

    const auto copy = [&](uint64_t start, std::span<const uint8_t> data)
    {
        const uint64_t from = std::max(offset, start);
        const uint64_t to = std::min(offset + size, start + data.size());
        if (from < to)
        {
            std::memcpy(bytes + (from - offset), data.data() + (from - start), to - from);
        }
    };

    copy(0, header_);
    copy(fst_offset_, fst_);
    for (const Placed& file : files_)
    {
        if (file.offset >= offset + size)
        {
            break;
        }

        copy(file.offset, file.data);
    }

    return true;
}

} // namespace wsf::wii
