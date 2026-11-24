# xmp-wsf

XMPlay input plugin for WSF rips (`.miniwsf`). It uses the same player core as `wsfplay`. That core runs the game's DSP
program (AX's microcode, or the DSP program of JAudio) on an emulated GameCube/Wii DSP. In game mode, the game's sound
code drives the DSP on an emulated Wii. In archive mode, WSF's model of the sound player in the SDK drives it.

XMPlay is a 32-bit program for Windows, and the plugin is a 32-bit DLL, `xmp-wsf.dll`, built with clang-cl. It
requires XMPlay 3.8.2 or later.

## Installing

The Releases page has `xmp-wsf-X.Y.Z.zip`, with the DLL and the licences. Put `xmp-wsf.dll` in XMPlay's folder, or in a
folder inside it, and restart XMPlay.

## Features

- Tags become XMPlay's tags: `game` as album and `year` as date. The Message info window lists every tag except `_lib`,
  `length`, `fade`, `loop_start`, `loop_length`, `volume`, `utf8`, `wsf_tracks`, `wsf_tempo` and `wsf_var`.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play for the default length and
  fade, and stop early when the sound ends by itself.
- Each track plays as a soundtrack album plays it: from the first sound, after 0.5 s of silence, with 1 s of silence
  after it. A loop plays the passes in its tags, or another number of passes with its `loop_start` and `loop_length`
  tags.
- The plugin's options page, among the decoders in XMPlay's options: default length and fade, the fade's curve,
  endless playback, rendering ahead, and the settings of an album's track: skip silence at the start, the silences
  before and after a track, the loops, one pass for long loops, the fade length for loops, ignore length and fade
  tags, and skip silence at the end. XMPlay stores the settings.
- During playback, the plugin renders ahead on a separate thread as fast as the CPU allows. It buffers up to 10 minutes
  and 10 seconds of audio (about 75 MiB). Seeking within the buffer is instant, and seeking further ahead waits for
  rendering to reach the requested position. Longer tracks also save the state of the emulation every 2 seconds.
  Seeking back before the buffered audio restores the latest state before the target and renders from there. At most
  128 states are kept, and the intervals between them grow as the track continues. With rendering ahead turned off,
  every track saves states, and seeking past what has played renders up to the target.
- Output is 16-bit stereo at 32,000 Hz, the rate of the Wii's audio interface. It isn't resampled.

## Building

**On GitHub (no Windows machine needed):** the `XMPlay plugin` workflow (`.github/workflows/xmplay.yml`) builds the DLL
and uploads it with the licences. Start it from the Actions tab of the repository (Run workflow). GitHub lists a
manual workflow only once it's on the default branch (`main`), and the repository must have GitHub Actions enabled. The
workflow downloads the XMPlay plugin SDK from un4seen.com (`externals/xsflib/ci/xmplay-sdk`).

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang tools for Windows"
component, plus CMake 3.20 or later. Download the XMPlay plugin SDK (<https://www.un4seen.com/files/xmp-sdk.zip>) and
unpack it. Use the folder that holds `xmpin.h` as the SDK path below.

```
cmake -S . -B build-xmplay -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=C:/path/to/xmp-sdk
cmake --build build-xmplay --config Release --target xmp-wsf
```

This produces `build-xmplay/plugins/xmplay/Release/xmp-wsf.dll`. Everything is built with the static C runtime
(`/MT`), and the plugin requires no runtime DLLs.

## Performance

Playback runs on one CPU core, and most of the time goes to emulating the DSP. Game mode also emulates the Wii's CPU
and is the slower of the two.

## Licence

Like the rest of WSF, the plugin is MIT-licensed, and it uses zlib. Ship `LICENSE` and `THIRD-PARTY-NOTICES.md` (or
their text) with a build. The XMPlay plugin SDK states no licence. The plugin uses only the declarations of the XMPlay
interface in the SDK, and no code from the SDK goes into it.
