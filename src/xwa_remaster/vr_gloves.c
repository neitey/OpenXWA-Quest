/* VR pilot gloves; see vr_gloves.h.
 *
 * The glove meshes are the Godot XR Tools gloves (CC0, DigitalN8m4r3),
 * converted by tools/vr/gen_glove_mesh.py and decimated to about a third
 * of their triangles (tools/vr/decimate_glove_mesh.cpp): one skinned mesh
 * per hand with a 26-joint OpenXR-style skeleton. They are skinned on the
 * CPU every frame (about 920 vertices per hand) and uploaded into the
 * scene mesh. Like in
 * XR Tools the hand hangs off the controller's aim pose, offset by
 * (+-0.03, -0.05, 0.15) m. */

#include "xwa_remaster/vr_gloves.h"
#include <SDL3/SDL_stdinc.h>
#include "xwa_remaster/vr_controllers.h"

#include "aeron/aeron.h"
#include "aeron/log.h"
#include "aeron/scene/gltf_mesh.h"
#include "aeron/scene/mesh.h"
#include "xwa_remaster/vr_crest_pin.h"
#include "xwa_remaster/vr_glove_mesh.h"

#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Math: 3x3 row-major and 4x4 column-major affine --------------------- */

typedef struct M3 {
	float m[9];
} M3;

static M3 m3_mul(const M3* a, const M3* b) {
	M3 r;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			r.m[i * 3 + j] = a->m[i * 3 + 0] * b->m[0 * 3 + j] + a->m[i * 3 + 1] * b->m[1 * 3 + j] +
							 a->m[i * 3 + 2] * b->m[2 * 3 + j];
		}
	}
	return r;
}

static void m3_apply(const M3* a, const float v[3], float out[3]) {
	float t[3];
	for (int i = 0; i < 3; ++i) {
		t[i] = a->m[i * 3 + 0] * v[0] + a->m[i * 3 + 1] * v[1] + a->m[i * 3 + 2] * v[2];
	}
	memcpy(out, t, sizeof t);
}

/* Quaternion (x, y, z, w) -> rotation matrix. */
static M3 m3_from_quat(const float q[4]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	M3 r = { { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), 2 * (x * y + w * z),
			   1 - 2 * (x * x + z * z), 2 * (y * z - w * x), 2 * (x * z - w * y), 2 * (y * z + w * x),
			   1 - 2 * (x * x + y * y) } };
	return r;
}

static void v3_norm(float v[3]) {
	const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-8f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

static void v3_cross(const float a[3], const float b[3], float out[3]) {
	const float t[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	memcpy(out, t, sizeof t);
}

/* q = a * b (x, y, z, w). */
static void q_mul(const float a[4], const float b[4], float out[4]) {
	const float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
						 a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
						 a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
						 a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
	memcpy(out, r, sizeof r);
}

static void q_axis(float x, float y, float z, float rad, float out[4]) {
	const float s = sinf(0.5f * rad);
	out[0] = x * s;
	out[1] = y * s;
	out[2] = z * s;
	out[3] = cosf(0.5f * rad);
}

/* 4x4 column-major affine: m[col * 4 + row]. */
typedef struct M4 {
	float m[16];
} M4;

static M4 m4_mul(const M4* a, const M4* b) {
	M4 r;
	for (int c = 0; c < 4; ++c) {
		for (int rr = 0; rr < 4; ++rr) {
			r.m[c * 4 + rr] = a->m[0 * 4 + rr] * b->m[c * 4 + 0] + a->m[1 * 4 + rr] * b->m[c * 4 + 1] +
							  a->m[2 * 4 + rr] * b->m[c * 4 + 2] + a->m[3 * 4 + rr] * b->m[c * 4 + 3];
		}
	}
	return r;
}

static M4 m4_trs(const float t[3], const float q[4], const float s[3]) {
	const M3 r = m3_from_quat(q);
	M4 m;
	memset(&m, 0, sizeof m);
	for (int c = 0; c < 3; ++c) {
		for (int rr = 0; rr < 3; ++rr) {
			m.m[c * 4 + rr] = r.m[rr * 3 + c] * s[c];
		}
	}
	m.m[12] = t[0];
	m.m[13] = t[1];
	m.m[14] = t[2];
	m.m[15] = 1.0f;
	return m;
}

static void m4_point(const M4* m, const float p[3], float out[3]) {
	for (int r = 0; r < 3; ++r) {
		out[r] = m->m[0 * 4 + r] * p[0] + m->m[1 * 4 + r] * p[1] + m->m[2 * 4 + r] * p[2] + m->m[12 + r];
	}
}

static void m4_vector(const M4* m, const float v[3], float out[3]) {
	for (int r = 0; r < 3; ++r) {
		out[r] = m->m[0 * 4 + r] * v[0] + m->m[1 * 4 + r] * v[1] + m->m[2 * 4 + r] * v[2];
	}
}

/* Inverse of a rigid (rotation + translation) transform. */
static M4 m4_rigid_inverse(const M4* m) {
	M4 r;
	memset(&r, 0, sizeof r);
	for (int c = 0; c < 3; ++c) {
		for (int rr = 0; rr < 3; ++rr) {
			r.m[c * 4 + rr] = m->m[rr * 4 + c];
		}
	}
	for (int rr = 0; rr < 3; ++rr) {
		r.m[12 + rr] = -(r.m[0 * 4 + rr] * m->m[12] + r.m[1 * 4 + rr] * m->m[13] + r.m[2 * 4 + rr] * m->m[14]);
	}
	r.m[15] = 1.0f;
	return r;
}

/* ---- Meshes -------------------------------------------------------------- */

static void gl_material(AeronGltfMaterial* m, float r, float g, float b, float metallic, float roughness) {
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

typedef struct GlMeshSource {
	AeronGltfVertex* vertices;
	uint16_t* indices;
	uint32_t vertex_count, index_count;
	AeronGltfMaterial material;
	uint32_t prim_material;
	float pad_bounds; /* grow the bounds (the mesh moves when skinned) */
} GlMeshSource;

static AeronSceneMesh* gl_upload(AeronCommandBuffer* cmd, GlMeshSource* d, const char* name) {
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
			mn[k] = fminf(mn[k], d->vertices[i].pos[k] - d->pad_bounds);
			mx[k] = fmaxf(mx[k], d->vertices[i].pos[k] + d->pad_bounds);
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

/* ---- Hands ----------------------------------------------------------------- */

enum { GL_FINGERS = 4 };

typedef struct GlHandModel {
	const XwaVrGloveJoint* joints;
	const XwaVrGloveVertex* vertices;
	const uint16_t* indices;
	uint32_t vertex_count, index_count;
	float side; /* +1 right, -1 left */
	/* Joint indices. */
	int wrist, thumb[4], finger[GL_FINGERS][5]; /* metacarpal, proximal, intermediate, distal, tip */
	float rest_curl[XWA_VR_GLOVE_JOINTS];        /* rest bend about local Z, radians */
	/* Crest pin (right hand): bone and pin transform relative to it. */
	int pin_bone;
	M4 pin_local;
} GlHandModel;

typedef struct GlHand {
	GlHandModel model;
	GlMeshSource source; /* skinned vertex output and the upload source */
	AeronSceneMesh* mesh;
	M4 global[XWA_VR_GLOVE_JOINTS];
	int shown;
	float curl[GL_FINGERS]; /* 0 straight .. 1 fist */
	float thumb_up;         /* 0 resting on the controller .. 1 thumbs up */
	int pointing;
	float tip_local[3];
	XwaVrGloveCapsule capsules[24];
	int capsule_count;
	float* local;   /* skinned vertices in LOCAL space, xyz (for the hands' overlay masks) */
	float* local_n; /* their normals in LOCAL space (unnormalized) */
} GlHand;

static struct {
	int tried, ok;
	GlHand hands[2];
	GlMeshSource pin_source;
	AeronSceneMesh* pin_mesh;
	int logged;
} s_gl;

/* XWA_XR_GLOVE_SCALE: glove size (1 = the model's own, a large hand). */
static float gl_scale(void) {
	const char* env = SDL_getenv("XWA_XR_GLOVE_SCALE");
	const float v = env && env[0] ? (float)atof(env) : 0.85f;
	return v > 0.3f && v < 2.0f ? v : 0.85f;
}

int XwaVrHands_Mode(void) {
	/* XWA_XR_HANDS: gloves / controllers / off; before it, XWA_XR_GLOVES=0
	 * meant no hands. */
	const char* env = SDL_getenv("XWA_XR_HANDS");
	if (env && env[0]) {
		return env[0] == 'g' || env[0] == 'G'   ? XWA_VR_HANDS_GLOVES
			   : env[0] == 'c' || env[0] == 'C' ? XWA_VR_HANDS_CONTROLLERS
												: XWA_VR_HANDS_OFF;
	}
	const char* gloves = SDL_getenv("XWA_XR_GLOVES");
	return gloves && gloves[0] == '0' ? XWA_VR_HANDS_OFF : XWA_VR_HANDS_CONTROLLERS;
}

static int gl_enabled(void) { return XwaVrHands_Mode() == XWA_VR_HANDS_GLOVES; }

static int gl_find(const GlHandModel* m, const char* name) {
	const size_t n = strlen(name);
	for (int j = 0; j < XWA_VR_GLOVE_JOINTS; ++j) {
		/* Names end in _L / _R. */
		if (strncmp(m->joints[j].name, name, n) == 0 && m->joints[j].name[n] == '_') {
			return j;
		}
	}
	return -1;
}

static void gl_globals(const GlHandModel* m, const float (*rot)[4], M4* out) {
	for (int j = 0; j < XWA_VR_GLOVE_JOINTS; ++j) {
		const XwaVrGloveJoint* jt = &m->joints[j];
		const M4 local = m4_trs(jt->t, rot ? rot[j] : jt->r, jt->s);
		/* The generator emits parents before children. */
		out[j] = jt->parent >= 0 ? m4_mul(&out[jt->parent], &local) : local;
	}
}

static int gl_model_init(GlHandModel* m, int right) {
	memset(m, 0, sizeof *m);
	m->joints = right ? k_glove_r_joints : k_glove_l_joints;
	m->vertices = right ? k_glove_r_vertices : k_glove_l_vertices;
	m->indices = right ? k_glove_r_indices : k_glove_l_indices;
	m->vertex_count = right ? XWA_VR_GLOVE_R_VERTICES : XWA_VR_GLOVE_L_VERTICES;
	m->index_count = right ? XWA_VR_GLOVE_R_INDICES : XWA_VR_GLOVE_L_INDICES;
	m->side = right ? 1.0f : -1.0f;
	for (int j = 0; j < XWA_VR_GLOVE_JOINTS; ++j) {
		if (m->joints[j].parent >= j) {
			return 0; /* parents must come first */
		}
	}
	static const char* const fingers[GL_FINGERS] = { "Index", "Middle", "Ring", "Little" };
	static const char* const parts[5] = { "Metacarpal", "Proximal", "Intermediate", "Distal", "Tip" };
	static const char* const thumb_parts[4] = { "Metacarpal", "Proximal", "Distal", "Tip" };
	char name[64];
	m->wrist = gl_find(m, "Wrist");
	for (int k = 0; k < 4; ++k) {
		snprintf(name, sizeof name, "Thumb_%s", thumb_parts[k]);
		m->thumb[k] = gl_find(m, name);
	}
	for (int f = 0; f < GL_FINGERS; ++f) {
		for (int k = 0; k < 5; ++k) {
			snprintf(name, sizeof name, "%s_%s", fingers[f], parts[k]);
			m->finger[f][k] = gl_find(m, name);
			if (m->finger[f][k] < 0) {
				return 0;
			}
		}
	}
	if (m->wrist < 0 || m->thumb[0] < 0 || m->thumb[3] < 0) {
		return 0;
	}
	for (int j = 0; j < XWA_VR_GLOVE_JOINTS; ++j) {
		const float* q = m->joints[j].r;
		m->rest_curl[j] = 2.0f * atan2f(q[2], q[3]);
	}
	return 1;
}

/* The crest pin: on the back of the hand over the index metacarpal (just
 * above the thumb), on the glove's surface, its top pointing along the
 * fingers. */
static void gl_pin_init(GlHandModel* m) {
	M4 rest[XWA_VR_GLOVE_JOINTS];
	gl_globals(m, NULL, rest);
	const int bone = m->finger[0][0];
	float a[3], b[3], target[3];
	const float zero[3] = { 0, 0, 0 };
	m4_point(&rest[m->finger[0][0]], zero, a);
	m4_point(&rest[m->finger[0][1]], zero, b);
	for (int k = 0; k < 3; ++k) {
		target[k] = a[k] + 0.55f * (b[k] - a[k]);
	}
	/* Back of the right hand faces +X in the aim frame (handshake grip) and
	 * the index side +Y: the upper back edge is what the pilot sees. */
	float dorsal[3] = { m->side, 0.75f, 0.0f };
	v3_norm(dorsal);
	int best = -1;
	float best_score = -1e9f;
	for (uint32_t i = 0; i < m->vertex_count; ++i) {
		const XwaVrGloveVertex* v = &m->vertices[i];
		const float d[3] = { v->pos[0] - target[0], v->pos[1] - target[1], v->pos[2] - target[2] };
		const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
		const float facing = v->normal[0] * dorsal[0] + v->normal[1] * dorsal[1] + v->normal[2] * dorsal[2];
		if (dist2 > 0.03f * 0.03f || facing < 0.5f) {
			continue;
		}
		const float score = facing - dist2 * 800.0f;
		if (score > best_score) {
			best_score = score;
			best = (int)i;
		}
	}
	float pos[3], n[3];
	if (best >= 0) {
		memcpy(pos, m->vertices[best].pos, sizeof pos);
		memcpy(n, m->vertices[best].normal, sizeof n);
	} else {
		memcpy(pos, target, sizeof pos);
		memcpy(n, dorsal, sizeof n);
	}
	v3_norm(n);
	float fwd[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	/* Up of the emblem: along the fingers, made perpendicular to n. */
	const float dn = fwd[0] * n[0] + fwd[1] * n[1] + fwd[2] * n[2];
	for (int k = 0; k < 3; ++k) {
		fwd[k] -= dn * n[k];
	}
	v3_norm(fwd);
	float x[3];
	v3_cross(fwd, n, x);
	M4 pin;
	memset(&pin, 0, sizeof pin);
	for (int k = 0; k < 3; ++k) {
		pin.m[0 + k] = x[k];
		pin.m[4 + k] = fwd[k];
		pin.m[8 + k] = n[k];
		pin.m[12 + k] = pos[k] + n[k] * 0.0004f;
	}
	pin.m[15] = 1.0f;
	const M4 inv = m4_rigid_inverse(&rest[bone]);
	m->pin_bone = bone;
	m->pin_local = m4_mul(&inv, &pin);
}

static int gl_ensure(AeronCommandBuffer* cmd) {
	if (s_gl.tried) {
		return s_gl.ok;
	}
	s_gl.tried = 1;
	int ok = 1;
	const float grey = 0.055f; /* dark grey, linear */
	for (int hand = 0; hand < 2 && ok; ++hand) {
		GlHand* h = &s_gl.hands[hand];
		ok = gl_model_init(&h->model, hand == 1);
		if (!ok) {
			break;
		}
		GlMeshSource* src = &h->source;
		src->vertex_count = h->model.vertex_count;
		src->index_count = h->model.index_count;
		src->vertices = (AeronGltfVertex*)calloc(src->vertex_count, sizeof *src->vertices);
		src->indices = (uint16_t*)malloc(src->index_count * sizeof *src->indices);
		if (!src->vertices || !src->indices) {
			ok = 0;
			break;
		}
		memcpy(src->indices, h->model.indices, src->index_count * sizeof *src->indices);
		for (uint32_t i = 0; i < src->vertex_count; ++i) {
			memcpy(src->vertices[i].pos, h->model.vertices[i].pos, sizeof src->vertices[i].pos);
			memcpy(src->vertices[i].normal, h->model.vertices[i].normal, sizeof src->vertices[i].normal);
			src->vertices[i].tangent[3] = 1.0f;
		}
		src->pad_bounds = 0.08f;
		/* Matte (roughness 0.9+): the lite mesh shader then skips the
		 * specular term and the face-normal derivatives for the gloves,
		 * which can cover a good part of the view up close. */
		gl_material(&src->material, grey, grey, grey * 1.08f, 0.0f, 0.92f);
		h->mesh = gl_upload(cmd, src, hand ? "vr.glove.right" : "vr.glove.left");
		ok = h->mesh != NULL;
	}
	if (ok) {
		gl_pin_init(&s_gl.hands[1].model);
		GlMeshSource* pin = &s_gl.pin_source;
		pin->vertex_count = XWA_VR_CREST_PIN_VERTEX_COUNT;
		pin->index_count = XWA_VR_CREST_PIN_VERTEX_COUNT;
		pin->vertices = (AeronGltfVertex*)calloc(pin->vertex_count, sizeof *pin->vertices);
		pin->indices = (uint16_t*)malloc(pin->index_count * sizeof *pin->indices);
		ok = pin->vertices && pin->indices;
		for (uint32_t i = 0; ok && i < pin->vertex_count; ++i) {
			for (int k = 0; k < 3; ++k) {
				pin->vertices[i].pos[k] = k_vr_crest_pin[i][k];
				pin->vertices[i].normal[k] = k_vr_crest_pin[i][3 + k];
			}
			pin->vertices[i].tangent[3] = 1.0f;
			pin->indices[i] = (uint16_t)i;
		}
		if (ok) {
			gl_material(&pin->material, 1.0f, 0.77f, 0.34f, 1.0f, 0.32f); /* gold */
			s_gl.pin_mesh = gl_upload(cmd, pin, "vr.glove.crest_pin");
			ok = s_gl.pin_mesh != NULL;
		}
	}
	if (!ok) {
		Aeron_LogWarn("xwa.remaster", "VR gloves: setup failed, gloves are off");
	}
	s_gl.ok = ok;
	return ok;
}

/* Joint rotations for this frame's curls. Fingers bend about their local
 * Z (the rest pose is slightly curled; 0 straightens that out). */
static void gl_pose(const GlHand* h, float (*rot)[4]) {
	const GlHandModel* m = &h->model;
	for (int j = 0; j < XWA_VR_GLOVE_JOINTS; ++j) {
		memcpy(rot[j], m->joints[j].r, sizeof rot[j]);
	}
	/* Proximal, intermediate, distal bend at full curl. The index wraps a
	 * trigger, not a fist: less. */
	static const float bend[3] = { 1.30f, 1.65f, 1.05f };
	static const float index_bend[3] = { 0.85f, 0.95f, 0.55f };
	/* A relaxed straight finger keeps a slight curve. */
	static const float relaxed[3] = { 0.06f, 0.10f, 0.06f };
	for (int f = 0; f < GL_FINGERS; ++f) {
		for (int k = 0; k < 3; ++k) {
			const int j = m->finger[f][1 + k];
			const float b = f == 0 ? index_bend[k] : bend[k];
			/* Pure flexion from the bone lined up with its parent: the
			 * model's rest pose kinks the fingers sideways and bends the
			 * index knuckle backwards, which looks broken when pointing.
			 * Right fingers curl about +Z, left about -Z. */
			q_axis(0.0f, 0.0f, 1.0f, m->side * (relaxed[k] + h->curl[f] * b), rot[j]);
		}
	}
	/* Thumb: the rest pose lies on the controller; thumbs up straightens it
	 * and swings it up. */
	if (h->thumb_up > 0.0f) {
		float q[4];
		q_axis(1.0f, 0.0f, 0.0f, -0.55f * h->thumb_up, q);
		q_mul(m->joints[m->thumb[0]].r, q, rot[m->thumb[0]]);
		for (int k = 1; k < 3; ++k) {
			const int j = m->thumb[k];
			q_axis(0.0f, 0.0f, 1.0f, -m->rest_curl[j] * h->thumb_up, q);
			q_mul(m->joints[j].r, q, rot[j]);
		}
	}
}

/* Skins the mesh into h->source.vertices with h->global. */
static void gl_skin(GlHand* h) {
	const GlHandModel* m = &h->model;
	M4 skin[XWA_VR_GLOVE_JOINTS];
	for (int j = 0; j < XWA_VR_GLOVE_JOINTS; ++j) {
		M4 ibm;
		memcpy(ibm.m, m->joints[j].inverse_bind, sizeof ibm.m);
		skin[j] = m4_mul(&h->global[j], &ibm);
	}
	for (uint32_t i = 0; i < m->vertex_count; ++i) {
		const XwaVrGloveVertex* v = &m->vertices[i];
		float p[3] = { 0, 0, 0 }, n[3] = { 0, 0, 0 };
		for (int k = 0; k < 4; ++k) {
			const float w = v->weights[k];
			if (w <= 0.0f) {
				continue;
			}
			const M4* s = &skin[v->joints[k] < XWA_VR_GLOVE_JOINTS ? v->joints[k] : 0];
			float tp[3], tn[3];
			m4_point(s, v->pos, tp);
			m4_vector(s, v->normal, tn);
			for (int a = 0; a < 3; ++a) {
				p[a] += w * tp[a];
				n[a] += w * tn[a];
			}
		}
		v3_norm(n);
		memcpy(h->source.vertices[i].pos, p, sizeof p);
		memcpy(h->source.vertices[i].normal, n, sizeof n);
	}
}

static void gl_capsule(GlHand* h, const float a[3], const float b[3], float r) {
	if (h->capsule_count < (int)(sizeof h->capsules / sizeof h->capsules[0])) {
		XwaVrGloveCapsule* c = &h->capsules[h->capsule_count++];
		memcpy(c->a, a, sizeof c->a);
		memcpy(c->b, b, sizeof c->b);
		c->radius = r;
	}
}

/* Smoothly approach `goal`. */
static float gl_ease(float v, float goal, float dt) {
	const float a = dt > 0.0f ? 1.0f - expf(-dt / 0.06f) : 1.0f;
	return v + (goal - v) * a;
}

/* A transform from hand space (the glove's model space) to LOCAL space and
 * on to the scene, as row-major 4x4 for an instance. */
typedef struct GlToScene {
	M3 hand_rot;      /* aim orientation, times the glove scale */
	float hand_pos[3]; /* aim position + glove offset, LOCAL */
	M3 rt_c;          /* world <- ship eye <- LOCAL axes */
	float cam[3];
	float units;
} GlToScene;

static void gl_local_point(const GlToScene* x, const float p[3], float out[3]) {
	m3_apply(&x->hand_rot, p, out);
	for (int k = 0; k < 3; ++k) {
		out[k] += x->hand_pos[k];
	}
}

static void gl_instance(AeronScene3D* scene, const AeronSceneMesh* mesh, const GlToScene* x, const M4* model) {
	/* scene = cam + units * rt_c * (hand_rot * (model * p) + hand_pos) */
	const M3 a = m3_mul(&x->rt_c, &x->hand_rot);
	float t[3];
	m3_apply(&x->rt_c, x->hand_pos, t);
	AeronSceneMeshInstance inst;
	memset(&inst, 0, sizeof inst);
	inst.mesh = mesh;
	for (int i = 0; i < 3; ++i) {
		float mt = 0.0f;
		for (int j = 0; j < 3; ++j) {
			float v = 0.0f;
			for (int k = 0; k < 3; ++k) {
				v += a.m[i * 3 + k] * (model ? model->m[j * 4 + k] : (k == j ? 1.0f : 0.0f));
			}
			inst.transform[i * 4 + j] = v * x->units;
		}
		if (model) {
			for (int k = 0; k < 3; ++k) {
				mt += a.m[i * 3 + k] * model->m[12 + k];
			}
		}
		inst.transform[i * 4 + 3] = x->cam[i] + (t[i] + mt) * x->units;
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

static void gl_env3(const char* name, float out[3]) {
	const char* v = SDL_getenv(name);
	out[0] = out[1] = out[2] = 0.0f;
	if (v && v[0]) {
		sscanf(v, "%f,%f,%f", &out[0], &out[1], &out[2]);
	}
}

void XwaVrGloves_Submit(AeronScene3D* scene, AeronCommandBuffer* cmd, const float crows[9], const float cam_pos[3],
						float units) {
	static uint64_t last_ns;
	const uint64_t now_ns = SDL_GetTicksNS();
	const float dt = last_ns ? (float)((double)(now_ns - last_ns) * 1e-9) : 0.0f;
	last_ns = now_ns;
	if (XwaVrHands_Mode() == XWA_VR_HANDS_CONTROLLERS) {
		s_gl.hands[0].shown = 0;
		s_gl.hands[1].shown = 0;
		XwaVrControllers_Submit(scene, cmd, crows, cam_pos, units);
		return;
	}
	XwaVrControllers_Idle();
	if (!scene || !cmd || !crows || !cam_pos || units <= 0.0f || !gl_enabled() || !gl_ensure(cmd)) {
		XwaVrGloves_Idle();
		return;
	}
	float extra[3];
	gl_env3("XWA_XR_GLOVE_OFFSET", extra);
	for (int hand = 0; hand < 2; ++hand) {
		GlHand* h = &s_gl.hands[hand];
		const GlHandModel* m = &h->model;
		AeronXrHand xh;
		h->shown = 0;
		h->capsule_count = 0;
		if (!Aeron_XrGetHand(hand, &xh)) {
			continue;
		}
		/* Pose from the controller: finger on the trigger -> resting curl
		 * that tightens with the pull; lifted -> pointing. */
		float index_goal;
		if (xh.index_touch == 1) {
			index_goal = 0.35f + 0.6f * xh.trigger;
		} else if (xh.index_touch == 0) {
			/* Lifted: always pointing, grip held or not (only the relaxed
			 * curve's slight natural bend). */
			index_goal = 0.0f;
		} else {
			index_goal = 0.9f * xh.trigger;
		}
		/* Relaxed (grip open) the fingers stay half closed. */
		const float grip_goal = 0.65f + 0.30f * xh.grip;
		float thumb_goal = 0.0f;
		if (xh.thumb_touch == 0) {
			thumb_goal = xh.grip > 0.5f ? 1.0f : 0.4f;
		}
		h->curl[0] = gl_ease(h->curl[0], index_goal, dt);
		for (int f = 1; f < GL_FINGERS; ++f) {
			h->curl[f] = gl_ease(h->curl[f], grip_goal, dt);
		}
		h->thumb_up = gl_ease(h->thumb_up, thumb_goal, dt);
		h->pointing = xh.index_touch != 1; /* lifted, or no touch sensing */

		float rot[XWA_VR_GLOVE_JOINTS][4];
		gl_pose(h, rot);
		gl_globals(m, (const float(*)[4])rot, h->global);
		gl_skin(h);
		AeronScene_MeshUpdateVertices(cmd, h->mesh, h->source.vertices, h->source.vertex_count);

		/* Hand space -> LOCAL -> scene. */
		/* Scaled about the controller: the hand keeps its grip on it. */
		const float scale = gl_scale();
		GlToScene x;
		x.hand_rot = m3_from_quat(xh.orientation);
		{
			const float offset[3] = { (0.03f * m->side + extra[0] * m->side) * scale, (-0.05f + extra[1]) * scale,
									  (0.15f + extra[2]) * scale };
			float o[3];
			m3_apply(&x.hand_rot, offset, o);
			for (int k = 0; k < 3; ++k) {
				x.hand_pos[k] = xh.position[k] + o[k];
			}
		}
		for (int k = 0; k < 9; ++k) {
			x.hand_rot.m[k] *= scale;
		}
		for (int i = 0; i < 3; ++i) {
			x.rt_c.m[i * 3 + 0] = crows[0 * 3 + i];
			x.rt_c.m[i * 3 + 1] = -crows[1 * 3 + i];
			x.rt_c.m[i * 3 + 2] = -crows[2 * 3 + i];
			x.cam[i] = cam_pos[i];
		}
		x.units = units;
		gl_instance(scene, h->mesh, &x, NULL);
		if (!h->local) {
			h->local = (float*)malloc((size_t)h->source.vertex_count * 3 * sizeof(float));
			h->local_n = (float*)malloc((size_t)h->source.vertex_count * 3 * sizeof(float));
		}
		if (h->local && h->local_n) {
			for (uint32_t i = 0; i < h->source.vertex_count; ++i) {
				gl_local_point(&x, h->source.vertices[i].pos, &h->local[i * 3]);
				m3_apply(&x.hand_rot, h->source.vertices[i].normal, &h->local_n[i * 3]);
			}
		}
		if (hand == 1 && s_gl.pin_mesh) {
			const M4 pin = m4_mul(&h->global[m->pin_bone], &m->pin_local);
			gl_instance(scene, s_gl.pin_mesh, &x, &pin);
		}

		/* Fingertip and the shape as capsules, LOCAL space. */
		const float zero[3] = { 0, 0, 0 };
		float p[3];
		m4_point(&h->global[m->finger[0][4]], zero, p);
		gl_local_point(&x, p, h->tip_local);
		for (int f = 0; f < GL_FINGERS; ++f) {
			for (int k = 1; k < 4; ++k) {
				float a[3], b[3];
				m4_point(&h->global[m->finger[f][k]], zero, p);
				gl_local_point(&x, p, a);
				m4_point(&h->global[m->finger[f][k + 1]], zero, p);
				gl_local_point(&x, p, b);
				gl_capsule(h, a, b, (f == 3 ? 0.0085f : 0.0095f) * scale);
			}
		}
		for (int k = 0; k < 3; ++k) {
			float a[3], b[3];
			m4_point(&h->global[m->thumb[k]], zero, p);
			gl_local_point(&x, p, a);
			m4_point(&h->global[m->thumb[k + 1]], zero, p);
			gl_local_point(&x, p, b);
			gl_capsule(h, a, b, 0.011f * scale);
		}
		{
			float a[3], b[3];
			m4_point(&h->global[m->wrist], zero, p);
			gl_local_point(&x, p, a);
			m4_point(&h->global[m->finger[1][1]], zero, p);
			gl_local_point(&x, p, b);
			gl_capsule(h, a, b, 0.028f * scale);
		}
		h->shown = 1;
	}
	if (!s_gl.logged) {
		Aeron_LogInfo("xwa.remaster", "VR gloves on (%u + %u vertices skinned per frame)",
					  s_gl.hands[0].model.vertex_count, s_gl.hands[1].model.vertex_count);
		s_gl.logged = 1;
	}
}

void XwaVrGloves_Idle(void) {
	s_gl.hands[0].shown = 0;
	s_gl.hands[1].shown = 0;
	XwaVrControllers_Idle();
}

int XwaVrGloves_IndexTip(int hand, float out[3], int* pointing) {
	/* Controllers: no fingertip (the console pokes with the controller). */
	if (hand < 0 || hand > 1 || !s_gl.hands[hand].shown) {
		return 0;
	}
	memcpy(out, s_gl.hands[hand].tip_local, sizeof s_gl.hands[hand].tip_local);
	if (pointing) {
		*pointing = s_gl.hands[hand].pointing;
	}
	return 1;
}

int XwaVrGloves_Capsules(int hand, XwaVrGloveCapsule* out, int max_out) {
	if (XwaVrHands_Mode() == XWA_VR_HANDS_CONTROLLERS) {
		return XwaVrControllers_Capsules(hand, out, max_out);
	}
	if (hand < 0 || hand > 1 || !s_gl.hands[hand].shown || !out) {
		return 0;
	}
	const int n = s_gl.hands[hand].capsule_count < max_out ? s_gl.hands[hand].capsule_count : max_out;
	memcpy(out, s_gl.hands[hand].capsules, (size_t)n * sizeof *out);
	return n;
}

int XwaVrGloves_Mesh(int hand, const float** local_xyz, const float** local_normals, int* vertex_count,
					 const uint16_t** indices, int* index_count) {
	if (XwaVrHands_Mode() == XWA_VR_HANDS_CONTROLLERS) {
		return XwaVrControllers_Mesh(hand, local_xyz, local_normals, vertex_count, indices, index_count);
	}
	if (hand < 0 || hand > 1 || !s_gl.hands[hand].shown || !s_gl.hands[hand].local || !s_gl.hands[hand].local_n) {
		return 0;
	}
	const GlHand* h = &s_gl.hands[hand];
	*local_xyz = h->local;
	*local_normals = h->local_n;
	*vertex_count = (int)h->source.vertex_count;
	*indices = h->source.indices;
	*index_count = (int)h->source.index_count;
	return 1;
}

void XwaVrGloves_Shutdown(void) {
	XwaVrControllers_Shutdown();
	for (int h = 0; h < 2; ++h) {
		free(s_gl.hands[h].local);
		free(s_gl.hands[h].local_n);
		if (s_gl.hands[h].mesh) {
			AeronScene_MeshDestroy(s_gl.hands[h].mesh);
		}
		free(s_gl.hands[h].source.vertices);
		free(s_gl.hands[h].source.indices);
	}
	if (s_gl.pin_mesh) {
		AeronScene_MeshDestroy(s_gl.pin_mesh);
	}
	free(s_gl.pin_source.vertices);
	free(s_gl.pin_source.indices);
	memset(&s_gl, 0, sizeof s_gl);
}
