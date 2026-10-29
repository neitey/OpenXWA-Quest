#ifndef XWA_RUNTIME_VR_BUTTONS_H
#define XWA_RUNTIME_VR_BUTTONS_H

/*
 * The VR (Quest Touch) controls and the labels every on-screen hint takes
 * from them: the one place to change a binding. The flight button table,
 * the lap console's keys, and the names below feed
 *
 *   - the game's own key hints (strings.txt), rewritten in port.c
 *     (XwaPort_ApplyVrStringHints),
 *   - mission radio messages ("Press 'T' to target ..."), rewritten as they
 *     are shown (XwaVrButtons_RewriteText),
 *   - the map legend and the map's exit hint (xwa_remaster/flight_map.c),
 *   - the controls card (xwa_remaster/vr_lap_console.c).
 *
 * The hangar menus run on Aeron's menu-keys mode (aeron/src/xr_input.c,
 * Aeron_XrApplyMenuKeys); its labels are the XWA_VR_MENU_* names below and
 * must follow that code.
 */

#include "aeron/aeron.h"
#include "xwa_remaster/vr_console_protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flight: a button's game key from the player's mapping (vr_mapping.h):
 * `plain` without Alt, `shifted` with an Alt grip held (KEY_NONE where that
 * function is not one game key: fire, the quick wheel, voice, the lap
 * console's own actions, or unbound). Static storage, valid until the next
 * call. */
typedef struct XwaVrButtonBinding {
	uint16_t plain;
	uint16_t shifted;
} XwaVrButtonBinding;

const XwaVrButtonBinding* XwaVrButtons_Binding(AeronXrButton button);
/* The function (vr_mapping.h id) on a button: without Alt (0 for a grip set
 * as Alt) or its Alt one (0 when unbound or Alt is off). */
int XwaVrButtons_Function(AeronXrButton button, int alt);
int XwaVrButtons_IsAltGrip(AeronXrButton button);
/* What a button does, for labels: "Next target", "Fire", "Hold: quick
 * wheel", "Hold: Alt" (a grip set as Alt); `alt` its Alt function. Empty
 * when nothing. */
void XwaVrButtons_SlotText(AeronXrButton button, int alt, char* out, size_t size);
/* An Alt grip held now (game-owned grips are a grab, not Alt). */
int XwaVrButtons_AltHeld(void);
/* The physical controller (0 left, 1 right) a button is on now. */
int XwaVrButtons_SlotHand(AeronXrButton button);
/* The button exists on the controllers in use (the Steam Frame's d-pad and
 * bumpers only with Frame controllers). */
int XwaVrButtons_SlotPresent(AeronXrButton button);

/* The quick wheel on a button: a grip holds it open; any other button opens
 * it with a press and closes it with the next (WheelToggle, from the flight
 * buttons). WheelSource: where it is wanted now - `hand` physical, `grip`
 * 1 when held open by that hand's grip (the caller reads the grip);
 * `squeezing` has bit h set while grip h is squeezed. 0 = nowhere. */
void XwaVrButtons_WheelToggle(AeronXrButton button);
void XwaVrButtons_WheelClose(void);
int XwaVrButtons_WheelSource(int* hand, int* grip, uint32_t squeezing);
/* Voice commands on a button: whether any button has it, and whether one
 * on physical controller `hand` is held now. */
int XwaVrButtons_VoiceButtons(void);
int XwaVrButtons_VoiceHeld(int hand);
/* While the mapping screen waits for a button press (set every frame; it
 * lapses 0.4 s after the last): the menus' own uses of B, X and Y (back,
 * screen away, scene) stay off. */
void XwaVrButtons_SetCaptureActive(int active);
int XwaVrButtons_CaptureActive(void);

/* B (plain) leaves the map instead of its flight action. */
#define XWA_VR_MAP_EXIT_BUTTON AERON_XR_BUTTON_B

/* Hangar menus (Aeron's menu-keys mode); the face buttons follow the
 * left-handed swap like every other name here. */
#define XWA_VR_MENU_MOVE "THUMBSTICK"
#define XWA_VR_MENU_SELECT XwaVrButtons_ButtonName(AERON_XR_BUTTON_A, 0) /* Enter (also the right trigger) */
#define XWA_VR_MENU_BACK XwaVrButtons_ButtonName(AERON_XR_BUTTON_B, 0)   /* Escape */
#define XWA_VR_MENU_SPACE XwaVrButtons_ButtonName(AERON_XR_BUTTON_X, 0)  /* Space */
/* Both triggers at once: land at a hangar bay, jump at a hyperspace buoy. */
#define XWA_VR_CHORD_LABEL "both triggers"
/* The left menu button: pause. */
#define XWA_VR_PAUSE_LABEL "menu button"
/* What the thumbsticks do in flight (controls card, help page). */
#define XWA_VR_STICK_STEER "Yaw / pitch"
#define XWA_VR_STICK_LEVER "Throttle (fwd/back) - roll"
/* Swapped thumbsticks (XWA_XR_SWAP_STICKS=1, VR Options "Swap Thumbsticks";
 * read every call): in flight the two thumbsticks trade what they do - the
 * left one steers, the right one rolls and moves the throttle lever - on
 * top of left-handed controls. Only the stick axes: the stick clicks and
 * every other button stay where they are. Aeron does the swap
 * (Aeron_XrSetSwapSticks, set by port.c while the flight buttons are on). */
int XwaVrButtons_SticksSwapped(void);

/* The lap console's (and voice commands') game key for `action`;
 * KEY_NONE for the ones that are not a single key. */
uint16_t XwaVrButtons_ConsoleKey(XwaVrConsoleAction action);

/* A button's name: "B", "left trigger", "right stick click", "flick
 * right" (right stick flick). `compact` gives the short form ("L-trigger",
 * "R-stick click") for radio messages. With left-handed controls
 * (Aeron_XrLeftHanded) it names the physical control that now sends it:
 * B is "Y", the left trigger "right trigger", and so on. */
const char* XwaVrButtons_ButtonName(AeronXrButton button, int compact);

/* The control that sends `key` in flight, e.g. "B" or "Alt + B" ("Alt+B"
 * when compact); "console" when only the lap console / voice has it; NULL
 * when nothing does. Static storage, valid until the next call. */
const char* XwaVrButtons_KeyLabel(uint16_t key, int compact);

/* The keyboard key's own name: "T", "Space", "Enter", "Shift+P". */
const char* XwaVrButtons_KeyName(uint16_t key);

/* What a key does in flight, for the controls card ("next target"); NULL
 * when it has no short description. */
const char* XwaVrButtons_KeyAction(uint16_t key);

/* The voice command list, by group (the voice card in the cockpit and the
 * VR Options page show it). Keep in step with the command table in
 * xwa_app/voice_core.c: one phrase per command; close wordings work too. */
typedef struct XwaVrVoiceGroup {
	const char* title;
	const char* const* phrases;
	int count;
} XwaVrVoiceGroup;
const XwaVrVoiceGroup* XwaVrButtons_VoiceGroups(int* count);

/* Rewrites the keyboard references in a game text for the Touch
 * controllers ("Press 'T'" -> "Press B", "the space bar" -> "grip+X",
 * "(Shift-D)" -> "(grip+L-trigger)"). Keys no VR control sends are left as
 * they are. Returns 1 when something changed (out holds the result, always
 * terminated). */
int XwaVrButtons_RewriteText(const char* in, char* out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
