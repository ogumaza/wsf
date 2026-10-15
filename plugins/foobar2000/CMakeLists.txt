# SPDX-License-Identifier: MIT

# foo_input_wsf: the foobar2000 component, from xsflib's template (xsflib_add_foobar2000_component). It's a DLL built
# with clang-cl on Windows. On macOS it's a .component bundle. The top-level CMakeLists.txt adds it when
# FOOBAR2000_SDK_DIR is set (see README.md).

# On macOS the component compiles the SDK's Objective-C++ files. CMake enables a language only at a file's top level.
if (APPLE)
    enable_language(OBJCXX)
endif()

xsflib_add_foobar2000_component(foo_input_wsf SDK ${FOOBAR2000_SDK_DIR} SOURCES foo_input_wsf.cpp LIBRARIES wsf_player
                                VERSION ${wsf_VERSION} BUNDLE_ID com.foobar2000.foo-input-wsf OPTIONS ${WSF_WARNINGS})
xsflib_add_version(foo_input_wsf VERSION ${wsf_VERSION} DEFINE WSF_VERSION DESCRIPTION "WSF decoder for foobar2000"
                   PRODUCT WSF COPYRIGHT "Copyright (c) 2026 the WSF authors. MIT licence.")
