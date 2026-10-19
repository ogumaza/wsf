// SPDX-License-Identifier: MIT

// xmp-wsf: XMPlay input plugin for WSF (.miniwsf), from xsflib's plugin template (xsflib/xmplay/input.h). The
// template's comment says what the plugin does. This file gives WSF's traits: its player, names, extensions and limits.
//
// The player core (src/wsf) does the emulation. Game mode runs the game's sound code on an emulated Wii. Archive mode
// runs WSF's model of the SDK's sound player. Both modes run the game's AX microcode on the emulated DSP.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"
#include "xsflib/xmplay/input.h"

#include "wsf/format.h"
#include "wsf/player.h"

namespace
{

struct Traits
{
    using Player = wsf::Player;
    using Set = wsf::LoadedSet;

    static constexpr const char* kName = "WSF";
    static constexpr std::array<const char*, 1> kExtensions = {"miniwsf"};
    static constexpr uint8_t kPsfVersion = wsf::kPsfVersionWsf;

    // The largest file read into memory: as large as a program may be. A .wsflib holds a game's sound archive. The
    // largest archives are hundreds of megabytes.
    static constexpr uint64_t kMaxFileSize = uint64_t{1} << 30;

    // The tags that arrange a sequence in archive mode: the tracks, the tempo and the variables.
    static constexpr std::array<const char*, 3> kPlaybackTags = {wsf::kTracksTag, wsf::kTempoTag, wsf::kVariablesTag};

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

} // namespace

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<Traits>::GetInterface(face, faceproc);
}
