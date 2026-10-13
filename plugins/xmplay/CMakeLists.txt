# SPDX-License-Identifier: MIT

# xmp-wsf: the XMPlay input plugin, from xsflib's template (xsflib_add_xmplay_plugin). It's a 32-bit DLL built with
# clang-cl. XMPlay is a 32-bit program. The top-level CMakeLists.txt adds it when XMPLAY_SDK_DIR is set (see README.md).

xsflib_add_xmplay_plugin(xmp-wsf SDK ${XMPLAY_SDK_DIR} SOURCES xmp_wsf.cpp LIBRARIES wsf_player OPTIONS ${WSF_WARNINGS})
xsflib_add_version(xmp-wsf VERSION ${wsf_VERSION} DEFINE WSF_VERSION DESCRIPTION "WSF plugin for XMPlay" PRODUCT WSF
                   COPYRIGHT "Copyright (c) 2026 the WSF authors. MIT licence.")
