#include "vr_keyboard.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/render.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/ui.h"
#include "aeron/vfs.h"
#include "xwa/frontend/frontend_text.h"
#include "xwa_runtime/input/input_bridge.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define VRK_FONT_BASENAME "fonts/setup"
#define VRK_FONT_MAX_SIZE (4u * 1024u * 1024u)
/* Host frames the keyboard stays up after the last frame that drew a text
 * field (the frontend redraws live fields every frame). */
#define VRK_LINGER_FRAMES 20

typedef struct VrKeyboard {
	int             init_failed;
	AeronFontAtlas  atlas;
	AeronUiContext* ui;
	unsigned int    last_serial;
	int             linger;
	int             shift;
	int             visible;
	int             mouse_captured; /* a press started on the keyboard */
	int             frame_open;
	int             dismissed; /* closed by the player while this field is up */
} VrKeyboard;

static VrKeyboard g_vrk;

static int vrk_mode(void) {
	const char* env = SDL_getenv("XWA_VR_KEYBOARD");
	if (env && env[0] == '0') {
		return 0;
	}
	if (env && env[0] == '1') {
		return 1;
	}
	return Aeron_XrIsPresenting();
}

static int vrk_init(void) {
	if (g_vrk.ui) {
		return 1;
	}
	if (g_vrk.init_failed) {
		return 0;
	}
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		return 0;
	}
	if (!AeronFontAtlas_LoadVfs(&g_vrk.atlas, cmd, Aeron_GetVfs(), AERON_VFS_ROOT_RESOURCE, VRK_FONT_BASENAME,
								VRK_FONT_MAX_SIZE)) {
		Aeron_CancelCommandBuffer(cmd);
		Aeron_LogError("xwa.vrkeyboard", "could not load the keyboard font");
		g_vrk.init_failed = 1;
		return 0;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		AeronFontAtlas_Release(&g_vrk.atlas);
		g_vrk.init_failed = 1;
		return 0;
	}
	g_vrk.ui = AeronUi_Create(NULL);
	if (!g_vrk.ui) {
		AeronFontAtlas_Release(&g_vrk.atlas);
		g_vrk.init_failed = 1;
		return 0;
	}
	AeronUi_SetFonts(g_vrk.ui, &(AeronUiFontSet) { .regular = &g_vrk.atlas });
	{
		AeronUiTheme theme = *AeronUi_GetTheme(g_vrk.ui);
		theme.text_px *= 1.4f;
		theme.title_px *= 1.2f;
		theme.row_height *= 1.3f;
		theme.column_spacing *= 0.6f;
		theme.open_fade_s = 0.0f;
		AeronUi_SetTheme(g_vrk.ui, &theme);
	}
	g_vrk.shift = 1; /* names start with a capital */
	return 1;
}

static void vrk_type(unsigned char ch) {
	XwaInputBridge_InjectFrontendChar(ch);
	if (ch >= 'A' && ch <= 'Z') {
		g_vrk.shift = 0; /* one capital, then lower case */
	}
}

/* One row of letter keys. */
static void vrk_row(const char* keys, int extra_columns, const char* extra_label, unsigned char extra_char) {
	const int n = (int)strlen(keys);
	AeronUi_BeginColumns(g_vrk.ui, n + extra_columns, NULL);
	for (int i = 0; i < n; ++i) {
		char label[2] = { keys[i], 0 };
		if (g_vrk.shift == 0 && label[0] >= 'A' && label[0] <= 'Z') {
			label[0] = (char)(label[0] - 'A' + 'a');
		}
		if (i > 0) {
			AeronUi_NextColumn(g_vrk.ui);
		}
		AeronUi_PushId(g_vrk.ui, (int)(unsigned char)keys[i]);
		if (AeronUi_Button(g_vrk.ui, label)) {
			vrk_type((unsigned char)label[0]);
		}
		AeronUi_PopId(g_vrk.ui);
	}
	if (extra_columns) {
		AeronUi_NextColumn(g_vrk.ui);
		if (AeronUi_Button(g_vrk.ui, extra_label)) {
			vrk_type(extra_char);
		}
	}
	AeronUi_EndColumns(g_vrk.ui);
}

void XwaVrKeyboard_BeginFrame(int32_t delta_us) {
	const AeronInputSnapshot* in = Aeron_InputSnapshot();
	g_vrk.frame_open             = 0;
	if (!in || !vrk_mode()) {
		g_vrk.visible = 0;
		return;
	}
	if (g_frontendEditFieldSerial != g_vrk.last_serial) {
		g_vrk.last_serial = g_frontendEditFieldSerial;
		g_vrk.linger      = VRK_LINGER_FRAMES;
	} else if (g_vrk.linger > 0) {
		--g_vrk.linger;
	}
	g_vrk.visible = g_vrk.linger > 0;
	if (!g_vrk.visible) {
		g_vrk.mouse_captured = 0;
		g_vrk.dismissed      = 0; /* the next text field opens it again */
		return;
	}
	if (!vrk_init()) {
		return;
	}

	/* The UI sees mouse and keyboard only: the VR trigger is both a mouse
	 * click (laser pointer) and gamepad South, which would type twice. */
	static AeronInputSnapshot ui_input;
	ui_input = *in;
	memset(ui_input.controllers, 0, sizeof ui_input.controllers);

	AeronUi_BeginFrame(g_vrk.ui, &(AeronUiFrameDesc) { .input = &ui_input, .dt_seconds = (float)delta_us * 1e-6f });
	/* The UI's reference space is 1080 px high; its width follows the game
	 * screen, which is 4:3 in VR (1440 wide), so fit the keyboard into it
	 * instead of assuming 1920. */
	float view_w_ref = 1920.0f;
	{
		AeronRectI rect;
		if (Aeron_GetContentPixelRect(&rect) && rect.height > 0) {
			view_w_ref = 1080.0f * (float)rect.width / (float)rect.height;
		}
	}
	const float width_ref = SDL_min(1400.0f, view_w_ref - 80.0f);
	const float x_ref     = (view_w_ref - width_ref) * 0.5f;
	if (g_vrk.dismissed) {
		/* Closed: a small button where it was brings it back. */
		if (AeronUi_BeginWindow(g_vrk.ui, "Keyboard (closed)",
								&(AeronUiWindowDesc) { .width_ref = 300.0f, .x_ref = x_ref, .y_ref = 120.0f })) {
			if (AeronUi_Button(g_vrk.ui, "Keyboard")) {
				g_vrk.dismissed = 0;
			}
			AeronUi_EndWindow(g_vrk.ui);
		}
	} else if (AeronUi_BeginWindow(g_vrk.ui, "Keyboard",
								   &(AeronUiWindowDesc) { .width_ref = width_ref, .x_ref = x_ref, .y_ref = 120.0f })) {
		vrk_row("1234567890", 1, "Del", 8);
		vrk_row("QWERTYUIOP", 0, NULL, 0);
		vrk_row("ASDFGHJKL", 1, "Enter", 13);
		vrk_row("ZXCVBNM-.", 0, NULL, 0);
		AeronUi_BeginColumns(g_vrk.ui, 4, (const float[]) { 1.0f, 3.0f, 1.0f, 1.0f });
		if (AeronUi_Button(g_vrk.ui, g_vrk.shift ? "SHIFT" : "Shift")) {
			g_vrk.shift = !g_vrk.shift;
		}
		AeronUi_NextColumn(g_vrk.ui);
		if (AeronUi_Button(g_vrk.ui, "Space")) {
			vrk_type(' ');
		}
		AeronUi_NextColumn(g_vrk.ui);
		if (AeronUi_Button(g_vrk.ui, "Cancel")) {
			vrk_type(27);
		}
		AeronUi_NextColumn(g_vrk.ui);
		/* Hides the keyboard only; the text field stays as it is. */
		if (AeronUi_Button(g_vrk.ui, "Close")) {
			g_vrk.dismissed = 1;
		}
		AeronUi_EndColumns(g_vrk.ui);
		AeronUi_EndWindow(g_vrk.ui);
	}
	/* The game's cursor is drawn underneath this overlay; show where the
	 * laser points on top of the keys. */
	AeronUi_DrawPointer(g_vrk.ui, 9.0f);
	const AeronUiOutput out = AeronUi_EndFrame(g_vrk.ui);
	g_vrk.frame_open        = 1;

	/* Keep the game from seeing clicks that belong to the keyboard, for the
	 * whole press (down through release). */
	if (out.mouse_over && (in->mouse.pressed_buttons & 1u)) {
		g_vrk.mouse_captured = 1;
	}
	if (g_vrk.mouse_captured) {
		const int released = (in->mouse.buttons & 1u) == 0;
		Aeron_InputSuppressMouseButtons();
		if (released) {
			g_vrk.mouse_captured = 0;
		}
	}
}

void XwaVrKeyboard_Submit(void) {
	if (g_vrk.frame_open && g_vrk.ui) {
		AeronUi_Submit(g_vrk.ui);
	}
	g_vrk.frame_open = 0;
}

void XwaVrKeyboard_Shutdown(void) {
	if (g_vrk.ui) {
		AeronUi_Destroy(g_vrk.ui);
		AeronFontAtlas_Release(&g_vrk.atlas);
	}
	memset(&g_vrk, 0, sizeof g_vrk);
}
