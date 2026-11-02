#include "xwa/util/time.h"
#ifdef XWA_MODERN
#include "xwa_runtime/timing/host_clock.h"
#endif

// GLOBAL: XWA 0x8B94D4
int g_gameTime;
// GLOBAL: XWA 0x781E64
uint32_t g_lastTickTime;
// GLOBAL: XWA 0x77D028
uint32_t g_simStepLastTickTime;

/* OpenXWA: sim speed (VR quick-wheel slow motion), Q16; 1.0 = classic.
 * While below 1 the frame delta takes all host time since the last call,
 * scaled, and keeps the scaled sub-tick remainder here (Q16 ms). */
static uint32_t s_frameDeltaScaleQ16 = 0x10000u;
static uint64_t s_frameDeltaCarryQ16;

void Time_SetFrameDeltaScale(float scale) {
	if (!(scale > 0.01f)) {
		scale = 0.01f;
	}
	if (scale > 1.0f) {
		scale = 1.0f;
	}
	s_frameDeltaScaleQ16 = (uint32_t)(scale * 65536.0f + 0.5f);
}

// FUNCTION: XWA 0x50E400
void Time_ResetFrameDeltaClocks(void) {
	g_lastTickTime = 0;
	g_simStepLastTickTime = 0;
	s_frameDeltaCarryQ16 = 0;
}

// FUNCTION: XWA 0x50E410
uint32_t Time_GetFrameDelta(void) {
	uint32_t time;
	uint32_t lastTickTime;
	uint32_t deltaTicks;

#ifdef XWA_MODERN
	XwaTime_NoteFrameDeltaPoll();
#endif
	time = timeGetTime();
	lastTickTime = g_lastTickTime;
	if (lastTickTime == 0) {
		lastTickTime = time;
		s_frameDeltaCarryQ16 = 0;
	}
	if (s_frameDeltaScaleQ16 < 0x10000u || s_frameDeltaCarryQ16 != 0) {
		/* Slowed: host ms since the last call, scaled, in 4 ms ticks. */
		const uint64_t acc = s_frameDeltaCarryQ16 + (uint64_t)(time - lastTickTime) * s_frameDeltaScaleQ16;
		deltaTicks = (uint32_t)(acc >> 18);
		s_frameDeltaCarryQ16 = acc - ((uint64_t)deltaTicks << 18);
		g_lastTickTime = time;
		if (s_frameDeltaScaleQ16 >= 0x10000u) {
			/* Back to full speed: the remainder (under a tick) returns to
			 * the classic carry, behind the host clock. */
			g_lastTickTime = time - (uint32_t)(s_frameDeltaCarryQ16 >> 16);
			s_frameDeltaCarryQ16 = 0;
		}
		if (g_lastTickTime == 0) {
			g_lastTickTime = 1; /* 0 means "not running" */
		}
		return deltaTicks;
	}
	deltaTicks = (time - lastTickTime) >> 2;
	g_lastTickTime = lastTickTime + 4 * deltaTicks;
	return deltaTicks;
}

// FUNCTION: XWA 0x50E430
uint32_t Time_GetSimStepDelta(void) {
	uint32_t time;
	uint32_t lastTickTime;
	uint32_t deltaTicks;

	time = timeGetTime();
	lastTickTime = g_simStepLastTickTime;
	if (lastTickTime == 0) {
		lastTickTime = time;
	}
	deltaTicks = (time - lastTickTime) >> 2;
	g_simStepLastTickTime = lastTickTime + 4 * deltaTicks;
	return deltaTicks;
}
