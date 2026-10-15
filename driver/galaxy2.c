// SPDX-License-Identifier: MIT

/*
 * WSF guest driver for Super Mario Galaxy 2. The game's sound code is JAudio2 under the game's audio system. Super
 * Mario Galaxy's is the same (see galaxy.c). The driver is compiled for Broadway and stored in each game-mode .wsflib
 * of the game. The player boots the game's startup code: the OS initialisation and the static constructors. The
 * game's main sets up the CPU's quantization registers for its fast float conversions. The rip then patches main's
 * next instruction to branch here.
 *
 * Galaxy 2 loads its sound files through its file loader, a thread that reads files and mounts archives on request.
 * The audio system reads its data tables through the same loader. The driver therefore starts the loader and loads
 * the sound files the way the game does: through the game's audio wrapper (AudSystemWrapper). The file functions add
 * a language's folder to each path to look for a translated file. They take the language from the game's object
 * holder. The game's system object holds that holder. The driver makes stand-ins for the two objects. The stand-ins
 * hold the language and the audio wrapper and nothing else.
 *
 * The driver calls the game's functions to do what the game's start-up does for sound, without its graphics. The
 * profile in `driver_params` gives their addresses. The ripper fills in the profile. The steps:
 *   2. the disc, then the game's root heaps and the object that makes the game's heaps. The root heaps split MEM2:
 *      its low part becomes the game's "ARAM", and the rest a heap. The object makes the system heap, the audio
 *      system's heap and the stationed heaps in MEM1 and MEM2. As in the game, the stationed heap in MEM1 becomes the
 *      current heap. The file ripper gets its buffer from that heap;
 *   3. the ARAM manager, with the calls of the GameCube;
 *   4. the file loader, then the stand-ins for the game's system object and its object holder;
 *   5. the audio system. The audio wrapper requests the sound files from the file loader and creates the audio system
 *      once they're in. The Wii Remote speaker's system is then taken out of the audio system. That system plays the
 *      speaker's sounds through WPAD, and WSF's machine has no Wii Remotes. The video interface's screen-saver
 *      threshold gets the value that VIInit gives it. The audio system then gets the volumes that the game sets as
 *      each scene starts;
 *   6. the wave bank that the audio system loads itself, then the audio system's data tables: the music's
 *      parameters, the multi BGMs and the stages' music. The game reads the tables as it loads its stationed waves.
 *      Those are the waves of the sounds that the particle effects of the stationed stages play. The driver sets up no
 *      particle effects and loads no stationed waves. Then the wave groups that the .miniwsf lists, one group of a
 *      wave bank each. The game loads them per stage and per scenario;
 *   7. the sound, by its JAudio sound ID. Music starts in the BGM manager's stage slot, the way the game starts a
 *      stage's music. Other sounds start in the audio system;
 *   8. the audio system's frame process, once a frame, forever.
 * A frame of the game is a sleep of the game's frame time. WSF's machine raises no video interrupts. A step that fails
 * stops the driver with its number in the status word. Step 1 is the check of the block's header.
 * `driver_params.status` reports the driver's progress to the player (see docs/wsf.md).
 *
 * As in jaudio.c, the block's version is 2. The block starts as jaudio.c's does: the sound, its handle and its wave
 * groups, at the offsets of ntdlib's Z2Params. A .miniwsf and the length analysis use the same addresses for every
 * driver. The profile follows the wave groups.
 *
 * Build with driver/build.py (clang). The result is committed as src/rip/galaxy2_driver_blob.h. Building WSF itself
 * requires no PowerPC toolchain.
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

/* The most data tables that the driver has the audio system read. */
#define MAX_TABLES 3u

/* VIInit's screen-saver threshold for a 60 Hz mode: 18,000 retraces, 5 minutes. */
#define DIMMING_RETRACES 18000u

/* AudWrap::startStageBgm's last argument as the game passes it. */
#define BGM_DEFAULT (-1)

struct Params
{
    u32 magic;
    u32 version;
    u32 status;           /* written by the driver */
    u32 sound_id;         /* the JAudio sound ID to play; patched by each .miniwsf */
    u32 sound;            /* the sound's JAISoundHandle (written by the game): its JAISound while it plays, then 0 */
    u32 wave_count;       /* the wave groups to load; patched by each .miniwsf */
    u32 waves[MAX_WAVES]; /* each the wave bank in the high half and the group in the low half */
    /* the game profile: the sizes and values that the game's start-up gives */
    u32 frame_ticks;        /* time-base ticks in one frame of the game */
    u32 aram_audio_size;    /* ARAM for the sound library */
    u32 aram_graph_size;    /* ARAM for graphics */
    u32 aram_priorities;    /* the ARAM manager's three thread priorities, a byte each, from bit 16 down */
    u32 heaps_size;         /* the object that makes the game's heaps */
    u32 ripper_buffer_size; /* the file ripper's buffer */
    u32 game_system_size;   /* the game's system object */
    u32 holder_size;        /* its object holder */
    u32 wrapper_size;       /* the audio wrapper */
    u32 language;           /* the game's number for the disc's language */
    /* the game profile: the addresses of the game's functions and variables, and offsets in its objects */
    u32 dvd_init;               /* void DVDInit(void) */
    u32 create_root_heaps;      /* the game's root heaps: JSystem's, and the heap in MEM2 above the game's "ARAM" */
    u32 alloc;                  /* void* operator new(u32 size) from the current heap */
    u32 heaps_ctor;             /* the constructor of the object that makes the game's heaps */
    u32 heaps;                  /* the variable that holds that object */
    u32 stationed_heap_offset;  /* the object's stationed heap in MEM1 */
    u32 stationed_heap2_offset; /* its stationed heap in MEM2 */
    u32 audio_heap_offset;      /* its heap for the audio system */
    u32 become_current_heap;    /* JKRHeap* JKRHeap::becomeCurrentHeap() */
    u32 setup_ripper;           /* void FileRipper::setup(u32 buffer size, JKRHeap*) */
    u32 create_aram;            /* JKRAram* JKRAram::create(u32 audio, u32 graph, s32, s32, s32) */
    u32 init_dvd;               /* the object holder's initDvd: JSystem's file loaders and the game's file loader */
    u32 game_system;            /* the variable that holds the game's system object */
    u32 holder_offset;          /* the system object's object holder */
    u32 language_offset;        /* the holder's language */
    u32 wrapper_offset;         /* the holder's audio wrapper */
    u32 wrapper_ctor;           /* AudSystemWrapper(JKRSolidHeap* audio, JKRHeap* MEM1, JKRHeap* MEM2) */
    u32 request_resources;      /* void (wrapper): requests the sound files */
    u32 create_audio_system;    /* void (wrapper): receives them and creates the audio system */
    u32 sleep;                  /* void OSSleepTicks(OSTime) */
    u32 audio_frame;            /* the audio system's frame process */
    u32 speaker_offset;         /* the audio system's pointer to the Wii Remote speaker's system */
    u32 system_waves_loaded;    /* bool (wrapper): whether the bank that the audio system loads is in ARAM */
    u32 system_waves_done;      /* void (wrapper): what the game does once that bank is in */
    u32 tables[MAX_TABLES];     /* void (void): each reads one of the audio system's data tables; 0 for none */
    u32 section_heap;           /* the variable that holds the sound library's section heap */
    u32 load_wave_arcs;         /* bool JAUSection::loadWaveArc(u32 bank, u32 mask): a wave bank's archives in `mask` */
    u32 wave_loaded;            /* bool JAUSectionHeap::isWaveLoaded(u32 bank, u32 group) */
    u32 music_section;          /* the top byte of a music sound ID */
    u32 start_music;            /* JAISoundHandle* (u32 id, bool, s32): music in the BGM manager's stage slot */
    u32 start_sound;            /* bool (audio system, JAISoundID*, JAISoundHandle*, const TVec3*) */
    u32 dimming_threshold;      /* the video interface's idle time before the screen dims, in retraces */
    u32 scene_volume;           /* void (audio system): the volumes that the game sets as each scene starts */
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

/* Allocates `size` bytes from the current heap and clears them. Fails with `step` when the heap is full. */
static u32 alloc_zeroed(u32 size, u32 step)
{
    const u32 block = ((fn1)driver_params.alloc)(size);
    if (!block)
    {
        fail(step);
    }
    for (u32 i = 0; i < size; i += 4)
    {
        *(volatile u32*)(block + i) = 0;
    }
    return block;
}

/* Whether every listed wave group is in ARAM. A group without a file on the disc never loads. The game has no samples
   for its waves. */
static int waves_loaded(void)
{
    volatile struct Params* p = &driver_params;
    const u32 section = word(p->section_heap);
    for (u32 i = 0; i < p->wave_count && i < MAX_WAVES; i++)
    {
        if (!(((fn3)p->wave_loaded)(section, p->waves[i] >> 16, p->waves[i] & 0xffff) & 0xff))
        {
            return 0;
        }
    }
    return 1;
}

/* Runs the audio system's frame process until `loaded` returns true for the audio wrapper. Fails with `step` after
   SETUP_FRAMES frames. */
static void wait_for_waves(u32 wrapper, u32 system, u32 loaded, u32 step)
{
    for (u32 frames = 0; !(((fn1)loaded)(wrapper) & 0xff); frames++)
    {
        if (frames > SETUP_FRAMES)
        {
            fail(step);
        }
        ((fn1)driver_params.audio_frame)(system);
        wait_frame();
    }
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

    /* The disc, the root heaps, then the game's heaps. The stationed heap in MEM1 becomes the current heap. The file
       ripper takes its buffer from it. */
    ((fn0)p->dvd_init)();
    ((fn0)p->create_root_heaps)();
    const u32 object = ((fn1)p->alloc)(p->heaps_size);
    if (!object)
    {
        fail(2);
    }
    const u32 heaps = ((fn1)p->heaps_ctor)(object);
    *(volatile u32*)p->heaps = heaps;
    const u32 stationed_heap = word(heaps + p->stationed_heap_offset);
    const u32 stationed_heap2 = word(heaps + p->stationed_heap2_offset);
    const u32 audio_heap = word(heaps + p->audio_heap_offset);
    if (!stationed_heap || !stationed_heap2 || !audio_heap)
    {
        fail(2);
    }
    ((fn1)p->become_current_heap)(stationed_heap);
    ((fn2)p->setup_ripper)(p->ripper_buffer_size, stationed_heap);

    /* The ARAM manager. */
    ((fn5)p->create_aram)(p->aram_audio_size, p->aram_graph_size, (p->aram_priorities >> 16) & 0xff,
                          (p->aram_priorities >> 8) & 0xff, p->aram_priorities & 0xff);

    /* The file loader, and the stand-ins for the system object and its object holder. */
    ((fn1)p->init_dvd)(0);
    const u32 holder = alloc_zeroed(p->holder_size, 4);
    const u32 game_system = alloc_zeroed(p->game_system_size, 4);
    *(volatile u32*)(holder + p->language_offset) = p->language;
    *(volatile u32*)(game_system + p->holder_offset) = holder;
    *(volatile u32*)p->game_system = game_system;

    /* The audio system, created by the audio wrapper once the file loader has the sound files. Without the speaker's
       system, the audio system's frame process skips the speaker. */
    const u32 wrapper = alloc_zeroed(p->wrapper_size, 5);
    ((fn4)p->wrapper_ctor)(wrapper, audio_heap, stationed_heap, stationed_heap2);
    *(volatile u32*)(holder + p->wrapper_offset) = wrapper;
    ((fn1)p->request_resources)(wrapper);
    ((fn1)p->create_audio_system)(wrapper);
    const u32 system = word(wrapper);
    if (!system)
    {
        fail(5);
    }
    *(volatile u32*)(system + p->speaker_offset) = 0;

    /* The screen saver and the volumes (see galaxy.c). While the video interface reports a dimmed screen, the audio
       system turns the music and most sound effects down. Without VIInit, the interface's threshold is 0, and the
       screen counts as dimmed from the first frame. The driver gives the threshold VIInit's value. No video interrupt
       counts the idle time up to it. The game sets the sound effects' volumes as each scene starts, and the driver
       does the same. */
    *(volatile u32*)p->dimming_threshold = DIMMING_RETRACES;
    ((fn1)p->scene_volume)(system);

    /* The wave bank that the audio system loads itself, the data tables, then the listed waves. The section loads the
       groups of a bank that a mask's bits pick. A group above 31 therefore can't load. The frame process runs while
       they load. */
    wait_for_waves(wrapper, system, p->system_waves_loaded, 6);
    ((fn1)p->system_waves_done)(wrapper);
    for (u32 i = 0; i < MAX_TABLES; i++)
    {
        if (p->tables[i])
        {
            ((fn0)p->tables[i])();
        }
    }
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

    /* The sound. The BGM manager starts music in its stage slot and returns the slot's JAISoundHandle. The driver
       copies the handle to `sound` once a frame. The audio system starts other sounds with `sound` as their handle. */
    u32 id = p->sound_id;
    u32 music = 0;
    if ((id >> 24) == p->music_section)
    {
        music = ((fn3)p->start_music)(id, 0, (u32)BGM_DEFAULT);
        p->sound = music ? word(music) : 0;
        if (!p->sound)
        {
            fail(7);
        }
    }
    else if (!(((fn4)p->start_sound)(system, (u32)&id, (u32)&p->sound, 0) & 0xff))
    {
        fail(7);
    }
    p->status = STATUS_PLAYING;

    for (;;)
    {
        ((fn1)p->audio_frame)(system);
        if (music)
        {
            p->sound = word(music);
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
