// SPDX-License-Identifier: MIT

// AX's microcode in a game's main.dol. The microcode is 4096 words long and fills the DSP's instruction RAM. After two
// NOPs at the reset vector, its exception vectors are seven jumps to the top of IRAM (0x0Fxx).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ax/pb.h"

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

// The build of AX that `microcode` comes with, by the size of the PBs that it reads: 0x120 bytes in the build of
// October 2006, and 0x140 bytes in the build of December 2006 and the later ones.
Build IdentifyBuild(const Microcode& microcode);

} // namespace wsf::ax
