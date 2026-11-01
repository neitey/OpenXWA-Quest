#ifndef XWA_RUNTIME_HOST_CLOCK_H
#define XWA_RUNTIME_HOST_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void XwaTime_Reset(void);
void XwaTime_AdvanceHostClock(int32_t delta_us);
uint64_t XwaTime_GetElapsedUs(void);
/* Time_GetFrameDelta was polled again this host frame (see host_clock.c:
 * a frame that polls it many times is a blocking wait on real time). */
void XwaTime_NoteFrameDeltaPoll(void);
/* Tests only (replaying from a save state): sets the host clock. */
void XwaTime_SetElapsedUsForTesting(uint64_t elapsed_us);
/* Returns host elapsed milliseconds, matching the unit of Win32 GetTickCount. */
uint32_t XwaTime_GetElapsedTicks(void);
uint64_t XwaTime_GetLegacyTimerIntervalUs(uint32_t intervalMs);

#ifdef __cplusplus
}
#endif

#endif
