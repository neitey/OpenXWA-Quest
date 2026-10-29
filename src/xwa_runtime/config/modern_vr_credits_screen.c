#include "xwa_runtime/config/modern_vr_credits_screen.h"

#include "xwa/assets/string_table.h"
#include "xwa/frontend/frontend_color.h"
#include "xwa/frontend/frontend_draw.h"
#include "xwa/frontend/frontend_text.h"
#include "xwa_runtime/config/modern_options_menu.h"

#include <stddef.h>

/* CREDIT_FINE: CREDIT_SMALL in 13 px rows (a block of several lines). */
typedef enum { CREDIT_GAP, CREDIT_HEADING, CREDIT_TEXT, CREDIT_NAME, CREDIT_SMALL, CREDIT_FINE } CreditStyle;

typedef struct {
	CreditStyle style;
	const char* text;
} CreditLine;

static const CreditLine k_credits[] = {
	{ CREDIT_TEXT, "Neitey's XW: Alliance VR is based on the open source project" },
	{ CREDIT_TEXT, "OpenXWA by Adrien Moulin (https://github.com/elyosh/OpenXWA)" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_HEADING, "AI Assisted Coding" },
	{ CREDIT_NAME, "Neitey" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_HEADING, "VR Design, Menu Environments & QA" },
	{ CREDIT_NAME, "MarkD" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_HEADING, "Radial Menu" },
	{ CREDIT_TEXT, "Radial menu code borrowed from Team Beef's JK:Academy VR" },
	{ CREDIT_GAP, NULL },
	/* Optional, off by default (VR Options, "Cockpit Model"). */
	{ CREDIT_HEADING, "Optional Cockpits" },
	{ CREDIT_TEXT, "Raptor007 (X-Wing Revival) and the XWA Upgrade team" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_HEADING, "Disclaimer" },
	{ CREDIT_TEXT, "VR-specific code was generated using Claude AI." },
	{ CREDIT_TEXT, "Additional artwork was created using ChatGPT." },
};

/* Second page. Full licence texts: assets/THIRD-PARTY-NOTICES.txt in the APK
 * (packaging/quest/THIRD-PARTY-NOTICES.txt). */
static const CreditLine k_licenses[] = {
	{ CREDIT_HEADING, "Voice Commands" },
	{ CREDIT_SMALL, "sherpa-onnx by k2-fsa (Next-gen Kaldi) - Apache 2.0" },
	{ CREDIT_SMALL, "Zipformer English model from k2-fsa icefall - Apache 2.0" },
	{ CREDIT_SMALL, "ONNX Runtime by Microsoft - MIT" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_HEADING, "VR Port" },
	{ CREDIT_SMALL, "Basis Universal by Binomial - Apache 2.0" },
	{ CREDIT_SMALL, "meshoptimizer by Arseny Kapoulkine - MIT" },
	{ CREDIT_SMALL, "OpenXR loader by the Khronos Group - Apache 2.0" },
	{ CREDIT_SMALL, "Glove models: Godot XR Tools by DigitalN8m4r3 - CC0" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_HEADING, "Engine" },
	{ CREDIT_SMALL, "SDL3 - zlib    FFmpeg - LGPL 2.1+    Zstandard - BSD" },
	{ CREDIT_SMALL, "astc-encoder by Arm, LLVM libc++ - Apache 2.0" },
	{ CREDIT_SMALL, "cgltf, cJSON, libyaml, stb, Dear ImGui - MIT" },
	{ CREDIT_SMALL, "bc7enc, bcdec, DirectXMath, DirectXTex, FidelityFX - MIT" },
	{ CREDIT_GAP, NULL },
	{ CREDIT_SMALL, "Full licence texts: THIRD-PARTY-NOTICES.txt in the app" },
};

static int s_page;

static void draw_lines(XwaModernOptionsMenu* menu, const CreditLine* lines, size_t count) {
	FrontendRect rect;

	for (size_t i = 0; i < count; ++i) {
		switch (lines[i].style) {
			case CREDIT_GAP:
				menu->y += 10;
				break;
			case CREDIT_HEADING:
				FrontendDraw_RectAssign(&rect, 0, menu->y, 639, menu->y + 15);
				FrontendText_DrawCentered(15, lines[i].text, &rect, g_colorLightBlue);
				menu->y += 20;
				break;
			case CREDIT_NAME:
				FrontendDraw_RectAssign(&rect, 0, menu->y, 639, menu->y + 15);
				FrontendText_DrawCentered(15, lines[i].text, &rect, g_colorPaleCyan);
				menu->y += 20;
				break;
			case CREDIT_SMALL:
			case CREDIT_FINE:
				FrontendDraw_RectAssign(&rect, 0, menu->y, 639, menu->y + 12);
				FrontendText_DrawCentered(12, lines[i].text, &rect, g_colorPaleBlue);
				menu->y += lines[i].style == CREDIT_FINE ? 13 : 15;
				break;
			default:
				FrontendDraw_RectAssign(&rect, 0, menu->y, 639, menu->y + 12);
				FrontendText_DrawCentered(12, lines[i].text, &rect, g_colorPaleBlue);
				menu->y += 17;
				break;
		}
	}
}

int XwaModernVrCreditsScreen_Update(int menu_center_x, int* cursor_row) {
	XwaModernOptionsMenu menu;
	int back;

	if (!cursor_row) {
		return 0;
	}
	/* The credits page is the longer one: it starts at the top, like the
	 * VR options (the menu is clipped at y 435). */
	XwaModernOptionsMenu_Begin(&menu, menu_center_x, s_page == 0 ? 38 : 90, cursor_row, 2);
	if (s_page == 0) {
		XwaModernOptionsMenu_DrawTitle(&menu, "VR Port Credits");
		menu.y += 1;
		draw_lines(&menu, k_credits, sizeof k_credits / sizeof k_credits[0]);
	} else {
		XwaModernOptionsMenu_DrawTitle(&menu, "Open Source Licenses");
		menu.y += 12;
		draw_lines(&menu, k_licenses, sizeof k_licenses / sizeof k_licenses[0]);
	}
	menu.y += s_page == 0 ? 5 : 14;
	if (XwaModernOptionsMenu_DrawAction(&menu, s_page == 0 ? "Open Source Licenses" : "VR Port Credits", 78, 0)) {
		s_page = !s_page;
		*cursor_row = 0;
	}
	back = XwaModernOptionsMenu_DrawAction(&menu, FrontendString_Get(STR_BACK), 87, 0);
	back |= XwaModernOptionsMenu_TakeEscape(&menu);
	if (back) {
		s_page = 0;
	}
	return back;
}
