/* VR cockpit HUD panels; see vr_cockpit_hud.h. */

#include "xwa_remaster/vr_cockpit_hud.h"
#include <SDL3/SDL_stdinc.h>

#include "aeron/aeron.h"
#include "aeron/scene/draw_list2d.h"
#include "xwa_remaster/vr_lap_console.h"
#include "xwa_remaster/hud.h"
#include "xwa_remaster/hud_cmd.h"
#include "xwa_remaster/hud_fixed.h"
#include "xwa_remaster/hud_text.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	CK_LEFT_POD,
	CK_RIGHT_POD,
	CK_MESSAGES,
	CK_MESSAGES_LOW,
	CK_COUNTERS,
	CK_SPEED,
	CK_CLOCK,
	CK_CHARGE,
	CK_CMD,
	CK_MFD_LEFT,
	CK_MFD_RIGHT,
	CK_COUNT
};

/* Placement relative to the seated eye point: yaw (+ right) and pitch
 * (+ up) in degrees, distance in metres, and the box (metres) the panel's
 * rectangle is fitted into, keeping its aspect. X-wing defaults. */
typedef struct CkSpec {
	const char* name;
	float yaw, pitch, distance;
	float box_w, box_h;
	/* > 0: fixed size, metres per classic HUD unit (1/480 of the screen
	 * height), instead of fitting the box: text keeps one size whatever the
	 * panel's width. */
	float m_per_unit;
} CkSpec;

static const CkSpec k_spec[CK_COUNT] = {
	[CK_LEFT_POD] = { "fore radar pod", -40.0f, 2.0f, 0.75f, 0.1955f, 0.2415f },
	[CK_RIGHT_POD] = { "aft radar pod", 40.0f, 2.0f, 0.75f, 0.1955f, 0.2415f },
	[CK_MESSAGES] = { "messages", 0.0f, -7.0f, 0.95f, 0.36f, 0.08f, 0.0036f },
	/* The flight-group line sits low on the flat screen (above the
	 * targeting computer): its own panel, so the two message areas don't
	 * join into one huge panel. */
	[CK_MESSAGES_LOW] = { "flight-group messages", 0.0f, -16.0f, 0.95f, 0.36f, 0.08f, 0.00216f },
	/* The top-bar readouts sit in a row on the targeting computer's top
	 * edge (placed by ck_place_row; the pose here is only a fallback). */
	[CK_COUNTERS] = { "counters", 0.0f, -21.0f, 0.62f, 0.12f, 0.03f },
	[CK_SPEED] = { "speed", -9.0f, -21.0f, 0.62f, 0.12f, 0.03f },
	[CK_CLOCK] = { "clock", 9.0f, -21.0f, 0.62f, 0.12f, 0.03f },
	[CK_CHARGE] = { "charge", 0.0f, -17.0f, 0.85f, 0.15f, 0.03f },
	[CK_CMD] = { "targeting computer", 0.0f, -30.0f, 0.62f, 0.20f, 0.16f },
	/* Beside the targeting computer, above the lap console. */
	[CK_MFD_LEFT] = { "left MFD", -29.0f, -25.0f, 0.62f, 0.17f, 0.13f },
	[CK_MFD_RIGHT] = { "right MFD", 29.0f, -25.0f, 0.62f, 0.17f, 0.13f },
};

/* Default spots that differ per cockpit (by the name the panel layout is
 * kept under, upper case): the X-Wing Revival cockpits (VR Options, see
 * vr_revival_cockpit.h) have their dashboards and screens elsewhere than
 * XWA's. Each panel sits just in front of the dash or screen it covers, so
 * it is seen at the same depth as the cockpit around it; it keeps its
 * default angular size (its box and text scale grow with the distance).
 * Fitted to the models from the eye with a depth map of each cockpit. */
typedef struct CkSpot {
	int panel;
	float yaw, pitch, distance; /* distance 0 ends the list */
} CkSpot;

static const struct {
	const char* cockpit;
	CkSpot spots[CK_COUNT + 1];
} k_cockpit_spots[] = {
	/* Pods below the canopy struts; the targeting computer on the
	 * intercept screen, the readout row on the target screen above it. */
	{ "REVIVALXWING",
	  { { CK_LEFT_POD, -43.0f, -9.0f, 0.53f },
		{ CK_RIGHT_POD, 43.0f, -9.0f, 0.53f },
		{ CK_MESSAGES, 0.0f, -4.0f, 0.95f },
		{ CK_MESSAGES_LOW, 0.0f, -16.0f, 0.65f },
		{ CK_CMD, 0.0f, -28.0f, 0.60f },
		{ CK_MFD_LEFT, -29.0f, -25.0f, 0.55f },
		{ CK_MFD_RIGHT, 29.0f, -25.0f, 0.55f } } },
	/* The dash is 0.75-0.9 m away. */
	{ "REVIVALYWING",
	  { { CK_MESSAGES, 0.0f, -7.0f, 0.93f },
		{ CK_MESSAGES_LOW, 0.0f, -13.0f, 0.83f },
		{ CK_CMD, 0.0f, -27.0f, 0.73f },
		{ CK_MFD_LEFT, -29.0f, -25.0f, 0.72f },
		{ CK_MFD_RIGHT, 29.0f, -25.0f, 0.72f } } },
	{ "REVIVALAWING",
	  { { CK_MESSAGES_LOW, 0.0f, -14.0f, 0.62f },
		{ CK_CMD, 0.0f, -27.0f, 0.63f },
		{ CK_MFD_LEFT, -29.0f, -25.0f, 0.59f },
		{ CK_MFD_RIGHT, 29.0f, -25.0f, 0.59f } } },
	/* The dash is 1.1 m away: the targeting computer on the round centre
	 * screen, the MFDs on the two round side screens. */
	{ "REVIVALBWING",
	  { { CK_MESSAGES_LOW, 0.0f, -12.0f, 1.07f },
		{ CK_CMD, 0.0f, -25.0f, 1.07f },
		{ CK_MFD_LEFT, -18.5f, -19.0f, 1.08f },
		{ CK_MFD_RIGHT, 18.5f, -19.0f, 1.08f } } },
	/* TIEs: the pods on the hull wall beside the viewport, the targeting
	 * computer on the console's centre (above the yoke, which would hide
	 * its lower edge), the MFDs on the console's side screens; the
	 * flight-group line above the readout row, in the viewport. Seen from
	 * the TIE seat 30 cm forward of the model's eye point (flight.c,
	 * k_xr_seat_shift): the same spots on the cockpit, nearer the eye. */
	{ "REVIVALTIEFIGHTER",
	  { { CK_LEFT_POD, -59.3f, 2.7f, 0.58f },
		{ CK_RIGHT_POD, 59.3f, 2.7f, 0.58f },
		{ CK_MESSAGES_LOW, 0.0f, -16.0f, 0.66f },
		{ CK_CMD, 0.0f, -39.7f, 0.37f },
		{ CK_MFD_LEFT, -40.2f, -37.4f, 0.41f },
		{ CK_MFD_RIGHT, 40.2f, -37.4f, 0.41f } } },
	{ "REVIVALTIEINTERCEPTOR",
	  { { CK_LEFT_POD, -59.3f, 2.7f, 0.58f },
		{ CK_RIGHT_POD, 59.3f, 2.7f, 0.58f },
		{ CK_MESSAGES_LOW, 0.0f, -16.0f, 0.66f },
		{ CK_CMD, 0.0f, -39.7f, 0.37f },
		{ CK_MFD_LEFT, -40.2f, -37.4f, 0.41f },
		{ CK_MFD_RIGHT, 40.2f, -37.4f, 0.41f } } },
	{ "REVIVALTIEBOMBER",
	  { { CK_LEFT_POD, -57.3f, 2.6f, 0.65f },
		{ CK_RIGHT_POD, 57.3f, 2.6f, 0.65f },
		{ CK_MESSAGES_LOW, 0.0f, -17.4f, 0.66f },
		{ CK_CMD, 0.0f, -45.1f, 0.34f },
		{ CK_MFD_LEFT, -42.6f, -38.7f, 0.38f },
		{ CK_MFD_RIGHT, 42.6f, -38.7f, 0.38f } } },
	/* YT-1300: the console slopes away from 0.7 to 1.1 m. */
	{ "REVIVALFALCON",
	  { { CK_LEFT_POD, -40.0f, 2.0f, 0.71f },
		{ CK_MESSAGES_LOW, 0.0f, -16.0f, 1.11f },
		{ CK_CMD, 0.0f, -26.0f, 0.68f },
		{ CK_MFD_LEFT, -29.0f, -25.0f, 0.66f },
		{ CK_MFD_RIGHT, 29.0f, -25.0f, 0.66f } } },
};

static const XwaHudWidgetId k_widgets[CK_COUNT][5] = {
	[CK_LEFT_POD] = { XWA_HUD_WIDGET_FORE_RADAR_FRAME, XWA_HUD_WIDGET_FORE_RADAR_SCOPE, XWA_HUD_WIDGET_LEFT_POWER,
					  XWA_HUD_WIDGET_SHIELD_HULL, XWA_HUD_WIDGET_COUNT },
	[CK_RIGHT_POD] = { XWA_HUD_WIDGET_AFT_RADAR_FRAME, XWA_HUD_WIDGET_AFT_RADAR_SCOPE, XWA_HUD_WIDGET_RIGHT_POWER,
					   XWA_HUD_WIDGET_BEAM, XWA_HUD_WIDGET_COUNT },
	[CK_MESSAGES] = { XWA_HUD_WIDGET_COUNT },
	[CK_MESSAGES_LOW] = { XWA_HUD_WIDGET_COUNT },
	[CK_COUNTERS] = { XWA_HUD_WIDGET_COUNT },
	[CK_SPEED] = { XWA_HUD_WIDGET_COUNT },
	[CK_CLOCK] = { XWA_HUD_WIDGET_COUNT },
	/* The charge bars sit inside the targeting computer's frame; a separate
	 * panel would show the target behind them, so they stay part of it. */
	[CK_CHARGE] = { XWA_HUD_WIDGET_COUNT },
	[CK_CMD] = { XWA_HUD_WIDGET_CMD_FRAME, XWA_HUD_WIDGET_LASER_CHARGE, XWA_HUD_WIDGET_ION_CHARGE,
				 XWA_HUD_WIDGET_COUNT },
	[CK_MFD_LEFT] = { XWA_HUD_WIDGET_MFD_LEFT_FRAME, XWA_HUD_WIDGET_COUNT },
	[CK_MFD_RIGHT] = { XWA_HUD_WIDGET_MFD_RIGHT_FRAME, XWA_HUD_WIDGET_COUNT },
};

static const uint16_t k_panes[CK_COUNT][4] = {
	[CK_LEFT_POD] = { XWA_HUD_PANE_LEFT_SUBSYSTEM, 0 },
	[CK_RIGHT_POD] = { XWA_HUD_PANE_RIGHT_SUBSYSTEM, 0 },
	[CK_MESSAGES] = { XWA_HUD_PANE_MESSAGE_SYSTEM, XWA_HUD_PANE_MESSAGE_READY, 0 },
	[CK_MESSAGES_LOW] = { XWA_HUD_PANE_MESSAGE_FLIGHT_GROUP, 0 },
	[CK_COUNTERS] = { XWA_HUD_PANE_TOP_WEAPONS, XWA_HUD_PANE_TOP_COUNTERMEASURE, XWA_HUD_PANE_TOP_PROVING_STATUS, 0 },
	[CK_SPEED] = { XWA_HUD_PANE_TOP_SPEED, XWA_HUD_PANE_TOP_THROTTLE, 0 },
	[CK_CLOCK] = { XWA_HUD_PANE_TOP_CRAFT_NAME, XWA_HUD_PANE_TOP_CLOCK, 0 },
	[CK_CHARGE] = { 0 },
	[CK_CMD] = { XWA_HUD_PANE_CMD, 0 },
	[CK_MFD_LEFT] = { XWA_HUD_PANE_MFD_LEFT_TITLE, XWA_HUD_PANE_MFD_LEFT_BODY, 0 },
	[CK_MFD_RIGHT] = { XWA_HUD_PANE_MFD_RIGHT_TITLE, XWA_HUD_PANE_MFD_RIGHT_BODY, 0 },
};

static struct {
	int target_w, target_h;
	uint32_t layout_generation, bundle_generation;
	/* Grow-only union of what each panel showed since the last reset, so a
	 * panel keeps its size while parts of it come and go. */
	float latched[CK_COUNT][4];
	uint8_t latched_valid[CK_COUNT];
	/* This frame's final rectangles (after clipping) and visibility. */
	float rect[CK_COUNT][4];
	uint8_t visible[CK_COUNT];
	char logged[512];
	/* Panels placed by hand, per cockpit (XWA_XR_HUD_POSE_<COCKPIT>):
	 * LOCAL-space poses that replace the defaults. */
	char cockpit_key[96];
	char pose_env[1024];
	uint8_t custom[CK_COUNT];
	float cpos[CK_COUNT][3], cq[CK_COUNT][4];
	/* Grabbing: panel held by each hand (-1 none), the panel relative to
	 * the hand, hovered panel per hand, last grip value. */
	int grab_panel[2];
	float grab_pos[2][3], grab_q[2][4];
	int hover_panel[2];
	float grip_prev[2];
	int layout_dirty;
	char layout_text[1024];
	/* Outline around hovered / held panels, drawn into the HUD targets. */
	AeronDrawList2D* outline;
	AeronDrawList2D* outline_aux;
	int outline_ready;
	int cmd_separate;
	int initialized;
	/* This cockpit's panel specs (k_spec with its k_cockpit_spots) and the
	 * readout row's size factor (the targeting computer's distance over
	 * its default). */
	char spec_cockpit[96];
	CkSpec spec[CK_COUNT];
	float row_scale;
} s_ck;

/* Short ids for the saved layout. */
static const char* const k_ids[CK_COUNT] = {
	[CK_LEFT_POD] = "pod_l",	[CK_RIGHT_POD] = "pod_r", [CK_MESSAGES] = "msg",	[CK_MESSAGES_LOW] = "msg_low",
	[CK_COUNTERS] = "counters", [CK_SPEED] = "speed",	  [CK_CLOCK] = "clock", [CK_CHARGE] = "charge",
	[CK_CMD] = "cmd",			[CK_MFD_LEFT] = "mfd_l",  [CK_MFD_RIGHT] = "mfd_r",
};

#define CK_GRAB_FORGIVE (1.15f * 1.10f) /* 15 %, then 10 % more forgiving */
#define CK_GRAB_MARGIN_M (0.02f * CK_GRAB_FORGIVE)
#define CK_GRAB_DEPTH_M (0.05f * CK_GRAB_FORGIVE)
#define CK_GRIP_ON 0.6f
#define CK_GRIP_OFF 0.35f

static void ck_union(float acc[4], int* valid, const float b[4]) {
	if (!*valid) {
		memcpy(acc, b, sizeof(float) * 4);
		*valid = 1;
		return;
	}
	acc[0] = fminf(acc[0], b[0]);
	acc[1] = fminf(acc[1], b[1]);
	acc[2] = fmaxf(acc[2], b[2]);
	acc[3] = fmaxf(acc[3], b[3]);
}

float XwaVrCockpitHud_SeatBack(void) {
	const char* env = SDL_getenv("XWA_XR_SEAT_POSITION");
	const float back = env && env[0] ? (float)atof(env) : 0.0f;
	return back < -0.2f ? -0.2f : back > 0.2f ? 0.2f : back;
}

static void ck_pose(const CkSpec* spec, float world_scale, AeronXrPanel* out) {
	const float deg = 3.14159265f / 180.0f;
	const float yaw = spec->yaw * deg, pitch = spec->pitch * deg;
	const float d = spec->distance / world_scale;
	/* LOCAL space: +X right, +Y up, -Z forward. */
	const float dir[3] = { sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) };
	out->position[0] = dir[0] * d;
	out->position[1] = dir[1] * d;
	out->position[2] = dir[2] * d - XwaVrCockpitHud_SeatBack(); /* -Z forward */
	/* The quad's +Z faces the eye point; its X stays level. */
	const float z[3] = { -dir[0], -dir[1], -dir[2] };
	float x[3] = { z[2], 0.0f, -z[0] }; /* (0,1,0) x z */
	const float xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
	x[0] /= xl;
	x[2] /= xl;
	const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
	/* Rotation matrix with columns x, y, z -> quaternion. */
	const float m00 = x[0], m01 = y[0], m02 = z[0];
	const float m10 = x[1], m11 = y[1], m12 = z[1];
	const float m20 = x[2], m21 = y[2], m22 = z[2];
	const float trace = m00 + m11 + m22;
	float qx, qy, qz, qw;
	if (trace > 0.0f) {
		const float t = sqrtf(trace + 1.0f) * 2.0f;
		qw = 0.25f * t;
		qx = (m21 - m12) / t;
		qy = (m02 - m20) / t;
		qz = (m10 - m01) / t;
	} else if (m00 > m11 && m00 > m22) {
		const float t = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		qw = (m21 - m12) / t;
		qx = 0.25f * t;
		qy = (m01 + m10) / t;
		qz = (m02 + m20) / t;
	} else if (m11 > m22) {
		const float t = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		qw = (m02 - m20) / t;
		qx = (m01 + m10) / t;
		qy = 0.25f * t;
		qz = (m12 + m21) / t;
	} else {
		const float t = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		qw = (m10 - m01) / t;
		qx = (m02 + m20) / t;
		qy = (m12 + m21) / t;
		qz = 0.25f * t;
	}
	out->orientation[0] = qx;
	out->orientation[1] = qy;
	out->orientation[2] = qz;
	out->orientation[3] = qw;
}

static void ck_rotate(const float q[4], const float v[3], float out[3]) {
	const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/* Speed | counters | clock: one row standing on the targeting computer's
 * top edge, in its plane, bottoms aligned. One text scale for all three,
 * as large as fits ROW_W. */
#define CK_ROW_W_M 0.34f
#define CK_ROW_GAP_M 0.008f
#define CK_ROW_MAX_M_PER_UNIT 0.0026f

static void ck_place_row(AeronXrPanel* out, const int* index_of, float world_scale, int target_h) {
	const int is = index_of[CK_SPEED], ic = index_of[CK_COUNTERS], ik = index_of[CK_CLOCK];
	if (is < 0 && ic < 0 && ik < 0) {
		return;
	}
	/* Counters centred, speed to their left, clock to their right: the
	 * row is symmetric, so the wider side decides the scale. */
	const float cw_px = ic >= 0 ? (float)out[ic].rect[2] : 0.0f;
	const float side_px = fmaxf(is >= 0 ? (float)out[is].rect[2] : 0.0f, ik >= 0 ? (float)out[ik].rect[2] : 0.0f);
	const float k = s_ck.row_scale > 0.0f ? s_ck.row_scale : 1.0f;
	const float gap = CK_ROW_GAP_M * k;
	const float m_per_px = fminf(k * CK_ROW_MAX_M_PER_UNIT * 480.0f / (float)target_h,
								 (k * CK_ROW_W_M - 2.0f * gap) / fmaxf(1.0f, cw_px + 2.0f * side_px));
	AeronXrPanel cmd;
	memset(&cmd, 0, sizeof cmd);
	if (index_of[CK_CMD] >= 0) {
		cmd = out[index_of[CK_CMD]];
	} else {
		ck_pose(&s_ck.spec[CK_CMD], world_scale, &cmd);
		cmd.size[0] = s_ck.spec[CK_CMD].box_w / world_scale;
		cmd.size[1] = s_ck.spec[CK_CMD].box_h / world_scale;
	}
	static const float ux[3] = { 1.0f, 0.0f, 0.0f }, uy[3] = { 0.0f, 1.0f, 0.0f };
	float ax[3], ay[3];
	ck_rotate(cmd.orientation, ux, ax);
	ck_rotate(cmd.orientation, uy, ay);
	const float half_c = 0.5f * cw_px * m_per_px + (ic >= 0 ? gap : 0.5f * gap);
	const int ids[3] = { is, ic, ik };
	for (int k = 0; k < 3; ++k) {
		if (ids[k] < 0) {
			continue;
		}
		AeronXrPanel* panel = &out[ids[k]];
		const float w = (float)panel->rect[2] * m_per_px, h = (float)panel->rect[3] * m_per_px;
		const float cx = k == 1 ? 0.0f : k == 0 ? -(half_c + 0.5f * w) : half_c + 0.5f * w;
		const float off_x = cx / world_scale;
		const float off_y = 0.5f * cmd.size[1] + (gap * 0.5f + 0.5f * h) / world_scale;
		for (int a = 0; a < 3; ++a) {
			panel->position[a] = cmd.position[a] + ax[a] * off_x + ay[a] * off_y;
		}
		memcpy(panel->orientation, cmd.orientation, sizeof panel->orientation);
		panel->size[0] = w / world_scale;
		panel->size[1] = h / world_scale;
	}
}

static void ck_qmul(const float a[4], const float b[4], float out[4]) {
	const float r[4] = {
		a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
		a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
		a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
		a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
	};
	memcpy(out, r, sizeof r);
}

static void ck_qnorm(float q[4]) {
	const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l > 1e-6f) {
		for (int k = 0; k < 4; ++k) {
			q[k] /= l;
		}
	} else {
		q[0] = q[1] = q[2] = 0.0f;
		q[3] = 1.0f;
	}
}

/* The panel specs for `cockpit`: the defaults, with its own spots. */
static void ck_select_specs(const char* cockpit) {
	char name[96];
	size_t n = 0;
	for (const char* c = cockpit ? cockpit : ""; *c && n + 1 < sizeof name; ++c) {
		if (isalnum((unsigned char)*c)) {
			name[n++] = (char)toupper((unsigned char)*c);
		}
	}
	name[n] = '\0';
	if (s_ck.row_scale > 0.0f && strcmp(name, s_ck.spec_cockpit) == 0) {
		return;
	}
	snprintf(s_ck.spec_cockpit, sizeof s_ck.spec_cockpit, "%s", name);
	memcpy(s_ck.spec, k_spec, sizeof s_ck.spec);
	for (size_t i = 0; i < sizeof k_cockpit_spots / sizeof k_cockpit_spots[0]; ++i) {
		if (strcmp(name, k_cockpit_spots[i].cockpit) != 0) {
			continue;
		}
		for (const CkSpot* spot = k_cockpit_spots[i].spots; spot->distance > 0.0f; ++spot) {
			CkSpec* spec = &s_ck.spec[spot->panel];
			const float k = spot->distance / k_spec[spot->panel].distance;
			spec->yaw = spot->yaw;
			spec->pitch = spot->pitch;
			spec->distance = spot->distance;
			spec->box_w *= k;
			spec->box_h *= k;
			spec->m_per_unit *= k;
		}
	}
	s_ck.row_scale = s_ck.spec[CK_CMD].distance / k_spec[CK_CMD].distance;
}

/* XWA_XR_HUD_POSE_ + the cockpit's name in capitals. */
static void ck_layout_key(const char* name, char* out, size_t size) {
	size_t n = (size_t)snprintf(out, size, "XWA_XR_HUD_POSE_");
	for (const char* c = name && name[0] ? name : "DEFAULT"; *c && n + 1 < size; ++c) {
		if (isalnum((unsigned char)*c)) {
			out[n++] = (char)toupper((unsigned char)*c);
		}
	}
	out[n] = '\0';
}

/* Hand-placed layouts that ship as the defaults (the cockpit's key unset
 * in openxwa-quest.cfg; a saved layout, even an empty one, wins). */
static const struct {
	const char* key;
	const char* layout;
} k_builtin_layouts[] = {
	{ "XWA_XR_HUD_POSE_XWINGCOCKPIT", "pod_l=-0.4130,-0.0378,-0.5794,-0.02721,0.27131,0.02440,0.96180;"
									  "pod_r=0.4016,0.0013,-0.5742,0.01347,-0.32619,0.00733,0.94518" },
	{ "XWA_XR_HUD_POSE_CORELLIANTRANSPORT2COCKPIT",
	  "pod_l=-0.3468,0.0794,-0.6747,-0.00307,0.22331,-0.00042,0.97474;"
	  "pod_r=0.4555,0.0481,-0.7340,0.01256,-0.27346,0.01234,0.96172" },
	{ "XWA_XR_HUD_POSE_CORELLIANTRANSPORTGUNNER",
	  "pod_l=-0.4119,-0.0053,-0.6071,0.01244,0.33782,-0.01006,0.94107;"
	  "pod_r=0.4409,0.0234,-0.5578,0.03452,-0.34751,0.01515,0.93692" },
};

static const char* ck_builtin_layout(const char* key) {
	for (size_t i = 0; i < sizeof k_builtin_layouts / sizeof k_builtin_layouts[0]; ++i) {
		if (strcmp(key, k_builtin_layouts[i].key) == 0) {
			return k_builtin_layouts[i].layout;
		}
	}
	return "";
}

/* Reads this cockpit's layout ("id=x,y,z,qx,qy,qz,qw;...") when it
 * changed. */
static void ck_read_layout(const char* cockpit) {
	char key[96];
	ck_layout_key(cockpit, key, sizeof key);
	const char* v = SDL_getenv(key);
	if (!v) {
		v = ck_builtin_layout(key);
	}
	if (strcmp(key, s_ck.cockpit_key) == 0 && strcmp(v, s_ck.pose_env) == 0) {
		return;
	}
	snprintf(s_ck.cockpit_key, sizeof s_ck.cockpit_key, "%s", key);
	snprintf(s_ck.pose_env, sizeof s_ck.pose_env, "%s", v);
	memset(s_ck.custom, 0, sizeof s_ck.custom);
	const char* p = v;
	while (*p) {
		char id[24];
		float f[7];
		int used = 0;
		if (sscanf(p, "%23[^=]=%f,%f,%f,%f,%f,%f,%f%n", id, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &used) ==
				8 &&
			used > 0) {
			for (int i = 0; i < CK_COUNT; ++i) {
				if (k_ids[i] && strcmp(id, k_ids[i]) == 0 && fabsf(f[0]) < 3.0f && fabsf(f[1]) < 3.0f &&
					fabsf(f[2]) < 3.0f) {
					memcpy(s_ck.cpos[i], f, sizeof s_ck.cpos[i]);
					memcpy(s_ck.cq[i], &f[3], sizeof s_ck.cq[i]);
					ck_qnorm(s_ck.cq[i]);
					s_ck.custom[i] = 1;
				}
			}
			p += used;
		} else {
			const char* next = strchr(p, ';');
			if (!next) {
				break;
			}
			p = next;
		}
		while (*p == ';' || *p == ' ') {
			p++;
		}
	}
}

static void ck_save_layout(void) {
	int n = 0;
	s_ck.layout_text[0] = '\0';
	for (int i = 0; i < CK_COUNT; ++i) {
		if (!s_ck.custom[i] || !k_ids[i]) {
			continue;
		}
		n += snprintf(s_ck.layout_text + n, sizeof s_ck.layout_text - (size_t)n, "%s%s=%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f",
					  n ? ";" : "", k_ids[i], s_ck.cpos[i][0], s_ck.cpos[i][1], s_ck.cpos[i][2], s_ck.cq[i][0],
					  s_ck.cq[i][1], s_ck.cq[i][2], s_ck.cq[i][3]);
		if (n >= (int)sizeof s_ck.layout_text - 1) {
			break;
		}
	}
	SDL_setenv_unsafe(s_ck.cockpit_key, s_ck.layout_text, 1);
	snprintf(s_ck.pose_env, sizeof s_ck.pose_env, "%s", s_ck.layout_text);
	s_ck.layout_dirty = 1;
	Aeron_LogInfo("xwa.remaster", "VR HUD layout %s saved", s_ck.cockpit_key);
}

int XwaVrCockpitHud_TakeLayout(char* key, int key_size, char* value, int value_size) {
	if (!s_ck.layout_dirty || !key || !value || key_size <= 0 || value_size <= 0) {
		return 0;
	}
	snprintf(key, (size_t)key_size, "%s", s_ck.cockpit_key);
	snprintf(value, (size_t)value_size, "%s", s_ck.layout_text);
	s_ck.layout_dirty = 0;
	return 1;
}

static void ck_apply_custom(AeronXrPanel* panel, int p) {
	memcpy(panel->position, s_ck.cpos[p], sizeof panel->position);
	panel->position[2] -= XwaVrCockpitHud_SeatBack(); /* stored as at seat position 0 */
	memcpy(panel->orientation, s_ck.cq[p], sizeof panel->orientation);
}

/* Squeezing the grip with a hand on a panel picks it up; it follows the
 * hand freely until the grip is let go, then the layout is saved for this
 * cockpit. */
static void ck_update_grab(AeronXrPanel* out, const int* index_of) {
	static const float ux[3] = { 1, 0, 0 }, uy[3] = { 0, 1, 0 }, uz[3] = { 0, 0, 1 };
	for (int hand = 0; hand < 2; ++hand) {
		AeronXrHand xh;
		s_ck.hover_panel[hand] = -1;
		if (!Aeron_XrGetHand(hand, &xh)) {
			if (s_ck.grab_panel[hand] >= 0) {
				s_ck.grab_panel[hand] = -1;
				ck_save_layout();
			}
			s_ck.grip_prev[hand] = 0.0f;
			continue;
		}
		/* The panel the hand is on (nearest plane). */
		int best = -1;
		float best_depth = 1e9f;
		for (int p = 0; p < CK_COUNT; ++p) {
			if (index_of[p] < 0) {
				continue;
			}
			const AeronXrPanel* panel = &out[index_of[p]];
			float ax[3], ay[3], az[3];
			ck_rotate(panel->orientation, ux, ax);
			ck_rotate(panel->orientation, uy, ay);
			ck_rotate(panel->orientation, uz, az);
			const float rel[3] = { xh.position[0] - panel->position[0], xh.position[1] - panel->position[1],
								   xh.position[2] - panel->position[2] };
			const float lx = rel[0] * ax[0] + rel[1] * ax[1] + rel[2] * ax[2];
			const float ly = rel[0] * ay[0] + rel[1] * ay[1] + rel[2] * ay[2];
			const float depth = rel[0] * az[0] + rel[1] * az[1] + rel[2] * az[2];
			if (fabsf(lx) < 0.5f * panel->size[0] + CK_GRAB_MARGIN_M &&
				fabsf(ly) < 0.5f * panel->size[1] + CK_GRAB_MARGIN_M && fabsf(depth) < CK_GRAB_DEPTH_M &&
				fabsf(depth) < best_depth) {
				best = p;
				best_depth = fabsf(depth);
			}
		}
		const int other = s_ck.grab_panel[1 - hand];
		if (best >= 0 && best != other) {
			s_ck.hover_panel[hand] = best;
		}
		const int held = s_ck.grab_panel[hand];
		if (held >= 0) {
			if (xh.grip < CK_GRIP_OFF) {
				s_ck.grab_panel[hand] = -1;
				Aeron_XrHapticPulse(hand, 0.3f, 20);
				ck_save_layout();
			} else {
				float off[3];
				ck_rotate(xh.orientation, s_ck.grab_pos[hand], off);
				for (int k = 0; k < 3; ++k) {
					s_ck.cpos[held][k] = xh.position[k] + off[k];
				}
				s_ck.cpos[held][2] += XwaVrCockpitHud_SeatBack();
				ck_qmul(xh.orientation, s_ck.grab_q[hand], s_ck.cq[held]);
				ck_qnorm(s_ck.cq[held]);
				s_ck.custom[held] = 1;
			}
		} else if (s_ck.hover_panel[hand] >= 0 && xh.grip >= CK_GRIP_ON && s_ck.grip_prev[hand] < CK_GRIP_OFF) {
			const int p = s_ck.hover_panel[hand];
			const AeronXrPanel* panel = &out[index_of[p]];
			const float inv[4] = { -xh.orientation[0], -xh.orientation[1], -xh.orientation[2], xh.orientation[3] };
			const float to_panel[3] = { panel->position[0] - xh.position[0], panel->position[1] - xh.position[1],
										panel->position[2] - xh.position[2] };
			ck_rotate(inv, to_panel, s_ck.grab_pos[hand]);
			ck_qmul(inv, panel->orientation, s_ck.grab_q[hand]);
			ck_qnorm(s_ck.grab_q[hand]);
			memcpy(s_ck.cpos[p], panel->position, sizeof s_ck.cpos[p]);
			s_ck.cpos[p][2] += XwaVrCockpitHud_SeatBack();
			memcpy(s_ck.cq[p], panel->orientation, sizeof s_ck.cq[p]);
			s_ck.custom[p] = 1;
			s_ck.grab_panel[hand] = p;
			Aeron_XrHapticPulse(hand, 0.6f, 30);
		}
		s_ck.grip_prev[hand] = xh.grip;
		if (s_ck.hover_panel[hand] >= 0 || s_ck.grab_panel[hand] >= 0) {
			XwaVrConsole_CaptureGrip(hand); /* the grip moves the panel, not the game */
		}
	}
}

int XwaVrCockpitHud_Build(int target_w, int target_h, float world_scale, int cmd_separate, const char* cockpit,
						  AeronXrPanel* out, int max_out) {
	const XwaHudPreparedDrawState* drawn = XwaRemasterHud_PreparedDrawState();
	int count = 0;
	if (!s_ck.initialized) {
		s_ck.grab_panel[0] = s_ck.grab_panel[1] = -1;
		s_ck.hover_panel[0] = s_ck.hover_panel[1] = -1;
		s_ck.initialized = 1;
	}
	s_ck.cmd_separate = cmd_separate;
	if (s_ck.grab_panel[0] < 0 && s_ck.grab_panel[1] < 0) {
		ck_read_layout(cockpit);
	}
	ck_select_specs(cockpit);
	memset(s_ck.visible, 0, sizeof s_ck.visible);
	if (!out || max_out <= 0 || target_w <= 0 || target_h <= 0 || !drawn || !drawn->valid) {
		return 0;
	}
	if (world_scale <= 0.0f) {
		world_scale = 1.0f;
	}
	if (s_ck.target_w != target_w || s_ck.target_h != target_h ||
		s_ck.layout_generation != drawn->layout_generation || s_ck.bundle_generation != drawn->bundle_generation) {
		memset(s_ck.latched_valid, 0, sizeof s_ck.latched_valid);
		s_ck.target_w = target_w;
		s_ck.target_h = target_h;
		s_ck.layout_generation = drawn->layout_generation;
		s_ck.bundle_generation = drawn->bundle_generation;
	}

	/* What each panel shows this frame. */
	float now[CK_COUNT][4];
	for (int p = 0; p < CK_COUNT; ++p) {
		float acc[4];
		int valid = 0;
		for (int i = 0; i < 5 && k_widgets[p][i] != XWA_HUD_WIDGET_COUNT; ++i) {
			float b[4];
			if (XwaRemasterHudFixed_WidgetBounds(k_widgets[p][i], b)) {
				ck_union(acc, &valid, b);
			}
		}
		for (int i = 0; i < 4 && k_panes[p][i]; ++i) {
			float b[4];
			/* What the text actually drew: the classic pane rectangles are
			 * often larger (empty corners) or smaller (clipped text). */
			if (XwaRemasterHudText_PaneGlyphBounds(k_panes[p][i], b)) {
				ck_union(acc, &valid, b);
			}
		}
		if (p == CK_CMD) {
			float b[4];
			if (XwaRemasterHudCmd_ViewBounds(b)) {
				ck_union(acc, &valid, b);
			}
		}
		if (valid) {
			memcpy(now[p], acc, sizeof now[p]);
			int latched = s_ck.latched_valid[p];
			ck_union(s_ck.latched[p], &latched, acc);
			s_ck.latched_valid[p] = 1;
			s_ck.visible[p] = 1;
		}
	}

	/* Final rectangles: the classic top bar and bottom panel are drawn as
	 * joined pieces, so cut neighbours apart where they overlap. */
	for (int p = 0; p < CK_COUNT; ++p) {
		memcpy(s_ck.rect[p], s_ck.latched[p], sizeof s_ck.rect[p]);
	}
	if (s_ck.latched_valid[CK_COUNTERS]) {
		float* c = s_ck.rect[CK_COUNTERS];
		const float pad = 0.15f * (c[3] - c[1]);
		c[0] -= pad;
		c[1] -= pad;
		c[2] += pad;
		c[3] += pad;
	}
	/* The MFD frames reach into the targeting computer's area (unless the
	 * targeting computer is drawn to a target of its own). */
	if (s_ck.latched_valid[CK_CMD] && !cmd_separate) {
		const float* cmd = s_ck.rect[CK_CMD];
		if (s_ck.latched_valid[CK_MFD_LEFT] && s_ck.rect[CK_MFD_LEFT][2] > cmd[0] &&
			s_ck.rect[CK_MFD_LEFT][0] < cmd[0]) {
			s_ck.rect[CK_MFD_LEFT][2] = cmd[0];
		}
		if (s_ck.latched_valid[CK_MFD_RIGHT] && s_ck.rect[CK_MFD_RIGHT][0] < cmd[2] &&
			s_ck.rect[CK_MFD_RIGHT][2] > cmd[2]) {
			s_ck.rect[CK_MFD_RIGHT][0] = cmd[2];
		}
	}

	/* The radar pods' frames reach along the top bar towards the centre, so
	 * their rectangles took in the speed/throttle, counters, clock and
	 * message text too: a second copy of those readouts showed in the pods
	 * (most visibly while throttling). Cut each pod away from every text
	 * panel it overlaps, keeping the pod's own instruments (scope, power,
	 * shields/beam) whole and as much of the frame as possible. */
	for (int pod = CK_LEFT_POD; pod <= CK_RIGHT_POD; ++pod) {
		if (!s_ck.latched_valid[pod]) {
			continue;
		}
		float keep[4];
		int keep_valid = 0;
		for (int i = 1; i < 5 && k_widgets[pod][i] != XWA_HUD_WIDGET_COUNT; ++i) {
			float b[4];
			if (XwaRemasterHudFixed_WidgetBounds(k_widgets[pod][i], b)) {
				ck_union(keep, &keep_valid, b);
			}
		}
		static const int k_text[] = { CK_SPEED, CK_COUNTERS, CK_CLOCK, CK_MESSAGES, CK_MESSAGES_LOW, CK_CHARGE };
		for (size_t t = 0; t < sizeof k_text / sizeof k_text[0]; ++t) {
			const int q = k_text[t];
			if (!s_ck.latched_valid[q]) {
				continue;
			}
			float* r = s_ck.rect[pod];
			const float m = 6.0f; /* both panels get 4 px of padding below */
			const float o[4] = { s_ck.rect[q][0] - m, s_ck.rect[q][1] - m, s_ck.rect[q][2] + m, s_ck.rect[q][3] + m };
			if (o[0] >= r[2] || o[2] <= r[0] || o[1] >= r[3] || o[3] <= r[1]) {
				continue;
			}
			/* Candidate cuts: move one pod edge past the text panel. */
			float best_area = -1.0f, cut[4];
			for (int side = 0; side < 4; ++side) {
				float c[4] = { r[0], r[1], r[2], r[3] };
				switch (side) {
				case 0: c[0] = o[2]; break; /* left edge right of it */
				case 1: c[2] = o[0]; break; /* right edge left of it */
				case 2: c[1] = o[3]; break; /* top edge below it */
				default: c[3] = o[1]; break; /* bottom edge above it */
				}
				if (c[2] - c[0] < 8.0f || c[3] - c[1] < 8.0f) {
					continue;
				}
				if (keep_valid && (keep[0] < c[0] - 0.5f || keep[1] < c[1] - 0.5f || keep[2] > c[2] + 0.5f ||
								   keep[3] > c[3] + 0.5f)) {
					continue;
				}
				const float area = (c[2] - c[0]) * (c[3] - c[1]);
				if (area > best_area) {
					best_area = area;
					memcpy(cut, c, sizeof cut);
				}
			}
			if (best_area > 0.0f) {
				memcpy(r, cut, sizeof cut);
			}
		}
	}

	/* Message panels show this frame's text only. Latched, they kept the
	 * width of the longest message so far and took in pieces of the radar
	 * pod frames left and right of a short one. (The pods are still cut
	 * against the latched size above, so they stay put.) */
	for (int p = CK_MESSAGES; p <= CK_MESSAGES_LOW; ++p) {
		if (s_ck.visible[p]) {
			memcpy(s_ck.rect[p], now[p], sizeof s_ck.rect[p]);
		}
	}

	char log[512];
	int log_len = 0;
	int index_of[CK_COUNT];
	for (int p = 0; p < CK_COUNT; ++p) {
		index_of[p] = -1;
	}
	for (int p = 0; p < CK_COUNT && count < max_out; ++p) {
		float* r = s_ck.rect[p];
		const float pad = 4.0f;
		r[0] = fmaxf(0.0f, floorf(r[0] - pad));
		r[1] = fmaxf(0.0f, floorf(r[1] - pad));
		r[2] = fminf((float)target_w, ceilf(r[2] + pad));
		r[3] = fminf((float)target_h, ceilf(r[3] + pad));
		const float w = r[2] - r[0], h = r[3] - r[1];
		if (!s_ck.visible[p] || w < 4.0f || h < 4.0f) {
			s_ck.visible[p] = 0;
			continue;
		}
		index_of[p] = count;
		AeronXrPanel* panel = &out[count++];
		memset(panel, 0, sizeof *panel);
		panel->rect[0] = (int)r[0];
		panel->rect[1] = (int)r[1];
		panel->rect[2] = (int)w;
		panel->rect[3] = (int)h;
		panel->source = p == CK_CMD && cmd_separate ? 1 : 0;
		const CkSpec* spec = &s_ck.spec[p];
		float size_w = spec->box_w, size_h = spec->box_w * h / w;
		if (spec->m_per_unit > 0.0f) {
			const float m_per_px = spec->m_per_unit * 480.0f / (float)target_h;
			size_w = w * m_per_px;
			size_h = h * m_per_px;
		} else if (size_h > spec->box_h) {
			size_h = spec->box_h;
			size_w = spec->box_h * w / h;
		}
		panel->size[0] = size_w / world_scale;
		panel->size[1] = size_h / world_scale;
		ck_pose(spec, world_scale, panel);
		if (log_len < (int)sizeof log - 64) {
			log_len += snprintf(log + log_len, sizeof log - (size_t)log_len, "%s%s %dx%d@%d,%d", log_len ? "; " : "",
								spec->name, (int)w, (int)h, (int)r[0], (int)r[1]);
		}
	}
	/* Hand-placed panels; the readout row follows the targeting computer
	 * unless its panels were placed themselves. */
	for (int p = 0; p < CK_COUNT; ++p) {
		if (index_of[p] >= 0 && s_ck.custom[p] && p != CK_SPEED && p != CK_COUNTERS && p != CK_CLOCK) {
			ck_apply_custom(&out[index_of[p]], p);
		}
	}
	ck_place_row(out, index_of, world_scale, target_h);
	for (int p = CK_COUNTERS; p <= CK_CLOCK; ++p) {
		if (index_of[p] >= 0 && s_ck.custom[p]) {
			ck_apply_custom(&out[index_of[p]], p);
		}
	}
	ck_update_grab(out, index_of);
	/* Show the held panel where the hand put it this frame. */
	for (int hand = 0; hand < 2; ++hand) {
		const int p = s_ck.grab_panel[hand];
		if (p >= 0 && index_of[p] >= 0) {
			ck_apply_custom(&out[index_of[p]], p);
		}
	}
	if (log_len && strcmp(log, s_ck.logged) != 0) {
		memcpy(s_ck.logged, log, sizeof s_ck.logged);
		Aeron_LogInfo("xwa.remaster", "VR cockpit HUD panels (%dx%d HUD): %s", target_w, target_h, log);
	}
	return count;
}

void XwaVrCockpitHud_PrepareOverlay(AeronCommandBuffer* cmd, int target_w, int target_h) {
	s_ck.outline_ready = 0;
	if (!cmd || target_w <= 0 || target_h <= 0) {
		return;
	}
	if (!s_ck.outline) {
		s_ck.outline = AeronDrawList_Create(64);
		s_ck.outline_aux = AeronDrawList_Create(16);
		if (!s_ck.outline || !s_ck.outline_aux) {
			return;
		}
	}
	AeronDrawList_Begin(s_ck.outline, NULL, target_w, target_h, AERON_DRAWLIST2D_LOAD, NULL);
	AeronDrawList_Begin(s_ck.outline_aux, NULL, target_w, target_h, AERON_DRAWLIST2D_LOAD, NULL);
	for (int p = 0; p < CK_COUNT; ++p) {
		const int held = s_ck.grab_panel[0] == p || s_ck.grab_panel[1] == p;
		const int hover = s_ck.hover_panel[0] == p || s_ck.hover_panel[1] == p;
		if (!s_ck.visible[p] || (!held && !hover)) {
			continue;
		}
		const float* r = s_ck.rect[p];
		AeronDrawList2DRRect frame;
		memset(&frame, 0, sizeof frame);
		frame.dst_x = r[0] + 1.0f;
		frame.dst_y = r[1] + 1.0f;
		frame.dst_w = r[2] - r[0] - 2.0f;
		frame.dst_h = r[3] - r[1] - 2.0f;
		frame.radius_px = 6.0f;
		frame.border_px = held ? 3.0f : 2.0f;
		frame.soft_px = 1.0f;
		const float a = held ? 0.95f : 0.7f;
		frame.border[0] = 0.498f * a;
		frame.border[1] = 0.910f * a;
		frame.border[2] = 1.0f * a;
		frame.border[3] = a;
		AeronDrawList_AddRRect(p == CK_CMD && s_ck.cmd_separate ? s_ck.outline_aux : s_ck.outline, &frame);
	}
	s_ck.outline_ready = AeronDrawList_Prepare(s_ck.outline, cmd) && AeronDrawList_Prepare(s_ck.outline_aux, cmd);
}

void XwaVrCockpitHud_RenderOverlay(AeronCommandBuffer* cmd, AeronRenderPass* pass, AeronRenderTarget* target, int aux) {
	AeronDrawList2D* list = aux ? s_ck.outline_aux : s_ck.outline;
	if (list && s_ck.outline_ready && cmd && pass && target) {
		AeronDrawList_RenderIntoPass(list, cmd, pass, target);
	}
}

void XwaVrCockpitHud_Shutdown(void) {
	if (s_ck.outline) {
		AeronDrawList_Destroy(s_ck.outline);
	}
	if (s_ck.outline_aux) {
		AeronDrawList_Destroy(s_ck.outline_aux);
	}
	memset(&s_ck, 0, sizeof s_ck);
}
