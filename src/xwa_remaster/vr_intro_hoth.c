#include "xwa_remaster/vr_intro_hoth.h"
#include "aeron/aeron.h"
#include "aeron/asset/opt_model.h"
#include "aeron/log.h"
#include "aeron/paths.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/mesh.h"
#include "aeron/vfs.h"
#include "xwa_2d.h"
#include "xwa_remaster/ship.h"
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the storyboard ---------------------------------------------------------
 *
 * Directions as seen by the seated viewer: azimuth degrees (0 = the screen,
 * + = right), elevation degrees (+ = up), distance in metres. The film's
 * timings (INTRO_FINAL.SNM, 15 fps) were measured frame by frame: its
 * camera turns 49 degrees left onto Hoth over 52-63 s, the transport shows
 * against the planet from 60.9 s and fills the picture as it passes the
 * camera at 65.7 s, the camera whips round after it to the Star Destroyers
 * and the film cuts to an X-wing's cockpit at 67.93 s. Later: the ion
 * cannon's bolt leaves Hoth at 75.5 s, strikes the Star Destroyer's
 * superstructure at 77.1 s and its left shield generator blows up at
 * 77.7 s; the transport crosses the Star Destroyer's hull at 98-101 s and
 * jumps to hyperspace at 109.3 s. The first line spoken (INTRO_FINAL.SUB,
 * frame 899) is at 59.93 s. */

#define HOTH_PLANET_GROUP 6030 /* the palest, icy world of PLANET.DAT: the film's Hoth */
#define HOTH_AZ (-70.0f)
#define HOTH_EL (-13.0f)
#define HOTH_RADIUS_DEG 45.0f
/* Hoth and the scene fade in over the 3 s before the first line spoken
 * (59.93 s); until then the crawl's starfield alone. */
#define HOTH_FADE_FROM 56.93f
#define HOTH_FADE_TO 59.93f
/* The craft are drawn from here (out at 1.4-1.9 km, unlit). */
#define HOTH_SHIPS_FROM 56.93f
/* The light comes up over these seconds: the craft lit as they near, the
 * Star Destroyers out of the dark. */
#define HOTH_LIGHT_FROM 62.5f
#define HOTH_LIGHT_TO 66.0f
#define HOTH_LIGHT_FLOOR 0.004f
/* The fights far off: they start as the light comes up. */
#define HOTH_FIGHT_FROM 64.0f
/* The ion cannon: the glow on Hoth, the bolt's flight, the strike. */
#define HOTH_ION_GLOW 75.5f
#define HOTH_ION_LAUNCH 75.9f
#define HOTH_ION_HIT 77.1f
#define HOTH_SHIELD_BURST 77.7f
/* The shield generator's dome is gone (the game's destroyed component:
 * hidden) once the burst covers it. */
#define HOTH_SHIELD_GONE 77.9f
/* Ion lightning wanders over the hull from the strike until the scene ends;
 * thicker just after the strike and as the transport crosses the hull. */
#define HOTH_ARCS_A_TO 79.5f
#define HOTH_ARCS_B_FROM 98.0f
#define HOTH_ARCS_B_TO 106.3f
/* The transport's jump. */
#define HOTH_JUMP 109.3f

typedef struct HothKey {
	float t, az, el, dist;
} HothKey;
/* A key in the lead Star Destroyer's model frame (metres: x starboard,
 * y aft, z up). */
typedef struct HothShipKey {
	float t, x, y, z;
} HothShipKey;

/* The transport: from in front of Hoth, past the viewer, towards the Star
 * Destroyers until the cut... */
static const HothKey k_transport[] = {
	{ 51.0f, -76.0f, 0.0f, 1450.0f }, { 60.9f, -75.0f, -1.0f, 450.0f }, { 62.9f, -74.0f, -1.2f, 265.0f },
	{ 64.7f, -66.0f, -2.0f, 105.0f }, { 65.2f, -55.0f, -3.0f, 60.0f },  { 65.7f, -15.0f, -9.0f, 18.0f },
	{ 66.3f, 48.0f, -2.0f, 45.0f },   { 66.9f, 47.0f, -0.5f, 95.0f },   { 67.93f, 45.0f, 0.5f, 220.0f },
	{ 73.0f, 46.0f, 8.0f, 650.0f },
};
/* ...then round the lead Star Destroyer's bow side, across its hull
 * (98-101 s) and away beyond it to the jump. */
static const HothShipKey k_transport_isd[] = {
	{ 80.0f, -900.0f, -500.0f, 650.0f }, { 88.0f, -1100.0f, 300.0f, 700.0f }, { 94.0f, -800.0f, -200.0f, 450.0f },
	{ 97.0f, -560.0f, -260.0f, 280.0f }, { 98.0f, -360.0f, -240.0f, 230.0f }, { 99.5f, 0.0f, -200.0f, 210.0f },
	{ 101.0f, 380.0f, -170.0f, 230.0f }, { 104.0f, 1100.0f, -60.0f, 420.0f }, { 109.3f, 2300.0f, 150.0f, 800.0f },
};
static const HothKey k_xwing1[] = {
	{ 51.0f, -73.0f, 4.0f, 1650.0f }, { 62.9f, -72.0f, 3.0f, 470.0f }, { 64.8f, -69.0f, 3.7f, 260.0f },
	{ 65.6f, -65.0f, 4.7f, 160.0f },  { 66.36f, -48.0f, 8.5f, 62.0f }, { 66.75f, 0.0f, 14.0f, 30.0f },
	{ 67.2f, 52.0f, 5.0f, 70.0f },    { 67.93f, 46.0f, 2.0f, 160.0f },
};
static const HothKey k_xwing2[] = {
	{ 51.0f, -80.0f, 6.0f, 1900.0f }, { 62.9f, -79.0f, 5.0f, 520.0f }, { 65.0f, -72.0f, 8.0f, 245.0f },
	{ 65.7f, -63.0f, 12.2f, 141.0f }, { 66.4f, -15.0f, 25.0f, 60.0f }, { 66.95f, 34.0f, 15.0f, 95.0f },
	{ 67.93f, 38.0f, 8.0f, 220.0f },
};

typedef struct HothCapital {
	float az, el, dist;
} HothCapital;
/* Star Destroyer 1 (the lead, higher and nearer) and 2. Their bows point
 * 139 degrees left of the line to them: long in view, three-quarter. */
static const HothCapital k_isd[2] = { { 55.0f, 7.0f, 1600.0f }, { 33.0f, 2.0f, 2600.0f } };
#define HOTH_ISD_BOW_TURN (-139.0f)

/* The fights: craft wheeling round a centre, X-wings against TIEs, close
 * enough to make out (the nearest pass ~150 m off), well clear of the Star
 * Destroyers; `size` scales the wheels (1: 170-310 m round). */
typedef struct HothFight {
	float az, el, dist, size;
} HothFight;
static const HothFight k_fight[2] = { { 40.0f, 24.0f, 450.0f, 0.55f }, { -38.0f, 15.0f, 550.0f, 0.6f } };
enum { HOTH_SIDE_REBEL, HOTH_SIDE_EMPIRE };
typedef struct HothFighter {
	uint8_t fight, side;
	int8_t keyed;   /* -1: wheels from the start; 1, 2: the X-wing that flew past, joining after the cut */
	float dies;     /* film time it explodes; 0 = never */
} HothFighter;
static const HothFighter k_fighters[] = {
	{ 0, HOTH_SIDE_REBEL, 1, 0.0f },    { 0, HOTH_SIDE_REBEL, 2, 0.0f },     { 0, HOTH_SIDE_EMPIRE, -1, 0.0f },
	{ 0, HOTH_SIDE_EMPIRE, -1, 83.0f }, { 0, HOTH_SIDE_EMPIRE, -1, 0.0f },   { 0, HOTH_SIDE_EMPIRE, -1, 90.5f },
	{ 0, HOTH_SIDE_EMPIRE, -1, 0.0f },  { 1, HOTH_SIDE_REBEL, -1, 0.0f },    { 1, HOTH_SIDE_REBEL, -1, 0.0f },
	{ 1, HOTH_SIDE_EMPIRE, -1, 95.0f }, { 1, HOTH_SIDE_EMPIRE, -1, 0.0f },   { 1, HOTH_SIDE_EMPIRE, -1, 0.0f },
};
#define HOTH_FIGHTERS ((int)(sizeof k_fighters / sizeof k_fighters[0]))

enum { HOTH_TRANSPORT, HOTH_XWING, HOTH_ISD, HOTH_TIE, HOTH_MESHES };
static const char* const k_opt[HOTH_MESHES] = { "FamilyTransport", "XWing", "ImperialStarDestroyer2", "TieFighter" };

#define HOTH_MAX_GLOWS 8
#define HOTH_BOOM_FRAMES 12
#define HOTH_PATH_MAX 24

typedef struct HothMesh {
	AeronSceneMesh* mesh;
	int glow_count;
	float glow[HOTH_MAX_GLOWS][7]; /* model metres x y z, radius, core rgb */
	float len_m;
	int shield_count;
	float shield[2][3]; /* the shield generators (ShieldGen parts), model metres */
	int shield_slot[2]; /* their mesh slots */
	float bridge[3];    /* the bridge (Bridge part) */
} HothMesh;

typedef struct HothPathKey {
	float t;
	float p[3];
} HothPathKey;

typedef struct HothPath {
	int n;
	HothPathKey k[HOTH_PATH_MAX];
} HothPath;

/* The lead Star Destroyer's upper surfaces, seen from above: the highest
 * upward-facing surface in each cell (model metres), for the ion lightning
 * to crawl over; NAN where there is no hull. */
#define HOTH_DECK_NX 32
#define HOTH_DECK_NY 64
typedef struct HothDeck {
	int ok;
	float x0, y0, cx, cy; /* the grid's corner and cell size */
	float z[HOTH_DECK_NY][HOTH_DECK_NX];
} HothDeck;

typedef struct HothFrame {
	float pos[3], n[3], u[3], r[3]; /* LOCAL: position, nose, up, starboard */
} HothFrame;

static struct {
	int step; /* the next load step */
	HothMesh mesh[HOTH_MESHES];
	AeronTexture* planet;
	float planet_disc; /* the disc's radius over half the image's width */
	AeronTexture* glow;
	AeronTexture* beam;
	AeronTexture* logo;
	float logo_aspect; /* height / width */
	AeronTexture* boom[HOTH_BOOM_FRAMES];
	int paths_built;
	HothPath transport, xwing[2];
	HothFrame isd[2];      /* the Star Destroyers now (the lead drifting, hoth_isd_drift) */
	HothFrame isd_base[2]; /* as placed */
	HothDeck deck;
	/* The last Submit's view. */
	int submitted;
	float org[3], right[3], up[3], fwd[3], k;
} s_hoth;

/* ---- small maths ------------------------------------------------------------ */

static float hoth_clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static float hoth_smooth(float x) {
	x = hoth_clamp01(x);
	return x * x * (3.0f - 2.0f * x);
}
static void hoth_dir(float az, float el, float d, float out[3]) {
	const float a = az * 0.017453293f, e = el * 0.017453293f;
	out[0] = d * cosf(e) * sinf(a);
	out[1] = d * sinf(e);
	out[2] = -d * cosf(e) * cosf(a);
}
static float hoth_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float hoth_len(const float a[3]) { return sqrtf(hoth_dot(a, a)); }
static void hoth_cross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}
static int hoth_norm(float v[3]) {
	const float l = hoth_len(v);
	if (l < 1e-9f) {
		return 0;
	}
	v[0] /= l;
	v[1] /= l;
	v[2] /= l;
	return 1;
}
/* A deterministic 0..1 from an integer. */
static float hoth_hash(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return (float)(x & 0xffffffU) / 16777215.0f;
}

/* LOCAL direction -> world direction (no scale). */
static void hoth_wdir(const float v[3], float out[3]) {
	for (int i = 0; i < 3; ++i) {
		out[i] = v[0] * s_hoth.right[i] + v[1] * s_hoth.up[i] - v[2] * s_hoth.fwd[i];
	}
}
/* LOCAL metres -> world. */
static void hoth_wpos(const float p[3], float out[3]) {
	float d[3];
	hoth_wdir(p, d);
	for (int i = 0; i < 3; ++i) {
		out[i] = s_hoth.org[i] + s_hoth.k * d[i];
	}
}

/* A Star Destroyer's frame: bows turned from the line of sight, rolled
 * ~25 degrees towards the viewer so its decks and bridge tower show. */
static void hoth_isd_frame(int i, HothFrame* f) {
	const HothCapital* c = &k_isd[i];
	hoth_dir(c->az, c->el, c->dist, f->pos);
	hoth_dir(c->az + HOTH_ISD_BOW_TURN, 0.0f, 1.0f, f->n);
	float tilt[3] = { -f->pos[0], -f->pos[1], -f->pos[2] };
	hoth_norm(tilt);
	float u[3] = { 0.47f * tilt[0], 1.0f + 0.47f * tilt[1], 0.47f * tilt[2] };
	hoth_cross(f->n, u, f->r);
	hoth_norm(f->r);
	hoth_cross(f->r, f->n, f->u);
	hoth_norm(f->u);
}

/* The lead Star Destroyer disabled by the ion cannon: from the strike it
 * sinks, gathering to DRIFT_SPEED over DRIFT_EASE seconds (~120 m down,
 * ~4 degrees, by the scene's end), and slowly lists to port. */
#define HOTH_DRIFT_SPEED 3.0f
#define HOTH_DRIFT_EASE 6.0f
#define HOTH_DRIFT_LIST_DEG 3.0f
/* How far it has sunk at `t` (metres): the speed easing up linearly over
 * EASE, then steady. */
static float hoth_drift_drop(float t) {
	const float s = t - 77.1f; /* HOTH_ION_HIT */
	if (s <= 0.0f) {
		return 0.0f;
	}
	return s < HOTH_DRIFT_EASE ? 0.5f * HOTH_DRIFT_SPEED * s * s / HOTH_DRIFT_EASE
							   : HOTH_DRIFT_SPEED * (s - 0.5f * HOTH_DRIFT_EASE);
}

static void hoth_isd_drift(float t) {
	s_hoth.isd[0] = s_hoth.isd_base[0];
	const float s = t - 77.1f; /* HOTH_ION_HIT */
	if (s <= 0.0f) {
		return;
	}
	HothFrame* f = &s_hoth.isd[0];
	f->pos[1] -= hoth_drift_drop(t);
	const float a = HOTH_DRIFT_LIST_DEG * 0.017453293f * hoth_smooth(s / 40.0f);
	const float c = cosf(a), sn = sinf(a);
	for (int i = 0; i < 3; ++i) {
		const float u = f->u[i], r = f->r[i];
		f->u[i] = c * u + sn * r;
		f->r[i] = c * r - sn * u;
	}
}

/* Model metres (x starboard, y aft, z up) of frame `f` -> LOCAL. */
static void hoth_model_to_local(const HothFrame* f, const float m[3], float out[3]) {
	for (int a = 0; a < 3; ++a) {
		out[a] = f->pos[a] + m[0] * f->r[a] - m[1] * f->n[a] + m[2] * f->u[a];
	}
}

/* LOCAL -> model metres of frame `f`. */
static void hoth_local_to_model(const HothFrame* f, const float p[3], float out[3]) {
	const float d[3] = { p[0] - f->pos[0], p[1] - f->pos[1], p[2] - f->pos[2] };
	out[0] = hoth_dot(d, f->r);
	out[1] = -hoth_dot(d, f->n);
	out[2] = hoth_dot(d, f->u);
}

/* ---- the paths ---------------------------------------------------------------- */

static void hoth_path_add(HothPath* p, float t, const float pos[3]) {
	if (p->n < HOTH_PATH_MAX) {
		p->k[p->n].t = t;
		memcpy(p->k[p->n].p, pos, sizeof p->k[p->n].p);
		p->n++;
	}
}

static void hoth_path_add_keys(HothPath* p, const HothKey* keys, int n) {
	for (int i = 0; i < n; ++i) {
		float pos[3];
		hoth_dir(keys[i].az, keys[i].el, keys[i].dist, pos);
		hoth_path_add(p, keys[i].t, pos);
	}
}

static void hoth_build_paths(void) {
	for (int i = 0; i < 2; ++i) {
		hoth_isd_frame(i, &s_hoth.isd_base[i]);
		s_hoth.isd[i] = s_hoth.isd_base[i];
	}
	memset(&s_hoth.transport, 0, sizeof s_hoth.transport);
	hoth_path_add_keys(&s_hoth.transport, k_transport, (int)(sizeof k_transport / sizeof k_transport[0]));
	for (size_t i = 0; i < sizeof k_transport_isd / sizeof k_transport_isd[0]; ++i) {
		const float m[3] = { k_transport_isd[i].x, k_transport_isd[i].y, k_transport_isd[i].z };
		float pos[3];
		hoth_model_to_local(&s_hoth.isd[0], m, pos);
		hoth_path_add(&s_hoth.transport, k_transport_isd[i].t, pos);
	}
	memset(s_hoth.xwing, 0, sizeof s_hoth.xwing);
	hoth_path_add_keys(&s_hoth.xwing[0], k_xwing1, (int)(sizeof k_xwing1 / sizeof k_xwing1[0]));
	hoth_path_add_keys(&s_hoth.xwing[1], k_xwing2, (int)(sizeof k_xwing2 / sizeof k_xwing2[0]));
	s_hoth.paths_built = 1;
}

/* Position on a keyed path at `t` (LOCAL metres): a cubic Hermite through
 * the keys with tangents from the neighbours; past the ends it carries on
 * along the end segment's direction. */
static void hoth_path(const HothPath* path, float t, float out[3]) {
	const int n = path->n;
	const HothPathKey* k = path->k;
	if (n < 2) {
		memcpy(out, k[0].p, 3 * sizeof(float));
		return;
	}
	if (t <= k[0].t || t >= k[n - 1].t) {
		const int a = t <= k[0].t ? 0 : n - 2;
		const float dt = k[a + 1].t - k[a].t;
		const float ref = t <= k[0].t ? k[0].t : k[n - 1].t;
		const float* p = t <= k[0].t ? k[0].p : k[n - 1].p;
		for (int c = 0; c < 3; ++c) {
			out[c] = p[c] + (k[a + 1].p[c] - k[a].p[c]) / dt * (t - ref);
		}
		return;
	}
	int i = 0;
	while (i < n - 2 && t > k[i + 1].t) {
		++i;
	}
	const int i0 = i > 0 ? i - 1 : i, i3 = i + 2 < n ? i + 2 : i + 1;
	const float t0 = k[i].t, t1 = k[i + 1].t, h = t1 - t0;
	const float s = (t - t0) / h;
	const float h00 = 2.0f * s * s * s - 3.0f * s * s + 1.0f, h10 = s * s * s - 2.0f * s * s + s;
	const float h01 = -2.0f * s * s * s + 3.0f * s * s, h11 = s * s * s - s * s;
	for (int c = 0; c < 3; ++c) {
		const float m0 = (k[i + 1].p[c] - k[i0].p[c]) / (k[i + 1].t - k[i0].t);
		const float m1 = (k[i3].p[c] - k[i].p[c]) / (k[i3].t - k[i].t);
		out[c] = h00 * k[i].p[c] + h10 * h * m0 + h01 * k[i + 1].p[c] + h11 * h * m1;
	}
}

/* Where fighter `f` wheels in its fight at `t`: a tilted circle round the
 * fight's centre with a slow weave, its own size, speed and phase. */
static void hoth_wheel(int f, float t, float out[3]) {
	const HothFight* fight = &k_fight[k_fighters[f].fight];
	float c[3];
	hoth_dir(fight->az, fight->el, fight->dist, c);
	const uint32_t s = (uint32_t)f * 97u + 13u;
	const float radius = (170.0f + 140.0f * hoth_hash(s)) * fight->size;
	const float w = (0.42f + 0.28f * hoth_hash(s + 1u)) * (hoth_hash(s + 2u) < 0.5f ? -1.0f : 1.0f);
	const float phase = 6.2831853f * hoth_hash(s + 3u);
	float e1[3] = { hoth_hash(s + 4u) - 0.5f, (hoth_hash(s + 5u) - 0.5f) * 0.6f, hoth_hash(s + 6u) - 0.5f };
	float e3[3] = { hoth_hash(s + 7u) - 0.5f, 1.0f, hoth_hash(s + 8u) - 0.5f };
	if (!hoth_norm(e1)) {
		e1[0] = 1.0f;
	}
	float e2[3];
	hoth_cross(e3, e1, e2);
	hoth_norm(e2);
	hoth_cross(e1, e2, e3);
	const float a = w * t + phase;
	const float weave = 70.0f * fight->size * sinf(0.63f * w * t + 2.0f * phase);
	/* The centre drifts a little too. */
	const float drift = 90.0f * fight->size * sinf(0.11f * t + phase);
	for (int i = 0; i < 3; ++i) {
		out[i] = c[i] + radius * (cosf(a) * e1[i] + sinf(a) * e2[i]) + weave * e3[i] + drift * e1[i];
	}
}

/* Where fighter `f` is at `t`: the X-wings that flew past keep to their
 * keyed run, then ease into their fight after the cut. */
static void hoth_fighter_pos(int f, float t, float out[3]) {
	const int keyed = k_fighters[f].keyed;
	if (keyed <= 0) {
		hoth_wheel(f, t, out);
		return;
	}
	float run[3], wheel[3];
	hoth_path(&s_hoth.xwing[keyed - 1], t, run);
	const float b = hoth_smooth((t - XWA_VR_HOTH_CUT) / 7.0f);
	if (b <= 0.0f) {
		memcpy(out, run, sizeof run);
		return;
	}
	hoth_wheel(f, t, wheel);
	for (int i = 0; i < 3; ++i) {
		out[i] = run[i] + (wheel[i] - run[i]) * b;
	}
}

/* A craft's pose from its positions round `t`: nose along the flight and
 * up banked into the turn, at most 70 degrees. */
static void hoth_pose_from(const float a[3], const float pos[3], const float b[3], float h, float nose[3],
						   float upv[3]) {
	float acc[3];
	for (int i = 0; i < 3; ++i) {
		nose[i] = b[i] - a[i];
		acc[i] = (b[i] - 2.0f * pos[i] + a[i]) / (h * h);
	}
	if (!hoth_norm(nose)) {
		nose[0] = 0.0f;
		nose[1] = 0.0f;
		nose[2] = -1.0f;
	}
	const float along = hoth_dot(acc, nose);
	float lat[3];
	for (int i = 0; i < 3; ++i) {
		lat[i] = acc[i] - along * nose[i];
	}
	const float g = 30.0f;
	float u[3] = { lat[0], g + lat[1], lat[2] };
	const float ln = hoth_len(lat);
	if (ln > g * 2.75f) { /* tan(70 deg) */
		const float sc = g * 2.75f / ln;
		u[0] = lat[0] * sc;
		u[1] = g + lat[1] * sc;
		u[2] = lat[2] * sc;
	}
	const float un = hoth_dot(u, nose);
	for (int i = 0; i < 3; ++i) {
		upv[i] = u[i] - un * nose[i];
	}
	if (!hoth_norm(upv)) {
		upv[0] = 0.0f;
		upv[1] = 1.0f;
		upv[2] = 0.0f;
	}
}

/* A craft's position at `t`: the transport (who -1) or fighter `who`. */
static void hoth_craft_pos(int who, float t, float out[3]) {
	if (who < 0) {
		/* Its run round and across the lead Star Destroyer (keyed in its
		 * frame) goes down with it. */
		hoth_path(&s_hoth.transport, t, out);
		out[1] -= hoth_drift_drop(t) * hoth_smooth((t - 73.0f) / 7.0f);
	} else {
		hoth_fighter_pos(who, t, out);
	}
}

/* The craft's pose straight from its flight (hoth_pose_from). */
static void hoth_craft_pose_raw(int who, float t, float pos[3], float nose[3], float upv[3]) {
	const float h = 0.05f;
	float a[3], b[3];
	hoth_craft_pos(who, t, pos);
	hoth_craft_pos(who, t - h, a);
	hoth_craft_pos(who, t + h, b);
	hoth_pose_from(a, pos, b, h, nose, upv);
}

/* The unbanked up for `nose` (LOCAL up, square to it) and its side. */
static int hoth_unbanked(const float nose[3], float u0[3], float side[3]) {
	const float y = nose[1];
	u0[0] = -y * nose[0];
	u0[1] = 1.0f - y * nose[1];
	u0[2] = -y * nose[2];
	if (!hoth_norm(u0)) {
		return 0;
	}
	hoth_cross(nose, u0, side);
	return 1;
}

/* The bank (radians) of up `upv` about `nose`; 0 when straight up/down. */
static float hoth_bank_of(const float nose[3], const float upv[3]) {
	float u0[3], side[3];
	if (!hoth_unbanked(nose, u0, side)) {
		return 0.0f;
	}
	return atan2f(hoth_dot(upv, side), hoth_dot(upv, u0));
}

/* The roll at most this fast (radians a second): as the craft whip past and
 * into the curve towards the Star Destroyers their bank follows the turn at
 * about a quarter of the ~650 deg/s it would; the turn's bank is first
 * averaged over +-ROLL_AVG steps so brief wiggles of the path don't roll
 * them at all. */
#define HOTH_ROLL_RATE (160.0f * 0.017453293f)
#define HOTH_ROLL_STEPS 150
#define HOTH_ROLL_AVG 15
#define HOTH_ROLL_DT 0.02f

/* The craft's pose with its bank smoothed and rate-limited: followed from
 * 3 s back (long enough to settle) to `t`, a function of `t` alone. */
static void hoth_craft_pose(int who, float t, float pos[3], float nose[3], float upv[3]) {
	hoth_craft_pose_raw(who, t, pos, nose, upv);
	float u0[3], side[3];
	if (!hoth_unbanked(nose, u0, side)) {
		return;
	}
	enum { N = HOTH_ROLL_STEPS + 1 + 2 * HOTH_ROLL_AVG };
	float raw[N];
	const float t0 = t - (float)(HOTH_ROLL_STEPS + HOTH_ROLL_AVG) * HOTH_ROLL_DT;
	for (int i = 0; i < N; ++i) {
		float p[3], n[3], u[3];
		hoth_craft_pose_raw(who, t0 + (float)i * HOTH_ROLL_DT, p, n, u);
		raw[i] = hoth_bank_of(n, u);
	}
	/* raw[i .. i + 2 AVG] averaged: the target at step i. */
	float sum = 0.0f;
	for (int i = 0; i <= 2 * HOTH_ROLL_AVG; ++i) {
		sum += raw[i];
	}
	const float inv = 1.0f / (float)(2 * HOTH_ROLL_AVG + 1);
	float bank = sum * inv;
	const float step = HOTH_ROLL_RATE * HOTH_ROLL_DT;
	for (int i = 1; i <= HOTH_ROLL_STEPS; ++i) {
		sum += raw[i + 2 * HOTH_ROLL_AVG] - raw[i - 1];
		const float d = sum * inv - bank;
		bank += d > step ? step : (d < -step ? -step : d);
	}
	const float c = cosf(bank), sn = sinf(bank);
	for (int i = 0; i < 3; ++i) {
		upv[i] = c * u0[i] + sn * side[i];
	}
}

static void hoth_transport_pose(float t, float pos[3], float nose[3], float upv[3]) {
	hoth_craft_pose(-1, t, pos, nose, upv);
}

static void hoth_fighter_pose(int f, float t, float pos[3], float nose[3], float upv[3]) {
	if (k_fighters[f].keyed > 0) {
		hoth_craft_pose(f, t, pos, nose, upv);
	} else {
		hoth_craft_pose_raw(f, t, pos, nose, upv);
	}
}

/* ---- loading ---------------------------------------------------------------- */

static float hoth_srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static float hoth_linear_to_srgb(float c) {
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static AeronTexture* hoth_texture(AeronCommandBuffer* cmd, const uint8_t* px, int w, int h, int srgb,
								  const char* name) {
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = w,
		.height = h,
		.mip_count = 1,
		.format = srgb ? AERON_TEXTURE_FORMAT_RGBA8_SRGB : AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = name,
	});
	if (!t || !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
			.texture = t,
			.width = w,
			.height = h,
			.raw_data = px,
			.raw_size = (size_t)w * (size_t)h * 4u,
		})) {
		if (t) {
			Aeron_DestroyTexture(t);
		}
		return NULL;
	}
	return t;
}

/* Straight sRGB RGBA -> premultiplied sRGB texture; `disc` (optional) gets
 * the opaque disc's radius over half the width. */
static AeronTexture* hoth_texture_pma(AeronCommandBuffer* cmd, const uint8_t* rgba, int w, int h, float* disc,
									  const char* name) {
	uint8_t* px = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);
	if (!px) {
		return NULL;
	}
	float r2max = 0.0f;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const uint8_t* s = &rgba[((size_t)y * w + x) * 4];
			uint8_t* d = &px[((size_t)y * w + x) * 4];
			const float a = (float)s[3] / 255.0f;
			for (int c = 0; c < 3; ++c) {
				d[c] = (uint8_t)(hoth_linear_to_srgb(hoth_srgb_to_linear((float)s[c] / 255.0f) * a) * 255.0f + 0.5f);
			}
			d[3] = s[3];
			if (disc && s[3] > 128) {
				const float dx = ((float)x + 0.5f) / (float)w - 0.5f, dy = ((float)y + 0.5f) / (float)h - 0.5f;
				r2max = fmaxf(r2max, dx * dx + dy * dy);
			}
		}
	}
	if (disc) {
		*disc = r2max > 0.01f ? 2.0f * sqrtf(r2max) : 0.95f;
	}
	AeronTexture* t = hoth_texture(cmd, px, w, h, 1, name);
	free(px);
	return t;
}

/* One DAT group of Resdata (Planet, Explosions). */
static int hoth_read_group(const char* dat, int group, Xwa2dFrameSet* set) {
	char paths[2][64];
	snprintf(paths[0], sizeof paths[0], "Resdata/%s.dat", dat);
	snprintf(paths[1], sizeof paths[1], "RESDATA/%s.DAT", dat);
	for (char* c = paths[1]; *c; ++c) {
		*c = (char)SDL_toupper((unsigned char)*c);
	}
	AeronVfs* vfs = Aeron_GetVfs();
	uint8_t* bytes = NULL;
	size_t size = 0;
	for (int i = 0; i < 2 && !bytes; ++i) {
		if (vfs && AeronVfs_Exists(vfs, AERON_VFS_ROOT_ASSET, paths[i]) &&
			!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, paths[i], 64u << 20, &bytes, &size)) {
			bytes = NULL;
		}
	}
	if (!bytes) {
		Aeron_LogWarn("xwa.remaster", "VR intro Hoth: no %s", paths[0]);
		return 0;
	}
	memset(set, 0, sizeof *set);
	char error[256];
	error[0] = 0;
	const int ok = Xwa2d_DatAppendGroup(bytes, size, (uint16_t)group, set, error, sizeof error) && set->count > 0;
	free(bytes);
	if (!ok) {
		Aeron_LogWarn("xwa.remaster", "VR intro Hoth: DAT group %d unavailable: %s", group, error);
		Xwa2dFrameSet_Free(set);
	}
	return ok;
}

/* The planet: the group's first sprite (its fully lit face). */
static void hoth_load_planet(AeronCommandBuffer* cmd) {
	Xwa2dFrameSet set;
	if (!hoth_read_group("Planet", HOTH_PLANET_GROUP, &set)) {
		return;
	}
	const Xwa2dFrame* pick = NULL;
	for (int i = 0; i < set.count; ++i) {
		if (set.frames[i].rgba && (!pick || set.frames[i].sprite_id < pick->sprite_id)) {
			pick = &set.frames[i];
		}
	}
	if (pick) {
		s_hoth.planet = hoth_texture_pma(cmd, pick->rgba, pick->width, pick->height, &s_hoth.planet_disc,
										 "xwa.vr_intro_hoth.planet");
	}
	Xwa2dFrameSet_Free(&set);
}

/* The fighter explosion (Explosions.dat group 2005, frames 1-12). */
static void hoth_load_boom(AeronCommandBuffer* cmd) {
	Xwa2dFrameSet set;
	if (!hoth_read_group("Explosions", 2005, &set)) {
		return;
	}
	for (int f = 0; f < HOTH_BOOM_FRAMES; ++f) {
		for (int i = 0; i < set.count; ++i) {
			if (set.frames[i].sprite_id == f + 1 && set.frames[i].rgba) {
				s_hoth.boom[f] = hoth_texture_pma(cmd, set.frames[i].rgba, set.frames[i].width, set.frames[i].height,
												  NULL, "xwa.vr_intro_hoth.boom");
				break;
			}
		}
	}
	Xwa2dFrameSet_Free(&set);
}

/* The X-Wing Alliance logo the menus show (vr/logo.rgba: "XWAL", u32 w, h,
 * straight RGBA; an APK asset, never in the repo). */
static void hoth_load_logo(AeronCommandBuffer* cmd) {
	static char beside_exe[1024];
	const char* const paths[3] = { "vr/logo.rgba", "assets/vr/logo.rgba", beside_exe };
	const char* override = SDL_getenv("XWA_XR_LOGO");
	beside_exe[0] = '\0';
#if !defined(SDL_PLATFORM_ANDROID)
	if (!Aeron_ApplicationPath("vr/logo.rgba", beside_exe, sizeof beside_exe)) {
		beside_exe[0] = '\0';
	}
#endif
	for (int i = -1; i < 3 && !s_hoth.logo; ++i) {
		const char* path = i < 0 ? (override && override[0] && strcmp(override, "0") ? override : NULL) : paths[i];
		if (path && !path[0]) {
			continue;
		}
		if (!path) {
			continue;
		}
		size_t size = 0;
		uint8_t* data = (uint8_t*)SDL_LoadFile(path, &size);
		if (!data) {
			continue;
		}
		if (size >= 12 && memcmp(data, "XWAL", 4) == 0) {
			uint32_t w, h;
			memcpy(&w, data + 4, 4);
			memcpy(&h, data + 8, 4);
			if (w && h && w <= 4096 && h <= 4096 && size >= 12 + (size_t)w * h * 4u) {
				s_hoth.logo = hoth_texture_pma(cmd, data + 12, (int)w, (int)h, NULL, "xwa.vr_intro_hoth.logo");
				s_hoth.logo_aspect = (float)h / (float)w;
			}
		}
		SDL_free(data);
	}
	if (!s_hoth.logo) {
		Aeron_LogInfo("xwa.remaster", "VR intro Hoth: no vr/logo.rgba (no logo card)");
	}
}

/* A soft round glow (engines, flashes) and a bolt: a bright core along u,
 * soft across v, tapered at both ends. */
static void hoth_make_sprites(AeronCommandBuffer* cmd) {
	enum { G = 64, BW = 64, BH = 16 };
	static uint8_t glow[G * G * 4], beam[BW * BH * 4];
	for (int y = 0; y < G; ++y) {
		for (int x = 0; x < G; ++x) {
			const float dx = ((float)x + 0.5f) / (float)G * 2.0f - 1.0f, dy = ((float)y + 0.5f) / (float)G * 2.0f - 1.0f;
			const float r2 = dx * dx + dy * dy;
			const float a = r2 >= 1.0f ? 0.0f : fminf(1.0f, expf(-r2 * 9.0f) + 0.35f * expf(-r2 * 2.5f)) * (1.0f - r2);
			uint8_t* p = &glow[(y * G + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(a * 255.0f + 0.5f);
			p[3] = 255;
		}
	}
	for (int y = 0; y < BH; ++y) {
		for (int x = 0; x < BW; ++x) {
			const float u = ((float)x + 0.5f) / (float)BW, v = ((float)y + 0.5f) / (float)BH * 2.0f - 1.0f;
			const float across = expf(-v * v * 5.0f) + 0.6f * expf(-v * v * 40.0f);
			const float ends = hoth_smooth(u / 0.15f) * hoth_smooth((1.0f - u) / 0.15f);
			const float a = fminf(1.0f, across * ends);
			uint8_t* p = &beam[(y * BW + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(a * 255.0f + 0.5f);
			p[3] = 255;
		}
	}
	s_hoth.glow = hoth_texture(cmd, glow, G, G, 0, "xwa.vr_intro_hoth.glow");
	s_hoth.beam = hoth_texture(cmd, beam, BW, BH, 0, "xwa.vr_intro_hoth.beam");
}

/* The deck map (HothDeck) from the model's triangles: each upward-facing
 * one sampled finely enough to touch every cell under it. */
static void hoth_build_deck(const AeronFlightModel* model) {
	HothDeck* d = &s_hoth.deck;
	const AeronGltfModel* r = &model->render;
	d->ok = 0;
	if (!r->vertices || !r->indices || r->index_count < 3) {
		return;
	}
	d->x0 = model->bounds.min.x;
	d->y0 = model->bounds.min.y;
	d->cx = (model->bounds.max.x - model->bounds.min.x) / (float)HOTH_DECK_NX;
	d->cy = (model->bounds.max.y - model->bounds.min.y) / (float)HOTH_DECK_NY;
	if (d->cx <= 0.0f || d->cy <= 0.0f) {
		return;
	}
	for (int j = 0; j < HOTH_DECK_NY; ++j) {
		for (int i = 0; i < HOTH_DECK_NX; ++i) {
			d->z[j][i] = NAN;
		}
	}
	const float step = 0.4f * fminf(d->cx, d->cy);
	for (uint32_t t = 0; t + 2 < r->index_count; t += 3) {
		const int slot = (int)r->vertices[r->indices[t]].mesh_index;
		const HothMesh* hm = &s_hoth.mesh[HOTH_ISD];
		if ((hm->shield_count > 0 && slot == hm->shield_slot[0]) ||
			(hm->shield_count > 1 && slot == hm->shield_slot[1])) {
			continue; /* not the shield generators' domes (one is blown away) */
		}
		const float* a = r->vertices[r->indices[t]].pos;
		const float* b = r->vertices[r->indices[t + 1]].pos;
		const float* c = r->vertices[r->indices[t + 2]].pos;
		const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		float n[3];
		hoth_cross(e1, e2, n);
		const float nl = hoth_len(n);
		if (nl <= 0.0f || fabsf(n[2]) < 0.3f * nl) {
			continue; /* not facing up (either winding) */
		}
		const float span = fmaxf(fmaxf(hoth_len(e1), hoth_len(e2)), hypotf(c[0] - b[0], c[1] - b[1]));
		const int steps = (int)fminf(ceilf(span / step), 96.0f) + 1;
		for (int u = 0; u <= steps; ++u) {
			for (int v = 0; u + v <= steps; ++v) {
				const float fu = (float)u / (float)steps, fv = (float)v / (float)steps;
				const float x = a[0] + e1[0] * fu + e2[0] * fv;
				const float y = a[1] + e1[1] * fu + e2[1] * fv;
				const float z = a[2] + e1[2] * fu + e2[2] * fv;
				const int i = (int)((x - d->x0) / d->cx), j = (int)((y - d->y0) / d->cy);
				if (i < 0 || j < 0 || i >= HOTH_DECK_NX || j >= HOTH_DECK_NY) {
					continue;
				}
				if (!(d->z[j][i] >= z)) {
					d->z[j][i] = z;
				}
			}
		}
	}
	d->ok = 1;
}

/* The deck's height at model (x, y); 0 where there is no hull. */
static int hoth_deck_at(float x, float y, float* z) {
	const HothDeck* d = &s_hoth.deck;
	if (!d->ok) {
		return 0;
	}
	const int i = (int)floorf((x - d->x0) / d->cx), j = (int)floorf((y - d->y0) / d->cy);
	if (i < 0 || j < 0 || i >= HOTH_DECK_NX || j >= HOTH_DECK_NY || isnan(d->z[j][i])) {
		return 0;
	}
	*z = d->z[j][i];
	return 1;
}

static void hoth_load_mesh(AeronCommandBuffer* cmd, int index) {
	HothMesh* m = &s_hoth.mesh[index];
	AeronFlightModel model;
	memset(&model, 0, sizeof model);
	if (!XwaRemasterShip_BuildOptModel(k_opt[index], &model)) {
		Aeron_LogWarn("xwa.remaster", "VR intro Hoth: no OPT for '%s'", k_opt[index]);
		return;
	}
	m->len_m = model.bounds.max.y - model.bounds.min.y;
	m->glow_count = 0;
	for (uint32_t g = 0; g < model.engine_glow_count && m->glow_count < HOTH_MAX_GLOWS; ++g) {
		const AeronFlightEngineGlow* eg = &model.engine_glows[g];
		if (!eg->enabled) {
			continue;
		}
		float* o = m->glow[m->glow_count++];
		o[0] = eg->position.x;
		o[1] = eg->position.y;
		o[2] = eg->position.z;
		o[3] = 0.45f * fmaxf(eg->dimensions.x, eg->dimensions.y);
		o[4] = eg->core_rgba[0];
		o[5] = eg->core_rgba[1];
		o[6] = eg->core_rgba[2];
	}
	/* The parts the ion cannon's strike needs (OPT mesh types: 7 bridge,
	 * 8 shield generator). */
	m->shield_count = 0;
	m->bridge[0] = 0.0f;
	m->bridge[1] = 0.35f * m->len_m;
	m->bridge[2] = 0.1f * m->len_m;
	for (uint32_t c = 0; c < model.component_count; ++c) {
		const AeronFlightComponent* comp = &model.components[c];
		const float centre[3] = { comp->center.x, comp->center.y, comp->center.z };
		if (comp->mesh_type == 8 && m->shield_count < 2 && c < AERON_MAX_MESH_SLOTS) {
			m->shield_slot[m->shield_count] = (int)c;
			memcpy(m->shield[m->shield_count++], centre, sizeof centre);
		} else if (comp->mesh_type == 7) {
			memcpy(m->bridge, centre, sizeof centre);
		}
	}
	if (index == HOTH_ISD) {
		hoth_build_deck(&model);
	}
	AeronSceneMeshCreateStatus status;
	m->mesh = AeronScene_MeshCreate(cmd, &model, k_opt[index], &status);
	Aeron_FlightModelFree(&model);
	if (!m->mesh) {
		Aeron_LogWarn("xwa.remaster", "VR intro Hoth: '%s' mesh creation failed (%d)", k_opt[index], (int)status);
	}
}

/* Clearance from the Star Destroyers' hulls along every keyed and wheeling
 * flight, sampled; logged once (XWA_XR_HOTH_CHECK=1). The hull, model
 * metres: a wedge from the bow (y -1012) to the full beam (473 each side)
 * at the stern, its deck at z 20 forward, the superstructure and tower up
 * to z 320 aft of y 200, its keel at z -255. */
static float hoth_hull_clearance(const float m[3]) {
	const float y = m[1];
	const float hx = fmaxf(473.0f * (y + 1012.0f) / 2024.0f, 30.0f);
	const float z1 = y > 200.0f ? 320.0f : 20.0f;
	const float dx = fmaxf(fabsf(m[0]) - hx, 0.0f), dy = fmaxf(fabsf(y) - 1012.0f, 0.0f);
	const float dz = fmaxf(fmaxf(-255.0f - m[2], m[2] - z1), 0.0f);
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void hoth_check_clearance(void) {
	const char* names[3] = { "transport", "X-wing 1", "X-wing 2" };
	for (int c = 0; c < 3 + HOTH_FIGHTERS; ++c) {
		float worst = 1e9f, worst_t = 0.0f;
		int worst_isd = 0;
		for (float t = HOTH_SHIPS_FROM; t < 120.9f; t += 0.05f) {
			float p[3];
			if (c == 0) {
				if (t > HOTH_JUMP) {
					break;
				}
				hoth_path(&s_hoth.transport, t, p);
			} else if (c < 3) {
				if (t > XWA_VR_HOTH_CUT) {
					break;
				}
				hoth_path(&s_hoth.xwing[c - 1], t, p);
			} else {
				const int f = c - 3;
				if (t < HOTH_FIGHT_FROM && k_fighters[f].keyed <= 0) {
					continue;
				}
				if (k_fighters[f].dies > 0.0f && t > k_fighters[f].dies) {
					break;
				}
				hoth_fighter_pos(f, t, p);
			}
			for (int i = 0; i < 2; ++i) {
				float m[3];
				hoth_local_to_model(&s_hoth.isd[i], p, m);
				const float d = hoth_hull_clearance(m);
				if (d < worst) {
					worst = d;
					worst_t = t;
					worst_isd = i;
				}
			}
		}
		char who[32];
		if (c < 3) {
			snprintf(who, sizeof who, "%s", names[c]);
		} else {
			snprintf(who, sizeof who, "fighter %d", c - 3);
		}
		Aeron_LogInfo("xwa.remaster", "VR intro Hoth: %s clears the Star Destroyers' hulls by %.0f m (SD %d, %.2f s)",
					  who, (double)worst, worst_isd + 1, (double)worst_t);
	}
}

int XwaVrIntroHoth_Load(AeronCommandBuffer* cmd) {
	if (!cmd) {
		return 0;
	}
	switch (s_hoth.step) {
		case 0:
			hoth_load_planet(cmd);
			hoth_make_sprites(cmd);
			hoth_load_boom(cmd);
			hoth_load_logo(cmd);
			break;
		case 1:
		case 2:
		case 3:
		case 4:
			hoth_load_mesh(cmd, s_hoth.step - 1);
			break;
		default:
			return 1;
	}
	if (++s_hoth.step == 1 + HOTH_MESHES) {
		hoth_build_paths();
		if (SDL_getenv("XWA_XR_HOTH_CHECK")) {
			hoth_check_clearance();
		}
		Aeron_LogInfo("xwa.remaster",
					  "VR intro Hoth: %s (planet %s, transport %s, X-wing %s, Star Destroyer %s, TIE %s, logo %s)",
					  XwaVrIntroHoth_Ready() ? "ready" : "not available", s_hoth.planet ? "yes" : "no",
					  s_hoth.mesh[HOTH_TRANSPORT].mesh ? "yes" : "no", s_hoth.mesh[HOTH_XWING].mesh ? "yes" : "no",
					  s_hoth.mesh[HOTH_ISD].mesh ? "yes" : "no", s_hoth.mesh[HOTH_TIE].mesh ? "yes" : "no",
					  s_hoth.logo ? "yes" : "no");
		return 1;
	}
	return 0;
}

int XwaVrIntroHoth_Ready(void) {
	return s_hoth.step >= 1 + HOTH_MESHES && s_hoth.paths_built && s_hoth.planet &&
		   s_hoth.mesh[HOTH_TRANSPORT].mesh && s_hoth.mesh[HOTH_XWING].mesh && s_hoth.mesh[HOTH_ISD].mesh;
}

void XwaVrIntroHoth_Release(void) {
	for (int i = 0; i < HOTH_MESHES; ++i) {
		if (s_hoth.mesh[i].mesh) {
			AeronScene_MeshDestroy(s_hoth.mesh[i].mesh);
		}
	}
	AeronTexture* tex[4] = { s_hoth.planet, s_hoth.glow, s_hoth.beam, s_hoth.logo };
	for (int i = 0; i < 4; ++i) {
		if (tex[i]) {
			Aeron_DestroyTexture(tex[i]);
		}
	}
	for (int i = 0; i < HOTH_BOOM_FRAMES; ++i) {
		if (s_hoth.boom[i]) {
			Aeron_DestroyTexture(s_hoth.boom[i]);
		}
	}
	memset(&s_hoth, 0, sizeof s_hoth);
}

/* ---- drawing ------------------------------------------------------------------ */

static void hoth_billboard(AeronScene3D* scene, AeronTexture* tex, AeronSceneBillboardStage stage,
						   AeronSceneBillboardBlend blend, const float c[4][3], const float tint[4]) {
	if (!tex) {
		return;
	}
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = tex;
	d.blend = blend;
	d.stage = stage;
	static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	for (int i = 0; i < 4; ++i) {
		memcpy(d.corners[i], c[i], sizeof d.corners[i]);
		memcpy(d.uv[i], uv[i], sizeof d.uv[i]);
		memcpy(d.colors[i], tint, sizeof d.colors[i]);
	}
	AeronScene_AddBillboard(scene, &d);
}

/* A viewer-facing square (world units) round world point `c`. */
static void hoth_facing(const float c[3], float half, float out[4][3]) {
	float f[3] = { c[0] - s_hoth.org[0], c[1] - s_hoth.org[1], c[2] - s_hoth.org[2] };
	hoth_norm(f);
	float r[3], u[3];
	hoth_cross(f, s_hoth.up, r);
	if (!hoth_norm(r)) {
		memcpy(r, s_hoth.right, sizeof r);
	}
	hoth_cross(r, f, u);
	for (int k = 0; k < 3; ++k) {
		out[0][k] = c[k] - half * r[k] - half * u[k];
		out[1][k] = c[k] + half * r[k] - half * u[k];
		out[2][k] = c[k] + half * r[k] + half * u[k];
		out[3][k] = c[k] - half * r[k] + half * u[k];
	}
}

/* An additive glow of `size` metres at LOCAL `p`, colour `rgb` (HDR). */
static void hoth_glow(AeronScene3D* scene, const float p[3], float size, const float rgb[3]) {
	float w[3], c[4][3];
	hoth_wpos(p, w);
	hoth_facing(w, 0.5f * size * s_hoth.k, c);
	const float tint[4] = { rgb[0], rgb[1], rgb[2], 1.0f };
	hoth_billboard(scene, s_hoth.glow, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, c,
				   tint);
}

/* The fighter explosion's frame at `age` seconds (1 s long), `size`
 * metres at LOCAL `p`. */
static void hoth_boom(AeronScene3D* scene, const float p[3], float size, float age, float brightness) {
	if (age < 0.0f || age >= 1.0f) {
		return;
	}
	const int f = (int)(age * (float)HOTH_BOOM_FRAMES);
	if (!s_hoth.boom[f]) {
		return;
	}
	float w[3], c[4][3];
	hoth_wpos(p, w);
	hoth_facing(w, 0.5f * size * s_hoth.k, c);
	const float b = 2.2f * brightness;
	const float tint[4] = { b, b, b, 1.0f };
	hoth_billboard(scene, s_hoth.boom[f], AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c,
				   tint);
}

/* A bolt from LOCAL `a` to `b`, `width` metres, facing the viewer. */
static void hoth_bolt(AeronScene3D* scene, const float a[3], const float b[3], float width, const float rgb[3]) {
	if (!s_hoth.beam) {
		return;
	}
	float wa[3], wb[3], along[3], mid[3], side[3];
	hoth_wpos(a, wa);
	hoth_wpos(b, wb);
	for (int k = 0; k < 3; ++k) {
		along[k] = wb[k] - wa[k];
		mid[k] = 0.5f * (wa[k] + wb[k]) - s_hoth.org[k];
	}
	hoth_cross(along, mid, side);
	if (!hoth_norm(side)) {
		return;
	}
	const float hw = 0.5f * width * s_hoth.k;
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = s_hoth.beam;
	d.blend = AERON_SCENE_BILLBOARD_BLEND_ADDITIVE;
	d.stage = AERON_SCENE_BILLBOARD_STAGE_OVERLAY;
	for (int k = 0; k < 3; ++k) {
		d.corners[0][k] = wa[k] - side[k] * hw;
		d.corners[1][k] = wb[k] - side[k] * hw;
		d.corners[2][k] = wb[k] + side[k] * hw;
		d.corners[3][k] = wa[k] + side[k] * hw;
	}
	static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	for (int i = 0; i < 4; ++i) {
		memcpy(d.uv[i], uv[i], sizeof d.uv[i]);
		d.colors[i][0] = rgb[0];
		d.colors[i][1] = rgb[1];
		d.colors[i][2] = rgb[2];
		d.colors[i][3] = 1.0f;
	}
	AeronScene_AddBillboard(scene, &d);
}

/* Bolts stay visible far off: at least `min_deg` wide in view. */
static float hoth_bolt_width(const float p[3], float metres, float min_deg) {
	return fmaxf(metres, hoth_len(p) * min_deg * 0.017453293f);
}

static void hoth_submit_planet(AeronScene3D* scene, float t) {
	const float fade = hoth_smooth((t - HOTH_FADE_FROM) / (HOTH_FADE_TO - HOTH_FADE_FROM));
	if (!s_hoth.planet || fade <= 0.0f) {
		return;
	}
	/* A sky quad (drawn under everything, at the far plane): its distance
	 * only sets the projection. */
	const float dist = 1.0e5f;
	float dir[3], cw[3], c[4][3];
	hoth_dir(HOTH_AZ, HOTH_EL, 1.0f, dir);
	hoth_wdir(dir, cw);
	float centre[3];
	for (int k = 0; k < 3; ++k) {
		centre[k] = s_hoth.org[k] + cw[k] * dist * s_hoth.k;
	}
	const float half = dist * s_hoth.k * tanf(HOTH_RADIUS_DEG * 0.017453293f) / fmaxf(s_hoth.planet_disc, 0.5f);
	hoth_facing(centre, half, c);
	const float tint[4] = { fade, fade, fade, fade }; /* premultiplied */
	hoth_billboard(scene, s_hoth.planet, AERON_SCENE_BILLBOARD_STAGE_SKY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

/* Model -> world of a craft at LOCAL `pos` with LOCAL nose and up. */
static void hoth_matrix(const float pos[3], const float nose[3], const float upv[3], float m[16]) {
	float n[3], u[3], r[3], p[3];
	hoth_wdir(nose, n);
	hoth_wdir(upv, u);
	hoth_cross(n, u, r);
	hoth_norm(r);
	hoth_cross(r, n, u);
	hoth_wpos(pos, p);
	const float k = s_hoth.k;
	for (int i = 0; i < 3; ++i) {
		m[i * 4 + 0] = k * r[i];
		m[i * 4 + 1] = -k * n[i];
		m[i * 4 + 2] = k * u[i];
		m[i * 4 + 3] = p[i];
	}
	m[12] = m[13] = m[14] = 0.0f;
	m[15] = 1.0f;
}

static void hoth_add_table(AeronScene3D* scene, int mesh, const float m[16], int far,
						   const AeronSceneMeshTable* table) {
	if (!s_hoth.mesh[mesh].mesh) {
		return;
	}
	AeronSceneMeshInstance inst;
	memset(&inst, 0, sizeof inst);
	inst.mesh = s_hoth.mesh[mesh].mesh;
	memcpy(inst.transform, m, sizeof inst.transform);
	memcpy(inst.prev_transform, m, sizeof inst.prev_transform);
	inst.zero_velocity = 1;
	inst.no_local_lights = 1;
	inst.cull_mode = AERON_CULL_BACK;
	if (far) {
		inst.shadow_flags |= AERON_SCENE_INSTANCE_NO_CAST_SHADOW;
	}
	inst.mesh_table = table;
	AeronScene_AddMeshInstance(scene, &inst);
}

static void hoth_add(AeronScene3D* scene, int mesh, const float m[16], int far) {
	hoth_add_table(scene, mesh, m, far, NULL);
}

/* The engines' glow, seen from behind. */
static void hoth_glows(AeronScene3D* scene, int mesh, const float m[16], float boost) {
	const HothMesh* hm = &s_hoth.mesh[mesh];
	if (!s_hoth.glow || !hm->glow_count) {
		return;
	}
	float back[3] = { m[1], m[5], m[9] }; /* model +y: the rear */
	hoth_norm(back);
	for (int g = 0; g < hm->glow_count; ++g) {
		const float* o = hm->glow[g];
		float p[3], to[3], c[4][3];
		for (int a = 0; a < 3; ++a) {
			p[a] = m[a * 4 + 0] * o[0] + m[a * 4 + 1] * o[1] + m[a * 4 + 2] * o[2] + m[a * 4 + 3];
			to[a] = s_hoth.org[a] - p[a];
		}
		hoth_norm(to);
		const float w = hoth_smooth(hoth_dot(back, to) * 1.6f);
		if (w <= 0.0f) {
			continue;
		}
		for (int a = 0; a < 3; ++a) {
			p[a] += to[a] * 0.6f * s_hoth.k;
		}
		const float b = 1.8f * w;
		const float core[4] = { (0.4f + 0.6f * o[4]) * b, (0.15f + 0.85f * o[5]) * b, (0.15f + 0.85f * o[6]) * b,
								1.0f };
		hoth_facing(p, o[3] * boost * s_hoth.k, c);
		hoth_billboard(scene, s_hoth.glow, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE,
					   c, core);
	}
}

/* The Star Destroyers: positions, poses (fixed). */
static void hoth_isd_matrix(int i, float m[16]) {
	const HothFrame* f = &s_hoth.isd[i];
	hoth_matrix(f->pos, f->n, f->u, m);
}

/* The lead Star Destroyer's shield generator that blows up (the film's
 * left one): the one nearer the viewer, so the tower does not hide it;
 * LOCAL, a little above the dome. Returns its mesh slot (-1: no such part,
 * a stand-in place). */
static int hoth_left_shield(float out[3]) {
	const HothMesh* hm = &s_hoth.mesh[HOTH_ISD];
	if (hm->shield_count < 1) {
		const float m[3] = { -100.0f, 0.35f * hm->len_m, 0.11f * hm->len_m };
		hoth_model_to_local(&s_hoth.isd[0], m, out);
		return -1;
	}
	int pick = 0;
	hoth_model_to_local(&s_hoth.isd[0], hm->shield[0], out);
	if (hm->shield_count > 1) {
		float b[3];
		hoth_model_to_local(&s_hoth.isd[0], hm->shield[1], b);
		if (hoth_len(b) < hoth_len(out)) {
			memcpy(out, b, sizeof b);
			pick = 1;
		}
	}
	for (int i = 0; i < 3; ++i) {
		out[i] += 25.0f * s_hoth.isd[0].u[i];
	}
	return hm->shield_slot[pick];
}

/* The lead Star Destroyer once its shield generator is gone: every part as
 * modelled but that dome, hidden as the game hides a destroyed component
 * (ship.c, XwaRemasterShip_BuildMeshTable). NULL when there is no such
 * part. */
static const AeronSceneMeshTable* hoth_isd_wrecked_table(void) {
	static AeronSceneMeshTable table;
	static int built_slot = -2;
	float dome[3];
	const int slot = hoth_left_shield(dome);
	if (slot < 0) {
		return NULL;
	}
	if (built_slot != slot) {
		for (uint32_t mi = 0; mi < AERON_MAX_MESH_SLOTS; ++mi) {
			memset(table.rows[mi], 0, sizeof table.rows[mi]);
			table.rows[mi][0][0] = table.rows[mi][1][1] = table.rows[mi][2][2] = 1.0f;
			table.visibility_packed[mi >> 2][mi & 3] = (int)mi == slot ? 0.0f : 1.0f;
			table.highlight_packed[mi >> 2][mi & 3] = 0.0f;
			table.markings_packed[mi >> 2][mi & 3] = 0.0f;
			table.emissive_packed[mi >> 2][mi & 3] = 1.0f;
		}
		built_slot = slot;
	}
	return &table;
}

/* The ion cannon: a glow on Hoth, the bolt across, a blue flash on the
 * superstructure, the shield generator bursting and burning; ion lightning
 * over the hull. */
static void hoth_submit_ion(AeronScene3D* scene, float t) {
	if (t < HOTH_ION_GLOW) {
		return;
	}
	static const float blue[3] = { 0.45f * 6.0f, 0.65f * 6.0f, 1.0f * 6.0f };
	float from[3], hit[3];
	/* From the planet's lit limb, far out (a sky distance for the eyes). */
	hoth_dir(-34.0f, -4.0f, 30000.0f, from);
	hoth_model_to_local(&s_hoth.isd[0], s_hoth.mesh[HOTH_ISD].bridge, hit);
	if (t < HOTH_ION_LAUNCH) {
		const float g = hoth_smooth((t - HOTH_ION_GLOW) / (HOTH_ION_LAUNCH - HOTH_ION_GLOW));
		const float rgb[3] = { blue[0] * g, blue[1] * g, blue[2] * g };
		hoth_glow(scene, from, hoth_bolt_width(from, 900.0f, 3.5f), rgb);
	} else if (t < HOTH_ION_HIT) {
		/* Slowing as it comes (the film's bolt crawls the last stretch). */
		const float s = 1.0f - powf(1.0f - (t - HOTH_ION_LAUNCH) / (HOTH_ION_HIT - HOTH_ION_LAUNCH), 2.2f);
		for (int i = 0; i < 12; ++i) {
			const float si = fmaxf(s - 0.008f * (float)i * (1.0f + 3.0f * (1.0f - s)), 0.0f);
			float p[3];
			for (int a = 0; a < 3; ++a) {
				p[a] = from[a] + (hit[a] - from[a]) * si;
			}
			const float k = 1.0f - (float)i / 12.0f;
			const float size = hoth_bolt_width(p, 150.0f, 3.0f) * (0.4f + 0.6f * k);
			const float rgb[3] = { blue[0] * k, blue[1] * k, blue[2] * k };
			hoth_glow(scene, p, size, rgb);
		}
	} else if (t < HOTH_ION_HIT + 0.7f) {
		const float g = 1.0f - (t - HOTH_ION_HIT) / 0.7f;
		const float rgb[3] = { blue[0] * 1.6f * g, blue[1] * 1.6f * g, blue[2] * 1.6f * g };
		hoth_glow(scene, hit, hoth_bolt_width(hit, 700.0f, 6.0f) * (0.6f + 0.4f * g), rgb);
	}
	/* The shield generator: it blows up once (the dome hidden from
	 * SHIELD_GONE, XwaVrIntroHoth_Submit). */
	if (t >= HOTH_SHIELD_BURST && t < HOTH_SHIELD_BURST + 1.6f) {
		float dome[3];
		hoth_left_shield(dome);
		hoth_boom(scene, dome, 260.0f, (t - HOTH_SHIELD_BURST) / 1.6f, 1.0f);
	}
	/* Ion lightning wandering over the hull until the scene ends: each arc's
	 * foot drifts over the deck (slow sines, every arc its own), its jagged
	 * path struck afresh 14 times a second, an arc now and then dark. */
	if (t >= HOTH_ION_HIT) {
		const float arc[3] = { blue[0] * 1.4f, blue[1] * 1.4f, blue[2] * 1.4f };
		const float since = t - HOTH_ION_HIT;
		const int busy = t < HOTH_ARCS_A_TO || (t >= HOTH_ARCS_B_FROM && t < HOTH_ARCS_B_TO);
		const int arcs = busy ? 7 : 4;
		const uint32_t tick = (uint32_t)(t * 14.0f);
		const float len = s_hoth.mesh[HOTH_ISD].len_m;
		for (int i = 0; i < arcs; ++i) {
			const float fi = (float)i;
			const uint32_t s = tick * 31u + (uint32_t)i * 7u;
			if (hoth_hash(s + 50u) < 0.22f) {
				continue;
			}
			/* Along the deck (-1 bow .. 1 aft) and across it (-1 .. 1). */
			const float along = 0.6f * sinf(since * (0.23f + 0.05f * fi) + 1.7f * fi) +
								0.4f * sinf(since * (0.11f + 0.03f * fi) + 2.9f * fi + 0.4f);
			const float across = 0.7f * sinf(since * (0.31f + 0.04f * fi) + 0.9f * fi + 0.5f) +
								 0.3f * sinf(since * (0.17f + 0.02f * fi) + 2.3f * fi);
			/* Bow (-0.5) to aft of the tower (0.3); the wedge's beam there. */
			const float my = (-0.1f + 0.36f * along) * len;
			const float half_w = fmaxf(0.234f * len * (my / len + 0.5f) * 0.85f, 15.0f);
			float m[3] = { across * half_w, my, 0.0f };
			/* Onto the hull: in towards the keel line until there is deck. */
			int on = 0;
			for (int tries = 0; tries < 6 && !on; ++tries) {
				on = hoth_deck_at(m[0], m[1], &m[2]);
				if (!on) {
					m[0] *= 0.5f;
				}
			}
			if (!on) {
				continue;
			}
			m[2] += 4.0f;
			float prev[3];
			hoth_model_to_local(&s_hoth.isd[0], m, prev);
			for (int seg = 0; seg < 5; ++seg) {
				const float dx = (hoth_hash(s + 10u + (uint32_t)seg) - 0.5f) * 110.0f;
				const float dy = (hoth_hash(s + 20u + (uint32_t)seg) - 0.5f) * 110.0f;
				float z;
				if (!hoth_deck_at(m[0] + dx, m[1] + dy, &z) || fabsf(z - m[2]) > 70.0f) {
					break; /* off the hull's edge or up a wall: the arc ends */
				}
				m[0] += dx;
				m[1] += dy;
				m[2] = z + 2.0f + 6.0f * hoth_hash(s + 30u + (uint32_t)seg);
				float p[3];
				hoth_model_to_local(&s_hoth.isd[0], m, p);
				hoth_bolt(scene, prev, p, hoth_bolt_width(p, 6.0f, 0.15f), arc);
				memcpy(prev, p, sizeof prev);
			}
		}
	}
}

/* Turbolasers from the Star Destroyers at the ships (near misses) from
 * 66 s; the fighters' lasers in the far fights. Each bolt is a function of
 * the film's time alone (no state). */
static void hoth_submit_fire(AeronScene3D* scene, float t) {
	static const float green[3] = { 0.35f * 4.0f, 1.0f * 4.0f, 0.4f * 4.0f };
	static const float red[3] = { 1.0f * 4.0f, 0.18f * 4.0f, 0.12f * 4.0f };
	const float speed_green = 650.0f;
	/* The turbolasers: at the transport and the X-wings until the cut, then
	 * at the fights. */
	for (int i = 0; i < 400; ++i) {
		const float tf = 66.0f + 0.27f * (float)i;
		if (tf > t) {
			break;
		}
		if (t - tf > 6.0f) {
			continue;
		}
		const HothFrame* c = &s_hoth.isd[i & 1];
		float from[3];
		const float m[3] = { (hoth_hash((uint32_t)i * 3u + 1u) - 0.5f) * 500.0f,
							 (hoth_hash((uint32_t)i * 3u + 2u) - 0.4f) * 1200.0f, 90.0f };
		hoth_model_to_local(c, m, from);
		float aim[3];
		if (tf < XWA_VR_HOTH_CUT) {
			const int target = i % 3;
			float tp[3];
			if (target == 0) {
				hoth_path(&s_hoth.transport, tf + 3.0f, tp);
			} else {
				hoth_fighter_pos(target - 1, tf + 3.0f, tp);
			}
			memcpy(aim, tp, sizeof aim);
		} else {
			/* A rebel in a fight (the X-wings are fighters 0, 1, 7, 8). */
			static const int rebels[4] = { 0, 1, 7, 8 };
			hoth_fighter_pos(rebels[i & 3], tf + 2.5f, aim);
		}
		for (int a = 0; a < 3; ++a) {
			aim[a] += (hoth_hash((uint32_t)i * 7u + (uint32_t)a) - 0.5f) * 90.0f;
		}
		float d[3] = { aim[0] - from[0], aim[1] - from[1], aim[2] - from[2] };
		const float len = hoth_len(d);
		if (len < 1.0f) {
			continue;
		}
		hoth_norm(d);
		const float s = (t - tf) * speed_green;
		if (s > len * 1.6f) {
			continue;
		}
		float a0[3], a1[3];
		for (int a = 0; a < 3; ++a) {
			a0[a] = from[a] + d[a] * fmaxf(s - 70.0f, 0.0f);
			a1[a] = from[a] + d[a] * s;
		}
		hoth_bolt(scene, a0, a1, hoth_bolt_width(a1, 5.0f, 0.12f), green);
	}
	/* The fighters: every 0.5-0.9 s each fires at an enemy of its fight
	 * (where that one will be when the bolt gets there). */
	if (t < HOTH_FIGHT_FROM) {
		return;
	}
	for (int f = 0; f < HOTH_FIGHTERS; ++f) {
		const HothFighter* hf = &k_fighters[f];
		const float period = 0.5f + 0.4f * hoth_hash((uint32_t)f * 11u + 3u);
		const float first = fmaxf(HOTH_FIGHT_FROM, hf->keyed > 0 ? XWA_VR_HOTH_CUT + 3.0f : HOTH_FIGHT_FROM) +
							period * hoth_hash((uint32_t)f * 11u + 4u);
		for (int k = 0; k < 4; ++k) {
			const int shot = (int)floorf((t - first) / period) - k;
			if (shot < 0) {
				break;
			}
			const float tf = first + period * (float)shot;
			if (hf->dies > 0.0f && tf >= hf->dies) {
				continue;
			}
			/* An enemy in the same fight, still flying. */
			int target = -1;
			for (int j = 0, pick = (int)(hoth_hash((uint32_t)(f * 131 + shot)) * 8.0f); j < HOTH_FIGHTERS; ++j) {
				const int e = (pick + j) % HOTH_FIGHTERS;
				if (k_fighters[e].fight == hf->fight && k_fighters[e].side != hf->side &&
					!(k_fighters[e].dies > 0.0f && tf >= k_fighters[e].dies)) {
					target = e;
					break;
				}
			}
			if (target < 0) {
				continue;
			}
			float from[3], to[3];
			hoth_fighter_pos(f, tf, from);
			hoth_fighter_pos(target, tf + 0.4f, to);
			float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
			const float len = hoth_len(d);
			if (len < 1.0f) {
				continue;
			}
			hoth_norm(d);
			const float s = (t - tf) * 800.0f;
			if (s > len * 1.3f) {
				continue;
			}
			float a0[3], a1[3];
			for (int a = 0; a < 3; ++a) {
				a0[a] = from[a] + d[a] * fmaxf(s - 60.0f, 0.0f);
				a1[a] = from[a] + d[a] * s;
			}
			hoth_bolt(scene, a0, a1, hoth_bolt_width(a1, 1.2f, 0.15f), hf->side == HOTH_SIDE_REBEL ? red : green);
		}
	}
}

/* The transport, its engines, the jump: a white flash and a streak where
 * it goes. */
static void hoth_submit_transport(AeronScene3D* scene, float t) {
	float m[16];
	if (t < HOTH_JUMP) {
		float pos[3], nose[3], upv[3];
		hoth_transport_pose(t, pos, nose, upv);
		hoth_matrix(pos, nose, upv, m);
		hoth_add(scene, HOTH_TRANSPORT, m, hoth_len(pos) > 400.0f);
		/* Far off its engines carry it (the film's glowing speck). */
		hoth_glows(scene, HOTH_TRANSPORT, m, 1.0f + fminf(hoth_len(pos) / 250.0f, 9.0f));
		return;
	}
	const float age = t - HOTH_JUMP;
	if (age > 0.45f) {
		return;
	}
	float pos[3], nose[3], upv[3];
	hoth_transport_pose(HOTH_JUMP, pos, nose, upv);
	const float g = 1.0f - age / 0.45f;
	const float white[3] = { 3.5f * g, 3.8f * g, 4.5f * g };
	hoth_glow(scene, pos, hoth_bolt_width(pos, 160.0f, 3.5f) * (0.6f + 0.8f * g), white);
	float tip[3];
	for (int a = 0; a < 3; ++a) {
		tip[a] = pos[a] + nose[a] * (300.0f + 3000.0f * age);
	}
	hoth_bolt(scene, pos, tip, hoth_bolt_width(pos, 12.0f, 0.5f), white);
}

static void hoth_submit_fights(AeronScene3D* scene, float t) {
	if (t < HOTH_FIGHT_FROM) {
		return;
	}
	for (int f = 0; f < HOTH_FIGHTERS; ++f) {
		const HothFighter* hf = &k_fighters[f];
		if (hf->keyed > 0) {
			continue; /* drawn with the keyed craft */
		}
		if (hf->dies > 0.0f && t >= hf->dies) {
			float p[3];
			hoth_fighter_pos(f, hf->dies, p);
			hoth_boom(scene, p, hoth_bolt_width(p, 80.0f, 3.5f), (t - hf->dies) / 1.0f, 1.0f);
			continue;
		}
		const int mesh = hf->side == HOTH_SIDE_EMPIRE ? HOTH_TIE : HOTH_XWING;
		if (!s_hoth.mesh[mesh].mesh) {
			continue;
		}
		float pos[3], nose[3], upv[3], m[16];
		hoth_fighter_pose(f, t, pos, nose, upv);
		hoth_matrix(pos, nose, upv, m);
		hoth_add(scene, mesh, m, 1);
		hoth_glows(scene, mesh, m, 9.0f);
	}
}

/* The X-Wing Alliance logo on the screen's place while the film shows its
 * own (screen put away). */
static void hoth_submit_logo(AeronScene3D* scene, float t, float screen_dist, float screen_w, float lift) {
	const float a = hoth_smooth((t - XWA_VR_HOTH_LOGO_IN) / 0.33f) *
					(1.0f - hoth_smooth((t - XWA_VR_HOTH_LOGO_OUT) / XWA_VR_HOTH_LOGO_FADE));
	if (!s_hoth.logo || a <= 0.0f) {
		return;
	}
	/* The film's logo spans ~88 % of its width. */
	const float w = 0.88f * screen_w, h = w * s_hoth.logo_aspect;
	float c[4][3];
	const float corners[4][3] = {
		{ -0.5f * w, lift - 0.5f * h, -screen_dist },
		{ 0.5f * w, lift - 0.5f * h, -screen_dist },
		{ 0.5f * w, lift + 0.5f * h, -screen_dist },
		{ -0.5f * w, lift + 0.5f * h, -screen_dist },
	};
	for (int i = 0; i < 4; ++i) {
		hoth_wpos(corners[i], c[i]);
	}
	const float tint[4] = { a, a, a, a }; /* premultiplied */
	hoth_billboard(scene, s_hoth.logo, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

void XwaVrIntroHoth_Submit(AeronScene3D* scene, const float org[3], const float right[3], const float up[3],
						   const float fwd[3], float k, float t, float screen_dist, float screen_w, float lift) {
	s_hoth.submitted = 0;
	if (!scene || !XwaVrIntroHoth_Ready() || t < HOTH_FADE_FROM || t >= XWA_VR_HOTH_HYPER) {
		return;
	}
	memcpy(s_hoth.org, org, sizeof s_hoth.org);
	memcpy(s_hoth.right, right, sizeof s_hoth.right);
	memcpy(s_hoth.up, up, sizeof s_hoth.up);
	memcpy(s_hoth.fwd, fwd, sizeof s_hoth.fwd);
	s_hoth.k = k;
	s_hoth.submitted = 1;
	hoth_isd_drift(t);
	hoth_submit_planet(scene, t);
	hoth_submit_logo(scene, t, screen_dist, screen_w, lift);
	if (t < HOTH_SHIPS_FROM) {
		return;
	}
	float m[16];
	for (int i = 0; i < 2; ++i) {
		hoth_isd_matrix(i, m);
		hoth_add_table(scene, HOTH_ISD, m, 1, i == 0 && t >= HOTH_SHIELD_GONE ? hoth_isd_wrecked_table() : NULL);
	}
	hoth_submit_transport(scene, t);
	/* The X-wings that flew past, then in their fight. */
	for (int x = 0; x < 2; ++x) {
		float pos[3], nose[3], upv[3];
		hoth_fighter_pose(x, t, pos, nose, upv);
		hoth_matrix(pos, nose, upv, m);
		hoth_add(scene, HOTH_XWING, m, hoth_len(pos) > 400.0f);
		hoth_glows(scene, HOTH_XWING, m, 1.0f + fminf(hoth_len(pos) / 150.0f, 9.0f));
	}
	hoth_submit_fights(scene, t);
	hoth_submit_ion(scene, t);
	hoth_submit_fire(scene, t);
}

int XwaVrIntroHoth_Lighting(float t, XwaVrEnvLighting* out) {
	if (!out || !s_hoth.submitted) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	const float l = HOTH_LIGHT_FLOOR +
					(1.0f - HOTH_LIGHT_FLOOR) * hoth_smooth((t - HOTH_LIGHT_FROM) / (HOTH_LIGHT_TO - HOTH_LIGHT_FROM));
	/* The sun from behind the viewer's right, above (the Star Destroyers'
	 * faces towards the viewer lit, as the film's are); Hoth's cool bounce
	 * from the left below; space's ambient. */
	float key[3], fill[3];
	hoth_dir(130.0f, 35.0f, 1.0f, key);
	hoth_dir(HOTH_AZ, HOTH_EL, 1.0f, fill);
	hoth_wdir(key, out->key_dir);
	hoth_wdir(fill, out->fill_dir);
	const float sun = 1.6f * l, bounce = 0.35f * l;
	out->key_color[0] = 1.0f * sun;
	out->key_color[1] = 0.97f * sun;
	out->key_color[2] = 0.92f * sun;
	out->fill_color[0] = 0.55f * bounce;
	out->fill_color[1] = 0.68f * bounce;
	out->fill_color[2] = 0.85f * bounce;
	for (int c = 0; c < 3; ++c) {
		out->ambient_sides[c] = 0.035f * l;
		out->ambient_top[c] = 0.035f * l;
		out->ambient_bottom[c] = 0.04f * l;
	}
	return 1;
}
