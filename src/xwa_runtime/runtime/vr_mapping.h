#ifndef XWA_RUNTIME_VR_MAPPING_H
#define XWA_RUNTIME_VR_MAPPING_H

/*
 * The player's VR controller button mapping (VR Options > Controller
 * Buttons): one function per button and layer. Every bindable control
 * (AeronXrButton: A B X Y, both triggers, stick clicks and grips, and the
 * Steam Frame's d-pad and bumpers) has a normal function and an Alt one;
 * the Alt layer is on while a grip set as Alt (XWA_XR_GRIP_ALT) is held.
 *
 * Stored in openxwa-quest.cfg by stable names, never numbers:
 *
 *   XWA_XR_GRIP_ALT=off               # off | left | right | both
 *   XWA_XR_BIND_A=target_next
 *   XWA_XR_BIND_A_ALT=lasers_to_shields
 *
 * A missing line is the built-in default (the Classic layout), an unknown
 * name falls back to it and the log says so. Slots are game-side roles
 * (left-handed controls move them to the other controller with the rest),
 * except the Frame's d-pad and bumpers, which are where they are.
 *
 * Rules (vr_mapping_test.c):
 *   - A grip set as Alt has no function of its own: holding it switches the
 *     other buttons to their Alt functions, nothing more. A grip the game
 *     owns (at a panel, the flight stick) is a grab and no Alt.
 *   - An Alt slot left unbound does what the button does without Alt.
 *   - One slot holds one function; a function may sit on several slots.
 */

#include "aeron/aeron.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XwaVrFnKind {
	XWA_VRF_KIND_NONE = 0,
	XWA_VRF_KIND_KEY,     /* one game key on press */
	XWA_VRF_KIND_CONSOLE, /* a lap console action on press (XwaPort_VrConsoleAction) */
	XWA_VRF_KIND_FIRE,    /* held: fire */
	XWA_VRF_KIND_WHEEL,   /* held: the quick wheel on that button's hand */
	XWA_VRF_KIND_VOICE,   /* voice command (push to talk) */
} XwaVrFnKind;

typedef enum XwaVrFnGroup {
	XWA_VRF_GROUP_CONTROLS = 0,
	XWA_VRF_GROUP_TARGETING,
	XWA_VRF_GROUP_WEAPONS,
	XWA_VRF_GROUP_ENERGY,
	XWA_VRF_GROUP_SHIP,
	XWA_VRF_GROUP_WINGMEN,
	XWA_VRF_GROUP_TURRETS,
	XWA_VRF_GROUP_COUNT
} XwaVrFnGroup;

typedef struct XwaVrFunction {
	const char* name;  /* stable settings name ("target_next") */
	const char* label; /* on screen ("Next target") */
	uint8_t group;     /* XwaVrFnGroup */
	uint8_t kind;      /* XwaVrFnKind */
	uint16_t key;      /* KIND_KEY: the game key */
	uint8_t action;    /* KIND_CONSOLE: XwaVrConsoleAction */
	int8_t arg;
} XwaVrFunction;

/* Function ids index this table; 0 is "unbound". */
int XwaVrMap_FunctionCount(void);
const XwaVrFunction* XwaVrMap_FunctionInfo(int fn);
/* -1 when unknown. "none" and "" are 0. */
int XwaVrMap_FunctionByName(const char* name);
const char* XwaVrMap_GroupTitle(int group);
/* The function that sends this game key (KIND_KEY), 0 when none. */
int XwaVrMap_FunctionByKey(uint16_t key);

/* Slots (AeronXrButton). */
typedef struct XwaVrSlotInfo {
	const char* cfg;      /* "LEFT_TRIGGER": XWA_XR_BIND_<cfg>[_ALT] */
	int hand;             /* game-side hand: 0 left, 1 right */
	int frame_only;       /* Steam Frame d-pad / bumpers */
	int physical;         /* not moved by left-handed controls */
} XwaVrSlotInfo;
const XwaVrSlotInfo* XwaVrMap_SlotInfo(AeronXrButton slot);

enum { XWA_VR_GRIP_ALT_OFF = 0, XWA_VR_GRIP_ALT_LEFT = 1, XWA_VR_GRIP_ALT_RIGHT = 2, XWA_VR_GRIP_ALT_BOTH = 3 };
extern const char* const k_xwa_vr_grip_alt_values[4]; /* "off" "left" "right" "both" */

typedef struct XwaVrMapping {
	uint8_t fn[AERON_XR_BUTTON_COUNT][2]; /* [slot][alt] */
	int grip_alt;                         /* XWA_VR_GRIP_ALT_* */
} XwaVrMapping;

/* The built-in default (Classic) and the presets. */
enum { XWA_VR_PRESET_CLASSIC = 0, XWA_VR_PRESET_ALT_BOTH, XWA_VR_PRESET_MINIMAL, XWA_VR_PRESET_COUNT };
const char* XwaVrMap_PresetName(int preset);
void XwaVrMap_PresetMapping(int preset, XwaVrMapping* out);

/* Reads XWA_XR_BIND_* / XWA_XR_GRIP_ALT through `get` (getenv) over the
 * Classic defaults. Returns the number of names it did not know. */
int XwaVrMap_Parse(XwaVrMapping* out, const char* (*get)(const char* key));

/* The live mapping (read from the environment on first use). */
const XwaVrMapping* XwaVrMap_Current(void);
/* Re-read from the environment (after the settings changed it). */
void XwaVrMap_Reload(void);
/* Changes the live mapping and the environment (not the cfg file; the
 * options screen saves). */
void XwaVrMap_Set(AeronXrButton slot, int alt, int fn);
void XwaVrMap_SetGripAlt(int mode);
void XwaVrMap_Replace(const XwaVrMapping* mapping);

/* Settings lines for `m`: up to 2 * AERON_XR_BUTTON_COUNT + 1 keys
 * (XWA_XR_GRIP_ALT first). Strings stay valid until the next call. */
int XwaVrMap_SettingLines(const XwaVrMapping* m, const char** keys, const char** values, int capacity);

/* Grips set as Alt (1u << AERON_XR_BUTTON_LEFT_GRIP / RIGHT_GRIP). */
uint32_t XwaVrMap_AltGripMask(const XwaVrMapping* m);
/* What `slot` does now: none for a grip set as Alt; with `alt_held` its Alt
 * function, or the normal one where Alt is unbound. */
int XwaVrMap_Effective(const XwaVrMapping* m, AeronXrButton slot, int alt_held);
/* `held` (1u << AeronXrButton): is an Alt grip among them? */
int XwaVrMap_AltHeld(const XwaVrMapping* m, uint32_t held);
/* The slots whose function now is of `kind`, given what is held. */
uint32_t XwaVrMap_SlotsOfKind(const XwaVrMapping* m, int kind, uint32_t held);

/* Where `fn` sits, for labels: "A", "Alt+Y, L-trigger" (names from
 * `slot_name`). Empty when nowhere. */
void XwaVrMap_FunctionPlaces(const XwaVrMapping* m, int fn, const char* (*slot_name)(AeronXrButton),
							 int include_frame, char* out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
