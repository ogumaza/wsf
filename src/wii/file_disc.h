// SPDX-License-Identifier: MIT

// A game partition made of files: its 0x440-byte header (sys/boot.bin), a file system table generated from the files'
// paths, and the files' data after the table. Game mode builds one from a rip's FILE chunks. The game then finds the
// files it requires, such as its sound archive, by their paths.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wii/system.h"

namespace wsf::wii
{

class FileDisc final : public Disc
{
public:
    // `header` is the partition's header. The table's offset and size in it are filled in. Each file is a path on the
    // disc and its data. Paths use '/' between names and have no leading '/' ("Sound/sound.brsar"). The data must
    // outlive the disc.
    FileDisc(std::vector<uint8_t> header, const std::vector<std::pair<std::string, std::span<const uint8_t>>>& files);

    bool Read(uint64_t offset, void* out, std::size_t size) override;

private:
    struct Placed
    {
        uint64_t offset;
        std::span<const uint8_t> data;
    };

    std::vector<uint8_t> header_;
    std::vector<uint8_t> fst_;
    uint64_t fst_offset_ = 0;
    std::vector<Placed> files_; // in order of their offsets
};

} // namespace wsf::wii
