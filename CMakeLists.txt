# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# Shared version for wsfrip, wsfplay, the foobar2000 component, Windows file properties and macOS bundles
# (xsflib_add_version below). For a release, update it here and add notes in docs/release-notes/.
project(wsf VERSION 1.7.0 LANGUAGES C CXX)

# xsflib: the code that WSF shares with the other PSF players and with the converters, as a submodule. Its
# XsflibProject.cmake sets what the projects built on it start with: C++20, a release build by default, the static C
# runtime, the options that keep the output the same on every platform, and the warnings for WSF's targets
# (WSF_WARNINGS). It refuses the compilers whose output would differ from other builds. bzip2's header includes
# windows.h on Windows. NOMINMAX keeps windows.h from defining min and max as macros.
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
    message(FATAL_ERROR "externals/xsflib is missing. Run: git submodule update --init --recursive")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(CHECK_COMPILERS NAME WSF PREFIX WSF NOMINMAX)

# ntdlib: a library for the hardware and formats of Nintendo's consoles, as a second submodule. It builds on xsflib.
# xsflib provides the compression libraries that ntdlib's image formats require: zlib (GCZ), Zstandard (RVZ), liblzma
# (LZMA and LZMA2 in WIA and RVZ) and bzip2 (WIA and RVZ). It provides libFLAC for the command-line tools' FLAC output
# too.
set(XSFLIB_PSF ON)
set(XSFLIB_CLI ON)
set(XSFLIB_DISC_LIBRARIES ON)
set(NTDLIB_DISC ON)
xsflib_add_libraries(xsflib ntdlib)

# The include directory for all of src, xsflib's common files (byte order, SHA-1 and more) and ntdlib's (the
# GameCube's and the Wii's addresses, DOL executables and the builds of the Wii's sound code).
add_library(wsf_common INTERFACE)
target_include_directories(wsf_common INTERFACE src)
target_link_libraries(wsf_common INTERFACE xsflib::common ntdlib::common)

# AXFX: the RVL SDK's effects for AX's aux buses, modelled on the games' code. They must give the same floats on every
# platform: xsflib_project turns off the contraction of multiplies and adds into fused multiply-adds for every target,
# with clang-cl too.
add_library(wsf_axfx STATIC
    src/axfx/axfx.h
    src/axfx/chorus.cpp
    src/axfx/chorus.h
    src/axfx/delay.cpp
    src/axfx/delay.h
    src/axfx/reverb_hi.cpp
    src/axfx/reverb_hi.h
    src/axfx/reverb_std.cpp
    src/axfx/reverb_std.h
    src/axfx/util.h
)
target_link_libraries(wsf_axfx PUBLIC wsf_common)
target_compile_options(wsf_axfx PRIVATE ${WSF_WARNINGS})

# AX model: the CPU side of the RVL SDK's audio library.
add_library(wsf_ax STATIC
    src/ax/dsp_link.cpp
    src/ax/dsp_link.h
    src/ax/pb.cpp
    src/ax/pb.h
    src/ax/system.cpp
    src/ax/system.h
    src/ax/tables.h
    src/ax/voice.cpp
    src/ax/voice.h
)
target_link_libraries(wsf_ax PUBLIC wsf_common ntdlib::ax ntdlib::dsp PRIVATE ntdlib::flipper)
target_compile_options(wsf_ax PRIVATE ${WSF_WARNINGS})

# The Wii for game mode: Broadway, memory, the DSP and Hollywood's hardware.
add_library(wsf_wii STATIC
    src/wii/ios.cpp
    src/wii/ios.h
    src/wii/system.cpp
    src/wii/system.h
)
target_link_libraries(wsf_wii PUBLIC wsf_common ntdlib::dsp ntdlib::flipper ntdlib::ppc)
target_compile_options(wsf_wii PRIVATE ${WSF_WARNINGS})

# The WSF file format: the PSF container, chunks, descriptors and tags.
add_library(wsf_format STATIC
    src/wsf/format.cpp
    src/wsf/format.h
)
target_link_libraries(wsf_format PUBLIC wsf_common xsflib::psf)
target_compile_options(wsf_format PRIVATE ${WSF_WARNINGS})

# NW4R SND model: the sequence player, channels, envelopes and the voice layer above AX.
add_library(wsf_nw4r STATIC
    src/nw4r/arrangement.h
    src/nw4r/driver.cpp
    src/nw4r/driver.h
    src/nw4r/effect_setup.h
    src/nw4r/effects.cpp
    src/nw4r/effects.h
    src/nw4r/engine.cpp
    src/nw4r/engine.h
    src/nw4r/rsar_options.h
    src/nw4r/sequence.cpp
    src/nw4r/sequence.h
    src/nw4r/util.h
)
target_link_libraries(wsf_nw4r PUBLIC wsf_ax wsf_axfx ntdlib::nwsnd ntdlib::rsar)
target_compile_options(wsf_nw4r PRIVATE ${WSF_WARNINGS})

# The ripper: game- and archive-mode sets, the game profiles and the length analysis.
add_library(wsf_rip STATIC
    src/rip/driver_blob.h
    src/rip/galaxy_driver_blob.h
    src/rip/galaxy2_driver_blob.h
    src/rip/jaudio.cpp
    src/rip/jaudio_driver_blob.h
    src/rip/jungle_beat_driver_blob.h
    src/rip/nw4r.cpp
    src/rip/pikmin_driver_blob.h
    src/rip/pikmin2_driver_blob.h
    src/rip/profiles.cpp
    src/rip/rip.cpp
    src/rip/rip.h
    src/rip/sound_libraries.h
)
target_link_libraries(wsf_rip PUBLIC wsf_format wsf_nw4r ntdlib::disc ntdlib::flipper ntdlib::jaudio xsflib::rip
                              PRIVATE ntdlib::disc_containers)
target_compile_options(wsf_rip PRIVATE ${WSF_WARNINGS})

# The player: a set's audio, in game mode on the emulated Wii or in archive mode on the model.
add_library(wsf_player STATIC
    src/wsf/archive_player.cpp
    src/wsf/archive_player.h
    src/wsf/native_effects.cpp
    src/wsf/native_effects.h
    src/wsf/player.cpp
    src/wsf/player.h
)
find_package(Threads REQUIRED)
target_link_libraries(wsf_player PUBLIC wsf_format wsf_nw4r wsf_wii xsflib::player Threads::Threads)
target_compile_options(wsf_player PRIVATE ${WSF_WARNINGS})

add_executable(wsfplay src/cli/wsfplay.cpp)
target_link_libraries(wsfplay PRIVATE wsf_player xsflib::cli)
target_compile_options(wsfplay PRIVATE ${WSF_WARNINGS})
xsflib_add_version(wsfplay VERSION ${wsf_VERSION} DEFINE WSF_VERSION DESCRIPTION "WSF renderer" PRODUCT WSF
                   COPYRIGHT "Copyright (c) 2026 the WSF authors. MIT licence.")

# Game mode's length analysis and wave discovery for JAudio games: the rip's sets play on the player while ntdlib's
# analysis watches the game's code.
add_library(wsf_jaudio_analysis STATIC src/rip/jaudio_analysis.cpp src/rip/jaudio_analysis.h)
target_link_libraries(wsf_jaudio_analysis PUBLIC wsf_rip PRIVATE wsf_player)
target_compile_options(wsf_jaudio_analysis PRIVATE ${WSF_WARNINGS})

add_executable(wsfrip src/cli/wsfrip.cpp)
# wsfrip rips on several threads (--jobs). glibc before 2.34 keeps threads in a separate library.
target_link_libraries(wsfrip PRIVATE wsf_jaudio_analysis wsf_rip xsflib::cli xsflib::rip Threads::Threads)
target_compile_options(wsfrip PRIVATE ${WSF_WARNINGS})
xsflib_add_version(wsfrip VERSION ${wsf_VERSION} DEFINE WSF_VERSION DESCRIPTION "WSF ripper" PRODUCT WSF
                   COPYRIGHT "Copyright (c) 2026 the WSF authors. MIT licence.")

foreach (tool wsfrip wsfplay)
    # Windows: UTF-8 as the tools' code page. macOS: a droplet app for each tool (wsfrip.app, wsfplay.app).
    xsflib_add_utf8_manifest(${tool})
    xsflib_add_droplet(${tool} VERSION ${wsf_VERSION})
endforeach()

# foobar2000 component (Windows with clang-cl, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_wsf component when set)")
if (FOOBAR2000_SDK_DIR)
    if (NOT MSVC AND NOT APPLE)
        message(FATAL_ERROR "the foobar2000 component requires clang-cl on Windows, or macOS")
    endif()
    add_subdirectory(plugins/foobar2000)
endif()

# XMPlay plugin (32-bit Windows with clang-cl): cmake -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=<unpacked SDK> ...
set(XMPLAY_SDK_DIR "" CACHE PATH "XMPlay plugin SDK (builds the xmp-wsf plugin when set)")
if (XMPLAY_SDK_DIR)
    add_subdirectory(plugins/xmplay)
endif()

# The tests. -DWSF_TESTS=OFF leaves them out of a build that only makes the programs and plugins, such as CI's macOS
# job.
option(WSF_TESTS "Build WSF's tests and register them with CTest." ON)
if (WSF_TESTS)
    enable_testing()

    # The unit tests: wsf_<name>_test from tests/<name>_test.cpp, registered as wsf_<name>. The tests that build sound
    # archives use ntdlib's builders (ntdlib::testing).
    xsflib_add_project_test(wsf ax wsf_ax)
    xsflib_add_project_test(wsf format wsf_format)
    xsflib_add_project_test(wsf nw4r wsf_nw4r ntdlib::testing)
    xsflib_add_project_test(wsf rip wsf_rip ntdlib::testing)

    # The tools report the version they were built as.
    string(REPLACE "." "\\." version_pattern ${wsf_VERSION})
    foreach (tool wsfrip wsfplay)
        add_test(NAME version_${tool} COMMAND ${tool} --version)
        set_tests_properties(version_${tool} PROPERTIES PASS_REGULAR_EXPRESSION "^${tool} ${version_pattern}[\r\n]*$")
    endforeach()

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()
