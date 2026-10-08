// SPDX-License-Identifier: MIT

// WAV and FLAC writers for the command-line tools (16-bit stereo).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wsf
{

// Writes interleaved 16-bit stereo samples to a WAV file, or to a FLAC file when the name ends in .flac (ignoring
// case). Returns an error message on failure.
std::optional<std::string> WriteAudio(const std::string& path, const std::vector<int16_t>& pcm, uint32_t rate);

} // namespace wsf
