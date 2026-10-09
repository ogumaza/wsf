// SPDX-License-Identifier: MIT

#include "ax/microcode.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "common/bytes.h"
#include "common/dol.h"

namespace wsf::ax
{
namespace
{

// Two NOPs, then seven jumps (0x029F) to 0x0Fxx.
bool IsAxVectors(std::span<const uint8_t> words)
{
    if (LoadBe32(words.data()) != 0)
    {
        return false;
    }

    for (int i = 0; i < 7; i++)
    {
        const uint8_t* jump = words.data() + 4 + i * 4;
        if (LoadBe16(jump) != 0x029f || (LoadBe16(jump + 2) & 0xff00) != 0x0f00)
        {
            return false;
        }
    }

    return true;
}

} // namespace

std::optional<Microcode> FindMicrocode(std::span<const uint8_t> file)
{
    const std::optional<Dol> dol = Dol::Parse(file);
    if (!dol)
    {
        return std::nullopt;
    }

    // The microcode is stored as data in one of the DOL's sections. The search assumes that it starts on an 8-byte
    // boundary.
    constexpr std::size_t kBytes = kMicrocodeWords * 2;
    for (const Dol::Section& section : dol->Sections())
    {
        for (std::size_t at = 0; at + kBytes <= section.data.size(); at += 8)
        {
            if (!IsAxVectors(section.data.subspan(at, 32)))
            {
                continue;
            }

            Microcode microcode;
            microcode.address = section.address + static_cast<uint32_t>(at);
            microcode.words.resize(kMicrocodeWords);
            for (std::size_t i = 0; i < kMicrocodeWords; i++)
            {
                microcode.words[i] = LoadBe16(&section.data[at + i * 2]);
            }

            return microcode;
        }
    }

    return std::nullopt;
}

Build IdentifyBuild(const Microcode& microcode)
{
    // The microcode reads each voice's PB by DMA. It sets the DMA's length with SI @DSBL, #length (0x16CB, then the
    // length in bytes).
    bool reads_0x120 = false;
    bool reads_0x140 = false;
    for (std::size_t i = 0; i + 1 < microcode.words.size(); i++)
    {
        if (microcode.words[i] == 0x16cb)
        {
            reads_0x120 |= microcode.words[i + 1] == kPbSizeOctober2006;
            reads_0x140 |= microcode.words[i + 1] == kPbSize;
        }
    }

    return reads_0x120 && !reads_0x140 ? Build::kOctober2006 : Build::kDecember2006;
}

} // namespace wsf::ax
