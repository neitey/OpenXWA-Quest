#include "xwa_runtime/snapshot/snapshot_present.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- tuning ------------------------------------------------------------
 * A larger move between two steps is a cut (respawn, hyperspace, slot
 * reuse), and so is a larger turn: those show the newer pose. */
#define PRESENT_TELEPORT_UNITS 0x40000
#define PRESENT_MIN_AXIS_DOT 0.9f
#define PRESENT_MAX_ANGLE_DELTA 0x2000 /* Q16: 45 degrees */
#define PRESENT_GAIN 0.05
#define PRESENT_NOMINAL_TICKS_PER_US (1.0 / 4000.0) /* the sim clock: 4 ms ticks */
#define PRESENT_MIN_LAG 2.0
#define PRESENT_MAX_LAG 48.0 /* ticks: at most ~190 ms behind the newest step */
enum {
	PRESENT_HISTORY = 8, /* steps kept per object: the shown time may lag several */
	PRESENT_ARRIVALS = 32,
	PRESENT_SLOT_GROW = 256,
	PRESENT_PAUSE_US = 500000, /* no step for this long: hold the newest */
};

typedef struct PresentPose {
	uint32_t serial; /* step this pose belongs to; 0 = empty */
	uint16_t signature;
	int16_t spin;
	int32_t pos[3];
	int16_t rows[9];
	uint16_t ang[4];
	int32_t prev_pos[3]; /* MobileObject.prevWorld* (spawn: where it moved from) */
} PresentPose;

typedef struct PresentSlot {
	PresentPose hist[PRESENT_HISTORY];
	/* This frame's shown pose, for things attached to the object. */
	uint32_t serial; /* frame data below is for this step */
	uint16_t signature;
	uint8_t moved;	 /* shown differs from the newest pose */
	uint8_t rotated; /* ... including its orientation */
	int32_t curr_pos[3], shown_pos[3];
	float curr_axes[9], shown_axes[9];
} PresentSlot;

typedef struct PresentCameraPose {
	uint32_t serial;
	uint8_t region, external, map_mode, in_hangar;
	int32_t focus;
	int32_t pos[3];
	float rows[9];
} PresentCameraPose;

static PresentSlot* s_slots;
static uint32_t s_slot_cap;
static PresentCameraPose s_cam[PRESENT_HISTORY];
static uint32_t s_serial; /* the newest step; serials start at 1 */
static int32_t s_step_time[PRESENT_HISTORY];
static uint32_t s_steps_kept; /* steps since the last reset, up to PRESENT_HISTORY */
static int s_have_step;

/* ---- presentation clock ----------------------------------------------------
 * In sim ticks, advanced by host time at the measured sim speed, and pulled
 * gently toward a lag behind the newest step that covers the gaps between
 * step arrivals seen lately. Multiplayer steps arrive unevenly: every
 * frame or two, then a stall while a peer's input or the host's world
 * message is on its way; the lag keeps the shown time moving through them. */
static struct {
	int valid;
	double present;
	double lag;		 /* ticks behind the newest step aimed for */
	double gap_peak; /* recent largest sim time between two step arrivals */
	uint64_t last_us;
	uint64_t arrival_us[PRESENT_ARRIVALS];
	int32_t arrival_time[PRESENT_ARRIVALS];
	uint32_t arrivals;
} s_clock;

static XwaSnapshotPresentStats s_stats;

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
	s_steps_kept = 0;
	s_clock.valid = 0;
	s_clock.arrivals = 0;
	s_clock.gap_peak = 0.0;
	memset(s_cam, 0, sizeof s_cam);
	if (s_slots) {
		memset(s_slots, 0, sizeof *s_slots * s_slot_cap);
	}
}

void XwaSnapshotPresent_TakeStats(XwaSnapshotPresentStats* out) {
	if (out) {
		*out = s_stats;
	}
	memset(&s_stats, 0, sizeof s_stats);
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

static int32_t step_time(uint32_t serial) { return s_step_time[serial % PRESENT_HISTORY]; }

/* Sim ticks per host microsecond over the recent arrivals. */
static double clock_speed(void) {
	const uint32_t n = s_clock.arrivals < PRESENT_ARRIVALS ? s_clock.arrivals : PRESENT_ARRIVALS;
	if (n >= 2) {
		const uint32_t newest = (s_clock.arrivals - 1) % PRESENT_ARRIVALS;
		const uint32_t oldest = (s_clock.arrivals - n) % PRESENT_ARRIVALS;
		const double us = (double)(s_clock.arrival_us[newest] - s_clock.arrival_us[oldest]);
		const double ticks = (double)(s_clock.arrival_time[newest] - s_clock.arrival_time[oldest]);
		if (us >= 250000.0 && ticks > 0.0) {
			const double speed = ticks / us;
			/* sane: the sim clock runs 0.25x .. 2x */
			if (speed > PRESENT_NOMINAL_TICKS_PER_US * 0.25 && speed < PRESENT_NOMINAL_TICKS_PER_US * 2.0) {
				return speed;
			}
		}
	}
	return PRESENT_NOMINAL_TICKS_PER_US;
}

/* Advances the clock for this host frame; returns the shown sim time. */
static double clock_advance(int new_step, uint64_t host_us) {
	const double newest = (double)step_time(s_serial);
	uint64_t dt_us = s_clock.valid && host_us > s_clock.last_us ? host_us - s_clock.last_us : 0;
	if (dt_us > 100000) {
		dt_us = 100000;
	}
	s_clock.last_us = host_us;
	if (new_step) {
		const uint32_t i = s_clock.arrivals % PRESENT_ARRIVALS;
		const uint64_t prev_us = s_clock.arrivals ? s_clock.arrival_us[(s_clock.arrivals - 1) % PRESENT_ARRIVALS] : 0;
		s_clock.arrival_us[i] = host_us;
		s_clock.arrival_time[i] = step_time(s_serial);
		s_clock.arrivals++;
		if (s_clock.arrivals >= 2 && host_us > prev_us) {
			/* the sim time the screen uses up before the next step can arrive */
			const double gap = (double)(host_us - prev_us) * clock_speed();
			s_clock.gap_peak = gap > s_clock.gap_peak ? gap : s_clock.gap_peak * 0.97 + gap * 0.03;
			if (gap > s_stats.max_gap_ticks) {
				s_stats.max_gap_ticks = gap;
			}
		}
	}
	double lag = s_clock.gap_peak * 1.15 + 1.0;
	lag = lag < PRESENT_MIN_LAG ? PRESENT_MIN_LAG : lag > PRESENT_MAX_LAG ? PRESENT_MAX_LAG : lag;
	s_clock.lag = lag;

	const double target = newest - lag;
	if (!s_clock.valid) {
		s_clock.valid = 1;
		s_clock.present = target;
	} else if (s_clock.arrivals &&
			   host_us - s_clock.arrival_us[(s_clock.arrivals - 1) % PRESENT_ARRIVALS] > PRESENT_PAUSE_US) {
		s_clock.present = newest; /* paused: hold, resume from the newest step */
	} else {
		s_clock.present += (double)dt_us * clock_speed();
		const double error = target - s_clock.present;
		const double snap = lag * 2.0 > 30.0 ? lag * 2.0 : 30.0;
		if (error > snap || error < -snap) {
			s_clock.present = target;
		} else {
			s_clock.present += error * PRESENT_GAIN;
		}
	}
	if (s_clock.present >= newest) {
		s_clock.present = newest;
		s_stats.held_frames++; /* caught up with the newest step: motion stops */
	}
	s_stats.frames++;
	s_stats.lag_ticks_sum += newest - s_clock.present;
	return s_clock.present;
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

/* a -> b at alpha; 0 when the turn is too large to be motion. */
static int blend_basis(const float a[9], const float b[9], float alpha, float out[9]) {
	for (int r = 0; r < 3; ++r) {
		if (dot3(a + r * 3, b + r * 3) < PRESENT_MIN_AXIS_DOT) {
			return 0;
		}
	}
	for (int k = 0; k < 9; ++k) {
		out[k] = a[k] + (b[k] - a[k]) * alpha;
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

static int blend_pos(const int32_t a[3], const int32_t b[3], float alpha, int32_t out[3]) {
	for (int k = 0; k < 3; ++k) {
		if (llabs((long long)b[k] - a[k]) > PRESENT_TELEPORT_UNITS) {
			return 0;
		}
	}
	for (int k = 0; k < 3; ++k) {
		const double delta = (double)b[k] - (double)a[k];
		out[k] = (int32_t)((long long)a[k] + llround(delta * alpha));
	}
	return 1;
}

static void blend_angle(uint16_t a, uint16_t b, float alpha, uint16_t* out) {
	const int16_t d = (int16_t)(uint16_t)(b - a);
	*out = (uint16_t)(a + (int)lrintf((float)d * alpha));
}

static int angles_close(uint16_t a, uint16_t b) {
	const int16_t d = (int16_t)(uint16_t)(b - a);
	return d <= PRESENT_MAX_ANGLE_DELTA && d >= -PRESENT_MAX_ANGLE_DELTA;
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
	return p->moved && p->serial == s_serial && p->signature == signature ? p : NULL;
}

/* ---- per object --------------------------------------------------------- */

static const PresentPose* slot_pose(const PresentSlot* p, uint32_t serial, uint16_t signature) {
	const PresentPose* pose = &p->hist[serial % PRESENT_HISTORY];
	return serial && pose->serial == serial && pose->signature == signature ? pose : NULL;
}

/* Shows f at step `older` + alpha toward `older + 1`. */
static void present_object(XwaFlightObject* f, uint32_t older, float alpha) {
	PresentSlot* p = present_slot(f->slot);
	if (!p) {
		return;
	}
	PresentPose* now = &p->hist[s_serial % PRESENT_HISTORY];
	now->serial = s_serial;
	now->signature = f->signature;
	memcpy(now->pos, f->world_pos, sizeof now->pos);
	memcpy(now->prev_pos, f->has_mobj ? f->prev_world_pos : f->world_pos, sizeof now->prev_pos);
	memcpy(now->rows, f->rows, sizeof now->rows);
	now->ang[0] = f->yaw;
	now->ang[1] = f->pitch;
	now->ang[2] = f->roll;
	now->ang[3] = f->angle_d;
	now->spin = f->spin_angle;

	p->serial = s_serial;
	p->signature = f->signature;
	p->moved = 0;
	p->rotated = 0;
	memcpy(p->curr_pos, f->world_pos, sizeof p->curr_pos);
	memcpy(p->shown_pos, f->world_pos, sizeof p->shown_pos);
	rows_to_axes(f->rows, p->curr_axes);
	memcpy(p->shown_axes, p->curr_axes, sizeof p->shown_axes);

	/* Tunnel slots are static-world proxies recycled along the trench. */
	if (older >= s_serial || f->genus == XWA_SNAP_GENUS_DS_TUNNEL) {
		return;
	}
	const PresentPose* b = slot_pose(p, older + 1, f->signature);
	const PresentPose* a = slot_pose(p, older, f->signature);
	PresentPose spawn;
	if (!b) {
		/* It appeared after the shown time: until that comes, hold it where
		 * it came from (a laser at the gun), not where it already flew to. */
		for (uint32_t j = older + 2; j <= s_serial; ++j) {
			const PresentPose* first = slot_pose(p, j, f->signature);
			if (first) {
				int32_t from[3];
				memcpy(f->world_pos, blend_pos(first->prev_pos, first->pos, 0.0f, from) ? from : first->pos,
					   sizeof f->world_pos);
				memcpy(p->shown_pos, f->world_pos, sizeof p->shown_pos);
				p->moved = memcmp(p->shown_pos, p->curr_pos, sizeof p->shown_pos) != 0;
				break;
			}
		}
		return;
	}
	if (!a) {
		/* New in step older+1: start from where the engine says it moved
		 * from (a launch point for projectiles); hold the turn. */
		spawn = *b;
		memcpy(spawn.pos, b->prev_pos, sizeof spawn.pos);
		a = &spawn;
	}

	int32_t pos[3];
	if (blend_pos(a->pos, b->pos, alpha, pos)) {
		memcpy(f->world_pos, pos, sizeof f->world_pos);
	} else {
		memcpy(f->world_pos, b->pos, sizeof f->world_pos);
	}
	memcpy(p->shown_pos, f->world_pos, sizeof p->shown_pos);
	p->moved = memcmp(p->shown_pos, p->curr_pos, sizeof p->shown_pos) != 0;

	float a_axes[9], b_axes[9], shown[9], curr[9];
	rows_to_axes(a->rows, a_axes);
	rows_to_axes(b->rows, b_axes);
	memcpy(curr, p->curr_axes, sizeof curr);
	if (orthonormalize(a_axes) && orthonormalize(b_axes) && orthonormalize(curr) &&
		blend_basis(a_axes, b_axes, alpha, shown)) {
		for (int k = 0; k < 9; ++k) {
			f->rows[k] = axis_to_q15(shown[k]);
		}
		/* attached things rotate with the exact bases */
		memcpy(p->curr_axes, curr, sizeof p->curr_axes);
		memcpy(p->shown_axes, shown, sizeof p->shown_axes);
		if (memcmp(f->rows, now->rows, sizeof f->rows) != 0) {
			p->moved = 1;
			p->rotated = 1;
		}
	} else {
		memcpy(f->rows, b->rows, sizeof f->rows);
	}

	/* The Euler path (orient-dirty objects, projectiles, debris spin). */
	int angles_ok = 1;
	for (int k = 0; k < 4; ++k) {
		angles_ok &= angles_close(a->ang[k], b->ang[k]);
	}
	uint16_t* out[4] = { &f->yaw, &f->pitch, &f->roll, &f->angle_d };
	for (int k = 0; k < 4; ++k) {
		if (angles_ok) {
			blend_angle(a->ang[k], b->ang[k], alpha, out[k]);
		} else {
			*out[k] = b->ang[k];
		}
	}
	if (angles_close((uint16_t)a->spin, (uint16_t)b->spin)) {
		uint16_t spin;
		blend_angle((uint16_t)a->spin, (uint16_t)b->spin, alpha, &spin);
		f->spin_angle = (int16_t)spin;
	} else {
		f->spin_angle = b->spin;
	}
	if (f->yaw != now->ang[0] || f->pitch != now->ang[1] || f->roll != now->ang[2] || f->angle_d != now->ang[3] ||
		f->spin_angle != now->spin) {
		p->moved = 1;
	}
}

/* ---- camera ------------------------------------------------------------ */

static int camera_same_mode(const PresentCameraPose* a, const XwaFlightCamera* c) {
	return a->region == c->region && a->external == c->external && a->map_mode == c->map_mode &&
		   a->in_hangar == c->in_hangar && a->focus == c->focus_obj_idx;
}

static void present_camera(XwaSnapshot* s, uint32_t older, float alpha) {
	XwaFlightCamera* c = &s->flight_camera;
	PresentCameraPose* now = &s_cam[s_serial % PRESENT_HISTORY];
	now->serial = s_serial;
	now->region = c->region;
	now->external = c->external;
	now->map_mode = c->map_mode;
	now->in_hangar = c->in_hangar;
	now->focus = c->focus_obj_idx;
	memcpy(now->pos, c->world_pos, sizeof now->pos);
	memcpy(now->rows, c->rows, sizeof now->rows);
	if (older >= s_serial) {
		return;
	}

	/* In the cockpit the eye rides on its craft: carry it rigidly with the
	 * craft's shown pose, so cockpit, craft and eye never drift apart. */
	const int32_t anchor_slot = c->in_hangar ? c->player_obj_idx : c->focus_obj_idx;
	if (!c->external && !c->map_mode && anchor_slot == c->player_obj_idx && anchor_slot >= 0 &&
		(uint32_t)anchor_slot < s_slot_cap && s_slots[anchor_slot].serial == s_serial) {
		const PresentSlot* anchor = &s_slots[anchor_slot];
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
			if (s->cockpit_valid) {
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

	const PresentCameraPose* a = &s_cam[older % PRESENT_HISTORY];
	const PresentCameraPose* b = &s_cam[(older + 1) % PRESENT_HISTORY];
	if (a->serial != older || b->serial != older + 1 || !camera_same_mode(a, c) || !camera_same_mode(b, c)) {
		return;
	}
	int32_t pos[3];
	if (blend_pos(a->pos, b->pos, alpha, pos)) {
		memcpy(c->world_pos, pos, sizeof pos);
	}
	float ra[9], rb[9], shown[9];
	memcpy(ra, a->rows, sizeof ra);
	memcpy(rb, b->rows, sizeof rb);
	if (orthonormalize(ra) && orthonormalize(rb) && blend_basis(ra, rb, alpha, shown)) {
		memcpy(c->rows, shown, sizeof shown);
	}
}

/* ---- the frame ------------------------------------------------------------ */

void XwaSnapshotPresent_Apply(XwaSnapshot* s, int32_t game_time, uint64_t host_us, int enabled) {
	if (!enabled || !s) {
		if (s_have_step) {
			XwaSnapshotPresent_Reset();
		}
		return;
	}
	if (s_have_step && game_time < step_time(s_serial)) {
		XwaSnapshotPresent_Reset(); /* new session, or the clock was put back */
	}
	const int new_step = !s_have_step || game_time != step_time(s_serial);
	if (new_step) {
		++s_serial;
		if (s_serial == 0) {
			s_serial = 1;
		}
		s_step_time[s_serial % PRESENT_HISTORY] = game_time;
		s_have_step = 1;
		if (s_steps_kept < PRESENT_HISTORY) {
			s_steps_kept++;
		}
		s_stats.steps++;
	}

	/* The two kept steps around the shown time. */
	const double shown = clock_advance(new_step, host_us);
	uint32_t older = s_serial; /* = newest: no blending */
	float alpha = 1.0f;
	{
		const uint32_t kept = s_steps_kept;
		for (uint32_t back = 1; back < kept; ++back) {
			const uint32_t k = s_serial - back;
			const double t0 = (double)step_time(k);
			const double t1 = (double)step_time(k + 1);
			if (t1 <= t0) {
				break;
			}
			if (shown >= t0 || back + 1 == kept) {
				older = k;
				alpha = (float)((shown - t0) / (t1 - t0));
				alpha = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
				break;
			}
		}
		if (older < s_serial && alpha >= 1.0f && older + 1 == s_serial) {
			older = s_serial;
		}
	}

	for (uint32_t i = 0; i < s->flight_object_count; ++i) {
		present_object(&s->flight_objects[i], older, alpha);
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
		present_camera(s, older, alpha);
	}
}
