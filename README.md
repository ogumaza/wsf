# wsf

WSF is a PSF-family music rip format for Nintendo Wii sequenced audio. It covers the music that Wii games play from
NintendoWare sound archives (`.brsar`) through the console's DSP. WSF is the Wii counterpart of the DS formats 2SF and
NCSF.

Each WSF rip contains the game's DSP program, AX's microcode. The player runs it on an emulated DSP. The mixing,
filtering and resampling therefore match the game. A rip uses one of two modes:

- **Game mode** requires a driver profile for the game. The rip contains the game's code. The player runs that code,
  including the game's OS and sound library, on an emulated Wii. The game plays the sound itself. The rip is therefore
  exact.
- **Archive mode** works with any sound archive. WSF's model of the SDK's sound player plays the archive.

`docs/wsf.md` specifies the format. Version 1 supports game mode for Wii Sports and Wii Play (both USA, revision 1), and
for WarioWare: Smooth Moves, Mario Kart Wii, Wii Music and Animal Crossing: City Folk (all USA). Other games and loose
sound archives are ripped in archive mode.

## Tools

| Tool | Description |
|---|---|
| `wsfrip [options] <input>...` | Rips each input into a `.wsflib` and a `.miniwsf` per sequence. The output goes in a folder next to the input. The folder has the input's name with `_wsf` added. Each of a game's sound archives gets a separate folder with the archive's name. Where two of the game's archives have the same name, the names of the folders above them are added. An input is a Wii disc image, an extracted disc or a sound archive (`.brsar`). An extracted disc is a folder with `sys/main.dol` and `files/`, either directly or in a `DATA` folder. Dolphin and Wiimm's tools extract discs in these layouts. A sound archive requires a game's `main.dol` for AX's microcode. Give one with the archive or with `--microcode`, or put one in the archive's folder. `--list` lists a game's sound archives. `--mode game\|archive` chooses the mode. `--archive`, `--only` and `--bgm` choose what to rip. `--help` shows the rest. |
| `wsfplay [options] <file>...` | Renders each `.miniwsf` to a WAV file next to it at 32,000 Hz. The length and fade come from the file's tags. `-o FILE` writes FILE instead. Its extension selects WAV or FLAC. `--length` and `--fade` override the tags. `--info` shows a file's tags and libraries. `--help` shows the rest. |
| `plugins/foobar2000` | foobar2000 input component: `foo_input_wsf.component` for macOS and `foo_input_wsf.dll` for Windows. Both are built with the foobar2000 SDK (see its [README](plugins/foobar2000/README.md)). |

`wsfrip` reads disc images in every common format: ISO, NKit (`.nkit.iso`, `.nkit.gcz`), RVZ, WIA, GCZ, WBFS, CISO and
WDF, whole or split into pieces. The game partition must be stored decrypted. NKit, RVZ and WIA images store it that
way. A plain ISO dump, and a WBFS, CISO, GCZ or WDF made from one, store it encrypted. Dolphin's Convert File makes an
RVZ from any of them without losing data.

Both tools accept multiple inputs and continue if one fails. A nonexistent input path stops the command before it writes
any files. For example, `wsfrip game.rvz` writes `game_wsf/`. `wsfplay game_wsf/*/*.miniwsf` then renders every sequence
in it. `--version` shows a tool's version.

### Downloads

The Releases page has `wsfrip` and `wsfplay` for 64-bit and 32-bit Windows, and the foobar2000 component for 64-bit and
32-bit foobar2000 on Windows. On other systems, build WSF from source (see "Building").

### Drag and drop

- **Windows:** drop files on `wsfrip.exe` or `wsfplay.exe`. A console window shows the results and stays open until you
  press Enter.
- **macOS:** the build also makes `wsfrip.app` and `wsfplay.app` in the build folder. Drop files on one, or open it and
  choose them. The app reports the results for each file and offers to open the output folders.

As above, the results go next to the files. Drop a sound archive together with a game's `main.dol` if the archive's
folder doesn't contain one. File and folder names can use any characters. On Windows, that requires Windows 10 version
1903 or later.

## Building

You need Git, CMake 3.20 or later, and C and C++20 compilers. zlib, Zstandard, liblzma, bzip2 and libFLAC are downloaded
and built along with WSF when they aren't installed.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload and its "C++ Clang tools for
  Windows" component. The workload includes CMake. The component adds clang-cl. Configure with `cmake -S . -B build -T
  ClangCL` instead of the configure command below. Run the commands in a Developer PowerShell or Developer Command
  Prompt.
- **macOS:** the Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`). `brew install
  zstd xz flac` saves building those three. A universal build (two architectures in `CMAKE_OSX_ARCHITECTURES`) still
  builds them.
- **Linux:** GCC or Clang, CMake and Git from your distribution. The development packages of zlib, Zstandard, liblzma,
  bzip2 and libFLAC (on Debian and Ubuntu, `zlib1g-dev libzstd-dev liblzma-dev libbz2-dev libflac-dev`) save building
  them.

MinGW, GCC and GNU-style Clang aren't supported on Windows. GCC and Clang aren't supported for 32-bit x86 because their
floating-point arithmetic there gives different results from the other builds. CMake rejects these configurations.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` has `release` and `debug` presets for the same steps: `cmake --preset release`, `cmake --build
--preset release` and `ctest --preset release`. The presets require CMake 3.21 or later.

The disc test (`tests/disc_test.cpp`) builds a small Wii disc and stores it in every format that the reader supports,
with each compression method and layout. It then checks that every file reads back the same.

## Layout

```
src/common/   small helpers shared by the rest (byte order, SHA-1, DOLs)
src/disc/     disc images in every format, and the files of a game's partition
src/rsar/     NintendoWare sound archives: sequences, banks and waves
src/dsp/      the DSP, which runs AX's microcode
src/ax/       AX's CPU side, and the exchange with its microcode
src/nw4r/     the model of nw4r::snd that plays archive mode
src/ppc/      the Broadway CPU
src/wii/      the Wii for game mode: its hardware, IOS, and a disc made of a rip's files
src/wsf/      the format and the player
src/rip/      the ripper, the game profiles and the driver's image
src/cli/      wsfrip and wsfplay
plugins/foobar2000/  the foobar2000 component
driver/       the game-mode driver's source, and the script that builds it
platform/     the Windows manifest and version resource, and the macOS droplet apps of wsfrip and wsfplay
docs/         the format's specification
tests/        ctest suites
```

The driver runs on the emulated Wii. `driver/build.py` builds it for PowerPC and requires a clang with the PowerPC
target. The driver's image is committed as `src/rip/driver_blob.h`. Building WSF itself requires no PowerPC toolchain.

## Licence

WSF is under the MIT licence (`LICENSE`). `THIRD-PARTY-NOTICES.md` lists the libraries that WSF uses and what a binary
distribution must include. This licence doesn't cover game data. Rips contain a game's code and sound data and are for
personal use.
