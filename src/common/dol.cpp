// SPDX-License-Identifier: MIT

#include "common/dol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "common/bytes.h"

namespace wsf
{
namespace
{

constexpr std::size_t kHeaderSize = 0x100;
constexpr int kSectionCount = 18; // 7 text, then 11 data

} // namespace

std::optional<Dol> Dol::Parse(std::span<const uint8_t> file)
{
    // The header holds the file offsets of the sections at 0x00, their load addresses at 0x48 and their sizes at 0x90.
    // The entry point is at 0xe0.
    if (file.size() < kHeaderSize)
    {
        return std::nullopt;
    }

    Dol dol;
    for (int i = 0; i < kSectionCount; i++)
    {
        const uint32_t offset = LoadBe32(&file[i * 4]);
        const uint32_t address = LoadBe32(&file[0x48 + i * 4]);
        const uint32_t size = LoadBe32(&file[0x90 + i * 4]);
        if (size == 0)
        {
            continue;
        }
        if (offset < kHeaderSize || offset > file.size() || size > file.size() - offset)
        {
            return std::nullopt;
        }

        dol.sections_.push_back({address, file.subspan(offset, size)});
    }

    dol.entry_ = LoadBe32(&file[0xe0]);

    return dol;
}

} // namespace wsf
