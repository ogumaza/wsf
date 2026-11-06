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
 * The sound library plays the effects' sequence and the events' sequence from its start-up too. Tracks of those
 * sequences poll ports, and the game's functions for effects write to them. For an effect, in place of steps 4 and 5:
 *   7. the effects' other wave groups, as the game's set-up of its first scene of play loads them;
 *   8. the wait for the tracks that play the effects;
 *   9. the effect, started by the game's function for its kind. An event's effect plays by an action of an event that
 *      the driver creates at the camera;
 *   10. the wait for the track that plays the effect.
 * The driver sets the game's scene to the play of a stage first. The game plays two of the system's effects only
 * there. Once a frame, the driver then calls the game's update of the camera, as the game's frame does. That update
 * gives each event its volume and pan by its place against the camera. The driver doesn't run the game's check of its
 * events. The event then stays as long as the effect plays, as an event that the game updates each frame does. The
 * driver watches the track that plays the effect.
 * The sound of the formation plays in a track of the effects sequence while the player moves the formation. The
 * game's function for the formation takes the move as x and y, -127 to 127, once a frame. It writes x and the move's
 * length to ports of the track. The driver passes a full move straight ahead each frame: x 0 and y 127.
 * These steps also apply to the GameCube release.
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

/* An effect's ID: 0x80000000 | kind << 24 | number << 8 | sequence. The sequence field holds 0 or 1, the effects'
   sequence or the events'. Pieces start at FIRST_PIECE. The kind gives the game's function that starts the effect. An
   event's number holds the event's type and the action (EVENT_TYPE). */
#define FIRST_PIECE 2u
#define EFFECT_KIND(id) (((id) >> 24) & 0x7fu)
#define EFFECT_NUMBER(id) (((id) >> 8) & 0xffffu)
#define EFFECT_SYSTEM 1u
#define EFFECT_PLAYER 2u
#define EFFECT_EVENT 3u
#define EFFECT_FORMATION 4u
#define EVENT_TYPE(number) ((number) >> 12)
#define EVENT_ACTION(number) ((number) & 0xfffu)

/* The wave system of the effects. The game's set-up of its first scene of play (Jac_SceneSetup, from 0x8001d5a0)
   loads groups 1 to 3 of that system without making them the system's scene. It makes group 0 of systems 14, 15 and 2
   their systems' scenes. An effect's wave groups in the block are those groups, in that order. The set-up also loads
   group 0 of wave system 22, which the bank file lacks. */
#define EFFECTS_SYSTEM 0u

/* The game's scene of a stage's play, and the system's effect that closes the menu. Jac_PlaySystemSe plays that effect
   only while the menu's flag is set. The effect that opens the menu sets it. */
#define SCENE_OF_PLAY 5u
#define EFFECT_CLOSES_MENU 7u

/* The move that the driver passes to the game's function for the formation each frame: straight ahead, at full
   length. */
#define FORMATION_X 0u
#define FORMATION_Y 127u

/* The DVD task's passes for a piece's files and for the effects' wave groups. The game's scene set-up makes them the
   same way. */
#define PASS_OF_PIECE 0x10000u
#define PASS_OF_EFFECTS 0x20000u

/* The port that a cutscene writes its part to, and the word of a track that holds the track's first child. The root
   track opens the track that plays the part as its first child. */
#define PART_PORT 0u
#define TRACK_FIRST_CHILD 0x44u

/* A track's state, a byte: 0 while the track is free. A track has 16 children, in words from TRACK_FIRST_CHILD. */
#define TRACK_STATE 0x3cu
#define TRACK_FREE 0u
#define TRACK_CHILDREN 16u

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
    /* the game profile for effects */
    u32 load_wave_scene;    /* void WaveScene_Load(u32 system, u32 group) */
    u32 scene;              /* the sound library's variable: the game's scene */
    u32 menu;               /* the sound library's flag: the menu is open */
    u32 play_system_effect; /* void Jac_PlaySystemSe(u32 number) */
    u32 play_player_effect; /* void Jac_PlayOrimaSe(u32 number) */
    u32 create_event;       /* s32 Jac_CreateEvent(u8 type, Vector3D* position) */
    u32 play_event_action;  /* u32 Jac_PlayEventAction(s32 event, u32 action) */
    u32 update_camera;      /* void Jac_UpdateCamera(void) */
    u32 system_track;       /* the name of the track that plays the system's effects */
    u32 player_track;       /* the name of the track that plays the player's effects */
    u32 events_track;       /* the name of the events' root track. Its children play the events */
    u32 move_formation;     /* void Jac_Orima_Formation(s32 x, s32 y) */
    u32 formation_track;    /* the name of the effects sequence's track for the formation */
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

/* The end of the sound: once its track is free again. */
static void check_end(volatile struct Params* p)
{
    if (p->status == STATUS_PLAYING && byte(piece_root + TRACK_STATE) == TRACK_FREE)
    {
        p->sound = 0;
        p->status = STATUS_FINISHED;
    }
}

/* The wait for ever. */
__attribute__((noreturn)) static void watch_piece(volatile struct Params* p)
{
    for (;;)
    {
        check_end(p);
        wait_frame();
    }
}

/* An event's place against the camera, as Jac_CreateEvent takes it: three floats. An event at the camera gets the full
   volume and the pan of the middle. */
static const u32 at_camera[3];

/* The first child of the `count` tracks at `parents` that plays, or 0. */
static u32 playing_child(const u32* parents, u32 count)
{
    for (u32 i = 0; i < count; i++)
    {
        for (u32 n = 0; n < TRACK_CHILDREN; n++)
        {
            const u32 child = word(parents[i] + TRACK_FIRST_CHILD + 4 * n);
            if (child && byte(child + TRACK_STATE) != TRACK_FREE)
            {
                return child;
            }
        }
    }

    return 0;
}

/* One frame of the formation's move, as the game's frame passes the player's move. */
static void move_formation(volatile struct Params* p)
{
    ((fn2)p->move_formation)(FORMATION_X, FORMATION_Y);
}

/* Steps 7 to 10: an effect. */
__attribute__((noreturn)) static void play_effect(volatile struct Params* p)
{
    /* The effects' other wave groups, then a pass on the disc after them. The groups of the effects' system load
       without becoming its scene. */
    for (u32 i = 0; i < p->wave_count; i++)
    {
        const u32 system = p->waves[i] >> 16;
        ((fn2)(system == EFFECTS_SYSTEM ? p->load_wave_scene : p->set_wave_scene))(system, p->waves[i] & 0xffffu);
    }
    ((fn3)p->check_pass)(PASS_OF_EFFECTS, (u32)&piece_read, 0);
    for (u32 polls = 0; !piece_read; polls++)
    {
        if (polls > SETUP_POLLS)
        {
            fail(7);
        }
        wait_frame();
    }

    /* The tracks that play the effects. The game's functions for effects drop an effect while their track is missing.
       The events' tracks are the children of the events' root track. */
    u32 system_track = 0, player_track = 0, events_track = 0;
    for (u32 polls = 0; !system_track || !player_track || !events_track; polls++)
    {
        if (polls > SETUP_POLLS)
        {
            fail(8);
        }
        wait_frame();
        system_track = ((fn1)p->get_track_handle)(p->system_track);
        player_track = ((fn1)p->get_track_handle)(p->player_track);
        events_track = ((fn1)p->get_track_handle)(p->events_track);
    }

    /* The effect. The track that plays it becomes a child of the track for its kind. One of the system's effects plays
       one of the player's. */
    *(volatile u32*)p->scene = SCENE_OF_PLAY;
    const u32 number = EFFECT_NUMBER(p->sound_id);
    const u32 kind = EFFECT_KIND(p->sound_id);
    u32 parents[2] = {system_track, player_track};
    u32 count = 2;
    switch (kind)
    {
    case EFFECT_SYSTEM:
        if (number == EFFECT_CLOSES_MENU)
        {
            *(volatile u32*)p->menu = 1;
        }
        ((fn1)p->play_system_effect)(number);
        break;
    case EFFECT_PLAYER:
        ((fn1)p->play_player_effect)(number);
        break;
    case EFFECT_EVENT:
        {
            const u32 event = ((fn2)p->create_event)(EVENT_TYPE(number), (u32)at_camera);
            if (event >= TRACK_CHILDREN)
            {
                fail(9);
            }
            ((fn2)p->play_event_action)(event, EVENT_ACTION(number));
            parents[0] = word(events_track + TRACK_FIRST_CHILD + 4 * event);
            count = 1;
            break;
        }
    case EFFECT_FORMATION:
        parents[0] = ((fn1)p->get_track_handle)(p->formation_track);
        if (!parents[0])
        {
            fail(9);
        }
        move_formation(p);
        count = 1;
        break;
    default:
        fail(9);
    }

    p->status = STATUS_PLAYING;
    for (u32 polls = 0; !(piece_root = playing_child(parents, count)); polls++)
    {
        if (polls > SETUP_POLLS)
        {
            fail(10);
        }
        if (kind == EFFECT_FORMATION)
        {
            move_formation(p);
        }
        ((fn0)p->update_camera)();
        wait_frame();
    }

    p->sound = (u32)&piece_root;
    for (;;)
    {
        if (kind == EFFECT_FORMATION)
        {
            move_formation(p);
        }
        ((fn0)p->update_camera)();
        check_end(p);
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
    if (piece < FIRST_PIECE)
    {
        play_effect(p);
    }
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
