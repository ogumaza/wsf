// SPDX-License-Identifier: MIT

// The rules WSF reads sound archives with.

#pragma once

#include "ntdlib/rsar/formats.h"

namespace wsf::nw4r
{

// WSF's texts, names as the archive has them, every field that playback uses, and the type of a sound in an archive
// before version 1.1 from the sound table.
inline constexpr ntdlib::rsar::ReadOptions kRsarOptions = {
    .wording = ntdlib::rsar::Wording::kSentences,
    .printable_names = false,
    .playback_fields = true,
    .type_from_detail = false,
};

} // namespace wsf::nw4r
