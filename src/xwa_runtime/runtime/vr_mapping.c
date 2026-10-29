#if defined(XWA_VR_MAPPING_TEST) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif
#include "xwa_runtime/runtime/vr_mapping.h"

#include "xwa/flight/flight.h"
#include "xwa_remaster/vr_console_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(XWA_VR_MAPPING_TEST)
#include "aeron/log.h"
#include <SDL3/SDL_stdinc.h>
#define VRMAP_LOG(...) Aeron_LogWarn("xwa.vr", __VA_ARGS__)
#define VRMAP_SETENV(k, v) SDL_setenv_unsafe((k), (v), 1)
#define VRMAP_GETENV(k) SDL_getenv(k)
#else
#define VRMAP_LOG(...) ((void)0)
#define VRMAP_SETENV(k, v) setenv((k), (v), 1)
#define VRMAP_GETENV(k) getenv(k)
#endif

#define FN_KEY(n, l, g, k) { n, l, XWA_VRF_GROUP_##g, XWA_VRF_KIND_KEY, k, 0, 0 }
#define FN_CON(n, l, g, a) { n, l, XWA_VRF_GROUP_##g, XWA_VRF_KIND_CONSOLE, 0, a, 0 }
#define FN_HOLD(n, l, kind) { n, l, XWA_VRF_GROUP_CONTROLS, kind, 0, 0, 0 }

/* Names are stored in the settings: never rename one, only add. */
static const XwaVrFunction k_functions[] = {
	{ "none", "Unbound", XWA_VRF_GROUP_CONTROLS, XWA_VRF_KIND_NONE, 0, 0, 0 },
	FN_HOLD("fire", "Fire (hold)", XWA_VRF_KIND_FIRE),
	FN_HOLD("quick_wheel", "Quick wheel", XWA_VRF_KIND_WHEEL),
	FN_HOLD("voice", "Voice command", XWA_VRF_KIND_VOICE),

	FN_KEY("target_next", "Next target", TARGETING, KEY_T),
	FN_KEY("target_prev", "Previous target", TARGETING, KEY_Y),
	FN_KEY("target_closest_enemy", "Closest enemy", TARGETING, KEY_R),
	FN_KEY("target_my_attacker", "My attacker", TARGETING, KEY_E),
	FN_KEY("target_targets_attacker", "My target's attacker", TARGETING, KEY_A),
	FN_KEY("target_newest", "Newest craft", TARGETING, KEY_U),
	FN_KEY("target_objective", "Mission objective", TARGETING, KEY_O),
	FN_KEY("target_incoming", "Incoming missile", TARGETING, KEY_I),
	FN_KEY("target_friendly", "Friendly craft", TARGETING, KEY_F1),
	FN_KEY("target_neutral", "Neutral craft", TARGETING, KEY_F2),
	FN_KEY("target_enemy", "Enemy craft", TARGETING, KEY_F3),
	FN_KEY("target_nav", "Nav buoy", TARGETING, KEY_N),
	FN_KEY("target_in_sights", "Target in sights", TARGETING, KEY_ALT_1),
	FN_KEY("target_clear", "Clear target", TARGETING, KEY_ALT_C),
	FN_KEY("subsystem_next", "Next subsystem", TARGETING, KEY_COMMA),
	FN_KEY("subsystem_prev", "Previous subsystem", TARGETING, KEY_LESS_THAN),
	FN_KEY("memory_1", "Recall memory 1", TARGETING, KEY_F5),
	FN_KEY("memory_2", "Recall memory 2", TARGETING, KEY_F6),
	FN_KEY("memory_3", "Recall memory 3", TARGETING, KEY_F7),
	FN_KEY("memory_1_store", "Store memory 1", TARGETING, KEY_SHIFT_F5),
	FN_KEY("memory_2_store", "Store memory 2", TARGETING, KEY_SHIFT_F6),
	FN_KEY("memory_3_store", "Store memory 3", TARGETING, KEY_SHIFT_F7),

	FN_KEY("change_weapon", "Change weapon", WEAPONS, KEY_W),
	FN_KEY("link_weapons", "Link weapons", WEAPONS, KEY_X),
	FN_KEY("convergence", "Convergence", WEAPONS, KEY_Z),
	FN_KEY("beam_toggle", "Beam on/off", WEAPONS, KEY_B),
	FN_KEY("countermeasures", "Countermeasures", WEAPONS, KEY_C),

	FN_KEY("lasers_to_shields", "Lasers to shields", ENERGY, KEY_APOSTROPHE),
	FN_KEY("shields_to_lasers", "Shields to lasers", ENERGY, KEY_SEMICOLON),
	FN_KEY("laser_level", "Laser recharge level", ENERGY, KEY_F9),
	FN_KEY("shield_level", "Shield recharge level", ENERGY, KEY_F10),
	FN_KEY("beam_level", "Beam recharge level", ENERGY, KEY_F8),
	FN_KEY("shield_balance", "Shield balance", ENERGY, KEY_S),
	FN_KEY("preset_1", "Energy preset 1", ENERGY, KEY_F11),
	FN_KEY("preset_2", "Energy preset 2", ENERGY, KEY_F12),
	FN_KEY("preset_1_store", "Store energy preset 1", ENERGY, KEY_SHIFT_F11),
	FN_KEY("preset_2_store", "Store energy preset 2", ENERGY, KEY_SHIFT_F12),

	FN_KEY("match_speed", "Match speed", SHIP, KEY_ENTER),
	FN_KEY("sfoils", "S-foils", SHIP, KEY_V),
	FN_KEY("dock", "Dock / board", SHIP, KEY_SHIFT_D),
	FN_KEY("pick_up", "Pick up", SHIP, KEY_SHIFT_P),
	FN_KEY("release", "Release cargo", SHIP, KEY_SHIFT_R),
	FN_CON("hyperspace", "Hyperspace", SHIP, XWA_VRC_HYPERSPACE),
	FN_KEY("confirm", "Confirm (Space)", SHIP, KEY_SPACE),
	FN_KEY("eject", "Eject", SHIP, KEY_ALT_E),
	FN_KEY("map", "Map", SHIP, KEY_M),

	FN_KEY("wing_attack", "Attack my target", WINGMEN, KEY_SHIFT_A),
	FN_KEY("wing_cover", "Cover me", WINGMEN, KEY_SHIFT_C),
	FN_KEY("wing_ignore", "Ignore my target", WINGMEN, KEY_SHIFT_I),
	FN_KEY("wing_evade", "Evade", WINGMEN, KEY_SHIFT_E),
	FN_KEY("wing_wait", "Wait", WINGMEN, KEY_SHIFT_W),
	FN_KEY("wing_go_ahead", "Go ahead", WINGMEN, KEY_SHIFT_G),
	FN_KEY("wing_home", "Return to base", WINGMEN, KEY_SHIFT_H),
	FN_KEY("wing_report", "Report in", WINGMEN, KEY_SHIFT_U),
	FN_KEY("wing_resupply", "Call resupply", WINGMEN, KEY_SHIFT_B),
	FN_KEY("wing_reinforce", "Call reinforcements", WINGMEN, KEY_SHIFT_S),

	FN_CON("gunner_attack", "Gunners: attack target", TURRETS, XWA_VRC_GUNNER_ATTACK),
	FN_KEY("auto_gunner", "Auto gunner on/off", TURRETS, KEY_F),
	FN_KEY("gunner_seat", "Gunner seat", TURRETS, KEY_G),
};

enum { FN_COUNT = (int)(sizeof k_functions / sizeof k_functions[0]) };

int XwaVrMap_FunctionCount(void) { return FN_COUNT; }

const XwaVrFunction* XwaVrMap_FunctionInfo(int fn) { return fn >= 0 && fn < FN_COUNT ? &k_functions[fn] : &k_functions[0]; }

int XwaVrMap_FunctionByName(const char* name) {
	if (!name || !name[0]) {
		return 0;
	}
	for (int i = 0; i < FN_COUNT; ++i) {
		if (strcmp(k_functions[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

int XwaVrMap_FunctionByKey(uint16_t key) {
	for (int i = 1; i < FN_COUNT && key != KEY_NONE; ++i) {
		if (k_functions[i].kind == XWA_VRF_KIND_KEY && k_functions[i].key == key) {
			return i;
		}
	}
	return 0;
}

const char* XwaVrMap_GroupTitle(int group) {
	static const char* const titles[XWA_VRF_GROUP_COUNT] = { "Controls", "Targeting", "Weapons", "Energy",
															 "Ship",     "Wingmen",   "Turrets" };
	return group >= 0 && group < XWA_VRF_GROUP_COUNT ? titles[group] : "";
}

static const XwaVrSlotInfo k_slots[AERON_XR_BUTTON_COUNT] = {
	[AERON_XR_BUTTON_A] = { "A", 1, 0, 0 },
	[AERON_XR_BUTTON_B] = { "B", 1, 0, 0 },
	[AERON_XR_BUTTON_X] = { "X", 0, 0, 0 },
	[AERON_XR_BUTTON_Y] = { "Y", 0, 0, 0 },
	[AERON_XR_BUTTON_LEFT_TRIGGER] = { "LEFT_TRIGGER", 0, 0, 0 },
	[AERON_XR_BUTTON_LEFT_STICK] = { "LEFT_STICK", 0, 0, 0 },
	[AERON_XR_BUTTON_RIGHT_STICK] = { "RIGHT_STICK", 1, 0, 0 },
	[AERON_XR_BUTTON_RIGHT_GRIP] = { "RIGHT_GRIP", 1, 0, 0 },
	[AERON_XR_BUTTON_RIGHT_TRIGGER] = { "RIGHT_TRIGGER", 1, 0, 0 },
	[AERON_XR_BUTTON_LEFT_GRIP] = { "LEFT_GRIP", 0, 0, 0 },
	[AERON_XR_BUTTON_DPAD_UP] = { "DPAD_UP", 0, 1, 1 },
	[AERON_XR_BUTTON_DPAD_DOWN] = { "DPAD_DOWN", 0, 1, 1 },
	[AERON_XR_BUTTON_DPAD_LEFT] = { "DPAD_LEFT", 0, 1, 1 },
	[AERON_XR_BUTTON_DPAD_RIGHT] = { "DPAD_RIGHT", 0, 1, 1 },
	[AERON_XR_BUTTON_BUMPER_LEFT] = { "BUMPER_LEFT", 0, 1, 1 },
	[AERON_XR_BUTTON_BUMPER_RIGHT] = { "BUMPER_RIGHT", 1, 1, 1 },
};

const XwaVrSlotInfo* XwaVrMap_SlotInfo(AeronXrButton slot) {
	static const XwaVrSlotInfo none = { "", 0, 0, 0 };
	return (int)slot >= 0 && (int)slot < AERON_XR_BUTTON_COUNT ? &k_slots[slot] : &none;
}

const char* const k_xwa_vr_grip_alt_values[4] = { "off", "left", "right", "both" };

/* ---- Presets ------------------------------------------------------------ */

/* As plain settings text, so a new preset needs no code. Classic is the
 * built-in default: the layout before the mapping existed, plus the Steam
 * Frame's extra buttons. */
static const char* const k_presets[XWA_VR_PRESET_COUNT][2] = {
	{ "Classic",
	  "XWA_XR_GRIP_ALT=off\n"
	  "XWA_XR_BIND_A=target_next\n"
	  "XWA_XR_BIND_B=target_prev\n"
	  "XWA_XR_BIND_X=target_closest_enemy\n"
	  "XWA_XR_BIND_Y=target_my_attacker\n"
	  "XWA_XR_BIND_LEFT_TRIGGER=change_weapon\n"
	  "XWA_XR_BIND_RIGHT_TRIGGER=fire\n"
	  "XWA_XR_BIND_LEFT_STICK=link_weapons\n"
	  "XWA_XR_BIND_RIGHT_STICK=target_in_sights\n"
	  "XWA_XR_BIND_LEFT_GRIP=quick_wheel\n"
	  "XWA_XR_BIND_RIGHT_GRIP=countermeasures\n"
	  "XWA_XR_BIND_DPAD_UP=target_my_attacker\n"
	  "XWA_XR_BIND_DPAD_DOWN=target_closest_enemy\n"
	  "XWA_XR_BIND_DPAD_LEFT=countermeasures\n"
	  "XWA_XR_BIND_DPAD_RIGHT=match_speed\n"
	  "XWA_XR_BIND_BUMPER_LEFT=voice\n"
	  "XWA_XR_BIND_BUMPER_RIGHT=countermeasures\n" },
	/* Both grips are Alt: the grips' own jobs move to Alt + a stick click. */
	{ "Alt on both grips",
	  "XWA_XR_GRIP_ALT=both\n"
	  "XWA_XR_BIND_A=target_next\n"
	  "XWA_XR_BIND_B=target_prev\n"
	  "XWA_XR_BIND_X=target_closest_enemy\n"
	  "XWA_XR_BIND_Y=target_my_attacker\n"
	  "XWA_XR_BIND_LEFT_TRIGGER=change_weapon\n"
	  "XWA_XR_BIND_RIGHT_TRIGGER=fire\n"
	  "XWA_XR_BIND_LEFT_STICK=link_weapons\n"
	  "XWA_XR_BIND_RIGHT_STICK=target_in_sights\n"
	  "XWA_XR_BIND_A_ALT=target_newest\n"
	  "XWA_XR_BIND_B_ALT=target_objective\n"
	  "XWA_XR_BIND_X_ALT=lasers_to_shields\n"
	  "XWA_XR_BIND_Y_ALT=shields_to_lasers\n"
	  "XWA_XR_BIND_LEFT_TRIGGER_ALT=beam_toggle\n"
	  "XWA_XR_BIND_LEFT_STICK_ALT=quick_wheel\n"
	  "XWA_XR_BIND_RIGHT_STICK_ALT=countermeasures\n"
	  "XWA_XR_BIND_DPAD_UP=target_my_attacker\n"
	  "XWA_XR_BIND_DPAD_DOWN=target_closest_enemy\n"
	  "XWA_XR_BIND_DPAD_LEFT=countermeasures\n"
	  "XWA_XR_BIND_DPAD_RIGHT=match_speed\n"
	  "XWA_XR_BIND_DPAD_UP_ALT=wing_attack\n"
	  "XWA_XR_BIND_DPAD_DOWN_ALT=wing_cover\n"
	  "XWA_XR_BIND_DPAD_LEFT_ALT=target_nav\n"
	  "XWA_XR_BIND_DPAD_RIGHT_ALT=hyperspace\n"
	  "XWA_XR_BIND_BUMPER_LEFT=voice\n"
	  "XWA_XR_BIND_BUMPER_RIGHT=quick_wheel\n" },
	/* Targeting and weapons only. */
	{ "Minimal",
	  "XWA_XR_GRIP_ALT=off\n"
	  "XWA_XR_BIND_A=target_next\n"
	  "XWA_XR_BIND_B=target_prev\n"
	  "XWA_XR_BIND_X=target_closest_enemy\n"
	  "XWA_XR_BIND_Y=target_my_attacker\n"
	  "XWA_XR_BIND_LEFT_TRIGGER=change_weapon\n"
	  "XWA_XR_BIND_RIGHT_TRIGGER=fire\n"
	  "XWA_XR_BIND_LEFT_GRIP=quick_wheel\n"
	  "XWA_XR_BIND_RIGHT_GRIP=countermeasures\n"
	  "XWA_XR_BIND_BUMPER_LEFT=voice\n" },
};

const char* XwaVrMap_PresetName(int preset) {
	return preset >= 0 && preset < XWA_VR_PRESET_COUNT ? k_presets[preset][0] : "";
}

/* KEY=value lines; a slot without a line is unbound. */
static int vrmap_apply_text(XwaVrMapping* m, const char* text) {
	int unknown = 0;
	char line[160];
	for (const char* p = text; p && *p;) {
		const char* end = strchr(p, '\n');
		const size_t n = end ? (size_t)(end - p) : strlen(p);
		snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
		p = end ? end + 1 : p + n;
		char* eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char* key = line;
		const char* value = eq + 1;
		if (strcmp(key, "XWA_XR_GRIP_ALT") == 0) {
			for (int g = 0; g < 4; ++g) {
				if (strcmp(value, k_xwa_vr_grip_alt_values[g]) == 0) {
					m->grip_alt = g;
				}
			}
			continue;
		}
		for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
			for (int alt = 0; alt < 2; ++alt) {
				char want[64];
				snprintf(want, sizeof want, "XWA_XR_BIND_%s%s", k_slots[s].cfg, alt ? "_ALT" : "");
				if (strcmp(key, want) == 0) {
					const int fn = XwaVrMap_FunctionByName(value);
					if (fn < 0) {
						++unknown;
					} else {
						m->fn[s][alt] = (uint8_t)fn;
					}
				}
			}
		}
	}
	return unknown;
}

void XwaVrMap_PresetMapping(int preset, XwaVrMapping* out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof *out);
	if (preset < 0 || preset >= XWA_VR_PRESET_COUNT) {
		preset = XWA_VR_PRESET_CLASSIC;
	}
	vrmap_apply_text(out, k_presets[preset][1]);
}

int XwaVrMap_Parse(XwaVrMapping* out, const char* (*get)(const char* key)) {
	int unknown = 0;
	XwaVrMap_PresetMapping(XWA_VR_PRESET_CLASSIC, out);
	if (!get) {
		return 0;
	}
	const char* grip = get("XWA_XR_GRIP_ALT");
	if (grip && grip[0]) {
		int found = 0;
		for (int g = 0; g < 4; ++g) {
			if (strcmp(grip, k_xwa_vr_grip_alt_values[g]) == 0) {
				out->grip_alt = g;
				found = 1;
			}
		}
		if (!found) {
			VRMAP_LOG("XWA_XR_GRIP_ALT=%s unknown: off", grip);
			++unknown;
		}
	}
	for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
		for (int alt = 0; alt < 2; ++alt) {
			char key[64];
			snprintf(key, sizeof key, "XWA_XR_BIND_%s%s", k_slots[s].cfg, alt ? "_ALT" : "");
			const char* value = get(key);
			if (!value || !value[0]) {
				continue; /* the default */
			}
			const int fn = XwaVrMap_FunctionByName(value);
			if (fn < 0) {
				VRMAP_LOG("%s=%s: no such function, the default stays", key, value);
				++unknown;
				continue;
			}
			out->fn[s][alt] = (uint8_t)fn;
		}
	}
	return unknown;
}

/* ---- The live mapping --------------------------------------------------- */

static XwaVrMapping g_map;
static int g_map_loaded;

static const char* vrmap_getenv(const char* key) { return VRMAP_GETENV(key); }

void XwaVrMap_Reload(void) {
	XwaVrMap_Parse(&g_map, vrmap_getenv);
	g_map_loaded = 1;
}

const XwaVrMapping* XwaVrMap_Current(void) {
	if (!g_map_loaded) {
		XwaVrMap_Reload();
	}
	return &g_map;
}

void XwaVrMap_Set(AeronXrButton slot, int alt, int fn) {
	char key[64];
	if ((int)slot < 0 || (int)slot >= AERON_XR_BUTTON_COUNT || fn < 0 || fn >= FN_COUNT) {
		return;
	}
	(void)XwaVrMap_Current();
	alt = alt != 0;
	g_map.fn[slot][alt] = (uint8_t)fn;
	snprintf(key, sizeof key, "XWA_XR_BIND_%s%s", k_slots[slot].cfg, alt ? "_ALT" : "");
	VRMAP_SETENV(key, k_functions[fn].name);
}

void XwaVrMap_SetGripAlt(int mode) {
	if (mode < 0 || mode > 3) {
		return;
	}
	(void)XwaVrMap_Current();
	g_map.grip_alt = mode;
	VRMAP_SETENV("XWA_XR_GRIP_ALT", k_xwa_vr_grip_alt_values[mode]);
}

void XwaVrMap_Replace(const XwaVrMapping* mapping) {
	if (!mapping) {
		return;
	}
	XwaVrMap_SetGripAlt(mapping->grip_alt);
	for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
		XwaVrMap_Set((AeronXrButton)s, 0, mapping->fn[s][0]);
		XwaVrMap_Set((AeronXrButton)s, 1, mapping->fn[s][1]);
	}
}

int XwaVrMap_SettingLines(const XwaVrMapping* m, const char** keys, const char** values, int capacity) {
	static char key_text[2 * AERON_XR_BUTTON_COUNT][48];
	int n = 0;
	if (!m || !keys || !values) {
		return 0;
	}
	if (n < capacity) {
		keys[n] = "XWA_XR_GRIP_ALT";
		values[n] = k_xwa_vr_grip_alt_values[m->grip_alt & 3];
		++n;
	}
	for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
		for (int alt = 0; alt < 2 && n < capacity; ++alt) {
			char* k = key_text[s * 2 + alt];
			snprintf(k, sizeof key_text[0], "XWA_XR_BIND_%s%s", k_slots[s].cfg, alt ? "_ALT" : "");
			keys[n] = k;
			values[n] = XwaVrMap_FunctionInfo(m->fn[s][alt])->name;
			++n;
		}
	}
	return n;
}

/* ---- Rules -------------------------------------------------------------- */

uint32_t XwaVrMap_AltGripMask(const XwaVrMapping* m) {
	uint32_t mask = 0;
	if (m && (m->grip_alt & XWA_VR_GRIP_ALT_LEFT)) {
		mask |= 1u << AERON_XR_BUTTON_LEFT_GRIP;
	}
	if (m && (m->grip_alt & XWA_VR_GRIP_ALT_RIGHT)) {
		mask |= 1u << AERON_XR_BUTTON_RIGHT_GRIP;
	}
	return mask;
}

int XwaVrMap_Effective(const XwaVrMapping* m, AeronXrButton slot, int alt_held) {
	if (!m || (int)slot < 0 || (int)slot >= AERON_XR_BUTTON_COUNT) {
		return 0;
	}
	if (XwaVrMap_AltGripMask(m) & (1u << slot)) {
		return 0; /* a modifier, nothing else */
	}
	if (alt_held && m->grip_alt != XWA_VR_GRIP_ALT_OFF && m->fn[slot][1] != 0) {
		return m->fn[slot][1];
	}
	return m->fn[slot][0];
}

int XwaVrMap_AltHeld(const XwaVrMapping* m, uint32_t held) { return (held & XwaVrMap_AltGripMask(m)) != 0; }

uint32_t XwaVrMap_SlotsOfKind(const XwaVrMapping* m, int kind, uint32_t held) {
	const int alt = XwaVrMap_AltHeld(m, held);
	uint32_t slots = 0;
	for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
		if (XwaVrMap_FunctionInfo(XwaVrMap_Effective(m, (AeronXrButton)s, alt))->kind == kind) {
			slots |= 1u << s;
		}
	}
	return slots;
}

void XwaVrMap_FunctionPlaces(const XwaVrMapping* m, int fn, const char* (*slot_name)(AeronXrButton),
							 int include_frame, char* out, size_t size) {
	size_t o = 0;
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';
	if (!m || fn <= 0 || !slot_name) {
		return;
	}
	const uint32_t alt_grips = XwaVrMap_AltGripMask(m);
	for (int alt = 0; alt < 2; ++alt) {
		if (alt && m->grip_alt == XWA_VR_GRIP_ALT_OFF) {
			break;
		}
		for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
			if (m->fn[s][alt] != fn || (alt_grips & (1u << s)) || (k_slots[s].frame_only && !include_frame)) {
				continue;
			}
			const int w = snprintf(out + o, size - o, "%s%s%s", o ? ", " : "", alt ? "Alt+" : "", slot_name((AeronXrButton)s));
			if (w < 0 || (size_t)w >= size - o) {
				out[size - 1] = '\0';
				return;
			}
			o += (size_t)w;
		}
	}
}
