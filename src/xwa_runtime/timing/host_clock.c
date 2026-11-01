#include "xwa_runtime/timing/host_clock.h"

#include "xwa/util/time.h"

#include "aeron/time.h"

#include <SDL3/SDL_timer.h>

enum { XWA_LEGACY_TIMER_QUANTUM_US = 15625 };

static uint64_t g_xwaElapsedUs;

/* Real time inside a blocking wait. The host clock advances once per host
 * frame, but the classic multiplayer code has waits that poll the frame
 * delta in a loop until a packet arrives or a timeout passes (resending the
 * world state to a player out of sync, waiting for acks, a player running
 * too far ahead). Frozen within the frame, their timeouts never passed and
 * their resends never repeated: the session start hung, and a nested
 * checksum packet re-entered the wait until the stack ran out. A frame that
 * polls the delta this often is in such a wait, and from then on the clock
 * follows real time (sleeping a little per poll instead of spinning). The
 * next frame's advance never goes back behind what the wait has seen. */
enum { XWA_BLOCKING_WAIT_POLLS = 64 };
static uint64_t g_frameBaseUs;     /* the host clock at this frame's advance */
static uint64_t g_frameRealStartUs; /* real time at this frame's advance */
static uint64_t g_maxReturnedUs;
static uint32_t g_framePolls;

void XwaTime_Reset(void) {
	g_xwaElapsedUs = 0;
	g_frameBaseUs = 0;
	g_maxReturnedUs = 0;
	g_framePolls = 0;
	g_frameRealStartUs = Aeron_NowUs();
	g_gameTime = 0;
}

void XwaTime_AdvanceHostClock(int32_t delta_us) {
	if (delta_us > 0) {
		g_xwaElapsedUs += (uint32_t)delta_us;
	}
	if (g_xwaElapsedUs < g_maxReturnedUs) {
		g_xwaElapsedUs = g_maxReturnedUs;
	}
	g_frameBaseUs = g_xwaElapsedUs;
	g_frameRealStartUs = Aeron_NowUs();
	g_framePolls = 0;
}

void XwaTime_NoteFrameDeltaPoll(void) {
	if (++g_framePolls == XWA_BLOCKING_WAIT_POLLS + 1) {
		/* The wait starts now: real time counts from here, not from the
		 * frame's start. A wait in the frame that loaded the mission (the
		 * session start's handshake) otherwise began with the whole load
		 * time on the clock, and the host could take that as its lead over
		 * the world (seconds of lag that took minutes to wear off). */
		g_frameBaseUs = g_maxReturnedUs > g_xwaElapsedUs ? g_maxReturnedUs : g_xwaElapsedUs;
		g_frameRealStartUs = Aeron_NowUs();
	}
	if (g_framePolls > XWA_BLOCKING_WAIT_POLLS) {
		SDL_Delay(1);
	}
}

uint64_t XwaTime_GetElapsedUs(void) {
	if (g_framePolls > XWA_BLOCKING_WAIT_POLLS) {
		const uint64_t now = Aeron_NowUs();
		const uint64_t us = g_frameBaseUs + (now > g_frameRealStartUs ? now - g_frameRealStartUs : 0);
		if (us > g_maxReturnedUs) {
			g_maxReturnedUs = us;
		}
		return g_maxReturnedUs > us ? g_maxReturnedUs : us;
	}
	return g_xwaElapsedUs;
}

void XwaTime_SetElapsedUsForTesting(uint64_t elapsed_us) {
	g_xwaElapsedUs = elapsed_us;
	g_frameBaseUs = elapsed_us;
	g_maxReturnedUs = 0;
	g_framePolls = 0;
}

uint32_t XwaTime_GetElapsedTicks(void) { return (uint32_t)(XwaTime_GetElapsedUs() / 1000u); }

uint64_t XwaTime_GetLegacyTimerIntervalUs(uint32_t intervalMs) {
	uint64_t requestedUs;

	requestedUs = (uint64_t)intervalMs * 1000u;
	/* Preserve the coarse GetTickCount deadline observed by the original while
	   allowing the host scheduler to use Aeron's precise clock. */
	return ((requestedUs + XWA_LEGACY_TIMER_QUANTUM_US - 1u) / XWA_LEGACY_TIMER_QUANTUM_US) *
		   XWA_LEGACY_TIMER_QUANTUM_US;
}

uint32_t timeGetTime(void) { return XwaTime_GetElapsedTicks(); }

uint32_t GetTickCount(void) { return XwaTime_GetElapsedTicks(); }
