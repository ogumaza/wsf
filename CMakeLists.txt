# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# Shared version for wsfrip, wsfplay, the foobar2000 component, Windows file properties and macOS bundles
# (wsf_add_version below). For a release, update it here and add notes in docs/release-notes/.
project(wsf VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Reject unsupported compilers. Windows builds require clang-cl or MSVC. On 32-bit x86, GCC and Clang use x87 floating
# point with extra precision. Their output would then differ from other builds. clang-cl and MSVC use SSE2 there.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(compiler_name GCC)
else()
    set(compiler_name ${CMAKE_CXX_COMPILER_ID})
endif()
if (WIN32 AND NOT MSVC)
    message(FATAL_ERROR "WSF doesn't support ${compiler_name} on Windows, such as MinGW's GCC or the GNU-style "
                        "clang++. Build with Visual Studio's clang-cl (cmake -T ClangCL) or with MSVC; README.md has "
                        "the steps.")
endif()
if (NOT MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86|x86_64|AMD64)$")
    message(FATAL_ERROR "WSF doesn't support 32-bit x86 builds with ${compiler_name}: their output would differ from "
                        "other builds. Build for 64-bit instead, or on Windows use clang-cl or MSVC, which build "
                        "32-bit x86 with SSE2.")
endif()
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# MSVC: link the static C runtime so that a plugin requires no runtime DLLs. This also applies in subprojects that
# declare an older minimum CMake version.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
# clang-cl takes MSVC's options but is Clang underneath. It gets Clang's settings below through /clang:. It ignores
# MSVC's /d1trimfile.
if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(WSF_CLANG_CL ON)
endif()
# Output must not depend on the platform. Turn off fused multiply-add contraction so that every build computes the same
# floats. On CPUs with FMA, such as Apple Silicon, Clang contracts by default. MSVC's /fp:precise doesn't contract.
if (WSF_CLANG_CL)
    add_compile_options(/clang:-ffp-contract=off)
    # clang-cl does 32-bit x86 floating point on the x87 unit unless it's given SSE2. MSVC uses SSE2 there by default.
    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_compile_options(/arch:SSE2)
    endif()
elseif (NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()
# __FILE__ gives paths relative to the source or build directory. Debug information keeps absolute paths so that
# debuggers find the sources. MSVC's /d1trimfile isn't documented. Visual Studio 2019 16.4 and later support it.
if (WSF_CLANG_CL)
    add_compile_options("/clang:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/="
                        "/clang:-fmacro-prefix-map=${CMAKE_BINARY_DIR}/=")
elseif (MSVC)
    cmake_path(NATIVE_PATH CMAKE_SOURCE_DIR source_dir)
    cmake_path(NATIVE_PATH CMAKE_BINARY_DIR binary_dir)
    add_compile_options(/d1trimfile:${source_dir} /d1trimfile:${binary_dir})
else()
    add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/= -fmacro-prefix-map=${CMAKE_BINARY_DIR}/=)
endif()

if (MSVC)
    set(WSF_WARNINGS /W4)
    # fopen and freopen are standard C++. The _s versions that MSVC suggests instead aren't portable.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(WSF_WARNINGS -Wall -Wextra)
endif()

# Windows: bzip2's header includes windows.h. Unless NOMINMAX is defined, windows.h defines min and max as macros. Those
# macros break std::min, std::max and std::numeric_limits::max.
if (WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# The compression libraries that the image formats require: zlib (GCZ), Zstandard (RVZ), liblzma (LZMA and LZMA2 in WIA
# and RVZ) and bzip2 (WIA and RVZ). Installed libraries are used where they exist. Otherwise each library is downloaded
# and built with WSF. A universal macOS build (more than one architecture in CMAKE_OSX_ARCHITECTURES) builds Zstandard
# and liblzma from source. Installed copies, such as Homebrew's, are built for the Mac's architecture only. zlib and
# bzip2 come with macOS for every architecture.
include(FetchContent)
list(LENGTH CMAKE_OSX_ARCHITECTURES osx_architectures)

find_package(ZLIB QUIET)
if (NOT ZLIB_FOUND)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    # Build only the static library. zlib's DLL would be built by default too, and its resource script doesn't
    # compile with clang-cl.
    set_property(DIRECTORY ${zlib_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    target_include_directories(zlibstatic PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

if (osx_architectures LESS 2)
    find_package(zstd CONFIG QUIET)
endif()
if (TARGET zstd::libzstd)
    set(WSF_ZSTD zstd::libzstd)
else()
    FetchContent_Declare(zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.7
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake)
    foreach (option ZSTD_BUILD_PROGRAMS ZSTD_BUILD_TESTS ZSTD_BUILD_SHARED ZSTD_LEGACY_SUPPORT ZSTD_MULTITHREAD_SUPPORT)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
    set_property(DIRECTORY ${zstd_SOURCE_DIR}/build/cmake PROPERTY EXCLUDE_FROM_ALL TRUE)
    # Only in the build: zstd's project installs the target. An installed target can't name the build folder.
    target_include_directories(libzstd_static INTERFACE $<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib>)
    set(WSF_ZSTD libzstd_static)
endif()

if (osx_architectures LESS 2)
    find_package(LibLZMA QUIET)
endif()
if (LIBLZMA_FOUND)
    set(WSF_LZMA LibLZMA::LibLZMA)
else()
    FetchContent_Declare(xz
        GIT_REPOSITORY https://github.com/tukaani-project/xz.git
        GIT_TAG v5.8.1
        GIT_SHALLOW TRUE)
    foreach (option XZ_TOOL_XZ XZ_TOOL_XZDEC XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO XZ_TOOL_SCRIPTS XZ_DOC XZ_NLS
                    BUILD_TESTING)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_MakeAvailable(xz)
    set_property(DIRECTORY ${xz_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    # xz's project gives the header folder only to the library's sources. As for zstd, the library's users get the
    # folder here, in the build only.
    target_include_directories(liblzma INTERFACE $<BUILD_INTERFACE:${xz_SOURCE_DIR}/src/liblzma/api>)
    set(WSF_LZMA liblzma)
endif()

find_package(BZip2 QUIET)
if (BZIP2_FOUND)
    set(WSF_BZIP2 BZip2::BZip2)
else()
    # bzip2 1.0.8 has no CMake build. Its library is built from its sources here.
    FetchContent_Declare(bzip2
        URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
        URL_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(bzip2)
    add_library(wsf_bzip2 STATIC
        ${bzip2_SOURCE_DIR}/blocksort.c
        ${bzip2_SOURCE_DIR}/bzlib.c
        ${bzip2_SOURCE_DIR}/compress.c
        ${bzip2_SOURCE_DIR}/crctable.c
        ${bzip2_SOURCE_DIR}/decompress.c
        ${bzip2_SOURCE_DIR}/huffman.c
        ${bzip2_SOURCE_DIR}/randtable.c
    )
    target_include_directories(wsf_bzip2 PUBLIC ${bzip2_SOURCE_DIR})
    if (MSVC)
        target_compile_options(wsf_bzip2 PRIVATE /w)
    endif()
    set(WSF_BZIP2 wsf_bzip2)
endif()

# Small helpers the other libraries share (byte order, SHA-1), and the include directory for all of src.
add_library(wsf_common STATIC
    src/common/bytes.h
    src/common/common_types.h
    src/common/dol.cpp
    src/common/dol.h
    src/common/sha1.cpp
    src/common/sha1.h
)
target_include_directories(wsf_common PUBLIC src)
target_compile_options(wsf_common PRIVATE ${WSF_WARNINGS})

# Disc images in every common format, and the files of a game's partition.
add_library(wsf_disc STATIC
    src/disc/compression.cpp
    src/disc/compression.h
    src/disc/game.cpp
    src/disc/game.h
    src/disc/image.cpp
    src/disc/image.h
    src/disc/junk.cpp
    src/disc/junk.h
)
target_link_libraries(wsf_disc PUBLIC wsf_common PRIVATE ZLIB::ZLIB ${WSF_ZSTD} ${WSF_LZMA} ${WSF_BZIP2})
target_compile_options(wsf_disc PRIVATE ${WSF_WARNINGS})

# The NintendoWare for Revolution sound formats: RSAR, RSEQ, RBNK, RWAR and RWAV.
add_library(wsf_rsar STATIC
    src/rsar/binary.h
    src/rsar/formats.cpp
    src/rsar/formats.h
)
target_link_libraries(wsf_rsar PUBLIC wsf_common)
target_compile_options(wsf_rsar PRIVATE ${WSF_WARNINGS})

# The DSP and the main memory it shares with the CPU. The DSP runs AX's microcode.
add_library(wsf_dsp STATIC
    src/dsp/boot_rom.cpp
    src/dsp/coef_rom.h
    src/dsp/dsp.cpp
    src/dsp/dsp.h
    src/dsp/hardware.cpp
    src/dsp/instructions.cpp
    src/dsp/internal.h
    src/dsp/main_memory.h
)
target_link_libraries(wsf_dsp PUBLIC wsf_common)
target_compile_options(wsf_dsp PRIVATE ${WSF_WARNINGS})
# One translation unit so that the compiler can inline the small register and flag helpers into the instructions.
set_target_properties(wsf_dsp PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 0)
# MSVC: without /bigobj, the instances of the instruction handlers in one object can exceed COFF's limit of sections.
if (MSVC)
    target_compile_options(wsf_dsp PRIVATE /bigobj)
endif()

# AX model: the CPU side of the RVL SDK's audio library.
add_library(wsf_ax STATIC
    src/ax/dsp_link.cpp
    src/ax/dsp_link.h
    src/ax/microcode.cpp
    src/ax/microcode.h
    src/ax/pb.cpp
    src/ax/pb.h
    src/ax/system.cpp
    src/ax/system.h
    src/ax/tables.h
    src/ax/voice.cpp
    src/ax/voice.h
)
target_link_libraries(wsf_ax PUBLIC wsf_dsp)
target_compile_options(wsf_ax PRIVATE ${WSF_WARNINGS})

# The Broadway interpreter, for game mode. Its floating point follows FPSCR's rounding mode at run time.
add_library(wsf_ppc STATIC
    src/ppc/cpu.cpp
    src/ppc/cpu.h
    src/ppc/fpu.cpp
)
target_link_libraries(wsf_ppc PUBLIC wsf_common)
target_compile_options(wsf_ppc PRIVATE ${WSF_WARNINGS})
if (MSVC)
    set_source_files_properties(src/ppc/fpu.cpp PROPERTIES COMPILE_OPTIONS /fp:strict)
else()
    set_source_files_properties(src/ppc/fpu.cpp PROPERTIES COMPILE_OPTIONS -frounding-math)
endif()

# The Wii for game mode: Broadway, memory, the DSP and Hollywood's hardware.
add_library(wsf_wii STATIC
    src/wii/dsp_interface.cpp
    src/wii/dsp_interface.h
    src/wii/exi.cpp
    src/wii/exi.h
    src/wii/file_disc.cpp
    src/wii/file_disc.h
    src/wii/ios.cpp
    src/wii/ios.h
    src/wii/system.cpp
    src/wii/system.h
)
target_link_libraries(wsf_wii PUBLIC wsf_dsp wsf_ppc)
target_compile_options(wsf_wii PRIVATE ${WSF_WARNINGS})

# The WSF file format: the PSF container, chunks, descriptors and tags.
add_library(wsf_format STATIC
    src/common/ascii.h
    src/wsf/format.cpp
    src/wsf/format.h
)
target_link_libraries(wsf_format PUBLIC wsf_common PRIVATE ZLIB::ZLIB)
target_compile_options(wsf_format PRIVATE ${WSF_WARNINGS})

# NW4R SND model: the sequence player, channels, envelopes and the voice layer above AX.
add_library(wsf_nw4r STATIC
    src/nw4r/driver.cpp
    src/nw4r/driver.h
    src/nw4r/engine.cpp
    src/nw4r/engine.h
    src/nw4r/envelope.cpp
    src/nw4r/envelope.h
    src/nw4r/sequence.cpp
    src/nw4r/sequence.h
    src/nw4r/tables.h
    src/nw4r/util.cpp
    src/nw4r/util.h
)
target_link_libraries(wsf_nw4r PUBLIC wsf_ax wsf_rsar)
target_compile_options(wsf_nw4r PRIVATE ${WSF_WARNINGS})

# The ripper: game- and archive-mode sets, the game profiles and the length analysis.
add_library(wsf_rip STATIC
    src/rip/driver_blob.h
    src/rip/rip.cpp
    src/rip/rip.h
)
target_link_libraries(wsf_rip PUBLIC wsf_format wsf_nw4r wsf_disc)
target_compile_options(wsf_rip PRIVATE ${WSF_WARNINGS})

# The player: a set's audio, in game mode on the emulated Wii or in archive mode on the model.
add_library(wsf_player STATIC
    src/wsf/archive_player.cpp
    src/wsf/archive_player.h
    src/wsf/playback.cpp
    src/wsf/playback.h
    src/wsf/player.cpp
    src/wsf/player.h
    src/wsf/render_ahead.cpp
    src/wsf/render_ahead.h
)
find_package(Threads REQUIRED)
target_link_libraries(wsf_player PUBLIC wsf_format wsf_nw4r wsf_wii Threads::Threads)
target_compile_options(wsf_player PRIVATE ${WSF_WARNINGS})

# Sets WSF_VERSION and, on Windows, adds the version resource shown in Explorer's file properties.
function(wsf_add_version target description)
    target_compile_definitions(${target} PRIVATE WSF_VERSION="${wsf_VERSION}")
    if (WIN32)
        get_target_property(type ${target} TYPE)
        if (type STREQUAL "EXECUTABLE")
            set(VERSION_FILE_TYPE VFT_APP)
            set(VERSION_FILE_NAME ${target}${CMAKE_EXECUTABLE_SUFFIX})
        else()
            set(VERSION_FILE_TYPE VFT_DLL)
            set(VERSION_FILE_NAME ${target}${CMAKE_SHARED_LIBRARY_SUFFIX})
        endif()
        set(VERSION_DESCRIPTION ${description})
        set(VERSION_NAME ${target})
        set(rc ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc)
        configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/platform/windows/version.rc.in ${rc} @ONLY)
        target_sources(${target} PRIVATE ${rc})
    endif()
endfunction()

# WAV/FLAC output and the console for the command-line tools. libFLAC comes from its CMake package (vcpkg and other
# CMake-built installs), then from pkg-config (Homebrew and Linux distributions). If neither has it, it's fetched and
# built from source. A universal macOS build (more than one architecture in CMAKE_OSX_ARCHITECTURES) always builds it.
# An installed libFLAC, such as Homebrew's, is built for the Mac's architecture only.
add_library(wsf_cli STATIC src/cli/audio_out.cpp src/cli/audio_out.h src/cli/console.cpp src/cli/console.h)
target_link_libraries(wsf_cli PUBLIC wsf_common)
target_compile_options(wsf_cli PRIVATE ${WSF_WARNINGS})
list(LENGTH CMAKE_OSX_ARCHITECTURES osx_architectures)
if (osx_architectures LESS 2)
    find_package(FLAC CONFIG QUIET)
    if (FLAC_FOUND)
        set(WSF_FLAC FLAC::FLAC)
    else()
        find_package(PkgConfig QUIET)
        if (PKG_CONFIG_FOUND)
            pkg_check_modules(FLAC QUIET IMPORTED_TARGET flac)
        endif()
        if (FLAC_FOUND)
            set(WSF_FLAC PkgConfig::FLAC)
        endif()
    endif()
endif()
if (NOT WSF_FLAC)
    include(FetchContent)
    FetchContent_Declare(flac
        GIT_REPOSITORY https://github.com/xiph/flac.git
        GIT_TAG 1.5.0
        GIT_SHALLOW TRUE)
    foreach (option BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_TESTING BUILD_DOCS INSTALL_MANPAGES
                    INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE WITH_OGG)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_MakeAvailable(flac)
    # Build only libFLAC: the helper libraries of flac's programs would be built by default too.
    set_property(DIRECTORY ${flac_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    set(WSF_FLAC FLAC::FLAC)
endif()
target_link_libraries(wsf_cli PRIVATE ${WSF_FLAC})

add_executable(wsfplay src/cli/wsfplay.cpp)
target_link_libraries(wsfplay PRIVATE wsf_player wsf_cli)
target_compile_options(wsfplay PRIVATE ${WSF_WARNINGS})
wsf_add_version(wsfplay "WSF renderer")

add_executable(wsfrip src/cli/wsfrip.cpp)
# wsfrip rips on several threads (--jobs). glibc before 2.34 keeps threads in a separate library.
target_link_libraries(wsfrip PRIVATE wsf_rip wsf_cli Threads::Threads)
target_compile_options(wsfrip PRIVATE ${WSF_WARNINGS})
wsf_add_version(wsfrip "WSF ripper")

foreach (tool wsfrip wsfplay)
    # Windows: UTF-8 as the tools' code page so that they can open paths outside the system's ANSI code page. Files
    # dropped on the tools can have such paths. The linker merges the manifest into the one it creates.
    if (MSVC)
        target_sources(${tool} PRIVATE platform/windows/utf8.manifest)
    endif()

    # macOS: a droplet app for each tool (wsfrip.app, wsfplay.app) so that files can be dropped on it in the Finder.
    # Each app is an AppleScript applet with the tool inside, in Contents/Resources. Its Info.plist holds the version.
    if (APPLE)
        find_program(OSACOMPILE osacompile)
        find_program(CODESIGN codesign)
        find_program(PLUTIL plutil)
        if (OSACOMPILE AND CODESIGN AND PLUTIL)
            set(DROPLET_TOOL ${tool})
            set(droplet_app ${CMAKE_BINARY_DIR}/${tool}.app)
            set(droplet_script ${CMAKE_BINARY_DIR}/${tool}.applescript)
            configure_file(platform/macos/droplet.applescript.in ${droplet_script} @ONLY)
            add_custom_command(
                OUTPUT ${droplet_app}/Contents/Resources/${tool}
                COMMAND ${CMAKE_COMMAND} -E rm -rf ${droplet_app}
                COMMAND ${OSACOMPILE} -o ${droplet_app} ${droplet_script}
                COMMAND ${PLUTIL} -replace CFBundleShortVersionString -string ${wsf_VERSION}
                        ${droplet_app}/Contents/Info.plist
                COMMAND ${PLUTIL} -replace CFBundleVersion -string ${wsf_VERSION} ${droplet_app}/Contents/Info.plist
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${tool}> ${droplet_app}/Contents/Resources/${tool}
                COMMAND ${CODESIGN} --force --deep --sign - ${droplet_app}
                DEPENDS ${tool} ${droplet_script}
                COMMENT "Building ${tool}.app"
                VERBATIM)
            add_custom_target(${tool}_app ALL DEPENDS ${droplet_app}/Contents/Resources/${tool})
        endif()
    endif()
endforeach()

# foobar2000 component (Windows with clang-cl or MSVC, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_wsf component when set)")
if (FOOBAR2000_SDK_DIR)
    if (NOT MSVC AND NOT APPLE)
        message(FATAL_ERROR "the foobar2000 component needs clang-cl or MSVC on Windows, or macOS")
    endif()
    add_subdirectory(plugins/foobar2000)
endif()

# The tests build images in every format. They require the compressors as well as the decompressors.
add_executable(disc_test tests/disc_test.cpp tests/check.h)
target_link_libraries(disc_test PRIVATE wsf_disc ZLIB::ZLIB ${WSF_ZSTD} ${WSF_LZMA} ${WSF_BZIP2})
target_compile_options(disc_test PRIVATE ${WSF_WARNINGS})

add_executable(ax_test tests/ax_test.cpp tests/check.h)
target_link_libraries(ax_test PRIVATE wsf_ax)
target_compile_options(ax_test PRIVATE ${WSF_WARNINGS})

add_executable(dsp_test tests/dsp_test.cpp tests/check.h tests/dsp_test_hashes.h)
target_link_libraries(dsp_test PRIVATE wsf_dsp)
target_compile_options(dsp_test PRIVATE ${WSF_WARNINGS})

add_executable(format_test tests/format_test.cpp tests/check.h)
target_link_libraries(format_test PRIVATE wsf_format ZLIB::ZLIB)
target_compile_options(format_test PRIVATE ${WSF_WARNINGS})

add_executable(nw4r_test tests/nw4r_test.cpp tests/check.h tests/rsar_files.h)
target_link_libraries(nw4r_test PRIVATE wsf_nw4r)
target_compile_options(nw4r_test PRIVATE ${WSF_WARNINGS})

add_executable(rip_test tests/rip_test.cpp tests/check.h tests/rsar_files.h)
target_link_libraries(rip_test PRIVATE wsf_rip)
target_compile_options(rip_test PRIVATE ${WSF_WARNINGS})

add_executable(rsar_test tests/rsar_test.cpp tests/check.h tests/rsar_files.h)
target_link_libraries(rsar_test PRIVATE wsf_rsar)
target_compile_options(rsar_test PRIVATE ${WSF_WARNINGS})

add_executable(sha1_test tests/sha1_test.cpp tests/check.h)
target_link_libraries(sha1_test PRIVATE wsf_common)
target_compile_options(sha1_test PRIVATE ${WSF_WARNINGS})

add_executable(snapshot_test tests/snapshot_test.cpp tests/check.h)
target_link_libraries(snapshot_test PRIVATE wsf_common)
target_compile_options(snapshot_test PRIVATE ${WSF_WARNINGS})

add_executable(render_ahead_test tests/render_ahead_test.cpp tests/check.h)
target_link_libraries(render_ahead_test PRIVATE wsf_player)
target_compile_options(render_ahead_test PRIVATE ${WSF_WARNINGS})

enable_testing()
add_test(NAME ax COMMAND ax_test)
add_test(NAME disc COMMAND disc_test)
add_test(NAME dsp COMMAND dsp_test)
add_test(NAME format COMMAND format_test)
add_test(NAME nw4r COMMAND nw4r_test)
add_test(NAME rip COMMAND rip_test)
add_test(NAME rsar COMMAND rsar_test)
add_test(NAME render_ahead COMMAND render_ahead_test)
add_test(NAME sha1 COMMAND sha1_test)
add_test(NAME snapshot COMMAND snapshot_test)

# The tools report the version they were built as.
string(REPLACE "." "\\." version_pattern ${wsf_VERSION})
add_test(NAME version_wsfrip COMMAND wsfrip --version)
set_tests_properties(version_wsfrip PROPERTIES PASS_REGULAR_EXPRESSION "^wsfrip ${version_pattern}[\r\n]*$")

# Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
file(GLOB local_tests CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/*/local_tests.cmake)
foreach (file IN LISTS local_tests)
    include(${file})
endforeach()
