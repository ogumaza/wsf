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
forever. The player produces the audio interface's output. For a game with an effect profile, the driver also sets up
the game's effects on AX's auxiliary buses (see "Effects"). For other games, a sound's sends to the auxiliary buses are
lost. AX loses them the same way when no effect is registered on a bus.

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
data. A player loads a decompressed program of at most 1 GiB.

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
      u32  the number of effects (see "Effects"), then each effect:
             u8   bus: 0 A, 1 B, 2 C
             u8   type: 1 FxReverbHi of 2006 and 2007, 2 FxReverbHi of 2008, 3 FxChorus, 4 FxDelay,
                  5 WarioWare: Smooth Moves' standard reverb, 6 FxReverbStd of 2008
             u8   the number of words in the type's parameters (6, 11, 3, 3, 5 and 10)
             u8   0
             u32  the number of presets, or 0 for an effect with one set of parameters
             u32  the number of sounds that the effect lists
             u32  the parameters: the words of one set, or of each preset in turn
             u32  the sounds that the effect lists, by their index in the archive's sound table
      u32  the number of sends that the game sets on sounds, then each send:
             u32  the sound, by its index in the archive's sound table
             u32  the bus
             f32  the send
      u32  the number of sounds whose tracks the game sets (see "Tracks that the game silences"), then each sound:
             u32  the sound, by its index in the archive's sound table
             u32  the tracks that play, bit N for track N
```

A descriptor that ends after the paths gives a build of 0. The model then follows its base build (see "Archive mode"). A
descriptor that ends after the build has no effects and no sends. One that ends after the sends plays every track of
every sound. These blocks are little-endian, like the rest of the descriptor. The parameters are the words of the effect
class's parameter structure: floats as their bits, and the integer modes of the reverbs of 2008 as integers. "Effects"
says which sounds get an effect: the sounds that it lists pick its presets or, without presets, get the effect. A player
skips an effect of a type that it doesn't know, and one whose number of words doesn't fit its type or whose bus isn't A,
B or C. It skips a sound whose tracks have a bit above bit 15 set. Players older than 1.4.0 ignore the effects and the
sends. Players of 1.4.1 and older ignore the tracks.

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
  version at 0x80003140. Since WSF 1.4.0, 0x80003188 holds the IOS that the game's TMD requests, with the same version.
  The OSInit of a later SDK stops the game unless the two words match. Players before 1.4.0 leave 0x80003188 at 0. They
  can't start the rips of New Play Control! Donkey Kong Jungle Beat.
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
  with AX's DSP-cycle budget, and AX's command lists. The model mixes in stereo. While an auxiliary bus has an effect,
  the command list has the microcode mix the bus, and the effect runs in each frame (see "Effects"). Without one, a
  sound's sends to the bus are lost. Game mode loses them the same way. A frame is 96 samples (3 ms at 32 kHz).
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

Later builds differ again. The earliest builds seen with these changes are of November 20 2008 and September 24 2009.
The latest build seen without the first change is of September 19 2008. The builds between those dates haven't been
checked. The model takes them as the older kind.

- Builds from November 2008 on take the low-pass filter's cutoff from a table. The Wii U's library does the same. A
  value below 0.169 gives 80 Hz. Above that, each thirtieth of the value raises the cutoff by about a third of an
  octave. The highest step is 12800 Hz, from 0.869. A value of 0.9 or more gives 16000 Hz. That turns the filter off.
  Older builds compute the cutoff as 32000 × 2^(10 (v − 1)) Hz for a value v from 0 to 1.
- Builds from September 2009 on let a note's volume and its envelope go above full level. A volume LFO on a note at full
  level then swings above full level as well as below. AX's envelope stops at twice full level. Older builds hold the
  volume and the envelope to full level.
- Builds from September 2009 on also move a track's pitch bend (command 0xc4) over the time that the commands 0xa3,
  0xa4 and 0xa5 give. The model doesn't yet. It sets the bend at once.

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

## Effects

Games put effects on AX's three auxiliary buses, A, B and C: reverbs, choruses and delays. The SDK's AXFX library has
the effects. `nw4r::snd` wraps each in an effect class: `FxReverbHi`, `FxReverbStd`, `FxChorus` and `FxDelay`.
`AxManager::AppendEffect` puts an effect on a bus. Each voice sends some of its sound to the buses. While a bus has an
effect, AX's command list has the microcode mix the bus into a ring of three frames in main memory. AX's interrupt then
runs the bus's callback on the frame that the DSP mixed two frames before. The callback runs the bus's effects in turn.
The microcode mixes the result back into the main mix two frames later. Each frame of a bus holds 96 samples of the
left, right and surround channels as 32-bit integers.

WSF plays the effects of the games that it has an effect profile for. A profile is for one release: the `main.dol` whose
SHA-1 it names. The ripper writes the effects for each sound archive into the archive descriptor and into the driver's
parameter block. The rips of other games of NW4R SND play without effects. Before 1.4.0, all of them did. For a game
without a profile, `wsfrip` says once that its rips play without the game's reverb and other effects.

| Game | Bus A | Bus B | Bus C |
|---|---|---|---|
| Wii Sports (USA, revision 1) | a reverb: the menus' in the common archive, each sport's in its archive | tennis, baseball and bowling: a chorus | none |
| Wii Play (USA, revision 1) | a reverb | a chorus | a delay |
| WarioWare: Smooth Moves (USA) | a reverb that the music picks | a delay | a chorus |
| Animal Crossing: City Folk (USA) | a reverb, with the game's sends on some music | none | none |
| Mario Kart Wii (USA) | a reverb with the parameters of the sound's scene | none | none |
| Wii Music (USA) | a reverb, and another for Mii Maestro | none | none |

The ripper takes each effect's parameters from the game's code. A game sets its effects up when a scene starts, long
before its music plays. A rip sets them up with its sound instead: the effects start in the first AX frame after the
sound has started. A chorus's oscillator then starts with the sound. A bus's callback gives silence for its first two
frames. The game's callback does too.

Some games change a bus's effect from scene to scene. The profile then has an effect for each setting. Each effect
lists the sounds of the scenes that use it. An effect without presets is set up for the sounds that it lists. One that
lists no sounds is set up for every sound that no other effect without presets on its bus lists. An effect with presets
is set up for every sound. The sounds that it lists pick its presets. A sound whose scene isn't known gets the effect
that lists no sounds.

- **Wii Sports** puts `nw4r::snd`'s default reverb on bus A in its menus. Each sport sets bus A as its scene starts.
  Golf and boxing have reverbs of other times and levels. Tennis, baseball and bowling keep the default reverb and add
  `FxChorus`'s default chorus on bus B. The ripper gives the archive of each sport that sport's effects, and the common
  archive the menus'.
- **Wii Play** has the same effects in every scene.
- **WarioWare: Smooth Moves** sets its delay and chorus up once at boot. Its music picks the reverb on bus A. In each
  frame, the game reads the music's local variables 0 and 1, in that order. A value from 1 to 10 that differs from the
  last one sets that row of a table of 10 reverbs, clears bus A and appends the reverb again. A value of 0 clears bus A.
  Other values change nothing. In a rip, the variables of the music do the same in each AX frame. The music is what
  `wsfrip --bgm` takes. A sound effect plays with bus A empty: the state of the game before any music has picked a
  reverb. The reverb is the SDK's "standard" reverb of 2006.
- **Animal Crossing: City Folk** has one reverb on bus A, with the parameters of `nw4r::snd`'s builds of 2008. As it
  starts a piece of music, the game sets the sound's send to bus A: 0.25 for the outdoor and town music, and 0.28 for
  K.K. Slider's songs. The descriptor lists those sends by sound. A rip sets the send as the sound starts.
- **Mario Kart Wii** has the standard reverb of 2008 on bus A. As each scene starts, the game sets the reverb from one
  set of parameters. A race on Mushroom Gorge or on the N64 courses Mario Raceway, Sherbet Land and DK's Jungle Parkway
  shortens the pre-delay and lowers the damping. The menus pick a setting by the music that their scene loads. The
  character selection and the globe of the online menus lower the output gain. The rankings have a shorter reverb. The
  ripper gives each course's sounds the course's reverb. Each piece of menu music gets the reverb of its scene. Other
  sounds get the reverb as the game sets it up first. Only six of the archive's 1,460 sequences send anything to bus A:
  four pieces of menu music and two sounds of N64 Sherbet Land. The scene of one of them, `SEQ_O_OPTION_CH`, isn't
  known.
- **Wii Music** has the standard reverb of 2008 on bus A in every scene but Mii Maestro. There, bus A has an
  `FxReverbHi` of 2008. The ripper gives Mii Maestro's sounds (`RP_OCS_`) that reverb. Some scenes put a reverb on bus B
  too. No sequence sends anything to bus B. In a jam session, the game sets the instruments' sends to the buses by
  stage. A rip doesn't know the stage and plays the instruments without those sends.

Archive mode plays WSF's models of the effects: `AXFXReverbHi` and its core `AXFXReverbHiExp` of 2006 and 2008, the
standard reverbs of October 2006 and of September 2008 (`AXFXReverbStdExp`), `AXFXChorus` with its core
`AXFXChorusExp`, and `AXFXDelay`. Each model follows the game's code operation for operation, in single precision
where the game uses it. The models give the same samples as the games' code. Their tests run each game's functions on
an emulated Broadway and compare thousands of frames. The ripper's length analysis plays the effects too. A sequence
that ends plays until its effects' output falls silent.

Game mode sets the effects up with the game's code and runs them on the same models. The game's code for an effect
takes tens of thousands of instructions in each AX frame. The models take a small part of that time on the computer
that plays the rip. The driver's block names each effect's class, with the class's `StartUp`, `Shutdown` and
`UpdateBuffer` (see "Driver"). The player replaces the first instruction of each of those and of `SetParam` with an
instruction that Broadway doesn't have. When the CPU stops there, the player makes the same call on its model of the
object. Then the player does what the replaced instruction does and lets the game's code go on. In `UpdateBuffer`, the
model processes the bus's frame in memory instead. The CPU returns to the caller at once. The call takes one cycle of
emulated time. A player's save state holds the models.

Twilight Princess's sound library, JAudio2, puts no effects on AX's buses. JAudio2's DSP program mixes four FX lines
instead. Each line is a delay with a feedback filter. The game's audio set-up sets all four lines from the first
configuration in the start-up archive (`.baa`). Game mode runs that set-up and the game's DSP program. Game-mode rips of
Twilight Princess therefore have the game's echo. As a scene starts, the game sets lines 2 and 3 from the scene's
configuration. Those two lines belong to the surround mix. In stereo, the game's echo is the same in every scene. Link's
Crossbow Training sets up the lines in the same way. Game-mode rips of that game have its echo too.

## Tracks that the game silences

A game can play only some tracks of a sequence. As the sound starts, the game sets the volumes of the other tracks to 0
through the sound's handle (`SeqSound::SetTrackVolume`). Those tracks still run and take voices. A sequence can then
hold several arrangements of a piece, and the game picks one. An effect profile can give the tracks that a game plays
of some sounds. The ripper writes them into the archive descriptor and into the driver's parameter block. In archive
mode, a sound plays those tracks unless its `wsf_tracks` tag gives others (see "Tags"). Game mode's driver sets the
other tracks' volumes to 0 as it starts the sound. The length analysis plays the same tracks.

**Animal Crossing: City Folk** has such arrangements in its outdoor music (`BGM_OUTDOOR_00` to `23`), its town music
(`BGM_TOWN_EARLY`, `_DAY`, `_NIGHT` and `_MIDNIGHT`) and its museum's music (`BGM_MUSEUM`). Each piece of outdoor and
town music holds three arrangements, one for each kind of weather. A rip plays the one for the game's first three
kinds of weather. The town music also silences track 15 in every weather. The museum's music has an arrangement for
each room. A rip plays the one that the game picks by default as the music starts. With `--mode archive`, `--tracks`
rips another arrangement of a piece.

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
0xBC  ...  the effects' functions (see driver.c): AXRegisterCallback, AxManager's GetInstance, AppendEffect and
           ClearEffect, then SeqSoundHandle's constructor and destructor and SeqSound::ReadVariable, for presets
0xD8  u32  the offset of a sound's sends in nw4r::snd's BasicSound, for a send that the game sets on the sound
0xDC  u32  the number of effects, 0 to 4 (0: the driver sets up no effect)
0xE0  u32  1 if the sound's variables pick the presets, or 0
0xE4  u32  the bus of the send that the game sets on the sound, or 0xFFFFFFFF
0xE8  f32  that send
0xEC  ...  4 effects of 8 words: the bus, the effect class's constructor, GetRequiredMemSize, AssignWorkBuffer and
           SetParam, the number of words in its parameters, the number of presets (0 for one set), and the index of
           its first word at 0x16C
0x16C ...  64 words: the effects' parameters, as in the archive descriptor
0x26C u32  for the player: the build of AXFX that the game links (0 October 2006, 1 December 2006, 2 September 2008)
0x270 ...  for the player: 4 effects of 4 words, the effect class (its type in the archive descriptor) and the class's
           StartUp, Shutdown and UpdateBuffer
0x2B0 u32  the offset of the SeqPlayer's 16 track pointers in nw4r::snd's SeqSound, for tracks that the game silences
0x2B4 u32  the offset of the volume in SeqTrack
0x2B8 u32  the tracks that the game silences on the sound, bit N for track N, or 0
```

A `.miniwsf` patches only the sound, the group and the heap size (a 12-byte `MEM` chunk at 0x8170001C). It also patches
the bank's group when the sound requires one (a 4-byte `MEM` chunk at 0x817000C4). For a sound whose variables pick the
presets, or on which the game sets a send, it patches 0xE0 to 0xEB too (a 12-byte `MEM` chunk at 0x817000F0). The
library sets up the effects of a sound that no effect lists (see "Effects"). A `.miniwsf` whose sound gets other effects
patches the block from 0xDC to the end of 0x270's effects instead (a 468-byte `MEM` chunk at 0x817000EC). For a sound
whose tracks the game silences, it patches 0x2B8 (a 4-byte `MEM` chunk at 0x817002C8). After applying all `MEM` chunks,
a player checks the driver block. If the block has the expected magic, the player rejects a version that it doesn't
support. WSF 1.1.0's players support version 1. Later players support versions 1 and 2.

The driver starts the audio interface and AX, opens the archive on the disc and sets up the sound archive player. It
loads the group (and the bank's group, if there is one) into a sound heap. Then it starts `nw4r::snd`'s sound system and
the sound. The sound therefore always starts the same number of frames after the sound system. The time the loading
takes doesn't matter. In a 2008 build, the sound archive player can't be set up before the sound system. It doesn't
actually use the system. For such a build, the driver sets the flag byte whose address is at 0xB8 until it starts the
sound system. The driver then calls the sound archive player's update. Between calls, it sleeps for the period at 0x18.
It sets the status to 2 when the sound's handle is free again.

With effects, the driver makes the game's effect objects after it starts the sound system. Each object gets a work
buffer for the largest of its presets. The driver then registers a frame callback with AX in place of the one that
`nw4r::snd` registered. The driver's callback calls that one first. With effects or tracks to silence, the driver turns
interrupts off and starts the sound. It sets the volumes of the tracks that the game silences to 0, as
`SeqSound::SetTrackVolume` does. With effects, it then sets the game's send on the sound and calls the update once. That
update passes the send to the sound's player. The driver turns interrupts on again. In the next AX frame, the driver's
callback appends the effects without presets. In every frame after that, the callback lets the sound's variables pick
the presets. Archive mode does the same.

The descriptor's status address points at the status word (0x81700018). A player uses it at both ends of the sound:

- **Start.** The player runs the rip from boot and discards the output until the status first reads 1 or 2. The boot and
  the loading of the sound then don't count towards its length. The player stops with an error if the status has bit 31
  set (the low byte is the step that failed). It also stops with an error if the status hasn't reached 1 after 10
  emulated seconds. With a status address of 0, there's no status word. The output then starts at boot.
- **End.** Like any PSF player, a player plays for the tagged length plus fade. Without a length tag, it plays until
  half a second after the driver reports that the sound has finished (status 2). It stops sooner if a default length and
  fade end first. The player checks the status after every millisecond of emulated time.

### JAudio2's driver (version 2)

Game-mode rips of games whose sound library is JAudio2 contain WSF's other driver, `driver/jaudio.c`. It loads at the
same address, and its parameter block has the same place and magic. Its version is 2, and the format's other rules stay
at version 1. WSF 1.1.0's players refuse it with "driver block version 2; this player supports version 1". The block:

```
0x00   u32  magic "WSFD"
0x04   u32  version (2)
0x08   u32  status: the same as version 1's
0x0C   u32  sound ID: the ID that the game passes to its sound library for the sound to play
0x10   u32  the sound's handle (written by the game): its sound while it plays, then 0
0x14   u32  the number of wave groups to load, at most 64
0x18   256  the wave groups: each the wave system in the high half and the group in the low half
0x118  u32  video frames between the calls to the game's audio frame process (2)
0x11C  ...  the sizes that the game's start-up gives, and the addresses of the game's functions and variables (40 words,
            in the order of ntdlib's drivers/jaudio/z2.h)
0x1BC  u32  the address of the game's function that erases a wave group of the music's wave system (1), or 0
0x1C0  u32  the address of the game's function that erases a wave group of the sound effects' wave system (0), or 0
0x1C4  u32  the number of the first scene's wave groups that follow, at most 4
0x1C8  16   the first scene's wave groups, each as at 0x18
0x1D8  u32  the number of the start-up's other wave groups that follow, at most 4
0x1DC  16   the start-up's other wave groups, each as at 0x18, erased only to make room
0x1EC  u32  the address of the game's sleep of the calling thread, or of the driver's sleep (0x224)
0x1F0  u32  time-base ticks in one video frame (1,012,500: 60 Hz)
0x1F4  u32  the address of the game's variable that holds the reset data's address
0x1F8  u32  the reset data's byte that lets the audio set-up read the disc
0x1FC  u32  for the driver's sleep: the address of the game's OSSetAlarm
0x200  u32  the address of the game's OSDisableInterrupts
0x204  u32  the address of the game's OSRestoreInterrupts
0x208  u32  the address of the game's OSSuspendThread
0x20C  u32  the address of the game's OSResumeThread
0x210  u32  for the driver's loading of a wave group: the address of the game's variable that holds JAUSectionHeap's
            instance
0x214  u32  the offset in that object of two words: the address of the wave banks' array, then its length
0x218  u32  the address of the game's JASWaveArc::load
0x21C  u32  the address of the game's JASWaveArc::loadTail
0x220  u32  the address of the game's JASWaveArc::erase
0x224  u32  for the ripper (written by the driver's build): the address of the driver's sleep
0x228  u32  the address of the driver's function that loads a wave group of wave bank 0
0x22C  u32  the address of the driver's function that loads a wave group of wave bank 1
0x230  u32  the address of the driver's function that gives a wave group's load status
0x234  u32  the address of the driver's function that erases a wave group of wave bank 1
0x238  u32  the address of the driver's function that erases a wave group of wave bank 0
```

A `.miniwsf` patches the sound ID (a 4-byte `MEM` chunk at 0x8170001C) and the wave groups (a `MEM` chunk at 0x81700024
with the count and the groups).

The driver repeats the game's start-up for sound with the game's functions: the heaps, the ARAM manager (the Wii keeps
it in MEM2), the thread switch manager, the disc thread, and the audio set-up. Before that, it sets up the reset data
that the game's main would allocate. The audio set-up loads the first scene's wave groups among others. The game's first
change of scene erases them. Before it loads the sound's wave groups, the driver erases those of the first scene's
groups that the list at 0x18 doesn't hold. It then loads the wave groups. The game's wave heap puts each group below the
lowest group in it. Space that an erased group leaves above that group stays unused. If a group doesn't fit, the driver
waits until none of the sound's groups is loading. It erases the sound's groups that loaded and the groups at 0x1DC.
Then it loads the sound's groups again. Once they're loaded, the driver starts the sound and calls the audio frame
process. It sleeps between calls for the video frames at 0x118. The function at 0x1EC does the sleeping. Some games
have no such function. In others, the scene manager has no function that loads or erases one wave group. For such a
game, the ripper writes the addresses of the driver's functions from 0x224 where the game's functions would go: at
0x1EC, and in the list at 0x11C in place of loadSeWave, loadBgmWave, getWaveLoadStatus, eraseBgmWave and eraseSeWave.
The driver's sleep works like Twilight Princess's threadSleep. It sets an alarm with the functions at 0x1FC to 0x20C.
The alarm's handler resumes the thread. The driver's functions for a wave group call the functions at 0x218 to 0x220.
Twilight Princess's scene manager calls them the same way. A group of wave bank 0 loads at the wave heap's head. A group
of bank 1 loads at the tail. The status rules are version 1's. On an error, the low byte gives the step: 1 the block's
header, 2 the root and system heaps, 3 the game's heaps, 4 the audio heap, 5 the audio set-up, 6 the wave groups, 7 the
start of the sound. Steps 5 and 6 give up after 600 frames.

### Donkey Kong Jungle Beat's driver (version 2)

Game-mode rips of New Play Control! Donkey Kong Jungle Beat contain a third driver, `driver/jungle_beat.c`. It loads at
the same address. Its block has the same place, magic and version as the block of `driver/jaudio.c`. The first 0x118
bytes of the two blocks match: the header, the status, the sound ID, the handle and the wave groups. A `.miniwsf`
patches them in the same way. The profile follows:

```
0x118  u32  time-base ticks in one frame of the game (1,012,500: 60 Hz)
0x11C  ...  the sizes that the game's start-up gives, and the addresses and offsets of the game's functions, variables
            and tables (34 words, in the order of driver/jungle_beat.c's Params)
```

The driver repeats the game's start-up for sound with the game's functions: the root heap, the game's heaps, the ARAM
manager (the game keeps its "ARAM" in MEM2) and the sound files. The game's audio system then starts. The driver takes
the Wii Remote speaker's manager out of it. That manager sends the speaker's sounds to the Wii Remotes. The player has
no Wii Remotes. Then the driver loads the wave groups and starts the sound. The game's BGM player starts music. It sets
each track of a piece to the piece's first state from tables in the game's code. A piece whose first state turns every
track off gets its next state at once. Other sounds start in the game's sound manager. The driver calls the audio frame
process once a frame and sleeps for a frame between calls. On an error, the low byte of the status gives the step: 1 the
block's header, 2 the game's heaps, 4 the sound files, 5 the audio system, 6 the wave groups, 7 the start of the sound.
Step 6 gives up after 600 frames.

### New Play Control! Pikmin's driver (version 2)

Game-mode rips of New Play Control! Pikmin contain the driver `driver/pikmin.c`. The game's sound library is an early
JAudio1 that the game calls through C functions (see "Game mode for Jac games"). The driver loads at the same address.
Its block has the same place, magic and version as the block of `driver/jaudio.c`. The sound ID and the wave groups
have the same places too. A `.miniwsf` patches them in the same way. The block:

```
0x00   u32  magic "WSFD"
0x04   u32  version (2)
0x08   u32  status: as in version 1
0x0C   u32  sound ID: 0x80000000 | the part << 8 | the piece's number in the game's sequence index
0x10   u32  the address of a word that holds the sound's root track while the sound plays, then 0 (written by the
            driver)
0x14   u32  the number of wave groups to load, at most 64
0x18   256  the wave groups: each the wave system in the high half and the group in the low half
0x118  u32  time-base ticks between the driver's polls (1,012,500: 60 Hz)
0x11C  ...  the sizes and values that the game's start-up gives, and the addresses of the game's functions and
            variables (26 words, in the order of driver/pikmin.c's Params)
```

The driver gives the sound library the memory that the game's start-up gives it: a heap in MEM1, and a block of MEM2 in
place of ARAM. The driver also gives the library a buffer for the disc before it starts the library. The library loads
the game's sequence index, the banks and the sound effects' first wave group. Once the library is up, the driver loads
the wave groups. For part 0, the driver reads the piece's sequence first. Once the disc has read those files, the driver
plays the piece in the game's slot for music. The piece's root track is the sound's root track. The game's cutscenes
play parts of one piece. The sound library starts that piece during the library's start-up. For any other part, the
driver writes the part's number to a port of that piece's root track. A cutscene does the same. The root track then
opens a track that plays the part. That track is the sound's root track. The driver polls once a frame and sleeps
between polls. The driver sets the status to 2 when the sound's root track is free again. On an error, the low byte of
the status gives the step: 1 the block's header, 2 the start of the sound library, 3 the disc's read of the sequence
and the wave groups, 4 the start of the piece, 5 the root track that a cutscene writes to, 6 the start of the part.
Steps 2, 3 and 6 give up after 600 polls.

### New Play Control! Pikmin 2's driver (version 2)

Game-mode rips of New Play Control! Pikmin 2 contain the driver `driver/pikmin2.c`. The game's sound library is a later
JAudio1. A sound layer in the game's code calls the library. The driver loads at the same address. Its block has the
same place, magic and version as the block of `driver/jaudio.c`. The first 0x118 bytes of the two blocks match, and a
`.miniwsf` patches them in the same way. The sound ID is a piece's number in the table at 0x21C with bit 31 set, or an
effect's ID. The profile follows:

```
0x118  u32  time-base ticks between the sound layer's frames (2,025,000: 30 Hz)
0x11C  ...  15 sizes that the game's start-up gives, then 47 addresses of the game's functions, variables and strings
            (in the order of driver/pikmin2.c's Params)
0x214  u32  the Home Button menu's wave group, as at 0x18
0x218  u32  the number of pieces in the table, at most 64
0x21C  ...  the pieces, 0x44 bytes each: a word with how the game makes the piece's object (bits 0 to 7), a boss's
            start point (bits 8 to 15) and the root track's beat interval (bits 16 to 31, 0 for the default); the
            object's sound information (4 words); the sequence's file name (24 bytes); an AutoBgm's conductor (24 bytes)
```

The driver repeats the game's start-up for sound with the game's functions. It enters the game's folder on the disc. It
makes the root heaps of MEM1 and MEM2, the system heap, the ARAM manager (the game keeps its "ARAM" in MEM2) and the
heap of the sound system. It reads the start-up archive, and makes the sound layer's factory and its global scene. The
global scene loads group 0 of wave system 0, the Home Button menu's wave group and the effects sequence. For a piece,
the driver makes a scene of the type that the game gives the challenge mode's menu. The scene loads the other groups at
0x18, one in each of its two areas. The driver makes the piece's object in it as the game makes it, and the scene starts
the piece. A boss's music starts at the boss's start point. An effect starts as the game's system effects start. The
driver calls the sound layer's frame work once a frame and sleeps between calls for the time at 0x118. On an error, the
low byte of the status gives the step: 1 the block's header or the piece's number, 2 the disc and the heaps, 3 the sound
system, 4 the global scene's resources, 5 the piece's scene and object, 6 the scene's resources, 7 the start of the
sound. Steps 4 and 6 give up after 600 frames.

### Super Mario Galaxy's driver (version 2)

Game-mode rips of Super Mario Galaxy contain another driver, `driver/galaxy.c`. It loads at the same address. Its block
has the same place, magic and version as the block of `driver/jaudio.c`. The first 0x118 bytes of the two blocks match:
the header, the status, the sound ID, the handle and the wave groups. A `.miniwsf` patches them in the same way. The
profile follows:

```
0x118  u32  time-base ticks in one frame of the game (1,012,500: 60 Hz)
0x11C  ...  the sizes that the game's start-up gives, and the addresses and offsets of the game's functions, variables
            and paths (38 words, in the order of driver/galaxy.c's Params)
```

The driver repeats the game's start-up for sound with the game's functions: the root heaps, the game's heaps, the ARAM
manager (the game keeps its "ARAM" in MEM2) and the sound files. The game's audio system then starts. The driver takes
the Wii Remote speaker's system out of it. That system sends the speaker's sounds to the Wii Remotes. The player has no
Wii Remotes. The audio system turns its sounds down while the video interface reports a dimmed screen. The driver
therefore gives the interface's screen-saver threshold the value that the SDK's VIInit gives it. The driver then sets
the volumes that the game sets as each scene starts. The audio system loads one wave group itself. The game's scene
manager then loads the wave groups that the game loads before its first scene. Then the driver loads the listed wave
groups and starts the sound. Music starts in the game's BGM manager. The manager sets each track of a piece to the
piece's first state from a table in the game's code. Other sounds start in the audio system. The driver calls the audio
frame process once a frame and sleeps for a frame between calls. On an error, the low byte of the status gives the step:
1 the block's header, 2 the game's heaps, 4 the sound files, 5 the audio system, 6 the wave groups, 7 the start of the
sound. Each of step 6's three waits gives up after 600 frames.

### Super Mario Galaxy 2's driver (version 2)

Game-mode rips of Super Mario Galaxy 2 contain a third driver for JAudio2, `driver/galaxy2.c`. It loads at the same
address. The first 0x118 bytes of its block match those of the other two blocks. The profile follows:

```
0x118  u32  time-base ticks in one frame of the game (1,012,500: 60 Hz)
0x11C  ...  the sizes that the game's start-up gives, the game's number for the disc's language, and the addresses and
            offsets of the game's functions and variables (44 words, in the order of driver/galaxy2.c's Params)
```

The game's sound code is the same as Super Mario Galaxy's. Its start-up for sound differs. Super Mario Galaxy 2 loads
its files through a file loader: a thread that reads files and mounts archives on request. The driver starts the loader.
Then the game's audio wrapper requests the sound files from the loader and creates the audio system. The game's start-up
makes the same calls. The game's file functions look for a translated file first. They add the folder of the game's
language to each path. The language comes from an object that the game's system object holds. The driver makes stand-ins
for the two objects. The stand-ins hold only the language of the disc's region and the audio wrapper. Like Super Mario
Galaxy's driver, the driver takes the Wii Remote speaker's system out of the audio system. The driver also gives the
screen-saver threshold the value that VIInit gives it and sets the volumes that the game sets as each scene starts. The
audio system loads one wave group itself. The driver then has the audio system read three of its tables: the music's
parameters, the pieces that play a stream together with a sequence, and the stages' music. The game reads them while it
loads more waves at start-up. Those waves belong to the sounds that the particle effects of the game's stationed stages
play. The driver loads none of them. It loads the listed wave groups and starts the sound the way Super Mario Galaxy's
driver does. On an error, the low byte of the status gives the step: 1 the block's header, 2 the game's heaps, 4 the
stand-ins, 5 the audio system, 6 the wave groups, 7 the start of the sound. Each of step 6's two waits gives up after
600 frames.

## Tags

The standard PSF tags apply: `title`, `artist`, `game`, `year`, `genre`, `comment`, `copyright`, `length`, `fade`,
`volume`, `_lib`, `_libN`. `wsfby` credits the ripper. The ripper also writes `wsf_sound` with the sound's label from
the sound archive. It writes `wsf_mode` (`game` or `archive`) so that players and listeners can tell the modes apart.

Three tags arrange a sequence in archive mode. A game can do the same through the sound's handle. A game may play one
sequence in several arrangements: with some of its tracks, at another tempo, or with variables that change what it
plays. Each arrangement then rips as a separate file. In archive mode, a player applies the tags after it sets up the
sequence and before the sequence's first tick. A player ignores a tag that's malformed or out of range. The other two
tags still apply. Game mode ignores all three.

- `wsf_tracks` gives the tracks that play. A player sets the track volume of every other track to 0. Such a track still
  runs. Its notes take voices, and its commands change the variables that other tracks read. The tag replaces the tracks
  that the archive descriptor gives the sound (see "Tracks that the game silences"). The value is one or more track
  numbers from 0 to 15 and ranges such as `8-14`, separated by commas or white space. Each number is decimal.
- `wsf_tempo` multiplies the sequence's tempo. The pitch stays the same. The value is a decimal number from 0.25 to 4,
  such as `1.1` or `0.9`. A player rounds it to single precision.
- `wsf_var` sets sequence variables. Some sequences play nothing until the game sets one of their variables. The value
  is one or more assignments `N=V`, separated by commas or white space. Line breaks count as white space. A tag on
  several lines therefore works too. N is 0 to 15 for the sound's player variables, and 16 to 31 for the global
  variables (global variable N − 16). V is −32,768 to 32,767. Each number is decimal, or hex with a `0x` prefix, and may
  have a minus sign. A later assignment to the same variable wins. A player ignores the whole tag if any part of it is
  malformed or out of range.

`length` and `fade` count time at the output rate of 32,000 Hz: a player plays `length` × 32,000 stereo samples before
the fade.

## Ripping

`wsfrip` builds sets from a Wii disc image, an extracted disc, an unpacked WiiWare title (see "WiiWare titles") or a
sound archive (`.brsar`). The image's game partition must be stored decrypted. NKit, RVZ and WIA images store it that
way. An encrypted partition is rejected. `wsfrip --list` shows a game's sound archives. A game with a driver profile is
ripped in game mode. Anything else is ripped in archive mode. `--mode` overrides the choice. A game with neither a sound
archive of NW4R SND nor JAudio2's start-up archive holds nothing that WSF rips. `wsfrip` stops with an error for it.
`wsfrip --list` says the same. The rips go in a folder next to the input. The folder has the input's name with `_wsf`
added. `-o` names another folder instead. Each of a game's sound archives gets a separate folder there. The folder has
the archive's name. Where two of a game's archives have the same name, their folder names start with the names of their
parent folders on the disc. As many parent folders are added as it takes to tell the archives apart:
`Sound/BGM/sound.brsar` and `Sound/SE/sound.brsar` go in `BGM_sound` and `SE_sound`.

Some games keep a sound archive inside an archive of CRI Middleware's file system, a CPK (`.cpk`). The ripper also
reads the sound archives in a game's CPK archives. A CPK may compress a file with CRILAYLA. The ripper decompresses
such a sound archive. A sound archive inside a CPK has the CPK's path, a slash and the archive's path in the CPK, such
as `data.cpk/SOUND/music.brsar`. `--archive` takes that path. A CPK that lists its files by ID only, without a table of
names, isn't read.

An earlier rip may have left a `.wsflib` of the same name with other contents. That rip may have taken other sounds or
used the other mode. The new `.wsflib` then gets the next free name, such as `<name>_2.wsflib`. The earlier `.wsflib`
stays as it was. A new `.miniwsf` replaces an earlier one of the same name. The other earlier `.miniwsf` files still
play. A `.wsflib` that no `.miniwsf` names any more stays in the folder too. To replace earlier rips, rip into an empty
folder or delete the earlier rips first.

`wsfrip` refuses to write a `.wsflib` whose program would be over 1 GiB. The message gives the program's size. In
archive mode and in game mode for JAudio, a rip of fewer sounds, such as with `--bgm` or `--only`, makes a smaller
library. A game-mode library for NW4R SND holds the whole sound archive. For an archive over 1 GiB, `--mode archive`
rips only the data that the sequences play.

`--only` selects the sequences whose labels match a regular expression. `--bgm` selects the music. The music is the
sequences whose labels contain BGM, in any case, and those that play on a music player. A music player has BGM, MUSIC,
SONG or FANFARE among the words of its name separated by underscores, and not SE. Digits may follow the word. Games name
their players more consistently than their music. The players therefore find music whose labels don't say what it is.
Given both options, `wsfrip` rips the music whose labels match.

The ripper lists and skips the sequences that make no sound. Some have a volume of 0 in the archive. The sound library
multiplies everything that a sound plays by that volume. The game can't make them heard. The length analysis (see "Game
mode") finds the others. Those are placeholders, sequences that only set variables for the game, muted copies of other
sounds, and sequences that play only on paths that depend on what the game sets or on random numbers. A rip always takes
one path. On that path they make no sound. In archive mode, `--var` can set the variables that such a sequence waits for
(see "Tags"). `--no-length` leaves out the analysis. Then only the sequences at a volume of 0 are skipped.

### Game mode

The `.wsflib` contains:

- the sections of the game's `main.dol`, with `main` patched to branch to the driver;
- the driver and its parameter block with the game profile and the archive's path;
- the disc's header (`sys/boot.bin`) and the sound archive, as `FILE` chunks;
- a process descriptor.

Each `.miniwsf` selects one sequence. The sequence's length comes from playing it on WSF's model of the sound player
(see "Archive mode") with the game's effects. A looping sequence gets two loops plus a 10-second fade. A sequence that
ends plays until half a second after its last sound. The effects' output counts as sound. The analysis waits for a
second without sound before it ends a sequence with effects. A delay's echo can come back after a quiet gap. Any silence
before the sequence ends is cut. A sequence still playing at 600 seconds gets a length of 600 seconds and the fade.
Sound means a sample louder than 8 of 32,767 (-72 dB).

A note of 9,600 ticks or more (200 beats at the default timebase) is a held note. A sound that plays until the game
stops it gives its notes such lengths. The game then stops the sound first.

A sequence loops when its main track jumps back to a command it has already played, or reaches the end of a loop that
repeats forever. A jump back inside a subroutine doesn't count. The exception is a subroutine that the track can't leave
while it holds a held note or a note of length 0 on a looping wave at its sustain level. A track's pass is what the
track plays from the command it jumps back to until the jump. A pass of an outer loop includes the passes of the loops
inside it. A pass changes the sound when the track plays a note, changes a setting of the track or the player, or opens
a track. A tie that continues a note at the same key and velocity changes nothing. A pass that changes nothing is idle.
A track that only polls or counts a variable makes idle passes. An idle pass is never a loop. Any other pass of the
first track is a loop. A pass of a later track is a loop if the track played a note during it and is the main track. A
pass that played no note is a loop too while the track holds a held note or a note of length 0 on a looping wave at its
sustain level and every other track still playing rests. A track rests when it will never continue, or when its last
pass was idle and it has changed nothing since. A track is the main track when no earlier track still playing has loops
that would count. The first track's loops count unless its last pass was idle. A later track's loops count only if its
last pass played a note and wasn't idle. A track that will never continue has no more loops. It waits forever, waits out
a held note, or waits for its notes to end while one of them never will: a note of length 0 on a looping wave. Two loops
count together only if they come from the same track and go back to the same command. A loop of another track, or one
that goes back elsewhere, starts the count again. Loops that go round before the first sound don't count. A track may
change a value over and over while a note fades in, or before another track starts. A loop that completed without a
sound ends the sequence at its last sound instead. After the main track's second loop, the fade waits for the other
tracks. Each other track still playing must have made two passes that changed the sound. A track whose last pass was
idle, that waits forever or that plays at a volume of 0 doesn't hold the fade back. The fade starts where the last of
them loops. A track that loops at another length than the main track then plays its second pass in full. A track can
also repeat its part in a counted loop that lasts longer than the analysis's limit of 600 seconds, such as 127 passes
of the whole piece. When the analysis reaches the limit with tracks in counted loops, it plays the sequence again with
each such track's outermost counted loop endless. The passes of those loops then count as loops. A track can also play
its part without a loop while another track opens it again in every loop. Each time such a track starts its part again
after an open, it makes a pass that changed the sound. The first open isn't a pass. An open keeps the track's wait. The
pass then comes when that wait runs out. Two opens during one wait make one pass.

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

### Game mode for JAudio2 games

Only game mode plays JAudio2. wsfrip refuses a JAudio2 game that has no driver profile, and it refuses `--mode archive`
and `--archive` for every JAudio2 game. wsfrip takes a disc as a JAudio2 game when the disc has a JAudio2 profile. A
disc that holds a start-up archive and has no profile for NW4R SND counts as a JAudio2 game too. The disc's sound
archives of NW4R SND, such as the Home Button menu's archive, aren't ripped then.

wsfrip finds a JAudio2 game's start-up archive (`.baa`) and the RARC archives beside it by their headers. A `.szs` file
that holds a start-up archive compressed with Yaz0 counts too. A profile can name the sound files in place of the RARC
archives. The profile of New Play Control! Donkey Kong Jungle Beat names the game's sequence archive, chord archive and
chord database. Super Mario Galaxy's profile names the game's sequence archive, chord archive, music-effect archive and
remix archive. Super Mario Galaxy 2's profile names those four and five more. The game's audio wrapper also requests the
Wii Remote speaker's sounds and a table of instrument lists. The driver has the audio system read three tables of music.
The `.wsflib` holds these files as `FILE` chunks. It also holds the AW files of the wave groups that the game's audio
set-up loads, and those of every group that a ripped sound plays. The archive's wave systems name the AW files. Each
sound first plays with only the set-up's groups loaded. The waves that its notes look up give its groups. A `.miniwsf`
loads at most 64 groups. If a sound can't start with its groups loaded, the ripper lists it and skips it. The groups may
not fit in the game's wave memory beside the set-up's groups. The length analysis finds such a sound. `--no-length`
leaves out that check too. The ripper then keeps every sound.

The length comes from playing the sound with its groups while the game's sequence parser is watched. A track that jumps
back to a command it has played, twice to the same command, makes the sound loop. A looping sound gets two loops plus a
10-second fade. A sound that ends plays until half a second after its last sound. One that stays busy without a sound
for 180 seconds ended at its last sound. The limit is 600 seconds. `--bgm` picks the sounds whose labels contain BGM,
or those of a section that the archive's name table calls BGM.

### Game mode for Jac games

An early JAudio1, such as the one in New Play Control! Pikmin, has no sound table. The game calls the sound library
through C functions (Jac, Jaf, Jaq and Jam). Only game mode plays such a game. The game's profile names the bank file
and the sequence archive on the disc and gives the address of the sequence index in `main.dol`. The bank file holds the
instrument banks and the wave systems. Each wave system's number is its place in the file's list. The index is a BARC: a
32-byte header, then a 32-byte entry for each file of the sequence archive. An entry holds the file's name in up to 14
bytes, and the file's offset and size in the archive at 0x18 and 0x1C. Files 0 and 1 are the sound effects' sequence and
the system's events. The other files that aren't empty are pieces of music. A piece's ID is 0x80000000 | the file's
number. The piece's label is the file's name without the extension. A piece plays group 0 of the wave system with the
piece's number.

A profile can name one piece that plays in parts. The profile then lists the parts that the game plays: each part's
number and the wave group that the game loads for the part. That piece's parts are ripped in its place. A part's label
adds `-` and the part's number to the piece's label. A part's ID is the piece's ID | the part's number << 8. The
`.wsflib` holds the bank file, the sequence archive and the AW files of the groups that the sound library loads at
start-up and that a ripped sound plays. Every piece and part is music for `--bgm`. The length analysis watches the loop
over a track's commands and the jump command. A track's parent and children give the sound's tracks. The rules for a
sound's length are those for JAudio2.

### Game mode for New Play Control! Pikmin 2

The game's sound library is a later JAudio1. Its start-up archive (`PSound.aaf`) holds the sound table, the instrument
banks and the wave systems. The profile names the game's sound files: the start-up archive, the sequence archive, the
conductors' and the key's archives, and the lists of music and track maps that the sound layer reads. The `.wsflib`
holds them as `FILE` chunks, with the AW files of the global scene's groups and of every group that a ripped sound
plays. The game's name table (`PSound.asn`) names the sounds.

The game's sound layer starts a piece of music by the sequence's file name. The rips' sounds are the game's 48 pieces of
music and the effects of the sound table. The pieces are the files of the sequence archive other than the effects
sequence. The sound table's sequences aren't ripped: the sound layer gives them their data, and the pieces take their
place. A piece plays group 0 and the group of the scene in which the game plays it. Wave discovery finds the groups of
the 14 pieces that the game never plays. A cave's piece plays with the first conductor of its floors. An effect plays
with the global scene's groups. An effect without a name is labelled `se<category>-<index>`. `--bgm` picks the pieces.

The length analysis is the one for JAudio2, with the rules of JAudio1's sequence parser. A cave's piece never repeats:
its conductor picks the phrases at random. The root track's polls of the game hold back every loop, and the piece plays
to the limit.

### Archive mode

The `.wsflib` holds the sound archive and AX's microcode as `FILE` chunks. It also holds an archive descriptor. Each
`.miniwsf` selects one sequence with a `SND ` chunk. Lengths come from the same analysis as in game mode.

The archive in the `.wsflib` holds only the data that the ripped sequences play: each sequence's file, and its bank's
file and wave data. A game's archive can hold hundreds of megabytes of sound effects and voices that a rip of its music
doesn't play. The archive's header, `SYMB` block and `INFO` block keep their sizes. The `FILE` block holds the kept
files group by group. Each starts at a multiple of 32 bytes. A file that several groups hold is kept once: the first
copy in the file's list of places that lies inside the archive. Banks in a group often share a wave archive. The new
archive then holds one copy of it. The `INFO` block's groups and items give the new places. The other items start at
the archive's end. The ripper reads each kept file back from the new archive and checks that it's the same. The archive
keeps RSAR's format. Players older than 1.4.1 play it too. Rips made with 1.2.0 to 1.4.0 hold the layout of the whole
archive. Zeros take the place of the data that they don't play.

A sound archive doesn't contain AX's microcode. For a game, the ripper takes the microcode from the game's `main.dol`.
For a loose `.brsar`, it takes the microcode from the `main.dol` that `--microcode` names or that's given with the
archive. Otherwise it uses a `main.dol` in the archive's folder. `--microcode` also takes the game's extracted disc or
unpacked title. The ripper then uses the disc's `main.dol` or the title's main program. A `main.dol` compressed with the
SDK's CX library is decompressed first (see "WiiWare titles"). The ripper takes the build of NW4R SND from the same
`main.dol`. The model plays the archive as that build would. A loose `.brsar` therefore requires the `main.dol` of the
game it comes from. With another game's `main.dol`, a rip can sound different. The ripper prints a notice that archive
mode uses WSF's model of the SDK's sound player. On a loose `.brsar`, the notice also says that a game with a driver
profile gives exact rips when the game itself is ripped. On a game with a profile, it says to leave out
`--mode archive`. Sequences whose data isn't all in the archive are listed and skipped. Wave sounds aren't ripped.

`--tracks`, `--tempo` and `--var` rip the sequences in an arrangement (see "Tags"). `wsfrip` writes them as the tags
`wsf_tracks`, `wsf_tempo` and `wsf_var`. `--var` can be given more than once. The length analysis plays the arrangement
too. `--tracks` replaces the tracks that the game plays of a sound (see "Tracks that the game silences"). The tracks
that the game plays go in the archive descriptor rather than a tag. `--suffix` adds a text to each rip's title and file
name. Rips of one sound in several arrangements can then share a folder and its `.wsflib`. In the file name, spaces and
punctuation in the text become underscores. The same happens to a label's spaces and punctuation. The four options work
in archive mode only. On a game with a driver profile, they require `--mode archive`.

Tags written by `wsfrip`: `title` (the sound's label, and `--suffix`'s text), `wsf_sound` (the sound's label), `game`,
`length` and `fade`, `wsf_mode`, `wsfby`, `utf8=1`, and optionally `wsf_tracks`, `wsf_tempo`, `wsf_var`, `artist`,
`year` and `copyright`.

### WiiWare titles

`wsfrip` rips a WiiWare game, or another title for the Wii's NAND, from a folder with the title's TMD and its decrypted
contents. The TMD is the folder's file whose name ends in `.tmd`. A WAD unpacker that decrypts the contents writes such
a folder. A Wii's NAND keeps each installed title's TMD (`title.tmd`) and contents in one:
`title/<the title ID's first 8 hex digits>/<its last 8>/content`. WiiWare games are under `title/00010001`. A WAD holds
the contents encrypted. `wsfrip` rejects it.

Each content's file is named after 8 hex digits, with the extension `.app`. A WAD unpacker names a content after its
index in the TMD. The NAND names it after its content ID. The ripper uses the naming under which more files hold their
contents. A file holds its content when it has the size and the SHA-1 that the TMD gives. Up to 63 bytes of padding may
follow. A file that doesn't hold its content is rejected as encrypted or damaged. The folder may leave out contents. The
NAND keeps a title's shared contents, such as the Home Button menu's files, in another folder.

A title may compress a content with the SDK's CX library: LZ77 (type 0x10) or LZ77 with longer copies (type 0x11). The
ripper decompresses such contents. Every title but the System Menu boots the SDK's NAND Boot Program. That program loads
the title's main program from content 1. A compressed main program is decompressed first. The ripper does the same. AX's
microcode and the build of NW4R SND come from the main program. For a disc, they come from its `main.dol`. A title
whose content 1 isn't a DOL boots its main program itself. The boot content is then the main program. A title has no
file named `main.dol`. A sound archive taken out of a title therefore rips with the title's folder, or the title's main
program, given with `--microcode`.

The ripper reads the title's U8 archives: the contents that are U8 archives, compressed or not. Their files make one
tree, like a disc's files. When two archives hold a file at the same path, each archive's files go in a folder named
after its content instead, such as `content5` for content 5. A sound archive that the title stores compressed is
decompressed: with LZ77, or with Huffman coding of bytes (type 0x28). The Home Button menu's sound archives are
compressed with Huffman coding. Archives inside the archives aren't read.

The game ID is the title ID's last 4 bytes, read as characters. The title is the English name in the banner, content 0.
Without an English name in a banner, the title is the game ID. A title rips in archive mode only. Game mode emulates a
disc. A title's game reads its contents from the NAND instead.

## Copyright

A `.wsflib` contains copyrighted game code and data, including AX's microcode in both modes. Rips are for personal use.
