/* Unit tests for the VR button mapping (src/xwa_runtime/runtime/vr_mapping.c).
 *
 *   cc -std=c11 -DXWA_VR_MAPPING_TEST -Isrc -Iaeron/include \
 *      tools/vrtest/vr_mapping_test.c src/xwa_runtime/runtime/vr_mapping.c -o /tmp/vr_mapping_test
 *   /tmp/vr_mapping_test
 */
#define _POSIX_C_SOURCE 200809L
#include "xwa_runtime/runtime/vr_mapping.h"

#include "xwa/flight/flight.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			fprintf(stderr, "%s:%d: FAILED %s\n", __FILE__, __LINE__, #cond); \
			++g_failed;                                                 \
		}                                                               \
	} while (0)

static const char* g_env[64][2];
static int g_env_count;

static const char* fake_get(const char* key) {
	for (int i = 0; i < g_env_count; ++i) {
		if (strcmp(g_env[i][0], key) == 0) {
			return g_env[i][1];
		}
	}
	return NULL;
}

static void env(const char* key, const char* value) {
	g_env[g_env_count][0] = key;
	g_env[g_env_count][1] = value;
	++g_env_count;
}

static int fn(const char* name) { return XwaVrMap_FunctionByName(name); }

static const char* slot_name(AeronXrButton s) {
	static const char* const n[AERON_XR_BUTTON_COUNT] = { "A", "B", "X", "Y", "LT", "LS", "RS", "RG",
														  "RT", "LG", "DU", "DD", "DL", "DR", "LB", "RB" };
	return n[s];
}

#define BIT(s) (1u << AERON_XR_BUTTON_##s)

int main(void) {
	XwaVrMapping m;

	/* Names are unique and resolvable; keys map back. */
	for (int i = 0; i < XwaVrMap_FunctionCount(); ++i) {
		CHECK(XwaVrMap_FunctionByName(XwaVrMap_FunctionInfo(i)->name) == i);
	}
	CHECK(fn("no_such_thing") == -1);
	CHECK(fn("none") == 0 && fn("") == 0);
	CHECK(XwaVrMap_FunctionByKey(KEY_T) == fn("target_next"));

	/* No lines: the Classic table, which is the old hard-coded one. */
	g_env_count = 0;
	CHECK(XwaVrMap_Parse(&m, fake_get) == 0);
	CHECK(m.grip_alt == XWA_VR_GRIP_ALT_OFF);
	CHECK(m.fn[AERON_XR_BUTTON_A][0] == fn("target_next"));
	CHECK(m.fn[AERON_XR_BUTTON_B][0] == fn("target_prev"));
	CHECK(m.fn[AERON_XR_BUTTON_X][0] == fn("target_closest_enemy"));
	CHECK(m.fn[AERON_XR_BUTTON_Y][0] == fn("target_my_attacker"));
	CHECK(m.fn[AERON_XR_BUTTON_LEFT_TRIGGER][0] == fn("change_weapon"));
	CHECK(m.fn[AERON_XR_BUTTON_RIGHT_TRIGGER][0] == fn("fire"));
	CHECK(m.fn[AERON_XR_BUTTON_LEFT_STICK][0] == fn("link_weapons"));
	CHECK(m.fn[AERON_XR_BUTTON_RIGHT_STICK][0] == fn("target_in_sights"));
	CHECK(m.fn[AERON_XR_BUTTON_LEFT_GRIP][0] == fn("quick_wheel"));
	CHECK(m.fn[AERON_XR_BUTTON_RIGHT_GRIP][0] == fn("countermeasures"));
	CHECK(m.fn[AERON_XR_BUTTON_BUMPER_LEFT][0] == fn("voice"));
	for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
		CHECK(m.fn[s][1] == 0);
	}
	CHECK(XwaVrMap_SlotsOfKind(&m, XWA_VRF_KIND_FIRE, 0) == BIT(RIGHT_TRIGGER));

	/* Lines override; an unknown name keeps the default and is counted. */
	g_env_count = 0;
	env("XWA_XR_BIND_A", "beam_toggle");
	env("XWA_XR_BIND_B", "launch_torpedo_from_the_toilet");
	env("XWA_XR_BIND_Y_ALT", "shields_to_lasers");
	env("XWA_XR_BIND_RIGHT_TRIGGER", "none");
	env("XWA_XR_BIND_X", "fire");
	env("XWA_XR_GRIP_ALT", "left");
	CHECK(XwaVrMap_Parse(&m, fake_get) == 1);
	CHECK(m.fn[AERON_XR_BUTTON_A][0] == fn("beam_toggle"));
	CHECK(m.fn[AERON_XR_BUTTON_B][0] == fn("target_prev"));
	CHECK(m.fn[AERON_XR_BUTTON_Y][1] == fn("shields_to_lasers"));
	CHECK(m.fn[AERON_XR_BUTTON_RIGHT_TRIGGER][0] == 0);
	CHECK(m.grip_alt == XWA_VR_GRIP_ALT_LEFT);

	/* Fire moved to X: the right trigger fires no more. */
	CHECK(XwaVrMap_SlotsOfKind(&m, XWA_VRF_KIND_FIRE, 0) == BIT(X));

	/* Grip as Alt, every row of the plan's table:
	 * another button with the Alt grip held -> its Alt function, */
	CHECK(XwaVrMap_AltHeld(&m, BIT(LEFT_GRIP)));
	CHECK(XwaVrMap_Effective(&m, AERON_XR_BUTTON_Y, 1) == fn("shields_to_lasers"));
	CHECK(XwaVrMap_Effective(&m, AERON_XR_BUTTON_Y, 0) == fn("target_my_attacker"));
	/* ... an unbound Alt slot does what the button does without Alt, */
	CHECK(XwaVrMap_Effective(&m, AERON_XR_BUTTON_A, 1) == fn("beam_toggle"));
	/* ... the Alt grip on its own -> nothing (no tap, no wheel), */
	CHECK(XwaVrMap_Effective(&m, AERON_XR_BUTTON_LEFT_GRIP, 0) == 0);
	CHECK(XwaVrMap_Effective(&m, AERON_XR_BUTTON_LEFT_GRIP, 1) == 0);
	CHECK(XwaVrMap_SlotsOfKind(&m, XWA_VRF_KIND_WHEEL, BIT(LEFT_GRIP)) == 0);
	/* ... a grip owned by a panel / the stick is not in `held` (aeron), so
	 * no Alt: */
	CHECK(!XwaVrMap_AltHeld(&m, 0));
	/* the right grip is not Alt in "left" mode. */
	CHECK(!XwaVrMap_AltHeld(&m, BIT(RIGHT_GRIP)));
	CHECK(XwaVrMap_Effective(&m, AERON_XR_BUTTON_RIGHT_GRIP, 0) == fn("countermeasures"));

	/* Both grips Alt; Alt functions only while one is held. */
	m.grip_alt = XWA_VR_GRIP_ALT_BOTH;
	CHECK(XwaVrMap_AltGripMask(&m) == (BIT(LEFT_GRIP) | BIT(RIGHT_GRIP)));
	CHECK(XwaVrMap_AltHeld(&m, BIT(RIGHT_GRIP)));
	CHECK(XwaVrMap_Effective(&m, AERON_XR_BUTTON_RIGHT_GRIP, 0) == 0);
	/* Grip Alt off: Alt functions are never used. */
	m.grip_alt = XWA_VR_GRIP_ALT_OFF;
	CHECK(XwaVrMap_Effective(&m, AERON_XR_BUTTON_Y, 1) == fn("target_my_attacker"));

	/* Places, for labels: Alt grips and Frame slots left out as asked. */
	{
		char text[128];
		XwaVrMapping c;
		XwaVrMap_PresetMapping(XWA_VR_PRESET_CLASSIC, &c);
		XwaVrMap_FunctionPlaces(&c, fn("target_closest_enemy"), slot_name, 0, text, sizeof text);
		CHECK(strcmp(text, "X") == 0);
		XwaVrMap_FunctionPlaces(&c, fn("target_closest_enemy"), slot_name, 1, text, sizeof text);
		CHECK(strcmp(text, "X, DD") == 0);
		XwaVrMapping a;
		XwaVrMap_PresetMapping(XWA_VR_PRESET_ALT_BOTH, &a);
		CHECK(a.grip_alt == XWA_VR_GRIP_ALT_BOTH);
		XwaVrMap_FunctionPlaces(&a, fn("countermeasures"), slot_name, 0, text, sizeof text);
		CHECK(strcmp(text, "Alt+RS") == 0);
		XwaVrMap_FunctionPlaces(&a, fn("quick_wheel"), slot_name, 0, text, sizeof text);
		CHECK(strcmp(text, "Alt+LS") == 0);
		/* Fire stays on the right trigger with an Alt grip held. */
		CHECK(XwaVrMap_SlotsOfKind(&a, XWA_VRF_KIND_FIRE, BIT(LEFT_GRIP)) == BIT(RIGHT_TRIGGER));
	}

	/* Settings lines round-trip. */
	{
		const char* keys[64];
		const char* values[64];
		XwaVrMapping a, b;
		XwaVrMap_PresetMapping(XWA_VR_PRESET_ALT_BOTH, &a);
		a.fn[AERON_XR_BUTTON_B][1] = (uint8_t)fn("eject");
		const int n = XwaVrMap_SettingLines(&a, keys, values, 64);
		CHECK(n == 1 + 2 * AERON_XR_BUTTON_COUNT);
		g_env_count = 0;
		for (int i = 0; i < n; ++i) {
			env(keys[i], values[i]);
		}
		CHECK(XwaVrMap_Parse(&b, fake_get) == 0);
		CHECK(memcmp(&a, &b, sizeof a) == 0);
	}

	/* Every preset parses without unknown names. */
	for (int p = 0; p < XWA_VR_PRESET_COUNT; ++p) {
		XwaVrMapping a;
		XwaVrMap_PresetMapping(p, &a);
		CHECK(a.fn[AERON_XR_BUTTON_RIGHT_TRIGGER][0] == fn("fire"));
		CHECK(XwaVrMap_PresetName(p)[0] != '\0');
	}

	if (g_failed) {
		fprintf(stderr, "%d check(s) failed\n", g_failed);
		return 1;
	}
	printf("vr_mapping_test: all checks passed (%d functions)\n", XwaVrMap_FunctionCount());
	return 0;
}
