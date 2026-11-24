// SPDX-License-Identifier: MIT

/*
 * The wait and the failure of a driver that sleeps through the game's OSSleepTicks for a frame: galaxy.c, galaxy2.c
 * and jungle_beat.c. A driver includes this file after `driver_params`. Its Params hold `status`, `sleep` (the
 * address of OSSleepTicks) and `frame_ticks`.
 */

#ifndef WSF_DRIVER_SLEEP_H
#define WSF_DRIVER_SLEEP_H

/* One frame of the game. OSSleepTicks takes an OSTime, a 64-bit count in r3 (high) and r4 (low). */
static void wait_frame(void)
{
    ((fn2)driver_params.sleep)(0, driver_params.frame_ticks);
}

/* Stops the driver at `step`. The status word reports the step, and the driver waits for ever. */
__attribute__((noreturn)) static void fail(u32 step)
{
    driver_params.status = STATUS_ERROR | step;
    for (;;)
    {
        wait_frame();
    }
}

#endif
