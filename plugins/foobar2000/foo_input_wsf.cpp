// SPDX-License-Identifier: MIT

// foo_input_wsf: foobar2000 input component for WSF (.miniwsf), from xsflib's input template
// (xsflib/foobar2000/input.h). The template's comment says what the component does. This file gives the decoder's name
// and GUIDs. plugin_traits.h gives WSF's other traits.
//
// The player core (src/wsf) does the emulation. Game mode runs the game's sound code on an emulated Wii. Archive mode
// runs the model of the SDK sound player. Both modes run the DSP program of the game (the AX microcode, or the JAudio
// program) on the emulated DSP.
//
// Build: see plugins/foobar2000/README.md (clang-cl on Windows, or Apple Clang on macOS, with the foobar2000 SDK).

#include "xsflib/foobar2000/input.h"
#include <SDK/foobar2000.h>

#include "../plugin_traits.h"

namespace
{

struct Traits : wsf::PluginTraits
{
    static constexpr const char* kDecoderName = "WSF decoder";

    // {A34DC260-7833-4B54-8DCC-750144AD1B92}
    static constexpr GUID kInputGuid = {0xa34dc260, 0x7833, 0x4b54, {0x8d, 0xcc, 0x75, 0x01, 0x44, 0xad, 0x1b, 0x92}};

    // Advanced preferences entries, under Decoding > WSF decoder.
    static constexpr GUID kBranchGuid = {0x314b08dd, 0x9806, 0x4128, {0x83, 0xf2, 0x39, 0x6f, 0xc3, 0x3e, 0x1c, 0xf5}};
    static constexpr GUID kLengthGuid = {0x37d0ea69, 0x4a53, 0x46ac, {0x99, 0xad, 0xbd, 0x74, 0xcd, 0x89, 0x0e, 0x66}};
    static constexpr GUID kFadeGuid = {0xd3112fc5, 0x414e, 0x4352, {0xb7, 0x86, 0xdb, 0xc9, 0x2d, 0xc1, 0xf2, 0xe6}};
    static constexpr GUID kEndlessGuid = {0xd98bb8a9, 0x9afb, 0x4392, {0xb9, 0xbe, 0x9e, 0x9e, 0x07, 0xf7, 0x2f, 0x2f}};
    static constexpr GUID kRenderAheadGuid = {
        0xa705e71b, 0xaced, 0x468f, {0xaf, 0x69, 0x5a, 0xbd, 0xe2, 0xe7, 0x23, 0xbc}};
    static constexpr GUID kFadeExponentGuid = {
        0xd6182b0b, 0x319c, 0x4e84, {0x97, 0x7d, 0x33, 0x80, 0x96, 0xb4, 0x75, 0x97}};

    // The settings that play a track as an album does (xsflib/player/settings.h).
    static constexpr GUID kSkipSilenceGuid = {
        0xa24ead7b, 0xa311, 0x40f5, {0xa3, 0x10, 0x4a, 0x9a, 0xd1, 0xc2, 0x8b, 0xf5}};
    static constexpr GUID kStartSilenceGuid = {
        0x7a0e6f07, 0x50c2, 0x4fae, {0xa5, 0x42, 0x56, 0xf1, 0x79, 0x29, 0x1e, 0x0c}};
    static constexpr GUID kLoopsGuid = {0xf0011682, 0x6cf6, 0x4eb7, {0xab, 0x6f, 0xb5, 0x2a, 0x95, 0xd8, 0x0c, 0x0e}};
    static constexpr GUID kLongLoopGuid = {
        0xf5738db7, 0xb9cd, 0x44cd, {0xbb, 0xce, 0x92, 0x74, 0xae, 0x16, 0x31, 0xae}};
    static constexpr GUID kLoopFadeGuid = {
        0xc0ce354e, 0xaa10, 0x486b, {0x91, 0x30, 0x1d, 0xd0, 0x94, 0x47, 0x7d, 0x7b}};
    static constexpr GUID kEndSilenceGuid = {
        0xa40b4520, 0x98fe, 0x40a5, {0x91, 0x0d, 0x1e, 0x61, 0x0b, 0xa8, 0xcb, 0x45}};
    static constexpr GUID kIgnoreTagsGuid = {
        0xc6053553, 0x4eaa, 0x4a4b, {0x83, 0x81, 0xa1, 0xa1, 0x55, 0x85, 0xe5, 0x5a}};
    static constexpr GUID kSkipEndGuid = {0x301f0696, 0x9236, 0x47df, {0xa8, 0x52, 0x33, 0x2f, 0x9b, 0xa5, 0x0f, 0x3c}};
};

xsflib::foobar2000::Settings<Traits> g_settings;
input_singletrack_factory_t<xsflib::foobar2000::Input<Traits>> g_input_wsf_factory;

} // namespace

DECLARE_FILE_TYPE("WSF files", "*.MINIWSF");

DECLARE_COMPONENT_VERSION("WSF Decoder", wsf::PluginTraits::kVersion, wsf::PluginTraits::kAbout);

VALIDATE_COMPONENT_FILENAME("foo_input_wsf.dll");
