// SPDX-License-Identifier: MIT

// Address types for the emulated Wii.

#pragma once

#include <cstdint>

namespace wsf
{

// Physical address: MEM1 is at 0, MEM2 at 0x10000000.
using PAddr = uint32_t;

} // namespace wsf
