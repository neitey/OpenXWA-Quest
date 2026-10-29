#include "xwa_runtime/runtime/vr_buttons.h"
#include <SDL3/SDL_stdinc.h>

#include "xwa/flight/flight.h"
#include "xwa_runtime/runtime/vr_mapping.h"

#include <SDL3/SDL_timer.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VR controller buttons in flight: the player's mapping (vr_mapping.c,
 * VR Options > Remap Buttons). Left-handed controls swap the
 * controllers under it (Aeron_XrLeftHanded). Steering, roll and the
 * throttle lever stay on the virtual gamepad, and so does fire, on whichever
 * buttons the mapping puts it (Aeron_XrSetFireButtons). */

/* The lap console's buttons (and the voice commands, which use them) that
 * are one game key. */
static const uint16_t k_console_keys[XWA_VRC_COUNT] = {
	[XWA_VRC_LASERS_TO_SHIELDS]    = KEY_APOSTROPHE,
	[XWA_VRC_SHIELDS_TO_LASERS]    = KEY_SEMICOLON,
	[XWA_VRC_TARGET_NEAREST_ENEMY] = KEY_R,
	[XWA_VRC_TARGET_ATTACKER]      = KEY_E,
	[XWA_VRC_MATCH_SPEED]          = KEY_ENTER,
	[XWA_VRC_TARGET_FRIENDLY]      = KEY_F1,
	[XWA_VRC_TARGET_NEUTRAL]       = KEY_F2,
	[XWA_VRC_TARGET_ENEMY]         = KEY_F3,
	[XWA_VRC_SUBSYSTEM_PREV]       = KEY_LESS_THAN,
	[XWA_VRC_SUBSYSTEM_NEXT]       = KEY_COMMA,
	[XWA_VRC_TARGET_NEWEST]        = KEY_U,
	[XWA_VRC_TARGET_OBJECTIVE]     = KEY_O,
	[XWA_VRC_TARGET_CLEAR]         = KEY_ALT_C,
	[XWA_VRC_FIRE_LINK]            = KEY_X,
	[XWA_VRC_CONVERGENCE]          = KEY_Z,
	[XWA_VRC_SFOILS]               = KEY_V,
	[XWA_VRC_MAP]                  = KEY_M,
	[XWA_VRC_DOCK]                 = KEY_SHIFT_D,
	[XWA_VRC_PICKUP]               = KEY_SHIFT_P,
	[XWA_VRC_RELEASE]              = KEY_SHIFT_R,
	[XWA_VRC_GUNNER]               = KEY_G,
	[XWA_VRC_AUTO_GUNNER]          = KEY_F,
	[XWA_VRC_COUNTERMEASURE]       = KEY_C,
	[XWA_VRC_EJECT]                = KEY_ALT_E,
	[XWA_VRC_WING_ATTACK]          = KEY_SHIFT_A,
	[XWA_VRC_WING_COVER]           = KEY_SHIFT_C,
	[XWA_VRC_WING_IGNORE]          = KEY_SHIFT_I,
	[XWA_VRC_WING_EVADE]           = KEY_SHIFT_E,
	[XWA_VRC_WING_WAIT]            = KEY_SHIFT_W,
	[XWA_VRC_WING_GO_AHEAD]        = KEY_SHIFT_G,
	[XWA_VRC_WING_HOME]            = KEY_SHIFT_H,
	[XWA_VRC_WING_REPORT]          = KEY_SHIFT_U,
	[XWA_VRC_WING_RESUPPLY]        = KEY_SHIFT_B,
	[XWA_VRC_WING_REINFORCE]       = KEY_SHIFT_S,
	[XWA_VRC_TARGET_NEXT]          = KEY_T,
	[XWA_VRC_TARGET_PREV]          = KEY_Y,
	[XWA_VRC_TARGET_NAV]           = KEY_N,
	[XWA_VRC_TARGET_TARGETS_ATTACKER] = KEY_A,
	[XWA_VRC_TARGET_INCOMING]      = KEY_I,
	[XWA_VRC_CYCLE_WARHEAD]        = KEY_W,
	[XWA_VRC_BEAM_TOGGLE]          = KEY_B,
	[XWA_VRC_CONFIRM]              = KEY_SPACE,
};

/* Keys the console's multi-press buttons send (laser / shield / beam
 * levels, presets, memories) and Space (its Hyperspace button). */
static const uint16_t k_console_extra_keys[] = {
	KEY_F8,       KEY_F9,       KEY_F10,      KEY_F11,       KEY_F12, KEY_SHIFT_F11, KEY_SHIFT_F12,
	KEY_F5,       KEY_F6,       KEY_F7,       KEY_SHIFT_F5,  KEY_SHIFT_F6, KEY_SHIFT_F7, KEY_S,
	KEY_SPACE,
};

/* A function's game key: its own, or its lap console action's. */
static uint16_t fn_key(int fn) {
	const XwaVrFunction* f = XwaVrMap_FunctionInfo(fn);
	if (f->kind == XWA_VRF_KIND_KEY) {
		return f->key;
	}
	return KEY_NONE;
}

/* Whether a slot is there on the controllers in use (the Frame's extras). */
static int slot_present(int s) {
	return !XwaVrMap_SlotInfo((AeronXrButton)s)->frame_only || Aeron_XrFrameControls();
}

const XwaVrButtonBinding* XwaVrButtons_Binding(AeronXrButton button) {
	static XwaVrButtonBinding binding;
	const XwaVrMapping* m = XwaVrMap_Current();
	binding.plain = binding.shifted = KEY_NONE;
	if ((int)button < 0 || (int)button >= AERON_XR_BUTTON_COUNT) {
		return &binding;
	}
	binding.plain = fn_key(XwaVrMap_Effective(m, button, 0));
	if (m->grip_alt != XWA_VR_GRIP_ALT_OFF && m->fn[button][1] && !(XwaVrMap_AltGripMask(m) & (1u << button))) {
		binding.shifted = fn_key(m->fn[button][1]);
	}
	return &binding;
}

int XwaVrButtons_Function(AeronXrButton button, int alt) {
	const XwaVrMapping* m = XwaVrMap_Current();
	if (alt && m->grip_alt == XWA_VR_GRIP_ALT_OFF) {
		return 0;
	}
	if (alt) {
		return (XwaVrMap_AltGripMask(m) & (1u << button)) ? 0 : m->fn[button][1];
	}
	return XwaVrMap_Effective(m, button, 0);
}

int XwaVrButtons_IsAltGrip(AeronXrButton button) {
	return (XwaVrMap_AltGripMask(XwaVrMap_Current()) & (1u << button)) != 0;
}

int XwaVrButtons_SlotHand(AeronXrButton button) {
	const XwaVrSlotInfo* info = XwaVrMap_SlotInfo(button);
	return info->physical || !Aeron_XrLeftHanded() ? info->hand : 1 - info->hand;
}

int XwaVrButtons_SlotPresent(AeronXrButton button) {
	return (int)button >= 0 && (int)button < AERON_XR_BUTTON_COUNT && slot_present((int)button);
}

int XwaVrButtons_AltHeld(void) { return XwaVrMap_AltHeld(XwaVrMap_Current(), Aeron_XrFlightButtonsHeld()); }

void XwaVrButtons_SlotText(AeronXrButton button, int alt, char* out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';
	if ((int)button < 0 || (int)button >= AERON_XR_BUTTON_COUNT) {
		return;
	}
	if (!alt && XwaVrButtons_IsAltGrip(button)) {
		snprintf(out, size, "Hold: Alt");
		return;
	}
	const int fn = XwaVrButtons_Function(button, alt);
	const XwaVrFunction* f = XwaVrMap_FunctionInfo(fn);
	const int grip = button == AERON_XR_BUTTON_LEFT_GRIP || button == AERON_XR_BUTTON_RIGHT_GRIP;
	const char* voice = SDL_getenv("XWA_XR_VOICE");
	switch (f->kind) {
		case XWA_VRF_KIND_NONE:
			return;
		case XWA_VRF_KIND_FIRE:
			snprintf(out, size, "Fire");
			return;
		case XWA_VRF_KIND_WHEEL:
			snprintf(out, size, "%s", grip ? "Hold: quick wheel" : "Quick wheel (open / close)");
			return;
		case XWA_VRF_KIND_VOICE:
			snprintf(out, size, "%s", voice && (voice[0] == 'h' || voice[0] == 'H') ? "Hold: voice command" : "Voice command");
			return;
		default:
			snprintf(out, size, "%s", f->label);
			return;
	}
}

/* ---- The quick wheel and voice on buttons -------------------------------- */

/* The wheel on a button that is not a grip opens with a press and closes
 * with the next (a grip holds it open, as before). */
static struct {
	int open;
	int hand;
} g_wheel_toggle;

void XwaVrButtons_WheelToggle(AeronXrButton button) {
	const int hand = XwaVrButtons_SlotHand(button);
	if (g_wheel_toggle.open && g_wheel_toggle.hand == hand) {
		g_wheel_toggle.open = 0;
	} else {
		g_wheel_toggle.open = 1;
		g_wheel_toggle.hand = hand;
	}
}

void XwaVrButtons_WheelClose(void) { g_wheel_toggle.open = 0; }

int XwaVrButtons_WheelSource(int* hand, int* grip, uint32_t squeezing) {
	const XwaVrMapping* m = XwaVrMap_Current();
	if (g_wheel_toggle.open) {
		*hand = g_wheel_toggle.hand;
		*grip = 0;
		return 1;
	}
	/* A grip with the wheel holds it open; the one squeezed first when both
	 * have it (`squeezing`: bit per physical hand). */
	const int alt = XwaVrButtons_AltHeld();
	int found = -1;
	static const AeronXrButton grips[2] = { AERON_XR_BUTTON_LEFT_GRIP, AERON_XR_BUTTON_RIGHT_GRIP };
	for (int g = 0; g < 2; ++g) {
		if (XwaVrMap_FunctionInfo(XwaVrMap_Effective(m, grips[g], alt))->kind != XWA_VRF_KIND_WHEEL) {
			continue;
		}
		const int h = XwaVrButtons_SlotHand(grips[g]);
		if (found < 0 || (squeezing & (1u << h))) {
			found = h;
		}
	}
	if (found < 0) {
		return 0;
	}
	*hand = found;
	*grip = 1;
	return 1;
}

int XwaVrButtons_VoiceButtons(void) {
	const XwaVrMapping* m = XwaVrMap_Current();
	for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
		for (int alt = 0; alt < 2; ++alt) {
			if (XwaVrMap_FunctionInfo(m->fn[s][alt])->kind == XWA_VRF_KIND_VOICE && slot_present(s)) {
				return 1;
			}
		}
	}
	return 0;
}

int XwaVrButtons_VoiceHeld(int hand) {
	const XwaVrMapping* m = XwaVrMap_Current();
	const uint32_t held = Aeron_XrFlightButtonsHeld();
	const uint32_t slots = XwaVrMap_SlotsOfKind(m, XWA_VRF_KIND_VOICE, held) & held;
	for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
		if ((slots & (1u << s)) && XwaVrButtons_SlotHand((AeronXrButton)s) == hand) {
			return 1;
		}
	}
	return 0;
}

/* ---- Assigning by press (the mapping screen) ----------------------------- */

/* Set every frame the screen waits; it lapses on its own shortly after
 * (so a press that was taken is not also B's "back", and a screen closed
 * mid-wait leaves nothing switched off). */
static uint64_t g_capture_ms;

void XwaVrButtons_SetCaptureActive(int active) { g_capture_ms = active ? SDL_GetTicks() + 1u : 0u; }

int XwaVrButtons_CaptureActive(void) { return g_capture_ms != 0 && SDL_GetTicks() - g_capture_ms < 400u; }

uint16_t XwaVrButtons_ConsoleKey(XwaVrConsoleAction action) {
	return (int)action > XWA_VRC_NONE && (int)action < XWA_VRC_COUNT ? k_console_keys[action] : KEY_NONE;
}

int XwaVrButtons_SticksSwapped(void) {
	const char* value = SDL_getenv("XWA_XR_SWAP_STICKS");
	return value && value[0] == '1';
}

const char* XwaVrButtons_ButtonName(AeronXrButton button, int compact) {
	static const char* const names[AERON_XR_BUTTON_COUNT][2] = {
		[AERON_XR_BUTTON_A]             = { "A", "A" },
		[AERON_XR_BUTTON_B]             = { "B", "B" },
		[AERON_XR_BUTTON_X]             = { "X", "X" },
		[AERON_XR_BUTTON_Y]             = { "Y", "Y" },
		[AERON_XR_BUTTON_LEFT_TRIGGER]  = { "left trigger", "L-trigger" },
		[AERON_XR_BUTTON_LEFT_STICK]    = { "left stick click", "L-stick click" },
		[AERON_XR_BUTTON_RIGHT_STICK]   = { "right stick click", "R-stick click" },
		[AERON_XR_BUTTON_RIGHT_GRIP]    = { "right grip", "R-grip" },
		[AERON_XR_BUTTON_RIGHT_TRIGGER] = { "right trigger", "R-trigger" },
		[AERON_XR_BUTTON_LEFT_GRIP]     = { "left grip", "L-grip" },
		[AERON_XR_BUTTON_DPAD_UP]       = { "d-pad up", "D-pad up" },
		[AERON_XR_BUTTON_DPAD_DOWN]     = { "d-pad down", "D-pad down" },
		[AERON_XR_BUTTON_DPAD_LEFT]     = { "d-pad left", "D-pad left" },
		[AERON_XR_BUTTON_DPAD_RIGHT]    = { "d-pad right", "D-pad right" },
		[AERON_XR_BUTTON_BUMPER_LEFT]   = { "left bumper", "L-bumper" },
		[AERON_XR_BUTTON_BUMPER_RIGHT]  = { "right bumper", "R-bumper" },
	};
	/* Left-handed: the control on the other controller (aeron.h); the
	 * Frame's d-pad and bumpers stay where they are. */
	static const char* const mirrored[AERON_XR_BUTTON_COUNT][2] = {
		[AERON_XR_BUTTON_A]             = { "X", "X" },
		[AERON_XR_BUTTON_B]             = { "Y", "Y" },
		[AERON_XR_BUTTON_X]             = { "A", "A" },
		[AERON_XR_BUTTON_Y]             = { "B", "B" },
		[AERON_XR_BUTTON_LEFT_TRIGGER]  = { "right trigger", "R-trigger" },
		[AERON_XR_BUTTON_LEFT_STICK]    = { "right stick click", "R-stick click" },
		[AERON_XR_BUTTON_RIGHT_STICK]   = { "left stick click", "L-stick click" },
		[AERON_XR_BUTTON_RIGHT_GRIP]    = { "left grip", "L-grip" },
		[AERON_XR_BUTTON_RIGHT_TRIGGER] = { "left trigger", "L-trigger" },
		[AERON_XR_BUTTON_LEFT_GRIP]     = { "right grip", "R-grip" },
		[AERON_XR_BUTTON_DPAD_UP]       = { "d-pad up", "D-pad up" },
		[AERON_XR_BUTTON_DPAD_DOWN]     = { "d-pad down", "D-pad down" },
		[AERON_XR_BUTTON_DPAD_LEFT]     = { "d-pad left", "D-pad left" },
		[AERON_XR_BUTTON_DPAD_RIGHT]    = { "d-pad right", "D-pad right" },
		[AERON_XR_BUTTON_BUMPER_LEFT]   = { "left bumper", "L-bumper" },
		[AERON_XR_BUTTON_BUMPER_RIGHT]  = { "right bumper", "R-bumper" },
	};
	if ((int)button < 0 || (int)button >= AERON_XR_BUTTON_COUNT) {
		return "?";
	}
	return (Aeron_XrLeftHanded() ? mirrored : names)[button][compact ? 1 : 0];
}

static int console_has_key(uint16_t key) {
	for (int a = XWA_VRC_NONE + 1; a < XWA_VRC_COUNT; ++a) {
		if (k_console_keys[a] == key) {
			return 1;
		}
	}
	for (size_t i = 0; i < sizeof k_console_extra_keys / sizeof k_console_extra_keys[0]; ++i) {
		if (k_console_extra_keys[i] == key) {
			return 1;
		}
	}
	return 0;
}

const char* XwaVrButtons_KeyLabel(uint16_t key, int compact) {
	static char label[48];
	if (key == KEY_NONE) {
		return NULL;
	}
	if (key == KEY_SPACE) {
		/* The quick wheel's centre (and the voice command "confirm"). */
		return "wheel > Confirm";
	}
	/* A plain press wins over an Alt one; the Touch buttons over the
	 * Frame's extras. */
	for (int shifted = 0; shifted < 2; ++shifted) {
		for (int frame = 0; frame < 2; ++frame) {
			for (int b = 0; b < AERON_XR_BUTTON_COUNT; ++b) {
				const XwaVrButtonBinding* binding = XwaVrButtons_Binding((AeronXrButton)b);
				if (XwaVrMap_SlotInfo((AeronXrButton)b)->frame_only != frame || !slot_present(b) ||
					(shifted ? binding->shifted : binding->plain) != key) {
					continue;
				}
				const char* name = XwaVrButtons_ButtonName((AeronXrButton)b, compact);
				snprintf(label, sizeof label, "%s%s", shifted ? (compact ? "Alt+" : "Alt + ") : "", name);
				return label;
			}
		}
	}
	return console_has_key(key) ? "console" : NULL;
}

const char* XwaVrButtons_KeyName(uint16_t key) {
	static char name[16];
	switch (key) {
		case KEY_SPACE:
			return "Space";
		case KEY_ENTER:
			return "Enter";
		case KEY_TAB:
			return "Tab";
		case KEY_BACKSPACE:
			return "Backspace";
		default:
			break;
	}
	if (key >= 'a' && key <= 'z') {
		snprintf(name, sizeof name, "%c", (char)toupper(key));
	} else if (key >= 'A' && key <= 'Z') {
		snprintf(name, sizeof name, "Shift+%c", (char)key);
	} else if (key >= KEY_ALT_1 && key <= KEY_ALT_3) {
		snprintf(name, sizeof name, "Alt+%d", key - KEY_ALT_1 + 1);
	} else if (key >= 0x80 && key < 0x80 + 26) {
		snprintf(name, sizeof name, "Alt+%c", (char)('A' + (key - 0x80)));
	} else if (key >= KEY_F1 && key <= KEY_F12) {
		snprintf(name, sizeof name, "F%d", key - KEY_F1 + 1);
	} else if (key >= KEY_SHIFT_F1 && key <= KEY_SHIFT_F12) {
		snprintf(name, sizeof name, "Shift+F%d", key - KEY_SHIFT_F1 + 1);
	} else if (key > 0x20 && key < 0x7f) {
		snprintf(name, sizeof name, "%c", (char)key);
	} else {
		snprintf(name, sizeof name, "#%u", (unsigned)key);
	}
	return name;
}

const char* XwaVrButtons_KeyAction(uint16_t key) {
	switch (key) {
		case KEY_R:
			return "target closest enemy";
		case KEY_O:
			return "nearest objective";
		case KEY_T:
			return "next target";
		case KEY_Y:
			return "previous target";
		case KEY_C:
			return "countermeasures";
		case KEY_ALT_1:
			return "target in sights";
		case KEY_SPACE:
			return "confirm";
		case KEY_ENTER:
			return "match speed";
		case KEY_SHIFT_P:
			return "pick up";
		case KEY_W:
			return "change weapon";
		case KEY_SHIFT_D:
			return "dock / board";
		case KEY_X:
			return "link weapons";
		case KEY_SHIFT_R:
			return "release";
		case KEY_E:
			return "target my attacker";
		case KEY_A:
			return "my target's attacker";
		case KEY_I:
			return "incoming missile";
		case KEY_B:
			return "beam weapon";
		case KEY_APOSTROPHE:
			return "lasers to shields";
		case KEY_SEMICOLON:
			return "shields to lasers";
		case KEY_S:
			return "shield balance";
		case KEY_N:
			return "nav buoy";
		case KEY_M:
			return "map";
		default:
			return NULL;
	}
}

/* ---- Rewriting game text ------------------------------------------------ */

static int starts_with_ci(const char* s, const char* prefix) {
	for (; *prefix; ++s, ++prefix) {
		if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
			return 0;
		}
	}
	return 1;
}

static int is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* A key token at s: a named key, F1-F12, Shift/Alt + a character, or one
 * character. Sets *key, returns its length (0 = none). `single_ok` allows a
 * lone punctuation character (only inside quotes). */
static int parse_token(const char* s, uint16_t* key, int single_ok) {
	static const struct {
		const char* word;
		uint16_t key;
	} named[] = {
		{ "spacebar", KEY_SPACE }, { "space bar", KEY_SPACE }, { "space", KEY_SPACE },  { "enter", KEY_ENTER },
		{ "return", KEY_ENTER },   { "tab", KEY_TAB },         { "backspace", KEY_BACKSPACE },
	};
	for (size_t i = 0; i < sizeof named / sizeof named[0]; ++i) {
		const size_t n = strlen(named[i].word);
		if (starts_with_ci(s, named[i].word) && !is_word(s[n])) {
			*key = named[i].key;
			return (int)n;
		}
	}
	for (int alt = 0; alt < 2; ++alt) {
		const char* mod = alt ? "alt" : "shift";
		const size_t n = strlen(mod);
		if (starts_with_ci(s, mod) && (s[n] == '-' || s[n] == '+')) {
			const char* t = s + n + 1;
			if ((t[0] == 'F' || t[0] == 'f') && isdigit((unsigned char)t[1]) && !alt) {
				const int f = t[2] && isdigit((unsigned char)t[2]) ? (t[1] - '0') * 10 + (t[2] - '0') : t[1] - '0';
				const int len = t[2] && isdigit((unsigned char)t[2]) ? 3 : 2;
				if (f >= 1 && f <= 12 && !is_word(t[len])) {
					*key = (uint16_t)(KEY_SHIFT_F1 + f - 1);
					return (int)(n + 1) + len;
				}
			}
			if (isalpha((unsigned char)t[0]) && !is_word(t[1])) {
				const char c = (char)tolower((unsigned char)t[0]);
				*key = alt ? (uint16_t)(0x80 + (c - 'a')) : (uint16_t)toupper((unsigned char)c);
				return (int)(n + 2);
			}
			return 0;
		}
	}
	if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
		const int two = isdigit((unsigned char)s[2]);
		const int f = two ? (s[1] - '0') * 10 + (s[2] - '0') : s[1] - '0';
		if (f >= 1 && f <= 12 && !is_word(s[two ? 3 : 2])) {
			*key = (uint16_t)(KEY_F1 + f - 1);
			return two ? 3 : 2;
		}
	}
	/* One letter or digit: a capital outside quotes ("a key" is prose). */
	if (isalnum((unsigned char)s[0]) && !is_word(s[1]) &&
		(single_ok || isupper((unsigned char)s[0]) || isdigit((unsigned char)s[0]))) {
		*key = (uint16_t)tolower((unsigned char)s[0]);
		return 1;
	}
	if (single_ok && s[0] > 0x20 && s[0] < 0x7f && s[0] != '\'' && s[0] != '"') {
		*key = (uint16_t)(unsigned char)s[0];
		return 1;
	}
	return 0;
}

/* A key reference at s: "'T'", "'T' key", "T key", "Shift-D", "space bar",
 * "Press T", each optionally after "the ". Returns its length and the key;
 * 0 = none. `after_press`: s follows "press " / "hit ". */
static int parse_reference(const char* s, uint16_t* key, int after_press) {
	int skip = 0;
	if (starts_with_ci(s, "the ")) {
		skip = 4;
	}
	const char* t = s + skip;
	int len = 0;
	if (t[0] == '\'' || t[0] == '"') {
		const int n = parse_token(t + 1, key, 1);
		if (n == 0 || t[1 + n] != t[0]) {
			return 0;
		}
		len = n + 2;
	} else {
		const int n = parse_token(t, key, 0);
		if (n == 0) {
			return 0;
		}
		const int modifier = starts_with_ci(t, "shift") || starts_with_ci(t, "alt");
		const int spacebar = *key == KEY_SPACE && (starts_with_ci(t, "space bar") || starts_with_ci(t, "spacebar"));
		const int keyword = starts_with_ci(t + n, " key") && !is_word(t[n + 4]);
		/* Bare tokens need context: a modifier, the space bar, "<x> key", or
		 * a capital letter / F-key right after "press". */
		const int pressed = after_press && skip == 0 && (isupper((unsigned char)t[0]) || isdigit((unsigned char)t[0])) &&
							(n > 1 || isupper((unsigned char)t[0]));
		if (!modifier && !spacebar && !keyword && !pressed) {
			return 0;
		}
		len = n;
	}
	if (starts_with_ci(t + len, " key") && !is_word(t[len + 4])) {
		len += 4;
	}
	return skip + len;
}

int XwaVrButtons_RewriteText(const char* in, char* out, size_t size) {
	size_t o = 0;
	int changed = 0;
	if (!in || !out || size == 0) {
		return 0;
	}
	for (size_t i = 0; in[i] != '\0';) {
		const int boundary = i == 0 || !is_word(in[i - 1]);
		int after_press = 0;
		if (i >= 6 && starts_with_ci(in + i - 6, "press ")) {
			after_press = 1;
		} else if (i >= 4 && starts_with_ci(in + i - 4, "hit ")) {
			after_press = 1;
		}
		uint16_t key = KEY_NONE;
		const int n = boundary ? parse_reference(in + i, &key, after_press) : 0;
		const char* label = n > 0 ? XwaVrButtons_KeyLabel(key, 1) : NULL;
		if (label) {
			if (strcmp(label, "console") == 0) {
				/* "Press 'V'" -> "Use the console" */
				const char* verbs[2] = { "press ", "hit " };
				for (int v = 0; v < 2; ++v) {
					const size_t vl = strlen(verbs[v]);
					if (o >= vl && starts_with_ci(out + o - vl, verbs[v])) {
						const int capital = isupper((unsigned char)out[o - vl]);
						o -= vl;
						label = capital ? "Use the console" : "use the console";
						break;
					}
				}
			}
			const size_t l = strlen(label);
			if (o + l < size) {
				memcpy(out + o, label, l);
				o += l;
			}
			i += (size_t)n;
			changed = 1;
			continue;
		}
		if (o + 1 < size) {
			out[o++] = in[i];
		}
		++i;
	}
	out[o] = '\0';
	return changed;
}

static const char* const k_voice_wing[] = { "attack my target",    "cover me",       "ignore my target",
											"evade",               "wait",           "go ahead",
											"return to base",      "report in",      "call resupply",
											"call reinforcements", "gunner attack my target" };
static const char* const k_voice_target[] = { "target closest enemy",    "next target",      "previous target",
											  "target my attacker",      "who is attacking my target",
											  "target incoming missile", "target objective", "newest target",
											  "next subsystem",          "clear target",     "nav buoy" };
static const char* const k_voice_ship[] = { "match speed",       "countermeasures",   "change weapon", "beam weapon",
											"s-foils",
											"lasers to shields", "shields to lasers", "link weapons",
											"convergence",       "map",               "dock",
											"pick up",           "release cargo",     "hyperspace",
											"confirm" };

const XwaVrVoiceGroup* XwaVrButtons_VoiceGroups(int* count) {
	static const XwaVrVoiceGroup groups[] = {
		{ "WINGMEN", k_voice_wing, (int)(sizeof k_voice_wing / sizeof k_voice_wing[0]) },
		{ "TARGETING", k_voice_target, (int)(sizeof k_voice_target / sizeof k_voice_target[0]) },
		{ "SHIP", k_voice_ship, (int)(sizeof k_voice_ship / sizeof k_voice_ship[0]) },
	};
	if (count) {
		*count = (int)(sizeof groups / sizeof groups[0]);
	}
	return groups;
}
