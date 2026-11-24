// SPDX-License-Identifier: MIT

/*
 * WSF guest driver for New Play Control! Pikmin 2, the Wii's release of Pikmin 2. The game's sound layer (PSSystem,
 * PSGame and PSM) runs on JAudio1, the first generation of the sound library of Nintendo EAD's JSystem. The driver is
 * compiled for Broadway and stored in each game-mode .wsflib of the game. The player boots the game's startup code: the
 * OS initialisation and the static constructors. The rip patches the game's main to branch here.
 *
 * The game plays its music through objects of its sound layer. A sound scene holds the objects. A piece's object
 * handles the calls that its sequence makes to the game: the root track's tempo and swing, and the jumps of the music
 * that changes with the action. A cave's music has a conductor. It picks the tracks and phrases that play. The driver
 * makes the piece's object with the game's functions, in a scene of its own, and starts it. The object's directors get
 * no game objects to follow. The music then plays as it does with nothing going on: the layers for work, treasure,
 * nearby enemies, battle and the ground stay muted, and the layers for the number of Pikmin play in full.
 *
 * An effect plays in the effects sequence. The global scene starts that sequence. It loads group 0 of wave system 0
 * and the Home Button menu's wave group. Group 0 holds the waves of most effects. The driver starts an effect as the
 * game starts its system effects, without a position. Some effects ask the game about its state: the story's flags and
 * the captains' marching horns (see play_data and rappas).
 *
 * The same steps apply to the GameCube's Pikmin 2, with that release's functions. The Wii's release keeps its files in
 * a folder of the disc, makes a root heap in each of the Wii's two memories, and loads the Home Button menu's waves
 * with the global scene. A frame of the game is a sleep of the game's frame time. WSF's machine raises no
 * video interrupts.
 *
 * The profile in `driver_params` gives the game's addresses and sizes, and a table of the pieces. The ripper fills it
 * in. The steps:
 *   2. the disc and the game's folder on it. Then JFWSystem::init's heaps: MEM1's root heap, MEM2's root heap (the
 *      game's root heap) and the system heap. Then the ARAM manager, the main thread's JKRThread, and the heap that the
 *      game's system object makes for the sound system;
 *   3. the sound system that System::createSoundSystem makes: the start-up archive from the disc, the factory of the
 *      game's sound layer, and the global scene with its effects sequence;
 *   4. the global scene's resources that System::loadSoundResource loads: its waves, the Home Button menu's waves and
 *      its effects sequence;
 *   5. a piece's object (see Piece), in a scene with no game objects. An effect gets no scene;
 *   6. the scene's resources: the piece's sequence, its conductor, and the wave groups that the .miniwsf lists;
 *   7. the sound: the scene starts the piece's first sequence. A boss's music starts as the boss starts it, at a part
 *      of the music. The sound system starts an effect. Before step 2, the driver draws the sound library's random
 *      numbers that an effect's value names. The effect's first draw then comes later in the generator's run;
 *   8. the sound layer's frame work, once a frame, forever.
 * A step that fails stops the driver with its number in the status word. Step 1 is the check of the block.
 * `driver_params.status` reports the driver's progress to the player (see docs/wsf.md).
 *
 * As in jaudio.c, the block's version is 2. The block starts as jaudio.c's does: the sound, its handle and its wave
 * groups, at the offsets of ntdlib's Z2Params. A .miniwsf and the length analysis use the same addresses for every
 * JAudio driver. The profile follows the wave groups.
 *
 * Build with driver/build.py (clang). The result is committed as src/rip/pikmin2_driver_blob.h. Building WSF itself
 * requires no PowerPC toolchain.
 */

#include "guest.h"

#define PARAMS_MAGIC 0x57534644u /* 'WSFD' */
#define PARAMS_VERSION 2u

/* The frames that the driver waits for a scene's resources before it gives up. */
#define LOAD_FRAMES 600u

#include "jaudio/pikmin2.h"

/* The global scene loads the Home Button menu's wave group in its second area. */
#define MENU_AREA 1u

struct Params
{
    u32 magic;
    u32 version;
    u32 status;   /* written by the driver */
    u32 sound_id; /* 0x80000000 | the piece's number in `pieces`, or an effect's ID and the draws before it
                     (DRAWS_SHIFT); patched by each .miniwsf */
    u32 sound;    /* the sound's JAISound: 0 once the sound has ended. The driver copies a piece's from the game once a
                     frame. The sound library keeps an effect's here */
    u32 wave_count;       /* the wave groups to load; patched by each .miniwsf */
    u32 waves[MAX_WAVES]; /* each the wave system in the high half and the group in the low half */
    /* the game profile: sizes */
    u32 frame_ticks;           /* time-base ticks between the sound layer's frames */
    u32 max_heaps;             /* each root heap's maximum of standard heaps */
    u32 system_heap_size;      /* JFWSystem's system heap */
    u32 aram_audio_size;       /* "ARAM" for the sound library: the game keeps its "ARAM" in MEM2 */
    u32 aram_graph_size;       /* "ARAM" for graphics */
    u32 aram_priorities;       /* the ARAM manager's three thread priorities, a byte each, from bit 16 down */
    u32 thread_size;           /* a JKRThread, for the main thread's */
    u32 heap_margin;           /* the root heap's space that the sound heap leaves free */
    u32 file_heap_size;        /* the heap that holds the start-up archive while the sound system starts */
    u32 factory_size;          /* PSM::Factory */
    u32 sound_memory;          /* the factory's heap size: the sound library's "ARAM" */
    u32 scene_info_size;       /* PSGame::SceneInfo */
    u32 scene_type;            /* the type of the piece's scene: one that the game plays with no game objects */
    u32 battle_directors_size; /* PSM::DirectorMgr_2PBattle */
    u32 directed_bgm_size;     /* PSSystem::DirectedBgm */
    u32 auto_bgm_size;         /* PSAutoBgm::AutoBgm */
    /* the game profile: the addresses of the game's functions, variables and strings */
    u32 dvd_init;              /* void DVDInit(void) */
    u32 change_dir;            /* BOOL DVDChangeDir(const char*) */
    u32 game_folder;           /* the game's folder on the disc. The game's paths start there */
    u32 sleep;                 /* void JFWDisplay::threadSleep(s64 ticks), a member: sleeps the calling thread */
    u32 get_current_thread;    /* OSThread* OSGetCurrentThread(void) */
    u32 init_mutex;            /* void OSInitMutex(OSMutex*) */
    u32 create_mem1_root_heap; /* MEM1's root heap: JKRExpHeap* (int max_heaps, bool) */
    u32 create_root_heap;      /* the game's root heap, in MEM2: JKRExpHeap* JKRExpHeap::createRoot(int, bool) */
    u32 create_exp_heap;       /* JKRExpHeap* JKRExpHeap::create(u32 size, JKRHeap* parent, bool) */
    u32 create_solid_heap;     /* JKRSolidHeap* JKRSolidHeap::create(u32 size, JKRHeap* parent, bool) */
    u32 get_free_size;         /* s32 JKRHeap::getFreeSize() */
    u32 become_system_heap;    /* JKRHeap* JKRHeap::becomeSystemHeap() */
    u32 become_current_heap;   /* JKRHeap* JKRHeap::becomeCurrentHeap() */
    u32 destroy_heap;          /* void JKRHeap::destroy() */
    u32 adjust_size;           /* s32 JKRSolidHeap::adjustSize() */
    u32 alloc;                 /* void* operator new(u32 size) from the current heap */
    u32 thread_ctor;           /* JKRThread::JKRThread(OSThread*, int message_count) */
    u32 create_aram;           /* JKRAram* JKRAram::create(u32 audio, u32 graph, s32, s32, s32) */
    u32 mount_file_cache;      /* JKRFileCache* JKRFileCache::mount(const char* path, JKRHeap*, const char*) */
    u32 audio_folder;          /* the folder of the game's sound files */
    u32 get_resource;          /* void* JKRFileLoader::getGlbResource(const char* name, JKRFileLoader*) */
    u32 start_up_archive;      /* the start-up archive's name in that folder */
    struct SoundLayer layer;   /* the sound layer's functions and variables (jaudio/pikmin2.h) */
    u32 big_boss;              /* PSM::EnemyBigBoss::sBigBoss: the final boss's sound object */
    u32 load_wave_scene;       /* void PSSystem::WaveScene::load(u8 wave_system, u8 group, u8 area, TaskChecker*) */
    u32 menu_waves;            /* the Home Button menu's wave group: (wave system << 16) | group */
    u32 get_random;            /* s32 JASPlayer::getRandomS32(void): the sound library's random numbers */
    /* the pieces */
    u32 piece_count;
    struct Piece pieces[MAX_PIECES];
};

__attribute__((section(".params"), used)) volatile struct Params driver_params = {
    .magic = PARAMS_MAGIC,
    .version = PARAMS_VERSION,
};

/* More offsets of the game's objects that the driver reads and writes (jaudio/pikmin2.h gives the others). */
#define LOADER_WAVE_SCENE 0x08 /* WaveLoader's WaveScene: the wave groups of its two areas */

/* A TaskChecker. It counts a scene's loads in flight. */
static u32 task[8] __attribute__((aligned(8)));

/* The game's play data at the story's start: every flag clear. The effects of the ship and the pod ask whether the
   debt is paid (the byte at +0x2f). */
static u32 play_data[0x40];

/* The captains' marching horns (PSGame::Rappa). Their effects ask for the horn's place in its tune (+0x2e) and for the
   wait after each note (+0x2c). The place starts at 0, the tune's beginning. The wait is the shortest that the game
   sets: the stick tilted all the way. */
#define RAPPA_SIZE 0x30
#define RAPPA_WAIT 0x2c
static u32 rappas[2][RAPPA_SIZE / 4];

/* A stand-in for the boss of a boss's music. As the music jumps to an attack, the game's onJump (MiddleBossSeq's at
   0x804e1b54, BigBossSeq's at 0x804e2168) marks the boss with the byte at +0xff. Without a boss, onJump stops the game
   with an assertion, and the attack plays nothing. Every byte of the stand-in starts clear. It also stands in for the
   final boss's sound object: the game only tests that sBigBoss is set. The stand-in covers the byte. */
#define STAND_IN_BOSS_SIZE 0x100
static u32 stand_in_boss[STAND_IN_BOSS_SIZE / 4];

static void wait_frame(void)
{
    /* threadSleep is a member function that doesn't use its object (r3). Its OSTime, a 64-bit count, starts at the
       next odd register: r5 (high) and r6 (low). */
    ((fn4)driver_params.sleep)(0, 0, 0, driver_params.frame_ticks);
}

__attribute__((noreturn)) static void fail(u32 step)
{
    driver_params.status = STATUS_ERROR | step;
    for (;;)
    {
        wait_frame();
    }
}

/* Calls a virtual function: the function at `slot` in the vtable that `object` holds at `vtable`. */
static u32 call_virtual(u32 object, u32 vtable, u32 slot, u32 argument)
{
    return ((fn2)word(word(object + vtable) + slot))(object, argument);
}

/* Readies the TaskChecker for a scene's loads. */
static void start_loads(void)
{
    ((fn1)driver_params.init_mutex)((u32)task);
    *(volatile u8*)((u32)task + TASK_COUNT) = 0;
}

/* Waits until the loads in flight are done, the way Scene::scene1stLoadSync does. Returns 0 if they take more than
   LOAD_FRAMES frames. */
static int wait_for_loads(void)
{
    for (u32 frames = 0; byte((u32)task + TASK_COUNT) != 0; frames++)
    {
        if (frames > LOAD_FRAMES)
        {
            return 0;
        }
        wait_frame();
    }
    return 1;
}

/* Loads a scene's resources the way Scene::scene1stLoadSync does. Waits until the loads are done. */
static int load_scene(u32 scene)
{
    start_loads();
    call_virtual(scene, 0, SCENE_LOAD, (u32)task);
    return wait_for_loads();
}

#include "jaudio/pikmin2_steps.h"

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

    /* A piece's ID sets the top bit. An effect's ID doesn't. */
    const int effect = (p->sound_id & 0x80000000u) == 0;
    const u32 number = p->sound_id & 0xffff;
    if (!effect && (number >= p->piece_count || number >= MAX_PIECES))
    {
        fail(1);
    }
    volatile struct Piece* piece = &p->pieces[effect ? 0 : number];
    if (effect)
    {
        pikmin2_draw_randoms();
    }

    /* The disc. The game's main enters the game's folder, and the game's paths start there. */
    ((fn0)p->dvd_init)();
    if (!((fn1)p->change_dir)(p->game_folder))
    {
        fail(2);
    }

    /* JFWSystem::init's heaps: MEM1's root heap, then MEM2's root heap, then the system heap in MEM2's root heap. The
       game's root heap is the one in MEM2. Then the ARAM manager, the main thread's JKRThread and the sound heap. */
    ((fn2)p->create_mem1_root_heap)(p->max_heaps, 0);
    const u32 root = ((fn2)p->create_root_heap)(p->max_heaps, 0);
    const u32 system = root ? ((fn3)p->create_exp_heap)(p->system_heap_size, root, 0) : 0;
    if (!system)
    {
        fail(2);
    }
    ((fn1)p->become_system_heap)(system);
    ((fn5)p->create_aram)(p->aram_audio_size, p->aram_graph_size, (p->aram_priorities >> 16) & 0xff,
                          (p->aram_priorities >> 8) & 0xff, p->aram_priorities & 0xff);
    const u32 sound = pikmin2_sound_heap(root);

    /* System::createSoundSystem: the start-up archive, read into a heap of its own, then the sound system. */
    const u32 files = ((fn3)p->create_exp_heap)(p->file_heap_size, root, 1);
    if (!files)
    {
        fail(3);
    }
    ((fn1)p->become_current_heap)(files);
    const u32 archive =
        ((fn2)p->get_resource)(p->start_up_archive, ((fn3)p->mount_file_cache)(p->audio_folder, files, 0));
    if (!archive)
    {
        fail(3);
    }
    u32 sys_if;
    const u32 manager = pikmin2_sound_system(archive, sound, files, &sys_if);

    /* System::loadSoundResource: the global scene's resources, in a solid heap. The global scene's scene1stLoadSync
       loads the Home Button menu's wave group as well, in the scene's second area. */
    const u32 global = word(manager + MANAGER_GLOBAL_SCENE);
    const u32 global_loader = global ? word(global + SCENE_WAVE_LOADER) : 0;
    if (!global_loader)
    {
        fail(4);
    }
    const u32 solid = ((fn3)p->create_solid_heap)(((fn1)p->get_free_size)(sound), sound, 1);
    ((fn1)p->become_current_heap)(solid);
    start_loads();
    call_virtual(global, 0, SCENE_LOAD, (u32)task);
    ((fn5)p->load_wave_scene)(global_loader + LOADER_WAVE_SCENE, p->menu_waves >> 16, p->menu_waves & 0xffff, MENU_AREA,
                              (u32)task);
    if (!wait_for_loads())
    {
        fail(4);
    }
    ((fn1)p->adjust_size)(solid);
    ((fn1)p->become_current_heap)(sound);

    pikmin2_game_objects();

    /* The scene's wave groups: the .miniwsf's groups other than those of the global scene, one in each of the scene's
       two areas. A scene loads groups of wave system 0. An effect's waves are all in the global scene's groups. */
    u32 areas[2] = {NO_WAVE_SCENE, NO_WAVE_SCENE};
    u32 area_count = 0;
    for (u32 i = 0; i < p->wave_count && i < MAX_WAVES; i++)
    {
        const u32 wave = p->waves[i];
        if (wave == 0 || wave == p->menu_waves)
        {
            continue;
        }
        if (area_count == 2 || effect || (wave >> 16) != 0 || (wave & 0xffff) >= NO_WAVE_SCENE)
        {
            fail(5);
        }
        areas[area_count++] = wave;
    }

    const u32 seq = effect ? 0 : pikmin2_start_piece(piece, manager, global, sound, areas);
    pikmin2_play(effect, piece, seq, sys_if);
}

__attribute__((naked, section(".text.entry"))) void driver_entry(void)
{
    __asm__ volatile("b driver_main");
}
