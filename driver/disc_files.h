// SPDX-License-Identifier: MIT

/*
 * The disc's files in the JAudio2 drivers that read them through JSystem: galaxy.c and jungle_beat.c. A driver
 * includes this file after `driver_params`, and defines MOUNT_IN_MEMORY and FROM_HEAD (JKRArchive's and
 * JKRDvdRipper's options) first. Its Params hold `path_to_entry` (DVDConvertPathToEntrynum) and `mount`
 * (JKRArchive::mount).
 */

#ifndef WSF_DRIVER_DISC_FILES_H
#define WSF_DRIVER_DISC_FILES_H

/* Whether the disc has a file at the game's `path`. */
static int on_disc(u32 path)
{
    return (s32)((fn1)driver_params.path_to_entry)(path) >= 0;
}

/* Mounts the archive at the game's `path` in memory from `heap`'s head. */
static u32 mount(u32 path, u32 heap)
{
    return ((fn4)driver_params.mount)(path, MOUNT_IN_MEMORY, heap, FROM_HEAD);
}

#endif
