#include "xwa_remaster/vr_hand_overlay.h"
#include <SDL3/SDL_stdinc.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>

#include "aeron/log.h"
#include "xwa_remaster/vr_env.h"
#include "xwa_remaster/vr_flight_stick.h"
#include "xwa_remaster/vr_gloves.h"

/*
 * Each eye gets one small mask per source (left hand, right hand, the
 * flight stick: meshes in LOCAL space), rasterised on the CPU over that
 * source's box in the eye image and drawn there as a sprite into the eye
 * image's alpha. A mask texel covers at least two eye pixels, so the
 * bilinear edge stays a couple of pixels wide.
 *
 * Cost per eye: every vertex is projected once (the eye's basis and the
 * field of view's tangents are worked out once per eye, not per vertex),
 * then only the triangles turned towards the eye are filled, each by
 * walking its edges row by row.
 *
 * The left eye's masks are filled in Prepare; the right eye's on a worker
 * thread meanwhile (the main thread goes on to draw the left eye), taken
 * in by Finish before the right eye is drawn. The worker reads only this
 * file's own copies of the meshes, made in Prepare.
 * XWA_XR_HANDS_OVERLAY_THREAD=0 fills both on the main thread.
 */
enum {
	HV_CELL = 256,  /* mask texels per side, per source */
	HV_SOURCES = 3, /* left hand, right hand, flight stick */
	HV_STICK = 2,
	HV_MAX_VERTS = 16384,
	HV_MAX_INDICES = 3 * 16384,
	HV_MAX_LAYERS = 24,
};

typedef struct HvEye {
	float m[9]; /* LOCAL -> eye (x right, y up, -z ahead), row-major */
	float pos[3];
	float tl, tu, kx, ky; /* pixel = ((x / -z) - tl) * kx, (tu - (y / -z)) * ky */
	int w, h;
} HvEye;

/* One source's mask this eye: where it lies in eye pixels. */
typedef struct HvCell {
	float ox, oy, scale; /* mask texel (0,0) at eye pixel (ox, oy); eye pixels per texel */
	int mw, mh;
} HvCell;

/* A layer that may hide part of a source, seen from the eye being filled. */
typedef struct HvNear {
	float n[3], ax[3], ay[3]; /* the quad's normal and axes, LOCAL */
	float c[3];               /* its centre, from the eye */
	float num;                /* c . n */
	float hw, hh;             /* half its size */
} HvNear;

/* A source's mesh this frame (LOCAL), copied from the gloves / stick. */
typedef struct HvSource {
	float* v;
	float* n;
	uint16_t* idx;
	int vn, in;
} HvSource;

/* What filling one eye's masks works with (one per eye: the two eyes may
 * be filled at the same time). */
typedef struct HvWork {
	uint8_t facing[HV_MAX_VERTS], behind[HV_MAX_VERTS];
	float tx[HV_MAX_VERTS], ty[HV_MAX_VERTS];
	/* The layers near enough to hide part of the source being filled. */
	HvNear near[HV_MAX_LAYERS];
	int near_count;
	uint8_t* px; /* the mask being filled */
	int mw, mh;
} HvWork;

static struct {
	AeronDrawList2D* list[2];
	AeronTexture* tex[2][HV_SOURCES];
	int ready[2];
	int rect[2][4];
	/* Each source's mask this frame, per eye. */
	uint8_t cells[2][HV_SOURCES][HV_CELL * HV_CELL * 4];
	HvCell cell[2][HV_SOURCES];
	int filled[2][HV_SOURCES];
	HvWork work[2];
	HvSource src[HV_SOURCES];
	HvEye eyes[2];
	float boxes[2][HV_SOURCES][4];
	int have[2][HV_SOURCES];
	/* Each source's vertices seen from each eye (projected once). */
	float sx[2][HV_SOURCES][HV_MAX_VERTS], sy[2][HV_SOURCES][HV_MAX_VERTS];
	uint8_t ok[2][HV_SOURCES][HV_MAX_VERTS];
	/* SetLayers: the quads (flight HUD and panels), else the menu screen. */
	AeronXrPanel layers[HV_MAX_LAYERS];
	float layer_rot[HV_MAX_LAYERS][9];
	int layer_count;
	int layers_set;
	/* Each eye's view of the layers: 1 a box, -1 may cover anything, 0 out
	 * of view. */
	float layer_box[2][HV_MAX_LAYERS][4];
	int layer_box_ok[2][HV_MAX_LAYERS];
	/* The right eye: still to be taken in by Finish; being filled by the
	 * worker. */
	int right_todo;
	int right_pending;
	/* The worker. */
	SDL_Thread* thread;
	SDL_Semaphore* go;
	SDL_Semaphore* done;
	int thread_state; /* 0 not tried, 1 running, -1 none (fill on the main thread) */
	volatile int quit;
} s_hv;

static int hv_enabled(void) {
	static int checked, enabled;
	if (!checked) {
		const char* env = SDL_getenv("XWA_XR_HANDS_OVER_SCREEN");
		enabled = !(env && env[0] == '0');
		checked = 1;
	}
	return enabled;
}

/* XWA_XR_HANDS_OVERLAY_CROP=0: the overlay covers the whole eye image
 * (should a runtime mishandle the cropped one). */
static int hv_crop(void) {
	static int checked, crop;
	if (!checked) {
		const char* env = SDL_getenv("XWA_XR_HANDS_OVERLAY_CROP");
		crop = !(env && env[0] == '0');
		checked = 1;
	}
	return crop;
}

static void hv_join(void);

void XwaVrHandOverlay_SetLayers(const AeronXrPanel* quads, int count) {
	hv_join(); /* the worker reads the layers */
	s_hv.layers_set = 1;
	s_hv.layer_count = 0;
	for (int i = 0; quads && i < count && s_hv.layer_count < HV_MAX_LAYERS; ++i) {
		if (quads[i].size[0] > 0.0f && quads[i].size[1] > 0.0f) {
			s_hv.layers[s_hv.layer_count++] = quads[i];
		}
	}
}

/* Rotation matrix (row-major) of unit quaternion q (x, y, z, w). */
static void hv_quat_m(const float q[4], float m[9]) {
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	m[0] = 1.0f - 2.0f * (y * y + z * z);
	m[1] = 2.0f * (x * y - w * z);
	m[2] = 2.0f * (x * z + w * y);
	m[3] = 2.0f * (x * y + w * z);
	m[4] = 1.0f - 2.0f * (x * x + z * z);
	m[5] = 2.0f * (y * z - w * x);
	m[6] = 2.0f * (x * z - w * y);
	m[7] = 2.0f * (y * z + w * x);
	m[8] = 1.0f - 2.0f * (x * x + y * y);
}

static void hv_eye_setup(const AeronXrView* view, int w, int h, HvEye* e) {
	float r[9];
	hv_quat_m(view->orientation, r);
	/* LOCAL -> eye is the transpose. */
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			e->m[i * 3 + j] = r[j * 3 + i];
		}
	}
	memcpy(e->pos, view->position, sizeof e->pos);
	const float tl = tanf(view->fov[0]), tr = tanf(view->fov[1]);
	const float tu = tanf(view->fov[2]), td = tanf(view->fov[3]);
	e->tl = tl;
	e->tu = tu;
	e->kx = (float)w / (tr - tl);
	e->ky = (float)h / (tu - td);
	e->w = w;
	e->h = h;
}

/* LOCAL point -> eye image pixel; 0 behind (or at) the eye. */
static inline int hv_project(const HvEye* e, const float p[3], float* x, float* y) {
	const float d0 = p[0] - e->pos[0], d1 = p[1] - e->pos[1], d2 = p[2] - e->pos[2];
	const float ex = e->m[0] * d0 + e->m[1] * d1 + e->m[2] * d2;
	const float ey = e->m[3] * d0 + e->m[4] * d1 + e->m[5] * d2;
	const float ez = e->m[6] * d0 + e->m[7] * d1 + e->m[8] * d2;
	if (-ez < 0.01f) {
		return 0;
	}
	const float inv = 1.0f / -ez;
	*x = (ex * inv - e->tl) * e->kx;
	*y = (e->tu - ey * inv) * e->ky;
	return 1;
}

#define HV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define HV_MAX(a, b) ((a) > (b) ? (a) : (b))

/* One triangle (mask texel space) into w's mask, coverage at texel
 * centres, either winding: sorted by y, then two halves (top vertex to the
 * middle one, middle to the bottom one), each walking its two edges down
 * one row at a time and filling the span between them. A texel covered is
 * set whole (all four bytes 255: only the alpha is read). */
static void hv_tri(HvWork* w, float x0, float y0, float x1, float y1, float x2, float y2) {
	const int mw = w->mw, mh = w->mh;
	/* Sort by y: (x0, y0) top, (x2, y2) bottom. */
	float t;
	if (y1 < y0) {
		t = x0, x0 = x1, x1 = t;
		t = y0, y0 = y1, y1 = t;
	}
	if (y2 < y1) {
		t = x1, x1 = x2, x2 = t;
		t = y1, y1 = y2, y2 = t;
		if (y1 < y0) {
			t = x0, x0 = x1, x1 = t;
			t = y0, y0 = y1, y1 = t;
		}
	}
	/* Rows whose centre (y + 0.5) lies within the triangle's span. */
	int iy0 = (int)ceilf(y0 - 0.5f), iy2 = (int)floorf(y2 - 0.5f);
	iy0 = HV_MAX(iy0, 0);
	iy2 = HV_MIN(iy2, mh - 1);
	if (iy0 > iy2 || y2 - y0 < 1e-6f) {
		return;
	}
	const float fx0 = HV_MIN(x0, HV_MIN(x1, x2)), fx1 = HV_MAX(x0, HV_MAX(x1, x2));
	if (fx1 < 0.0f || fx0 > (float)mw) {
		return;
	}
	/* The long edge (top to bottom) and the two short ones, as x at a row
	 * centre: x = a + b * py. */
	const float bl = (x2 - x0) / (y2 - y0), al = x0 - bl * y0;
	const float bt = y1 - y0 > 1e-6f ? (x1 - x0) / (y1 - y0) : 0.0f, at = x0 - bt * y0;
	const float bb = y2 - y1 > 1e-6f ? (x2 - x1) / (y2 - y1) : 0.0f, ab = x1 - bb * y1;
	uint8_t* const px = w->px;
	for (int y = iy0; y <= iy2; ++y) {
		const float py = (float)y + 0.5f;
		const float xl = al + bl * py;
		const float xs = py < y1 ? at + bt * py : ab + bb * py;
		const float lo = HV_MAX(HV_MIN(xl, xs), fx0), hi = HV_MIN(HV_MAX(xl, xs), fx1);
		int ix0 = (int)ceilf(lo - 0.5f), ix1 = (int)floorf(hi - 0.5f);
		ix0 = HV_MAX(ix0, 0);
		ix1 = HV_MIN(ix1, mw - 1);
		if (ix1 >= ix0) {
			memset(px + ((size_t)y * (size_t)mw + (size_t)ix0) * 4, 0xff, (size_t)(ix1 - ix0 + 1) * 4);
		}
	}
}

static int hv_boxes_meet(const float a[4], const float b[4], float margin) {
	return a[0] - margin < b[2] && a[2] + margin > b[0] && a[1] - margin < b[3] && a[3] + margin > b[1];
}

/* Eye `ei`'s view of the layers. */
static void hv_layer_boxes(const HvEye* e, int ei) {
	for (int i = 0; i < s_hv.layer_count; ++i) {
		const AeronXrPanel* q = &s_hv.layers[i];
		const float* r = s_hv.layer_rot[i];
		float* b = s_hv.layer_box[ei][i];
		b[0] = b[1] = 1e9f;
		b[2] = b[3] = -1e9f;
		int ok = 1;
		for (int k = 0; k < 4; ++k) {
			const float cx = ((k & 1) ? 0.5f : -0.5f) * q->size[0], cy = ((k & 2) ? 0.5f : -0.5f) * q->size[1];
			const float p[3] = { q->position[0] + r[0] * cx + r[1] * cy, q->position[1] + r[3] * cx + r[4] * cy,
								 q->position[2] + r[6] * cx + r[7] * cy };
			float x, y;
			if (!hv_project(e, p, &x, &y)) {
				ok = -1; /* a corner beside or behind the eye: it may cover anything */
				break;
			}
			b[0] = fminf(b[0], x);
			b[1] = fminf(b[1], y);
			b[2] = fmaxf(b[2], x);
			b[3] = fmaxf(b[3], y);
		}
		if (ok > 0 && (b[2] < 0.0f || b[3] < 0.0f || b[0] > (float)e->w || b[1] > (float)e->h)) {
			ok = 0;
		}
		s_hv.layer_box_ok[ei][i] = ok;
	}
}

/* The layers within reach of a source with box `b` that could hide part
 * of it, for hv_behind. */
static void hv_near_layers(HvWork* w, const HvEye* e, int ei, const float b[4]) {
	w->near_count = 0;
	for (int i = 0; i < s_hv.layer_count; ++i) {
		const AeronXrPanel* q = &s_hv.layers[i];
		const float c[3] = { q->position[0] - e->pos[0], q->position[1] - e->pos[1], q->position[2] - e->pos[2] };
		const int ok = s_hv.layer_box_ok[ei][i];
		if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < 1.5f * 1.5f &&
			(ok < 0 || (ok > 0 && hv_boxes_meet(b, s_hv.layer_box[ei][i], 0.0f)))) {
			const float* r = s_hv.layer_rot[i];
			HvNear* nl = &w->near[w->near_count++];
			for (int k = 0; k < 3; ++k) {
				nl->ax[k] = r[k * 3];
				nl->ay[k] = r[k * 3 + 1];
				nl->n[k] = r[k * 3 + 2];
				nl->c[k] = c[k];
			}
			nl->num = c[0] * nl->n[0] + c[1] * nl->n[1] + c[2] * nl->n[2];
			nl->hw = 0.5f * q->size[0];
			nl->hh = 0.5f * q->size[1];
		}
	}
}

/* Is the point at `d` from the eye behind one of the near layers (the line
 * from the eye to it goes through the quad)? The line meets the quad's
 * plane at t = num / (d . n), which must lie in (0, 1). */
static int hv_behind(const HvWork* w, const float d[3]) {
	for (int i = 0; i < w->near_count; ++i) {
		const HvNear* nl = &w->near[i];
		const float dn = d[0] * nl->n[0] + d[1] * nl->n[1] + d[2] * nl->n[2];
		if (dn > 0.0f ? !(nl->num > 0.0f && nl->num < dn) : !(nl->num < 0.0f && nl->num > dn)) {
			continue; /* the point is on the eye's side of the plane */
		}
		const float t = nl->num / dn;
		const float h[3] = { d[0] * t - nl->c[0], d[1] * t - nl->c[1], d[2] * t - nl->c[2] };
		const float lx = h[0] * nl->ax[0] + h[1] * nl->ax[1] + h[2] * nl->ax[2];
		const float ly = h[0] * nl->ay[0] + h[1] * nl->ay[1] + h[2] * nl->ay[2];
		if (fabsf(lx) <= nl->hw && fabsf(ly) <= nl->hh) {
			return 1;
		}
	}
	return 0;
}

/* The mask cell for a source whose box is b (eye pixels, clipped). */
static void hv_cell_begin(HvWork* w, HvCell* c, const float b[4]) {
	const float bw = b[2] - b[0], bh = b[3] - b[1];
	c->scale = fmaxf(2.0f, fmaxf(bw, bh) / (float)(HV_CELL - 5));
	c->mw = (int)ceilf(bw / c->scale) + 4;
	c->mh = (int)ceilf(bh / c->scale) + 4;
	c->mw = c->mw > HV_CELL ? HV_CELL : c->mw;
	c->mh = c->mh > HV_CELL ? HV_CELL : c->mh;
	c->ox = b[0] - 2.0f * c->scale;
	c->oy = b[1] - 2.0f * c->scale;
	w->mw = c->mw;
	w->mh = c->mh;
	memset(w->px, 0, (size_t)c->mw * (size_t)c->mh * 4);
}

static int hv_box_clip(float b[4], const HvEye* e) {
	b[0] = fmaxf(b[0], 0.0f);
	b[1] = fmaxf(b[1], 0.0f);
	b[2] = fminf(b[2], (float)e->w);
	b[3] = fminf(b[3], (float)e->h);
	return b[2] > b[0] && b[3] > b[1];
}

/* Copies source `s`'s mesh this frame (a hand, or the stick) into
 * s_hv.src[s]; 0 when it is not shown. */
static int hv_source_copy(int s) {
	HvSource* d = &s_hv.src[s];
	const float *v, *n;
	const uint16_t* idx;
	int vn, in;
	d->vn = d->in = 0;
	const int ok = s == HV_STICK ? XwaVrStick_Mesh(&v, &n, &vn, &idx, &in) : XwaVrGloves_Mesh(s, &v, &n, &vn, &idx, &in);
	if (!ok || vn <= 0 || in < 3 || vn > HV_MAX_VERTS || in > HV_MAX_INDICES) {
		return 0;
	}
	if (!d->v) {
		d->v = (float*)malloc(sizeof(float) * 3 * HV_MAX_VERTS);
		d->n = (float*)malloc(sizeof(float) * 3 * HV_MAX_VERTS);
		d->idx = (uint16_t*)malloc(sizeof(uint16_t) * HV_MAX_INDICES);
		if (!d->v || !d->n || !d->idx) {
			free(d->v);
			free(d->n);
			free(d->idx);
			d->v = d->n = NULL;
			d->idx = NULL;
			return 0;
		}
	}
	memcpy(d->v, v, sizeof(float) * 3 * (size_t)vn);
	memcpy(d->n, n, sizeof(float) * 3 * (size_t)vn);
	memcpy(d->idx, idx, sizeof(uint16_t) * (size_t)in);
	d->vn = vn;
	d->in = in;
	return 1;
}

/* Source `s` seen from eye `ei`: its vertices projected (kept for the
 * fill) and its box. Returns 0 when it is not shown or not in view. */
static int hv_source_project(const HvEye* e, int ei, int s, float b[4]) {
	const HvSource* src = &s_hv.src[s];
	if (src->vn <= 0) {
		return 0;
	}
	float* sx = s_hv.sx[ei][s];
	float* sy = s_hv.sy[ei][s];
	uint8_t* ok = s_hv.ok[ei][s];
	b[0] = b[1] = 1e9f;
	b[2] = b[3] = -1e9f;
	for (int i = 0; i < src->vn; ++i) {
		ok[i] = (uint8_t)hv_project(e, &src->v[i * 3], &sx[i], &sy[i]);
		if (ok[i]) {
			b[0] = fminf(b[0], sx[i]);
			b[1] = fminf(b[1], sy[i]);
			b[2] = fmaxf(b[2], sx[i]);
			b[3] = fmaxf(b[3], sy[i]);
		}
	}
	return hv_box_clip(b, e);
}

/* Source `s`'s mask for eye `ei`, from its projection. */
static int hv_source_fill(HvWork* w, const HvEye* e, int ei, int s, HvCell* c, const float b[4]) {
	const HvSource* src = &s_hv.src[s];
	const float* v = src->v;
	const float* n = src->n;
	const uint16_t* idx = src->idx;
	const int vn = src->vn, in = src->in;
	const float* sx = s_hv.sx[ei][s];
	const float* sy = s_hv.sy[ei][s];
	const uint8_t* ok = s_hv.ok[ei][s];
	hv_near_layers(w, e, ei, b);
	hv_cell_begin(w, c, b);
	const float inv = 1.0f / c->scale;
	for (int i = 0; i < vn; ++i) {
		const float* p = &v[i * 3];
		const float d[3] = { p[0] - e->pos[0], p[1] - e->pos[1], p[2] - e->pos[2] };
		w->facing[i] = n[i * 3] * d[0] + n[i * 3 + 1] * d[1] + n[i * 3 + 2] * d[2] < 0.0f;
		w->behind[i] = w->near_count > 0 && hv_behind(w, d);
		w->tx[i] = (sx[i] - c->ox) * inv;
		w->ty[i] = (sy[i] - c->oy) * inv;
	}
	int seen = 0;
	for (int t = 0; t + 2 < in; t += 3) {
		const int k0 = idx[t], k1 = idx[t + 1], k2 = idx[t + 2];
		if (k0 >= vn || k1 >= vn || k2 >= vn || !ok[k0] || !ok[k1] || !ok[k2] ||
			!(w->facing[k0] | w->facing[k1] | w->facing[k2]) || (w->behind[k0] & w->behind[k1] & w->behind[k2])) {
			continue;
		}
		hv_tri(w, w->tx[k0], w->ty[k0], w->tx[k1], w->ty[k1], w->tx[k2], w->ty[k2]);
		seen = 1;
	}
	return seen;
}

/* All of eye `e`'s masks (the main thread or the worker). */
static void hv_fill_eye(int e) {
	HvWork* w = &s_hv.work[e];
	for (int s = 0; s < HV_SOURCES; ++s) {
		s_hv.filled[e][s] = 0;
		if (s_hv.have[e][s]) {
			w->px = s_hv.cells[e][s];
			s_hv.filled[e][s] = hv_source_fill(w, &s_hv.eyes[e], e, s, &s_hv.cell[e][s], s_hv.boxes[e][s]);
		}
	}
}

/* Eye `e`'s masks uploaded and its alpha list built (main thread). */
static int hv_commit_eye(AeronCommandBuffer* cmd, int e) {
	const int* r = s_hv.rect[e];
	if (!s_hv.list[e] && !(s_hv.list[e] = AeronDrawList_Create(8))) {
		return 0;
	}
	AeronDrawList_Begin(s_hv.list[e], NULL, s_hv.eyes[e].w, s_hv.eyes[e].h, AERON_DRAWLIST2D_LOAD, NULL);
	/* In the overlay's part of the eye image nothing shows over the
	 * layers ... */
	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	AeronDrawList_AddFill(s_hv.list[e], (float)r[0], (float)r[1], (float)r[2], (float)r[3], zero,
						  AERON_BLIT2D_BLEND_ALPHA_SET, NULL);
	/* ... but the hands and the stick. */
	AeronTextureUploadDesc uploads[HV_SOURCES];
	uint32_t upload_count = 0;
	for (int s = 0; s < HV_SOURCES; ++s) {
		if (!s_hv.filled[e][s]) {
			continue;
		}
		const HvCell* c = &s_hv.cell[e][s];
		AeronTexture** tex = &s_hv.tex[e][s];
		if (!*tex) {
			*tex = Aeron_CreateTexture(&(AeronTextureDesc) {
				.width = HV_CELL,
				.height = HV_CELL,
				.mip_count = 1,
				.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
				.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
				.debug_name = "xwa.vr_hands.overlay_mask",
			});
			if (!*tex) {
				return 0;
			}
		}
		uploads[upload_count++] = (AeronTextureUploadDesc) {
			.texture = *tex,
			.width = c->mw,
			.height = c->mh,
			.pixels = s_hv.cells[e][s],
			.pitch = c->mw * 4,
			.pixel_format = AERON_PIXEL_FORMAT_RGBA8888,
			.cycle = 1,
		};
		AeronDrawList2DSprite sp;
		memset(&sp, 0, sizeof sp);
		sp.texture = *tex;
		sp.src_u0 = 1.0f / (float)HV_CELL;
		sp.src_v0 = 1.0f / (float)HV_CELL;
		sp.src_u1 = (float)(c->mw - 1) / (float)HV_CELL;
		sp.src_v1 = (float)(c->mh - 1) / (float)HV_CELL;
		sp.dst_x = c->ox + c->scale;
		sp.dst_y = c->oy + c->scale;
		sp.dst_w = (float)(c->mw - 2) * c->scale;
		sp.dst_h = (float)(c->mh - 2) * c->scale;
		sp.tint[0] = sp.tint[1] = sp.tint[2] = sp.tint[3] = 1.0f;
		sp.blend = AERON_BLIT2D_BLEND_ALPHA_OVER;
		sp.filter = AERON_BLIT2D_FILTER_LINEAR;
		AeronDrawList_AddSprite(s_hv.list[e], &sp);
	}
	/* One copy pass for the eye's masks. */
	if (upload_count && !Aeron_UploadTextureBatchCmd(cmd, uploads, upload_count)) {
		return 0;
	}
	s_hv.ready[e] = AeronDrawList_Prepare(s_hv.list[e], cmd);
	return s_hv.ready[e];
}

/* ---- The worker ------------------------------------------------------- */

static int SDLCALL hv_thread_main(void* arg) {
	(void)arg;
	for (;;) {
		SDL_WaitSemaphore(s_hv.go);
		if (s_hv.quit) {
			break;
		}
		hv_fill_eye(1);
		SDL_SignalSemaphore(s_hv.done);
	}
	return 0;
}

static int hv_thread_ready(void) {
	if (s_hv.thread_state == 0) {
		s_hv.thread_state = -1;
		const char* env = SDL_getenv("XWA_XR_HANDS_OVERLAY_THREAD");
		if (env && env[0] == '0') {
			return 0;
		}
		s_hv.go = SDL_CreateSemaphore(0);
		s_hv.done = SDL_CreateSemaphore(0);
		s_hv.quit = 0;
		if (s_hv.go && s_hv.done) {
			s_hv.thread = SDL_CreateThread(hv_thread_main, "xwa_vr_hand_mask", NULL);
		}
		if (s_hv.thread) {
			s_hv.thread_state = 1;
		} else {
			Aeron_LogWarn("xwa.remaster", "hand overlay: no worker thread, both eyes' masks on the main thread");
			if (s_hv.go) {
				SDL_DestroySemaphore(s_hv.go);
			}
			if (s_hv.done) {
				SDL_DestroySemaphore(s_hv.done);
			}
			s_hv.go = s_hv.done = NULL;
		}
	}
	return s_hv.thread_state > 0;
}

/* Waits for the worker's right eye, if it is filling one. */
static void hv_join(void) {
	if (s_hv.right_pending) {
		SDL_WaitSemaphore(s_hv.done);
		s_hv.right_pending = 0;
	}
}

/* ---- Frame ------------------------------------------------------------ */

static int hv_prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], int w, int h);

int XwaVrHandOverlay_Prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], int w, int h) {
	hv_join(); /* last frame's, should it not have been taken in */
	s_hv.right_todo = 0;
	const int shown = hv_prepare(cmd, views, w, h);
	s_hv.layers_set = 0; /* one Prepare (the menus' Prepare sets its own layer) */
	return shown;
}

static int hv_prepare(AeronCommandBuffer* cmd, const AeronXrView views[2], int w, int h) {
	s_hv.ready[0] = s_hv.ready[1] = 0;
	if (!hv_enabled() || !cmd || !views || w <= 0 || h <= 0) {
		return 0;
	}
	int unknown_layers = 0;
	if (!s_hv.layers_set) {
		/* The menus: the screen and its frame, facing the viewer. */
		float c[3], size[2];
		s_hv.layer_count = 0;
		if (XwaVrEnv_LayerRect(c, size)) {
			AeronXrPanel* q = &s_hv.layers[0];
			memset(q, 0, sizeof *q);
			memcpy(q->position, c, sizeof q->position);
			q->orientation[3] = 1.0f;
			q->size[0] = size[0];
			q->size[1] = size[1];
			s_hv.layer_count = 1;
		} else {
			unknown_layers = 1; /* keep the hands in front */
		}
	} else if (s_hv.layer_count == 0) {
		return 0;
	}
	for (int i = 0; i < s_hv.layer_count; ++i) {
		hv_quat_m(s_hv.layers[i].orientation, s_hv.layer_rot[i]);
	}
	int shown_src = 0;
	for (int s = 0; s < HV_SOURCES; ++s) {
		shown_src |= hv_source_copy(s);
	}
	if (!shown_src) {
		return 0;
	}
	int over = 0, any = 0;
	for (int e = 0; e < 2; ++e) {
		hv_eye_setup(&views[e], w, h, &s_hv.eyes[e]);
		for (int s = 0; s < HV_SOURCES; ++s) {
			s_hv.have[e][s] = hv_source_project(&s_hv.eyes[e], e, s, s_hv.boxes[e][s]);
			any |= s_hv.have[e][s];
		}
		hv_layer_boxes(&s_hv.eyes[e], e);
		for (int i = 0; i < s_hv.layer_count && !over; ++i) {
			const int ok = s_hv.layer_box_ok[e][i];
			for (int s = 0; s < HV_SOURCES && !over; ++s) {
				over = ok < 0 || (ok > 0 && s_hv.have[e][s] && hv_boxes_meet(s_hv.boxes[e][s], s_hv.layer_box[e][i], 8.0f));
			}
		}
		over |= unknown_layers;
	}
	if (!any || !over) {
		return 0;
	}
	/* The part of each eye image the overlay shows: the sources' boxes. */
	for (int e = 0; e < 2; ++e) {
		float u[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
		for (int s = 0; s < HV_SOURCES; ++s) {
			if (s_hv.have[e][s]) {
				u[0] = fminf(u[0], s_hv.boxes[e][s][0]);
				u[1] = fminf(u[1], s_hv.boxes[e][s][1]);
				u[2] = fmaxf(u[2], s_hv.boxes[e][s][2]);
				u[3] = fmaxf(u[3], s_hv.boxes[e][s][3]);
			}
		}
		int* r = s_hv.rect[e];
		if (u[2] > u[0] && u[3] > u[1]) {
			r[0] = (int)floorf(u[0]) - 4;
			r[1] = (int)floorf(u[1]) - 4;
			r[2] = (int)ceilf(u[2]) + 4;
			r[3] = (int)ceilf(u[3]) + 4;
			r[0] = r[0] < 0 ? 0 : r[0];
			r[1] = r[1] < 0 ? 0 : r[1];
			r[2] = (r[2] > w ? w : r[2]) - r[0];
			r[3] = (r[3] > h ? h : r[3]) - r[1];
		} else {
			r[0] = r[1] = 0;
			r[2] = r[3] = 2;
		}
		if (!hv_crop()) {
			r[0] = r[1] = 0;
			r[2] = w;
			r[3] = h;
		}
	}
	/* The right eye on the worker meanwhile, the left one here. */
	if (hv_thread_ready()) {
		s_hv.right_pending = 1;
		SDL_SignalSemaphore(s_hv.go);
	}
	hv_fill_eye(0);
	if (!hv_commit_eye(cmd, 0)) {
		return 0; /* (the next Prepare waits for the worker) */
	}
	s_hv.right_todo = 1;
	return 1;
}

void XwaVrHandOverlay_Finish(AeronCommandBuffer* cmd) {
	if (!s_hv.right_todo) {
		return;
	}
	s_hv.right_todo = 0;
	if (s_hv.right_pending) {
		hv_join();
	} else {
		hv_fill_eye(1);
	}
	if (cmd) {
		(void)hv_commit_eye(cmd, 1);
	}
}

void XwaVrHandOverlay_Rects(int rect[2][4]) { memcpy(rect, s_hv.rect, sizeof s_hv.rect); }

AeronDrawList2D* XwaVrHandOverlay_List(int eye) {
	return eye >= 0 && eye < 2 && s_hv.ready[eye] ? s_hv.list[eye] : NULL;
}

void XwaVrHandOverlay_Shutdown(void) {
	hv_join();
	if (s_hv.thread) {
		s_hv.quit = 1;
		SDL_SignalSemaphore(s_hv.go);
		SDL_WaitThread(s_hv.thread, NULL);
	}
	if (s_hv.go) {
		SDL_DestroySemaphore(s_hv.go);
	}
	if (s_hv.done) {
		SDL_DestroySemaphore(s_hv.done);
	}
	for (int e = 0; e < 2; ++e) {
		if (s_hv.list[e]) {
			AeronDrawList_Destroy(s_hv.list[e]);
		}
		for (int s = 0; s < HV_SOURCES; ++s) {
			if (s_hv.tex[e][s]) {
				Aeron_DestroyTexture(s_hv.tex[e][s]);
			}
		}
	}
	for (int s = 0; s < HV_SOURCES; ++s) {
		free(s_hv.src[s].v);
		free(s_hv.src[s].n);
		free(s_hv.src[s].idx);
	}
	memset(&s_hv, 0, sizeof s_hv);
}
