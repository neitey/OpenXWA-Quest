/* VR Options > Remap Buttons: the player's button mapping
 * (xwa_runtime/runtime/vr_mapping.h), pointer-driven.
 *
 *   - Overview: every button with its function and its Alt function; a
 *     click on either opens the function list for that slot.
 *   - Assign by Function: the function list with where each one sits; a
 *     click waits for a button press ("hold your Alt grip for the Alt
 *     layer"), and a taken button asks once before it is replaced.
 *   - Make Grip act as Alt, Presets, Restore Default Buttons.
 *
 * Every change applies at once (the flight buttons read the live mapping)
 * and is saved to openxwa-quest.cfg. */

#include "xwa_runtime/config/modern_vr_buttons_screen.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "xwa/assets/string_table.h"
#include "xwa/frontend/frontend_button.h"
#include "xwa/frontend/frontend_color.h"
#include "xwa/frontend/frontend_draw.h"
#include "xwa/frontend/frontend_text.h"
#include "xwa_runtime/config/modern_options_menu.h"
#include "xwa_runtime/config/modern_vr_options_screen.h"
#include "xwa_runtime/runtime/vr_buttons.h"
#include "xwa_runtime/runtime/vr_mapping.h"

#include <SDL3/SDL.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

enum { PAGE_OVERVIEW, PAGE_PICK, PAGE_BY_FUNCTION, PAGE_CAPTURE, PAGE_REPLACE, PAGE_PRESETS };

/* Button ids for hover state (g_buttonHoverState has 256). */
enum { ID_SLOT_BASE = 100, ID_FN_BASE = 140, ID_CANCEL = 230, ID_REPLACE = 231 }; /* rows: 234..243 */

#define CAPTURE_TIMEOUT_MS 5000u

static struct {
	int page;
	int slot, alt;         /* PICK: the slot being chosen for */
	int fn;                /* BY_FUNCTION / CAPTURE / REPLACE: the function */
	int captured_slot, captured_alt;
	int armed;             /* CAPTURE: everything was let go once */
	uint32_t prev_pressed; /* CAPTURE: slots pressed last frame */
	uint64_t capture_ms;
	char status[160];
	uint64_t status_ms;
} s_bs;

/* The order on screen: right hand's first, then the left's, then the
 * Frame's extras. */
static const AeronXrButton k_order[] = {
	AERON_XR_BUTTON_A,           AERON_XR_BUTTON_B,          AERON_XR_BUTTON_RIGHT_TRIGGER,
	AERON_XR_BUTTON_RIGHT_GRIP,  AERON_XR_BUTTON_RIGHT_STICK, AERON_XR_BUTTON_X,
	AERON_XR_BUTTON_Y,           AERON_XR_BUTTON_LEFT_TRIGGER, AERON_XR_BUTTON_LEFT_GRIP,
	AERON_XR_BUTTON_LEFT_STICK,  AERON_XR_BUTTON_DPAD_UP,     AERON_XR_BUTTON_DPAD_DOWN,
	AERON_XR_BUTTON_DPAD_LEFT,   AERON_XR_BUTTON_DPAD_RIGHT,  AERON_XR_BUTTON_BUMPER_LEFT,
	AERON_XR_BUTTON_BUMPER_RIGHT,
};
enum { ORDER_COUNT = (int)(sizeof k_order / sizeof k_order[0]) };

static const char* const k_grip_alt_texts[4] = { "Off", "Left Grip", "Right Grip", "Both Grips" };

static void bs_status(const char* fmt, const char* a, const char* b) {
	snprintf(s_bs.status, sizeof s_bs.status, fmt, a ? a : "", b ? b : "");
	s_bs.status_ms = SDL_GetTicks();
	Aeron_LogInfo("xwa.config", "buttons: %s", s_bs.status);
}

/* "A", "Right trigger", "D-pad up" (left-handed: the control that sends it). */
static const char* bs_slot_name(AeronXrButton slot) {
	static char name[2][48];
	static int flip;
	flip ^= 1;
	snprintf(name[flip], sizeof name[0], "%s", XwaVrButtons_ButtonName(slot, 0));
	name[flip][0] = (char)toupper((unsigned char)name[flip][0]);
	return name[flip];
}

static const char* bs_slot_short(AeronXrButton slot) { return XwaVrButtons_ButtonName(slot, 1); }

static void bs_save(void) {
	const char* keys[2 * AERON_XR_BUTTON_COUNT + 1];
	const char* values[2 * AERON_XR_BUTTON_COUNT + 1];
	const int n = XwaVrMap_SettingLines(XwaVrMap_Current(), keys, values, (int)(sizeof keys / sizeof keys[0]));
	XwaModernVrOptions_SaveValues(keys, values, n);
}

void XwaModernVrButtons_RestoreDefaults(void) {
	XwaVrMapping classic;
	XwaVrMap_PresetMapping(XWA_VR_PRESET_CLASSIC, &classic);
	XwaVrMap_Replace(&classic);
	bs_save();
}

static int bs_frame(void) { return Aeron_XrFrameControls(); }

/* ---- Overview ------------------------------------------------------------ */

static void bs_draw_status(int y) {
	if (s_bs.status[0] && SDL_GetTicks() - s_bs.status_ms < 5000u) {
		FrontendRect rect;
		FrontendDraw_RectAssign(&rect, 0, y, 639, y + 12);
		FrontendText_DrawCentered(12, s_bs.status, &rect, g_colorGreen);
	}
}

/* A clickable cell; returns 1 on a click. */
static int bs_cell(int x, int y, const char* text, int id, int color) {
	return FrontendButton_DrawMenuButton(x, y, text, 12, color, id, 0, "settingsound") == 1;
}

static void bs_draw_overview(XwaModernOptionsMenu* menu, int* cursor_row) {
	const XwaVrMapping* m = XwaVrMap_Current();
	const int alt_on = m->grip_alt != XWA_VR_GRIP_ALT_OFF;
	int y = menu->y - 4;
	{
		static const char* const help[2] = {
			"Click a function to change it. The thumbsticks fly, the menu button pauses.",
			"Both triggers: land / hyperspace when offered. A grip on a panel or the stick moves it.",
		};
		for (int i = 0; i < 2; ++i) {
			FrontendRect rect;
			FrontendDraw_RectAssign(&rect, 0, y, 639, y + 12);
			FrontendText_DrawCentered(12, help[i], &rect, g_colorPaleBlue);
			y += 13;
		}
	}
	y += 4;
	FrontendText_Draw(12, "BUTTON", 222 - FrontendText_MeasureWidth("BUTTON", 12), y, g_colorLightBlue);
	FrontendText_Draw(12, "FUNCTION", 236, y, g_colorLightBlue);
	if (alt_on) {
		FrontendText_Draw(12, "WITH ALT HELD", 446, y, g_colorLightBlue);
	}
	y += 16;
	for (int i = 0; i < ORDER_COUNT; ++i) {
		const AeronXrButton slot = k_order[i];
		if (!XwaVrButtons_SlotPresent(slot)) {
			continue; /* the Frame's d-pad and bumpers with Frame controllers */
		}
		const char* name = bs_slot_name(slot);
		FrontendText_Draw(12, name, 222 - FrontendText_MeasureWidth(name, 12), y, g_colorPaleBlue);
		if (XwaVrButtons_IsAltGrip(slot)) {
			FrontendText_Draw(12, "Alt (hold)", 236, y, g_colorGray);
		} else {
			char text[64];
			XwaVrButtons_SlotText(slot, 0, text, sizeof text);
			if (bs_cell(236, y, text[0] ? text : "-", ID_SLOT_BASE + 2 * (int)slot,
						text[0] ? g_colorPaleBlue : g_colorGray)) {
				s_bs.page = PAGE_PICK;
				s_bs.slot = (int)slot;
				s_bs.alt = 0;
				*cursor_row = 0;
			}
			if (alt_on) {
				XwaVrButtons_SlotText(slot, 1, text, sizeof text);
				if (bs_cell(446, y, text[0] ? text : "(same)", ID_SLOT_BASE + 2 * (int)slot + 1,
							text[0] ? g_colorPaleBlue : g_colorGray)) {
					s_bs.page = PAGE_PICK;
					s_bs.slot = (int)slot;
					s_bs.alt = 1;
					*cursor_row = 0;
				}
			}
		}
		y += 14;
	}
	bs_draw_status(y + 2);
	menu->y = y + 18;
}

/* ---- The function list ----------------------------------------------------- */

/* Draws the grouped list in three columns; returns the function clicked (or
 * -1). `mark`: the function to show as current (PICK). `places`: show where
 * each one sits (BY_FUNCTION). */
static int bs_draw_functions(int top, int bottom, int mark, int places) {
	const XwaVrMapping* m = XwaVrMap_Current();
	/* Inside the datapad's frame (about 36 .. 606). */
	static const int col_x[3] = { 38, 230, 422 };
	const int col_w = 184;
	int col = 0, y = top, clicked = -1;
	for (int g = 0; g < XWA_VRF_GROUP_COUNT; ++g) {
		int first = 1;
		for (int f = 0; f < XwaVrMap_FunctionCount(); ++f) {
			const XwaVrFunction* fi = XwaVrMap_FunctionInfo(f);
			if (fi->group != g || (places && f == 0)) {
				continue;
			}
			if (first) {
				if (y + 26 > bottom && col < 2) {
					++col;
					y = top;
				}
				FrontendText_Draw(12, XwaVrMap_GroupTitle(g), col_x[col], y, g_colorLightBlue);
				y += 14;
				first = 0;
			} else if (y + 13 > bottom && col < 2) {
				++col;
				y = top;
			}
			const int x = col_x[col] + 6;
			if (bs_cell(x, y, fi->label, ID_FN_BASE + f, f == mark ? g_colorGreen : g_colorPaleBlue)) {
				clicked = f;
			}
			if (places) {
				char where[96];
				XwaVrMap_FunctionPlaces(m, f, bs_slot_short, bs_frame(), where, sizeof where);
				const int lx = x + FrontendText_MeasureWidth(fi->label, 12) + 6;
				const int room = col_x[col] + col_w - lx;
				if (where[0] && room > 18) {
					/* Shortened to the column. */
					size_t n = strlen(where);
					while (n > 1 && FrontendText_MeasureWidth(where, 12) > room) {
						where[--n] = '\0';
						if (n > 2) {
							where[n - 1] = '.';
						}
					}
					FrontendText_Draw(12, where, lx, y, g_colorGray);
				}
			}
			y += 13;
		}
	}
	return clicked;
}

/* ---- Assign by press ------------------------------------------------------- */

/* The slots pressed now on the physical controllers (game-side roles). */
static uint32_t bs_pressed_slots(void) {
	uint32_t pressed = 0;
	const int lh = Aeron_XrLeftHanded();
	for (int h = 0; h < 2; ++h) {
		AeronXrHand xh;
		if (!Aeron_XrGetHand(h, &xh)) {
			continue;
		}
		const int right = lh ? !h : h; /* the game-side role of this controller */
		if (xh.buttons & AERON_XR_HAND_BUTTON_LOWER) {
			pressed |= 1u << (right ? AERON_XR_BUTTON_A : AERON_XR_BUTTON_X);
		}
		if (xh.buttons & AERON_XR_HAND_BUTTON_UPPER) {
			pressed |= 1u << (right ? AERON_XR_BUTTON_B : AERON_XR_BUTTON_Y);
		}
		if (xh.buttons & AERON_XR_HAND_BUTTON_STICK) {
			pressed |= 1u << (right ? AERON_XR_BUTTON_RIGHT_STICK : AERON_XR_BUTTON_LEFT_STICK);
		}
		if (xh.trigger >= 0.6f) {
			pressed |= 1u << (right ? AERON_XR_BUTTON_RIGHT_TRIGGER : AERON_XR_BUTTON_LEFT_TRIGGER);
		}
		if (xh.grip >= 0.6f) {
			pressed |= 1u << (right ? AERON_XR_BUTTON_RIGHT_GRIP : AERON_XR_BUTTON_LEFT_GRIP);
		}
		/* The Frame's extras are where they are. */
		if (xh.buttons & AERON_XR_HAND_BUTTON_BUMPER) {
			pressed |= 1u << (h ? AERON_XR_BUTTON_BUMPER_RIGHT : AERON_XR_BUTTON_BUMPER_LEFT);
		}
		if (xh.buttons & AERON_XR_HAND_BUTTON_DPAD_UP) {
			pressed |= 1u << AERON_XR_BUTTON_DPAD_UP;
		}
		if (xh.buttons & AERON_XR_HAND_BUTTON_DPAD_DOWN) {
			pressed |= 1u << AERON_XR_BUTTON_DPAD_DOWN;
		}
		if (xh.buttons & AERON_XR_HAND_BUTTON_DPAD_LEFT) {
			pressed |= 1u << AERON_XR_BUTTON_DPAD_LEFT;
		}
		if (xh.buttons & AERON_XR_HAND_BUTTON_DPAD_RIGHT) {
			pressed |= 1u << AERON_XR_BUTTON_DPAD_RIGHT;
		}
	}
	return pressed;
}

static void bs_assign(int slot, int alt, int fn) {
	const XwaVrFunction* f = XwaVrMap_FunctionInfo(fn);
	XwaVrMap_Set((AeronXrButton)slot, alt, fn);
	bs_save();
	char where[64];
	snprintf(where, sizeof where, "%s%s", alt ? "Alt + " : "", bs_slot_name((AeronXrButton)slot));
	if (fn == 0) {
		bs_status("%s is unbound", where, NULL);
	} else {
		bs_status("%s: %s", where, f->label);
	}
}

static void bs_start_capture(int fn) {
	s_bs.page = PAGE_CAPTURE;
	s_bs.fn = fn;
	s_bs.armed = 0;
	s_bs.prev_pressed = 0;
	s_bs.capture_ms = SDL_GetTicks();
	XwaVrButtons_SetCaptureActive(1);
}

static void bs_end_capture(int page) {
	s_bs.page = page;
	XwaVrButtons_SetCaptureActive(0);
}

/* Returns 1 when a button was taken (the page changed). */
static int bs_update_capture(void) {
	const XwaVrMapping* m = XwaVrMap_Current();
	const uint32_t pressed = bs_pressed_slots();
	const uint32_t alt_grips = XwaVrMap_AltGripMask(m);
	XwaVrButtons_SetCaptureActive(1); /* every frame it waits */
	if (!s_bs.armed) {
		/* The trigger that clicked the function must be let go first. */
		s_bs.armed = pressed == 0;
		s_bs.prev_pressed = pressed;
		return 0;
	}
	const uint32_t fresh = pressed & ~s_bs.prev_pressed & ~alt_grips;
	s_bs.prev_pressed = pressed;
	for (int s = 0; s < AERON_XR_BUTTON_COUNT; ++s) {
		if (!(fresh & (1u << s)) || !XwaVrButtons_SlotPresent((AeronXrButton)s)) {
			continue;
		}
		const int alt = (pressed & alt_grips) != 0;
		const int current = m->fn[s][alt];
		XwaVrButtons_SetCaptureActive(0);
		if (current != 0 && current != s_bs.fn) {
			s_bs.captured_slot = s;
			s_bs.captured_alt = alt;
			s_bs.page = PAGE_REPLACE;
		} else {
			bs_assign(s, alt, s_bs.fn);
			s_bs.page = PAGE_BY_FUNCTION;
		}
		return 1;
	}
	return 0;
}

/* ---- The screen ------------------------------------------------------------ */

int XwaModernVrButtonsScreen_Update(int menu_center_x, int* cursor_row) {
	XwaModernOptionsMenu menu;
	FrontendRect rect;
	const XwaVrMapping* m = XwaVrMap_Current();

	if (!cursor_row) {
		return 0;
	}
	switch (s_bs.page) {
		case PAGE_PICK: {
			char title[96];
			snprintf(title, sizeof title, "%s%s", s_bs.alt ? "With Alt held: " : "",
					 bs_slot_name((AeronXrButton)s_bs.slot));
			XwaModernOptionsMenu_Begin(&menu, menu_center_x, 28, cursor_row, 1);
			XwaModernOptionsMenu_DrawTitle(&menu, title);
			const int fn = bs_draw_functions(menu.y, 412, m->fn[s_bs.slot][s_bs.alt], 0);
			menu.y = 416;
			int back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
			back |= XwaModernOptionsMenu_TakeEscape(&menu);
			if (fn >= 0) {
				bs_assign(s_bs.slot, s_bs.alt, fn);
				back = 1;
			}
			if (back) {
				s_bs.page = PAGE_OVERVIEW;
				*cursor_row = 0;
			}
			return 0;
		}
		case PAGE_BY_FUNCTION: {
			XwaModernOptionsMenu_Begin(&menu, menu_center_x, 28, cursor_row, 1);
			XwaModernOptionsMenu_DrawTitle(&menu, "Assign by Function");
			FrontendDraw_RectAssign(&rect, 0, menu.y - 2, 639, menu.y + 10);
			FrontendText_DrawCentered(12, "Click a function, then press the button for it", &rect, g_colorPaleBlue);
			const int fn = bs_draw_functions(menu.y + 14, 400, -1, 1);
			bs_draw_status(402);
			menu.y = 416;
			int back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
			back |= XwaModernOptionsMenu_TakeEscape(&menu);
			if (fn > 0) {
				bs_start_capture(fn);
			} else if (back) {
				s_bs.page = PAGE_OVERVIEW;
				*cursor_row = 0;
			}
			return 0;
		}
		case PAGE_CAPTURE: {
			XwaModernOptionsMenu_Begin(&menu, menu_center_x, 28, cursor_row, 1);
			XwaModernOptionsMenu_DrawTitle(&menu, "Assign by Function");
			(void)XwaModernOptionsMenu_TakeEscape(&menu); /* B is a button to take */
			if (bs_update_capture()) {
				return 0;
			}
			const uint64_t waited = SDL_GetTicks() - s_bs.capture_ms;
			if (waited >= CAPTURE_TIMEOUT_MS) {
				bs_end_capture(PAGE_BY_FUNCTION);
				bs_status("No button pressed: nothing changed", NULL, NULL);
				return 0;
			}
			char line[128];
			snprintf(line, sizeof line, "Press a button for %s", XwaVrMap_FunctionInfo(s_bs.fn)->label);
			FrontendDraw_RectAssign(&rect, 0, 160, 639, 175);
			FrontendText_DrawCentered(15, line, &rect, g_colorGreen);
			FrontendDraw_RectAssign(&rect, 0, 186, 639, 198);
			FrontendText_DrawCentered(12,
									  m->grip_alt != XWA_VR_GRIP_ALT_OFF ? "Hold your Alt grip for the Alt layer"
																		 : "Any button, trigger or grip",
									  &rect, g_colorPaleBlue);
			snprintf(line, sizeof line, "%u", (unsigned)((CAPTURE_TIMEOUT_MS - waited + 999u) / 1000u));
			FrontendDraw_RectAssign(&rect, 0, 210, 639, 222);
			FrontendText_DrawCentered(12, line, &rect, g_colorGray);
			if (!s_bs.armed) {
				FrontendDraw_RectAssign(&rect, 0, 230, 639, 242);
				FrontendText_DrawCentered(12, "(let go of the trigger first)", &rect, g_colorGray);
			}
			return 0;
		}
		case PAGE_REPLACE: {
			const int slot = s_bs.captured_slot, alt = s_bs.captured_alt;
			char line[160];
			XwaModernOptionsMenu_Begin(&menu, menu_center_x, 28, cursor_row, 2);
			XwaModernOptionsMenu_DrawTitle(&menu, "Assign by Function");
			snprintf(line, sizeof line, "%s%s is %s. Replace it with %s?", alt ? "Alt + " : "",
					 bs_slot_name((AeronXrButton)slot), XwaVrMap_FunctionInfo(m->fn[slot][alt])->label,
					 XwaVrMap_FunctionInfo(s_bs.fn)->label);
			FrontendDraw_RectAssign(&rect, 0, 170, 639, 182);
			FrontendText_DrawCentered(12, line, &rect, g_colorPaleBlue);
			menu.y = 214;
			if (XwaModernOptionsMenu_DrawAction(&menu, "Replace", ID_REPLACE, 0)) {
				const char* old = XwaVrMap_FunctionInfo(m->fn[slot][alt])->label;
				char moved[96];
				snprintf(moved, sizeof moved, "%s", old);
				bs_assign(slot, alt, s_bs.fn);
				char status[sizeof s_bs.status];
				snprintf(status, sizeof status, "%.96s (replaced %.48s)", s_bs.status, moved);
				memcpy(s_bs.status, status, sizeof status);
				s_bs.page = PAGE_BY_FUNCTION;
				*cursor_row = 0;
				return 0;
			}
			int back = XwaModernOptionsMenu_DrawAction(&menu, "Cancel", ID_CANCEL, 0);
			back |= XwaModernOptionsMenu_TakeEscape(&menu);
			if (back) {
				s_bs.page = PAGE_BY_FUNCTION;
				*cursor_row = 0;
			}
			return 0;
		}
		case PAGE_PRESETS: {
			XwaModernOptionsMenu_Begin(&menu, menu_center_x, 28, cursor_row, XWA_VR_PRESET_COUNT + 1);
			XwaModernOptionsMenu_DrawTitle(&menu, "Button Presets");
			FrontendDraw_RectAssign(&rect, 0, menu.y, 639, menu.y + 12);
			FrontendText_DrawCentered(12, "A starting point: every button is replaced, change any of them after.",
									  &rect, g_colorPaleBlue);
			menu.y += 26;
			static const char* const about[XWA_VR_PRESET_COUNT] = {
				"The standard layout (grips: quick wheel, countermeasures)",
				"Both grips are Alt: a second function on every button",
				"Targeting and weapons only",
			};
			for (int p = 0; p < XWA_VR_PRESET_COUNT; ++p) {
				if (XwaModernOptionsMenu_DrawAction(&menu, XwaVrMap_PresetName(p), 240 + p, 0)) {
					XwaVrMapping preset;
					XwaVrMap_PresetMapping(p, &preset);
					XwaVrMap_Replace(&preset);
					bs_save();
					bs_status("Preset loaded: %s", XwaVrMap_PresetName(p), NULL);
					s_bs.page = PAGE_OVERVIEW;
					*cursor_row = 0;
					return 0;
				}
				FrontendDraw_RectAssign(&rect, 0, menu.y - 4, 639, menu.y + 8);
				FrontendText_DrawCentered(12, about[p], &rect, g_colorGray);
				menu.y += 14;
			}
			menu.y += 10;
			int back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
			back |= XwaModernOptionsMenu_TakeEscape(&menu);
			if (back) {
				s_bs.page = PAGE_OVERVIEW;
				*cursor_row = 0;
			}
			return 0;
		}
		default:
			break;
	}

	/* Overview. */
	XwaModernOptionsMenu_Begin(&menu, menu_center_x, 28, cursor_row, 5);
	XwaModernOptionsMenu_DrawTitle(&menu, "Remap Buttons");
	bs_draw_overview(&menu, cursor_row);
	if (s_bs.page != PAGE_OVERVIEW) {
		return 0;
	}
	uint8_t grip = (uint8_t)m->grip_alt;
	if (XwaModernOptionsMenu_DrawCycleU8(&menu, &grip, "Make Grip act as Alt", k_grip_alt_texts, 4, 234, 0) &&
		grip != m->grip_alt) {
		XwaVrMap_SetGripAlt(grip);
		bs_save();
		bs_status(grip ? "%s: Alt while held (its own function moves to the list)" : "Grips: their own functions again",
				  k_grip_alt_texts[grip], NULL);
	}
	menu.y -= 4;
	if (XwaModernOptionsMenu_DrawAction(&menu, "Assign by Function...", 235, 0)) {
		s_bs.page = PAGE_BY_FUNCTION;
		*cursor_row = 0;
		return 0;
	}
	menu.y -= 4;
	if (XwaModernOptionsMenu_DrawAction(&menu, "Presets...", 236, 0)) {
		s_bs.page = PAGE_PRESETS;
		*cursor_row = 0;
		return 0;
	}
	menu.y -= 4;
	if (XwaModernOptionsMenu_DrawAction(&menu, "Restore Default Buttons", 237, 0)) {
		XwaModernVrButtons_RestoreDefaults();
		bs_status("Default buttons restored", NULL, NULL);
	}
	menu.y -= 4;
	int back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
	back |= XwaModernOptionsMenu_TakeEscape(&menu);
	if (back) {
		s_bs.page = PAGE_OVERVIEW;
		XwaVrButtons_SetCaptureActive(0);
		return 1;
	}
	return 0;
}

void XwaModernVrButtonsScreen_Reset(void) {
	if (s_bs.page == PAGE_CAPTURE) {
		XwaVrButtons_SetCaptureActive(0);
	}
	s_bs.page = PAGE_OVERVIEW;
}
