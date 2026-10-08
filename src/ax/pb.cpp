// SPDX-License-Identifier: MIT

#include "ax/pb.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "common/bytes.h"

namespace wsf::ax
{

static_assert(std::is_trivially_copyable_v<Pb>);

Pb LoadPb(const uint8_t* bytes)
{
    // A PB contains only 16-bit words. The byte order is converted one word at a time.
    std::array<uint16_t, kPbSize / 2> words;
    for (std::size_t i = 0; i < words.size(); i++)
    {
        words[i] = LoadBe16(bytes + i * 2);
    }

    return std::bit_cast<Pb>(words);
}

void StorePb(const Pb& pb, uint8_t* bytes)
{
    const auto words = std::bit_cast<std::array<uint16_t, kPbSize / 2>>(pb);
    for (std::size_t i = 0; i < words.size(); i++)
    {
        StoreBe16(bytes + i * 2, words[i]);
    }
}

} // namespace wsf::ax
