// SPDX-License-Identifier: MIT

/*
 * WSF guest driver for New Play Control! Pikmin. The game's sound library is an early JAudio1 with a C interface (Jac,
 * Jaf, Jaq and Jam), without a sound layer above it. The Wii's release keeps the GameCube's sound library and adds the
 * Wii's steps. It's compiled for Broadway and stored in the game's .wsflib. The player boots the game's startup code:
 * the OS initialisation and the static constructors. The OS initialisation sets up the disc too. The rip patches the
 * game's main to branch here.
 *
 * The driver calls the game's functions to do what the game's start-up does for sound, without its graphics. The
 * profile in `driver_params` gives their addresses. The ripper fills in the profile. The steps:
 *   1. the memory that the game's start-up gives the sound library: a block of MEM1 for its heap, and its "ARAM", a
 *      block of MEM2. The Wii has no ARAM. The game gives the block's address to the sound library, and the sound
 *      library passes it to its DSP program;
 *   2. the sound library, with that memory and a buffer for the disc. The sound library loads the game's sequence
 *      index, its banks and the effects' first wave group. Then it starts its threads;
 *   3. the wait until the sound library is up;
 *   4. the piece of music: its sequence and its wave groups. The game's scene set-up reads them the same way;
 *   5. the piece, in the first of the two slots that the game plays music in, once the disc has read it;
 *   6. a wait for ever. The sound library's threads play the piece. The driver watches the piece's root track and
 *      reports its end.
 * The music of the game's cutscenes is one piece in parts. The sound library plays that piece from its start-up. The
 * piece's root track polls a port. A cutscene writes the number of a part to that port. The root track then opens a
 * track that plays the part. For a part, the driver loads the part's wave group in step 4. In step 5, the driver
 * writes the part's number to the port. A cutscene does the same. The driver then watches the track that plays the
 * part.
 * WSF's machine raises no video interrupts. The driver waits between its polls with the OS's sleep instead.
 * `driver_params.status` reports the driver's progress to the player (see docs/wsf.md).
 *
 * The block's version is 2, the version of driver/jaudio.c. Its first words after the header have the places that
 * ntdlib's Z2Params gives them: the sound, its handle and its wave groups. A .miniwsf and the length analysis then
 * write and read them at the same addresses for both drivers.
 *
 * Build with driver/build.py (clang). The result is committed as src/rip/pikmin_driver_blob.h. Building WSF itself
 * requires no PowerPC toolchain.
 */

typedef unsigned int u32;

#define PARAMS_MAGIC 0x57534644u /* 'WSFD' */
#define PARAMS_VERSION 2u

/* Values of Params.status. They're macros because an enumerator can't hold 0x80000000 before C23. */
#define STATUS_PLAYING 1u
#define STATUS_FINISHED 2u
#define STATUS_ERROR 0x80000000u /* | step number */

/* The polls that the driver makes for the sound library and the disc before it gives up: 10 s at 60 polls a second. */
#define SETUP_POLLS 600u

/* The most wave groups that a .miniwsf can list. Z2Params has the same limit. */
#define MAX_WAVES 64u

/* The fields of Params.sound_id: 0x80000000 | part << 8 | piece. The piece field holds the piece's number in the
   sequence index. Part 0 plays the piece in the game's slot for music. Any other part goes to the piece that the sound
   library plays from its start-up. */
#define SOUND_PIECE(id) ((id) & 0xffu)
#define SOUND_PART(id) (((id) >> 8) & 0xffu)

/* The DVD task's pass for a piece's files. The game's scene set-up makes it the same way. */
#define PASS_OF_PIECE 0x10000u

/* The port that a cutscene writes its part to, and the word of a track that holds the track's first child. The root
   track opens the track that plays the part as its first child. */
#define PART_PORT 0u
#define TRACK_FIRST_CHILD 0x44u

/* A track's state, a byte: 0 while the track is free. */
#define TRACK_STATE 0x3cu
#define TRACK_FREE 0u

struct Params
{
    u32 magic;
    u32 version;
    u32 status;     /* written by the driver */
    u32 sound_id;   /* the piece or part to play (see SOUND_PIECE); patched by each .miniwsf */
    u32 sound;      /* the address of `piece_root` while the sound plays, and 0 after it (written by the driver) */
    u32 wave_count; /* the wave groups to load; patched by each .miniwsf */
    u32 waves[MAX_WAVES]; /* each the wave system in the high half and the group in the low half */
    /* the game profile: sizes and values that the game's start-up gives */
    u32 frame_ticks;      /* time-base ticks between the driver's polls */
    u32 audio_heap_size;  /* the block of MEM1 for the sound library's heap */
    u32 audio_heap_align; /* its alignment */
    u32 disc_buffer_size; /* the size of the buffer for the disc that the game gives the sound library */
    u32 aram_arena_lo;    /* the start of MEM2's arena that the game's start-up sets before it takes the "ARAM" */
    u32 aram_size;        /* the sound library's "ARAM" in MEM2 */
    u32 aram_align;       /* its alignment */
    u32 sound_path;       /* the sound files' folder on the disc, a string in the game's data */
    u32 music_slot;       /* the slot that the game plays its music in */
    u32 music_handle;     /* that slot's root sequence */
    u32 part_track;       /* the name of the root track that a cutscene writes its part to */
    /* the game profile: the addresses of the game's functions and variables */
    u32 sleep_ticks;        /* void OSSleepTicks(s64 ticks) */
    u32 alloc_mem1;         /* void* OSAllocFromMEM1ArenaLo(u32 size, u32 align) */
    u32 set_mem2_arena_lo;  /* void OSSetMEM2ArenaLo(void*) */
    u32 alloc_mem2;         /* void* OSAllocFromMEM2ArenaLo(u32 size, u32 align) */
    u32 set_aram_base;      /* the sound library's setter of its "ARAM" block's address */
    u32 start;              /* void Jac_Start(void* heap, u32 size, u32 aram, char* path) */
    u32 add_disc_buffer;    /* void Jac_AddDVDBuffer(u8* buffer, u32 size) */
    u32 boot_ok;            /* the sound library's flag: it's up */
    u32 ready_bgm;          /* void Jac_ReadyBgm(u32 piece): the sequence, read from the disc */
    u32 collect_aram;       /* void Collect_AramMotherHeap(void) */
    u32 set_wave_scene;     /* void WaveScene_Set(u32 system, u32 group) */
    u32 check_pass;         /* void DVDT_CheckPass(u32 pass, u32* flag, void (*)(u32)) */
    u32 play_bgm;           /* void Jac_PlayBgm(u32 slot, u32 piece) */
    u32 handle_to_sequence; /* seqp_* Jaf_HandleToSeq(u32 handle) */
    u32 get_track_handle;   /* seqp_* Jam_GetTrackHandle(u32 name) */
    u32 write_port;         /* u32 Jam_WritePortAppDirect(seqp_* track, u32 port, u16 value) */
};

__attribute__((section(".params"), used)) volatile struct Params driver_params = {
    .magic = PARAMS_MAGIC,
    .version = PARAMS_VERSION,
};

typedef u32 (*fn0)(void);
typedef u32 (*fn1)(u32);
typedef u32 (*fn2)(u32, u32);
typedef u32 (*fn3)(u32, u32, u32);
typedef u32 (*fn4)(u32, u32, u32, u32);

static u32 word(u32 address)
{
    return *(volatile u32*)address;
}

static unsigned char byte(u32 address)
{
    return *(volatile unsigned char*)address;
}

/* OSSleepTicks takes an OSTime, a 64-bit count in r3 (high) and r4 (low). */
static void wait_frame(void)
{
    ((fn2)driver_params.sleep_ticks)(0, driver_params.frame_ticks);
}

__attribute__((noreturn)) static void fail(u32 step)
{
    driver_params.status = STATUS_ERROR | step;
    for (;;)
    {
        wait_frame();
    }
}

/* The flag that the disc task's pass sets once the disc has read everything before it, and the sound's track: the
   piece's root track, or the track that plays the part. The length analysis reads that track through `sound`. */
static volatile u32 piece_read;
static volatile u32 piece_root;

/* The wait for ever. The sound ends when its track is free again. */
__attribute__((noreturn)) static void watch_piece(volatile struct Params* p)
{
    for (;;)
    {
        if (p->status == STATUS_PLAYING && byte(piece_root + TRACK_STATE) == TRACK_FREE)
        {
            p->sound = 0;
            p->status = STATUS_FINISHED;
        }
        wait_frame();
    }
}

__attribute__((noreturn)) void driver_main(void)
{
    volatile struct Params* p = &driver_params;
    if (p->magic != PARAMS_MAGIC || p->version != PARAMS_VERSION || p->wave_count > MAX_WAVES)
    {
        fail(1);
    }

    /* The sound library's memory. The game's start-up moves MEM2's arena above the area that the game's emulation of
       ARAM takes for graphics. The start-up takes the sound library's "ARAM" from the arena there. The sound library's
       heap comes from the game's heap in MEM1. The driver takes that heap and the buffer for the disc from MEM1's
       arena. The sound library sends the address of its "ARAM" to the DSP program when the library starts. */
    const u32 heap = ((fn2)p->alloc_mem1)(p->audio_heap_size, p->audio_heap_align);
    const u32 disc_buffer = ((fn2)p->alloc_mem1)(p->disc_buffer_size, p->audio_heap_align);
    ((fn1)p->set_mem2_arena_lo)(p->aram_arena_lo);
    ((fn1)p->set_aram_base)(((fn2)p->alloc_mem2)(p->aram_size, p->aram_align));

    /* The buffer for the disc, then the sound library. The game gives the buffer just after the sound library starts.
       The sound library's disc thread switches to a new buffer at the thread's next task. Until then, the thread reads
       through two buffers of 32 KB in turn. On the console, the first task, the read of the bank file, probably
       outlasts the rest of the game's start-up. In WSF, the disc reads faster. The buffer came during the load of the
       effects' first wave group. That load ended in the first small buffer. The next load used the new size of 512 KB
       with the second small buffer. The read ran over the sound library's sequences. A buffer given before the sound
       library starts takes effect at once. */
    ((fn2)p->add_disc_buffer)(disc_buffer, p->disc_buffer_size);
    ((fn4)p->start)(heap, p->audio_heap_size, p->aram_size, p->sound_path);

    /* The wait until the sound library is up. */
    for (u32 polls = 0; !word(p->boot_ok); polls++)
    {
        if (polls > SETUP_POLLS)
        {
            fail(2);
        }
        wait_frame();
    }

    /* The piece's sequence and the wave groups, then a pass on the disc after them. The pass sets `piece_read` once the
       disc has read them. A part's piece is in memory from the sound library's start-up. */
    const u32 piece = SOUND_PIECE(p->sound_id);
    const u32 part = SOUND_PART(p->sound_id);
    if (!part)
    {
        ((fn1)p->ready_bgm)(piece);
    }
    ((fn0)p->collect_aram)();
    for (u32 i = 0; i < p->wave_count; i++)
    {
        ((fn2)p->set_wave_scene)(p->waves[i] >> 16, p->waves[i] & 0xffffu);
    }
    ((fn3)p->check_pass)(PASS_OF_PIECE | piece, (u32)&piece_read, 0);
    for (u32 polls = 0; !piece_read; polls++)
    {
        if (polls > SETUP_POLLS)
        {
            fail(3);
        }
        wait_frame();
    }

    /* A part: its number, written to the port of the piece's root track. The sound starts there. The root track polls
       the port and opens the track that plays the part. */
    if (part)
    {
        const u32 root = ((fn1)p->get_track_handle)(p->part_track);
        if (!root)
        {
            fail(5);
        }
        ((fn3)p->write_port)(root, PART_PORT, part);
        p->status = STATUS_PLAYING;
        for (u32 polls = 0; !word(root + TRACK_FIRST_CHILD); polls++)
        {
            if (polls > SETUP_POLLS)
            {
                fail(6);
            }
            wait_frame();
        }

        piece_root = word(root + TRACK_FIRST_CHILD);
        p->sound = (u32)&piece_root;
        watch_piece(p);
    }

    /* The piece, in the game's slot for music. */
    ((fn0)p->collect_aram)();
    ((fn2)p->play_bgm)(p->music_slot, piece);
    piece_root = ((fn1)p->handle_to_sequence)(p->music_handle);
    if (byte(piece_root + TRACK_STATE) == TRACK_FREE)
    {
        fail(4);
    }
    p->sound = (u32)&piece_root;
    p->status = STATUS_PLAYING;
    watch_piece(p);
}

__attribute__((naked, section(".text.entry"))) void driver_entry(void)
{
    __asm__ volatile("b driver_main");
}
