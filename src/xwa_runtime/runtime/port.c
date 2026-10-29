#include "xwa_runtime/runtime/port.h"

#include <SDL3/SDL_timer.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "aeron/aeron.h"
#include "aeron/compat/ddraw.h"
#include "aeron/compat/dsound.h"
#include "aeron/compat/host.h"
#include "xwa/assets/file_io.h"
#include "xwa/audio/music.h"
#include "xwa/assets/linez.h"
#include "xwa/assets/opt_model.h"
#include "xwa/assets/sprite_resource.h"
#include "xwa/assets/string_table.h"
#include "xwa/config/game_config.h"
#include "xwa/config/pilot.h"
#include "xwa/flight/flight.h"
#include "xwa/flight/flight_display.h"
#include "xwa/flight/hangar.h"
#include "xwa/flight/hud/hud.h"
#include "xwa/flight/mission/mission.h"
#include "xwa/flight/object/collision.h"
#include "xwa/frontend/flight_loading.h"
#include "xwa/frontend/frontend_display.h"
#include "xwa/frontend/frontend_flight.h"
#include "xwa/frontend/frontend_screen.h"
#include "xwa_runtime/runtime/flight_checkpoints.h"
#include "xwa/frontend/frontend_input.h"
#include "xwa/frontend/frontend_resources.h"
#include "xwa/input/forcefeedback.h"
#include "xwa/render/renderer.h"
#include "xwa/util/memory.h"
#include "xwa/util/time.h"
#include "xwa/xwa_options.h"
#include "xwa_runtime/config/modern_controller_options_screen.h"
#include "xwa_runtime/config/modern_input_options.h"
#include "xwa_runtime/config/modern_video_options.h"
#include "xwa_runtime/input/controller_mapping.h"
#include "xwa_runtime/input/input_bridge.h"
#include "xwa_runtime/input/mouse_flight.h"
#include "xwa_runtime/input/rumble_provider.h"
#include "xwa_runtime/input/winmm_joystick_provider.h"
#include "xwa_runtime/runtime/flight_pause_task.h"
#include "xwa_runtime/runtime/flight_task.h"
#include "xwa_runtime/runtime/frontend_task.h"
#include "xwa_runtime/runtime/movie_task.h"
#include "xwa_runtime/runtime/presentation.h"
#include "xwa_runtime/runtime/vr_buttons.h"
#include "xwa_runtime/runtime/vr_mapping.h"
#include "xwa_runtime/snapshot/snapshot.h"
#include "xwa_runtime/timing/host_clock.h"

#include <stdio.h>
#include <string.h>

static int g_xwaPortInitialized;
static int g_xwaPortShouldQuit;
static int g_xwaPortHadFocus;
/* One-way latch: the window has held input focus at least once since init.
 * The frontend/movie focus pause only engages after this is set, so a launch
 * where the OS never handed focus back (setup dialogs, background start) does
 * not begin as a silent freeze. */
static int g_xwaPortEverHadFocus;
static int g_xwaPortClassicFlightRenderingEnabled = 1;
static char g_xwaPortCommandLine[1024];
/* Flight mouse capture. Relative mode is re-asserted on every flight tick, so a
 * manual release has to be latched here instead of applied once. */
static int g_xwaPortMouseCaptureSuspended;
/* Cached OS cursor visibility. Aeron_SetRelativeMouseMode hides the cursor on
 * every transition and never restores it, so visibility is driven explicitly
 * next to each capture decision; -1 forces the first call through. */
static int g_xwaPortHostCursorVisible = -1;

static AeronDx5Rect XwaPort_Dx5PresentationRect(void* context, int surfaceWidth, int surfaceHeight) {
	const XwaPresentationRect rect = XwaPresentation_ClassicSafeFrame();
	(void)context;
	(void)surfaceWidth;
	(void)surfaceHeight;
	return (AeronDx5Rect) { rect.x, rect.y, rect.width, rect.height };
}

static void XwaPort_Dx5Presented(void* context, int surfaceWidth, int surfaceHeight) {
	(void)context;
	XwaSnapshot_EmitSurfaceEvent(XWA_SURFACE_EVENT_PRESENT, 0, 0, surfaceWidth - 1, surfaceHeight - 1);
}

static const char XWA_STRING_DATA_TAG[] = "STRINGDATA";

enum {
	XWA_STRING_DATA_INITIAL_BYTES = 0xa410,
};

static void XwaPort_FreeStartupAssets(void) {
	if (g_stringDataHandle != 0) {
		Memory_FreeHandle(XWA_STRING_DATA_TAG, g_stringDataHandle);
		g_stringDataHandle = 0;
	}

	Linez_FreeDict();
	SpriteResource_FreeGroups();
}

static void XwaPort_ApplyClassicFlightRenderingPolicy(void) {
	const int suppress = XwaFlightTask_IsActive() && !FlightDisplay_IsFrontendModalActive() &&
						 !g_xwaPortClassicFlightRenderingEnabled;
	AeronDx5_SetClassicFlightRenderingSuppressed(suppress);
}

void XwaPort_SetClassicFlightRenderingEnabled(int enabled) {
	g_xwaPortClassicFlightRenderingEnabled = enabled ? 1 : 0;
}

void XwaPort_SubmitRetainedClassicFrame(void) { AeronDx5_ForceSubmitRetainedFrame(); }

uint64_t XwaPort_GetClassicFlightFrameSerial(void) { return AeronDx5_GetClassicFlightFrameSerial(); }

void XwaPort_SetCommandLine(const char* commandLine) {
	if (commandLine == 0) {
		commandLine = "";
	}

	snprintf(g_xwaPortCommandLine, sizeof(g_xwaPortCommandLine), "%s", commandLine);
	g_cmdLine = g_xwaPortCommandLine;
}

int XwaPort_Init(void) {
	AeronDx5_Configure(&(AeronDx5Config) {
		.presentation_rect = XwaPort_Dx5PresentationRect,
		.presented = XwaPort_Dx5Presented,
	});
	XwaWinmmJoystick_RegisterSource();
	XwaRumble_RegisterProvider();
	XwaTime_Reset();
	File_SetVfs(Aeron_GetVfs());
	if (SpriteResource_LoadCatalog("Resdata.txt") != 0) {
		Aeron_LogError("xwa.assets", "Failed to load required sprite catalog 'Resdata.txt'");
	}
	Linez_LoadDict("xwa.tab");
	/* Original _WinMain@16 gives the string loader a valid initial handle. Large
	   string tables replace this allocation after measuring the file. */
	g_stringDataHandle = Memory_AllocHandle(XWA_STRING_DATA_TAG, XWA_STRING_DATA_INITIAL_BYTES);
	if (g_stringDataHandle == 0) {
		Aeron_LogError("xwa.assets", "Failed to allocate initial game string data buffer");
		XwaPort_FreeStartupAssets();
		return 0;
	}
	if (StringTable_LoadGameStrings() != 0) {
		Aeron_LogError("xwa.assets", "Failed to load game string table 'strings.txt'");
		XwaPort_FreeStartupAssets();
		return 0;
	}
	/* Original _WinMain@16 render startup: allocate scene buffers, initialize the
	   D3D texture pool, then enable the frontend model-render path. */
	sw3d_InitSceneBuffers();
	D3DInfo_InitPool();
	Renderer_InitFrontendHardwareSettings();
	g_flightRenderToFrontend = 1;
	/* TODO: Reimplement remaining original WinMain @ 0x50A4A0 early init:
	   deusdbg command-line setup, nopilots/console flags. */
	XwaOptions_ParseCommandLine(g_cmdLine);
	if (!XwaFrontendTask_Init()) {
		return 0;
	}

	g_xwaPortInitialized = 1;
	g_xwaPortShouldQuit = 0;
	g_xwaPortHadFocus = 1;
	g_xwaPortEverHadFocus = 0;
	g_xwaPortClassicFlightRenderingEnabled = 1;
	return 1;
}

int XwaPort_EverHadFocus(void) { return g_xwaPortEverHadFocus; }

static void XwaPort_SetHostCursorVisible(int visible) {
	visible = visible != 0;
	if (g_xwaPortHostCursorVisible != visible) {
		g_xwaPortHostCursorVisible = visible;
		Aeron_SetHostCursorVisible(visible);
	}
}

int XwaPort_QueueMouseLookToggle(void) {
	if (!XwaFlightTask_IsActive() || FlightDisplay_IsFrontendModalActive()) {
		return 0;
	}

	if (g_injectedKeyCount >= (int)(sizeof g_injectedKeyStack / sizeof g_injectedKeyStack[0])) {
		Aeron_LogWarn("xwa", "could not queue cockpit mouse-look toggle: flight key stack is full");
		return 1;
	}

	g_injectedKeyStack[g_injectedKeyCount++] = KEY_SCROLL_LOCK;
	return 1;
}

/* VR controller buttons in flight: xwa_runtime/runtime/vr_buttons.c. */

static int g_xwaPortVrFlightButtons;

/* Queues a flight action key behind any already pending ones (the injected
 * stack pops from the top, so FIFO order means inserting at the bottom). */
static int XwaPort_QueueFlightActionKey(uint16_t key) {
	int i;
	if (key == KEY_NONE) {
		return 0;
	}
	if (g_injectedKeyCount >= (int)(sizeof g_injectedKeyStack / sizeof g_injectedKeyStack[0])) {
		return 0;
	}
	for (i = g_injectedKeyCount; i > 0; --i) {
		g_injectedKeyStack[i] = g_injectedKeyStack[i - 1];
	}
	g_injectedKeyStack[0] = key;
	++g_injectedKeyCount;
	return 1;
}

/* Short haptic tick on the VR controllers as press feedback. */
static void XwaPort_VrButtonTick(void) {
	const AeronInputSnapshot* input = Aeron_InputSnapshot();
	int                       i;
	if (!input) {
		return;
	}
	for (i = 0; i < AERON_CONTROLLER_MAX; ++i) {
		const AeronControllerSnapshot* pad = &input->controllers[i];
		if (pad->connected && strcmp(pad->path, "openxr:touch") == 0) {
			Aeron_RumbleController(pad->instance_id, 0x3000, 0x6000, 25);
			return;
		}
	}
}

static CraftData* XwaPort_VrConsoleCraft(void);
static int XwaPort_VrChordEligible(int* hyper);

/* The local player is in the map view (flight running, not in a menu). */
static int XwaPort_VrMapOpen(void) {
	if (!XwaFlightTask_IsActive() || g_inHangarReady || FlightDisplay_IsFrontendModalActive() ||
		XwaFlightPauseTask_IsActive() || g_localPlayer < 0 || g_localPlayer >= XWA_PLAYER_COUNT) {
		return 0;
	}
	return g_players[g_localPlayer].mapCameraState != 0;
}

/* Both triggers pulled together (within VR_CHORD_WINDOW_MS): lands in the
 * hangar when the game offers it ("press Space" near a mothership's bay)
 * and jumps to hyperspace at a buoy. While either is on offer the left
 * trigger's own action (cycle warheads) waits for its release, so the
 * chord does not also switch warheads. */
#define VR_CHORD_PULL 0.75f
#define VR_CHORD_RELEASE 0.3f
#define VR_CHORD_WINDOW_MS 350u

static struct {
	int down[2];
	uint64_t down_ms[2];
	int fired;      /* chord fired; wait for both triggers released */
	int pending[2]; /* deferred trigger press (its function), per physical hand */
} g_vrChord;

static void XwaPort_RunVrFunction(AeronXrButton button, int fn);

static void XwaPort_UpdateVrChord(int enabled) {
	AeronXrHand hand[2];
	int hyper = 0;
	const int eligible = enabled && XwaPort_VrChordEligible(&hyper);
	const uint64_t now = SDL_GetTicks();
	for (int h = 0; h < 2; ++h) {
		float t = 0.0f;
		if (Aeron_XrGetHand(h, &hand[h])) {
			t = hand[h].trigger;
		}
		if (!g_vrChord.down[h] && t >= VR_CHORD_PULL) {
			g_vrChord.down[h] = 1;
			g_vrChord.down_ms[h] = now;
		} else if (g_vrChord.down[h] && t < VR_CHORD_RELEASE) {
			g_vrChord.down[h] = 0;
			if (g_vrChord.pending[h]) {
				/* A plain trigger pull after all. */
				if (!g_vrChord.fired) {
					XwaPort_RunVrFunction(h == XwaVrButtons_SlotHand(AERON_XR_BUTTON_LEFT_TRIGGER)
											  ? AERON_XR_BUTTON_LEFT_TRIGGER
											  : AERON_XR_BUTTON_RIGHT_TRIGGER,
										  g_vrChord.pending[h]);
				}
				g_vrChord.pending[h] = 0;
			}
		}
	}
	if (!g_vrChord.down[0] && !g_vrChord.down[1]) {
		g_vrChord.fired = 0;
	}
	if (!eligible || g_vrChord.fired || !g_vrChord.down[0] || !g_vrChord.down[1]) {
		return;
	}
	const uint64_t a = g_vrChord.down_ms[0], b = g_vrChord.down_ms[1];
	if ((a > b ? a - b : b - a) > VR_CHORD_WINDOW_MS) {
		return;
	}
	g_vrChord.fired = 1;
	g_vrChord.pending[0] = g_vrChord.pending[1] = 0;
	if (hyper) {
		XwaPort_VrConsoleAction(XWA_VRC_HYPERSPACE, 0);
	} else {
		XwaPort_QueueFlightActionKey(KEY_SPACE);
	}
	XwaPort_VrButtonTick();
}

/* One press of a mapped function (vr_mapping.h). Fire, voice and a quick
 * wheel on a grip work by holding and do nothing here. */
static void XwaPort_RunVrFunction(AeronXrButton button, int fn) {
	const XwaVrFunction* f = XwaVrMap_FunctionInfo(fn);
	Aeron_LogInfo("xwa.vr", "button %s%s: %s", XwaVrButtons_AltHeld() ? "Alt+" : "", XwaVrButtons_ButtonName(button, 1),
				  f->name);
	switch (f->kind) {
		case XWA_VRF_KIND_KEY:
			if (XwaPort_QueueFlightActionKey(f->key)) {
				XwaPort_VrButtonTick();
			}
			break;
		case XWA_VRF_KIND_CONSOLE:
			XwaPort_VrConsoleAction((XwaVrConsoleAction)f->action, f->arg);
			XwaPort_VrButtonTick();
			break;
		case XWA_VRF_KIND_WHEEL:
			if (button != AERON_XR_BUTTON_LEFT_GRIP && button != AERON_XR_BUTTON_RIGHT_GRIP) {
				XwaVrButtons_WheelToggle(button);
			}
			break;
		default:
			break;
	}
}

static void XwaPort_UpdateVrFlightButtons(int enabled) {
	AeronXrButton button;
	int           shifted;
	int           hyper = 0;
	enabled = enabled && Aeron_XrIsPresenting();
	/* The thumbstick response curve (Stick Response) applies in flight. */
	XwaControllerMapping_SetXrFlight(enabled);
	if (enabled != g_xwaPortVrFlightButtons) {
		g_xwaPortVrFlightButtons = enabled;
		Aeron_XrSetFlightButtons(enabled);
	}
	/* Swapped thumbsticks: in flight only, with the flight buttons (the
	 * hangar's menu keys and the menus' pointer keep the sticks as they
	 * are). Every frame, so the VR Options change applies at once. */
	Aeron_XrSetSwapSticks(enabled && XwaVrButtons_SticksSwapped());
	XwaPort_UpdateVrChord(enabled);
	if (!enabled) {
		g_vrChord.pending[0] = g_vrChord.pending[1] = 0;
		XwaVrButtons_WheelClose();
		return;
	}
	const XwaVrMapping* map = XwaVrMap_Current();
	/* Fire on whichever buttons have it now (an Alt grip may change that). */
	Aeron_XrSetFireButtons(XwaVrMap_SlotsOfKind(map, XWA_VRF_KIND_FIRE, Aeron_XrFlightButtonsHeld()));
	const int chord_offered = XwaPort_VrChordEligible(&hyper);
	while (Aeron_XrPollButton(&button, &shifted)) {
		(void)shifted;
		if ((int)button < 0 || (int)button >= AERON_XR_BUTTON_COUNT || !XwaVrButtons_SlotPresent(button)) {
			continue;
		}
		const int alt = XwaVrButtons_AltHeld();
		const int fn = XwaVrMap_Effective(map, button, alt);
		/* The craft is lost and loading the quick save is offered: A loads
		 * it, B carries on to the mission's end. */
		if (!alt && (button == AERON_XR_BUTTON_A || button == AERON_XR_BUTTON_B) &&
			XwaFlightCheckpoints_OfferAnswer(button == AERON_XR_BUTTON_A)) {
			XwaPort_VrButtonTick();
			continue;
		}
		if (button == XWA_VR_MAP_EXIT_BUTTON && !alt && XwaPort_VrMapOpen()) {
			/* B leaves the map (the map view says so). */
			if (XwaPort_QueueFlightActionKey(KEY_M)) {
				XwaPort_VrButtonTick();
			}
			continue;
		}
		const int kind = XwaVrMap_FunctionInfo(fn)->kind;
		if ((button == AERON_XR_BUTTON_LEFT_TRIGGER || button == AERON_XR_BUTTON_RIGHT_TRIGGER) && chord_offered &&
			(kind == XWA_VRF_KIND_KEY || kind == XWA_VRF_KIND_CONSOLE)) {
			/* Both triggers land / jump: a trigger's own press is decided on
			 * its release (or dropped by the chord). */
			if (!g_vrChord.fired) {
				g_vrChord.pending[XwaVrButtons_SlotHand(button)] = fn;
			}
			continue;
		}
		XwaPort_RunVrFunction(button, fn);
	}
}

/* VR lap console: live ship state for its buttons, and its presses as the
 * game's own key actions. */
static CraftData* XwaPort_VrConsoleCraft(void) {
	const int flight = XwaFlightTask_IsActive();
	PlayerData* player;
	uint16_t objIdx;
	if (!Aeron_XrIsPresenting() || !flight || g_inHangarReady || FlightDisplay_IsFrontendModalActive() ||
		XwaFlightPauseTask_IsActive()) {
		return NULL;
	}
	/* While a mission loads the flight task is already active but the object
	 * storage is not (or still holds the last mission's stale pointers):
	 * gate on the storage handle like the snapshot does. */
	if (!g_objectTable || g_objectTableHandle == 0 || g_localPlayer < 0 || g_localPlayer >= XWA_PLAYER_COUNT) {
		return NULL;
	}
	player = &g_players[g_localPlayer];
	objIdx = (uint16_t)player->objectIndex;
	if (objIdx == 0xffffu || objIdx >= g_objectTableSlotCount || player->mapCameraState || player->hyperspacePhase ||
		g_objectTable[objIdx].objectType == OBJ_None ||
		g_objectTable[objIdx].objectSignature != player->boundObjectSignature || g_objectTable[objIdx].mobj == NULL) {
		return NULL;
	}
	return g_objectTable[objIdx].mobj->pCraft;
}

/* The VR throttle lever is an absolute throttle, and the flight input only
 * acts on it when it crosses into another 1/16 step: a lever left at full
 * from the last mission (or moved while the game set the throttle itself:
 * a new mission, the hangar, match speed, a collision) did nothing until
 * pulled back past where it stood. While the lever rests, it follows the
 * craft's real throttle instead, and the flight input's smoothed value with
 * it (so following it sends no throttle key). */
static void XwaPort_SyncVrThrottleLever(void) {
	static float last_lever = -1.0f;
	static uint64_t resting_since_us;
	const uint64_t now_us = XwaTime_GetElapsedUs();
	if (!Aeron_XrIsPresenting()) {
		last_lever = -1.0f;
		return;
	}
	const float lever = Aeron_XrThrottleLever();
	if (lever != last_lever) {
		last_lever = lever;
		resting_since_us = now_us;
		return;
	}
	const CraftData* craft = XwaPort_VrConsoleCraft();
	/* Resting for a moment: the steps the lever sent have landed. */
	if (!craft || now_us - resting_since_us < 600000u) {
		return;
	}
	const float actual = (float)craft->throttleSpeed / 65535.0f;
	if (fabsf(actual - lever) <= 0.6f / 16.0f) {
		return;
	}
	Aeron_XrSetThrottleLever(actual);
	last_lever = Aeron_XrThrottleLever();
	if (g_throttleSmoothed != -1) {
		g_throttleSmoothed = (int)lrintf(actual * 255.0f);
	}
}

void XwaPort_GetVrConsoleState(XwaVrConsoleState* out) {
	static int flight_was_active;
	static unsigned int mission_serial;
	const int flight_active = XwaFlightTask_IsActive();
	const CraftData* craft = XwaPort_VrConsoleCraft();
	const PlayerData* player = &g_players[g_localPlayer];
	uint16_t objectType;
	if (flight_active && !flight_was_active) {
		++mission_serial;
	}
	flight_was_active = flight_active;
	memset(out, 0, sizeof *out);
	out->mission_serial = mission_serial;
	out->hud_argb = g_hudColors[0];
	out->offer_active = flight_active && XwaFlightCheckpoints_Offering();
	out->map_open = XwaPort_VrMapOpen();
	if (!craft) {
		return;
	}
	out->chord_offered = XwaPort_VrChordEligible(&out->chord_hyper);
	objectType = g_objectTable[(uint16_t)player->objectIndex].objectType;
	out->valid = 1;
	out->laser_level = craft->laserRedirect % 5;
	out->shield_level = craft->shieldRedirect % 5;
	out->beam_level = craft->beamLevel % 5;
	out->has_shields = (craft->systemFlags & 1) != 0;
	out->has_beam = (craft->systemFlags & 0x100) != 0;
	out->shield_balance = craft->shieldDistribMode % 3;
	out->has_sfoils = objectType == OBJ_XWing || objectType == OBJ_BWing;
	out->sfoils_closed = (craft->sFoilState & 2) != 0;
	out->has_target = player->currentTargetObjectIdx != 0xffffu;
	for (int i = 0; i < 3; ++i) {
		const int slot = player->targetPresetSlot[i];
		if (slot >= 0 && (uint32_t)slot < g_objectTableSlotCount && g_objectTable[slot].objectType != OBJ_None) {
			out->memory_set[i] = Player_ObjectInPlayerRegion(g_localPlayer, slot) ? 1 : 2;
		}
	}
	for (unsigned slot = 0; slot < craft->laserSlotCount; ++slot) {
		const WarheadInventoryEntry* weapon = &craft->warheadData[slot];
		if (weapon->weaponType >= 4u) {
			out->has_turrets = 1;
			if (player->turretAutoFireState == 2 && player->currentTargetObjectIdx != 0xffffu &&
				weapon->turretTargetObjIdx == player->currentTargetObjectIdx) {
				out->gunners_on_target = 1;
			}
		}
	}
	out->hyper_ready = player->hyperspacePhase == 0 && (craft->systemFlags & 0x80u) &&
					   (craft->workingSubsystems & 0x80u) &&
					   (player->pendingActionId == 10 || collide_PlayerHyperBuoyRegion(g_localPlayer) >= 0);
}

int XwaPort_VrInFlight(void) { return XwaPort_VrConsoleCraft() != NULL; }

int XwaPort_VrInMap(void) { return Aeron_XrIsPresenting() && XwaPort_VrMapOpen(); }

/* Slow motion while the VR quick wheel is open: the sim through its frame
 * delta, single player only (multiplayer runs in lockstep). The sound
 * effects are pitched down only part of the way (fully to 0.45 would be
 * some 14 semitones: rumble, not sound), the radio voices not at all, and
 * the music stays at speed, quieter (its streams topped up every frame). */
void XwaPort_SetSimTimeScale(float scale) {
	if (!(scale > 0.0f) || scale > 1.0f || g_flightPlayerCount > 1 || !XwaPort_VrInFlight()) {
		scale = 1.0f;
	}
	Time_SetFrameDeltaScale(scale);
	float k = (1.0f - scale) / 0.55f; /* 0 normal .. 1 at 45 % */
	k = k > 1.0f ? 1.0f : k;
	DSoundCompat_SetTimeEffect(1.0f - 0.28f * k, 1.0f - 0.45f * k);
	if (scale < 1.0f) {
		Music_ServiceStreams();
	}
}

/* The two-trigger chord is on offer: landing in a hangar (the game's
 * pending "Space" action 2 near a mothership's bay) or a hyperspace jump
 * (`*hyper`, at a buoy with a working hyperdrive). */
static int XwaPort_VrChordEligible(int* hyper) {
	const CraftData* craft = XwaPort_VrConsoleCraft();
	*hyper = 0;
	if (!craft) {
		return 0;
	}
	const PlayerData* player = &g_players[g_localPlayer];
	if (player->hyperspacePhase != 0) {
		return 0;
	}
	if ((craft->systemFlags & 0x80u) && (craft->workingSubsystems & 0x80u) &&
		(player->pendingActionId == 10 || collide_PlayerHyperBuoyRegion(g_localPlayer) >= 0)) {
		*hyper = 1;
		return 1;
	}
	return player->pendingActionId == 2;
}

/* A line of our own in the in-flight message pane (where "entering area"
 * reports appear), e.g. the voice command confirmations. Text in [brackets]
 * is highlighted like the game's own messages. */
void XwaPort_ShowFlightNote(const char* text) {
	static char note[72];
	if (!text || !text[0] || !XwaPort_VrConsoleCraft()) {
		return;
	}
	snprintf(note, sizeof note, "%s", text);
	g_msgSenderIff = 0;
	msg_addMessagePtr(0, note);
	msg_emitInFlightMessage(MSG_BLANK, g_localPlayer);
}

static void XwaPort_QueueKeyTimes(uint16_t key, int times) {
	for (int i = 0; i < times; ++i) {
		XwaPort_QueueFlightActionKey(key);
	}
}

void XwaPort_VrConsoleAction(XwaVrConsoleAction action, int arg) {
	if (action == XWA_VRC_MAP && XwaPort_VrMapOpen()) {
		/* The console is hidden in the map, but "close map" is still heard. */
		XwaPort_QueueFlightActionKey(KEY_M);
		return;
	}
	const CraftData* craft = XwaPort_VrConsoleCraft();
	if (!craft || (int)action <= XWA_VRC_NONE || (int)action >= XWA_VRC_COUNT) {
		return;
	}
	switch (action) {
		case XWA_VRC_LASER_LEVEL:
			/* The game cycles levels one key press at a time. */
			XwaPort_QueueKeyTimes(KEY_F9, ((arg - craft->laserRedirect) % 5 + 5) % 5);
			return;
		case XWA_VRC_SHIELD_LEVEL:
			XwaPort_QueueKeyTimes(KEY_F10, ((arg - craft->shieldRedirect) % 5 + 5) % 5);
			return;
		case XWA_VRC_BEAM_LEVEL:
			XwaPort_QueueKeyTimes(KEY_F8, ((arg - craft->beamLevel) % 5 + 5) % 5);
			return;
		case XWA_VRC_SHIELD_BALANCE:
			XwaPort_QueueKeyTimes(KEY_S, ((arg - craft->shieldDistribMode) % 3 + 3) % 3);
			return;
		case XWA_VRC_PRESET_RECALL:
			XwaPort_QueueFlightActionKey(arg ? KEY_F12 : KEY_F11);
			return;
		case XWA_VRC_PRESET_STORE:
			XwaPort_QueueFlightActionKey(arg ? KEY_SHIFT_F12 : KEY_SHIFT_F11);
			return;
		case XWA_VRC_MEMORY_RECALL:
			if (arg >= 0 && arg < 3) {
				XwaPort_QueueFlightActionKey((uint16_t)(KEY_F5 + arg));
			}
			return;
		case XWA_VRC_MEMORY_STORE:
			if (arg >= 0 && arg < 3) {
				XwaPort_QueueFlightActionKey((uint16_t)(KEY_SHIFT_F5 + arg));
			}
			return;
		case XWA_VRC_GUNNER_ATTACK: {
			/* The game's F toggles: on the current (hostile) target, or back to
			 * defending the ship when they are already on it. This one only
			 * ever sends them at the target. */
			const PlayerData* player = &g_players[g_localPlayer];
			XwaVrConsoleState st;
			XwaPort_GetVrConsoleState(&st);
			if (!st.has_turrets) {
				XwaPort_ShowFlightNote("[Gunners] This craft has no turrets");
			} else if (player->currentSeatIdx != 0) {
				XwaPort_ShowFlightNote("[Gunners] You are in a gunner seat");
			} else if (player->currentTargetObjectIdx == 0xffffu) {
				XwaPort_ShowFlightNote("[Gunners] Target something first");
			} else if (st.gunners_on_target) {
				XwaPort_ShowFlightNote("[Gunners] Already attacking your target");
			} else {
				XwaPort_QueueFlightActionKey(KEY_F);
			}
			return;
		}
		case XWA_VRC_HYPERSPACE: {
			/* The game offers the jump while the craft is at a hyperspace buoy
			 * ("Hyperspace to ...?") and Space confirms it; the button works for
			 * as long as the craft is there, also after that prompt timed out. */
			PlayerData* player = &g_players[g_localPlayer];
			const int region = player->pendingActionId == 10 ? player->pendingActionParam
															 : collide_PlayerHyperBuoyRegion(g_localPlayer);
			if (!(craft->systemFlags & 0x80u)) {
				XwaPort_ShowFlightNote("[Hyperspace] This craft has no hyperdrive");
			} else if (!(craft->workingSubsystems & 0x80u)) {
				XwaPort_ShowFlightNote("[Hyperspace] Hyperdrive damaged");
			} else if (player->hyperspacePhase != 0) {
				/* Already jumping. */
			} else if (region < 0) {
				XwaPort_ShowFlightNote("[Hyperspace] Fly to a hyperspace buoy first");
			} else {
				if (player->pendingActionId != 10) {
					player->pendingActionId = 10;
					player->pendingActionParam = (int16_t)region;
					player->pendingActionTimer = 708;
				}
				XwaPort_QueueFlightActionKey(KEY_SPACE);
			}
			return;
		}
		default:
			XwaPort_QueueFlightActionKey(XwaVrButtons_ConsoleKey(action));
			return;
	}
}

/* The game's own key hints (strings.txt) rewritten for the Touch
 * controllers, applied once the headset presents. Every control named comes
 * from the bindings in vr_buttons.c, so a changed binding changes these
 * texts too. In flight Space is the quick wheel's Confirm (or the voice
 * command "confirm"), landing and hyperspace also take both triggers; the hangar menus run on the thumbstick, A (Enter),
 * B (back) and X (Space). */
static char g_vrHintText[40][96];
static int g_vrHintCount;

static char* XwaPort_VrHint(const char* fmt, const char* label) {
	if (g_vrHintCount >= (int)(sizeof g_vrHintText / sizeof g_vrHintText[0])) {
		return (char*)"";
	}
	char* out = g_vrHintText[g_vrHintCount++];
	snprintf(out, sizeof g_vrHintText[0], fmt, label);
	return out;
}

static void XwaPort_ApplyVrStringHints(void) {
	/* Written again when the hand setting changes (the hints name the
	 * controls); the same calls refill the same buffers. */
	static int applied = -1;
	const int left_handed = Aeron_XrLeftHanded();
	if (applied == left_handed) {
		return;
	}
	applied = left_handed;
	g_vrHintCount = 0;
	char space[48], space_cap[48], attacker[48], objective[48], exit_map[16], centre[48];
	snprintf(space, sizeof space, "%s", XwaVrButtons_KeyLabel(KEY_SPACE, 0));
	snprintf(space_cap, sizeof space_cap, "%s", space);
	space_cap[0] = (char)toupper((unsigned char)space_cap[0]);
	snprintf(attacker, sizeof attacker, "%s", XwaVrButtons_KeyLabel(KEY_E, 0));
	attacker[0] = (char)toupper((unsigned char)attacker[0]);
	snprintf(objective, sizeof objective, "%s", XwaVrButtons_KeyLabel(KEY_O, 0));
	snprintf(exit_map, sizeof exit_map, "%s", XwaVrButtons_ButtonName(XWA_VR_MAP_EXIT_BUTTON, 0));
	snprintf(centre, sizeof centre, "%s", XwaVrButtons_KeyLabel(KEY_C, 0));
	static const struct {
		int id;
		const char* fmt; /* %s: the control */
		int capital;     /* the control starts a sentence */
	} k_flight[] = {
		{ MSG_ASK_ABOUT_HYPER, "\x03" "Pull [%s] to hyperspace to [*]", -1 },
		{ MSG_HANGAR_TRACTOR, "\x04" "Pull [%s] to activate tractor beam and land", -1 },
		{ MSG_END_MISSION, "\x04" "Press [%s] to end mission", 0 },
		{ MSG_END_MISSION_PENALTY, "\x04" "Leaving now is [2000] point penalty! [%s] to quit anyway", 1 },
		{ MSG_END_MISSION_INCOMPLETE, "\x04" "Leaving early, score incomplete! [%s] to quit anyway", 1 },
		{ MSG_END_MISSION_DISCONNECT, "\x04" "WARNING! [%s] will disconnect from the host!", 1 },
		{ MSG_END_MISSION_ABORT, "\x04" "WARNING! You are the host! [%s] will abort this game!", 1 },
		{ MSG_REINFORCE_CONFIRM, "\x04" "Press [%s] to confirm reinforcement request", 0 },
		{ MSG_USE_MY_TARGET, "\x04" "From [*].  Attack my target!  [%s] to target", 1 },
		{ MSG_IGNORE_MY_TARGET, "\x04" "From [*].  Ignore my target!  [%s] to ignore", 1 },
		{ MSG_HEAD_HOME, "\x04" "From [*].  Head home!  [%s] to comply", 1 },
		{ MSG_WAIT_FOR_ORDERS, "\x04" "From [*].  Wait for orders!  [%s] to wait", 1 },
		{ MSG_GO_AHEAD, "\x04" "From [*].  Go ahead!  [%s] to proceed", 1 },
		{ MSG_EVADE, "\x04" "From [*].  Evade!  [%s] to target attacker", 1 },
		{ MSG_COVER_ME_ATTACKER, "\x04" "From [*].  Cover me!  [%s] to target attacker", 1 },
		{ MSG_COVER_ME, "\x04" "From [*].  Cover me!  [%s] to target me", 1 },
		{ MSG_HANGAR_RESTART, "\x04" "Press [%s] to restart mission", 0 },
		{ MSG_MISSION_PAUSED, "\x03" "Mission paused.  Press the [%s] to continue", -2 },
		{ MSG_MISSILE_WARNING, "\x04" "Missile Warning! [%s] targets attacker", -3 },
	};
	for (size_t i = 0; i < sizeof k_flight / sizeof k_flight[0]; ++i) {
		const char* label = k_flight[i].capital == -1   ? XWA_VR_CHORD_LABEL
							: k_flight[i].capital == -2 ? XWA_VR_PAUSE_LABEL
							: k_flight[i].capital == -3 ? attacker
							: k_flight[i].capital == 1  ? space_cap
														: space;
		if (k_flight[i].id >= 0 && k_flight[i].id < 538) {
			g_strInFlightMessages[k_flight[i].id] = XwaPort_VrHint(k_flight[i].fmt, label);
		}
	}
	g_strPressSpaceBar = XwaPort_VrHint("Press %s to continue", space);
	g_strHangarMiscStrings[HANGAR_MISC_ARROWS_TO_MOVE] =
		XwaPort_VrHint("Use the " XWA_VR_MENU_MOVE " and %s to navigate the menu.", XWA_VR_MENU_SELECT);
	g_strHangarMiscStrings[HANGAR_MISC_NUMBER_KEYS_FOR_CAMERA] = (char*)"Look around: you are in the hangar.";
	g_strHangarMiscStrings[HANGAR_MISC_ZERO_FOR_COCKPIT] = XwaPort_VrHint("Press %s to go back in the menus.", XWA_VR_MENU_BACK);
	g_strHangarMiscStrings[HANGAR_MISC_SPACE_TO_LAUNCH] =
		XwaPort_VrHint("When you are ready, press %s to launch.", XWA_VR_MENU_SPACE);
	/* Proving grounds level 7 / 8 text: "the O key" (nearest objective). */
	g_strProvingGroundDescs[8 * 6 + 1] = XwaPort_VrHint("droid from the smelter room. %s", objective);
	g_strProvingGroundDescs[8 * 7 + 6] = XwaPort_VrHint("finding the droid, use %s to ", objective);
	/* The map legend (MFD): h, *, -, z, m, c, Space; then their texts. Only
	 * what a control reaches in the map stays; the key column is as wide as
	 * row 6's key. */
	for (int row = 0; row < 4; ++row) {
		g_strMapStrings[row] = (char*)"";
		g_strMapStrings[row + 7] = (char*)"";
	}
	g_strMapStrings[4] = XwaPort_VrHint("%s", exit_map);
	g_strMapStrings[5] = XwaPort_VrHint("%s", centre);
	g_strMapStrings[6] = XwaPort_VrHint("%s  ", space);
	Aeron_LogInfo("xwa", "VR: game key hints rewritten for the Touch controllers");
}

/* Mission radio messages ("Press 'T' to target ...") rewritten for the
 * Touch controllers once per loaded mission (a message that still differs
 * from what this last wrote there is new). */
static void XwaPort_ApplyVrMissionMessages(void) {
	static char done[64][sizeof g_missionMessages[0].message];
	char out[160];
	int changed = 0;
	for (int i = 0; i < 64; ++i) {
		char* text = g_missionMessages[i].message;
		if (strcmp(text, done[i]) == 0) {
			continue;
		}
		if (XwaVrButtons_RewriteText(text, out, sizeof out)) {
			snprintf(text, sizeof g_missionMessages[0].message, "%s", out);
			++changed;
		}
		snprintf(done[i], sizeof done[i], "%s", text);
	}
	if (changed) {
		Aeron_LogInfo("xwa", "VR: %d mission messages rewritten for the Touch controllers", changed);
	}
}

/* Picks the VR controller mode for this frame: keyboard-only menus in the
 * hangar, bindable buttons in flight, neither elsewhere (the laser pointer
 * handles the frontend). */
static void XwaPort_UpdateVrModes(void) {
	const int flight = XwaFlightTask_IsActive();
	const int modal  = flight && FlightDisplay_IsFrontendModalActive();
	if (Aeron_XrIsPresenting()) {
		XwaPort_ApplyVrStringHints();
		if (flight) {
			XwaPort_ApplyVrMissionMessages();
		}
	}
	if (!Aeron_XrIsPresenting()) {
		Aeron_XrSetMenuKeys(0);
		XwaPort_UpdateVrFlightButtons(0);
		return;
	}
	Aeron_XrSetMenuKeys(flight && g_inHangarReady && !modal);
	XwaPort_UpdateVrFlightButtons(flight && !g_inHangarReady && !modal && !XwaFlightPauseTask_IsActive());
}

/* The VR controller's B in the 2D menus: one screen back. In a modal (the
 * options datapad, a dialog) it is Escape, their own back / cancel; on a
 * frontend screen it presses that screen's back button. */
static void XwaPort_ServiceVrBack(int modal) {
	if (!Aeron_XrTakeMenuBPressed()) {
		return;
	}
	if (XwaVrButtons_CaptureActive()) {
		return; /* the mapping screen waits for a button: B is one */
	}
	if (modal || FrontendScreen_IsModalActive()) {
		XwaInputBridge_InjectFrontendChar(27);
	} else {
		FrontendScreen_RequestBack();
	}
}

void XwaPort_ToggleMouseCapture(void) {
	g_xwaPortMouseCaptureSuspended = !g_xwaPortMouseCaptureSuspended;
	Aeron_LogInfo("xwa", "%s", g_xwaPortMouseCaptureSuspended ? "mouse released to OS" : "mouse captured");
}

int XwaPort_IsMouseCaptureSuspended(void) { return g_xwaPortMouseCaptureSuspended; }

static void XwaPort_TickBody(int32_t delta_us) {
	const AeronInputSnapshot* input;
	int flightResult;

	if (!g_xwaPortInitialized) {
		return;
	}
	if (XwaControllerMapping_ConsumeSelectionChange()) {
		XwaModernControllerOptionsScreen_ResetCapture();
		Joystick_ReinitializeDevices();
		if (XwaFlightTask_IsActive()) {
			/* Flight normally detects a joystick only during initialization. */
			const uint16_t joystickActive = (uint16_t)Input_DetectActiveJoystick();
			g_joystickDetectResultWord = joystickActive;
			g_joystickEnabled = joystickActive != 0;
		}
		XwaControllerMapping_CopySelectedActions(g_gameConfig.joyButtons);
		ForceFeedback_Reconfigure();
	}
	XwaPort_ApplyClassicFlightRenderingPolicy();

	input = Aeron_InputSnapshot();
	if (input != 0 && input->has_focus) {
		g_xwaPortEverHadFocus = 1;
	}
	XwaPort_UpdateVrModes();

	XwaSnapshot_BeginTick();

	if (XwaMovieTask_IsActive()) {
		Aeron_SetRelativeMouseMode(0);
		XwaPort_SetHostCursorVisible(0);
		input = Aeron_InputSnapshot();
		XwaInputBridge_UpdateFrontend(input);
		(void)Aeron_XrTakeMenuBPressed(); /* no back in the films */
		XwaTime_AdvanceHostClock(delta_us);
		XwaSnapshot_SetSceneKind(XWA_SCENE_CUTSCENE);
		XwaMovieTask_Tick();
		XwaSnapshot_Commit();
		return;
	}

	if (XwaFlightTask_IsActive()) {
		const int wasPaused = XwaFlightPauseTask_IsActive();
		XwaSnapshot_SetSceneKind(XWA_SCENE_FLIGHT);
		input = Aeron_InputSnapshot();
		/* Capture this host frame's keyboard/mouse edges into the DirectInput shim so
		 * buffered key events are not lost on frames the fixed-step flight loop skips. */
		AeronCompat_Update(0);
		/* Same for mouse flight: fold this host frame's mouse deltas into its
		 * accumulator so motion on unsampled frames is not lost. */
		XwaMouseFlight_Pump();
		if (FlightDisplay_IsFrontendModalActive()) {
			Aeron_SetRelativeMouseMode(0);
			/* The modal draws the frontend software cursor; keep the OS one hidden
			 * even if the capture was released by hotkey before it opened. */
			XwaPort_SetHostCursorVisible(0);
			XwaInputBridge_UpdateFrontend(input);
			XwaPort_ServiceVrBack(1);
			XwaTime_AdvanceHostClock(delta_us);
			XwaFrontendTask_ServiceFrameSystems();
			if (!FlightDisplay_PumpFrontendModal()) {
				XwaSnapshot_SetSceneKind(XWA_SCENE_FRONTEND_MODAL);
				FrontendDisplay_ClearPresentFrameReady();
				XwaSnapshot_Commit();
				return;
			}
		} else {
			/* A click inside the window re-captures a released pointer. The shim
			 * already dropped this frame's buttons -- capture was still off when
			 * AeronCompat_Update ran -- so the restoring click is not also delivered
			 * to the flight sim. */
			if (g_xwaPortMouseCaptureSuspended && input != 0 && input->has_focus &&
				input->mouse.inside_content && input->mouse.pressed_buttons != 0u) {
				g_xwaPortMouseCaptureSuspended = 0;
				Aeron_LogInfo("xwa", "mouse captured");
			}
			(void)Aeron_XrTakeMenuBPressed(); /* flight: B is a flight button */
			const int capture = input != 0 && input->has_focus && !g_xwaPortMouseCaptureSuspended;
			Aeron_SetRelativeMouseMode(capture);
			XwaPort_SetHostCursorVisible(!capture);
			/* Flight keyboard/mouse is served through the DirectInput compat shim,
			 * which samples the Aeron snapshot on demand from the recovered DInput_*
			 * calls during the tick -- no bridge feeding needed here. */
			XwaTime_AdvanceHostClock(delta_us);
		}

		XwaPort_SyncVrThrottleLever();
		if (XwaFlightCheckpoints_DeathMenuDue()) {
			/* The craft is lost: the in-flight menu as the death menu,
			 * opened directly (once the wreck is gone the flight no longer
			 * reads the pilot's keys, a queued Escape would be lost). */
			(void)Flight_OpenLocalOptionsModal();
		}
		XwaFlightTask_Tick();
		if (wasPaused) {
			/* Retain the complete paused view, including captured HUD draws.
			 * The resume frame also skips simulation and snapshot publication. */
			return;
		}
		if (XwaFlightTask_IsComplete()) {
			flightResult = XwaFlightTask_Shutdown();
			/* Shutdown ends the flight-only suppression scope. Frontend surface
			   initialization below must populate its GPU flip chain normally. */
			XwaPort_ApplyClassicFlightRenderingPolicy();
			FrontendFlight_CompleteLaunchSession(flightResult);
			/* The original launch callback returns 1 after flight result 2,
			   terminating the frontend main loop. */
			if (flightResult == 2) {
				g_xwaPortShouldQuit = 1;
			}
		} else if (FlightDisplay_IsFrontendModalActive()) {
			/* In-flight frontend modals emit the same fixed 640x480 draw stream
			   as the normal frontend. Route it through the HD frontend
			   reconstruction instead of presenting a flight-resolution surface. */
			XwaSnapshot_SetSceneKind(XWA_SCENE_FRONTEND_MODAL);
			FrontendDisplay_ClearPresentFrameReady();
		} else if (FlightLoading_AreFrontendSurfacesAttached()) {
			/* The loading screen is frontend 2D rendered into surfaces temporarily
			   attached to flight. Its snapshot already contains the sprite/glyph/PRESENT
			   records, so route this tick to the frontend reconstruction and do not expose
			   partially initialized flight state to the HD flight renderer. */
			XwaSnapshot_SetSceneKind(XWA_SCENE_LOADING);
			FrontendDisplay_ClearPresentFrameReady();
		} else {
			XwaSnapshot_CaptureFlight();
		}
		XwaSnapshot_Commit();
		return;
	}

	Aeron_SetRelativeMouseMode(0);
	/* The frontend draws its own software cursor and the capture release does not
	 * carry across a flight session. */
	XwaPort_SetHostCursorVisible(0);
	g_xwaPortMouseCaptureSuspended = 0;
	input = Aeron_InputSnapshot();
	XwaInputBridge_UpdateFrontend(input);
	if (input != 0 && !input->has_focus && g_xwaPortEverHadFocus) {
		g_xwaPortHadFocus = 0;
		XwaFrontendTask_Pause();
		XwaSnapshot_Commit();
		return;
	}

	if (!g_xwaPortHadFocus) {
		FrontendDisplay_SetReactivatedFlag(1);
	}
	g_xwaPortHadFocus = 1;
	XwaTime_AdvanceHostClock(delta_us);
	XwaSnapshot_SetSceneKind(XWA_SCENE_FRONTEND);
	XwaPort_ServiceVrBack(0);
	XwaFrontendTask_Tick();
	if (FrontendFlight_HasPendingLaunch()) {
		FrontendFlight_BeginPendingLaunch();
	}
	XwaSnapshot_Commit();
}

void XwaPort_Tick(int32_t delta_us) {
	XwaMovieTask_ReapFinished();
	const int movieTick = XwaMovieTask_IsActive();
	XwaPort_TickBody(delta_us);
	if (movieTick || XwaMovieTask_IsActive()) {
		return;
	}
	/* Complete the compatibility renderer's host frame after all recovered
	 * presents have been coalesced and before remaster layers are submitted. */
	AeronDx5_EndFrame();
}

void XwaPort_PausedFrame(void) {
	XwaPort_ApplyClassicFlightRenderingPolicy();
	if (XwaFlightTask_IsActive()) {
		AeronCompat_Update(1);
	}
	XwaMovieTask_ReapFinished();
	if (XwaMovieTask_IsActive()) {
		XwaMovieTask_PausedFrame();
		return;
	}
	AeronDx5_EndFrame();
}

void XwaPort_Shutdown(void) {
	Aeron_SetRelativeMouseMode(0);
	XwaPort_SetHostCursorVisible(1);
	XwaModernVideoOptions_Flush();
	XwaModernInputOptions_Flush();
	/* TODO: Run recovered shutdown paths in original-compatible order. */
	if (XwaFlightTask_IsActive()) {
		XwaFlightTask_Shutdown();
	}
	XwaMovieTask_Shutdown();
	Pilot_Save(0);
	XwaFrontendTask_Shutdown();
	FlightDisplay_FreeSurfaces();
	FrontendDisplay_FreeSurfaces();
	AeronDx5_Shutdown();
	XwaPort_FreeStartupAssets();
	g_xwaPortInitialized = 0;
	g_xwaPortShouldQuit = 0;
	g_xwaPortHadFocus = 0;
	g_xwaPortEverHadFocus = 0;
	g_xwaPortClassicFlightRenderingEnabled = 1;
}

int XwaPort_ShouldQuit(void) {
	return g_xwaPortShouldQuit || File_HasFatalError() || XwaFrontendTask_ShouldQuit();
}

int XwaPort_GetExitCode(void) {
	if (File_HasFatalError()) {
		return File_GetFatalExitCode();
	}

	return 0;
}

uint64_t XwaPort_NextWakeDelayUs(void) {
	if (XwaMovieTask_IsActive()) {
		return XwaMovieTask_NextWakeDelayUs();
	}
	if (XwaFlightTask_IsActive()) {
		if (FlightDisplay_IsFrontendModalActive()) {
			return FlightDisplay_GetFrontendModalWakeDelayUs();
		}
		return XwaFlightTask_NextWakeDelayUs();
	}

	if (FrontendFlight_HasPendingLaunch()) {
		return 0;
	}

	return XwaFrontendTask_NextWakeDelayUs();
}
