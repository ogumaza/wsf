// SPDX-License-Identifier: MIT

/*
 * The check of the wave groups in the Galaxy drivers (galaxy.c and galaxy2.c). A driver includes this file after
 * `driver_params`. Its Params hold `section_heap`, `wave_loaded` (JAUSectionHeap::isWaveLoaded), `wave_count` and
 * `waves`.
 */

#ifndef WSF_DRIVER_GALAXY_WAVES_H
#define WSF_DRIVER_GALAXY_WAVES_H

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

#endif
