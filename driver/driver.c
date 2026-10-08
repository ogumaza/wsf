// SPDX-License-Identifier: MIT

/*
 * WSF guest driver for games whose sound code is NintendoWare for Revolution's nw4r::snd. It's compiled for Broadway
 * and stored in every game-mode .wsflib. The player boots the game's startup code: the OS initialisation and the
 * static constructors. The rip patches the game's main to branch here.
 *
 * The driver then does what the game's boot would do for sound, using the game's functions. The profile in
 * `driver_params` gives their addresses. The ripper fills in the profile. The steps:
 *   1. the audio interface and AX (AIInit, AXInit);
 *   2. the sound archive, from the disc (nw4r::snd::DvdSoundArchive) with its INFO chunk;
 *   3. a SoundArchivePlayer on it;
 *   4. a sound heap, with the group that holds the sound's data loaded into it, and the group with its bank when
 *      that's another one;
 *   5. nw4r::snd's sound system and its threads, and the sound started;
 *   6. SoundArchivePlayer::Update, about 60 times per emulated second, forever.
 * The sound system starts last. The sound then starts the same number of sound frames after the system, with the
 * same random numbers. The loading time doesn't matter.
 * `driver_params.status` reports the driver's progress to the player (see docs/wsf.md).
 *
 * Build with driver/build.py (clang). The result is committed as src/rip/driver_blob.h. Building WSF itself requires
 * no PowerPC toolchain.
 */

typedef unsigned int u32;
typedef int s32;

#define PARAMS_MAGIC 0x57534644u /* 'WSFD' */
#define PARAMS_VERSION 1u

/* Values of Params.status. They're macros because an enumerator can't hold 0x80000000 before C23. */
#define STATUS_BOOTING 0u
#define STATUS_PLAYING 1u
#define STATUS_FINISHED 2u
#define STATUS_ERROR 0x80000000u /* | step number */

struct Params {
    u32 magic;
    u32 version;
    u32 status;          /* written by the driver */
    u32 sound_id;        /* the sound to play; patched by each .miniwsf */
    u32 group_id;        /* the group that holds its data; patched by each .miniwsf */
    u32 heap_size;       /* the sound heap for the group; patched by each .miniwsf */
    u32 frame_ticks;     /* time-base ticks between SoundArchivePlayer::Update calls */
    u32 info_size;       /* the sound archive's INFO chunk size */
    u32 sound_priority;  /* the priority of nw4r::snd's sound thread */
    u32 dvd_priority;    /* the priority of its disc thread */
    char archive_path[64];
    /* game profile: the addresses of the game's functions */
    u32 ai_init;            /* void AIInit(void* stack) */
    u32 ax_init;            /* void AXInit(void) */
    u32 alloc;              /* void* OSAllocFromMEM1ArenaLo(u32 size, u32 align) */
    u32 sleep_ticks;        /* void OSSleepTicks(s64 ticks) */
    u32 init_sound_system;  /* SoundSystem::InitSoundSystem(s32 sound_priority, s32 dvd_priority) */
    u32 archive_ctor;       /* DvdSoundArchive::DvdSoundArchive() */
    u32 archive_open;       /* bool DvdSoundArchive::Open(const char* path) */
    u32 archive_load_info;  /* bool DvdSoundArchive::LoadHeader(void* buffer, u32 size) */
    u32 player_ctor;        /* SoundArchivePlayer::SoundArchivePlayer() */
    u32 player_mem_size;    /* u32 SoundArchivePlayer::GetRequiredMemSize(const SoundArchive*) */
    u32 player_strm_size;   /* u32 SoundArchivePlayer::GetRequiredStrmBufferSize(const SoundArchive*) */
    u32 player_setup;       /* bool SoundArchivePlayer::Setup(archive, mem, mem size, strm, strm size) */
    u32 heap_ctor;          /* SoundHeap::SoundHeap() */
    u32 heap_create;        /* bool SoundHeap::Create(void* buffer, u32 size) */
    u32 load_group;         /* bool SoundArchivePlayer::LoadGroup(u32 id, SoundMemoryAllocatable*, u32 block) */
    u32 start_sound;        /* SoundStartable::detail_StartSound(handle, id, args, player, info) */
    u32 update;             /* void SoundArchivePlayer::Update() */
    u32 startable_offset;   /* SoundArchivePlayer's SoundStartable base, from its start */
    u32 object_size;        /* room for each of the archive, the player and the heap */
    u32 bank_group_id;      /* the group with the sound's bank when group_id lacks it, or NO_GROUP; patched by a
                               .miniwsf that requires it */
    u32 init_flag;          /* the sound system's flag byte (SoundArchivePlayer::Setup checks it first in 2008
                               builds), or 0 */
};

#define NO_GROUP 0xffffffffu

__attribute__((section(".params"), used)) volatile struct Params driver_params = {
    .magic = PARAMS_MAGIC,
    .version = PARAMS_VERSION,
    .bank_group_id = NO_GROUP,
};

typedef u32 (*fn0)(void);
typedef u32 (*fn1)(u32);
typedef u32 (*fn2)(u32, u32);
typedef u32 (*fn3)(u32, u32, u32);
typedef u32 (*fn4)(u32, u32, u32, u32);
typedef u32 (*fn6)(u32, u32, u32, u32, u32, u32);

/* A SoundHandle: the sound it holds, or 0 once the sound has finished. */
static volatile u32 handle;

static void sleep_ticks(u32 ticks) {
    /* OSSleepTicks takes an OSTime, a 64-bit count in r3 (high) and r4 (low). */
    ((fn2)driver_params.sleep_ticks)(0, ticks);
}

__attribute__((noreturn)) static void fail(u32 step) {
    driver_params.status = STATUS_ERROR | step;
    for (;;)
        sleep_ticks(driver_params.frame_ticks);
}

static u32 alloc(u32 size) {
    const u32 p = ((fn2)driver_params.alloc)(size, 32);
    if (!p)
        fail(99);
    for (u32* q = (u32*)p; q < (u32*)(p + ((size + 3) & ~3u)); q++)
        *q = 0;
    return p;
}

__attribute__((noreturn)) void driver_main(void) {
    volatile struct Params* p = &driver_params;
    if (p->magic != PARAMS_MAGIC || p->version != PARAMS_VERSION)
        fail(1);

    /* The audio interface and AX. */
    ((fn1)p->ai_init)(0);
    ((fn0)p->ax_init)();

    /* The sound archive, read from the disc. */
    const u32 archive = alloc(p->object_size);
    ((fn1)p->archive_ctor)(archive);
    if (!(((fn2)p->archive_open)(archive, (u32)p->archive_path) & 0xff))
        fail(2);
    const u32 info = alloc(p->info_size);
    if (!(((fn3)p->archive_load_info)(archive, info, p->info_size) & 0xff))
        fail(3);

    /* The player. In a 2008 build, SoundArchivePlayer::Setup doesn't run before the sound system has started. Setup
       doesn't use the sound system. The driver sets the flag until the system really starts, last. */
    if (p->init_flag)
        *(volatile unsigned char*)p->init_flag = 1;
    const u32 player = alloc(p->object_size);
    ((fn1)p->player_ctor)(player);
    const u32 mem_size = ((fn2)p->player_mem_size)(player, archive);
    const u32 strm_size = ((fn2)p->player_strm_size)(player, archive);
    const u32 mem = alloc(mem_size);
    const u32 strm = strm_size ? alloc(strm_size) : 0;
    if (!(((fn6)p->player_setup)(player, archive, mem, mem_size, strm, strm_size) & 0xff))
        fail(4);

    /* A sound heap with the sound's group in it. */
    const u32 heap = alloc(p->object_size);
    ((fn1)p->heap_ctor)(heap);
    if (!(((fn3)p->heap_create)(heap, alloc(p->heap_size), p->heap_size) & 0xff))
        fail(5);
    if (!(((fn4)p->load_group)(player, p->group_id, heap, 0) & 0xff))
        fail(6);
    if (p->bank_group_id != NO_GROUP && !(((fn4)p->load_group)(player, p->bank_group_id, heap, 0) & 0xff))
        fail(8);

    /* nw4r::snd's sound system and threads, then the sound. */
    if (p->init_flag)
        *(volatile unsigned char*)p->init_flag = 0;
    ((fn2)p->init_sound_system)(p->sound_priority, p->dvd_priority);
    if (((fn6)p->start_sound)(player + p->startable_offset, (u32)&handle, p->sound_id, 0, 0, 0) != 0)
        fail(7);
    p->status = STATUS_PLAYING;

    for (;;) {
        ((fn1)p->update)(player);
        if (p->status == STATUS_PLAYING && handle == 0)
            p->status = STATUS_FINISHED;
        sleep_ticks(p->frame_ticks);
    }
}

__attribute__((naked, section(".text.entry"))) void driver_entry(void) {
    __asm__ volatile("b driver_main");
}
