// SPDX-License-Identifier: MIT

// The search for a game's functions in the main.dol of another release, by their signatures (FunctionSignature).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/dol.h"

#include "rip/profiles.h"

namespace wsf::rip
{

// Finds functions of another main.dol in `dol` by their signatures: for each address in `addresses`, the place in `dol`
// of the function with the signature at the same index in `signatures`. A small function can match in several places.
// The functions of a library move together. The offset of the nearest function that matched once therefore picks the
// place. Returns the places, or nothing if a function isn't there.
std::optional<std::vector<uint32_t>> FindFunctions(const ntdlib::Dol& dol, std::span<const uint32_t> addresses,
                                                   std::span<const FunctionSignature> signatures);

} // namespace wsf::rip
