// SPDX-License-Identifier: MIT

// AX's microcode, as linked into a game's main.dol. The microcode is 4096 words long and fills the DSP's instruction
// RAM. After two NOPs at the reset vector, its exception vectors are seven jumps to the top of IRAM (0x0Fxx).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wsf::ax
{

struct Microcode
{
    std::vector<uint16_t> words;
    uint32_t address = 0; // the cached address the game loads it from
};

// The size of the IRAM image of AX's DSP task.
constexpr std::size_t kMicrocodeWords = 0x1000;

// Finds AX's microcode in a main.dol. Returns nothing if the DOL has none.
std::optional<Microcode> FindMicrocode(std::span<const uint8_t> dol);

} // namespace wsf::ax
