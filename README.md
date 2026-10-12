# wsf

WSF is a PSF-family music rip format for Nintendo Wii sequenced audio. It covers the music that Wii games play through
the console's DSP: from NintendoWare sound archives (`.brsar`), or with JAudio2, the sound library of Nintendo's
JSystem. WSF is the Wii counterpart of the DS formats 2SF and NCSF.

Each WSF rip contains the game's DSP program: AX's microcode, or JAudio2's. The player runs it on an emulated DSP. The
mixing, filtering and resampling therefore match the game. A rip uses one of two modes:

- **Game mode** requires a driver profile for the game. The rip contains the game's code. The player runs that code,
  including the game's OS and sound library, on an emulated Wii. The game plays the sound itself. The rip is therefore
  exact.
- **Archive mode** works with any sound archive. WSF's model of the SDK's sound player plays the archive.

`docs/wsf.md` specifies the format. Version 1 supports game mode for Wii Sports and Wii Play (both USA, revision 1), and
for WarioWare: Smooth Moves, Mario Kart Wii, Wii Music and Animal Crossing: City Folk (all USA). Other games, WiiWare
games and loose sound archives are ripped in archive mode. Another release of one of these games gets game mode too
when its code matches. The European and Japanese releases of WarioWare: Smooth Moves get it that way. Archive mode
follows the versions of the sound library from 2006 (WarioWare: Smooth Moves) to 2008 (Wii Music). Archive mode also
follows two changes in later versions. `docs/wsf.md` lists them.

Game mode also plays The Legend of Zelda: Twilight Princess (USA, revision 2). The game's sound library is JAudio2.
Rips of Twilight Princess require a player newer than WSF 1.1.0. WSF 1.1.0 refuses them. They take more processing to
play than other games' rips. A slow computer may not play them in real time. Archive mode doesn't play JAudio2.
`wsfrip` therefore can't rip another JAudio2 game or another release of Twilight Princess. It stops with an error that
says so.

## Tools

| Tool | Description |
|---|---|
| `wsfrip [options] <input>...` | Rips each input into a `.wsflib` and a `.miniwsf` per sequence. The output goes in a folder next to the input. The folder has the input's name with `_wsf` added. Each of a game's sound archives gets a separate folder with the archive's name. Where two of the game's archives have the same name, the names of the folders above them are added. A `.wsflib` that differs from one of the same name in the folder gets the next free name, such as `<name>_2.wsflib`. The rips already in the folder then still play. To replace earlier rips, rip into an empty folder. An input is a Wii disc image, an extracted disc, an unpacked WiiWare title or a sound archive (`.brsar`). An extracted disc is a folder with `sys/main.dol` and `files/`, either directly or in a `DATA` folder. Dolphin and Wiimm's tools extract discs in these layouts. An unpacked title is a folder with the title's TMD (`.tmd`) and its decrypted contents (`.app`). A sound archive requires the `main.dol` of its game. `wsfrip` takes AX's microcode and the build of the sound library from it. Give the `main.dol` with the archive or with `--microcode`, or put it in the archive's folder. `--list` lists a game's sound archives, or a JAudio2 game's sounds and sections. `--mode game\|archive` chooses the mode. `--archive`, `--only` and `--bgm` choose what to rip. Sequences that make no sound, such as placeholders and those that only set values for the game, are listed and skipped. `--help` shows the rest. |
| `wsfplay [options] <file>...` | Renders each `.miniwsf` to a WAV file next to it at 32,000 Hz. The length and fade come from the file's tags. `-o FILE` writes FILE instead. Its extension selects WAV or FLAC. `--length` and `--fade` override the tags. `--info` shows a file's tags and libraries. `--help` shows the rest. |
| `plugins/foobar2000` | foobar2000 input component: `foo_input_wsf.component` for macOS and `foo_input_wsf.dll` for Windows. Both are built with the foobar2000 SDK (see its [README](plugins/foobar2000/README.md)). |

`wsfrip` reads disc images in every common format: ISO, NKit (`.nkit.iso`, `.nkit.gcz`), RVZ, WIA, GCZ, WBFS, CISO and
WDF, whole or split into pieces. The game partition must be stored decrypted. NKit, RVZ and WIA images store it that
way. A plain ISO dump, and a WBFS, CISO, GCZ or WDF made from one, store it encrypted. Dolphin's Convert File makes an
RVZ from any of them without losing data.

`wsfrip` rips WiiWare games in archive mode only. A WAD holds a title's contents encrypted. `wsfrip` rejects it. Give
`wsfrip` a folder with the title's TMD and its decrypted contents instead. A WAD unpacker that decrypts the contents
writes such a folder. A Wii's NAND keeps each installed title in one: `title/00010001/<ID>/content`. `<ID>` is the last
8 hex digits of the title ID. Dolphin installs a WAD in its NAND with Tools > Install WAD. The title's folder is then
under `Wii/title/00010001` in Dolphin's user folder. `wsfrip` reads the contents named by their index or by their
content ID. A content that doesn't match the TMD's SHA-1 is rejected as encrypted or damaged.

Both tools accept multiple inputs and continue if one fails. A nonexistent input path stops the command before it writes
any files. For example, `wsfrip game.rvz` writes `game_wsf/`. `wsfplay game_wsf/*/*.miniwsf` then renders every sequence
in it. The rips of a JAudio2 game go in `game_wsf/` itself. `--version` shows a tool's version.

### Downloads

The Releases page has `wsfrip` and `wsfplay` for 64-bit Windows, and the foobar2000 component for 64-bit and 32-bit
foobar2000 on Windows. On macOS, build WSF from source (see "Building").

### Drag and drop

- **Windows:** drop files on `wsfrip.exe` or `wsfplay.exe`. A console window shows the results and stays open until you
  press Enter.
- **macOS:** the build also makes `wsfrip.app` and `wsfplay.app` in the build folder. Drop files on one, or open it and
  choose them. The app reports the results for each file and offers to open the output folders.

As above, the results go next to the files. Drop a sound archive together with the `main.dol` of its game if the
archive's folder doesn't contain that file. Drop a WiiWare title's folder, not the files in it. File and folder names
can use any characters. On Windows, that requires Windows 10 version 1903 or later.

## Building

You need Git, CMake 3.20 or later, and C and C++20 compilers. WSF builds on two libraries. The repository holds both
as submodules. xsflib (`externals/xsflib`) is a library for players and rippers of the PSF family. ntdlib
(`externals/ntdlib`) is a library for the hardware and formats of Nintendo's consoles. Clone with
`git clone --recursive`, or run `git submodule update --init --recursive` in a clone. xsflib's tests take seqlib from
xsflib's submodule. zlib, Zstandard, liblzma, bzip2 and libFLAC are downloaded and built along with WSF when they
aren't installed.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload and its "C++ Clang tools for
  Windows" component. The workload includes CMake. The component adds clang-cl. Configure with `cmake -S . -B build -A
  x64 -T ClangCL` instead of the configure command below. Run the commands in a Developer PowerShell or Developer
  Command Prompt.
- **macOS on Apple Silicon:** the Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`).
  `brew install zstd xz flac` saves building those three.

Linux, Intel Macs, MSVC and 32-bit Windows aren't supported. The one exception is the 32-bit foobar2000 component. Build
it with clang-cl, configured with `-A Win32 -T ClangCL`.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` has `release` and `debug` presets for the same steps: `cmake --preset release`, `cmake --build
--preset release` and `ctest --preset release`. The presets require CMake 3.23 or later. They don't select clang-cl. On
Windows, use the commands above.

## Layout

```
src/ax/       AX's CPU side, and the exchange with its microcode
src/nw4r/     the model of nw4r::snd that plays archive mode
src/wii/      the Wii for game mode: its hardware, IOS, and a disc made of a rip's files
src/wsf/      the format and the player
src/rip/      the ripper, the game profiles and the driver's image
src/cli/      wsfrip and wsfplay
externals/xsflib/    a library for players and rippers of the PSF family
externals/ntdlib/    a library for the hardware and formats of Nintendo's consoles
plugins/foobar2000/  the foobar2000 component
driver/       the game-mode drivers' sources (NW4R SND's and JAudio2's), and the script that builds them
docs/         the format's specification
tests/        ctest suites
```

xsflib holds the PSF container, playback and the tools' frames. xsflib's CMake helpers make the Windows programs'
version resources and manifests, the macOS droplet apps and the foobar2000 component. ntdlib holds the disc reader,
the Broadway CPU, the hardware that the GameCube and the Wii share (the DSP that runs AX's microcode, its interface,
EXI, and the processor and audio interfaces) and the sound archive readers.

The driver runs on the emulated Wii. `driver/build.py` builds it for PowerPC and requires a clang with the PowerPC
target. The driver's image is committed as `src/rip/driver_blob.h`. Building WSF itself requires no PowerPC toolchain.

## Licence

WSF is under the MIT licence (`LICENSE`). `THIRD-PARTY-NOTICES.md` lists the libraries and data that WSF uses and what a
binary distribution must include. This licence doesn't cover game data. Rips contain a game's code and sound data and
are for personal use.
