# foo_input_wsf

foobar2000 input component for WSF rips (`.miniwsf`). It uses the same player core as `wsfplay`. The player core runs
the game's AX microcode on an emulated GameCube/Wii DSP. In game mode, the game's sound code drives it on an emulated
Wii. In archive mode, WSF's model of the SDK's sound player drives it.

**Status:** the component builds with Apple Clang for foobar2000 on macOS. It builds with clang-cl for 32-bit and
64-bit foobar2000 on Windows. In tests on Windows, 64-bit and 32-bit foobar2000 2.26.1 load, decode, play and seek rips
of both modes. `ctest` covers the player core.

## Features

- Like kode54's PSF-family components, the component shows tags as metadata: `game` as album, `year` as date, and
  `replaygain_*` as ReplayGain. Tags can be edited. Editing rewrites the tag area and keeps `_lib`, `length`, `fade`,
  `volume` and `utf8`, and the tags that arrange a sequence: `wsf_tracks`, `wsf_tempo` and `wsf_var`.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play for the default length and
  fade. They stop early when the sound ends by itself.
- Advanced preferences › Decoding › WSF decoder: default length and fade, endless playback, and rendering ahead. Endless
  playback and rendering ahead apply to playback only, never to conversion or scanning.
- During playback, the component renders ahead on a separate thread as fast as the CPU allows. It buffers up to 10
  minutes and 10 seconds of audio (about 75 MiB). That's enough for the longest track that `wsfrip`'s length analysis
  produces. Seeking within the buffer is instant. Seeking further ahead waits for rendering to reach the requested
  position. Rendering ahead keeps one CPU core busy until it reaches the end of the track or the buffer limit. Longer
  tracks also save the emulation's state every 2 seconds. Seeking back before the buffered audio restores the latest
  state before the target and renders from there. At most 128 states are kept. The intervals between them grow as the
  track continues. With rendering ahead turned off, every track saves states. Seeking past what has played then renders
  up to the target.
- Output is 16-bit stereo at the Wii audio interface's rate of 32,000 Hz. It isn't resampled.

## Building

You need the foobar2000 SDK, from <https://www.foobar2000.org/SDK>. Unpack it and use the folder containing `pfc` and
`foobar2000` as the SDK path below. A newer SDK than the one the component was last built with may require changes to
xsflib's `cmake/XsflibApps.cmake`.

**On macOS:** you need the Xcode command line tools (`xcode-select --install`), CMake 3.20 or later, and the SDK
unpacked somewhere (macOS's `tar -xf SDK-<date>.7z` unpacks it).

```
cmake -S . -B build-fb2k -DCMAKE_BUILD_TYPE=Release -DFOOBAR2000_SDK_DIR=/path/to/SDK \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-fb2k --target foo_input_wsf
```

This produces `build-fb2k/plugins/foobar2000/foo_input_wsf.component`, a bundle for Apple Silicon Macs. It
runs on macOS 11 or later. The SDK's projects have the same minimum. Without the two `CMAKE_OSX_` options, the bundle
only runs on the architecture of the Mac that built it. For a `.fb2k-component`, put the bundle in a `mac` folder inside
a zip. The bundle is signed ad hoc. It has no Developer ID signature and isn't notarized.

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang tools for Windows"
component, plus CMake 3.20 or later.

```
cmake -S . -B build-fb2k -A x64 -T ClangCL -DFOOBAR2000_SDK_DIR=C:/path/to/SDK
cmake --build build-fb2k --config Release --target foo_input_wsf
```

This produces `build-fb2k/plugins/foobar2000/Release/foo_input_wsf.dll`. For a `.fb2k-component`, zip the DLL. For
64-bit foobar2000 v2, put the DLL in an `x64` folder inside the zip. For 32-bit foobar2000, configure with `-A Win32 -T
ClangCL` and put that DLL at the top level of the zip. With the 64-bit DLL in `pkg\x64`, Windows' `tar` makes the zip:
`tar -a -c -f foo_input_wsf.zip -C pkg x64`. Add `foo_input_wsf.dll` after `x64` for a 32-bit DLL in `pkg` itself. Then
rename the zip to `foo_input_wsf.fb2k-component`. Windows PowerShell's `Compress-Archive` writes the folder's name with
a backslash. The zip format doesn't allow that. foobar2000 doesn't recognize such a package. Omitting `-T ClangCL`
selects MSVC. MSVC isn't supported.

The build compiles the SDK's `pfc`, `foobar2000/SDK` and `foobar2000/foobar2000_component_client` sources directly. On
Windows it links `foobar2000/shared/shared-<arch>.lib` (or `shared.lib`). On macOS it compiles the parts of
`foobar2000/shared` and the Objective-C++ files that the SDK's Xcode projects build. If the SDK layout has changed,
adjust xsflib's `cmake/XsflibApps.cmake`.

On Windows everything is built with the static C runtime (`/MT`). The component then requires no runtime DLLs. On macOS
it uses only the system's libraries.

## Performance

Playback runs on one CPU core. Most of the time goes to emulating the DSP. Game mode also emulates the Wii's CPU.
Archive mode is the faster of the two. A 64-bit build is faster than a 32-bit one. A clang-cl build is faster than an
MSVC one. Use 64-bit foobar2000 and the 64-bit component if your other components support them.

## Licence

Like the rest of WSF, the component is MIT-licensed. It uses zlib. Ship `LICENSE` and `THIRD-PARTY-NOTICES.md` (or their
text) with a build. Nothing in it is GPL. It can therefore be distributed as a foobar2000 component.
