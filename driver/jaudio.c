// SPDX-License-Identifier: MIT

/*
 * WSF guest driver for games whose sound code is JAudio2 on JSystem's framework, the sound layer that Nintendo's
 * engines use. It's compiled for Broadway and stored in every game-mode .wsflib of such a game. The player boots the
 * game's startup code: the OS initialisation and the static constructors. The rip patches the game's main to branch
 * here.
 *
 * ntdlib's drivers/jaudio/z2.h then does what the game's start-up does for sound, with the game's functions: the
 * heaps, the ARAM manager, the thread switch manager, the disc thread and the audio set-up. It loads the sound's wave
 * groups, starts the sound and runs the audio frame process for ever. The profile in `driver_params.z2` gives the
 * functions' addresses and the sizes. The ripper fills it in. This file gives the Wii's steps:
 *   - before the heaps, the reset data that the game's main allocates. The audio set-up reads the disc only once the
 *     reset data's flag says that the game has started;
 *   - a frame of the game, as a sleep of the game's frame time. WSF's machine raises no video interrupts;
 *   - the ARAM manager and its transfer buffer. The game keeps its "ARAM" in MEM2, and the calls are the GameCube's.
 * `driver_params.status` reports the driver's progress to the player (see docs/wsf.md).
 *
 * Some games lack functions that the profile names. Link's Crossbow Training has no JFWDisplay::threadSleep. The
 * game's scene manager has no function that loads one wave group either. This file has replacements for them, and the
 * block gives their addresses. For such a game, the ripper puts the addresses in the profile. The driver's sleep works
 * like threadSleep. It sets an alarm of the OS and suspends the thread. The driver's functions for one wave group call
 * JAudio2's functions in the way that Twilight Princess's scene manager calls them. The ripper chooses the
 * replacements, not the driver. A game with the functions therefore runs the same instructions as before the
 * replacements were added.
 *
 * The block's version is 2. The NW4R driver's block (driver.c) is version 1. A player of WSF 1.1.0 accepts only
 * version 1. It refuses this driver's rips.
 *
 * Build with driver/build.py (clang). The result is committed as src/rip/jaudio_driver_blob.h. Building WSF itself
 * requires no PowerPC toolchain.
 */

#include "jaudio/z2.h"

#define PARAMS_MAGIC 0x57534644u /* 'WSFD' */
#define PARAMS_VERSION 2u

/* The bytes of the reset data that the game's main allocates. */
#define RESET_DATA_SIZE 24u

/* JAudio2's offsets: a wave bank's getWaveArc(u32 group) in the bank's vtable, and a wave archive's load status
   (Z2_WAVE_NONE, Z2_WAVE_LOADING or Z2_WAVE_LOADED). */
#define WAVE_BANK_GET_ARC 0x10u
#define WAVE_ARC_STATUS 0x4cu

/* The words of an OSAlarm, with room to spare. */
#define ALARM_WORDS 16u

typedef void (*sleep_fn)(u32, u32, u32, u32);

struct Params
{
    u32 magic;
    u32 version;
    u32 status;         /* written by the driver */
    struct Z2Params z2; /* the sound, its wave groups and the profile (ntdlib's drivers/jaudio/z2.h) */
    u32 sleep;          /* void JFWDisplay::threadSleep(s64 ticks), a member: sleeps the calling thread */
    u32 retrace_ticks;  /* time-base ticks in one video frame */
    u32 reset_data;     /* the variable that holds the reset data's address */
    u32 reset_flag;     /* the offset of the reset data's byte that the audio set-up checks */
    /* the OS's functions for the driver's sleep */
    u32 set_alarm;          /* void OSSetAlarm(OSAlarm*, OSTime ticks, OSAlarmHandler) */
    u32 disable_interrupts; /* BOOL OSDisableInterrupts(void) */
    u32 restore_interrupts; /* BOOL OSRestoreInterrupts(BOOL) */
    u32 suspend_thread;     /* s32 OSSuspendThread(OSThread*) */
    u32 resume_thread;      /* s32 OSResumeThread(OSThread*) */
    /* JAudio2's functions and variables for the driver's functions for one wave group */
    u32 section_heap;       /* the variable that holds JAUSectionHeap's instance */
    u32 wave_bank_table;    /* the offset in the section heap of two words: the wave banks' array, then its length */
    u32 load_wave_arc;      /* bool JASWaveArc::load(JASHeap*): a group of wave bank 0, at the wave heap's head */
    u32 load_wave_arc_tail; /* bool JASWaveArc::loadTail(JASHeap*): a group of wave bank 1, at the heap's tail */
    u32 erase_wave_arc;     /* bool JASWaveArc::erase(void) */
    /* the driver's replacements, for the ripper: the sleep, and the functions for one wave group */
    sleep_fn driver_sleep;
    z2_fn2 driver_load_se_wave;
    z2_fn2 driver_load_bgm_wave;
    z2_fn3 driver_wave_status;
    z2_fn2 driver_erase_bgm_wave;
    z2_fn2 driver_erase_se_wave;
};

static void alarm_sleep(u32 object, u32 pad, u32 high, u32 low);
static u32 load_se_wave(u32 scene, u32 group);
static u32 load_bgm_wave(u32 scene, u32 group);
static u32 wave_status(u32 scene, u32 group, u32 bank);
static u32 erase_bgm_wave(u32 scene, u32 group);
static u32 erase_se_wave(u32 scene, u32 group);

__attribute__((section(".params"), used)) volatile struct Params driver_params = {
    .magic = PARAMS_MAGIC,
    .version = PARAMS_VERSION,
    .driver_sleep = alarm_sleep,
    .driver_load_se_wave = load_se_wave,
    .driver_load_bgm_wave = load_bgm_wave,
    .driver_wave_status = wave_status,
    .driver_erase_bgm_wave = erase_bgm_wave,
    .driver_erase_se_wave = erase_se_wave,
};

/* The reset data, in the driver's memory. The game's main would allocate it from the arena. */
static volatile unsigned char reset_data[RESET_DATA_SIZE];

/* The alarm of the driver's sleep, and the thread that it resumes. */
static volatile u32 alarm[ALARM_WORDS] __attribute__((aligned(8)));
static volatile u32 sleeper;

static void z2_init_machine(volatile struct Z2Params* p)
{
    (void)p;
    for (u32 i = 0; i < RESET_DATA_SIZE; i++)
    {
        reset_data[i] = 0;
    }
    reset_data[driver_params.reset_flag] = 1;
    *(volatile u32*)driver_params.reset_data = (u32)reset_data;
}

static void z2_wait_frame(volatile struct Z2Params* p)
{
    /* threadSleep is a member function that doesn't use its object (r3). Its OSTime, a 64-bit count, starts at the
       next odd register: r5 (high) and r6 (low). */
    ((z2_fn4)driver_params.sleep)(0, 0, 0, p->frame_retraces * driver_params.retrace_ticks);
}

/* The alarm's handler: the sleeping thread runs again. */
static void wake(u32 alarm_address, u32 context)
{
    (void)alarm_address;
    (void)context;
    ((z2_fn1)driver_params.resume_thread)(sleeper);
}

/* The driver's sleep, with threadSleep's arguments. With interrupts off, the alarm can't go off before the thread is
   suspended. OSSetAlarm takes the OSTime in the same registers as threadSleep: r5 (high) and r6 (low). OSSetAlarm
   takes the handler in r7. */
static void alarm_sleep(u32 object, u32 pad, u32 high, u32 low)
{
    (void)object;
    (void)pad;
    const u32 enabled = ((z2_fn0)driver_params.disable_interrupts)();
    sleeper = ((z2_fn0)driver_params.z2.get_current_thread)();
    ((z2_fn5)driver_params.set_alarm)((u32)alarm, 0, high, low, (u32)wake);
    ((z2_fn1)driver_params.suspend_thread)(sleeper);
    ((z2_fn1)driver_params.restore_interrupts)(enabled);
}

/* The wave archive of group `group` of wave bank `bank`, or 0. Twilight Princess's scene manager finds it the same
   way. */
static u32 wave_arc(u32 bank, u32 group)
{
    const u32 heap = z2_word(driver_params.section_heap);
    const u32 table = heap + driver_params.wave_bank_table;
    const u32 wave_bank = heap && bank < z2_word(table + 4) ? z2_word(z2_word(table) + 4 * bank) : 0;
    return wave_bank ? ((z2_fn2)z2_word(z2_word(wave_bank) + WAVE_BANK_GET_ARC))(wave_bank, group) : 0;
}

/* The driver's functions for one wave group, with the scene manager's arguments. They don't use the first. */
static u32 load_se_wave(u32 scene, u32 group)
{
    (void)scene;
    const u32 arc = wave_arc(0, group);
    return arc ? ((z2_fn2)driver_params.load_wave_arc)(arc, 0) : 0;
}

static u32 load_bgm_wave(u32 scene, u32 group)
{
    (void)scene;
    const u32 arc = wave_arc(1, group);
    return arc ? ((z2_fn2)driver_params.load_wave_arc_tail)(arc, 0) : 0;
}

static u32 wave_status(u32 scene, u32 group, u32 bank)
{
    (void)scene;
    const u32 arc = wave_arc(bank, group);
    return arc ? z2_word(arc + WAVE_ARC_STATUS) : Z2_WAVE_NONE;
}

static u32 erase_bgm_wave(u32 scene, u32 group)
{
    (void)scene;
    const u32 arc = wave_arc(1, group);
    return arc ? ((z2_fn1)driver_params.erase_wave_arc)(arc) : 0;
}

static u32 erase_se_wave(u32 scene, u32 group)
{
    (void)scene;
    const u32 arc = wave_arc(0, group);
    return arc ? ((z2_fn1)driver_params.erase_wave_arc)(arc) : 0;
}

static void z2_create_aram(volatile struct Z2Params* p)
{
    ((z2_fn5)p->create_aram)(p->aram_audio_size, p->aram_graph_size, p->aram_priorities >> 16 & 0xff,
                             p->aram_priorities >> 8 & 0xff, p->aram_priorities & 0xff);
}

static void z2_set_aram_buffer(volatile struct Z2Params* p, u32 system_heap)
{
    ((z2_fn3)p->set_aram_buffer)(0, p->aram_buffer_size, system_heap);
}

__attribute__((noreturn)) void driver_main(void)
{
    volatile struct Params* p = &driver_params;
    if (p->magic != PARAMS_MAGIC || p->version != PARAMS_VERSION)
    {
        p->status = Z2_STATUS_ERROR | 1;
        for (;;)
        {
        }
    }

    z2_main(&p->z2, &p->status);
}

__attribute__((naked, section(".text.entry"))) void driver_entry(void)
{
    __asm__ volatile("b driver_main");
}
