# wsf

WSF is a PSF-family music rip format for Nintendo Wii sequenced audio. It covers the music that Wii games play through
the console's DSP: from NintendoWare sound archives (`.brsar`), or with JAudio, the sound library of Nintendo's
JSystem. WSF is the Wii counterpart of the DS formats 2SF and NCSF.

Each WSF rip contains the game's DSP program: AX's microcode, or the DSP program of JAudio. The player runs it on an
emulated DSP, and the mixing, filtering and resampling therefore match the game. A rip uses one of two modes:

- **Game mode** requires a driver profile for the game. The rip contains the game's code, and the player runs that
  code, including the OS and the sound library of the game, on an emulated Wii. The game plays the sound itself, and the
  rip is therefore exact.
- **Archive mode** works with any sound archive. WSF's model of the sound player in the SDK plays the archive.

`docs/wsf.md` specifies the format. Version 1 supports game mode for Wii Sports and Wii Play (both USA, revision 1), and
for WarioWare: Smooth Moves, Mario Kart Wii, Wii Music and Animal Crossing: City Folk (all USA). Other games, WiiWare
games and loose sound archives are ripped in archive mode. Another release of one of these games gets game mode too
when its code matches. The European releases of all six get it that way (Wii Sports and Wii Play in revision 1), and so
does the Japanese release of WarioWare: Smooth Moves. Archive mode follows the versions of the sound library from 2006
(WarioWare: Smooth Moves) to 2008 (Wii Music), and three changes in later versions that `docs/wsf.md` lists.

Mega Man 9 (WiiWare) plays each piece of its music as four sequences, one for each channel of the NES's sound chip.
`wsfrip` rips each piece as one `.miniwsf`, and like the game, the `.miniwsf` starts the four together. In some views,
the Forecast Channel and the News Channel play a layer over the base of a piece. The mix of the two in each view rips as
one `.miniwsf`, and a rip whose title ends in `globe` plays the mix of the globe. Players of WSF 1.5.0 and older refuse
such rips. `docs/wsf.md` describes them under "Sounds that play together".

WSF plays the reverb and other effects that Wii Sports, Wii Play, WarioWare: Smooth Moves, Animal Crossing: City Folk,
Mario Kart Wii and Wii Music (all USA) put on the Wii's auxiliary buses, in both modes. The European releases of all six
games and the Japanese release of WarioWare: Smooth Moves get the same effects when `wsfrip` rips their disc image or
extracted folder. Both modes play WSF's models of the effects, and game mode sets them up with the game's code. The
models give the same samples as the game's code. Rips of other NintendoWare games play without effects, apart from the
channels and the WiiWare game below. `docs/wsf.md` describes the effects under "Effects".

The rips of five Wii channels have the effects of the channels too: the Wii Shop Channel, the Mii Channel and the Photo
Channel 1.1 (all World), and the Forecast and News Channels (USA). A channel rips in archive mode only. While a photo
with a mood is shown, the Photo Channel plays the music of its photos through a mood effect that the channel defines in
its code. `wsfrip` rips each piece of that music once more in each of the four moods. Players of WSF 1.7.0 and older
refuse the rip of a mood but play the other rips of the channel. The Mii Channel plays its music with other tracks or
at a faster tempo in some of its scenes, and `wsfrip` rips the music once more in each of four such states. The menus
of Mario Kart Wii play two of their pieces with other tracks, depending on the page on screen. `wsfrip` rips each piece
once more in each of those states. `docs/wsf.md` describes the states of the Mii Channel and of the Mario Kart Wii menus
under "Tracks that the game silences".

Pokémon Rumble (WiiWare, USA) plays its music through a reverb. Most of its pieces also play through a delay, and some
through a chorus as well. Its rips have those effects, in archive mode only. WSF 1.8.2 and older play them with the
reverb alone.

The golf of Wii Sports plays its wind only while the hole has wind. The game sets a speed and a direction for the wind
sound, and a rip of the wind plays a speed of 8 (`docs/wsf.md`, "Variables that the game sets"). Mario Kart Wii sets a
value for seven of its sound effects each time it plays one. Six of them rip with the value of their first play in the
game, and the same section of `docs/wsf.md` lists them. Smooth Moves plays the music of its microgames through the
reverb of their stage. A rip of such a piece plays the reverb of the stage that plays it first.

Wii Music's main menu plays its music with other tracks in some of its states. `wsfrip` rips that music once more with
the tracks of each of three such states, in both modes. The game starts the menu in a state that plays tracks 0 to 7.
There, the menu's carousel plays tracks 1 to 6 at 0.74 or 0.51 of their volume (-2.6 or -5.8 dB), and the rips play
them at full volume. Pitch Perfect plays its beats with more tracks at higher levels, and one more track joins near the
end of a round. `wsfrip` rips the beats with the tracks of the lowest levels, and once more with each other set of
tracks. Pitch Perfect also sends its phrases and the jingles of its pictures to the reverb, and their rips do the same.
`docs/wsf.md` describes the states and the levels under "Tracks that the game silences".

Five of Wii Play's minigames add or swap tracks of their music as a game goes on: Table Tennis, Laser Hockey, Billiards,
Find Mii and Tanks. `wsfrip` rips each piece with the tracks that sound as the minigame starts it, and once more with
each later set of tracks, in both modes. Billiards plays two of its states at half the volume of the sound (-6 dB). The
rips play at full volume. `docs/wsf.md` describes the states under "Tracks that the game silences".

Animal Crossing: City Folk plays its outdoor and town music with other tracks in rain and in snow, and the music of its
museum plays other tracks in some rooms. `wsfrip` rips each such piece once more in each of those weathers and rooms, in
both modes. `docs/wsf.md` describes them under "Tracks that the game silences".

Twilight Princess mutes some tracks of five pieces as it starts them, and a rip mutes the same tracks. Hyrule Field's
day music plays one arrangement while Link walks and another while he rides. The rips contain both:
`Z2BGM_FIELD_LINK_DAY` and `Z2BGM_FIELD_LINK_DAY_riding`. The scenes, the fights and the other code that starts music
put 17 pieces in more states. Some states play a track at a volume between 0 and 1. A rip starts a piece in the state
that the game starts it in, such as Castle Town's music in its first room. The rips also contain each other state, named
after the piece and the state, for example `Z2BGM_TOAL_VILLEGE_status1`. Hyrule Field's day music also follows the code
of the field: what Link does, the enemies near him and the time of day. The rips contain each way through the piece, in
each arrangement. `_status12` plays the sections of a long walk or ride, `_status4` to `_status7` the sections of a
fight by their four entries, `_status10` and `_status11` the two endings of a day, `_status8` and `_status9` the starts
that the field's code makes once no music plays, and `_riding_status3` the section of Link standing on his horse.

Game mode also plays four games whose sound library is JAudio2: The Legend of Zelda: Twilight Princess (USA revisions 0
and 2, Europe and Japan), Link's Crossbow Training (USA revision 1, Europe and Japan), and Super Mario Galaxy and Super
Mario Galaxy 2 (USA, Europe and Japan). The rips of Twilight Princess and Link's Crossbow Training have the echo of the
games. Players of WSF 1.1.0 and older refuse the rips of these games. The rips take more processing to play than those
of other games, and a slow computer may not play them in real time. The two Galaxy games stream most of their music.
`wsfrip` rips the pieces that the games play from sequences, such as the jingles and the music of the power-ups. It rips
each of the player's voices once more in Luigi's voice, and the name of that rip ends in `_Luigi`. A voice that plays
the same waves for both players gets one rip. As you play, the Galaxy games put some pieces in other states: the Star
Ball's speeds, riding Yoshi, a Dash Pepper or a Blimp Fruit, Bowser's escape, a galaxy picked on the map of the dome,
and others. A state turns tracks on or off, and some states also change the tempo, the pitch of the tracks or the volume
of one track. `wsfrip` rips each state that plays other notes once more, named after the piece and the state, for
example `BGM_TAMAKORO_speed_very_high`: 20 states of Galaxy's pieces and 35 of Galaxy 2's. Some pieces play a sequence
over one of the streams. A state of such a piece rips the sequence alone, without its stream, and its name contains
`_layer_`, for example `BGM_ASTRO_DOME_layer_rotate`. Each state plays from the start of the piece. In the game, the
piece moves into the state over up to three seconds.

Game mode also plays New Play Control! Donkey Kong Jungle Beat (USA), another JAudio2 game. Its rips require WSF 1.4.0
or later. The game plays many of its effects in the chords of the music. A rip plays each such effect over the music of
a stage as the game does, with the music silent. The game also adds layers to its music as you play: a combo, a flight
with a Helibird and each part of a boss fight turn more tracks on or change them. A rip of Jungle Beat contains each
piece as the game starts it, and 102 variants of 44 pieces, one for each later state. A variant is named after the piece
and what sets its state, for example `JA_BGM_MAIN_01_combo1` and `_combo2` for the layers of a combo, `_helibird` for a
flight with a Helibird, `_round2` and `_change` for the second round of a boss and the change before it, and
`_underwater` for TropicalCoast's music while DK swims below. Some enemies and objects also play a loop beside the music
while they're near DK, such as a sleeping hog or a stunned enemy. A rip contains 135 more variants of 27 pieces for
those, one for each loop that a stage plays, named after the loop, for example `JA_BGM_MAIN_01_enemy_sleeping`. Some
boss fights switch to other music once they start, and the final stage of Survival Mode plays `JA_BGM_BOSS_FINAL` from
the start, in place of the stage music. Four of the variants play loops over such music. Each variant plays its state
from the start of the piece. The game moves from one state to the next with a short fade, and the rips leave out those
changes. The states are those of the GameCube release, plus `JA_BGM_COURSE_SELECT_state1`, the map's music with two
tracks off. The specification, `docs/wsf.md`, lists the states and the stages that play each piece.

Archive mode doesn't play JAudio2. `wsfrip` therefore can't rip another JAudio2 game, or a release of these games that
isn't listed. It stops with an error that says so.

Game mode also plays New Play Control! Pikmin (USA). The sound library of the game is the early JAudio1 of the GameCube
release. `wsfrip` rips the sequenced music and the sound effects. The music is the pieces of the game, and each part of
the piece that the cutscenes play. The effects are those of the game's catalogue, and six that the game's code plays
itself. The streamed music (`.stx`) isn't ripped. Like the rips of Twilight Princess, these require a player newer than
WSF 1.1.0.

Game mode also plays New Play Control! Pikmin 2 (USA). The sound library of the game is the later JAudio1 of the
GameCube release. `wsfrip` rips the 48 pieces of music and the sound effects. The music of a cave changes with the
floor, and the music of a boss with what the boss does. `wsfrip` rips a variant of the piece for the conductor of each
other floor and for each other part of the music of a boss. Two parts of the music of the final boss have a layer that
plays only while a captain stands near the boss. They get one more variant each, labelled `-near`. The streamed music
(`.ast`) isn't ripped. Like the rips of Twilight Princess, these require a player newer than WSF 1.1.0.

## Tools

| Tool | Description |
|---|---|
| `wsfrip [options] <input>...` | Rips each input into a `.wsflib` and a `.miniwsf` per sequence, or per piece for music that a game plays as several sequences together. The output goes in a folder next to the input. The folder has the input's name with `_wsf` added. Each sound archive of a game gets a separate folder with the archive's name. Where two archives of the game have the same name, the names of the folders above them are added. A `.wsflib` that differs from one of the same name in the folder gets the next free name, such as `<name>_2.wsflib`, and the rips already in the folder still play. To replace earlier rips, rip into an empty folder. An input is a Wii disc image, an extracted disc, an unpacked title (a WiiWare game or a channel) or a sound archive (`.brsar`). `wsfrip` also finds the sound archives inside a game's CRI CPK archives (`.cpk`). An extracted disc is a folder with `sys/main.dol` and `files/`, either directly or in a `DATA` folder. Dolphin and Wiimm's tools extract discs in these layouts. An unpacked title is a folder with the title's TMD (`.tmd`) and its decrypted contents (`.app`). A sound archive requires the `main.dol` of its game: `wsfrip` reads AX's microcode and the build of the sound library from it. Give the `main.dol` with the archive or with `--microcode`, or put it in the archive's folder. A WiiWare game or a channel has no `main.dol`, and its main program takes that role. `--microcode` takes the game's unpacked title or extracted disc, and a main program as the title stores it, compressed or not. `--list` lists a game's sound archives, or a JAudio game's sounds (with a JAudio2 game's sections). `--mode game\|archive` chooses the mode. `--archive`, `--only` and `--bgm` choose what to rip. In archive mode, `--tracks`, `--tempo` and `--var` rip each sequence in an arrangement that a game can play it in: some of its tracks, another tempo, or variables set. `--suffix` adds a text to the titles and file names of the rips. Each arrangement of a sound then rips as a separate file. Sequences that make no sound, such as placeholders and those that only set values for the game, are listed and skipped. `--help` shows the rest. |
| `wsfplay [options] <file>...` | Renders each `.miniwsf` to a WAV file next to it at 32,000 Hz, as a soundtrack album plays it: from the first sound, after 0.5 s of silence, for the length and fade in the file's tags, with 1 s of silence after it. `--keep-opening`, `--start-silence`, `--end-silence`, `--loops`, `--loop-fade`, `--one-pass-over`, `--ignore-tags` and `--keep-ending` change that, and `--raw` plays the file as it is, with no skip and no added silence. `-o FILE` writes FILE instead, and its extension selects WAV or FLAC. `--length` and `--fade` override the tags. `--fade-exponent P` sets the curve of the fade, from 0.5 to 4: 1 fades linearly, and the default, 2, fades with the square of the time left in the fade, as most soundtrack albums do. `--info` shows a file's tags and libraries. `--help` shows the rest. |
| `plugins/foobar2000` | foobar2000 input component: `foo_input_wsf.component` for macOS and `foo_input_wsf.dll` for Windows. Both are built with the foobar2000 SDK (see its [README](plugins/foobar2000/README.md)). |
| `plugins/xmplay` | XMPlay input plugin: `xmp-wsf.dll`, a 32-bit DLL for XMPlay on Windows, built with clang-cl and the XMPlay plugin SDK. See its [README](plugins/xmplay/README.md). |

`wsfrip` reads disc images in every common format: ISO, NKit (`.nkit.iso`, `.nkit.gcz`), RVZ, WIA, GCZ, WBFS, CISO and
WDF, whole or split into pieces. The game partition must be stored decrypted. NKit, RVZ and WIA images store it that
way. A plain ISO dump, and a WBFS, CISO, GCZ or WDF made from one, store it encrypted. Dolphin's Convert File makes an
RVZ from any of them without losing data.

`wsfrip` rips titles (WiiWare games and channels) in archive mode only. A WAD contains the contents of a title
encrypted, and `wsfrip` rejects it. Give `wsfrip` a folder with the title's TMD and its decrypted contents instead. A
WAD unpacker that decrypts the contents writes such a folder. The NAND of a Wii keeps each installed title in one:
`title/00010001/<ID>/content` for a WiiWare game and `title/00010002/<ID>/content` for a channel. `<ID>` is the last 8
hex digits of the title ID. Dolphin installs a WAD in its NAND with Tools > Install WAD. The folder of a WiiWare game is
then under `Wii/title/00010001` in Dolphin's user folder, and the folder of a channel under `Wii/title/00010002`.
`wsfrip` reads the contents named by their index or by their content ID. A content that doesn't match its SHA-1 in the
TMD is rejected as encrypted or damaged.

Both tools accept multiple inputs and continue if one fails. A nonexistent input path stops the command before it writes
any files. For example, `wsfrip game.rvz` writes `game_wsf/`. `wsfplay game_wsf/*/*.miniwsf` then renders every sequence
in it. The rips of a JAudio game go in `game_wsf/` itself. `--version` shows a tool's version.

### Ripping faster

Finding the length of each sequence takes most of the time of a rip. `wsfrip` plays every sequence until it ends or
starts to repeat, for 10 minutes at most, and music that hasn't repeated by then plays on for up to an hour. A sound
effect that loops for as long as the game keeps it going, such as wind or a machine, often plays the full 10 minutes. In
a JAudio game, such as Super Mario Galaxy, a sound may also play once before that, to find the waves it uses.

- `--bgm` rips only the music. It's the quickest way to rip a game's soundtrack.
- `--only REGEX` rips only the sounds whose labels match. For example, `--only BOSS` rips the sounds whose labels
  contain `BOSS`. `--list` shows a JAudio game's labels.
- `--archive PATH` rips only one of the sound archives of a game. `--list` shows them.
- `--jobs N` sets how many sequences play at once. The default, the number of CPU threads, is the fastest setting.
- `--no-length` skips the length analysis. The rips then have no length or fade tags, and a player plays each one for
  its default length. Only the sequences at a volume of 0 are skipped.

Dropping a disc image on `wsfrip` rips everything. To use these options, run `wsfrip` from a command prompt or a
terminal:

    wsfrip --bgm "Super Mario Galaxy.rvz"

### Downloads

The Releases page has `wsfrip` and `wsfplay` for 64-bit Windows, the foobar2000 component for 64-bit and 32-bit
foobar2000 on Windows, and the XMPlay plugin. On macOS, build WSF from source (see "Building").

To install the XMPlay plugin, unzip `xmp-wsf-X.Y.Z.zip`, put `xmp-wsf.dll` in XMPlay's folder or in a folder inside it,
and restart XMPlay.

### Drag and drop

- **Windows:** drop files on `wsfrip.exe` or `wsfplay.exe`. A console window shows the results and stays open until you
  press Enter.
- **macOS:** the build also makes `wsfrip.app` and `wsfplay.app` in the build folder. Drop files on one, or open it and
  choose them. The app reports the results for each file and offers to open the output folders.

As above, the results go next to the files. Drop a sound archive together with the `main.dol` of its game if the
folder of the archive doesn't contain that file. Drop the folder of a title, not the files in it. File and folder names
can use any characters. On Windows, that requires Windows 10 version 1903 or later.

## Building

You need Git, CMake 3.20 or later, and C and C++20 compilers. WSF builds on two libraries, and the repository contains
both as submodules: xsflib (`externals/xsflib`), a library for players and rippers of the PSF family, and ntdlib
(`externals/ntdlib`), a library for the hardware and formats of Nintendo's consoles. Clone with `git clone --recursive`,
or run `git submodule update --init --recursive` in a clone. The tests also use seqlib, a submodule of xsflib. zlib,
Zstandard, liblzma, bzip2 and libFLAC are downloaded and built along with WSF when they aren't installed.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload and its "C++ Clang tools for
  Windows" component. The workload includes CMake, and the component adds clang-cl. Configure with
  `cmake -S . -B build -A x64 -T ClangCL` instead of the configure command below. Run the commands in a Developer
  PowerShell or Developer Command Prompt.
- **macOS on Apple Silicon:** the Xcode command line tools (`xcode-select --install`) and CMake (`brew install cmake`).
  `brew install zstd xz flac` saves building those three.

Linux, Intel Macs, MSVC and 32-bit Windows aren't supported. The exceptions are the 32-bit foobar2000 component and the
XMPlay plugin: build them with clang-cl, configured with `-A Win32 -T ClangCL`.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` has `release` and `debug` presets for the same steps: `cmake --preset release`,
`cmake --build --preset release` and `ctest --preset release`. The presets require CMake 3.23 or later and don't select
clang-cl. On Windows, use the commands above.

## Layout

```
src/wii/      the Wii for game mode: its hardware and IOS
src/wsf/      the format and the player
src/rip/      the ripper: the parts that every game shares, the game profiles, a file for each sound library
              and the drivers' images
src/cli/      wsfrip and wsfplay
externals/xsflib/    a library for players and rippers of the PSF family
externals/ntdlib/    a library for the hardware and formats of Nintendo's consoles
plugins/foobar2000/  the foobar2000 component
plugins/xmplay/      the XMPlay plugin
driver/       the sources of the game-mode drivers, for NW4R SND and JAudio, and the script that builds them
docs/         the format's specification
tests/        ctest suites
```

xsflib contains the PSF container, playback and the frames of the tools. Its CMake helpers make the version resources
and manifests of the Windows programs, the macOS droplet apps, the foobar2000 component and the XMPlay plugin. ntdlib
contains the disc reader, the Broadway CPU, the hardware that the GameCube and the Wii share (the DSP that runs AX's
microcode, its interface, EXI, and the processor and audio interfaces) and the sound archive readers. It also contains
the models that archive mode plays on: AX's CPU side, the effects on AX's aux buses (AXFX) and NW4R SND.

The drivers run on the emulated Wii. `driver/build.py` builds them for PowerPC and requires a clang with the PowerPC
target. The driver images are committed in `src/rip/` as `driver_blob.h` (NW4R SND), `jaudio_driver_blob.h` (Twilight
Princess and Link's Crossbow Training), `jungle_beat_driver_blob.h` (New Play Control! Donkey Kong Jungle Beat),
`pikmin_driver_blob.h` (New Play Control! Pikmin), `pikmin2_driver_blob.h` (New Play Control! Pikmin 2),
`galaxy_driver_blob.h` (Super Mario Galaxy) and `galaxy2_driver_blob.h` (Super Mario Galaxy 2). Building WSF itself
requires no PowerPC toolchain.

## Licence

WSF is under the MIT licence (`LICENSE`). `THIRD-PARTY-NOTICES.md` lists the libraries and data that WSF uses and what a
binary distribution must include. This licence doesn't cover game data. Rips contain a game's code and sound data and
are for personal use.
