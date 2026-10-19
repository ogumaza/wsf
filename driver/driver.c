// SPDX-License-Identifier: MIT

/*
 * WSF guest driver for games whose sound code is NintendoWare for Revolution's nw4r::snd. It's compiled for Broadway
 * and stored in every game-mode .wsflib of such a game. A JAudio game's .wsflib contains one of the JAudio drivers
 * instead. The player boots the game's startup code: the OS initialisation and the static constructors. The rip patches
 * the game's main to branch here.
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
 *
 * For a game with an effect profile, the driver also sets up the game's effects on AX's aux buses (docs/wsf.md,
 * "Effects"). It makes the game's effect objects before it starts the sound. AX's frame callback then appends them in
 * the first frame after the sound has started. Archive mode does the same. In each frame, the callback lets the sound's
 * variables pick the presets of an effect that has them. The profile can also give tracks that the game silences on the
 * sound (docs/wsf.md, "Tracks that the game silences"). The driver sets their volumes to 0 as the sound starts.
 * `driver_params.status` reports the driver's progress to the player (see docs/wsf.md).
 *
 * Build with driver/build.py (clang). The result is committed as src/rip/driver_blob.h. Building WSF itself requires
 * no PowerPC toolchain.
 */

typedef unsigned int u32;

#define PARAMS_MAGIC 0x57534644u /* 'WSFD' */
#define PARAMS_VERSION 1u

/* Values of Params.status. They're macros because an enumerator can't hold 0x80000000 before C23. */
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
    /* the effects on the aux buses (none while effect_count is 0) */
    u32 ax_register_callback; /* AXOutCallback AXRegisterCallback(AXOutCallback) */
    u32 ax_manager;           /* AxManager& AxManager::GetInstance() */
    u32 append_effect;        /* bool AxManager::AppendEffect(AuxBus, FxBase*) */
    u32 clear_effect;         /* void AxManager::ClearEffect(AuxBus, int frames) */
    u32 seq_handle_ctor;      /* SeqSoundHandle::SeqSoundHandle(SoundHandle*), for presets */
    u32 seq_handle_dtor;      /* SeqSoundHandle::~SeqSoundHandle(), for presets */
    u32 read_variable;        /* bool SeqSound::ReadVariable(int, s16*), for presets */
    u32 sound_sends;          /* the offset of BasicSound's sends to the aux buses (BasicSound::SetFxSend) */
    u32 effect_count;         /* the effects in effects[] */
    u32 follow_variables;     /* whether the sound's variables pick the presets; patched by a .miniwsf */
    u32 send_bus;             /* the bus of the send that the game sets on the sound, or NO_BUS; patched by a
                                 .miniwsf */
    u32 send;                 /* that send, as a float's bits; patched by a .miniwsf */
    struct Effect {
        u32 bus;
        u32 ctor;      /* FxXxx::FxXxx() */
        u32 mem_size;  /* u32 FxXxx::GetRequiredMemSize() */
        u32 assign;    /* bool FxXxx::AssignWorkBuffer(void* buffer, u32 size) */
        u32 set_param; /* bool FxXxx::SetParam(const Param&) */
        u32 words;     /* the words of the class's Param */
        u32 presets;   /* 0 for the one Param that the effect has, or the number of its presets */
        u32 first;     /* the Param's first word in words[] */
    } effects[4];
    u32 words[64]; /* the effects' Params, word by word */
    /* The player reads these to run WSF's models of the effects in place of the game's code. The driver doesn't.
       They give the build of the AXFX library, and each effect's class with its virtual StartUp, Shutdown and
       UpdateBuffer. */
    u32 axfx_build; /* 0 October 2006, 1 December 2006, 2 September 2008 */
    struct Native {
        u32 type;          /* the class, as in the archive descriptor */
        u32 start_up;      /* bool FxXxx::StartUp() */
        u32 shutdown;      /* void FxXxx::Shutdown() */
        u32 update_buffer; /* void FxXxx::UpdateBuffer(int channels, void** buffers, ...) */
    } native[4];
    /* The tracks that the game silences on the sound (none while silenced_tracks is 0). The driver writes a volume of
       0 into each, as SeqSound::SetTrackVolume does. */
    u32 seq_tracks;      /* the offset of the SeqPlayer's 16 track pointers in SeqSound */
    u32 track_volume;    /* the offset of the volume, a float, in SeqTrack */
    u32 silenced_tracks; /* bit N for track N; patched by a .miniwsf */
};

#define NO_GROUP 0xffffffffu
#define NO_BUS 0xffffffffu

__attribute__((section(".params"), used)) volatile struct Params driver_params = {
    .magic = PARAMS_MAGIC,
    .version = PARAMS_VERSION,
    .bank_group_id = NO_GROUP,
    .send_bus = NO_BUS,
};

typedef u32 (*fn0)(void);
typedef u32 (*fn1)(u32);
typedef u32 (*fn2)(u32, u32);
typedef u32 (*fn3)(u32, u32, u32);
typedef u32 (*fn4)(u32, u32, u32, u32);
typedef u32 (*fn6)(u32, u32, u32, u32, u32, u32);

/* A SoundHandle: the sound it holds, or 0 once the sound has finished. */
static volatile u32 handle;

/* The effects: the game's objects, the callback that AX called before the driver's, whether the sound has started (1)
   and its effects have been appended (2), the preset that the sound's variables picked last (0 for none), and whether
   the effect with presets is on its bus. */
static u32 effect_objects[4];
static u32 next_frame_callback;
static volatile u32 effects_state;
static int preset;
static u32 preset_appended;

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

/* MSR[EE] off, returning the MSR before (OSDisableInterrupts), and the MSR back (OSRestoreInterrupts). */
static u32 disable_interrupts(void) {
    u32 msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" : : "r"(msr & ~0x8000u) : "memory");
    return msr;
}

static void restore_interrupts(u32 msr) {
    __asm__ volatile("mtmsr %0" : : "r"(msr) : "memory");
}

static u32 ax_manager(void) {
    return ((fn0)driver_params.ax_manager)();
}

/* AxManager::ClearEffect(bus, 0): shuts the bus's effects down at once. */
static void clear_bus(u32 bus) {
    ((fn3)driver_params.clear_effect)(ax_manager(), bus, 0);
}

static void append(u32 bus, u32 object) {
    ((fn3)driver_params.append_effect)(ax_manager(), bus, object);
}

/* WarioWare: Smooth Moves' choice of the reverb (0x80100e40): a value from 1 to the number of presets that differs
   from the last one sets that preset and appends the effect again. 0 takes the bus's effects away. */
static void pick_preset(int value) {
    volatile struct Params* p = &driver_params;
    for (u32 i = 0; i < p->effect_count; i++) {
        volatile struct Effect* e = &p->effects[i];
        if (!e->presets)
            continue;
        if (value == 0) {
            if (preset != 0) {
                preset = 0;
                clear_bus(e->bus);
                preset_appended = 0;
            }
            return;
        }
        if (value < 0 || (u32)value > e->presets || value == preset)
            return;
        preset = value;
        ((fn2)e->set_param)(effect_objects[i], (u32)&p->words[e->first + (u32)(value - 1) * e->words]);
        if (preset_appended)
            clear_bus(e->bus);
        append(e->bus, effect_objects[i]);
        preset_appended = 1;
        return;
    }
}

/* AX's frame callback, after the game's: the effects' part. Archive mode's is nw4r::Effects::OnAxFrame. */
static void frame_callback(void) {
    volatile struct Params* p = &driver_params;
    if (next_frame_callback)
        ((fn0)next_frame_callback)();
    if (effects_state == 1) {
        effects_state = 2;
        for (u32 i = 0; i < p->effect_count; i++)
            if (!p->effects[i].presets)
                append(p->effects[i].bus, effect_objects[i]);
    }
    if (effects_state == 2 && p->follow_variables) {
        /* 0x800fee28 makes a SeqSoundHandle for the music. If the music is a sequence, it reads the sequence's local
           variables 0 and 1 (-1 until the sound has started), then passes each to 0x80100e40. */
        u32 seq_handle = 0;
        short values[2] = {-1, -1};
        ((fn2)p->seq_handle_ctor)((u32)&seq_handle, (u32)&handle);
        const u32 sound = seq_handle;
        if (sound)
            for (u32 i = 0; i < 2; i++)
                ((fn3)p->read_variable)(sound, i, (u32)&values[i]);
        ((fn2)p->seq_handle_dtor)((u32)&seq_handle, (u32)-1);
        if (sound)
            for (u32 i = 0; i < 2; i++)
                pick_preset(values[i]);
    }
}

/* SeqSound::SetTrackVolume(silenced_tracks, 0) on the sound that `handle` holds. 0 is a float's 0.0. */
static void silence_tracks(void) {
    volatile struct Params* p = &driver_params;
    for (u32 i = 0; i < 16; i++) {
        if (!(p->silenced_tracks & (1u << i)))
            continue;
        const u32 track = *(volatile u32*)(handle + p->seq_tracks + i * 4);
        if (track)
            *(volatile u32*)(track + p->track_volume) = 0;
    }
}

/* Makes the effect objects, each with a work buffer large enough for its Params, and takes over AX's frame callback. */
static void set_up_effects(void) {
    volatile struct Params* p = &driver_params;
    for (u32 i = 0; i < p->effect_count && i < 4; i++) {
        volatile struct Effect* e = &p->effects[i];
        const u32 object = alloc(p->object_size);
        ((fn1)e->ctor)(object);
        u32 size = 0;
        const u32 sets = e->presets ? e->presets : 1;
        for (u32 k = 0; k < sets; k++) {
            ((fn2)e->set_param)(object, (u32)&p->words[e->first + k * e->words]);
            const u32 needed = ((fn1)e->mem_size)(object);
            if (needed > size)
                size = needed;
        }
        if (!(((fn3)e->assign)(object, alloc(size), size) & 0xff))
            fail(9);
        effect_objects[i] = object;
    }
    next_frame_callback = ((fn1)p->ax_register_callback)((u32)frame_callback);
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

    /* nw4r::snd's sound system and threads, then the sound. With effects or silenced tracks, no AX frame comes between
       the start of the sound and the driver's writes to it. The silenced tracks are then silent from the sound's first
       frame. With effects, an update in that window passes the game's send on the sound to its player. AX's next frame
       then appends the effects. */
    if (p->init_flag)
        *(volatile unsigned char*)p->init_flag = 0;
    ((fn2)p->init_sound_system)(p->sound_priority, p->dvd_priority);
    const u32 effects = p->effect_count != 0;
    const u32 at_once = effects || p->silenced_tracks;
    u32 msr = 0;
    if (effects)
        set_up_effects();
    if (at_once)
        msr = disable_interrupts();
    if (((fn6)p->start_sound)(player + p->startable_offset, (u32)&handle, p->sound_id, 0, 0, 0) != 0)
        fail(7);
    if (p->silenced_tracks && handle)
        silence_tracks();
    if (effects) {
        if (p->send_bus < 3 && handle)
            *(volatile u32*)(handle + p->sound_sends + p->send_bus * 4) = p->send;
        ((fn1)p->update)(player);
        effects_state = 1;
    }
    if (at_once)
        restore_interrupts(msr);
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
