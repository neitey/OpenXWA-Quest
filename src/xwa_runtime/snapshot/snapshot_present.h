#ifndef XWA_RUNTIME_SNAPSHOT_PRESENT_H
#define XWA_RUNTIME_SNAPSHOT_PRESENT_H

/*
 * Presentation interpolation for coarse simulation steps.
 *
 * Multiplayer runs the flight simulation in 8-tick steps (about 31 Hz), so
 * without help the headset shows every world state for two or three display
 * frames. The capture keeps the previous step's pose per object slot and,
 * every host frame, rewrites the snapshot's poses to a smoothed presentation
 * time among the last few steps: behind the newest by just enough to ride
 * out the gaps between step arrivals seen lately (network waits). The
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
 * the snapshot shows (g_gameTime), host_us the host clock; enabled = 0 shows
 * the newest step as is and forgets the history. */
void XwaSnapshotPresent_Apply(XwaSnapshot* s, int32_t game_time, uint64_t host_us, int enabled);

typedef struct XwaSnapshotPresentStats {
	uint32_t frames;
	uint32_t steps;
	uint32_t held_frames;  /* the shown time caught up with the newest step */
	double lag_ticks_sum;  /* shown time behind the newest step, summed */
	double max_gap_ticks;  /* longest wait between two steps, in sim ticks */
} XwaSnapshotPresentStats;

/* The counters since the last call (for the multiplayer timing log). */
void XwaSnapshotPresent_TakeStats(XwaSnapshotPresentStats* out);

void XwaSnapshotPresent_Reset(void);

#ifdef __cplusplus
}
#endif

#endif
