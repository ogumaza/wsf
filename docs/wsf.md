# WSF: Wii Sound Format

Version 1. WSF stores sequenced music from Nintendo Wii games so that it can be played back as the game plays it. Every
WSF rip contains the game's AX microcode. AX's microcode is the DSP program that mixes the Wii's audio. Every player
runs the microcode on an emulated DSP. A rip uses one of two modes:

- **Game mode**, like 2SF for the DS. The rip contains the game's code. The player runs that code. Game mode is exact.
  It requires a driver profile for the game (see "Ripping").
- **Archive mode**, like NCSF. The rip contains one of the game's sound archives. The player plays it with a model of
  the SDK's sound player: NintendoWare for Revolution's `nw4r::snd`, and the CPU side of the SDK's AX library. Archive
  mode works for any sound archive. See "Archive mode".

Wii games play sounds from NintendoWare sound archives (`.brsar`, RSAR): sequences, wave sounds and streams. WSF plays
sequences. Streams are recorded audio. Other tools already play them.

In game mode, a WSF player emulates the parts of a Wii that a game's code uses to play its sound:

- Broadway, the Wii's PowerPC 750CL CPU, with its floating-point and paired-single instructions;
- MEM1 and MEM2, and the hardware registers of Hollywood, the Wii's system chip, that the game's OS uses: the processor
  interface's interrupts, the DSP interface, the audio interface, the external interface (the clock and console
  settings), the serial interface, and the IPC registers to the I/O processor;
- a high-level model of IOS, the I/O processor's kernel, that handles the requests of the game's OS and sound code;
- the DSP at the instruction level to run the game's AX microcode.

A rip boots the game's `main.dol` from its entry point. The OS initialises itself. The game's static constructors run.
The game's `main` is patched to branch to a small **driver** stored in the rip. The driver sets up the game's sound
library, loads one sound's data from the emulated disc and plays it. It then calls the sound library's per-frame update
forever. The player produces the audio interface's output. The driver doesn't run the game's setup of effects. A sound's
sends to the auxiliary buses are lost. AX loses them the same way when no effect is registered on a bus.

## Files

| Extension | Contents |
|---|---|
| `.wsflib` | Shared data for one sound archive. Game mode: the game's code, the driver, the disc's header and the sound archive. Archive mode: the sound archive and AX's microcode. |
| `.miniwsf` | One sound: a small chunk that selects the sound, plus tags. It references a `.wsflib` with `_lib`. |

Both use the PSF container. There is no standalone `.wsf` file: Windows treats `.wsf` as a Windows Script Host script.

## PSF container

```
0x00  3 bytes  "PSF"
0x03  u8       version: 0x27 for WSF (see "Version byte" below)
0x04  u32      R: size of the reserved area
0x08  u32      N: size of the compressed program
0x0C  u32      CRC-32 of the compressed program
0x10  R bytes  reserved area (uncompressed)
      N bytes  program, zlib-compressed
      "[TAG]" followed by tags (optional)
```

All integers in the container, the chunk headers and the descriptors are little-endian. The CRC-32 is the one used by
zlib. Data that the Wii itself reads, such as the driver's parameter block, keeps the Wii's big-endian byte order.

### Version byte

WSF uses **0x27**. Known PSF version bytes are 0x01 (PSF1), 0x02 (PSF2), 0x11 (SSF), 0x12 (DSF), 0x13 (Sega Genesis),
0x21 (USF), 0x22 (GSF), 0x23 (SNSF), 0x24 (2SF), 0x25 (NCSF), 0x26 (3SF) and 0x41 (QSF). The high nibble 2 marks
Nintendo systems. 0x27 is the next free value after the one for the 3DS. The value is defined once, in
`src/wsf/format.h`.

## Program: chunks

The decompressed program is a sequence of chunks. Each chunk is an 8-byte header followed by its payload. The payload is
padded with zeros to a multiple of 4 bytes:

```
u32  type (four ASCII characters, stored as bytes in order)
u32  size of the payload in bytes (excluding padding)
...  payload
```

| Type | Payload | Meaning |
|---|---|---|
| `MEM ` | `u32 address`, then data | Load the data into the Wii's memory at `address`. The address is a cached address in the game's view of memory: 0x80000000-0x817FFFFF for MEM1 and 0x90000000-0x93FFFFFF for MEM2. |
| `FILE` | `u16 path length`, the UTF-8 path (no leading `/`, `/` separators), zeros to a multiple of 4, then the file data | A file on the game's disc (for example `Sound/sound.brsar`), or a file that the descriptor names. |
| `SND ` | `u32 sound index` | Archive mode: the sound to play, as its index in the sound archive's sound table. The last one in loading order takes effect. |

Unknown chunk types are skipped. Chunks are applied in order. For the same address or path, later data replaces earlier
data.

### Loading order

As in other PSF formats, `_lib` is loaded recursively before the file that references it. Its path is relative to that
file. After the referencing file, `_lib2`, `_lib3` and so on are loaded in numeric order. Loading stops at the first
missing number. An empty `_lib` or `_libN` names no file. Chunks are applied in this loading order. A game-mode
`.miniwsf` usually contains one `MEM` chunk that patches the driver's parameter block. An archive-mode file contains one
`SND ` chunk.

## Reserved area: descriptors

The reserved area may contain a process descriptor (game mode) or an archive descriptor (archive mode). At least one
loaded file must have a descriptor. The last descriptor in loading order takes effect and determines the mode.

### Process descriptor (game mode)

```
0x00  u32  magic "WSFP"
0x04  u32  descriptor version (1)
0x08  u32  entry point: main.dol's, a cached address
0x0C  u32  address of the driver's status word (0 if none)
0x10  u32  the IOS the game runs on, as its title's low word (21 for IOS21)
0x14  u32  flags: bit 0 = the DSP is required; version 1 has no other flags
```

A version 1 player rejects process descriptors with a different version or flags other than 1 (only bit 0 set).

### Archive descriptor (archive mode)

```
0x00  u32  magic "WSFA"
0x04  u32  descriptor version (1)
0x08  u32  flags: bit 0 = the DSP is required; version 1 has no other flags
0x0C  u32  output mode: 1 stereo (version 1 plays stereo only)
0x10  u32  the address the game loads AX's microcode from (a cached address)
0x14  u16  length of the sound archive's path, then the path (UTF-8)
      u16  length of the microcode's path, then the path (UTF-8)
      zeros to a multiple of 4
      u32  the game's build of nw4r::snd, by the date in its version banner as YYYYMMDD (20080308), or 0 if unknown
```

A descriptor that ends after the paths gives a build of 0. The model then follows its base build (see "Archive mode").

The paths identify `FILE` chunks containing the sound archive (`.brsar`) and AX's microcode: the 4,096 big-endian 16-bit
words that the game loads into the DSP's instruction memory. A version 1 player rejects archive descriptors with a
different version, flags other than 1, or an output mode other than 1. Output modes 0 (mono) and 2 (surround) are
reserved for a later version.

## The emulated system (game mode)

A version 1 player provides:

- **The CPU.** Broadway at 729 MHz, one cycle per instruction. Its time base and decrementer count at a quarter of the
  243 MHz bus clock. The CPU starts at the descriptor's entry point with floating point enabled and address translation
  on. Block address translation registers map MEM1 cached at 0x80000000 and uncached at 0xC0000000. They map MEM2 cached
  at 0x90000000 and uncached at 0xD0000000.
- **Memory.** MEM1 (24 MiB at physical 0) and MEM2 (64 MiB at physical 0x10000000). Both are zero-filled and then loaded
  with the `MEM` chunks. Before the game starts, low memory holds what the system menu's loader leaves: the disc
  header's first 32 bytes at 0x80000000, the boot magic 0x0D15EA5E at 0x80000020, the memory sizes, the bus and CPU
  clocks (0x800000F8 and 0x800000FC), the disc's file system table near the top of MEM1 (its address at 0x80000038), the
  OS arenas (in MEM1, ending at 0x81700000 below the driver, and in MEM2, from 0x90000800 to 0x933E0000) and the IOS
  version at 0x80003140.
- **The disc.** A game partition made of the set's files: `sys/boot.bin`, the partition's 0x440-byte header, then a file
  system table generated from the other `FILE` chunks' paths, and their data. The game reads it through IOS's disc drive
  device.
- **Hollywood.** The processor interface's interrupt cause and mask registers drive the CPU's external interrupt. The
  DSP interface (physical 0x0C005000) has the mailboxes, the DSP control register, the ARAM DMA (to MEM2) and the audio
  interface's DMA. The DSP's documentation describes these. The DSP runs one slice behind the CPU. Every 12,600 CPU
  cycles, it runs the cycles of the previous slice, at one DSP cycle per 6 CPU cycles. A CPU read of a mailbox's high
  half first gives the DSP 12 cycles. The audio interface's sample counter counts at the streaming rate while playback
  is on. It starts a period after playback starts or the rate changes. The external interface has the clock (at 0) and
  default console settings in SRAM. Other registers keep what's written to them.
- **IOS.** Requests come through Hollywood's IPC registers. IOS acknowledges them after 1,000 CPU cycles and answers
  after 20,000. The devices: `/dev/stm/immediate` and `/dev/stm/eventhook` (the event hook never fires), `/dev/fs` and
  the NAND files, with `/shared2/sys/SYSCONF` holding stereo sound and default settings, `/dev/di` (the disc) and
  `/dev/es` (the game's title ID). Other devices are missing. The disc is a single-layer disc. Its drive rejects two
  requests that a retail drive rejects and that the SDK's start-up code checks for: an unencrypted read past the disc's
  end ("logical block address out of range", 0x052100) and a ReportKey ("invalid command", 0x052000). RequestError then
  returns those errors.
- **Output.** The audio interface's DMA, 16-bit stereo at 32,000 Hz: 8 samples from main memory every 182,250 CPU
  cycles. The player reports them as they come.

A player may skip the iterations of a three-instruction loop that loads a word from memory, compares it and branches
back while it's unchanged. It may skip them up to the next interrupt check. Nothing else changes memory while the CPU
runs. The result is therefore the same.

## Archive mode

An archive-mode player runs a model of the SDK's sound library on AX's microcode instead of the game's code:

- **The DSP** is the same as in game mode. It runs the microcode from the `FILE` chunk that the descriptor names. The
  model starts the microcode in the state that the DSP's boot ROM leaves it in. It exchanges AX's mails with the
  microcode each frame.
- **AX's CPU side**: 96 voices with AX's priorities, the copy of the voices' parameter blocks to main memory each frame
  with AX's DSP-cycle budget, and AX's command lists. The model mixes in stereo with no auxiliary effects. A sound's
  sends to the auxiliary buses are lost, as in game mode. A frame is 96 samples (3 ms at 32 kHz).
- **`nw4r::snd`**: the sequence player (sequence commands, tracks, variables, random numbers), bank lookup, envelopes,
  LFO, sweep, portamento, pitch and volume and pan curves, the voices and their priorities. The random number generator
  (x = x × 0x19660D + 0x3C6EF35F, from 0x12345678) steps once a frame, after the channels' updates. The sound's first
  frame comes before the generator's first step. Game mode with WSF's driver behaves the same way.

WSF's implementation (`src/ax`, `src/nw4r`) follows the builds of `nw4r::snd` (June 2007) and AX (December 2006) that
Wii Sports and Wii Play link. The archive descriptor gives the game's build of `nw4r::snd`. For builds from March 2008
(Mario Kart Wii) on, the model follows that build where the builds differ:

- The commands 0xa3, 0xa4 and 0xa5 give a time to the command of 0xb0-0xdf that follows them. The time comes from the
  command, a random range or a variable, after the command's argument. The track's volume, pan and surround pan move to
  their new values over that many ticks.
- A note's envelope holds at its peak for (hold + 1)² / 4 milliseconds before it decays. The hold comes from the bank's
  region or from the track's command 0xb1. A track that command 0xb2 makes monophonic plays each note on the last note's
  channel.
- A voice's envelope lands on its target when the step towards it is too small to take in a frame. A released note's
  last frame shows this.
- A note whose region's note-off type is 1 ignores the note-off at the end of its length. A note whose region has a key
  group (its alternate assign) releases the notes of its track in that group as it starts.
- Command 0xe3's sweep is in 64ths of a semitone rather than 32nds. Command 0xe1 keeps the tempo within 0 to 1023. A
  tempo from a variable left at its default of -1 then stops the sequence.

Builds from June 2008 (Wii Music's build) on also differ in these ways:

- Command 0xb3 sets the track's velocity range. A note's velocity is scaled by the range over 127. The velocity can then
  exceed 127. Command 0xfb sets the track's attack, decay, sustain, release and hold back to the bank's values.
- Commands 0xb4 and 0xb5 set the type of a biquad filter on the track's notes and its value over 127. Types 1 to 5 are
  the library's presets: a low-pass, a high-pass and three band-passes. The model applies them. Any other type takes its
  coefficients from a function that the game registers. Archive mode plays such a type unfiltered.

The model also follows an older pair of builds: AX of October 2006 and the build of `nw4r::snd` that comes with it.
WarioWare: Smooth Moves links them. That build of `nw4r::snd` has no version banner. The archive descriptor gives a
build of 0 for it. The model recognises the pair by the AX microcode in the set instead. That microcode reads parameter
blocks of 0x120 bytes. The later microcodes read 0x140 bytes. Where the older builds differ from the base builds:

- AX has 64 voices. AX's parameter blocks are 0x120 bytes long, with a block for updates within a frame and no biquad
  filter. The command list uses other command numbers. AX also counts other DSP cycles for each voice.
- A sequence counts time in 416ths of a tick. Each frame, the player first takes out the whole ticks that it has
  counted. It adds the tempo next. Then it plays the ticks that it took out. The timebase plays no part. A tick lasts
  416 / tempo frames.
- Commands 0xc0, 0xd7 and 0xdc (pan, surround pan and starting pan) raise a value of 0 or 1 by one.
- The pan always follows the square root curve. Each channel of a stereo wave pans from its side, whatever the archive's
  pan mode.
- A released note stops once its volume times its envelope reaches 0, whether or not a track owns it.
- Each frame, a voice's envelope starts at the volume that the last frame aimed for. It steps towards the new target by
  the difference over 96 samples. The step is rounded towards 0. The mix changes at once, with no ramp.

Other versions of the library may differ in other ways. Their archive-mode rips may then differ from what the game
plays. A game whose AX microcode rejects the command lists that the model makes can't be ripped in archive mode. When a
game has a driver profile, game mode is preferred.

Version 1 plays sequence sounds only. A sound's data comes from the groups in the archive. The ripper skips sounds whose
sequence or bank isn't in the archive. The output starts with the first frame after the sound starts. The sound has
finished when the sequence has ended and no voice is sounding.

## Driver (game mode)

Game-mode rips contain WSF's driver, `driver/driver.c`. The driver is loaded at 0x81700000, and its entry point is at
the same address. The driver's parameter block is at **0x81700010**. The block's words are big-endian:

```
0x00  u32  magic "WSFD"
0x04  u32  version (1)
0x08  u32  status (written by the driver): 0 booting, 1 playing, 2 finished, 0x80000000 | step on error
0x0C  u32  sound index: the sound to play
0x10  u32  group index: the archive's group that holds the sound's sequence, and its bank unless 0xB4 says otherwise
0x14  u32  the size of the sound heap the group is loaded into
0x18  u32  time-base ticks between the driver's calls to the sound library's update (1,012,500: 60 Hz)
0x1C  u32  the size of the sound archive's INFO block
0x20  u32  the sound thread's priority (4)
0x24  u32  the disc thread's priority (3)
0x28  64   the sound archive's path on the disc, NUL-terminated
0x68  ...  game profile: addresses of the game's functions, then two sizes (see driver.c)
0xB4  u32  group index of the group with the sound's bank, when the group at 0x10 doesn't hold it, or 0xFFFFFFFF
0xB8  u32  the address of the sound system's flag byte, for a build that checks the flag before setting up the sound
           archive player, or 0
```

A `.miniwsf` patches only the sound, the group and the heap size (a 12-byte `MEM` chunk at 0x8170001C). It also patches
the bank's group when the sound requires one (a 4-byte `MEM` chunk at 0x817000C4). After applying all `MEM` chunks, a
version 1 player checks the driver block. If the block has the expected magic, the player rejects versions other than 1.

The driver starts the audio interface and AX, opens the archive on the disc and sets up the sound archive player. It
loads the group (and the bank's group, if there is one) into a sound heap. Then it starts `nw4r::snd`'s sound system and
the sound. The sound therefore always starts the same number of frames after the sound system. The time the loading
takes doesn't matter. In a 2008 build, the sound archive player can't be set up before the sound system. It doesn't
actually use the system. For such a build, the driver sets the flag byte whose address is at 0xB8 until it starts the
sound system. The driver then calls the sound archive player's update. Between calls, it sleeps for the period at 0x18.
It sets the status to 2 when the sound's handle is free again.

The descriptor's status address points at the status word (0x81700018). A player uses it at both ends of the sound:

- **Start.** The player runs the rip from boot and discards the output until the status first reads 1 or 2. The boot and
  the loading of the sound then don't count towards its length. The player stops with an error if the status has bit 31
  set (the low byte is the step that failed). It also stops with an error if the status hasn't reached 1 after 10
  emulated seconds. With a status address of 0, there's no status word. The output then starts at boot.
- **End.** Like any PSF player, a player plays for the tagged length plus fade. Without a length tag, it plays until
  half a second after the driver reports that the sound has finished (status 2). It stops sooner if a default length and
  fade end first. The player checks the status after every millisecond of emulated time.

## Tags

The standard PSF tags apply: `title`, `artist`, `game`, `year`, `genre`, `comment`, `copyright`, `length`, `fade`,
`volume`, `_lib`, `_libN`. `wsfby` credits the ripper. The ripper also writes `wsf_sound` with the sound's label from
the sound archive. It writes `wsf_mode` (`game` or `archive`) so that players and listeners can tell the modes apart.

`length` and `fade` count time at the output rate of 32,000 Hz: a player plays `length` × 32,000 stereo samples before
the fade.

## Ripping

`wsfrip` builds sets from a Wii disc image, an extracted disc or a sound archive (`.brsar`). The image's game partition
must be stored decrypted. NKit, RVZ and WIA images store it that way. An encrypted partition is rejected. `wsfrip
--list` shows a game's sound archives. A game with a driver profile is ripped in game mode. Anything else is ripped in
archive mode. `--mode` overrides the choice. The rips go in a folder next to the input. The folder has the input's name
with `_wsf` added. `-o` names another folder instead. Each of a game's sound archives gets a separate folder there. The
folder has the archive's name. Where two of a game's archives have the same name, their folder names start with the
names of their parent folders on the disc. As many parent folders are added as it takes to tell the archives apart:
`Sound/BGM/sound.brsar` and `Sound/SE/sound.brsar` go in `BGM_sound` and `SE_sound`.

`--only` selects the sequences whose labels match a regular expression. `--bgm` selects the music. The music is the
sequences whose labels contain BGM, in any case, and those that play on a music player. A music player has BGM, MUSIC,
SONG or FANFARE among the words of its name separated by underscores, and not SE. Digits may follow the word. Games name
their players more consistently than their music. The players therefore find music whose labels don't say what it is.
Given both options, `wsfrip` rips the music whose labels match.

The ripper lists and skips the sequences that make no sound. Some have a volume of 0 in the archive. The sound library
multiplies everything that a sound plays by that volume. The game can't make them heard. The length analysis (see "Game
mode") finds the others. Those are placeholders, sequences that only set variables for the game, muted copies of other
sounds, and sequences that play only on paths that depend on what the game sets or on random numbers. A rip always takes
one path. On that path they make no sound. `--no-length` leaves out the analysis. Then only the sequences at a volume of
0 are skipped.

### Game mode

The `.wsflib` contains:

- the sections of the game's `main.dol`, with `main` patched to branch to the driver;
- the driver and its parameter block with the game profile and the archive's path;
- the disc's header (`sys/boot.bin`) and the sound archive, as `FILE` chunks;
- a process descriptor.

Each `.miniwsf` selects one sequence. The sequence's length comes from playing it on WSF's model of the sound player
(see "Archive mode"). A looping sequence gets two loops plus a 10-second fade. A sequence that ends plays until half a
second after its last sound. Any silence before the sequence ends is cut. A sequence still playing at 600 seconds gets a
length of 600 seconds and the fade. Sound means a sample louder than 8 of 32,767 (-72 dB).

A note of 9,600 ticks or more (200 beats at the default timebase) is a held note. A sound that plays until the game
stops it gives its notes such lengths. The game then stops the sound first.

A sequence loops when its main track jumps back to a command it has already played, or reaches the end of a loop that
repeats forever. A jump back inside a subroutine doesn't count. The exception is a subroutine that the track can't leave
while it holds a held note or a note of length 0 on a looping wave at its sustain level. A track's pass is what the
track plays from the command it jumps back to until the jump. A pass of an outer loop includes the passes of the loops
inside it. A pass changes the sound when the track plays a note, changes a setting of the track or the player, or opens a
track. A tie that continues a note at the same key and velocity changes nothing. A pass that changes nothing is idle. A track that only polls or counts a variable makes idle passes. An
idle pass is never a loop. Any other pass of the first track is a loop. A pass of a later track is a loop if the track
played a note during it and is the main track. A pass that played no note is a loop too while the track holds a held
note or a note of length 0 on a looping wave at its sustain level and every other track still playing rests. A track
rests when it will never continue, or when its last pass was idle and it has changed nothing since. A track is the main
track when no earlier track still playing has loops that would count. The first track's loops count unless its last pass
was idle. A later track's loops count only if its last pass played a note and wasn't idle. A track that will never
continue has no more loops. It waits forever, waits out a held note, or waits for its notes to end while one of them
never will: a note of length 0 on a looping wave. Two loops count together only if they come from the same track and go
back to the same command. A loop of another track, or one that goes back elsewhere, starts the count again. Loops that
go round before the first sound don't count. A track may change a value over and over while a note fades in, or before
another track starts. A loop that completed without a sound ends the sequence at its last sound instead.

A track can't change the sound while its notes sound in four cases: when it waits for its notes of length 0 to end (on
looping waves, they never do), when it waits out a held note, when it waits forever, and when it makes still passes
while none of its notes has length left to run out. A length taken from a variable left at its default of -1 makes a
track wait forever. A still pass is idle and draws no random number. It also leaves every variable that a track reads
with the value that the pass before left. When no track can change the sound, or the tempo is 0, the sequence becomes
steady once nothing else changes: no note's envelope is in its attack, hold, decay or release, no track's volume or pan
is moving, and no wave starts or stops. A steady sequence loops when every running wave has looped twice since the sound
last changed. A steady sequence that has made no sound since then ended at its last sound instead. If no track will
change the sound before the game stops the sequence, the sequence has ended when its last note has. A track that makes
still passes with notes in them will still change the sound. Once its notes end, its next pass starts a new one.

A sequence that stays busy without a sound for 180 seconds ended where it fell silent. An example is one whose music has
ended while a track that plays no notes runs on. The ripper skips a sequence that never makes a sound. For a game whose
AX microcode rejects the model's command lists, the model plays the sequence without the DSP. In place of the microcode,
it follows each voice's position. A wave that doesn't loop then ends when it would on the DSP. Without the DSP, a note
in use counts as sound.

Game profiles (function addresses) are specific to each game and version. They're matched by game ID and the SHA-1 of
`main.dol`. Version 1 supports Wii Sports and Wii Play (both USA, revision 1), and WarioWare: Smooth Moves, Mario Kart
Wii, Wii Music and Animal Crossing: City Folk (all USA). The driver loads the archive's group that holds a sound's
sequence. It also loads the group with the sound's bank when that's another group. The last group in the archive's table
holds the files that no other group does. That group can't be loaded. A sound whose sequence or bank only that group
holds is skipped.

A profile also holds a signature of each of its functions. A signature is the function's length up to its first `blr`,
its first word and an FNV-1a hash of its words. In each word, the fields that the linker fills in are cleared: branch
targets, and the immediates of `addis` and of the loads, stores and adds that don't address the stack. Another release
or revision of a game may link the same functions at other addresses. Its game ID starts with the same three
characters. For such a `main.dol`, the ripper looks for the functions of that game's profile by their signatures. It
then derives a profile from the addresses it finds. A function that matches in several places takes the offset of the
nearest function that matched in one. A profile whose library build checks the sound system's flag (the builds from
2008) can't be derived this way.

### Archive mode

The `.wsflib` holds the sound archive and AX's microcode as `FILE` chunks. It also holds an archive descriptor. Each
`.miniwsf` selects one sequence with a `SND ` chunk. Lengths come from the same analysis as in game mode.

A sound archive doesn't contain AX's microcode. For a game, the ripper takes the microcode from the game's `main.dol`.
For a loose `.brsar`, it takes the microcode from the `main.dol` that `--microcode` names or that's given with the
archive. Otherwise it uses a `main.dol` in the archive's folder. The ripper prints a notice that archive mode uses WSF's
model of the SDK's sound player. The notice also says that ripping the game gives exact rips. Sequences whose data isn't
all in the archive are listed and skipped. Wave sounds aren't ripped.

Tags written by `wsfrip`: `title` and `wsf_sound` (the sound's label), `game`, `length` and `fade`, `wsf_mode`, `wsfby`,
`utf8=1`, and optionally `artist`, `year` and `copyright`.

## Copyright

A `.wsflib` contains copyrighted game code and data, including AX's microcode in both modes. Rips are for personal use.
