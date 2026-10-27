# wsf

WSF is a PSF-family music rip format for Nintendo Wii sequenced audio. It covers the music that Wii games play through
the console's DSP: from NintendoWare sound archives (`.brsar`), or with JAudio, the sound library of Nintendo's
JSystem. WSF is the Wii counterpart of the DS formats 2SF and NCSF.

Each WSF rip contains the game's DSP program: AX's microcode, or the DSP program of JAudio. The player runs it on an
emulated DSP. The mixing, filtering and resampling therefore match the game. A rip uses one of two modes:

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

Mega Man 9 (WiiWare) plays each piece of its music as four sequences, one for each channel of the NES's sound chip.
`wsfrip` rips each piece as one `.miniwsf`. Like the game, the `.miniwsf` starts the four together. Such rips require a
player newer than WSF 1.5.0. WSF 1.5.0 refuses them. `docs/wsf.md` describes them under "Sounds that play together".

WSF plays the reverb and other effects that Wii Sports, Wii Play, WarioWare: Smooth Moves, Animal Crossing: City Folk,
Mario Kart Wii and Wii Music (all USA) put on the Wii's auxiliary buses, in both modes. Both modes play WSF's models of
the effects. Game mode sets them up with the game's code. The models give the same samples as the game's code. The rips
of five Wii channels have the channels' effects too: the Wii Shop Channel, the Mii Channel and the Photo Channel 1.1
(all World), and the Forecast and News Channels (USA). A channel rips in archive mode only. While a photo with a mood is
shown, the Photo Channel plays the music of its photos through a mood effect. The channel defines that effect in its
code. `wsfrip` rips each piece of that music once more in each of the four moods. A mood's rip requires a player newer
than WSF 1.7.0. WSF 1.7.0 refuses such a rip but plays the channel's other rips. The Mii Channel plays its music with
other tracks or at a faster tempo in some of its scenes. `wsfrip` rips the music once more in each of four such states.
Rips of other NintendoWare games play without effects. `docs/wsf.md` describes the effects under "Effects". It
describes the Mii Channel's states under "Tracks that the game silences".

Game mode also plays four games whose sound library is JAudio2: The Legend of Zelda: Twilight Princess (USA,
revision 2), Link's Crossbow Training (USA, revision 1), Super Mario Galaxy and Super Mario Galaxy 2 (both USA). The
rips of Twilight Princess and Link's Crossbow Training have the games' echo. The rips of these games require a player
newer than WSF 1.1.0. WSF 1.1.0 refuses them. They take more processing to play than other games' rips. A slow computer
may not play them in real time. The two Galaxy games stream most of their music. `wsfrip` rips the pieces that the games
play from sequences, such as the jingles and the music of the power-ups. Game mode also plays New Play Control! Donkey
Kong Jungle Beat (USA), another JAudio2 game. Its rips require WSF 1.4.0 or later. The game plays many of its effects in
the chords of the music. A rip plays each such effect over a stage's music as the game does. The music stays silent.
Archive mode doesn't play JAudio2.
`wsfrip` therefore can't rip another JAudio2 game or another release of these games. It stops with an error that says
so.

Game mode also plays New Play Control! Pikmin (USA). The game's sound library is the early JAudio1 of the GameCube
release. `wsfrip` rips the game's sequenced music: its pieces, and each part of the piece that the cutscenes play. The
game's streamed music (`.stx`) isn't ripped. Like Twilight Princess's rips, these require a player newer than WSF 1.1.0.

Game mode also plays New Play Control! Pikmin 2 (USA). The game's sound library is the later JAudio1 of the GameCube
release. `wsfrip` rips the game's 48 pieces of music and its sound effects. The game's streamed music (`.ast`) isn't
ripped. Like Twilight Princess's rips, these require a player newer than WSF 1.1.0.

## Tools

| Tool | Description |
|---|---|
| `wsfrip [options] <input>...` | Rips each input into a `.wsflib` and a `.miniwsf` per sequence, or per piece for music that a game plays as several sequences together. The output goes in a folder next to the input. The folder has the input's name with `_wsf` added. Each of a game's sound archives gets a separate folder with the archive's name. Where two of the game's archives have the same name, the names of the folders above them are added. A `.wsflib` that differs from one of the same name in the folder gets the next free name, such as `<name>_2.wsflib`. The rips already in the folder then still play. To replace earlier rips, rip into an empty folder. An input is a Wii disc image, an extracted disc, an unpacked title (a WiiWare game or a channel) or a sound archive (`.brsar`). `wsfrip` also finds the sound archives inside a game's CRI CPK archives (`.cpk`). An extracted disc is a folder with `sys/main.dol` and `files/`, either directly or in a `DATA` folder. Dolphin and Wiimm's tools extract discs in these layouts. An unpacked title is a folder with the title's TMD (`.tmd`) and its decrypted contents (`.app`). A sound archive requires the `main.dol` of its game. `wsfrip` takes AX's microcode and the build of the sound library from it. Give the `main.dol` with the archive or with `--microcode`, or put it in the archive's folder. A WiiWare game or a channel has no `main.dol`. Its main program takes that role. `--microcode` takes the game's unpacked title or extracted disc, and a main program as the title stores it, compressed or not. `--list` lists a game's sound archives, or a JAudio game's sounds (with a JAudio2 game's sections). `--mode game\|archive` chooses the mode. `--archive`, `--only` and `--bgm` choose what to rip. In archive mode, `--tracks`, `--tempo` and `--var` rip each sequence in an arrangement that a game can play it in: some of its tracks, another tempo, or variables set. `--suffix` adds a text to the rips' titles and file names. Each arrangement of a sound then rips as a separate file. Sequences that make no sound, such as placeholders and those that only set values for the game, are listed and skipped. `--help` shows the rest. |
| `wsfplay [options] <file>...` | Renders each `.miniwsf` to a WAV file next to it at 32,000 Hz. The length and fade come from the file's tags. `-o FILE` writes FILE instead. Its extension selects WAV or FLAC. `--length` and `--fade` override the tags. `--info` shows a file's tags and libraries. `--help` shows the rest. |
| `plugins/foobar2000` | foobar2000 input component: `foo_input_wsf.component` for macOS and `foo_input_wsf.dll` for Windows. Both are built with the foobar2000 SDK (see its [README](plugins/foobar2000/README.md)). |
| `plugins/xmplay` | XMPlay input plugin: `xmp-wsf.dll`, a 32-bit DLL for XMPlay on Windows, built with clang-cl and the XMPlay plugin SDK. See its [README](plugins/xmplay/README.md). |

`wsfrip` reads disc images in every common format: ISO, NKit (`.nkit.iso`, `.nkit.gcz`), RVZ, WIA, GCZ, WBFS, CISO and
WDF, whole or split into pieces. The game partition must be stored decrypted. NKit, RVZ and WIA images store it that
way. A plain ISO dump, and a WBFS, CISO, GCZ or WDF made from one, store it encrypted. Dolphin's Convert File makes an
RVZ from any of them without losing data.

`wsfrip` rips titles (WiiWare games and channels) in archive mode only. A WAD holds a title's contents encrypted.
`wsfrip` rejects it. Give `wsfrip` a folder with the title's TMD and its decrypted contents instead. A WAD unpacker that
decrypts the contents writes such a folder. A Wii's NAND keeps each installed title in one:
`title/00010001/<ID>/content` for a WiiWare game and `title/00010002/<ID>/content` for a channel. `<ID>` is the last 8
hex digits of the title ID. Dolphin installs a WAD in its NAND with Tools > Install WAD. A WiiWare game's folder is then
under `Wii/title/00010001` in Dolphin's user folder. A channel's is under `Wii/title/00010002`. `wsfrip` reads the
contents named by their index or by their content ID. A content that doesn't match the TMD's SHA-1 is rejected as
encrypted or damaged.

Both tools accept multiple inputs and continue if one fails. A nonexistent input path stops the command before it writes
any files. For example, `wsfrip game.rvz` writes `game_wsf/`. `wsfplay game_wsf/*/*.miniwsf` then renders every sequence
in it. The rips of a JAudio game go in `game_wsf/` itself. `--version` shows a tool's version.

### Downloads

The Releases page has `wsfrip` and `wsfplay` for 64-bit Windows, the foobar2000 component for 64-bit and 32-bit
foobar2000 on Windows, and the XMPlay plugin. On macOS, build WSF from source (see "Building").

To install the XMPlay plugin, unzip `xmp-wsf-X.Y.Z.zip` and put `xmp-wsf.dll` in XMPlay's folder, or in a folder inside
it. Then restart XMPlay.

### Drag and drop

- **Windows:** drop files on `wsfrip.exe` or `wsfplay.exe`. A console window shows the results and stays open until you
  press Enter.
- **macOS:** the build also makes `wsfrip.app` and `wsfplay.app` in the build folder. Drop files on one, or open it and
  choose them. The app reports the results for each file and offers to open the output folders.

As above, the results go next to the files. Drop a sound archive together with the `main.dol` of its game if the
archive's folder doesn't contain that file. Drop a title's folder, not the files in it. File and folder names
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

Linux, Intel Macs, MSVC and 32-bit Windows aren't supported. The exceptions are the 32-bit foobar2000 component and the
XMPlay plugin. Build them with clang-cl, configured with `-A Win32 -T ClangCL`.

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
src/axfx/     the models of the SDK's effects for AX's aux buses (AXFX)
src/nw4r/     the model of nw4r::snd that plays archive mode
src/wii/      the Wii for game mode: its hardware, IOS, and a disc made of a rip's files
src/wsf/      the format and the player
src/rip/      the ripper: the parts that every game shares, the game profiles, a file for each sound library
              and the drivers' images
src/cli/      wsfrip and wsfplay
externals/xsflib/    a library for players and rippers of the PSF family
externals/ntdlib/    a library for the hardware and formats of Nintendo's consoles
plugins/foobar2000/  the foobar2000 component
plugins/xmplay/      the XMPlay plugin
driver/       the game-mode drivers' sources, for NW4R SND and JAudio, and the script that builds them
docs/         the format's specification
tests/        ctest suites
```

xsflib holds the PSF container, playback and the tools' frames. xsflib's CMake helpers make the Windows programs'
version resources and manifests, the macOS droplet apps, the foobar2000 component and the XMPlay plugin. ntdlib holds
the disc reader, the Broadway CPU, the hardware that the GameCube and the Wii share (the DSP that runs AX's microcode,
its interface, EXI, and the processor and audio interfaces) and the sound archive readers.

The drivers run on the emulated Wii. `driver/build.py` builds them for PowerPC and requires a clang with the PowerPC
target. The drivers' images are committed in `src/rip/` as `driver_blob.h` (NW4R SND), `jaudio_driver_blob.h` (Twilight
Princess and Link's Crossbow Training), `jungle_beat_driver_blob.h` (New Play Control! Donkey Kong Jungle Beat),
`pikmin_driver_blob.h` (New Play Control! Pikmin), `pikmin2_driver_blob.h` (New Play Control! Pikmin 2),
`galaxy_driver_blob.h` (Super Mario Galaxy) and `galaxy2_driver_blob.h` (Super Mario Galaxy 2). Building WSF itself
requires no PowerPC toolchain.

## Licence

WSF is under the MIT licence (`LICENSE`). `THIRD-PARTY-NOTICES.md` lists the libraries and data that WSF uses and what a
binary distribution must include. This licence doesn't cover game data. Rips contain a game's code and sound data and
are for personal use.
