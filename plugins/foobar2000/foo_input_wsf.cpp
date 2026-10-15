// SPDX-License-Identifier: MIT

// foo_input_wsf: foobar2000 input component for WSF (.miniwsf), from xsflib's input template
// (xsflib/foobar2000/input.h). The template's comment says what the component does. This file gives WSF's traits: its
// player, names, extensions, GUIDs and limits.
//
// The player core (src/wsf) does the emulation. Game mode runs the game's sound code on an emulated Wii. Archive mode
// runs WSF's model of the SDK's sound player. Both modes run the game's AX microcode on the emulated DSP.
//
// Build: see plugins/foobar2000/README.md (clang-cl on Windows, or Apple Clang on macOS, with the foobar2000 SDK).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/foobar2000/input.h"
#include "xsflib/psf/psf.h"
#include <SDK/foobar2000.h>

#include "wsf/format.h"
#include "wsf/player.h"

namespace
{

struct Traits
{
    using Player = wsf::Player;
    using Set = wsf::LoadedSet;

    static constexpr const char* kName = "WSF";
    static constexpr const char* kDecoderName = "WSF decoder";
    static constexpr std::array<const char*, 1> kExtensions = {"miniwsf"};
    static constexpr uint8_t kPsfVersion = wsf::kPsfVersionWsf;

    // The tags that arrange a sequence in archive mode. Editing or removing the tags keeps them as they are.
    static constexpr std::array<const char*, 3> kKeptTags = {wsf::kTempoTag, wsf::kTracksTag, wsf::kVariablesTag};

    // The largest file read into memory: as large as a program may be. A .wsflib holds a game's sound archive. The
    // largest archives are hundreds of megabytes.
    static constexpr t_filesize kMaxFileSize = t_filesize{1} << 30;

    // {A34DC260-7833-4B54-8DCC-750144AD1B92}
    static constexpr GUID kInputGuid = {0xa34dc260, 0x7833, 0x4b54, {0x8d, 0xcc, 0x75, 0x01, 0x44, 0xad, 0x1b, 0x92}};

    // Advanced preferences entries, under Decoding > WSF decoder.
    static constexpr GUID kBranchGuid = {0x314b08dd, 0x9806, 0x4128, {0x83, 0xf2, 0x39, 0x6f, 0xc3, 0x3e, 0x1c, 0xf5}};
    static constexpr GUID kLengthGuid = {0x37d0ea69, 0x4a53, 0x46ac, {0x99, 0xad, 0xbd, 0x74, 0xcd, 0x89, 0x0e, 0x66}};
    static constexpr GUID kFadeGuid = {0xd3112fc5, 0x414e, 0x4352, {0xb7, 0x86, 0xdb, 0xc9, 0x2d, 0xc1, 0xf2, 0xe6}};
    static constexpr GUID kEndlessGuid = {0xd98bb8a9, 0x9afb, 0x4392, {0xb9, 0xbe, 0x9e, 0x9e, 0x07, 0xf7, 0x2f, 0x2f}};
    static constexpr GUID kRenderAheadGuid = {
        0xa705e71b, 0xaced, 0x468f, {0xaf, 0x69, 0x5a, 0xbd, 0xe2, 0xe7, 0x23, 0xbc}};

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return wsf::LoadSet(path, reader, out, separators);
    }
};

xsflib::foobar2000::Settings<Traits> g_settings;
input_singletrack_factory_t<xsflib::foobar2000::Input<Traits>> g_input_wsf_factory;

} // namespace

DECLARE_FILE_TYPE("WSF files", "*.MINIWSF");

DECLARE_COMPONENT_VERSION("WSF Decoder", WSF_VERSION,
                          "Plays WSF (Nintendo Wii sequenced music) rips. The game's AX microcode runs on an emulated "
                          "DSP. In game mode, the game's sound code drives it on an emulated Wii. In archive mode, "
                          "WSF's model of the SDK's sound player drives it.\n\n"
                          "MIT licence. Uses zlib.");

VALIDATE_COMPONENT_FILENAME("foo_input_wsf.dll");
