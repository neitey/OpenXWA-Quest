#ifndef XWA_RUNTIME_SNAPSHOT_PRESENT_H
#define XWA_RUNTIME_SNAPSHOT_PRESENT_H

/*
 * Presentation interpolation for coarse simulation steps.
 *
 * Multiplayer runs the flight simulation in 8-tick steps (about 31 Hz), so
 * without help the headset shows every world state for two or three display
 * frames. The capture keeps the previous step's pose per object slot and,
 * every host frame, rewrites the snapshot's poses to a smoothed presentation
 * time between the last two steps (about half a step of visual lag). The
 * simulation, the lockstep and the network are untouched: this only edits the
 * captured snapshot after the classic frame has run.
 *
 * XWA_FLIGHT_INTERPOLATE=0 turns it off, =1 also enables it for single-step
 * play; unset, it runs whenever the flight step is longer than one tick.
 */

#include <stdint.h>

#include "xwa_runtime/snapshot/snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0 off, 1 on, -1 automatic (coarse steps only). */
int XwaSnapshotPresent_Mode(void);

/* Rewrites s's flight poses for this host frame. game_time is the sim clock
 * the snapshot shows (g_gameTime); enabled = 0 shows the newest step as is
 * and forgets the history. */
void XwaSnapshotPresent_Apply(XwaSnapshot* s, int32_t game_time, int enabled);

void XwaSnapshotPresent_Reset(void);

#ifdef __cplusplus
}
#endif

#endif
