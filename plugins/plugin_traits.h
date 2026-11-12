// SPDX-License-Identifier: MIT

// The traits that the foobar2000 component and the XMPlay plugin share, for xsflib's plugin templates
// (xsflib/foobar2000/input.h and xsflib/xmplay/input.h): WSF's player, names, extensions, limits, version and about
// text. The foobar2000 component's traits derive from PluginTraits. The XMPlay plugin uses PluginTraits as they are.
// The plugin's CMake target defines WSF_VERSION.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"

#include "wsf/format.h"
#include "wsf/player.h"

namespace wsf
{

struct PluginTraits
{
    using Player = wsf::Player;
    using Set = wsf::LoadedSet;

    static constexpr const char* kName = "WSF";
    static constexpr std::array<const char*, 1> kExtensions = {"miniwsf"};
    static constexpr uint8_t kPsfVersion = kPsfVersionWsf;

    // The largest file read into memory: as large as a program may be. A .wsflib holds a game's sound archive. The
    // largest archives are hundreds of megabytes.
    static constexpr uint64_t kMaxFileSize = uint64_t{1} << 30;

    // The tags that arrange a sequence in archive mode: the tracks, the tempo and the variables.
    static constexpr std::array<const char*, 3> kPlaybackTags = {kTracksTag, kTempoTag, kVariablesTag};

    static constexpr const char* kVersion = WSF_VERSION;
    static constexpr const char* kAbout =
        "Plays WSF (Nintendo Wii sequenced music) rips. The game's AX microcode runs on an emulated DSP. In game mode, "
        "the game's sound code drives it on an emulated Wii. In archive mode, WSF's model of the SDK's sound player "
        "drives it.\n\n"
        "MIT licence. Uses zlib.";

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return wsf::LoadSet(path, reader, out, separators);
    }
};

} // namespace wsf
