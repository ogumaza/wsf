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
};

__attribute__((section(".params"), used)) volatile struct Params driver_params = {
    .magic = PARAMS_MAGIC,
    .version = PARAMS_VERSION,
};

/* The reset data, in the driver's memory. The game's main would allocate it from the arena. */
static volatile unsigned char reset_data[RESET_DATA_SIZE];

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
