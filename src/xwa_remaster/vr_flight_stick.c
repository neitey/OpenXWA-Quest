/* VR flight stick; see vr_flight_stick.h. */

#include "xwa_remaster/vr_flight_stick.h"
#include <SDL3/SDL_stdinc.h>

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/scene/gltf_mesh.h"
#include "aeron/scene/mesh.h"
#include "xwa_remaster/vr_lap_console.h"
#include "xwa_runtime/input/controller_mapping.h"

#include <SDL3/SDL_timer.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Geometry, metres, stick space: y up along the stick, origin at the
 * pivot, x right, z back (OpenXR axes). */
#define ST_GRIP_CENTER_M 0.18f
#define ST_GRIP_LOW_M 0.12f
#define ST_GRIP_TOP_M 0.235f
#define ST_GRIP_R_M 0.0165f
#define ST_SHAFT_R_M 0.008f
#define ST_REACH_M 0.07f   /* palm to the grip's axis to take it */
#define ST_LEAVE_M 0.09f   /* ... and to count as away again */
#define ST_MAX_DEG 20.0f   /* full deflection */
#define ST_DEAD_DEG 1.2f   /* centre dead zone */
#define ST_TWIST_DEG 30.0f /* full roll with XWA_XR_STICK_TWIST */
#define ST_TWIST_DEAD_DEG 4.0f
#define ST_GRIP_ON 0.6f
#define ST_GRIP_OFF 0.35f
#define ST_SPRING_S 0.06f
#define ST_PI 3.14159265358979f

/* Default pivot in LOCAL space (the recentred head at the origin): right
 * of the seat centre, between the knees, the grip a hand's width above the
 * thighs and below the lap console's near edge. */
static const float k_default_pivot[3] = { 0.12f, -0.70f, -0.32f };

typedef struct StMesh {
	AeronGltfVertex* vertices;
	uint16_t* indices;
	uint32_t vertex_count, index_count, vertex_cap, index_cap;
	AeronGltfMaterial material;
	uint32_t prim_material;
	AeronSceneMesh* mesh;
} StMesh;

enum { ST_BOOT, ST_SHAFT, ST_GRIP, ST_MESHES };

static struct {
	int tried, ok;
	StMesh meshes[ST_MESHES];
	float pivot[3];
	float fwd, side, twist; /* shown tilt (radians) and twist */
	int held;               /* right hand flies it */
	float grab_offset[3];   /* palm - grip centre at the grab */
	float grab_yaw;
	int moving;             /* left hand moves it */
	float move_offset[3];   /* palm - pivot */
	int hover[2];
	float grip_prev[2];
	int updated, shown;
	int dirty;
	char placement[64];
	uint64_t last_ns;
	int logged;
	/* XwaVrStick_Mesh: this frame's stick in LOCAL space. */
	float mesh_q[4];
	float* mesh_xyz;
	float* mesh_nrm;
	uint16_t* mesh_idx;
	int mesh_vn, mesh_in, mesh_ready;
} s_st;

/* ---- small math --------------------------------------------------------- */

static float st_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void st_rotate(const float q[4], const float v[3], float out[3]) {
	const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

static void st_qmul(const float a[4], const float b[4], float out[4]) {
	const float r[4] = {
		a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
		a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
		a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
		a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
	};
	memcpy(out, r, sizeof r);
}

/* The stick's axis for a forward and sideways tilt. */
static void st_axis(float fwd, float side, float d[3]) {
	d[0] = tanf(side);
	d[1] = 1.0f;
	d[2] = -tanf(fwd);
	const float l = sqrtf(st_dot(d, d));
	d[0] /= l;
	d[1] /= l;
	d[2] /= l;
}

/* Rotation taking +Y to `d`, then turned `twist` about it. */
static void st_stick_quat(const float d[3], float twist, float q[4]) {
	/* Shortest arc from (0,1,0). */
	const float c = d[1];
	float axis[3] = { d[2], 0.0f, -d[0] }; /* up x d */
	const float s = sqrtf(axis[0] * axis[0] + axis[2] * axis[2]);
	if (s < 1e-6f) {
		q[0] = q[1] = q[2] = 0.0f;
		q[3] = 1.0f;
	} else {
		const float angle = atan2f(s, c);
		const float k = sinf(0.5f * angle) / s;
		q[0] = axis[0] * k;
		q[1] = 0.0f;
		q[2] = axis[2] * k;
		q[3] = cosf(0.5f * angle);
	}
	if (twist != 0.0f) {
		/* Twist about the stick's own axis: local Y, applied first. */
		const float t[4] = { 0.0f, sinf(-0.5f * twist), 0.0f, cosf(-0.5f * twist) };
		st_qmul(q, t, q);
	}
}

static float st_env(const char* name, float fallback) {
	const char* v = SDL_getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

int XwaVrStick_Enabled(void) {
	const char* env = SDL_getenv("XWA_XR_FLIGHT_STICK");
	return !(env && env[0] == '0');
}

/* The pivot from XWA_XR_STICK_POS (read every frame, so the VR options'
 * reset applies at once), else the default spot. */
static void st_place(void) {
	if (s_st.moving) {
		return;
	}
	memcpy(s_st.pivot, k_default_pivot, sizeof s_st.pivot);
	const char* v = SDL_getenv("XWA_XR_STICK_POS");
	float p[3];
	if (v && v[0] && sscanf(v, "%f,%f,%f", &p[0], &p[1], &p[2]) == 3 && fabsf(p[0]) < 2.0f && fabsf(p[1]) < 2.0f &&
		fabsf(p[2]) < 2.0f) {
		memcpy(s_st.pivot, p, sizeof p);
	}
}

/* ---- meshes ---------------------------------------------------------------- */

static int st_reserve(StMesh* m, uint32_t verts, uint32_t idx) {
	if (m->vertex_count + verts > m->vertex_cap) {
		const uint32_t cap = (m->vertex_count + verts) * 2u;
		AeronGltfVertex* nv = (AeronGltfVertex*)realloc(m->vertices, cap * sizeof *nv);
		if (!nv) {
			return 0;
		}
		m->vertices = nv;
		m->vertex_cap = cap;
	}
	if (m->index_count + idx > m->index_cap) {
		const uint32_t cap = (m->index_count + idx) * 2u;
		uint16_t* ni = (uint16_t*)realloc(m->indices, cap * sizeof *ni);
		if (!ni) {
			return 0;
		}
		m->indices = ni;
		m->index_cap = cap;
	}
	return 1;
}

static void st_vertex(StMesh* m, float x, float y, float z, float nx, float ny, float nz) {
	AeronGltfVertex* v = &m->vertices[m->vertex_count++];
	memset(v, 0, sizeof *v);
	v->pos[0] = x;
	v->pos[1] = y;
	v->pos[2] = z;
	const float l = sqrtf(nx * nx + ny * ny + nz * nz);
	v->normal[0] = l > 0.0f ? nx / l : 0.0f;
	v->normal[1] = l > 0.0f ? ny / l : 1.0f;
	v->normal[2] = l > 0.0f ? nz / l : 0.0f;
	v->tangent[0] = 1.0f;
	v->tangent[3] = 1.0f;
}

/* A ring strip from (y0, r0) to (y1, r1) about the Y axis. */
static int st_frustum(StMesh* m, float y0, float r0, float y1, float r1, int segs) {
	if (!st_reserve(m, (uint32_t)(2 * (segs + 1)), (uint32_t)(6 * segs))) {
		return 0;
	}
	const uint16_t base = (uint16_t)m->vertex_count;
	const float slope = (r0 - r1) / fmaxf(1e-6f, y1 - y0);
	for (int i = 0; i <= segs; ++i) {
		const float a = 2.0f * ST_PI * (float)i / (float)segs;
		const float c = cosf(a), s = sinf(a);
		st_vertex(m, r0 * c, y0, r0 * s, c, slope, s);
		st_vertex(m, r1 * c, y1, r1 * s, c, slope, s);
	}
	for (int i = 0; i < segs; ++i) {
		const uint16_t a = (uint16_t)(base + 2 * i), b = (uint16_t)(a + 1), c = (uint16_t)(a + 2),
					   d = (uint16_t)(a + 3);
		uint16_t* o = &m->indices[m->index_count];
		o[0] = a, o[1] = b, o[2] = c, o[3] = c, o[4] = b, o[5] = d;
		m->index_count += 6;
	}
	return 1;
}

/* A flat disc at `y` facing up (+1) or down (-1). */
static int st_disc(StMesh* m, float y, float r, float facing, int segs) {
	if (!st_reserve(m, (uint32_t)(segs + 2), (uint32_t)(3 * segs))) {
		return 0;
	}
	const uint16_t centre = (uint16_t)m->vertex_count;
	st_vertex(m, 0.0f, y, 0.0f, 0.0f, facing, 0.0f);
	for (int i = 0; i <= segs; ++i) {
		const float a = 2.0f * ST_PI * (float)i / (float)segs;
		st_vertex(m, r * cosf(a), y, r * sinf(a), 0.0f, facing, 0.0f);
	}
	for (int i = 0; i < segs; ++i) {
		uint16_t* o = &m->indices[m->index_count];
		o[0] = centre;
		o[1] = facing > 0.0f ? (uint16_t)(centre + 2 + i) : (uint16_t)(centre + 1 + i);
		o[2] = facing > 0.0f ? (uint16_t)(centre + 1 + i) : (uint16_t)(centre + 2 + i);
		m->index_count += 3;
	}
	return 1;
}

/* A dome (upper half sphere) on top of a cylinder of radius r at y. */
static int st_dome(StMesh* m, float y, float r, int segs, int rings) {
	for (int k = 0; k < rings; ++k) {
		const float a0 = 0.5f * ST_PI * (float)k / (float)rings;
		const float a1 = 0.5f * ST_PI * (float)(k + 1) / (float)rings;
		if (!st_reserve(m, (uint32_t)(2 * (segs + 1)), (uint32_t)(6 * segs))) {
			return 0;
		}
		const uint16_t base = (uint16_t)m->vertex_count;
		for (int i = 0; i <= segs; ++i) {
			const float a = 2.0f * ST_PI * (float)i / (float)segs;
			const float c = cosf(a), s = sinf(a);
			st_vertex(m, r * cosf(a0) * c, y + r * sinf(a0), r * cosf(a0) * s, cosf(a0) * c, sinf(a0), cosf(a0) * s);
			st_vertex(m, r * cosf(a1) * c, y + r * sinf(a1), r * cosf(a1) * s, cosf(a1) * c, sinf(a1), cosf(a1) * s);
		}
		for (int i = 0; i < segs; ++i) {
			const uint16_t a = (uint16_t)(base + 2 * i), b = (uint16_t)(a + 1), c = (uint16_t)(a + 2),
						   d = (uint16_t)(a + 3);
			uint16_t* o = &m->indices[m->index_count];
			o[0] = a, o[1] = b, o[2] = c, o[3] = c, o[4] = b, o[5] = d;
			m->index_count += 6;
		}
	}
	return 1;
}

static void st_material(AeronGltfMaterial* m, float r, float g, float b, float metallic, float roughness) {
	memset(m, 0, sizeof *m);
	m->base_color_factor[0] = r;
	m->base_color_factor[1] = g;
	m->base_color_factor[2] = b;
	m->base_color_factor[3] = 1.0f;
	m->emissive_strength = 1.0f;
	m->metallic_factor = metallic;
	m->roughness_factor = roughness;
	m->double_sided = 1;
	m->alpha_mode = AERON_GLTF_ALPHA_OPAQUE;
	m->alpha_cutoff = 0.5f;
}

static AeronSceneMesh* st_upload(AeronCommandBuffer* cmd, StMesh* d, const char* name) {
	AeronFlightModel fm;
	memset(&fm, 0, sizeof fm);
	fm.render.vertices = d->vertices;
	fm.render.vertex_count = d->vertex_count;
	fm.render.indices = d->indices;
	fm.render.index_count = d->index_count;
	fm.render.opaque_index_count = d->index_count;
	fm.render.mask_index_offset = d->index_count;
	fm.render.blend_index_offset = d->index_count;
	fm.render.material_count = 1;
	fm.render.materials = &d->material;
	fm.render.variant_slots = 1;
	fm.render.total_prim_count = 1;
	d->prim_material = 0;
	fm.render.prim_variant_material = &d->prim_material;
	float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
	for (uint32_t i = 0; i < d->vertex_count; ++i) {
		for (int k = 0; k < 3; ++k) {
			mn[k] = fminf(mn[k], d->vertices[i].pos[k]);
			mx[k] = fmaxf(mx[k], d->vertices[i].pos[k]);
		}
	}
	fm.bounds.min.x = mn[0];
	fm.bounds.min.y = mn[1];
	fm.bounds.min.z = mn[2];
	fm.bounds.max.x = mx[0];
	fm.bounds.max.y = mx[1];
	fm.bounds.max.z = mx[2];
	fm.max_extent = fmaxf(fmaxf(mx[0] - mn[0], mx[1] - mn[1]), mx[2] - mn[2]);
	AeronSceneMeshCreateStatus status;
	return AeronScene_MeshCreate(cmd, &fm, name, &status);
}

static int st_ensure(AeronCommandBuffer* cmd) {
	if (s_st.tried) {
		return s_st.ok;
	}
	s_st.tried = 1;
	const int segs = 24;
	StMesh* boot = &s_st.meshes[ST_BOOT];
	StMesh* shaft = &s_st.meshes[ST_SHAFT];
	StMesh* grip = &s_st.meshes[ST_GRIP];
	int ok = 1;
	/* Base plate and rubber boot (stays upright). */
	ok = ok && st_frustum(boot, -0.016f, 0.060f, -0.006f, 0.060f, segs) && st_disc(boot, -0.006f, 0.060f, 1.0f, segs);
	ok = ok && st_frustum(boot, -0.006f, 0.042f, 0.012f, 0.034f, segs) &&
		 st_frustum(boot, 0.012f, 0.034f, 0.030f, 0.024f, segs) &&
		 st_frustum(boot, 0.030f, 0.024f, 0.046f, 0.012f, segs);
	/* Shaft. */
	ok = ok && st_frustum(shaft, 0.0f, ST_SHAFT_R_M, ST_GRIP_LOW_M + 0.004f, ST_SHAFT_R_M, 12);
	/* Grip: a collar, the handle widening slightly to the top, a dome. */
	ok = ok && st_frustum(grip, ST_GRIP_LOW_M - 0.006f, 0.011f, ST_GRIP_LOW_M, ST_GRIP_R_M - 0.002f, segs) &&
		 st_frustum(grip, ST_GRIP_LOW_M, ST_GRIP_R_M - 0.002f, ST_GRIP_TOP_M, ST_GRIP_R_M, segs) &&
		 st_dome(grip, ST_GRIP_TOP_M, ST_GRIP_R_M, segs, 6) && st_disc(grip, ST_GRIP_LOW_M - 0.006f, 0.011f, -1.0f, segs);
	if (ok) {
		st_material(&boot->material, 0.018f, 0.018f, 0.020f, 0.0f, 0.85f);
		st_material(&shaft->material, 0.35f, 0.36f, 0.38f, 0.9f, 0.35f);
		st_material(&grip->material, 0.030f, 0.030f, 0.032f, 0.0f, 0.7f);
		boot->mesh = st_upload(cmd, boot, "vr.stick.boot");
		shaft->mesh = st_upload(cmd, shaft, "vr.stick.shaft");
		grip->mesh = st_upload(cmd, grip, "vr.stick.grip");
		ok = boot->mesh && shaft->mesh && grip->mesh;
	}
	if (!ok) {
		Aeron_LogWarn("xwa.remaster", "VR flight stick: mesh setup failed, stick is off");
	}
	s_st.ok = ok;
	return ok;
}

/* ---- update ---------------------------------------------------------------- */

/* The palm centre of a hand, LOCAL: the glove's palm capsule when shown,
 * else a point a little behind the controller's aim origin. */
static void st_palm(int hand, const AeronXrHand* xh, float out[3]) {
	XwaVrGloveCapsule caps[24];
	const int n = XwaVrGloves_Capsules(hand, caps, 24);
	if (n > 0) {
		const XwaVrGloveCapsule* palm = &caps[n - 1];
		for (int k = 0; k < 3; ++k) {
			out[k] = 0.5f * (palm->a[k] + palm->b[k]);
		}
		return;
	}
	static const float back[3] = { 0.0f, -0.02f, 0.07f };
	float o[3];
	st_rotate(xh->orientation, back, o);
	for (int k = 0; k < 3; ++k) {
		out[k] = xh->position[k] + o[k];
	}
}

/* Distance from p to the grip's axis segment at the shown tilt. */
static float st_grip_distance(const float p[3], float centre[3]) {
	float d[3];
	st_axis(s_st.fwd, s_st.side, d);
	float lo[3], hi[3];
	for (int k = 0; k < 3; ++k) {
		lo[k] = s_st.pivot[k] + d[k] * ST_GRIP_LOW_M;
		hi[k] = s_st.pivot[k] + d[k] * ST_GRIP_TOP_M;
		centre[k] = s_st.pivot[k] + d[k] * ST_GRIP_CENTER_M;
	}
	const float seg[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
	const float rel[3] = { p[0] - lo[0], p[1] - lo[1], p[2] - lo[2] };
	float t = st_dot(rel, seg) / fmaxf(1e-6f, st_dot(seg, seg));
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	const float q[3] = { rel[0] - seg[0] * t, rel[1] - seg[1] * t, rel[2] - seg[2] * t };
	return sqrtf(st_dot(q, q));
}

/* The hand's heading about the vertical (twisting the stick). */
static float st_hand_yaw(const AeronXrHand* xh) {
	static const float fwd[3] = { 0.0f, 0.0f, -1.0f };
	float f[3];
	st_rotate(xh->orientation, fwd, f);
	return atan2f(f[0], -f[2]);
}

static float st_wrap(float a) {
	while (a > ST_PI) {
		a -= 2.0f * ST_PI;
	}
	while (a < -ST_PI) {
		a += 2.0f * ST_PI;
	}
	return a;
}

/* Tilt -> -1..1 with a centre dead zone. */
/* The stick's deflection to Aeron (it drives the right thumbstick's axes)
 * and to the controller mapping (the axes it drives skip the thumbstick
 * response curve: the stick stays linear). */
static void st_set_virtual_stick(int held, float x, float y, float roll) {
	Aeron_XrSetVirtualStick(held, x, y, roll);
	XwaControllerMapping_SetXrVirtualStick(held, x, y, roll);
}

static float st_axis_value(float angle, float max_rad, float dead_rad) {
	const float a = fabsf(angle);
	if (a <= dead_rad) {
		return 0.0f;
	}
	float v = (a - dead_rad) / (max_rad - dead_rad);
	v = v > 1.0f ? 1.0f : v;
	return angle < 0.0f ? -v : v;
}

void XwaVrStick_Update(void) {
	const uint64_t now_ns = SDL_GetTicksNS();
	float dt = s_st.last_ns ? (float)((double)(now_ns - s_st.last_ns) * 1e-9) : 0.0f;
	s_st.last_ns = now_ns;
	dt = dt > 0.1f ? 0.1f : dt;
	if (!XwaVrStick_Enabled()) {
		return;
	}
	st_place();
	s_st.updated = 1;
	const float max_rad = fminf(fmaxf(st_env("XWA_XR_STICK_ANGLE", ST_MAX_DEG), 5.0f), 45.0f) * ST_PI / 180.0f;
	const int twist_on = st_env("XWA_XR_STICK_TWIST", 0.0f) >= 0.5f;

	AeronXrHand hands[2];
	float palm[2][3];
	int valid[2];
	for (int h = 0; h < 2; ++h) {
		valid[h] = Aeron_XrGetHand(h, &hands[h]);
		if (valid[h]) {
			st_palm(h, &hands[h], palm[h]);
		}
	}

	/* Right hand: take, fly, let go. */
	{
		const int h = 1;
		float centre[3];
		const float dist = valid[h] ? st_grip_distance(palm[h], centre) : 1e9f;
		const int near = valid[h] && dist < (s_st.hover[h] ? ST_LEAVE_M : ST_REACH_M) && !s_st.moving;
		const float grip = valid[h] ? hands[h].grip : 0.0f;
		if (s_st.held) {
			if (!valid[h] || grip < ST_GRIP_OFF) {
				s_st.held = 0;
				Aeron_XrHapticPulse(h, 0.25f, 15);
			}
		} else if (near && grip >= ST_GRIP_ON && s_st.grip_prev[h] < ST_GRIP_OFF) {
			s_st.held = 1;
			for (int k = 0; k < 3; ++k) {
				s_st.grab_offset[k] = palm[h][k] - centre[k];
			}
			s_st.grab_yaw = st_hand_yaw(&hands[h]) - s_st.twist;
			Aeron_XrHapticPulse(h, 0.5f, 25);
		}
		if (near && !s_st.hover[h] && !s_st.held) {
			Aeron_XrHapticPulse(h, 0.12f, 8); /* "you can take it" */
		}
		s_st.hover[h] = near;
		s_st.grip_prev[h] = grip;
	}

	/* Left hand: move the whole stick. */
	{
		const int h = 0;
		float centre[3];
		const float dist = valid[h] ? st_grip_distance(palm[h], centre) : 1e9f;
		const int near = valid[h] && dist < (s_st.hover[h] ? ST_LEAVE_M : ST_REACH_M) && !s_st.held;
		const float grip = valid[h] ? hands[h].grip : 0.0f;
		if (s_st.moving) {
			if (!valid[h] || grip < ST_GRIP_OFF) {
				s_st.moving = 0;
				snprintf(s_st.placement, sizeof s_st.placement, "%.3f,%.3f,%.3f", s_st.pivot[0], s_st.pivot[1],
						 s_st.pivot[2]);
				SDL_setenv_unsafe("XWA_XR_STICK_POS", s_st.placement, 1);
				s_st.dirty = 1;
				Aeron_XrHapticPulse(h, 0.3f, 20);
			} else {
				for (int k = 0; k < 3; ++k) {
					s_st.pivot[k] = palm[h][k] - s_st.move_offset[k];
				}
			}
		} else if (near && grip >= ST_GRIP_ON && s_st.grip_prev[h] < ST_GRIP_OFF) {
			s_st.moving = 1;
			for (int k = 0; k < 3; ++k) {
				s_st.move_offset[k] = palm[h][k] - s_st.pivot[k];
			}
			Aeron_XrHapticPulse(h, 0.6f, 30);
		}
		s_st.hover[h] = near;
		s_st.grip_prev[h] = grip;
	}

	/* Tilt: follows the held hand like a lever, else springs back. */
	float roll = 0.0f;
	if (s_st.held) {
		float v[3];
		for (int k = 0; k < 3; ++k) {
			v[k] = palm[1][k] - s_st.grab_offset[k] - s_st.pivot[k];
		}
		const float up = fmaxf(v[1], 0.02f);
		float fwd = atan2f(-v[2], up);
		float side = atan2f(v[0], up);
		fwd = fwd > max_rad ? max_rad : fwd < -max_rad ? -max_rad : fwd;
		side = side > max_rad ? max_rad : side < -max_rad ? -max_rad : side;
		s_st.fwd = fwd;
		s_st.side = side;
		if (twist_on) {
			const float tmax = ST_TWIST_DEG * ST_PI / 180.0f;
			float tw = st_wrap(st_hand_yaw(&hands[1]) - s_st.grab_yaw);
			tw = tw > tmax ? tmax : tw < -tmax ? -tmax : tw;
			s_st.twist = tw;
			roll = st_axis_value(tw, tmax, ST_TWIST_DEAD_DEG * ST_PI / 180.0f);
		} else {
			s_st.twist = 0.0f;
		}
	} else {
		const float k = dt > 0.0f ? expf(-dt / ST_SPRING_S) : 0.0f;
		s_st.fwd *= k;
		s_st.side *= k;
		s_st.twist *= k;
	}
	const float dead = ST_DEAD_DEG * ST_PI / 180.0f;
	st_set_virtual_stick(s_st.held, st_axis_value(s_st.side, max_rad, dead), st_axis_value(s_st.fwd, max_rad, dead),
						 roll);
	for (int h = 0; h < 2; ++h) {
		if (s_st.hover[h] || (h == 1 && s_st.held) || (h == 0 && s_st.moving)) {
			XwaVrConsole_CaptureGrip(h); /* the grip takes the stick, not a game action */
		}
	}
	if (!s_st.logged) {
		Aeron_LogInfo("xwa.remaster", "VR flight stick at %.2f,%.2f,%.2f (max %.0f deg, twist roll %s)",
					  (double)s_st.pivot[0], (double)s_st.pivot[1], (double)s_st.pivot[2],
					  (double)(max_rad * 180.0f / ST_PI), twist_on ? "on" : "off");
		s_st.logged = 1;
	}
}

/* ---- drawing --------------------------------------------------------------- */

static void st_instance(AeronScene3D* scene, const AeronSceneMesh* mesh, const float q[4], const float pos[3],
						const float crows[9], const float cam_pos[3], float units) {
	/* scene = cam + units * rt_c * (R * p + pos), rt_c: LOCAL -> world axes. */
	float rt_c[9];
	for (int i = 0; i < 3; ++i) {
		rt_c[i * 3 + 0] = crows[0 * 3 + i];
		rt_c[i * 3 + 1] = -crows[1 * 3 + i];
		rt_c[i * 3 + 2] = -crows[2 * 3 + i];
	}
	float r[9];
	{
		static const float ex[3] = { 1, 0, 0 }, ey[3] = { 0, 1, 0 }, ez[3] = { 0, 0, 1 };
		float cx[3], cy[3], cz[3];
		st_rotate(q, ex, cx);
		st_rotate(q, ey, cy);
		st_rotate(q, ez, cz);
		for (int i = 0; i < 3; ++i) {
			r[i * 3 + 0] = cx[i];
			r[i * 3 + 1] = cy[i];
			r[i * 3 + 2] = cz[i];
		}
	}
	AeronSceneMeshInstance inst;
	memset(&inst, 0, sizeof inst);
	inst.mesh = mesh;
	for (int i = 0; i < 3; ++i) {
		float t = 0.0f;
		for (int j = 0; j < 3; ++j) {
			float v = 0.0f;
			for (int k = 0; k < 3; ++k) {
				v += rt_c[i * 3 + k] * r[k * 3 + j];
			}
			inst.transform[i * 4 + j] = v * units;
			t += rt_c[i * 3 + j] * pos[j];
		}
		inst.transform[i * 4 + 3] = cam_pos[i] + t * units;
	}
	inst.transform[15] = 1.0f;
	memcpy(inst.prev_transform, inst.transform, sizeof inst.transform);
	inst.zero_velocity = 1;
	inst.draw_first = 1; /* hides cockpit: drawn first, the cockpit behind it is not shaded */
	inst.no_local_lights = 1;
	/* Never in the bloom: with one bloom for both eyes it would glow beside
	 * them in the right eye (doubled), so they stay just below the bloom
	 * threshold (as the cockpit does, flight.c fl_xr_cockpit_bloom_cap). */
	inst.base_color_emissive_strength = -0.98f;
	inst.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW |
						AERON_SCENE_INSTANCE_EXCLUDE_FROM_RECEIVER_LOCAL_SHADOW;
	AeronScene_AddMeshInstance(scene, &inst);
}

void XwaVrStick_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const float crows[9], const float cam_pos[3],
					   float units) {
	s_st.shown = 0;
	if (!scene || !cmd || !crows || !cam_pos || units <= 0.0f || !XwaVrStick_Enabled() || !s_st.updated ||
		!st_ensure(cmd)) {
		return;
	}
	static const float upright[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float d[3], q[4];
	st_axis(s_st.fwd, s_st.side, d);
	st_stick_quat(d, s_st.twist, q);
	st_instance(scene, s_st.meshes[ST_BOOT].mesh, upright, s_st.pivot, crows, cam_pos, units);
	st_instance(scene, s_st.meshes[ST_SHAFT].mesh, q, s_st.pivot, crows, cam_pos, units);
	st_instance(scene, s_st.meshes[ST_GRIP].mesh, q, s_st.pivot, crows, cam_pos, units);
	s_st.shown = 1;
	s_st.mesh_ready = 0; /* XwaVrStick_Mesh builds it when asked */
	memcpy(s_st.mesh_q, q, sizeof s_st.mesh_q);
}

int XwaVrStick_Mesh(const float** local_xyz, const float** local_normals, int* vertex_count,
					const uint16_t** indices, int* index_count) {
	if (!s_st.shown || !s_st.ok) {
		return 0;
	}
	if (!s_st.mesh_ready) {
		int vn = 0, in = 0;
		for (int m = 0; m < ST_MESHES; ++m) {
			vn += (int)s_st.meshes[m].vertex_count;
			in += (int)s_st.meshes[m].index_count;
		}
		if (vn > 65535) {
			return 0;
		}
		if (!s_st.mesh_xyz) {
			s_st.mesh_xyz = (float*)malloc((size_t)vn * 3 * sizeof(float));
			s_st.mesh_nrm = (float*)malloc((size_t)vn * 3 * sizeof(float));
			s_st.mesh_idx = (uint16_t*)malloc((size_t)in * sizeof(uint16_t));
			if (!s_st.mesh_xyz || !s_st.mesh_nrm || !s_st.mesh_idx) {
				free(s_st.mesh_xyz);
				free(s_st.mesh_nrm);
				free(s_st.mesh_idx);
				s_st.mesh_xyz = s_st.mesh_nrm = NULL;
				s_st.mesh_idx = NULL;
				return 0;
			}
		}
		static const float upright[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		int v0 = 0, i0 = 0;
		for (int m = 0; m < ST_MESHES; ++m) {
			const StMesh* sm = &s_st.meshes[m];
			const float* q = m == ST_BOOT ? upright : s_st.mesh_q;
			for (uint32_t i = 0; i < sm->vertex_count; ++i) {
				float p[3], n[3];
				st_rotate(q, sm->vertices[i].pos, p);
				st_rotate(q, sm->vertices[i].normal, n);
				for (int k = 0; k < 3; ++k) {
					s_st.mesh_xyz[(v0 + (int)i) * 3 + k] = p[k] + s_st.pivot[k];
					s_st.mesh_nrm[(v0 + (int)i) * 3 + k] = n[k];
				}
			}
			for (uint32_t i = 0; i < sm->index_count; ++i) {
				s_st.mesh_idx[i0 + (int)i] = (uint16_t)(sm->indices[i] + v0);
			}
			v0 += (int)sm->vertex_count;
			i0 += (int)sm->index_count;
		}
		s_st.mesh_vn = v0;
		s_st.mesh_in = i0;
		s_st.mesh_ready = 1;
	}
	*local_xyz = s_st.mesh_xyz;
	*local_normals = s_st.mesh_nrm;
	*vertex_count = s_st.mesh_vn;
	*indices = s_st.mesh_idx;
	*index_count = s_st.mesh_in;
	return 1;
}

int XwaVrStick_Capsules(XwaVrGloveCapsule* out, int max_out) {
	if (!s_st.shown || !out || max_out < 2) {
		return 0;
	}
	float d[3];
	st_axis(s_st.fwd, s_st.side, d);
	for (int k = 0; k < 3; ++k) {
		out[0].a[k] = s_st.pivot[k];
		out[0].b[k] = s_st.pivot[k] + d[k] * ST_GRIP_LOW_M;
		out[1].a[k] = s_st.pivot[k] + d[k] * ST_GRIP_LOW_M;
		out[1].b[k] = s_st.pivot[k] + d[k] * ST_GRIP_TOP_M;
	}
	out[0].radius = 0.03f;
	out[1].radius = ST_GRIP_R_M + 0.006f;
	return 2;
}

void XwaVrStick_EndFrame(void) {
	if (!s_st.updated) {
		if (s_st.held || s_st.moving) {
			s_st.held = 0;
			s_st.moving = 0;
		}
		s_st.hover[0] = s_st.hover[1] = 0;
		s_st.fwd = s_st.side = s_st.twist = 0.0f;
		st_set_virtual_stick(0, 0.0f, 0.0f, 0.0f);
		s_st.shown = 0;
	}
	s_st.updated = 0;
}

int XwaVrStick_TakePlacement(char* out, int size) {
	if (!s_st.dirty || !out || size <= 0) {
		return 0;
	}
	snprintf(out, (size_t)size, "%s", s_st.placement);
	s_st.dirty = 0;
	return 1;
}

void XwaVrStick_Shutdown(void) {
	st_set_virtual_stick(0, 0.0f, 0.0f, 0.0f);
	for (int i = 0; i < ST_MESHES; ++i) {
		if (s_st.meshes[i].mesh) {
			AeronScene_MeshDestroy(s_st.meshes[i].mesh);
		}
		free(s_st.meshes[i].vertices);
		free(s_st.meshes[i].indices);
	}
	free(s_st.mesh_xyz);
	free(s_st.mesh_nrm);
	free(s_st.mesh_idx);
	memset(&s_st, 0, sizeof s_st);
}
