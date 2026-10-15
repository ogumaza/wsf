// SPDX-License-Identifier: MIT

/*
 * WSF guest driver for New Play Control! Donkey Kong Jungle Beat. The game's sound code is JAudio2, the sound library
 * of JSystem's framework, under the game's audio system. The driver is compiled for Broadway and stored in each
 * game-mode .wsflib of the game. The player boots the game's startup code: the OS initialisation and the static
 * constructors. The rip patches the game's main to branch here.
 *
 * The driver calls the game's functions to do what the game's start-up does for sound, without its graphics. The
 * profile in `driver_params` gives their addresses. The ripper fills in the profile. The steps:
 *   2. the disc, JSystem's root heap and the game's heaps. The heaps' constructor makes the game heap, the audio heap
 *      in MEM2 and the block of MEM2 that the game keeps as its "ARAM". The game heap becomes the current heap;
 *   3. the ARAM manager, with the calls of the GameCube;
 *   4. the sound files, loaded the way the game's audio system loads them: the start-up archive into the game heap's
 *      tail, decoded from Yaz0, the sequence archive and the chord archive mounted in the audio heap, and the chord
 *      database into the audio heap;
 *   5. the audio system, created the way the game creates it once the files are in. The sound library loads the waves
 *      that the start-up archive marks as staying loaded. The Wii Remote speaker's manager is then taken out. The
 *      manager plays the speaker's sounds through WPAD, and WSF's machine has no Wii Remotes. The speaker's sounds
 *      come from another file than the TV's;
 *   6. the wave groups that the .miniwsf lists, one group of a wave bank each. The game loads its waves per stage;
 *   7. the sound, by its JAudio sound ID. Music starts in the game's BGM player, the way the game starts it. The sound
 *      manager starts other sounds;
 *   8. the audio system's frame process, once a frame, forever.
 * A frame of the game is a sleep of the game's frame time. WSF's machine raises no video interrupts. A step that fails
 * stops the driver with its number in the status word. Step 1 is the check of the block's header.
 * `driver_params.status` reports the driver's progress to the player (see docs/wsf.md).
 *
 * As in jaudio.c, the block's version is 2. The block starts as jaudio.c's does: the sound, its handle and its wave
 * groups, at the offsets of ntdlib's Z2Params. A .miniwsf and the length analysis use the same addresses for both
 * drivers. The profile follows the wave groups.
 *
 * Build with driver/build.py (clang). The result is committed as src/rip/jungle_beat_driver_blob.h. Building WSF
 * itself requires no PowerPC toolchain.
 */

typedef unsigned int u32;
typedef int s32;

#define PARAMS_MAGIC 0x57534644u /* 'WSFD' */
#define PARAMS_VERSION 2u

/* Values of Params.status. They're macros because an enumerator can't hold 0x80000000 before C23. */
#define STATUS_PLAYING 1u
#define STATUS_FINISHED 2u
#define STATUS_ERROR 0x80000000u /* | step number */

/* The frames that the driver waits for the waves before it gives up. */
#define SETUP_FRAMES 600u

/* The most wave groups that a .miniwsf lists. ntdlib's Z2Params has the same limit. */
#define MAX_WAVES 64u

/* A wave archive's state once it is in ARAM. */
#define WAVES_LOADED 2u

/* JKRDvdRipper's and JKRArchive's options: decode Yaz0, allocate from a heap's head or tail, and mount an archive in
   memory. */
#define EXPAND 1u
#define FROM_HEAD 1u
#define FROM_TAIL 2u
#define MOUNT_IN_MEMORY 1u

/* The tracks of a BGM that its states set. */
#define MUSIC_TRACKS 16u

struct Params
{
    u32 magic;
    u32 version;
    u32 status;           /* written by the driver */
    u32 sound_id;         /* the JAudio sound ID to play; patched by each .miniwsf */
    u32 sound;            /* the sound's JAISoundHandle (written by the game): its JAISound while it plays, then 0 */
    u32 wave_count;       /* the wave groups to load; patched by each .miniwsf */
    u32 waves[MAX_WAVES]; /* each the wave bank in the high half and the group in the low half */
    /* the game profile: the sizes that the game's start-up gives */
    u32 frame_ticks;     /* time-base ticks in one frame of the game */
    u32 aram_audio_size; /* ARAM for the sound library */
    u32 aram_graph_size; /* ARAM for graphics */
    u32 aram_priorities; /* the ARAM manager's three thread priorities, a byte each, from bit 16 down */
    u32 heaps_size;      /* the object that holds the game's heaps */
    /* the game profile: the addresses of the game's functions and variables */
    u32 dvd_init;              /* void DVDInit(void) */
    u32 create_root_heap;      /* the game's root heap: JKRExpHeap::createRoot(1, true) */
    u32 alloc;                 /* void* operator new(u32 size) from the current heap */
    u32 heaps_ctor;            /* the constructor of the object that holds the game's heaps */
    u32 heaps;                 /* the variable that holds that object: the game heap at +0, the audio heap at +8 */
    u32 become_current_heap;   /* JKRHeap* JKRHeap::becomeCurrentHeap() */
    u32 current_heap;          /* the variable that holds JSystem's current heap */
    u32 create_aram;           /* JKRAram* JKRAram::create(u32 audio, u32 graph, s32, s32, s32) */
    u32 sleep;                 /* void OSSleepTicks(OSTime) */
    u32 free;                  /* void operator delete(void*) */
    u32 path_to_entry;         /* s32 DVDConvertPathToEntrynum(const char*) */
    u32 load_file;             /* JKRDvdRipper::loadToMainRAM(const char*, u8*, expand, u32, JKRHeap*, direction...) */
    u32 mount;                 /* JKRArchive* JKRArchive::mount(const char*, mode, JKRHeap*, direction) */
    u32 sound_archive_path;    /* the game's paths of its sound files */
    u32 sequence_archive_path;
    u32 chord_archive_path;
    u32 chord_database_path;
    u32 create_audio_system;   /* the game's audio system: (JKRHeap*, void* baa, JKRArchive* sequences,
                                  JKRArchive* chords, void* chord_database) */
    u32 audio_frame;           /* the audio system's frame process */
    u32 speaker_manager;       /* the variable that holds the Wii Remote speaker's manager */
    u32 speaker_offset;        /* the audio system's pointer to that manager */
    u32 section_heap;          /* the variable that holds the sound library's section heap */
    u32 wave_banks_offset;     /* the section heap's table of wave banks */
    u32 load_wave_arcs;        /* bool JAUSection::loadWaveArc(u32 bank, u32 mask): a wave bank's archives in `mask` */
    u32 wave_arc_state_offset; /* a wave archive's state (2 once it is in ARAM), and its disc entry after it */
    /* the game profile: the audio system's BGM player. It starts music */
    u32 music_section;       /* the top byte of a music sound ID */
    u32 music_player_offset; /* the player in the audio system. Its vtable gives start(JAISoundID*) at 8, and its
                                sound's JAISoundHandle is at +4 */
    u32 music_tables;        /* 12 bytes per BGM, by the low half of its ID: the indices of its tables of track
                                states, -1 for none. The second table's index is at +4 */
    u32 music_states;        /* the second table: 8 states of 8 bytes per index */
    u32 set_music_state;     /* void (player, int state, bool at_once): applies a state of the second table */
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
typedef u32 (*fn5)(u32, u32, u32, u32, u32);
typedef u32 (*fn9)(u32, u32, u32, u32, u32, u32, u32, u32, u32);

static u32 word(u32 address)
{
    return *(volatile u32*)address;
}

static void wait_frame(void)
{
    /* OSSleepTicks takes an OSTime, a 64-bit count in r3 (high) and r4 (low). */
    ((fn2)driver_params.sleep)(0, driver_params.frame_ticks);
}

__attribute__((noreturn)) static void fail(u32 step)
{
    driver_params.status = STATUS_ERROR | step;
    for (;;)
    {
        wait_frame();
    }
}

/* A virtual function of `object`. The vtable is the object's first word. */
static u32 virtual_function(u32 object, u32 slot)
{
    return word(word(object) + slot);
}

/* A wave archive of the listed `wave`: (wave bank << 16) | group. A bank's vtable gives its archive at slot 0x10 and
   the number of its archives at slot 0x14. Returns 0 if the bank or the group doesn't exist. */
static u32 wave_arc(u32 wave)
{
    volatile struct Params* p = &driver_params;
    const u32 bank = word(word(p->section_heap) + p->wave_banks_offset + (wave >> 16) * 4);
    if (!bank || (wave & 0xffff) >= ((fn1)virtual_function(bank, 0x14))(bank))
    {
        return 0;
    }
    return ((fn2)virtual_function(bank, 0x10))(bank, wave & 0xffff);
}

/* Whether every listed wave archive is in ARAM. An archive without a file on the disc (entry number below 0) is never
   loaded. */
static int waves_loaded(void)
{
    volatile struct Params* p = &driver_params;
    for (u32 i = 0; i < p->wave_count && i < MAX_WAVES; i++)
    {
        const u32 arc = wave_arc(p->waves[i]);
        if (arc && (s32)word(arc + p->wave_arc_state_offset + 4) >= 0 &&
            word(arc + p->wave_arc_state_offset) != WAVES_LOADED)
        {
            return 0;
        }
    }
    return 1;
}

/* Whether the first state of the music `id` in the BGM player's second table turns every track off. A state holds
   a code for each track in a nibble, high nibble first. The player ignores the code's bit 3. Codes 0 and 2 turn a
   track off, codes 1 and 3 turn it on, and code 7 leaves it as it was. */
static int starts_silent(u32 id)
{
    volatile struct Params* p = &driver_params;
    const s32 index = (s32)word(p->music_tables + (id & 0xffff) * 12 + 4);
    if (index < 0)
    {
        return 0;
    }
    const u32 state = p->music_states + (u32)index * 0x40;
    for (u32 track = 0; track < MUSIC_TRACKS; track++)
    {
        const u32 code = (*(volatile unsigned char*)(state + track / 2) >> ((track & 1) ? 0 : 4)) & 7;
        if (code != 0 && code != 2)
        {
            return 0;
        }
    }
    return 1;
}

/* Whether the disc has a file at the game's `path`. */
static int on_disc(u32 path)
{
    return (s32)((fn1)driver_params.path_to_entry)(path) >= 0;
}

/* Loads the file at the game's `path` into a buffer from `heap`, decoded from Yaz0 if it's compressed. */
static u32 load(u32 path, u32 heap, u32 direction)
{
    return ((fn9)driver_params.load_file)(path, 0, EXPAND, 0, heap, direction, 0, 0, 0);
}

/* Mounts the archive at the game's `path` in memory from `heap`'s head. */
static u32 mount(u32 path, u32 heap)
{
    return ((fn4)driver_params.mount)(path, MOUNT_IN_MEMORY, heap, FROM_HEAD);
}

__attribute__((noreturn)) void driver_main(void)
{
    volatile struct Params* p = &driver_params;
    if (p->magic != PARAMS_MAGIC || p->version != PARAMS_VERSION)
    {
        p->status = STATUS_ERROR | 1;
        for (;;)
        {
        }
    }

    /* The disc, JSystem's root heap, then the game's heaps. The game heap becomes the current heap. */
    ((fn0)p->dvd_init)();
    ((fn0)p->create_root_heap)();
    const u32 object = ((fn1)p->alloc)(p->heaps_size);
    if (!object)
    {
        fail(2);
    }
    const u32 heaps = ((fn1)p->heaps_ctor)(object);
    *(volatile u32*)p->heaps = heaps;
    const u32 game_heap = word(heaps);
    const u32 audio_heap = word(heaps + 8);
    if (!game_heap || !audio_heap)
    {
        fail(2);
    }
    ((fn1)p->become_current_heap)(game_heap);

    /* The ARAM manager. */
    ((fn5)p->create_aram)(p->aram_audio_size, p->aram_graph_size, (p->aram_priorities >> 16) & 0xff,
                          (p->aram_priorities >> 8) & 0xff, p->aram_priorities & 0xff);

    /* The sound files, in the game's order. */
    if (!on_disc(p->sound_archive_path) || !on_disc(p->sequence_archive_path) || !on_disc(p->chord_archive_path) ||
        !on_disc(p->chord_database_path))
    {
        fail(4);
    }
    const u32 baa = load(p->sound_archive_path, game_heap, FROM_TAIL);
    const u32 sequences = mount(p->sequence_archive_path, audio_heap);
    const u32 chords = mount(p->chord_archive_path, audio_heap);
    const u32 chord_database = load(p->chord_database_path, audio_heap, FROM_HEAD);
    if (!baa || !sequences || !chords || !chord_database)
    {
        fail(4);
    }

    /* The audio system, from the audio heap. The start-up archive's copy is freed once the sound library has read
       it. */
    const u32 last_heap = word(p->current_heap);
    ((fn1)p->become_current_heap)(audio_heap);
    const u32 system = ((fn5)p->create_audio_system)(audio_heap, baa, sequences, chords, chord_database);
    ((fn1)p->free)(baa);
    ((fn1)p->become_current_heap)(last_heap);
    if (!system)
    {
        fail(5);
    }

    /* Without the speaker's manager, the audio system and the game's speaker functions skip the speaker. They do the
       same before the audio system creates the manager. */
    *(volatile u32*)p->speaker_manager = 0;
    *(volatile u32*)(system + p->speaker_offset) = 0;

    /* The listed waves. The section loads the archives of a bank that a mask's bits pick. A group above 31 therefore
       can't load. Every wave bank of the game has one group. The frame process runs while the archives load. */
    const u32 section = word(p->section_heap);
    for (u32 i = 0; i < p->wave_count && i < MAX_WAVES; i++)
    {
        if ((p->waves[i] & 0xffff) < 32)
        {
            ((fn3)p->load_wave_arcs)(section, p->waves[i] >> 16, 1u << (p->waves[i] & 0xffff));
        }
    }
    for (u32 frames = 0; !waves_loaded(); frames++)
    {
        if (frames > SETUP_FRAMES)
        {
            fail(6);
        }
        ((fn1)p->audio_frame)(system);
        wait_frame();
    }

    /* The sound. The BGM player starts music and sets the tracks to the music's first states. Some tracks start off.
       The game turns them on later, or never. A BGM whose first state turns every track off gets its next state at
       once. The game turns that BGM's tracks on later. The sound manager starts other sounds. Its vtable gives
       startSound(JAISoundID*, JAISoundHandle*, const TVec3*) at 0xc. The player holds its sound's handle. The driver
       copies the handle to `sound` once a frame. */
    const u32 id = p->sound_id;
    const u32 player = (id >> 24) == p->music_section ? system + p->music_player_offset : 0;
    if (player)
    {
        ((fn2)virtual_function(player, 8))(player, (u32)&id);
        p->sound = word(player + 4);
        if (!p->sound)
        {
            fail(7);
        }
        if (starts_silent(id))
        {
            ((fn3)p->set_music_state)(player, 1, 1);
        }
    }
    else if (!(((fn4)virtual_function(system, 0xc))(system, (u32)&id, (u32)&p->sound, 0) & 0xff))
    {
        fail(7);
    }
    p->status = STATUS_PLAYING;

    for (;;)
    {
        ((fn1)p->audio_frame)(system);
        if (player)
        {
            p->sound = word(player + 4);
        }
        if (p->status == STATUS_PLAYING && p->sound == 0)
        {
            p->status = STATUS_FINISHED;
        }
        wait_frame();
    }
}

__attribute__((naked, section(".text.entry"))) void driver_entry(void)
{
    __asm__ volatile("b driver_main");
}
