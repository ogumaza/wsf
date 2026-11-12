// SPDX-License-Identifier: MIT

// The search for functions by their signatures (see dol_search.h).

#include "rip/dol_search.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/dol.h"
#include "xsflib/bytes.h"

namespace wsf::rip
{
namespace
{

// Opcodes of addi, addis and the D-form loads and stores. The linker may fill in their immediates.
bool IsDForm(uint32_t opcode)
{
    return opcode == 14 || opcode == 15 || (opcode >= 32 && opcode <= 55);
}

// A word of code with the fields that the linker fills in cleared: a branch's target, and the immediate of an addis or
// of a load, store or add that doesn't address the stack (r1).
uint32_t MaskedWord(uint32_t word)
{
    const uint32_t opcode = word >> 26;
    if (opcode == 18)
    {
        return word & 0xfc000003;
    }
    if (IsDForm(opcode) && (opcode == 15 || (word >> 16 & 31) != 1))
    {
        return word & 0xffff0000;
    }

    return word;
}

// The FNV-1a hash of `code`'s masked words, in big-endian order.
uint64_t MaskedHash(std::span<const uint8_t> code)
{
    uint64_t hash = 0xcbf29ce484222325;
    for (std::size_t i = 0; i + 4 <= code.size(); i += 4)
    {
        const uint32_t word = MaskedWord(xsflib::LoadBe32(&code[i]));
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            hash ^= word >> shift & 0xff;
            hash *= 0x100000001b3;
        }
    }

    return hash;
}

} // namespace

std::optional<std::vector<uint32_t>> FindFunctions(const ntdlib::Dol& dol, std::span<const uint32_t> addresses,
                                                   std::span<const FunctionSignature> signatures)
{
    // The places whose code matches each function's signature.
    std::vector<std::vector<uint32_t>> places(addresses.size());
    for (const ntdlib::Dol::Section& section : dol.Sections())
    {
        for (std::size_t at = 0; at + 4 <= section.data.size(); at += 4)
        {
            const uint32_t first = MaskedWord(xsflib::LoadBe32(&section.data[at]));
            for (std::size_t i = 0; i < addresses.size(); i++)
            {
                const FunctionSignature& signature = signatures[i];
                if (first == signature.first && at + signature.size <= section.data.size() &&
                    MaskedHash(section.data.subspan(at, signature.size)) == signature.hash)
                {
                    places[i].push_back(section.address + static_cast<uint32_t>(at));
                }
            }
        }
    }

    // How far each function that matched once moved.
    std::vector<std::optional<int64_t>> moved(addresses.size());
    for (std::size_t i = 0; i < addresses.size(); i++)
    {
        if (places[i].empty())
        {
            return std::nullopt;
        }

        if (places[i].size() == 1)
        {
            moved[i] = int64_t{places[i][0]} - addresses[i];
        }
    }

    std::vector<uint32_t> found(addresses.size());
    for (std::size_t i = 0; i < addresses.size(); i++)
    {
        // A function that matched in several places moved as far as the nearest function that matched once.
        const uint32_t address = addresses[i];
        const auto distance = [addresses, address](std::size_t k)
        {
            return std::abs(int64_t{addresses[k]} - address);
        };

        std::optional<std::size_t> nearest;
        for (std::size_t j = 0; j < addresses.size(); j++)
        {
            if (moved[j] && (!nearest || distance(j) < distance(*nearest)))
            {
                nearest = j;
            }
        }
        if (!nearest)
        {
            return std::nullopt;
        }

        const int64_t offset = moved[i] ? *moved[i] : *moved[*nearest];
        const auto place = std::find(places[i].begin(), places[i].end(), static_cast<uint32_t>(address + offset));
        if (place == places[i].end())
        {
            return std::nullopt;
        }

        found[i] = *place;
    }

    return found;
}

} // namespace wsf::rip
