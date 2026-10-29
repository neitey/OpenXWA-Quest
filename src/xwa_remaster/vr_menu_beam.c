/* VR menu laser beam; see vr_menu_beam.h. */

#include "xwa_remaster/vr_menu_beam.h"

#include "aeron/aeron.h"
#include <SDL3/SDL_stdinc.h>
#include "aeron/scene/draw_list2d.h"
#include "xwa_remaster/vr_gloves.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The beam texture: a vertical strip in the target's top-left corner, the
 * screen end at the top. */
#define MB_TEX_W 48
#define MB_WIDTH_M 0.016f /* the whole strip; the bright core is a quarter */
#define MB_MIN_LENGTH_M 0.005f /* drawn up close too (a hand right at the console) */

static AeronDrawList2D* s_list;

static float mb_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static int mb_norm(float v[3]) {
	const float l = sqrtf(mb_dot(v, v));
	if (l < 1e-6f) {
		return 0;
	}
	v[0] /= l;
	v[1] /= l;
	v[2] /= l;
	return 1;
}

static void mb_cross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Rotation with columns x, y, z -> quaternion (x, y, z, w). */
static void mb_basis_quat(const float x[3], const float y[3], const float z[3], float q[4]) {
	const float m00 = x[0], m11 = y[1], m22 = z[2];
	const float trace = m00 + m11 + m22;
	if (trace > 0.0f) {
		const float s = sqrtf(trace + 1.0f) * 2.0f;
		q[3] = 0.25f * s;
		q[0] = (y[2] - z[1]) / s;
		q[1] = (z[0] - x[2]) / s;
		q[2] = (x[1] - y[0]) / s;
	} else if (m00 > m11 && m00 > m22) {
		const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		q[3] = (y[2] - z[1]) / s;
		q[0] = 0.25f * s;
		q[1] = (y[0] + x[1]) / s;
		q[2] = (z[0] + x[2]) / s;
	} else if (m11 > m22) {
		const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		q[3] = (z[0] - x[2]) / s;
		q[0] = (y[0] + x[1]) / s;
		q[1] = 0.25f * s;
		q[2] = (z[1] + y[2]) / s;
	} else {
		const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		q[3] = (x[1] - y[0]) / s;
		q[0] = (z[0] + x[2]) / s;
		q[1] = (z[1] + y[2]) / s;
		q[2] = 0.25f * s;
	}
}

static void mb_strip(float x, float w, float radius, float soft, const float top[4], const float bottom[4], int h) {
	AeronDrawList2DRRect r;
	memset(&r, 0, sizeof r);
	r.dst_x = x;
	r.dst_y = 0.0f;
	r.dst_w = w;
	r.dst_h = (float)h;
	r.radius_px = radius;
	r.soft_px = soft;
	memcpy(r.fill_top, top, sizeof r.fill_top);
	memcpy(r.fill_bottom, bottom, sizeof r.fill_bottom);
	AeronDrawList_AddRRect(s_list, &r);
}

int XwaVrBeam_DrawStrips(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height) {
	if (!cmd || !target || width < 2 * MB_TEX_W || height <= 0) {
		return 0;
	}
	if (!s_list) {
		s_list = AeronDrawList_Create(0);
		if (!s_list) {
			return 0;
		}
	}
	AeronDrawList_Begin(s_list, target, width, height, AERON_DRAWLIST2D_CLEAR, NULL);
	/* Premultiplied: a soft cyan glow and a core, strongest at the far end.
	 * Strip 0 is the menu pointer, translucent so the screen shows through
	 * (XWA_XR_BEAM_OPACITY, default 0.5); strip 1 the faint console beams. */
	const char* opacity_env = SDL_getenv("XWA_XR_BEAM_OPACITY");
	float opacity = opacity_env && opacity_env[0] ? (float)atof(opacity_env) : 0.5f;
	opacity = opacity < 0.05f ? 0.05f : opacity > 1.0f ? 1.0f : opacity;
	for (int faint = 0; faint < 2; ++faint) {
		const float k = faint ? 0.35f : opacity;
		const float x = faint ? (float)MB_TEX_W : 0.0f;
		const float glow_top[4] = { 0.10f * 0.45f * k, 0.65f * 0.45f * k, 0.45f * k, 0.45f * k };
		const float glow_bottom[4] = { 0.10f * 0.12f * k, 0.65f * 0.12f * k, 0.12f * k, 0.12f * k };
		const float core_top[4] = { 0.85f * k, 0.97f * k, k, k };
		const float core_bottom[4] = { 0.85f * 0.35f * k, 0.97f * 0.35f * k, 0.35f * k, 0.35f * k };
		mb_strip(x + 4.0f, MB_TEX_W - 8.0f, 20.0f, 16.0f, glow_top, glow_bottom, height);
		mb_strip(x + MB_TEX_W * 0.5f - 6.0f, 12.0f, 6.0f, 3.0f, core_top, core_bottom, height);
	}
	AeronDrawList_Render(s_list, cmd);
	return 1;
}

int XwaVrBeam_Pose(const float start[3], const float end[3], const AeronXrView views[2], int faint, int height,
				   AeronXrPanel* out) {
	float along[3] = { end[0] - start[0], end[1] - start[1], end[2] - start[2] };
	const float length = sqrtf(mb_dot(along, along));
	if (!out || !views || height <= 0 || length < MB_MIN_LENGTH_M || !mb_norm(along)) {
		return 0;
	}
	const float mid[3] = { 0.5f * (start[0] + end[0]), 0.5f * (start[1] + end[1]), 0.5f * (start[2] + end[2]) };
	/* Turned about its own axis to face the eyes. */
	float to_eye[3];
	for (int k = 0; k < 3; ++k) {
		to_eye[k] = 0.5f * (views[0].position[k] + views[1].position[k]) - mid[k];
	}
	const float d = mb_dot(to_eye, along);
	float z[3] = { to_eye[0] - d * along[0], to_eye[1] - d * along[1], to_eye[2] - d * along[2] };
	if (!mb_norm(z)) {
		return 0; /* looking straight down the beam */
	}
	float x[3];
	mb_cross(along, z, x);
	memset(out, 0, sizeof *out);
	out->rect[0] = faint ? MB_TEX_W : 0;
	out->rect[1] = 0;
	out->rect[2] = MB_TEX_W;
	out->rect[3] = height;
	memcpy(out->position, mid, sizeof mid);
	mb_basis_quat(x, along, z, out->orientation);
	out->size[0] = faint ? MB_WIDTH_M * 0.75f : MB_WIDTH_M;
	out->size[1] = length;
	return 1;
}

int XwaVrMenuBeam_Frame(AeronCommandBuffer* cmd, AeronRenderTarget* target, int width, int height,
						const AeronXrView views[2], AeronXrPanel* out) {
	float hit[3], start[3];
	if (!cmd || !target || !views || !out || !Aeron_XrPointerHit(hit)) {
		return 0;
	}
	/* From the glove's fingertip, else from the controller. */
	int pointing = 1;
	if (!XwaVrGloves_IndexTip(1, start, &pointing)) {
		AeronXrHand hand;
		if (!Aeron_XrGetHand(1, &hand)) {
			return 0;
		}
		memcpy(start, hand.position, sizeof start);
	}
	/* It ends on the screen's glass, where the cursor is drawn (see the
	 * frontend's cursor depth). */
	{
		const char* on = SDL_getenv("XWA_XR_CURSOR_GLASS");
		const char* depth_env = SDL_getenv("XWA_XR_CURSOR_DEPTH");
		if (!depth_env || !depth_env[0]) {
			depth_env = SDL_getenv("XWA_XR_FRAME_DEPTH");
		}
		const float depth = depth_env && depth_env[0] ? (float)SDL_atof(depth_env) : 0.02f;
		const float glass_z = hit[2] + depth; /* LOCAL: -z ahead */
		if (!(on && on[0] == '0') && depth > 0.0f && start[2] > glass_z && hit[2] < glass_z) {
			const float f = (glass_z - start[2]) / (hit[2] - start[2]);
			for (int a = 0; a < 3; ++a) {
				hit[a] = start[a] + (hit[a] - start[a]) * f;
			}
		}
	}
	return XwaVrBeam_Pose(start, hit, views, 0, height, out) && XwaVrBeam_DrawStrips(cmd, target, width, height);
}

void XwaVrMenuBeam_Shutdown(void) {
	if (s_list) {
		AeronDrawList_Destroy(s_list);
		s_list = NULL;
	}
}
