#include "xwa_runtime/snapshot/snapshot_present.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- tuning ------------------------------------------------------------
 * A larger one-step move is a cut (respawn, hyperspace, slot reuse), and so
 * is a larger one-step turn: those show the newest pose. */
#define PRESENT_TELEPORT_UNITS 0x40000
#define PRESENT_MIN_AXIS_DOT 0.9f
#define PRESENT_MAX_ANGLE_DELTA 0x2000 /* Q16: 45 degrees */
#define PRESENT_GAIN 0.05
enum {
	PRESENT_IDLE_FRAMES = 8, /* host frames without a step: a pause */
	PRESENT_SLOT_GROW = 256,
};

typedef struct PresentSlot {
	uint32_t serial; /* step the "curr" pose belongs to */
	uint16_t signature;
	uint8_t valid;
	/* This frame's shown pose, for things attached to the object. */
	uint8_t moved;	 /* shown differs from the newest pose */
	uint8_t rotated; /* ... including its orientation */
	int32_t prev_pos[3], curr_pos[3];
	int16_t prev_rows[9], curr_rows[9];
	uint16_t prev_ang[4], curr_ang[4];
	int16_t prev_spin, curr_spin;
	int32_t shown_pos[3];
	float curr_axes[9], shown_axes[9];
} PresentSlot;

typedef struct PresentCamera {
	uint32_t serial;
	uint8_t valid;
	uint8_t region, external, map_mode, in_hangar;
	int32_t focus;
	int32_t prev_pos[3], curr_pos[3];
	float prev_rows[9], curr_rows[9];
} PresentCamera;

static PresentSlot* s_slots;
static uint32_t s_slot_cap;
static PresentCamera s_cam;
static uint32_t s_serial;
static int s_have_step, s_have_prev_step;
static int32_t s_step_time, s_prev_step_time;

/* Presentation clock, in sim ticks: advances by the average sim time per
 * host frame and is pulled gently toward half a step behind the newest step,
 * so the shown time moves evenly although the steps arrive every 2-3 frames. */
static struct {
	int valid;
	double present;
	double rate; /* sim ticks per host frame (EMA) */
	double step; /* sim ticks per step (EMA) */
	int32_t last_time;
	uint32_t idle;
} s_clock;

int XwaSnapshotPresent_Mode(void) {
	static int mode = -2;
	if (mode == -2) {
		const char* env = getenv("XWA_FLIGHT_INTERPOLATE");
		mode = !env || !env[0] ? -1 : env[0] == '0' ? 0 : 1;
	}
	return mode;
}

void XwaSnapshotPresent_Reset(void) {
	s_have_step = 0;
	s_have_prev_step = 0;
	s_clock.valid = 0;
	s_cam.valid = 0;
	if (s_slots) {
		memset(s_slots, 0, sizeof *s_slots * s_slot_cap);
	}
}

static PresentSlot* present_slot(uint32_t slot) {
	if (slot >= s_slot_cap) {
		const uint32_t cap = (slot / PRESENT_SLOT_GROW + 1) * PRESENT_SLOT_GROW;
		PresentSlot* grown = (PresentSlot*)realloc(s_slots, sizeof *grown * cap);
		if (!grown) {
			return NULL;
		}
		memset(grown + s_slot_cap, 0, sizeof *grown * (cap - s_slot_cap));
		s_slots = grown;
		s_slot_cap = cap;
	}
	return &s_slots[slot];
}

static float present_alpha(void) {
	if (!s_have_prev_step || s_step_time <= s_prev_step_time) {
		s_clock.valid = 0;
		return 1.0f;
	}
	const double span = (double)(s_step_time - s_prev_step_time);
	const int32_t t = s_step_time;
	if (!s_clock.valid) {
		s_clock.valid = 1;
		s_clock.step = span;
		s_clock.rate = span * 0.4; /* about 2.5 host frames per step */
		s_clock.last_time = t;
		s_clock.idle = 0;
		s_clock.present = (double)t - 0.5 * span;
	} else {
		const double advance = (double)(t - s_clock.last_time);
		s_clock.last_time = t;
		if (advance > 0.0) {
			s_clock.step += (advance - s_clock.step) / 16.0;
			s_clock.idle = 0;
		} else if (++s_clock.idle >= PRESENT_IDLE_FRAMES) {
			/* paused: hold, and resume from the newest step */
			s_clock.present = (double)t;
		}
		s_clock.rate += (advance - s_clock.rate) / 16.0;
		if (s_clock.rate > s_clock.step) {
			s_clock.rate = s_clock.step;
		}
		const double target = (double)t - 0.5 * s_clock.step;
		const double snap = s_clock.step * 4.0 > 16.0 ? s_clock.step * 4.0 : 16.0;
		s_clock.present += s_clock.rate;
		const double error = target - s_clock.present;
		if (error > snap || error < -snap) {
			s_clock.present = target;
		} else {
			s_clock.present += error * PRESENT_GAIN;
		}
	}
	double alpha = (s_clock.present - (double)s_prev_step_time) / span;
	if (alpha < 0.0) {
		alpha = 0.0;
	} else if (alpha > 1.0) {
		alpha = 1.0;
	}
	return (float)alpha;
}

/* ---- small vector helpers --------------------------------------------- */

static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static int normalize3(float* v) {
	const float len = sqrtf(dot3(v, v));
	if (len <= 1e-6f) {
		return 0;
	}
	v[0] /= len;
	v[1] /= len;
	v[2] /= len;
	return 1;
}

/* Rows 0 and 1 Gram-Schmidt, row 2 their cross product on the side of the
 * given row 2 (keeps whatever handedness the basis has). */
static int orthonormalize(float m[9]) {
	float* x = m;
	float* y = m + 3;
	float* z = m + 6;
	if (!normalize3(x)) {
		return 0;
	}
	const float d = dot3(x, y);
	for (int k = 0; k < 3; ++k) {
		y[k] -= d * x[k];
	}
	if (!normalize3(y)) {
		return 0;
	}
	const float c[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
	const float sign = dot3(c, z) < 0.0f ? -1.0f : 1.0f;
	for (int k = 0; k < 3; ++k) {
		z[k] = c[k] * sign;
	}
	return 1;
}

/* prev -> curr at alpha; 0 when the turn is too large to be motion. */
static int blend_basis(const float prev[9], const float curr[9], float alpha, float out[9]) {
	for (int r = 0; r < 3; ++r) {
		if (dot3(prev + r * 3, curr + r * 3) < PRESENT_MIN_AXIS_DOT) {
			return 0;
		}
	}
	for (int k = 0; k < 9; ++k) {
		out[k] = prev[k] + (curr[k] - prev[k]) * alpha;
	}
	return orthonormalize(out);
}

static void rows_to_axes(const int16_t rows[9], float out[9]) {
	for (int k = 0; k < 9; ++k) {
		out[k] = (float)rows[k] * (1.0f / 32768.0f);
	}
}

static int16_t axis_to_q15(float v) {
	long q = lrintf(v * 32768.0f);
	if (q > 32767) {
		q = 32767;
	} else if (q < -32767) {
		q = -32767;
	}
	return (int16_t)q;
}

static int blend_pos(const int32_t prev[3], const int32_t curr[3], float alpha, int32_t out[3]) {
	for (int k = 0; k < 3; ++k) {
		if (llabs((long long)curr[k] - prev[k]) > PRESENT_TELEPORT_UNITS) {
			memcpy(out, curr, sizeof(int32_t) * 3);
			return 0;
		}
	}
	for (int k = 0; k < 3; ++k) {
		const double delta = (double)curr[k] - (double)prev[k];
		out[k] = (int32_t)((long long)prev[k] + llround(delta * alpha));
	}
	return 1;
}

/* A world vector of the owner's newest pose, in its shown pose. */
static void slot_rotate(const PresentSlot* p, const float v[3], float out[3]) {
	const float local[3] = { dot3(p->curr_axes, v), dot3(p->curr_axes + 3, v), dot3(p->curr_axes + 6, v) };
	for (int k = 0; k < 3; ++k) {
		out[k] = local[0] * p->shown_axes[k] + local[1] * p->shown_axes[3 + k] + local[2] * p->shown_axes[6 + k];
	}
}

/* A world point attached to the owner: carried from its newest pose to the
 * shown one. */
static void slot_carry_point(const PresentSlot* p, XwaPreciseWorldPoint* point) {
	float rel[3];
	for (int k = 0; k < 3; ++k) {
		rel[k] = (float)((double)point->base[k] - (double)p->curr_pos[k]) + point->offset[k];
	}
	if (p->rotated) {
		float r[3];
		slot_rotate(p, rel, r);
		memcpy(rel, r, sizeof rel);
	}
	for (int k = 0; k < 3; ++k) {
		point->base[k] = p->shown_pos[k];
		point->offset[k] = rel[k];
	}
}

static void slot_carry_i32(const PresentSlot* p, int32_t pos[3]) {
	XwaPreciseWorldPoint point = { { pos[0], pos[1], pos[2] }, { 0.0f, 0.0f, 0.0f } };
	slot_carry_point(p, &point);
	for (int k = 0; k < 3; ++k) {
		pos[k] = (int32_t)((long long)point.base[k] + llroundf(point.offset[k]));
	}
}

static const PresentSlot* moved_owner(uint32_t slot, uint16_t signature) {
	if (slot >= s_slot_cap) {
		return NULL;
	}
	const PresentSlot* p = &s_slots[slot];
	return p->valid && p->moved && p->serial == s_serial && p->signature == signature ? p : NULL;
}

/* ---- per object --------------------------------------------------------- */

static void present_object(XwaFlightObject* f, float alpha) {
	PresentSlot* p = present_slot(f->slot);
	if (!p) {
		return;
	}
	const uint16_t now_ang[4] = { f->yaw, f->pitch, f->roll, f->angle_d };
	if (p->serial != s_serial || !p->valid || p->signature != f->signature) {
		const int continues =
			p->valid && p->signature == f->signature && p->serial != s_serial && p->serial + 1u == s_serial;
		if (continues) {
			memcpy(p->prev_pos, p->curr_pos, sizeof p->prev_pos);
			memcpy(p->prev_rows, p->curr_rows, sizeof p->prev_rows);
			memcpy(p->prev_ang, p->curr_ang, sizeof p->prev_ang);
			p->prev_spin = p->curr_spin;
		} else {
			/* New this step: start from where the engine says it moved
			 * from (a launch point for projectiles); hold the turn. */
			const int fresh_step = p->serial != s_serial;
			memcpy(p->prev_pos, f->has_mobj && fresh_step ? f->prev_world_pos : f->world_pos,
				   sizeof p->prev_pos);
			memcpy(p->prev_rows, f->rows, sizeof p->prev_rows);
			memcpy(p->prev_ang, now_ang, sizeof p->prev_ang);
			p->prev_spin = f->spin_angle;
		}
		p->serial = s_serial;
		p->signature = f->signature;
		p->valid = 1;
	}
	memcpy(p->curr_pos, f->world_pos, sizeof p->curr_pos);
	memcpy(p->curr_rows, f->rows, sizeof p->curr_rows);
	memcpy(p->curr_ang, now_ang, sizeof p->curr_ang);
	p->curr_spin = f->spin_angle;
	p->moved = 0;
	p->rotated = 0;
	memcpy(p->shown_pos, f->world_pos, sizeof p->shown_pos);
	rows_to_axes(f->rows, p->curr_axes);
	memcpy(p->shown_axes, p->curr_axes, sizeof p->shown_axes);

	/* Tunnel slots are static-world proxies recycled along the trench. */
	if (alpha >= 1.0f || f->genus == XWA_SNAP_GENUS_DS_TUNNEL) {
		return;
	}

	if (memcmp(p->prev_pos, p->curr_pos, sizeof p->prev_pos) != 0 &&
		blend_pos(p->prev_pos, p->curr_pos, alpha, p->shown_pos)) {
		memcpy(f->world_pos, p->shown_pos, sizeof f->world_pos);
		p->moved = 1;
	}

	if (memcmp(p->prev_rows, p->curr_rows, sizeof p->prev_rows) != 0) {
		float prev_axes[9], curr_axes[9], shown[9];
		rows_to_axes(p->prev_rows, prev_axes);
		memcpy(curr_axes, p->curr_axes, sizeof curr_axes);
		if (orthonormalize(prev_axes) && orthonormalize(curr_axes) &&
			blend_basis(prev_axes, curr_axes, alpha, shown)) {
			for (int k = 0; k < 9; ++k) {
				f->rows[k] = axis_to_q15(shown[k]);
			}
			/* attached things: rotate with the exact bases */
			memcpy(p->curr_axes, curr_axes, sizeof p->curr_axes);
			memcpy(p->shown_axes, shown, sizeof p->shown_axes);
			p->moved = 1;
			p->rotated = 1;
		}
	}

	/* The Euler path (orient-dirty objects, projectiles, debris spin). */
	int angles_ok = 1;
	int16_t d_ang[4];
	for (int k = 0; k < 4; ++k) {
		d_ang[k] = (int16_t)(uint16_t)(p->curr_ang[k] - p->prev_ang[k]);
		if (d_ang[k] > PRESENT_MAX_ANGLE_DELTA || d_ang[k] < -PRESENT_MAX_ANGLE_DELTA) {
			angles_ok = 0;
		}
	}
	if (angles_ok) {
		uint16_t* out[4] = { &f->yaw, &f->pitch, &f->roll, &f->angle_d };
		for (int k = 0; k < 4; ++k) {
			*out[k] = (uint16_t)(p->prev_ang[k] + (int)lrintf((float)d_ang[k] * alpha));
		}
	}
	const int16_t d_spin = (int16_t)(uint16_t)((uint16_t)p->curr_spin - (uint16_t)p->prev_spin);
	if (d_spin <= PRESENT_MAX_ANGLE_DELTA && d_spin >= -PRESENT_MAX_ANGLE_DELTA) {
		f->spin_angle = (int16_t)(uint16_t)((uint16_t)p->prev_spin + (int)lrintf((float)d_spin * alpha));
	}
}

/* ---- camera ------------------------------------------------------------ */

static void present_camera(XwaSnapshot* s, float alpha) {
	XwaFlightCamera* c = &s->flight_camera;
	PresentCamera* k = &s_cam;
	const int same_mode = k->valid && k->region == c->region && k->external == c->external &&
						  k->map_mode == c->map_mode && k->in_hangar == c->in_hangar &&
						  k->focus == c->focus_obj_idx;
	if (k->serial != s_serial || !same_mode) {
		const int continues = same_mode && k->serial != s_serial && k->serial + 1u == s_serial;
		if (continues) {
			memcpy(k->prev_pos, k->curr_pos, sizeof k->prev_pos);
			memcpy(k->prev_rows, k->curr_rows, sizeof k->prev_rows);
		} else {
			memcpy(k->prev_pos, c->world_pos, sizeof k->prev_pos);
			memcpy(k->prev_rows, c->rows, sizeof k->prev_rows);
		}
		k->serial = s_serial;
		k->valid = 1;
		k->region = c->region;
		k->external = c->external;
		k->map_mode = c->map_mode;
		k->in_hangar = c->in_hangar;
		k->focus = c->focus_obj_idx;
	}
	memcpy(k->curr_pos, c->world_pos, sizeof k->curr_pos);
	memcpy(k->curr_rows, c->rows, sizeof k->curr_rows);
	if (alpha >= 1.0f) {
		return;
	}

	/* In the cockpit the eye rides on its craft: carry it rigidly with the
	 * craft's shown pose, so cockpit, craft and eye never drift apart. */
	const int32_t anchor_slot = c->in_hangar ? c->player_obj_idx : c->focus_obj_idx;
	const PresentSlot* anchor = NULL;
	if (!c->external && !c->map_mode && anchor_slot == c->player_obj_idx && anchor_slot >= 0 &&
		(uint32_t)anchor_slot < s_slot_cap) {
		const PresentSlot* p = &s_slots[anchor_slot];
		if (p->valid && p->serial == s_serial) {
			anchor = p;
		}
	}
	if (anchor) {
		if (!anchor->moved) {
			return;
		}
		slot_carry_i32(anchor, c->world_pos);
		if (anchor->rotated) {
			float rows[9];
			for (int r = 0; r < 3; ++r) {
				slot_rotate(anchor, c->rows + r * 3, rows + r * 3);
			}
			memcpy(c->rows, rows, sizeof rows);
			if (s->cockpit_valid && anchor_slot == c->player_obj_idx) {
				XwaCockpit* cockpit = &s->cockpit;
				float v[3];
				slot_rotate(anchor, cockpit->hardpoint_world, v);
				memcpy(cockpit->hardpoint_world, v, sizeof v);
				slot_rotate(anchor, cockpit->camera_pan, v);
				memcpy(cockpit->camera_pan, v, sizeof v);
			}
		}
		return;
	}

	int32_t pos[3];
	if (blend_pos(k->prev_pos, k->curr_pos, alpha, pos)) {
		memcpy(c->world_pos, pos, sizeof pos);
	}
	float prev_rows[9], curr_rows[9], shown[9];
	memcpy(prev_rows, k->prev_rows, sizeof prev_rows);
	memcpy(curr_rows, k->curr_rows, sizeof curr_rows);
	if (memcmp(prev_rows, curr_rows, sizeof prev_rows) != 0 && orthonormalize(prev_rows) &&
		orthonormalize(curr_rows) && blend_basis(prev_rows, curr_rows, alpha, shown)) {
		memcpy(c->rows, shown, sizeof shown);
	}
}

/* ---- the frame ------------------------------------------------------------ */

void XwaSnapshotPresent_Apply(XwaSnapshot* s, int32_t game_time, int enabled) {
	if (!enabled || !s) {
		if (s_have_step) {
			XwaSnapshotPresent_Reset();
		}
		return;
	}
	if (s_have_step && game_time < s_step_time) {
		XwaSnapshotPresent_Reset(); /* new session, or the clock was put back */
	}
	if (!s_have_step || game_time != s_step_time) {
		s_have_prev_step = s_have_step;
		s_prev_step_time = s_step_time;
		s_step_time = game_time;
		s_have_step = 1;
		++s_serial;
	}
	const float alpha = present_alpha();

	for (uint32_t i = 0; i < s->flight_object_count; ++i) {
		present_object(&s->flight_objects[i], alpha);
	}

	/* Effects and trails attached to an object follow its shown pose. */
	for (uint32_t i = 0; i < s->particle_effect_count; ++i) {
		XwaParticleEffect* e = &s->particle_effects[i];
		if (e->source_kind != XWA_PARTICLE_SOURCE_OBJECT) {
			continue;
		}
		const PresentSlot* owner = moved_owner(e->owner_slot, e->owner_signature);
		if (!owner) {
			continue;
		}
		slot_carry_point(owner, &e->emitter_world_pos);
		for (uint32_t j = 0; j < e->particle_count && e->first_particle + j < s->particle_count; ++j) {
			XwaParticle* particle = &s->particles[e->first_particle + j];
			slot_carry_point(owner, &particle->world_pos);
			if (e->billboard_mode != XWA_PARTICLE_BILLBOARD_FACING) {
				slot_carry_point(owner, &particle->tail_world_pos);
			}
		}
	}
	for (uint32_t i = 0; i < s->trail_emitter_count; ++i) {
		const XwaTrailEmitter* t = &s->trail_emitters[i];
		const PresentSlot* owner = moved_owner(t->owner_slot, t->owner_signature);
		if (owner && t->point_count > 0 && t->first_point < s->trail_point_count) {
			/* only the newest point rides on the emitter; older ones stay
			 * where they were laid down */
			slot_carry_i32(owner, s->trail_points[t->first_point].world_pos);
		}
	}

	if (s->flight_camera_valid) {
		present_camera(s, alpha);
	}
}
