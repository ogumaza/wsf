// SPDX-License-Identifier: MIT

// xmp-wsf: XMPlay input plugin for WSF (.miniwsf), from xsflib's plugin template (xsflib/xmplay/input.h). The
// template's comment says what the plugin does. plugin_traits.h gives WSF's traits.
//
// The player core (src/wsf) does the emulation. Game mode runs the game's sound code on an emulated Wii. Archive mode
// runs WSF's model of the SDK's sound player. Both modes run the game's AX microcode on the emulated DSP.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include "xsflib/xmplay/input.h"

#include "../plugin_traits.h"

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<wsf::PluginTraits>::GetInterface(face, faceproc);
}
