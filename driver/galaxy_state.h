// SPDX-License-Identifier: MIT

/*
 * The state of a piece of music in the Galaxy drivers (galaxy.c and galaxy2.c). The game's BGM manager gives a piece
 * a state from the piece's row of the state table: which children of track 0 play. Some of the game's code also sets
 * the piece's tempo, the pitch of its children or the volume of one child with the state. ntdlib's
 * jaudio/galaxy_states.h lists the states and what sets them. A state's .miniwsf patches the words from `state` on.
 * Once the driver has started the piece, it gives the piece the state through the game's functions, with no fade.
 */

#ifndef WSF_DRIVER_GALAXY_STATE_H
#define WSF_DRIVER_GALAXY_STATE_H

#include "guest.h"

/* The value of a word that names no state or no child. */
#define NO_STATE 0xffffffffu

/* JAISound's virtual functions: the tempo manager (JAITempoMgr*), and child n's parameters (JAISoundParamsMove*). */
#define SOUND_TEMPO_MGR 0x28u
#define SOUND_CHILD 0xcu

/* The block's words for the state, at its end. */
struct PieceState
{
    u32 set_bgm_state;     /* void MR::setStageBGMState(s32 state, u32 frames) */
    u32 move_pitch;        /* void JAISoundParamsMove::movePitch(f32 pitch, u32 frames) */
    u32 move_child_volume; /* void (s32 child, u32 frames, f32 volume) through the stage's BGM, or 0 for none */
    u32 state;             /* the piece's state, or NO_STATE for the start's; patched by a state's .miniwsf */
    u32 tempo;             /* the tempo ratio, a float's bits, or 0 to leave the tempo */
    u32 pitch;             /* the pitch ratio, a float's bits */
    u32 pitched_children;  /* the children of track 0 that take the pitch, bit n for child n */
    u32 quiet_child;       /* a child of track 0 that plays at `quiet_volume`, or NO_STATE */
    u32 quiet_volume;      /* that volume, a float's bits */
};

/* Calls `fn` with `a` to `c` in r3 to r5 and the float at `f` in f1, as the game's code passes a float. The driver is
   built without floating point. A float that it passed would go in a general register. */
__attribute__((naked)) static void call_with_float(u32 fn, const volatile u32* f, u32 a, u32 b, u32 c)
{
    __asm__ volatile(
        "mtctr 3\n"
        "lfs 1, 0(4)\n"
        "mr 3, 5\n"
        "mr 4, 6\n"
        "mr 5, 7\n"
        "bctr\n");
}

/* Gives the stage's music, whose JAISound is `sound`, the state in `s`. The state's row sets the children of track 0
   through the BGM's track controllers. The tempo goes to the sound's tempo manager as the BGM's function at 0x80081280
   in Galaxy 2's main.dol writes it for a move of 0 frames: the ratio, then no move. The pitch goes to each child that
   `pitched_children` names, and the quiet child's volume to its track controller. */
static void set_piece_state(volatile struct PieceState* s, u32 sound)
{
    if (s->state == NO_STATE || !sound)
    {
        return;
    }

    ((fn2)s->set_bgm_state)(s->state, 0);
    const u32 vtable = word(sound);
    if (s->tempo)
    {
        const u32 tempo = ((fn1)word(vtable + SOUND_TEMPO_MGR))(sound);
        if (tempo)
        {
            *(volatile u32*)tempo = s->tempo;
            *(volatile u32*)(tempo + 4) = 0;
            *(volatile u32*)(tempo + 8) = 0;
            *(volatile u32*)(tempo + 0xc) = 0;
        }
    }
    for (u32 i = 0; i < 16; i++)
    {
        if (!(s->pitched_children & (1u << i)))
        {
            continue;
        }
        const u32 child = ((fn2)word(vtable + SOUND_CHILD))(sound, i);
        if (child)
        {
            call_with_float(s->move_pitch, &s->pitch, child, 0, 0);
        }
    }
    if (s->quiet_child != NO_STATE && s->move_child_volume)
    {
        call_with_float(s->move_child_volume, &s->quiet_volume, s->quiet_child, 0, 0);
    }
}

#endif
