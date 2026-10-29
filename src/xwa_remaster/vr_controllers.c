/* VR controller models; see vr_controllers.h.
 *
 * The generic controller is built here from a few shapes (a rounded head
 * with a thumbstick and two buttons, a handle, a trigger and a grip
 * button), in the controller's aim space: +X right, +Y up, -Z where it
 * points; the left one is the mirror image. It is our own simple design.
 *
 * A runtime model is a glTF binary: its meshes are merged into one scene
 * mesh. Parts under the runtime's animatable nodes (XR_EXT models) get a
 * mesh slot of their own and are moved through the instance's mesh table;
 * the rest is baked. Base colour textures are packed into one atlas. */

#include "xwa_remaster/vr_controllers.h"
#include "xwa_remaster/vr_ktx2_decode.h"

#include "aeron/aeron.h"
#include "aeron/asset/flight_model.h"
#include "aeron/log.h"
#include "aeron/scene/gltf_mesh.h"
#include "aeron/scene/mesh.h"

#include "cgltf.h"
#include "ktx2_writer.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "meshoptimizer.h"

#include <SDL3/SDL_system.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CT_PI 3.14159265358979f

/* ---- Math ---------------------------------------------------------------- */

static void ct_rotate(const float q[4], const float v[3], float out[3]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	const float tx = 2.0f * (y * v[2] - z * v[1]);
	const float ty = 2.0f * (z * v[0] - x * v[2]);
	const float tz = 2.0f * (x * v[1] - y * v[0]);
	out[0] = v[0] + w * tx + (y * tz - z * ty);
	out[1] = v[1] + w * ty + (z * tx - x * tz);
	out[2] = v[2] + w * tz + (x * ty - y * tx);
}

static void ct_norm(float v[3]) {
	const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-12f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

static void ct_cross(const float a[3], const float b[3], float out[3]) {
	const float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	memcpy(out, r, sizeof r);
}

/* Column-major 4x4 (glTF's layout): m[col * 4 + row]. */
static void m4_identity(float m[16]) {
	memset(m, 0, 16 * sizeof(float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void m4_mul(const float a[16], const float b[16], float out[16]) {
	float r[16];
	for (int c = 0; c < 4; ++c) {
		for (int rr = 0; rr < 4; ++rr) {
			r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] + a[2 * 4 + rr] * b[c * 4 + 2] +
							a[3 * 4 + rr] * b[c * 4 + 3];
		}
	}
	memcpy(out, r, sizeof r);
}

static void m4_point(const float m[16], const float p[3], float out[3]) {
	float r[3];
	for (int i = 0; i < 3; ++i) {
		r[i] = m[0 * 4 + i] * p[0] + m[1 * 4 + i] * p[1] + m[2 * 4 + i] * p[2] + m[3 * 4 + i];
	}
	memcpy(out, r, sizeof r);
}

static void m4_vector(const float m[16], const float v[3], float out[3]) {
	float r[3];
	for (int i = 0; i < 3; ++i) {
		r[i] = m[0 * 4 + i] * v[0] + m[1 * 4 + i] * v[1] + m[2 * 4 + i] * v[2];
	}
	memcpy(out, r, sizeof r);
}

static void m4_from_pose(const float t[3], const float q[4], float m[16]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	m[0] = 1 - 2 * (y * y + z * z);
	m[1] = 2 * (x * y + w * z);
	m[2] = 2 * (x * z - w * y);
	m[3] = 0;
	m[4] = 2 * (x * y - w * z);
	m[5] = 1 - 2 * (x * x + z * z);
	m[6] = 2 * (y * z + w * x);
	m[7] = 0;
	m[8] = 2 * (x * z + w * y);
	m[9] = 2 * (y * z - w * x);
	m[10] = 1 - 2 * (x * x + y * y);
	m[11] = 0;
	m[12] = t[0];
	m[13] = t[1];
	m[14] = t[2];
	m[15] = 1;
}

/* Inverse of an affine 4x4 (general 3x3 part). */
static int m4_inverse_affine(const float m[16], float out[16]) {
	const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
	const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	const float det = a * A + b * B + c * C;
	if (fabsf(det) < 1e-12f) {
		m4_identity(out);
		return 0;
	}
	const float s = 1.0f / det;
	float r[16];
	r[0] = A * s;
	r[1] = B * s;
	r[2] = C * s;
	r[4] = -(b * i - c * h) * s;
	r[5] = (a * i - c * g) * s;
	r[6] = -(a * h - b * g) * s;
	r[8] = (b * f - c * e) * s;
	r[9] = -(a * f - c * d) * s;
	r[10] = (a * e - b * d) * s;
	r[3] = r[7] = r[11] = 0.0f;
	r[15] = 1.0f;
	const float t[3] = { m[12], m[13], m[14] };
	for (int k = 0; k < 3; ++k) {
		r[12 + k] = -(r[0 + k] * t[0] + r[4 + k] * t[1] + r[8 + k] * t[2]);
	}
	memcpy(out, r, sizeof r);
	return 1;
}

/* ---- Mesh building ---------------------------------------------------------- */

typedef struct CtBuild {
	AeronGltfVertex* v;
	uint32_t vn, vcap;
	uint16_t* idx;
	uint32_t in, icap;
	int failed;
} CtBuild;

static uint32_t ct_vertex(CtBuild* b, const float p[3], uint32_t material, float slot, float u, float v) {
	if (b->vn >= b->vcap) {
		const uint32_t cap = b->vcap ? b->vcap * 2 : 1024;
		AeronGltfVertex* nv = cap <= 65535u * 2u ? (AeronGltfVertex*)realloc(b->v, cap * sizeof *nv) : NULL;
		if (!nv) {
			b->failed = 1;
			return 0;
		}
		b->v = nv;
		b->vcap = cap;
	}
	AeronGltfVertex* out = &b->v[b->vn];
	memset(out, 0, sizeof *out);
	memcpy(out->pos, p, sizeof out->pos);
	out->tangent[0] = 1.0f;
	out->tangent[3] = 1.0f;
	out->uv[0] = u;
	out->uv[1] = v;
	out->mesh_index = slot;
	out->prim_id = material;
	return b->vn++;
}

static void ct_tri(CtBuild* b, uint32_t i0, uint32_t i1, uint32_t i2) {
	if (b->in + 3 > b->icap) {
		const uint32_t cap = b->icap ? b->icap * 2 : 4096;
		uint16_t* ni = (uint16_t*)realloc(b->idx, cap * sizeof *ni);
		if (!ni) {
			b->failed = 1;
			return;
		}
		b->idx = ni;
		b->icap = cap;
	}
	if (i0 > 65535u || i1 > 65535u || i2 > 65535u) {
		b->failed = 1;
		return;
	}
	b->idx[b->in++] = (uint16_t)i0;
	b->idx[b->in++] = (uint16_t)i1;
	b->idx[b->in++] = (uint16_t)i2;
}

/* Smooth normals of vertices [first, vn) from their triangles [ifirst, in). */
static void ct_normals(CtBuild* b, uint32_t first, uint32_t ifirst) {
	for (uint32_t i = first; i < b->vn; ++i) {
		memset(b->v[i].normal, 0, sizeof b->v[i].normal);
	}
	for (uint32_t t = ifirst; t + 2 < b->in; t += 3) {
		AeronGltfVertex* a = &b->v[b->idx[t]];
		AeronGltfVertex* c = &b->v[b->idx[t + 1]];
		AeronGltfVertex* d = &b->v[b->idx[t + 2]];
		const float e1[3] = { c->pos[0] - a->pos[0], c->pos[1] - a->pos[1], c->pos[2] - a->pos[2] };
		const float e2[3] = { d->pos[0] - a->pos[0], d->pos[1] - a->pos[1], d->pos[2] - a->pos[2] };
		float n[3];
		ct_cross(e1, e2, n);
		for (int k = 0; k < 3; ++k) {
			a->normal[k] += n[k];
			c->normal[k] += n[k];
			d->normal[k] += n[k];
		}
	}
	for (uint32_t i = first; i < b->vn; ++i) {
		ct_norm(b->v[i].normal);
	}
}

/* A grid of rows x cols (columns wrap around): vertices from `at(r, c)`. */
typedef void (*CtSurfaceFn)(const void* ctx, int r, int c, int rows, int cols, float out[3]);

static void ct_surface(CtBuild* b, CtSurfaceFn fn, const void* ctx, int rows, int cols, uint32_t material,
					   int flip) {
	const uint32_t first = b->vn, ifirst = b->in;
	for (int r = 0; r < rows; ++r) {
		for (int c = 0; c < cols; ++c) {
			float p[3];
			fn(ctx, r, c, rows, cols, p);
			ct_vertex(b, p, material, 0.0f, 0.0f, 0.0f);
		}
	}
	for (int r = 0; r + 1 < rows; ++r) {
		for (int c = 0; c < cols; ++c) {
			const uint32_t a = first + (uint32_t)(r * cols + c), bb = first + (uint32_t)(r * cols + (c + 1) % cols);
			const uint32_t d = first + (uint32_t)((r + 1) * cols + c),
						   e = first + (uint32_t)((r + 1) * cols + (c + 1) % cols);
			if (flip) {
				ct_tri(b, a, d, bb);
				ct_tri(b, bb, d, e);
			} else {
				ct_tri(b, a, bb, d);
				ct_tri(b, bb, e, d);
			}
		}
	}
	ct_normals(b, first, ifirst);
}

/* Superellipsoid: centre, radii, roundness (e1 vertical, e2 around), and
 * axes (columns: local x, y, z in model space). */
typedef struct CtSuper {
	float c[3], r[3], e1, e2;
	float ax[3], ay[3], az[3];
} CtSuper;

static float ct_spow(float v, float e) { return v < 0.0f ? -powf(-v, e) : powf(v, e); }

static void ct_super_at(const void* ctx, int row, int col, int rows, int cols, float out[3]) {
	const CtSuper* s = (const CtSuper*)ctx;
	const float eta = -0.5f * CT_PI + CT_PI * (float)row / (float)(rows - 1);
	const float om = -CT_PI + 2.0f * CT_PI * (float)col / (float)cols;
	const float ce = ct_spow(cosf(eta), s->e1);
	const float lx = s->r[0] * ce * ct_spow(cosf(om), s->e2);
	const float lz = s->r[2] * ce * ct_spow(sinf(om), s->e2);
	const float ly = s->r[1] * ct_spow(sinf(eta), s->e1);
	for (int k = 0; k < 3; ++k) {
		out[k] = s->c[k] + s->ax[k] * lx + s->ay[k] * ly + s->az[k] * lz;
	}
}

static void ct_super(CtBuild* b, const CtSuper* s, int rows, int cols, uint32_t material) {
	ct_surface(b, ct_super_at, s, rows, cols, material, 0);
}

/* Surface of revolution: profile points (radius, height) along `axis`
 * from `origin`; `sx`/`sz` squash the cross-section. */
typedef struct CtLathe {
	float origin[3], axis[3], u[3], w[3];
	float sx, sz;
	const float (*profile)[2];
	int n;
} CtLathe;

static void ct_lathe_at(const void* ctx, int row, int col, int rows, int cols, float out[3]) {
	(void)rows;
	const CtLathe* l = (const CtLathe*)ctx;
	const float t = 2.0f * CT_PI * (float)col / (float)cols;
	const float r = l->profile[row][0], h = l->profile[row][1];
	const float cu = cosf(t) * r * l->sx, sw = sinf(t) * r * l->sz;
	for (int k = 0; k < 3; ++k) {
		out[k] = l->origin[k] + l->axis[k] * h + l->u[k] * cu + l->w[k] * sw;
	}
}

static void ct_lathe(CtBuild* b, const float origin[3], const float axis_in[3], const float (*profile)[2], int n,
					 float sx, float sz, int cols, uint32_t material) {
	CtLathe l;
	memcpy(l.origin, origin, sizeof l.origin);
	memcpy(l.axis, axis_in, sizeof l.axis);
	ct_norm(l.axis);
	/* u: the model's +X projected off the axis (or +Z when along X). */
	float ref[3] = { 1.0f, 0.0f, 0.0f };
	if (fabsf(l.axis[0]) > 0.9f) {
		ref[0] = 0.0f;
		ref[2] = 1.0f;
	}
	ct_cross(l.axis, ref, l.w);
	ct_norm(l.w);
	ct_cross(l.w, l.axis, l.u);
	ct_norm(l.u);
	l.sx = sx;
	l.sz = sz;
	l.profile = profile;
	l.n = n;
	/* Winding: outward for profiles that go up the axis. */
	ct_surface(b, ct_lathe_at, &l, n, cols, material, 1);
}

/* ---- The generic controller ----------------------------------------------------- */

enum { CT_MAT_BODY, CT_MAT_BUTTON, CT_MAT_TRIGGER, CT_MAT_DARK, CT_MATS };

/* The head (a rounded puck) and where its top is, right hand, aim space. */
static const CtSuper k_head = { { 0.0f, -0.013f, 0.036f }, { 0.034f, 0.016f, 0.042f }, 0.45f, 0.55f,
								{ 1, 0, 0 },               { 0, 1, 0 },                 { 0, 0, 1 } };

static float ct_head_top(float x, float z) {
	const float dx = fabsf((x - k_head.c[0]) / k_head.r[0]), dz = fabsf((z - k_head.c[2]) / k_head.r[2]);
	const float h = powf(dx, 2.0f / k_head.e2) + powf(dz, 2.0f / k_head.e2);
	const float a = 1.0f - powf(h, k_head.e2 / k_head.e1);
	return k_head.c[1] + k_head.r[1] * (a > 0.0f ? powf(a, k_head.e1 / 2.0f) : 0.0f);
}

/* Handle: from under the head, down and back. */
static const float k_handle_origin[3] = { 0.0f, -0.022f, 0.052f };
static const float k_handle_axis[3] = { 0.0f, -0.829f, 0.559f };
static const float k_handle_len = 0.097f;

/* Face controls, right hand. */
static const float k_stick_xz[2] = { -0.013f, 0.032f };
static const float k_upper_xz[2] = { 0.013f, 0.020f };
static const float k_lower_xz[2] = { 0.021f, 0.039f };
static const float k_menu_xz[2] = { -0.012f, 0.058f };
static const float k_rest_xz[2] = { 0.016f, 0.058f };

typedef struct CtGeneric {
	CtBuild build;
	float anchor[XWA_VR_CTL_COUNT][3];
	float face_center[3], face_normal[3];
	float outline[8][3];
	int outline_count;
	XwaVrGloveCapsule capsules[2];
} CtGeneric;

static void ct_handle_point(float h, float out[3]) {
	for (int k = 0; k < 3; ++k) {
		out[k] = k_handle_origin[k] + k_handle_axis[k] * h;
	}
}

/* The grip button's direction off the handle axis: forward and inwards. */
static void ct_grip_dir(float out[3]) {
	float axis[3] = { k_handle_axis[0], k_handle_axis[1], k_handle_axis[2] };
	ct_norm(axis);
	const float x[3] = { 1.0f, 0.0f, 0.0f };
	float front[3];
	ct_cross(x, axis, front); /* (0, -az, ay): forward-down */
	ct_norm(front);
	out[0] = -0.6f;
	out[1] = 0.8f * front[1];
	out[2] = 0.8f * front[2];
	ct_norm(out);
}

static void ct_generic_build(CtGeneric* g) {
	CtBuild* b = &g->build;
	memset(g, 0, sizeof *g);
	/* Head. */
	ct_super(b, &k_head, 22, 44, CT_MAT_BODY);
	/* Handle, a little oval. */
	{
		static const float prof[][2] = {
			{ 0.0185f, -0.004f }, { 0.0200f, 0.000f }, { 0.0198f, 0.020f }, { 0.0190f, 0.045f },
			{ 0.0178f, 0.070f },  { 0.0165f, 0.080f }, { 0.0150f, 0.087f }, { 0.0120f, 0.092f },
			{ 0.0075f, 0.0955f }, { 0.0000f, 0.097f },
		};
		ct_lathe(b, k_handle_origin, k_handle_axis, prof, (int)(sizeof prof / sizeof prof[0]), 0.85f, 1.0f, 36,
				 CT_MAT_BODY);
	}
	/* Trigger: a paddle under the front, tilted forward. */
	{
		const float a = 25.0f * CT_PI / 180.0f;
		CtSuper t = { { 0.0f, -0.034f, 0.006f }, { 0.009f, 0.014f, 0.008f }, 0.5f, 0.6f,
					  { 1, 0, 0 },                { 0, cosf(a), -sinf(a) },     { 0, sinf(a), cosf(a) } };
		ct_super(b, &t, 12, 24, CT_MAT_TRIGGER);
	}
	/* Grip button on the handle. */
	float grip_dir[3];
	ct_grip_dir(grip_dir);
	float grip_c[3];
	{
		float p[3];
		ct_handle_point(0.035f, p);
		for (int k = 0; k < 3; ++k) {
			grip_c[k] = p[k] + grip_dir[k] * 0.0150f;
		}
		CtSuper s;
		memset(&s, 0, sizeof s);
		memcpy(s.c, grip_c, sizeof s.c);
		s.r[0] = 0.0055f; /* across: grip_dir */
		s.r[1] = 0.020f;  /* along the handle */
		s.r[2] = 0.011f;
		s.e1 = 0.5f;
		s.e2 = 0.6f;
		memcpy(s.ax, grip_dir, sizeof s.ax);
		memcpy(s.ay, k_handle_axis, sizeof s.ay);
		ct_norm(s.ay);
		ct_cross(s.ax, s.ay, s.az);
		ct_norm(s.az);
		ct_super(b, &s, 12, 24, CT_MAT_TRIGGER);
	}
	/* Thumbstick: a dark well, the shaft and the cap. */
	const float up[3] = { 0.0f, 1.0f, 0.0f };
	const float stick_y = ct_head_top(k_stick_xz[0], k_stick_xz[1]);
	{
		const float o[3] = { k_stick_xz[0], stick_y, k_stick_xz[1] };
		static const float well[][2] = { { 0.0f, 0.0010f }, { 0.0128f, 0.0010f }, { 0.0130f, -0.0030f } };
		static const float shaft[][2] = { { 0.0042f, 0.0005f }, { 0.0040f, 0.0065f } };
		static const float cap[][2] = { { 0.0080f, 0.0058f }, { 0.0105f, 0.0062f }, { 0.0106f, 0.0094f },
										{ 0.0096f, 0.0108f }, { 0.0050f, 0.0110f }, { 0.0000f, 0.0108f } };
		ct_lathe(b, o, up, well, 3, 1.0f, 1.0f, 32, CT_MAT_DARK);
		ct_lathe(b, o, up, shaft, 2, 1.0f, 1.0f, 16, CT_MAT_DARK);
		ct_lathe(b, o, up, cap, 6, 1.0f, 1.0f, 32, CT_MAT_DARK);
	}
	/* Face buttons and the menu button. */
	static const float button[][2] = { { 0.0066f, -0.0015f }, { 0.0066f, 0.0020f }, { 0.0057f, 0.0030f },
									   { 0.0000f, 0.0031f } };
	static const float small_button[][2] = { { 0.0042f, -0.0010f }, { 0.0042f, 0.0010f }, { 0.0034f, 0.0016f },
											 { 0.0000f, 0.0017f } };
	const float (*const xzs[3])[2] = { &k_upper_xz, &k_lower_xz, &k_menu_xz };
	for (int i = 0; i < 3; ++i) {
		const float* xz = *xzs[i];
		const float o[3] = { xz[0], ct_head_top(xz[0], xz[1]), xz[1] };
		if (i < 2) {
			ct_lathe(b, o, up, button, 4, 1.0f, 1.0f, 28, CT_MAT_BUTTON);
		} else {
			ct_lathe(b, o, up, small_button, 4, 1.0f, 1.0f, 20, CT_MAT_BUTTON);
		}
	}
	/* Thumb rest: a flat oval pad. */
	const float rest_y = ct_head_top(k_rest_xz[0], k_rest_xz[1]);
	{
		CtSuper s = { { k_rest_xz[0], rest_y + 0.0002f, k_rest_xz[1] }, { 0.0085f, 0.0007f, 0.0052f }, 0.3f, 0.8f,
					  { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
		ct_super(b, &s, 8, 28, CT_MAT_DARK);
	}

	/* Anchors (where the labels point). */
	{
		const float t[3] = { 0.0f, -0.040f, -0.004f };
		memcpy(g->anchor[XWA_VR_CTL_TRIGGER], t, sizeof t);
		for (int k = 0; k < 3; ++k) {
			g->anchor[XWA_VR_CTL_GRIP][k] = grip_c[k] + grip_dir[k] * 0.0055f;
		}
		const float (*const ctl_xz[5])[2] = { &k_upper_xz, &k_lower_xz, &k_stick_xz, &k_menu_xz, &k_rest_xz };
		static const int ctl_id[5] = { XWA_VR_CTL_UPPER, XWA_VR_CTL_LOWER, XWA_VR_CTL_STICK, XWA_VR_CTL_MENU,
									   XWA_VR_CTL_THUMBREST };
		static const float lift[5] = { 0.0031f, 0.0031f, 0.0110f, 0.0017f, 0.0009f };
		for (int i = 0; i < 5; ++i) {
			const float* xz = *ctl_xz[i];
			g->anchor[ctl_id[i]][0] = xz[0];
			g->anchor[ctl_id[i]][1] = ct_head_top(xz[0], xz[1]) + lift[i];
			g->anchor[ctl_id[i]][2] = xz[1];
		}
	}
	for (int k = 0; k < 3; ++k) {
		g->face_center[k] = (g->anchor[XWA_VR_CTL_UPPER][k] + g->anchor[XWA_VR_CTL_LOWER][k] +
							 g->anchor[XWA_VR_CTL_STICK][k]) /
							3.0f;
	}
	g->face_normal[0] = 0.0f;
	g->face_normal[1] = 1.0f;
	g->face_normal[2] = 0.0f;
	{
		float hend[3];
		ct_handle_point(k_handle_len, hend);
		const float pts[][3] = {
			{ 0.0f, -0.004f, -0.008f },  { 0.0f, 0.0f, 0.079f },    { -0.035f, -0.010f, 0.036f },
			{ 0.035f, -0.010f, 0.036f }, { hend[0], hend[1], hend[2] }, { 0.0f, -0.048f, 0.004f },
		};
		g->outline_count = (int)(sizeof pts / sizeof pts[0]);
		memcpy(g->outline, pts, sizeof pts);
	}
	/* Capsules: the head, then the handle (the palm is around it). */
	{
		XwaVrGloveCapsule* c = &g->capsules[0];
		const float a[3] = { 0.0f, -0.012f, 0.002f }, e[3] = { 0.0f, -0.012f, 0.070f };
		memcpy(c->a, a, sizeof a);
		memcpy(c->b, e, sizeof e);
		c->radius = 0.028f;
		c = &g->capsules[1];
		ct_handle_point(0.0f, c->a);
		ct_handle_point(0.085f, c->b);
		c->radius = 0.020f;
	}
}

/* The left hand: the mirror image (x negated, windings reversed). */
static void ct_generic_mirror(const CtGeneric* r, CtGeneric* l) {
	*l = *r;
	CtBuild* b = &l->build;
	b->v = (AeronGltfVertex*)malloc(r->build.vn * sizeof *b->v);
	b->idx = (uint16_t*)malloc(r->build.in * sizeof *b->idx);
	b->vcap = r->build.vn;
	b->icap = r->build.in;
	if (!b->v || !b->idx) {
		b->failed = 1;
		return;
	}
	memcpy(b->v, r->build.v, r->build.vn * sizeof *b->v);
	for (uint32_t i = 0; i < b->vn; ++i) {
		b->v[i].pos[0] = -b->v[i].pos[0];
		b->v[i].normal[0] = -b->v[i].normal[0];
	}
	for (uint32_t t = 0; t + 2 < r->build.in; t += 3) {
		b->idx[t] = r->build.idx[t];
		b->idx[t + 1] = r->build.idx[t + 2];
		b->idx[t + 2] = r->build.idx[t + 1];
	}
	for (int c = 0; c < XWA_VR_CTL_COUNT; ++c) {
		l->anchor[c][0] = -l->anchor[c][0];
	}
	l->face_center[0] = -l->face_center[0];
	l->face_normal[0] = -l->face_normal[0];
	for (int i = 0; i < l->outline_count; ++i) {
		l->outline[i][0] = -l->outline[i][0];
	}
	for (int i = 0; i < 2; ++i) {
		l->capsules[i].a[0] = -l->capsules[i].a[0];
		l->capsules[i].b[0] = -l->capsules[i].b[0];
	}
}

/* ---- Scene meshes ------------------------------------------------------------------ */

static void ct_material(AeronGltfMaterial* m, float r, float g, float b, float metallic, float roughness) {
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

typedef struct CtUpload {
	AeronGltfVertex* vertices;
	uint32_t vertex_count;
	uint16_t* indices;
	uint32_t index_count, opaque_count, mask_count; /* opaque, then mask, then blend */
	AeronGltfMaterial* materials;
	uint32_t material_count;
	uint32_t* prim_material; /* [prim_count] */
	uint32_t prim_count;
	AeronGltfChannelKtx2 channels[AERON_GLTF_CHANNEL_COUNT];
	float pad;
} CtUpload;

static AeronSceneMesh* ct_upload(AeronCommandBuffer* cmd, const CtUpload* u, const char* name) {
	/* Matte (roughness 0.9+): the lite mesh shader then skips the specular
	 * term and the face-normal derivatives for the controllers, which can
	 * fill much of the view when the player looks at them up close. */
	for (uint32_t m = 0; m < u->material_count; ++m) {
		u->materials[m].roughness_factor = fmaxf(u->materials[m].roughness_factor, 0.92f);
	}
	AeronFlightModel fm;
	memset(&fm, 0, sizeof fm);
	fm.render.vertices = u->vertices;
	fm.render.vertex_count = u->vertex_count;
	fm.render.indices = u->indices;
	fm.render.index_count = u->index_count;
	fm.render.opaque_index_count = u->opaque_count;
	fm.render.mask_index_offset = u->opaque_count;
	fm.render.mask_index_count = u->mask_count;
	fm.render.blend_index_offset = u->opaque_count + u->mask_count;
	fm.render.blend_index_count = u->index_count - u->opaque_count - u->mask_count;
	fm.render.material_count = u->material_count;
	fm.render.materials = u->materials;
	fm.render.variant_slots = 1;
	fm.render.total_prim_count = u->prim_count;
	fm.render.prim_variant_material = u->prim_material;
	memcpy(fm.render.channels, u->channels, sizeof fm.render.channels);
	float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
	for (uint32_t i = 0; i < u->vertex_count; ++i) {
		for (int k = 0; k < 3; ++k) {
			mn[k] = fminf(mn[k], u->vertices[i].pos[k] - u->pad);
			mx[k] = fmaxf(mx[k], u->vertices[i].pos[k] + u->pad);
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

static AeronSceneMesh* ct_upload_generic(AeronCommandBuffer* cmd, const CtGeneric* g, const char* name) {
	AeronGltfMaterial mats[CT_MATS];
	ct_material(&mats[CT_MAT_BODY], 0.028f, 0.029f, 0.032f, 0.0f, 0.55f);
	ct_material(&mats[CT_MAT_BUTTON], 0.10f, 0.10f, 0.11f, 0.0f, 0.35f);
	ct_material(&mats[CT_MAT_TRIGGER], 0.05f, 0.05f, 0.055f, 0.0f, 0.45f);
	ct_material(&mats[CT_MAT_DARK], 0.012f, 0.012f, 0.014f, 0.0f, 0.7f);
	uint32_t prim[CT_MATS] = { 0, 1, 2, 3 };
	CtUpload u;
	memset(&u, 0, sizeof u);
	u.vertices = g->build.v;
	u.vertex_count = g->build.vn;
	u.indices = g->build.idx;
	u.index_count = g->build.in;
	u.opaque_count = g->build.in;
	u.materials = mats;
	u.material_count = CT_MATS;
	u.prim_material = prim;
	u.prim_count = CT_MATS;
	u.pad = 0.01f;
	return ct_upload(cmd, &u, name);
}

/* ---- Runtime models (glTF) ------------------------------------------------------- */

/* A light copy of a model for the hands' masks (the overlay over the HUD
 * and panels): meshoptimizer's simplifier keeps a subset of the vertices
 * (it never moves them), part by part (mesh slot by slot, so moving parts
 * stay whole). Positions are stored in their slot's frame. */
typedef struct CtCut {
	float* pos;
	float* nrm;
	uint8_t* slot;
	uint16_t* idx;
	uint32_t vn, in;
} CtCut;

static void ct_cut_free(CtCut* c) {
	free(c->pos);
	free(c->nrm);
	free(c->slot);
	free(c->idx);
	memset(c, 0, sizeof *c);
}

/* `model_pos`: every vertex in one space (for the simplifier); `pos`, `nrm`,
 * `slot`: what is kept per vertex. About `ratio` of the triangles stay, fewer
 * when that keeps the shape within ~1 % of the model's size. */
static int ct_cut_build(CtCut* out, const float* model_pos, const float* pos, const float* nrm, const uint8_t* slot,
						uint32_t vn, const uint16_t* idx, uint32_t in, float ratio) {
	memset(out, 0, sizeof *out);
	if (vn == 0 || in < 3) {
		return 0;
	}
	unsigned int* src = (unsigned int*)malloc(in * sizeof *src);
	unsigned int* dst = (unsigned int*)malloc(in * sizeof *dst);
	unsigned int* kept = (unsigned int*)malloc(in * sizeof *kept);
	uint32_t* remap = (uint32_t*)malloc(vn * sizeof *remap);
	int ok = src && dst && kept && remap;
	size_t kept_n = 0;
	/* Part by part. */
	for (int s = 0; ok && s < 256; ++s) {
		size_t n = 0;
		for (uint32_t t = 0; t + 2 < in; t += 3) {
			if (idx[t] < vn && idx[t + 1] < vn && idx[t + 2] < vn && (slot ? slot[idx[t]] : 0) == s) {
				src[n++] = idx[t];
				src[n++] = idx[t + 1];
				src[n++] = idx[t + 2];
			}
		}
		if (n == 0) {
			if (!slot) {
				break;
			}
			continue;
		}
		size_t target = (size_t)((float)n * ratio) / 3 * 3;
		target = target < 3 ? 3 : target;
		float err = 0.0f;
		const size_t m = meshopt_simplify(dst, src, n, model_pos, vn, 3 * sizeof(float), target, 0.01f,
										  meshopt_SimplifyLockBorder, &err);
		memcpy(kept + kept_n, dst, m * sizeof *dst);
		kept_n += m;
		if (!slot) {
			break;
		}
	}
	if (ok) {
		for (uint32_t i = 0; i < vn; ++i) {
			remap[i] = UINT32_MAX;
		}
		uint32_t used = 0;
		for (size_t k = 0; k < kept_n; ++k) {
			if (remap[kept[k]] == UINT32_MAX) {
				remap[kept[k]] = used++;
			}
		}
		out->pos = (float*)malloc((used ? used : 1) * 3 * sizeof(float));
		out->nrm = (float*)malloc((used ? used : 1) * 3 * sizeof(float));
		out->slot = (uint8_t*)malloc(used ? used : 1);
		out->idx = (uint16_t*)malloc((kept_n ? kept_n : 1) * sizeof(uint16_t));
		ok = out->pos && out->nrm && out->slot && out->idx && used <= 65535u;
		for (uint32_t i = 0; ok && i < vn; ++i) {
			const uint32_t r = remap[i];
			if (r != UINT32_MAX) {
				memcpy(&out->pos[r * 3], &pos[i * 3], 3 * sizeof(float));
				memcpy(&out->nrm[r * 3], &nrm[i * 3], 3 * sizeof(float));
				out->slot[r] = slot ? slot[i] : 0;
			}
		}
		for (size_t k = 0; ok && k < kept_n; ++k) {
			out->idx[k] = (uint16_t)remap[kept[k]];
		}
		out->vn = used;
		out->in = (uint32_t)kept_n;
	}
	free(src);
	free(dst);
	free(kept);
	free(remap);
	if (!ok || out->in == 0) {
		ct_cut_free(out);
		return 0;
	}
	return 1;
}

typedef struct CtNode {
	int parent;
	float rest_local[16];
	float rest_world[16];
	float world[16]; /* this frame */
	int animatable;  /* index into the runtime's node list, -1 */
	int dynamic;     /* animatable or under one */
	int slot;        /* mesh slot of its vertices (0 = baked) */
	char name[64];
} CtNode;

typedef struct CtRuntime {
	uint32_t generation; /* loaded (or failed) */
	AeronXrRenderModelKind kind;
	AeronSceneMesh* mesh;
	int node_count;
	CtNode* nodes;
	int slot_node[AERON_MAX_MESH_SLOTS];
	int slot_count;
	AeronSceneMeshTable table;
	/* Labels: the node whose part a control is, and its centre there. */
	int ctl_node[XWA_VR_CTL_COUNT];
	float ctl_local[XWA_VR_CTL_COUNT][3];
	/* The light copy for the hands' masks. */
	CtCut cut;
	char name[64];
} CtRuntime;

static void ct_runtime_free(CtRuntime* rt) {
	if (rt->mesh) {
		AeronScene_MeshDestroy(rt->mesh);
	}
	free(rt->nodes);
	ct_cut_free(&rt->cut);
	const uint32_t generation = rt->generation;
	memset(rt, 0, sizeof *rt);
	rt->generation = generation;
}

/* Which control a node is, from its name ("trigger", "b_button", ...).
 * `weak` (second pass): the system / home button stands in for a menu
 * button the model does not have.
 * Steam Frame controllers (a d-pad and View on the left, A B X Y on the
 * right) are seen as Touch controllers by the game: Touch X / Y land on
 * the d-pad's down / up, Touch menu on View, Touch B on the right's top
 * three buttons (B among them). */
static int ct_match_control(const char* name, int hand, int weak) {
	char low[64];
	size_t n = 0;
	for (; name[n] && n + 1 < sizeof low; ++n) {
		low[n] = (char)tolower((unsigned char)name[n]);
	}
	low[n] = 0;
	/* Meta's models: "b_trigger_front" is the trigger, "b_trigger_grip" the
	 * grip button. */
	if (strstr(low, "squeeze") || strstr(low, "grip")) {
		return XWA_VR_CTL_GRIP;
	}
	if (strstr(low, "trigger")) {
		return XWA_VR_CTL_TRIGGER;
	}
	if (strstr(low, "thumbrest") || strstr(low, "thumb_rest")) {
		return XWA_VR_CTL_THUMBREST;
	}
	if (strstr(low, "thumbstick") || strstr(low, "joystick")) {
		return XWA_VR_CTL_STICK;
	}
	if (strstr(low, "menu") || strstr(low, "view")) {
		return XWA_VR_CTL_MENU;
	}
	if (strstr(low, "system") || strstr(low, "home") || strstr(low, "oculus")) {
		return weak ? XWA_VR_CTL_MENU : -1;
	}
	if (strstr(low, "dpad") || strstr(low, "d_pad")) {
		if (hand == 0 && strstr(low, "down")) {
			return XWA_VR_CTL_LOWER; /* X */
		}
		if (hand == 0 && strstr(low, "up")) {
			return XWA_VR_CTL_UPPER; /* Y */
		}
		return -1;
	}
	/* Face buttons: the letter as a word of its own, or glued to
	 * "button" ("a_button", "buttonA", "button_a"). Of single letters the
	 * last one counts: Meta's joints are "b_button_a", "b_" marking a bone
	 * (taken first, the right controller's A was its B). */
	const char upper = hand ? 'b' : 'y', lower = hand ? 'a' : 'x';
	char tok[64];
	size_t t = 0;
	int letter = -1;
	for (size_t i = 0; i <= n; ++i) {
		const char c = low[i];
		if (isalnum((unsigned char)c)) {
			if (t + 1 < sizeof tok) {
				tok[t++] = c;
			}
			continue;
		}
		tok[t] = 0;
		if (t == 1 && (tok[0] == upper || tok[0] == lower)) {
			letter = tok[0] == upper ? XWA_VR_CTL_UPPER : XWA_VR_CTL_LOWER;
		}
		if (t == 7 && strncmp(tok, "button", 6) == 0 && (tok[6] == upper || tok[6] == lower)) {
			return tok[6] == upper ? XWA_VR_CTL_UPPER : XWA_VR_CTL_LOWER;
		}
		if (t == 7 && strncmp(tok + 1, "button", 6) == 0 && (tok[0] == upper || tok[0] == lower)) {
			return tok[0] == upper ? XWA_VR_CTL_UPPER : XWA_VR_CTL_LOWER;
		}
		t = 0;
	}
	return letter;
}

/* Bilinear resample of an RGBA8 image into a cell of the atlas. */
static void ct_blit(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dx, int dy, int cw, int ch) {
	for (int y = 0; y < ch; ++y) {
		const float fy = ((float)y + 0.5f) * (float)sh / (float)ch - 0.5f;
		const int y0 = fy < 0.0f ? 0 : (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
		const float ty = fy < 0.0f ? 0.0f : fy - (float)y0;
		for (int x = 0; x < cw; ++x) {
			const float fx = ((float)x + 0.5f) * (float)sw / (float)cw - 0.5f;
			const int x0 = fx < 0.0f ? 0 : (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
			const float tx = fx < 0.0f ? 0.0f : fx - (float)x0;
			uint8_t* o = &dst[((size_t)(dy + y) * (size_t)dw + (size_t)(dx + x)) * 4];
			for (int k = 0; k < 4; ++k) {
				const float a = src[((size_t)y0 * sw + x0) * 4 + k], b = src[((size_t)y0 * sw + x1) * 4 + k];
				const float c = src[((size_t)y1 * sw + x0) * 4 + k], d = src[((size_t)y1 * sw + x1) * 4 + k];
				const float v = (a + (b - a) * tx) * (1.0f - ty) + (c + (d - c) * tx) * ty;
				o[k] = (uint8_t)(v + 0.5f);
			}
		}
	}
}

static int ct_tiny_ktx2(uint8_t r, uint8_t g, uint8_t b, uint8_t a, int srgb, AeronGltfChannelKtx2* out) {
	uint8_t px[4 * 4 * 4];
	for (int i = 0; i < 16; ++i) {
		px[i * 4 + 0] = r;
		px[i * 4 + 1] = g;
		px[i * 4 + 2] = b;
		px[i * 4 + 3] = a;
	}
	return write_ktx2_rgba_with_generated_mips_to_buffer_limited_alpha(
		4, 4, px, srgb ? KTX2_TF_SRGB : KTX2_TF_LINEAR, false, 1, KTX2_ALPHA_STRAIGHT, &out->data, &out->size);
}

/* Reads accessor element i as floats (0 when missing). */
static void ct_read(const cgltf_accessor* a, cgltf_size i, float* out, int n) {
	memset(out, 0, (size_t)n * sizeof *out);
	if (a) {
		cgltf_accessor_read_float(a, i, out, (cgltf_size)n);
	}
}

/* Loads `glb` as the runtime model of `hand`. `names`: the runtime's
 * animatable node names (SPACE models). */
/* A texture's image: its own, or its KHR_texture_basisu one (KTX2). */
static const cgltf_image* ct_texture_image(const cgltf_texture* tex) {
	if (!tex) {
		return NULL;
	}
	const cgltf_image* img = tex->image ? tex->image : tex->has_basisu ? tex->basisu_image : NULL;
	return img && img->buffer_view ? img : NULL;
}

/* A copy of the runtime's model in the app's files folder
 * (controller_left.glb / controller_right.glb, next to openxwa.log), so it
 * can be looked at off the headset. Written when the model loads. */
static void ct_save_glb(int hand, const AeronXrRenderModel* rm) {
#if defined(SDL_PLATFORM_ANDROID)
	const char* dir = SDL_GetAndroidExternalStoragePath();
	if (!dir || !dir[0] || !rm->glb || !rm->glb_size) {
		return;
	}
	char path[1024];
	snprintf(path, sizeof path, "%s/controller_%s.glb", dir, hand ? "right" : "left");
	FILE* f = fopen(path, "wb");
	if (f) {
		const int ok = fwrite(rm->glb, 1, rm->glb_size, f) == rm->glb_size;
		if (fclose(f) == 0 && ok) {
			Aeron_LogInfo("xwa.remaster", "controller model saved: %s", path);
		}
	}
#else
	(void)hand;
	(void)rm;
#endif
}

static int ct_runtime_load(CtRuntime* rt, AeronCommandBuffer* cmd, int hand, const AeronXrRenderModel* rm) {
	ct_save_glb(hand, rm);
	cgltf_options options;
	memset(&options, 0, sizeof options);
	cgltf_data* data = NULL;
	if (cgltf_parse(&options, rm->glb, rm->glb_size, &data) != cgltf_result_success) {
		Aeron_LogWarn("xwa.remaster", "controller model: not a glTF the parser takes");
		return 0;
	}
	int ok = 0;
	uint8_t* atlas = NULL;
	CtBuild b;
	memset(&b, 0, sizeof b);
	uint16_t* lists[3] = { NULL, NULL, NULL };
	uint32_t list_n[3] = { 0, 0, 0 }, list_cap[3] = { 0, 0, 0 };
	AeronGltfMaterial* mats = NULL;
	uint32_t* prim_material = NULL;
	float* skin_m = NULL; /* a skinned mesh's joint matrices (joint * inverse bind), model space */
	float bb_min[3] = { 1e9f, 1e9f, 1e9f }, bb_max[3] = { -1e9f, -1e9f, -1e9f }; /* at rest, model space */
	CtUpload u;
	memset(&u, 0, sizeof u);
	if (cgltf_load_buffers(&options, data, NULL) != cgltf_result_success) {
		Aeron_LogWarn("xwa.remaster", "controller model: buffers not loaded");
		goto done;
	}
	/* Nodes, their rest transforms and which ones move. */
	rt->node_count = (int)data->nodes_count;
	rt->nodes = (CtNode*)calloc(data->nodes_count ? data->nodes_count : 1, sizeof *rt->nodes);
	if (!rt->nodes) {
		goto done;
	}
	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		const cgltf_node* n = &data->nodes[i];
		CtNode* cn = &rt->nodes[i];
		cn->parent = n->parent ? (int)(n->parent - data->nodes) : -1;
		cgltf_node_transform_local(n, cn->rest_local);
		cgltf_node_transform_world(n, cn->rest_world);
		cn->animatable = -1;
		snprintf(cn->name, sizeof cn->name, "%s", n->name ? n->name : "");
		for (uint32_t k = 0; k < rm->node_count && n->name; ++k) {
			if (strcmp(n->name, rm->node_names[k]) == 0) {
				cn->animatable = (int)k;
			}
		}
	}
	for (int i = 0; i < rt->node_count; ++i) {
		for (int p = i; p >= 0; p = rt->nodes[p].parent) {
			if (rt->nodes[p].animatable >= 0) {
				rt->nodes[i].dynamic = 1;
				break;
			}
		}
	}
	/* Base colour images -> one atlas (up to four, 2x2). PNG / JPEG, or
	 * KTX2 (Basis Universal: KHR_texture_basisu, Meta's models; their
	 * texture carries the button letters). */
	const cgltf_image* images[4] = { NULL };
	int image_count = 0;
	for (cgltf_size m = 0; m < data->materials_count; ++m) {
		const cgltf_texture* tex = data->materials[m].pbr_metallic_roughness.base_color_texture.texture;
		const cgltf_image* img = ct_texture_image(tex);
		if (!img || !img->buffer_view) {
			continue;
		}
		int known = 0;
		for (int k = 0; k < image_count; ++k) {
			known |= images[k] == img;
		}
		if (!known && image_count < 4) {
			images[image_count++] = img;
		}
	}
	int cell = 0, grid_w = 1, grid_h = 1;
	int image_ok[4] = { 0, 0, 0, 0 };
	if (image_count > 0) {
		uint8_t* decoded[4] = { NULL };
		uint8_t* decoded_ktx2[4] = { NULL }; /* the ones from XwaKtx2_DecodeRgba (free()) */
		int dw[4] = { 0 }, dh[4] = { 0 };
		int biggest = 0;
		for (int k = 0; k < image_count; ++k) {
			const cgltf_buffer_view* bv = images[k]->buffer_view;
			const uint8_t* bytes = (const uint8_t*)bv->buffer->data + bv->offset;
			int ch = 0;
			if (bv->size >= 12 && memcmp(bytes, "\xABKTX 20\xBB\r\n\x1A\n", 12) == 0) {
				decoded_ktx2[k] = XwaKtx2_DecodeRgba(bytes, bv->size, &dw[k], &dh[k]);
				decoded[k] = decoded_ktx2[k];
			} else {
				decoded[k] = stbi_load_from_memory(bytes, (int)bv->size, &dw[k], &dh[k], &ch, 4);
			}
			if (decoded[k]) {
				biggest = dw[k] > biggest ? dw[k] : biggest;
				biggest = dh[k] > biggest ? dh[k] : biggest;
			} else {
				Aeron_LogInfo("xwa.remaster", "controller model: image %d (%s) not decoded", k,
							  images[k]->mime_type ? images[k]->mime_type : "?");
			}
		}
		cell = 64;
		while (cell < biggest && cell < 1024) {
			cell *= 2;
		}
		grid_w = image_count > 1 ? 2 : 1;
		grid_h = image_count > 2 ? 2 : 1;
		atlas = (uint8_t*)calloc((size_t)cell * grid_w * cell * grid_h, 4);
		if (atlas) {
			for (int k = 0; k < image_count; ++k) {
				if (decoded[k]) {
					ct_blit(decoded[k], dw[k], dh[k], atlas, cell * grid_w, (k % 2) * cell, (k / 2) * cell, cell, cell);
					image_ok[k] = 1;
				}
			}
		}
		for (int k = 0; k < image_count; ++k) {
			if (decoded_ktx2[k]) {
				free(decoded_ktx2[k]);
			} else {
				stbi_image_free(decoded[k]);
			}
		}
	}
	int any_image = 0;
	for (int k = 0; k < image_count; ++k) {
		any_image |= image_ok[k];
	}
	/* Materials (one extra default at the end). */
	const uint32_t mat_count = (uint32_t)data->materials_count + 1u;
	if (mat_count > AERON_GLTF_MAX_MATERIALS) {
		Aeron_LogWarn("xwa.remaster", "controller model: too many materials");
		goto done;
	}
	mats = (AeronGltfMaterial*)calloc(mat_count, sizeof *mats);
	if (!mats) {
		goto done;
	}
	for (uint32_t m = 0; m < mat_count; ++m) {
		ct_material(&mats[m], 0.05f, 0.05f, 0.055f, 0.0f, 0.5f);
		if (m + 1 == mat_count) {
			continue;
		}
		const cgltf_material* gm = &data->materials[m];
		if (gm->has_pbr_metallic_roughness) {
			memcpy(mats[m].base_color_factor, gm->pbr_metallic_roughness.base_color_factor, 4 * sizeof(float));
			mats[m].metallic_factor = gm->pbr_metallic_roughness.metallic_factor;
			mats[m].roughness_factor = gm->pbr_metallic_roughness.roughness_factor;
		}
		if (!gm->emissive_texture.texture) {
			memcpy(mats[m].emissive_factor, gm->emissive_factor, 3 * sizeof(float));
		}
		mats[m].double_sided = gm->double_sided ? 1u : 0u;
		mats[m].alpha_mode = gm->alpha_mode == cgltf_alpha_mode_mask    ? AERON_GLTF_ALPHA_MASK
							 : gm->alpha_mode == cgltf_alpha_mode_blend ? AERON_GLTF_ALPHA_BLEND
																		: AERON_GLTF_ALPHA_OPAQUE;
		mats[m].alpha_cutoff = gm->alpha_cutoff;
		const cgltf_image* tex_image = ct_texture_image(gm->pbr_metallic_roughness.base_color_texture.texture);
		for (int k = 0; k < image_count; ++k) {
			if (tex_image && tex_image == images[k] && image_ok[k]) {
				const float aw = (float)(cell * grid_w), ah = (float)(cell * grid_h);
				mats[m].uv_xform[AERON_GLTF_CHANNEL_BASE_COLOR][0] = (float)((k % 2) * cell) / aw;
				mats[m].uv_xform[AERON_GLTF_CHANNEL_BASE_COLOR][1] = (float)((k / 2) * cell) / ah;
				mats[m].uv_xform[AERON_GLTF_CHANNEL_BASE_COLOR][2] = (float)cell / aw;
				mats[m].uv_xform[AERON_GLTF_CHANNEL_BASE_COLOR][3] = (float)cell / ah;
			}
		}
	}
	/* Mesh slots: a moving node with geometry under it gets one. */
	rt->slot_count = 1;
	rt->slot_node[0] = -1;
	for (int i = 0; i < rt->node_count; ++i) {
		rt->nodes[i].slot = 0;
		if (rt->nodes[i].dynamic && data->nodes[i].mesh && rt->slot_count < AERON_MAX_MESH_SLOTS) {
			rt->nodes[i].slot = rt->slot_count;
			rt->slot_node[rt->slot_count++] = i;
		}
	}
	/* Geometry: one primitive id per glTF primitive. */
	uint32_t prim_total = 0;
	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		if (data->nodes[i].mesh) {
			prim_total += (uint32_t)data->nodes[i].mesh->primitives_count;
		}
	}
	prim_material = (uint32_t*)calloc(prim_total ? prim_total : 1, sizeof *prim_material);
	if (!prim_material) {
		goto done;
	}
	float ctl_sum[XWA_VR_CTL_COUNT][3];
	int ctl_cnt[XWA_VR_CTL_COUNT];
	memset(ctl_sum, 0, sizeof ctl_sum);
	memset(ctl_cnt, 0, sizeof ctl_cnt);
	for (int c = 0; c < XWA_VR_CTL_COUNT; ++c) {
		rt->ctl_node[c] = -1;
	}
	/* The controls' nodes: the first match in the tree (then the system
	 * button for a missing menu button). */
	for (int weak = 0; weak < 2; ++weak) {
		for (int i = 0; i < rt->node_count; ++i) {
			const int c = ct_match_control(rt->nodes[i].name, hand, weak);
			if (c >= 0 && rt->ctl_node[c] < 0) {
				rt->ctl_node[c] = i;
			}
		}
	}
	uint32_t prim_id = 0;
	for (cgltf_size i = 0; i < data->nodes_count; ++i) {
		const cgltf_mesh* mesh = data->nodes[i].mesh;
		if (!mesh) {
			continue;
		}
		const CtNode* cn = &rt->nodes[i];
		/* Baked parts: in model space; moving ones: in their node's frame. */
		float to_model[16];
		if (cn->slot == 0) {
			memcpy(to_model, cn->rest_world, sizeof to_model);
		} else {
			m4_identity(to_model);
		}
		/* A skinned mesh (Meta's) is placed by its joints, joint * inverse
		 * bind matrix, as glTF says: its own node's transform is ignored.
		 * Where the two disagree, the node's transform draws it turned. */
		const cgltf_skin* node_skin = data->nodes[i].skin;
		free(skin_m);
		skin_m = NULL;
		if (node_skin && node_skin->joints_count && cn->slot == 0) {
			skin_m = (float*)malloc(node_skin->joints_count * 16 * sizeof(float));
			if (!skin_m) {
				goto done;
			}
			for (cgltf_size j = 0; j < node_skin->joints_count; ++j) {
				float ibm[16];
				m4_identity(ibm);
				if (node_skin->inverse_bind_matrices && j < node_skin->inverse_bind_matrices->count) {
					cgltf_accessor_read_float(node_skin->inverse_bind_matrices, j, ibm, 16);
				}
				if (node_skin->joints[j]) {
					m4_mul(rt->nodes[node_skin->joints[j] - data->nodes].rest_world, ibm, &skin_m[j * 16]);
				} else {
					memcpy(&skin_m[j * 16], cn->rest_world, 16 * sizeof(float));
				}
			}
		}
		float skin_off_max = 0.0f; /* how far the skin moves a vertex from where its node puts it */
		int skin_used = 0;
		for (cgltf_size p = 0; p < mesh->primitives_count; ++p, ++prim_id) {
			const cgltf_primitive* prim = &mesh->primitives[p];
			prim_material[prim_id] = prim->material ? (uint32_t)(prim->material - data->materials) : mat_count - 1;
			if (prim->type != cgltf_primitive_type_triangles) {
				continue;
			}
			const cgltf_accessor *pos = NULL, *nrm = NULL, *uv = NULL, *joints = NULL, *weights = NULL;
			for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
				const cgltf_attribute* at = &prim->attributes[a];
				if (at->type == cgltf_attribute_type_position) {
					pos = at->data;
				} else if (at->type == cgltf_attribute_type_normal) {
					nrm = at->data;
				} else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) {
					uv = at->data;
				} else if (at->type == cgltf_attribute_type_joints && at->index == 0) {
					joints = at->data;
				} else if (at->type == cgltf_attribute_type_weights && at->index == 0) {
					weights = at->data;
				}
			}
			/* A skinned mesh (Meta's): its buttons are joints, not meshes of
			 * their own; a vertex belongs to the joint that weighs most. */
			const cgltf_skin* skin = data->nodes[i].skin;
			if (!skin || !joints || !weights) {
				joints = weights = NULL;
			}
			if (!pos) {
				continue;
			}
			const uint32_t base = b.vn;
			for (cgltf_size v = 0; v < pos->count; ++v) {
				float pp[3], nn[3], tt[2];
				ct_read(pos, v, pp, 3);
				ct_read(nrm, v, nn, 3);
				ct_read(uv, v, tt, 2);
				/* Where the vertex is at rest, in model space. */
				float vert_m[16];
				memcpy(vert_m, cn->rest_world, sizeof vert_m);
				int owner = (int)i; /* the node whose part the vertex is */
				if (joints) {
					float jw[4] = { 0, 0, 0, 0 }, jj[4] = { 0, 0, 0, 0 };
					cgltf_accessor_read_float(weights, v, jw, 4);
					cgltf_accessor_read_float(joints, v, jj, 4);
					int best = 0;
					for (int k = 1; k < 4; ++k) {
						best = jw[k] > jw[best] ? k : best;
					}
					const cgltf_size j = (cgltf_size)(jj[best] + 0.5f);
					if (jw[best] > 0.0f && j < skin->joints_count && skin->joints[j]) {
						owner = (int)(skin->joints[j] - data->nodes);
					}
					if (skin_m) {
						float sm[16], wsum = 0.0f;
						memset(sm, 0, sizeof sm);
						for (int k = 0; k < 4; ++k) {
							const cgltf_size jk = (cgltf_size)(jj[k] + 0.5f);
							if (jw[k] > 0.0f && jk < skin->joints_count) {
								for (int e = 0; e < 16; ++e) {
									sm[e] += jw[k] * skin_m[jk * 16 + e];
								}
								wsum += jw[k];
							}
						}
						if (wsum > 1e-6f) {
							for (int e = 0; e < 16; ++e) {
								vert_m[e] = sm[e] / wsum;
							}
							float by_node[3], by_skin[3];
							m4_point(cn->rest_world, pp, by_node);
							m4_point(vert_m, pp, by_skin);
							const float dx = by_skin[0] - by_node[0], dy = by_skin[1] - by_node[1],
										dz = by_skin[2] - by_node[2];
							const float d = sqrtf(dx * dx + dy * dy + dz * dz);
							skin_off_max = d > skin_off_max ? d : skin_off_max;
							skin_used = 1;
						}
					}
				}
				/* The controls' centres, in model space at rest. */
				float rest[3];
				m4_point(vert_m, pp, rest);
				for (int k = 0; k < 3; ++k) {
					bb_min[k] = rest[k] < bb_min[k] ? rest[k] : bb_min[k];
					bb_max[k] = rest[k] > bb_max[k] ? rest[k] : bb_max[k];
				}
				for (int c = 0; c < XWA_VR_CTL_COUNT; ++c) {
					for (int q = owner; q >= 0 && rt->ctl_node[c] >= 0; q = rt->nodes[q].parent) {
						if (q == rt->ctl_node[c]) {
							for (int k = 0; k < 3; ++k) {
								ctl_sum[c][k] += rest[k];
							}
							ctl_cnt[c]++;
							break;
						}
					}
				}
				float mp[3], mn[3];
				const float* place = cn->slot == 0 ? vert_m : to_model;
				m4_point(place, pp, mp);
				m4_vector(place, nn, mn);
				ct_norm(mn);
				const uint32_t vi = ct_vertex(&b, mp, prim_id, (float)cn->slot, tt[0], tt[1]);
				memcpy(b.v[vi].normal, mn, sizeof mn);
			}
			if (b.failed) {
				Aeron_LogWarn("xwa.remaster", "controller model: too many vertices");
				goto done;
			}
			const AeronGltfAlphaMode mode = mats[prim_material[prim_id]].alpha_mode;
			const int list = mode == AERON_GLTF_ALPHA_MASK ? 1 : mode == AERON_GLTF_ALPHA_BLEND ? 2 : 0;
			const cgltf_size count = prim->indices ? prim->indices->count : pos->count;
			for (cgltf_size k = 0; k + 2 < count + 0 && k < count; k += 3) {
				uint32_t t3[3];
				for (int e = 0; e < 3; ++e) {
					const cgltf_size ii = prim->indices ? cgltf_accessor_read_index(prim->indices, k + (cgltf_size)e)
														: k + (cgltf_size)e;
					t3[e] = base + (uint32_t)ii;
				}
				if (t3[0] > 65535u || t3[1] > 65535u || t3[2] > 65535u) {
					Aeron_LogWarn("xwa.remaster", "controller model: too many vertices");
					goto done;
				}
				if (list_n[list] + 3 > list_cap[list]) {
					list_cap[list] = list_cap[list] ? list_cap[list] * 2 : 4096;
					uint16_t* nl = (uint16_t*)realloc(lists[list], list_cap[list] * sizeof *nl);
					if (!nl) {
						goto done;
					}
					lists[list] = nl;
				}
				for (int e = 0; e < 3; ++e) {
					lists[list][list_n[list]++] = (uint16_t)t3[e];
				}
			}
		}
		if (node_skin) {
			Aeron_LogInfo("xwa.remaster",
						  "  %s mesh \"%s\": skin of %u joints (%s inverse binds)%s, %.1f mm at most from its node's "
						  "transform",
						  hand ? "right" : "left", cn->name, (unsigned)node_skin->joints_count,
						  node_skin->inverse_bind_matrices ? "with" : "no", skin_used ? ", placed by the skin" : "",
						  (double)(skin_off_max * 1000.0f));
		}
	}
	free(skin_m);
	skin_m = NULL;
	if (b.vn == 0) {
		Aeron_LogWarn("xwa.remaster", "controller model: no triangles");
		goto done;
	}
	/* Anchors: each control's centre in its node's frame. */
	for (int c = 0; c < XWA_VR_CTL_COUNT; ++c) {
		if (rt->ctl_node[c] < 0 || ctl_cnt[c] == 0) {
			rt->ctl_node[c] = -1;
			continue;
		}
		float centre[3], inv[16];
		for (int k = 0; k < 3; ++k) {
			centre[k] = ctl_sum[c][k] / (float)ctl_cnt[c];
		}
		m4_inverse_affine(rt->nodes[rt->ctl_node[c]].rest_world, inv);
		m4_point(inv, centre, rt->ctl_local[c]);
		static const char* const k_ctl_names[XWA_VR_CTL_COUNT] = { "trigger", "grip",  "upper (B/Y)", "lower (A/X)",
																	"stick",   "menu", "thumb rest" };
		Aeron_LogInfo("xwa.remaster", "  %s control %s: node \"%s\", %d vertices, centre %.1f %.1f %.1f mm",
					  hand ? "right" : "left", k_ctl_names[c], rt->nodes[rt->ctl_node[c]].name, ctl_cnt[c],
					  (double)(centre[0] * 1000.0f), (double)(centre[1] * 1000.0f), (double)(centre[2] * 1000.0f));
	}
	/* The thumb rest has no part of its own in Meta's models (a touch patch
	 * of the shell): beside A / X, level with the gap below B / Y, i.e.
	 * A + 0.767 (A - stick) + 1.087 (B - A), measured on the Quest 3 Touch
	 * Plus (both hands, mirrored), lifted 1.5 mm onto the shell (away from
	 * the trigger, which is under the face). On the controller's body. */
	if (rt->ctl_node[XWA_VR_CTL_THUMBREST] < 0 && rt->ctl_node[XWA_VR_CTL_UPPER] >= 0 &&
		rt->ctl_node[XWA_VR_CTL_LOWER] >= 0 && rt->ctl_node[XWA_VR_CTL_STICK] >= 0 &&
		rt->ctl_node[XWA_VR_CTL_TRIGGER] >= 0) {
		float up_c[3], lo_c[3], st_c[3], tr_c[3], rest[3], lift[3];
		for (int k = 0; k < 3; ++k) {
			up_c[k] = ctl_sum[XWA_VR_CTL_UPPER][k] / (float)ctl_cnt[XWA_VR_CTL_UPPER];
			lo_c[k] = ctl_sum[XWA_VR_CTL_LOWER][k] / (float)ctl_cnt[XWA_VR_CTL_LOWER];
			st_c[k] = ctl_sum[XWA_VR_CTL_STICK][k] / (float)ctl_cnt[XWA_VR_CTL_STICK];
			tr_c[k] = ctl_sum[XWA_VR_CTL_TRIGGER][k] / (float)ctl_cnt[XWA_VR_CTL_TRIGGER];
			rest[k] = lo_c[k] + 0.767f * (lo_c[k] - st_c[k]) + 1.087f * (up_c[k] - lo_c[k]);
			lift[k] = lo_c[k] - tr_c[k];
		}
		ct_norm(lift);
		for (int k = 0; k < 3; ++k) {
			rest[k] += lift[k] * 0.0015f;
		}
		const int lower = rt->ctl_node[XWA_VR_CTL_LOWER];
		const int body = rt->nodes[lower].parent >= 0 ? rt->nodes[lower].parent : lower;
		float inv[16];
		rt->ctl_node[XWA_VR_CTL_THUMBREST] = body;
		m4_inverse_affine(rt->nodes[body].rest_world, inv);
		m4_point(inv, rest, rt->ctl_local[XWA_VR_CTL_THUMBREST]);
		Aeron_LogInfo("xwa.remaster", "  %s control thumb rest: beside %s on \"%s\", centre %.1f %.1f %.1f mm",
					  hand ? "right" : "left", hand ? "A" : "X", rt->nodes[body].name, (double)(rest[0] * 1000.0f),
					  (double)(rest[1] * 1000.0f), (double)(rest[2] * 1000.0f));
	}
	/* Index lists: opaque, mask, blend. */
	const uint32_t total = list_n[0] + list_n[1] + list_n[2];
	uint16_t* idx = (uint16_t*)malloc(total * sizeof *idx);
	if (!idx) {
		goto done;
	}
	memcpy(idx, lists[0], list_n[0] * sizeof *idx);
	if (list_n[1]) {
		memcpy(idx + list_n[0], lists[1], list_n[1] * sizeof *idx);
	}
	if (list_n[2]) {
		memcpy(idx + list_n[0] + list_n[1], lists[2], list_n[2] * sizeof *idx);
	}
	free(b.idx);
	b.idx = idx;
	b.in = total;
	/* Textures: the atlas, and plain stand-ins for the other channels. */
	u.vertices = b.v;
	u.vertex_count = b.vn;
	u.indices = b.idx;
	u.index_count = b.in;
	u.opaque_count = list_n[0];
	u.mask_count = list_n[1];
	u.materials = mats;
	u.material_count = mat_count;
	u.prim_material = prim_material;
	u.prim_count = prim_total;
	u.pad = 0.02f;
	if (any_image && atlas) {
		if (!write_ktx2_rgba_with_generated_mips_to_buffer_limited_alpha(
				cell * grid_w, cell * grid_h, atlas, KTX2_TF_SRGB, false, 0, KTX2_ALPHA_STRAIGHT,
				&u.channels[AERON_GLTF_CHANNEL_BASE_COLOR].data, &u.channels[AERON_GLTF_CHANNEL_BASE_COLOR].size) ||
			!ct_tiny_ktx2(128, 128, 255, 255, 0, &u.channels[AERON_GLTF_CHANNEL_NORMAL]) ||
			!ct_tiny_ktx2(255, 255, 255, 255, 0, &u.channels[AERON_GLTF_CHANNEL_METALLIC_ROUGHNESS]) ||
			!ct_tiny_ktx2(0, 0, 0, 255, 1, &u.channels[AERON_GLTF_CHANNEL_EMISSIVE])) {
			Aeron_LogWarn("xwa.remaster", "controller model: texture encoding failed, untextured");
			for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; ++c) {
				free(u.channels[c].data);
				u.channels[c].data = NULL;
				u.channels[c].size = 0;
			}
			for (uint32_t m = 0; m < mat_count; ++m) {
				memset(mats[m].uv_xform, 0, sizeof mats[m].uv_xform);
			}
		}
	}
	{
		char label[96];
		snprintf(label, sizeof label, "xwa.vr_controller.%s", hand ? "right" : "left");
		rt->mesh = ct_upload(cmd, &u, label);
	}
	if (!rt->mesh) {
		Aeron_LogWarn("xwa.remaster", "controller model: scene mesh not created");
		goto done;
	}
	/* The light copy for the hands' masks (opaque parts). */
	{
		float* model_pos = (float*)malloc(b.vn * 3 * sizeof(float));
		float* pos = (float*)malloc(b.vn * 3 * sizeof(float));
		float* nrm = (float*)malloc(b.vn * 3 * sizeof(float));
		uint8_t* slot = (uint8_t*)malloc(b.vn);
		if (model_pos && pos && nrm && slot) {
			for (uint32_t i = 0; i < b.vn; ++i) {
				const int sl = (int)b.v[i].mesh_index;
				memcpy(&pos[i * 3], b.v[i].pos, 3 * sizeof(float));
				memcpy(&nrm[i * 3], b.v[i].normal, 3 * sizeof(float));
				slot[i] = (uint8_t)sl;
				if (sl > 0 && sl < rt->slot_count) {
					m4_point(rt->nodes[rt->slot_node[sl]].rest_world, b.v[i].pos, &model_pos[i * 3]);
				} else {
					memcpy(&model_pos[i * 3], b.v[i].pos, 3 * sizeof(float));
				}
			}
			ct_cut_build(&rt->cut, model_pos, pos, nrm, slot, b.vn, b.idx, list_n[0], 0.2f);
		}
		free(model_pos);
		free(pos);
		free(nrm);
		free(slot);
	}
	rt->kind = rm->kind;
	snprintf(rt->name, sizeof rt->name, "%s", rm->name);
	ok = 1;
	{
		int matched = 0;
		for (int c = 0; c < XWA_VR_CTL_COUNT; ++c) {
			matched += rt->ctl_node[c] >= 0;
		}
		Aeron_LogInfo("xwa.remaster",
					  "%s controller model \"%s\": %u vertices, %u triangles (mask copy %u), %d nodes (%d moving), "
					  "%d of %d images, %d controls found by name",
					  hand ? "right" : "left", rm->name, b.vn, b.in / 3, rt->cut.in / 3, rt->node_count,
					  rt->slot_count - 1, any_image ? image_count : 0, image_count, matched);
		Aeron_LogInfo("xwa.remaster",
					  "  %s model bounds at rest (grip space): x %.1f .. %.1f, y %.1f .. %.1f, z %.1f .. %.1f mm",
					  hand ? "right" : "left", (double)(bb_min[0] * 1000.0f), (double)(bb_max[0] * 1000.0f),
					  (double)(bb_min[1] * 1000.0f), (double)(bb_max[1] * 1000.0f), (double)(bb_min[2] * 1000.0f),
					  (double)(bb_max[2] * 1000.0f));
		for (int i = 0; i < rt->node_count && i < 64; ++i) {
			const cgltf_node* n = &data->nodes[i];
			char xf[160] = "";
			if (n->has_matrix) {
				snprintf(xf, sizeof xf, " matrix");
			} else {
				int at = 0;
				if (n->has_translation && at < (int)sizeof xf) {
					at += snprintf(xf + at, sizeof xf - (size_t)at, " t %.1f %.1f %.1f mm",
								   (double)(n->translation[0] * 1000.0f), (double)(n->translation[1] * 1000.0f),
								   (double)(n->translation[2] * 1000.0f));
				}
				if (n->has_rotation && at < (int)sizeof xf) {
					at += snprintf(xf + at, sizeof xf - (size_t)at, " r %.3f %.3f %.3f %.3f", (double)n->rotation[0],
								   (double)n->rotation[1], (double)n->rotation[2], (double)n->rotation[3]);
				}
				if (n->has_scale && at < (int)sizeof xf) {
					snprintf(xf + at, sizeof xf - (size_t)at, " s %.3f %.3f %.3f", (double)n->scale[0],
							 (double)n->scale[1], (double)n->scale[2]);
				}
			}
			Aeron_LogInfo("xwa.remaster", "  node %d \"%s\" parent %d%s%s%s%s", i, rt->nodes[i].name,
						  rt->nodes[i].parent, n->mesh ? " mesh" : "", n->skin ? " skinned" : "",
						  rt->nodes[i].animatable >= 0 ? " moving" : "", xf);
		}
	}
done:
	for (int c = 0; c < AERON_GLTF_CHANNEL_COUNT; ++c) {
		free(u.channels[c].data);
	}
	free(skin_m);
	free(atlas);
	free(b.v);
	free(b.idx);
	for (int l = 0; l < 3; ++l) {
		free(lists[l]);
	}
	free(mats);
	free(prim_material);
	cgltf_free(data);
	if (!ok) {
		ct_runtime_free(rt);
	}
	return ok;
}

/* This frame's node transforms (model space), moving nodes from the
 * runtime's poses. */
static void ct_runtime_pose(CtRuntime* rt, const AeronXrRenderModelNode* nodes, uint32_t node_count) {
	for (int i = 0; i < rt->node_count; ++i) {
		CtNode* n = &rt->nodes[i];
		float local[16];
		if (n->animatable >= 0 && (uint32_t)n->animatable < node_count) {
			const AeronXrRenderModelNode* s = &nodes[n->animatable];
			m4_from_pose(s->position, s->orientation, local);
		} else {
			memcpy(local, n->rest_local, sizeof local);
		}
		/* glTF lists parents first in practice; fall back to rest otherwise. */
		if (n->parent >= 0 && n->parent < i) {
			m4_mul(rt->nodes[n->parent].world, local, n->world);
		} else if (n->parent >= 0) {
			memcpy(n->world, n->rest_world, sizeof n->world);
		} else {
			memcpy(n->world, local, sizeof n->world);
		}
	}
	for (int s = 0; s < AERON_MAX_MESH_SLOTS; ++s) {
		float m[16];
		if (s > 0 && s < rt->slot_count) {
			memcpy(m, rt->nodes[rt->slot_node[s]].world, sizeof m);
		} else {
			m4_identity(m);
		}
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 4; ++c) {
				rt->table.rows[s][r][c] = m[c * 4 + r];
			}
		}
		int visible = 1;
		if (s > 0 && s < rt->slot_count) {
			for (int q = rt->slot_node[s]; q >= 0; q = rt->nodes[q].parent) {
				const int a = rt->nodes[q].animatable;
				if (a >= 0 && (uint32_t)a < node_count && !nodes[a].visible) {
					visible = 0;
				}
			}
		}
		rt->table.visibility_packed[s >> 2][s & 3] = visible ? 1.0f : 0.0f;
		rt->table.highlight_packed[s >> 2][s & 3] = 0.0f;
		rt->table.markings_packed[s >> 2][s & 3] = 0.0f;
		rt->table.emissive_packed[s >> 2][s & 3] = 1.0f;
	}
}

/* ---- Per hand ---------------------------------------------------------------- */

enum { CT_MAX_NODES = 64 };

typedef struct CtHand {
	CtGeneric generic;
	AeronSceneMesh* generic_mesh;
	CtRuntime runtime;
	int shown;
	int use_runtime;
	XwaVrControllerView view;
	XwaVrGloveCapsule capsules[2];
	CtCut generic_cut; /* the generic model's light copy */
	float* local;   /* the masks' mesh in LOCAL space */
	float* local_n;
	uint32_t local_cap;
	const uint16_t* cut_idx;
	uint32_t cut_vn, cut_in;
	AeronXrRenderModelNode nodes[CT_MAX_NODES];
} CtHand;

static struct {
	int tried, ok;
	CtHand hands[2];
	int logged;
} s_ct;

static int ct_ensure(AeronCommandBuffer* cmd) {
	if (s_ct.tried) {
		return s_ct.ok;
	}
	s_ct.tried = 1;
	CtGeneric* right = &s_ct.hands[1].generic;
	ct_generic_build(right);
	ct_generic_mirror(right, &s_ct.hands[0].generic);
	if (right->build.failed || s_ct.hands[0].generic.build.failed) {
		Aeron_LogWarn("xwa.remaster", "VR controllers: mesh building failed");
		return 0;
	}
	s_ct.hands[0].generic_mesh = ct_upload_generic(cmd, &s_ct.hands[0].generic, "xwa.vr_controller.generic_left");
	s_ct.hands[1].generic_mesh = ct_upload_generic(cmd, &s_ct.hands[1].generic, "xwa.vr_controller.generic_right");
	for (int h = 0; h < 2; ++h) {
		const CtBuild* gb = &s_ct.hands[h].generic.build;
		float* pos = (float*)malloc(gb->vn * 3 * sizeof(float));
		float* nrm = (float*)malloc(gb->vn * 3 * sizeof(float));
		if (pos && nrm) {
			for (uint32_t i = 0; i < gb->vn; ++i) {
				memcpy(&pos[i * 3], gb->v[i].pos, 3 * sizeof(float));
				memcpy(&nrm[i * 3], gb->v[i].normal, 3 * sizeof(float));
			}
			ct_cut_build(&s_ct.hands[h].generic_cut, pos, pos, nrm, NULL, gb->vn, gb->idx, gb->in, 0.2f);
		}
		free(pos);
		free(nrm);
	}
	s_ct.ok = s_ct.hands[0].generic_mesh && s_ct.hands[1].generic_mesh;
	if (!s_ct.ok) {
		Aeron_LogWarn("xwa.remaster", "VR controllers: scene meshes not created");
	} else {
		Aeron_LogInfo("xwa.remaster", "VR controllers: generic model %u vertices, %u triangles (mask copy %u)",
					  s_ct.hands[1].generic.build.vn, s_ct.hands[1].generic.build.in / 3,
					  s_ct.hands[1].generic_cut.in / 3);
	}
	return s_ct.ok;
}

/* Model space -> LOCAL -> scene, as row-major 4x4 for an instance. */
typedef struct CtToScene {
	float rot[9]; /* row-major, model -> LOCAL */
	float pos[3];
	float rt_c[9]; /* LOCAL axes -> world */
	float cam[3];
	float units;
} CtToScene;

static void ct_local_point(const CtToScene* x, const float p[3], float out[3]) {
	for (int i = 0; i < 3; ++i) {
		out[i] = x->rot[i * 3 + 0] * p[0] + x->rot[i * 3 + 1] * p[1] + x->rot[i * 3 + 2] * p[2] + x->pos[i];
	}
}

static void ct_local_vector(const CtToScene* x, const float v[3], float out[3]) {
	for (int i = 0; i < 3; ++i) {
		out[i] = x->rot[i * 3 + 0] * v[0] + x->rot[i * 3 + 1] * v[1] + x->rot[i * 3 + 2] * v[2];
	}
}

static void ct_to_scene(CtToScene* x, const float pos[3], const float q[4], const float crows[9], const float cam[3],
						float units) {
	const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
	const float r[9] = { 1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qw * qz),     2 * (qx * qz + qw * qy),
						 2 * (qx * qy + qw * qz),     1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qw * qx),
						 2 * (qx * qz - qw * qy),     2 * (qy * qz + qw * qx),     1 - 2 * (qx * qx + qy * qy) };
	memcpy(x->rot, r, sizeof r);
	memcpy(x->pos, pos, sizeof x->pos);
	for (int i = 0; i < 3; ++i) {
		x->rt_c[i * 3 + 0] = crows[0 * 3 + i];
		x->rt_c[i * 3 + 1] = -crows[1 * 3 + i];
		x->rt_c[i * 3 + 2] = -crows[2 * 3 + i];
		x->cam[i] = cam[i];
	}
	x->units = units;
}

static void ct_instance(AeronScene3D* scene, const AeronSceneMesh* mesh, const CtToScene* x,
						const AeronSceneMeshTable* table) {
	/* scene = cam + units * rt_c * (rot * p + pos) */
	float a[9], t[3];
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			a[i * 3 + j] = x->rt_c[i * 3 + 0] * x->rot[0 * 3 + j] + x->rt_c[i * 3 + 1] * x->rot[1 * 3 + j] +
						   x->rt_c[i * 3 + 2] * x->rot[2 * 3 + j];
		}
		t[i] = x->rt_c[i * 3 + 0] * x->pos[0] + x->rt_c[i * 3 + 1] * x->pos[1] + x->rt_c[i * 3 + 2] * x->pos[2];
	}
	AeronSceneMeshInstance inst;
	memset(&inst, 0, sizeof inst);
	inst.mesh = mesh;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			inst.transform[i * 4 + j] = a[i * 3 + j] * x->units;
		}
		inst.transform[i * 4 + 3] = x->cam[i] + t[i] * x->units;
	}
	inst.transform[15] = 1.0f;
	memcpy(inst.prev_transform, inst.transform, sizeof inst.transform);
	inst.mesh_table = table;
	inst.prev_mesh_table = table;
	inst.zero_velocity = 1;
	inst.draw_first = 1;
	inst.no_local_lights = 1;
	/* Never in the bloom: with one bloom for both eyes it would glow beside
	 * them in the right eye (doubled), so they stay just below the bloom
	 * threshold (as the cockpit does, flight.c fl_xr_cockpit_bloom_cap). */
	inst.base_color_emissive_strength = -0.98f;
	inst.shadow_flags = AERON_SCENE_INSTANCE_NO_CAST_SHADOW | AERON_SCENE_INSTANCE_NO_RECEIVE_SHADOW |
						AERON_SCENE_INSTANCE_EXCLUDE_FROM_RECEIVER_LOCAL_SHADOW;
	AeronScene_AddMeshInstance(scene, &inst);
}

static int ct_reserve_local(CtHand* h, uint32_t vn) {
	if (vn <= h->local_cap) {
		return 1;
	}
	float* a = (float*)realloc(h->local, vn * 3 * sizeof(float));
	if (!a) {
		return 0;
	}
	h->local = a;
	float* n = (float*)realloc(h->local_n, vn * 3 * sizeof(float));
	if (!n) {
		return 0;
	}
	h->local_n = n;
	h->local_cap = vn;
	return 1;
}

static int ct_runtime_wanted(void) {
	const char* env = getenv("XWA_XR_CONTROLLER_MODEL");
	return !(env && (env[0] == 'g' || env[0] == '0')); /* "generic" / 0: ours always */
}

void XwaVrControllers_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const float crows[9],
							 const float cam_pos[3], float units) {
	if (!scene || !cmd || !crows || !cam_pos || units <= 0.0f || !ct_ensure(cmd)) {
		XwaVrControllers_Idle();
		return;
	}
	for (int hand = 0; hand < 2; ++hand) {
		CtHand* h = &s_ct.hands[hand];
		AeronXrHand xh;
		h->shown = 0;
		if (!Aeron_XrGetHand(hand, &xh)) {
			continue;
		}
		/* The runtime's model, loaded (once per version) when it has one. */
		AeronXrRenderModel rm;
		h->use_runtime = 0;
		if (ct_runtime_wanted() && Aeron_XrGetRenderModel(hand, &rm) && rm.glb) {
			if (rm.generation != h->runtime.generation) {
				ct_runtime_free(&h->runtime);
				h->runtime.generation = rm.generation;
				ct_runtime_load(&h->runtime, cmd, hand, &rm);
			}
			if (h->runtime.mesh) {
				float pos[3], q[4];
				if (Aeron_XrLocateRenderModel(hand, pos, q, h->nodes, CT_MAX_NODES)) {
					uint32_t node_count = rm.node_count < CT_MAX_NODES ? rm.node_count : CT_MAX_NODES;
					ct_runtime_pose(&h->runtime, h->nodes, node_count);
					CtToScene x;
					ct_to_scene(&x, pos, q, crows, cam_pos, units);
					ct_instance(scene, h->runtime.mesh, &x, h->runtime.slot_count > 1 ? &h->runtime.table : NULL);
					h->use_runtime = 1;
					/* Controls found by name: where they are now. */
					XwaVrControllerView* v = &h->view;
					CtToScene by_aim;
					ct_to_scene(&by_aim, xh.position, xh.orientation, crows, cam_pos, units);
					for (int c = 0; c < XWA_VR_CTL_COUNT; ++c) {
						float p[3];
						if (h->runtime.ctl_node[c] >= 0) {
							m4_point(h->runtime.nodes[h->runtime.ctl_node[c]].world, h->runtime.ctl_local[c], p);
							ct_local_point(&x, p, v->anchor[c]);
						} else {
							ct_local_point(&by_aim, h->generic.anchor[c], v->anchor[c]);
						}
					}
					/* The masks' mesh: the model's light copy. */
					const CtCut* cut = &h->runtime.cut;
					if (cut->vn && ct_reserve_local(h, cut->vn)) {
						for (uint32_t i = 0; i < cut->vn; ++i) {
							const int s = cut->slot[i];
							float p[3], n[3];
							if (s > 0 && s < h->runtime.slot_count) {
								const float* w = h->runtime.nodes[h->runtime.slot_node[s]].world;
								m4_point(w, &cut->pos[i * 3], p);
								m4_vector(w, &cut->nrm[i * 3], n);
							} else {
								memcpy(p, &cut->pos[i * 3], sizeof p);
								memcpy(n, &cut->nrm[i * 3], sizeof n);
							}
							ct_local_point(&x, p, &h->local[i * 3]);
							ct_local_vector(&x, n, &h->local_n[i * 3]);
						}
						h->cut_idx = cut->idx;
						h->cut_vn = cut->vn;
						h->cut_in = cut->in;
					} else {
						h->cut_vn = 0;
					}
				}
			}
		}
		/* Always located from the aim pose: the generic model (when it is
		 * the one shown), the face, the outline and the capsules. */
		CtToScene aim;
		ct_to_scene(&aim, xh.position, xh.orientation, crows, cam_pos, units);
		const CtGeneric* g = &h->generic;
		if (!h->use_runtime) {
			ct_instance(scene, h->generic_mesh, &aim, NULL);
			for (int c = 0; c < XWA_VR_CTL_COUNT; ++c) {
				ct_local_point(&aim, g->anchor[c], h->view.anchor[c]);
			}
			h->cut_vn = 0;
		}
		const CtCut* gcut = &h->generic_cut;
		if (h->cut_vn == 0 && gcut->vn && ct_reserve_local(h, gcut->vn)) {
			for (uint32_t i = 0; i < gcut->vn; ++i) {
				ct_local_point(&aim, &gcut->pos[i * 3], &h->local[i * 3]);
				ct_local_vector(&aim, &gcut->nrm[i * 3], &h->local_n[i * 3]);
			}
			h->cut_idx = gcut->idx;
			h->cut_vn = gcut->vn;
			h->cut_in = gcut->in;
		}
		ct_local_point(&aim, g->face_center, h->view.face_center);
		ct_local_vector(&aim, g->face_normal, h->view.face_normal);
		h->view.outline_count = g->outline_count;
		for (int i = 0; i < g->outline_count; ++i) {
			ct_local_point(&aim, g->outline[i], h->view.outline[i]);
		}
		for (int i = 0; i < 2; ++i) {
			ct_local_point(&aim, g->capsules[i].a, h->capsules[i].a);
			ct_local_point(&aim, g->capsules[i].b, h->capsules[i].b);
			h->capsules[i].radius = g->capsules[i].radius;
		}
		h->view.runtime_model = h->use_runtime;
		h->shown = 1;
	}
	if (!s_ct.logged) {
		Aeron_LogInfo("xwa.remaster", "VR controllers on (in place of the gloves)");
		s_ct.logged = 1;
	}
}

void XwaVrControllers_Idle(void) {
	s_ct.hands[0].shown = 0;
	s_ct.hands[1].shown = 0;
}

int XwaVrControllers_View(int hand, XwaVrControllerView* out) {
	if (hand < 0 || hand > 1 || !s_ct.hands[hand].shown || !out) {
		return 0;
	}
	*out = s_ct.hands[hand].view;
	return 1;
}

int XwaVrControllers_Capsules(int hand, XwaVrGloveCapsule* out, int max_out) {
	if (hand < 0 || hand > 1 || !s_ct.hands[hand].shown || !out || max_out <= 0) {
		return 0;
	}
	const int n = max_out < 2 ? max_out : 2;
	memcpy(out, s_ct.hands[hand].capsules, (size_t)n * sizeof *out);
	return n;
}

int XwaVrControllers_Mesh(int hand, const float** local_xyz, const float** local_normals, int* vertex_count,
						  const uint16_t** indices, int* index_count) {
	if (hand < 0 || hand > 1 || !s_ct.hands[hand].shown || !s_ct.hands[hand].cut_vn) {
		return 0;
	}
	const CtHand* h = &s_ct.hands[hand];
	*local_xyz = h->local;
	*local_normals = h->local_n;
	*vertex_count = (int)h->cut_vn;
	*indices = h->cut_idx;
	*index_count = (int)h->cut_in;
	return 1;
}

void XwaVrControllers_Shutdown(void) {
	for (int i = 0; i < 2; ++i) {
		CtHand* h = &s_ct.hands[i];
		ct_runtime_free(&h->runtime);
		if (h->generic_mesh) {
			AeronScene_MeshDestroy(h->generic_mesh);
		}
		free(h->generic.build.v);
		free(h->generic.build.idx);
		ct_cut_free(&h->generic_cut);
		free(h->local);
		free(h->local_n);
	}
	memset(&s_ct, 0, sizeof s_ct);
}
