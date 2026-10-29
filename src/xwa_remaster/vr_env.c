#include "xwa_remaster/vr_env.h"
#include "xwa_runtime/runtime/vr_buttons.h"

#include "aeron/aeron.h"
#include "aeron/asset/opt_model.h"
#include "aeron/audio.h"
#include "aeron/log.h"
#include "aeron/scene/billboard.h"
#include "aeron/scene/draw_list2d.h"
#include "aeron/scene/font_atlas.h"
#include "aeron/scene/text2d.h"
#include "aeron/scene/mesh.h"
#include "aeron/vfs.h"
#include "aeron/xr.h"
#include "xwa/assets/model_type.h"
#include "xwa/assets/object_type.h"
#include "xwa/config/game_config.h"
#include "xwa/flight/fediskio.h"
#include "xwa/frontend/frontend_input.h"
#include "xwa/frontend/frontend_mission.h"
#include "xwa/frontend/mission_debrief.h"
#include "xwa_2d.h"
#include "xwa_remaster/ship.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A prop of the classic carrier hangar (Hangar_EnterHangar's default
 * layout, highest detail level): position relative to the launch
 * reference, Q16 yaw and pitch relative to it. */
#define ENV_ON_FLOOR 0x7fffffff

typedef struct EnvProp {
	const char* opt;
	int x, y, z;
	int yaw, pitch;
} EnvProp;

static const EnvProp k_props[] = {
	{ "HangarMonitor", -1059, 1313, ENV_ON_FLOOR, 38600, 0 },
	{ "HangarCrane", 725, -416, ENV_ON_FLOOR, 23500, 0 },
	{ "HangarGenerator", -1352, 1440, ENV_ON_FLOOR, 536, 0 },
	{ "HangarWorkstand", -1065, -56, ENV_ON_FLOOR, 47736, 0 },
	{ "HangarWorkstand", 2079, 733, ENV_ON_FLOOR, 6400, 0 },
	{ "HangarWorkstand", -2488, 1215, ENV_ON_FLOOR, 11864, 0 },
	{ "HangarCrate", 794, -750, -848, 6800, 12500 },
	{ "HangarCrate", -803, -1464, ENV_ON_FLOOR, 49200, 0 },
	{ "HangarCrate", -1563, 944, ENV_ON_FLOOR, 51436, 0 },
	{ "ContainerBox", -506, -2136, -722, 32836, 16480 },
	{ "ContainerBox", -841, -1628, ENV_ON_FLOOR, 53100, 0 },
	{ "ContainerBox", 1240, -2098, -722, 272, 16480 },
	{ "ContainerSphere", -834, -2034, ENV_ON_FLOOR, 32336, 0 },
	{ "CargoCanister", -3474, -275, ENV_ON_FLOOR, 29436, 0 },
	{ "Ywing", -1038, -798, ENV_ON_FLOOR, 7700, 0 },
	{ "Shuttle", 1146, -1288, -741, 50536, 0 },
	{ "HangarWorkstand", 820, -665, ENV_ON_FLOOR, 49136, 0 },
	{ "HangarCrate", -873, -1456, -849, 60472, 20400 },
	{ "HangarCrate", -3095, -592, ENV_ON_FLOOR, 51436, 0 },
	{ "ContainerBox", -1751, -1996, ENV_ON_FLOOR, 31172, 0 },
	{ "ContainerBox", 979, -2103, ENV_ON_FLOOR, 37036, 0 },
	{ "ContainerSphere", -1374, -2068, ENV_ON_FLOOR, 40536, 0 },
	{ "CargoCanister", 2806, 1357, ENV_ON_FLOOR, 20536, 0 },
	{ "CargoCanister", -195, -1910, ENV_ON_FLOOR, 54372, 0 },
	{ "Awing", 2178, 506, -830, 40036, 1500 },
	{ "Ywing", 1100, -503, ENV_ON_FLOOR, 49036, 0 },
	{ "Xwing", -1003, 280, -825, 16600, 0 },
	{ "HangarDroid", 227, 351, ENV_ON_FLOOR, 58736, 0 },
	{ "HangarDroid2", -70, 400, ENV_ON_FLOOR, 0, 0 },
	{ "Shuttle", 1127, 959, -741, 43136, 0 },
	{ "HangarRoofCrane", -1400, 786, -282, 0, 0 },
};

#define ENV_PROP_COUNT ((int)(sizeof k_props / sizeof k_props[0]))

/* ---- "Battle of Endor": the box-art scene ---------------------------
 * A still frame loosely after the X-Wing Alliance cover: the Falcon below
 * left, the second Death Star upper right over Endor's rim, the rebel
 * fleet on the left with a cruiser bursting under the superlaser, Star
 * Destroyers before the station, X-wings diving in from the lower right.
 * A B-wing (left) and two TIE interceptors (right) sit just outside the
 * forward view. Directions: yaw (degrees, + right of the screen), pitch
 * (+ up), distance in metres from the viewer; orientation as the craft's
 * nose and up directions (world: x right, y forward, z up). */
typedef struct EnvShip {
	const char* opt; /* OPT name; alternatives separated by '|' */
	float yaw, pitch, dist;
	float nose[3], up[3];
	int aim; /* 1-based index (same scene) the nose points at (0 = use nose) */
	/* 1: nose and up are as the viewer sees the craft (x right, y away
	 * along the line of sight to it, z up), so it keeps the look it has
	 * on a picture wherever around the viewer it is placed. */
	int view;
} EnvShip;

#define ENV_DS_YAW 36.0f
#define ENV_DS_PITCH 21.0f
#define ENV_DS_NOSE { 0.5488f, 0.7553f, 0.3584f } /* toward the Death Star */

static const EnvShip k_endor[] = {
	{ "MilleniumFalcon2", -33.0f, -24.0f, 36.0f, { 0.62f, 0.72f, 0.30f }, { -0.35f, -0.25f, 0.9f } },
	/* X-wings and TIEs in a dogfight: each nose on its target. */
	{ "Xwing", 38.0f, -30.0f, 40.0f, ENV_DS_NOSE, { 0.2f, -0.4f, 0.9f }, 5 },
	{ "Xwing", 46.0f, -22.0f, 65.0f, ENV_DS_NOSE, { -0.3f, -0.3f, 0.9f }, 19 },
	{ "Xwing", 30.0f, -34.0f, 85.0f, ENV_DS_NOSE, { 0.0f, -0.3f, 0.95f }, 6 },
	{ "TieFighter", 44.0f, 4.0f, 160.0f, { -0.6f, -0.7f, -0.2f }, { 0.0f, 0.0f, 1.0f }, 2 },
	{ "TieFighter", 52.0f, 12.0f, 210.0f, { -0.5f, -0.8f, -0.1f }, { 0.2f, 0.0f, 1.0f }, 4 },
	{ "TieFighter", 20.0f, 32.0f, 260.0f, { -0.4f, -0.8f, -0.3f }, { 0.0f, 0.1f, 1.0f } },
	/* The Falcon's target, below the screen, firing back. */
	{ "TieFighter", -10.0f, -36.0f, 110.0f, { -0.4f, -0.9f, 0.1f }, { 0.0f, 0.0f, 1.0f }, 1 },
	{ "ImperialStarDestroyer2", 27.0f, 12.0f, 10000.0f, { -0.8f, -0.5f, -0.1f }, { 0.0f, 0.0f, 1.0f } },
	{ "ImperialStarDestroyer2", 47.0f, 16.0f, 13000.0f, { -0.8f, -0.55f, -0.05f }, { 0.05f, 0.0f, 1.0f } },
	{ "CalamariCruiserNew", -44.0f, 8.0f, 6000.0f, { 0.8f, 0.55f, 0.1f }, { 0.0f, 0.0f, 1.0f } },
	{ "CalamariWinged", -58.0f, 18.0f, 8000.0f, { 0.85f, 0.5f, 0.05f }, { 0.0f, 0.0f, 1.0f } },
	{ "Frigate2", -40.0f, -6.0f, 3000.0f, { 0.8f, 0.6f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
	{ "Corvette2", -50.0f, -12.0f, 1500.0f, { 0.7f, 0.7f, 0.1f }, { 0.0f, 0.0f, 1.0f } },
	/* Hit by the superlaser (ENV_BURST). */
	{ "CalamariCruiserNew", -38.0f, 31.0f, 7000.0f, { 0.7f, 0.7f, -0.1f }, { 0.0f, 0.0f, 1.0f } },
	/* Out of the forward view: turn left / right. */
	{ "Bwing", -85.2f, -2.4f, 24.1f, { 0.15f, 1.0f, 0.05f }, { 0.9f, 0.0f, 0.4f } },
	{ "TieInterceptor", 92.6f, 5.2f, 22.1f, { -0.2f, 1.0f, 0.1f }, { 0.0f, 0.0f, 1.0f } },
	{ "TieInterceptor", 76.0f, 13.6f, 29.7f, { -0.2f, 1.0f, 0.15f }, { -0.2f, 0.0f, 1.0f } },
	/* Hit by the second X-wing: half inside its explosion (ENV_VICTIM). */
	{ "TieFighter", 50.0f, -8.0f, 85.0f, { -0.3f, -0.5f, 0.2f }, { 0.3f, 0.0f, 1.0f } },
};
#define ENV_ENDOR_COUNT ((int)(sizeof k_endor / sizeof k_endor[0]))
#define ENV_FALCON 0
#define ENV_FALCON_TARGET 7
#define ENV_BURST 14
#define ENV_VICTIM 18

/* Frozen laser salvos: shooter -> target (indices in the scene's ship
 * list) from the shooter's guns, as spans of the shooter-target distance
 * along its nose. */
typedef enum EnvGuns {
	ENV_GUNS_XWING,       /* four wingtip cannons, red */
	ENV_GUNS_TIE,         /* two chin guns, green */
	ENV_GUNS_AWING,       /* two wingtip cannons, red */
	ENV_GUNS_INTERCEPTOR, /* TIE Interceptor: two wingtip guns, green */
	ENV_GUNS_BWING,       /* B-wing: the wing's cannons, red */
} EnvGuns;

typedef struct EnvSalvo {
	int shooter, target;
	EnvGuns guns;
	float spans[2][2];
	int span_count;
} EnvSalvo;

static const EnvSalvo k_salvos[] = {
	{ 1, 4, ENV_GUNS_XWING, { { 0.20f, 0.32f } }, 1 },
	{ 2, ENV_VICTIM, ENV_GUNS_XWING, { { 0.25f, 0.40f } }, 1 },
	{ 3, 5, ENV_GUNS_XWING, { { 0.20f, 0.30f } }, 1 },
	{ 4, 1, ENV_GUNS_TIE, { { 0.15f, 0.25f }, { 0.40f, 0.50f } }, 2 },
	{ 5, 3, ENV_GUNS_TIE, { { 0.15f, 0.25f }, { 0.40f, 0.50f } }, 2 },
	{ 7, 0, ENV_GUNS_TIE, { { 0.20f, 0.30f }, { 0.45f, 0.55f } }, 2 },
};

/* ---- "X-Wing": the X-Wing box art -------------------------------------
 * The cover of the original X-Wing and only what is on it: three X-wings
 * and three TIE fighters over a purple nebula, a fourth fighter bursting
 * upper left, red X-wing and green TIE fire. Each craft keeps the attitude
 * it has on the cover as the viewer sees it (view-relative nose / up: x
 * right, y away along the line of sight, z up), and keeps its place on the
 * cover, pushed out from the middle just far enough to clear the menu
 * screen (about 21 degrees either side, 19 below to 17 above, the logo
 * to 28 above within 12 either side). */
static const EnvShip k_boxart[] = {
	/* 0: lower right, the big one: from behind, engines toward us, banked
	 * with its wings rising to the right, climbing away to the upper left. */
	{ "Xwing", 36.0f, -24.0f, 24.0f, { -0.25f, 0.93f, 0.27f }, { -0.55f, -0.20f, 0.80f }, 0, 1 },
	/* 1: middle left: flying right and away, rolled near knife edge (upper
	 * wings to the upper left), its top and engines toward us. */
	{ "Xwing", -36.0f, -3.0f, 30.0f, { 0.55f, 0.80f, 0.0f }, { 0.68f, -0.47f, 0.40f }, 0, 1 },
	/* 2: upper left, by the burst: flying right, from its rear left quarter,
	 * banked right wing up. */
	{ "Xwing", -34.0f, 34.0f, 70.0f, { 0.75f, 0.60f, 0.15f }, { -0.35f, -0.25f, 0.90f }, 0, 1 },
	/* 3: upper middle (left of the logo): coming at us three-quarters
	 * from the front left, cockpit toward us, the wings seen obliquely
	 * with their axis rising to the right; firing to the upper left. */
	{ "TieFighter", -19.0f, 24.0f, 60.0f, { -0.65f, -0.65f, 0.40f }, { -0.09f, 0.59f, 0.80f }, 0, 1 },
	/* 4: right of the middle: three-quarters from the front left, window
	 * toward us, the wing axis falling steeply to the right; firing to the
	 * lower left. */
	{ "TieFighter", 30.0f, 7.0f, 52.0f, { -0.60f, -0.75f, -0.25f }, { 0.53f, -0.62f, 0.57f }, 0, 1 },
	/* 5: at the cover's right edge, mostly cut off there: as TIE 4. */
	{ "TieFighter", 52.0f, 4.0f, 80.0f, { -0.60f, -0.75f, -0.25f }, { 0.53f, -0.62f, 0.57f }, 0, 1 },
};

/* Where the fighter bursts, upper left by X-wing 2: yaw, pitch, metres. */
static const float k_box_burst[3] = { -21.0f, 36.0f, 72.0f };

static const EnvSalvo k_box_salvos[] = {
	/* The big X-wing's fire, away into the scene. */
	{ 0, 5, ENV_GUNS_XWING, { { 0.15f, 0.27f }, { 0.45f, 0.57f } }, 2 },
	/* X-wing 1's fire to the right. */
	{ 1, 4, ENV_GUNS_XWING, { { 0.15f, 0.28f } }, 1 },
	/* The two long green streaks to the upper left. */
	{ 3, 2, ENV_GUNS_TIE, { { 0.08f, 0.40f } }, 1 },
	/* TIE 4's fire to the left. */
	{ 4, 1, ENV_GUNS_TIE, { { 0.10f, 0.32f }, { 0.48f, 0.66f } }, 2 },
};

/* ---- the played scenes ----------------------------------------------------
 * The TIE Fighter, Defender of the Empire and Balance of Power box arts play a
 * short fight that ends on the cover's moment (see "the TIE Fighter box art,
 * played"): the same flight, fire, explosion and sound machinery, told per
 * scene which craft dies where, who shoots it, who runs, passes or flies in
 * formation. Ships 0..animated-1 fly in (at most 8); passers fly straight
 * past early on; the rest (capital ships) hold their places. */
typedef struct EnvRunner {
	int ship, chaser;
	float lead_m, break_m, end_s, roll_s, pull_w;
	float away[3];
	int weave; /* jinks instead (see env_tie_weave_pose) */
} EnvRunner;

typedef struct EnvAnim {
	int victim;          /* shot down at the cover's moment */
	const float* blast;  /* its explosion: yaw, pitch, metres, diameter */
	float blast_tint[3]; /* the explosion's colour (sprites as they are: 1.5, 1.4, 1.3) */
	int hunter;          /* who shoots it down (-1: a TIE fighter on the right) */
	int chase_ship;      /* kept in the middle of the view while it chases (-1: none) */
	const EnvRunner* runners;
	int runner_count;
	int first_passer, passers;
	const float (*pass)[2];
	const int* shooters; /* firing bursts along their noses until the salvos */
	int shooter_count;
	const signed char* leader; /* per ship: flies in formation on this one (-1) */
	int animated;
} EnvAnim;

/* ---- "TIE Fighter": the TIE Fighter box art ---------------------------
 * The cover of TIE Fighter (its logo left out): a TIE Interceptor coming at
 * the viewer in front of a huge explosion in which an X-wing breaks up, a
 * second Interceptor upper left and a TIE fighter on the left firing green,
 * a TIE Bomber upper right firing, a small TIE fighter on the right, Darth
 * Vader's helmet looming behind it all over purple clouds. Placed and
 * oriented as on the cover (view-relative), pushed out from the middle just
 * far enough to clear the menu screen and logo; the helmet is the backdrop
 * the screen stands in front of. */
static const EnvShip k_tieart[] = {
	/* 0: the big Interceptor below the screen, coming at us, cockpit window
	 * toward us, seen a little from above, turned slightly to our left. */
	{ "TieInterceptor", -8.0f, -40.0f, 12.0f, { -0.12f, -0.97f, 0.20f }, { 0.10f, -0.25f, 0.96f }, 0, 1 },
	/* 1: Interceptor upper left: three-quarters from the front, wings rising
	 * to the right, firing to the upper left. */
	{ "TieInterceptor", -32.0f, 19.0f, 40.0f, { -0.60f, -0.45f, 0.65f }, { -0.31f, 0.88f, 0.33f }, 0, 1 },
	/* 2: TIE fighter on the left: window to the front left, firing left. */
	{ "TieFighter", -31.0f, -4.0f, 55.0f, { -0.75f, -0.55f, 0.10f }, { -0.08f, 0.27f, 0.89f }, 0, 1 },
	/* 3: TIE Bomber upper right: from the front above, rolled, firing to
	 * the upper right. */
	{ "TieBomber", 36.0f, 16.0f, 28.0f, { 0.30f, -0.60f, 0.75f }, { -0.45f, 0.60f, 0.66f }, 0, 1 },
	/* 4: the small TIE fighter right of the middle, coming at us, its
	 * cockpit window toward us. */
	{ "TieFighter", 28.0f, 2.0f, 90.0f, { 0.28f, -0.94f, -0.18f }, { 0.05f, -0.17f, 0.98f }, 0, 1 },
	/* 5: the X-wing breaking up in the explosion, nose down to the right. */
	{ "Xwing", 18.0f, -32.0f, 45.0f, { 0.55f, -0.35f, -0.75f }, { 0.30f, -0.60f, 0.50f }, 0, 1 },
	/* 6, 7: not on the cover: in the animation an X-wing runs from the big
	 * Interceptor and an A-wing from the TIE fighter on the left, both gone
	 * from the view by the cover's moment (hidden in the still; see the
	 * runners). */
	{ "Xwing", 80.0f, 30.0f, 300.0f, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0, 0 },
	{ "AWing", -80.0f, 30.0f, 300.0f, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0, 0 },
	/* 8, 9: not on the cover: two A-wings screaming past the viewer early in
	 * the animation (see the passers); hidden in the still. */
	{ "AWing", 80.0f, -30.0f, 300.0f, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0, 0 },
	{ "AWing", -80.0f, -30.0f, 300.0f, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0, 0 },
};
/* The passers: ships 8 and 9 fly straight in from far ahead, cross each
 * other's course PASS_CROSS_M ahead of the viewer and pass it at PASS_AT
 * seconds, one on the left, one on the right (viewer's x right, y ahead,
 * z up; metres at the moment they pass). */
#define ENV_TIE_FIRST_PASSER 8
#define ENV_TIE_PASSERS 2
#define ENV_TIE_PASS_AT 3.0f
#define ENV_TIE_PASS_SPEED 140.0f
#define ENV_TIE_PASS_CROSS_M 35.0f
static const float k_tie_pass[ENV_TIE_PASSERS][2] = { { -7.0f, 3.0f }, { 7.5f, -2.0f } };
#define ENV_TIE_RUNNERS 2
#define ENV_TIE_CHASE_SHIP 0 /* the Interceptor after the jinking X-wing */
#define ENV_TIE_FIRST_RUNNER 6
/* Who runs from whom, and how it gets away: the break comes where it is
 * break_m from the viewer (0: never by distance) or end_s before the
 * cover's moment, whichever is first; a barrel roll of roll_s (0: none),
 * then a pull at pull_w rad/s toward `away` (viewer's x right, y ahead,
 * z up). */
static const EnvRunner k_tie_runners[ENV_TIE_RUNNERS] = {
	/* The X-wing from the big Interceptor: jinking left and right three
	 * times ahead of it, the Interceptor correcting after it, until it is
	 * hit and disappears in the great explosion. */
	{ 6, 0, 50.0f, 0.0f, 0.0f, 0.0f, 0.0f, { 0.0f, 0.0f, 0.0f }, 1 },
	/* The A-wing from the TIE fighter on the left: it holds on to the end
	 * of the TIE's path (where the TIE will be on the cover), then curves
	 * away out past the viewer's left. */
	{ 7, 2, 50.0f, 0.0f, 0.02f, 0.0f, 0.8f, { -1.0f, -0.4f, 0.3f }, 0 },
};

/* The explosion's centre behind the big Interceptor: yaw, pitch, metres,
 * diameter in metres. */
static const float k_tie_blast[4] = { -6.0f, -33.0f, 70.0f, 58.0f };

static const EnvSalvo k_tie_salvos[] = {
	/* The long green streaks of the Interceptor upper left. */
	{ 1, 0, ENV_GUNS_INTERCEPTOR, { { 0.05f, 1.10f } }, 1 },
	/* The TIE fighter's fire to the left. */
	{ 2, 0, ENV_GUNS_TIE, { { 0.05f, 0.75f } }, 1 },
	/* The bomber's streak to the upper right. */
	{ 3, 0, ENV_GUNS_TIE, { { 0.10f, 1.60f } }, 1 },
};

/* ---- "Defender of the Empire": its box art ------------------------------
 * The cover of TIE Fighter: Defender of the Empire (its title left out): a
 * TIE Bomber coming at the viewer lower right firing green, a TIE Advanced
 * upper right seen from the front, a tight formation of four TIE Advanced on
 * the left firing to the lower left, an A-wing breaking up in a great orange
 * explosion lower left, a TIE pilot's helmet looming behind it all over
 * purple clouds. Cover attitudes (view-relative, fitted to the cover against
 * renders of the models), places pushed out just far enough to clear the
 * screen and logo. */
static const EnvShip k_doeart[] = {
	/* 0: the big TIE Bomber lower right: coming at us, the cockpit pod's
	 * window and the ordnance pod's nose toward us, rolled with the pods'
	 * axis falling to the right; its two green streaks down past us. */
	{ "TieBomber", 30.0f, -30.0f, 18.0f, { -0.20f, -0.95f, -0.25f }, { 0.52f, -0.32f, 0.79f }, 0, 1 },
	/* 1: the TIE Advanced upper right, from the front, its wings like '<'
	 * and '>' rising to the right, turned a little to our right. */
	{ "TieAdvanced", 37.0f, 19.0f, 32.0f, { 0.51f, -0.82f, 0.26f }, { -0.35f, 0.07f, 0.93f }, 0, 1 },
	/* 2..5: the formation on the left (2 leads, upper right of it; 3 upper
	 * left, 4 lower left, 5 lower right): three-quarters from the front
	 * below, rolled so the wings run diagonally, heading down to the left. */
	{ "TieAdvanced", -27.0f, -3.0f, 42.0f, { -0.50f, -0.70f, -0.50f }, { 0.37f, -0.70f, 0.61f }, 0, 1 },
	{ "TieAdvanced", -39.0f, -5.0f, 42.0f, { -0.50f, -0.70f, -0.50f }, { 0.37f, -0.70f, 0.61f }, 0, 1 },
	{ "TieAdvanced", -39.0f, -18.0f, 42.0f, { -0.50f, -0.70f, -0.50f }, { 0.37f, -0.70f, 0.61f }, 0, 1 },
	{ "TieAdvanced", -27.0f, -16.0f, 42.0f, { -0.50f, -0.70f, -0.50f }, { 0.37f, -0.70f, 0.61f }, 0, 1 },
	/* 6: the A-wing breaking up in the explosion, nose down to the left. */
	{ "AWing", -11.0f, -33.0f, 58.0f, { -0.50f, -0.50f, -0.70f }, { -0.71f, 0.71f, 0.00f }, 0, 1 },
	/* 7, 8: not on the cover: two A-wings screaming past early in the fight
	 * (the passers); hidden in the still. */
	{ "AWing", 80.0f, -30.0f, 300.0f, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0, 0 },
	{ "AWing", -80.0f, -30.0f, 300.0f, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 0, 0 },
};

/* The explosion round the A-wing: yaw, pitch, metres, diameter in metres. */
static const float k_doe_blast[4] = { -12.0f, -32.0f, 64.0f, 48.0f };

static const EnvSalvo k_doe_salvos[] = {
	/* The bomber's two streaks down past the viewer. */
	{ 0, 6, ENV_GUNS_TIE, { { 0.04f, 0.26f } }, 1 },
	/* The formation's long streaks to the lower left (the lower right one). */
	{ 5, 6, ENV_GUNS_TIE, { { 0.08f, 2.4f } }, 1 },
};

/* ---- "Balance of Power": the X-Wing vs. TIE Fighter add-on's box art ----
 * Its artwork inside the border (no logo, stickers or labels): a Super Star
 * Destroyer diagonally across the middle, its bow lower left, a brown world
 * behind it, a big B-wing upper left and another lower right seen from
 * behind, their engines glowing red, an A-wing from behind at the bottom,
 * three TIE Advanced upper right, small B-wings by the dreadnought's stern,
 * Star Destroyers in the distance and red-pink flak. Cover attitudes, the
 * fighters pushed out to clear the screen; the dreadnought and the world are
 * the backdrop the screen stands in front of. */
static const EnvShip k_bopart[] = {
	/* 0: the big B-wing upper left, from behind and below, the cockpit end
	 * of its wing up to the left. */
	{ "BWing", -36.0f, 8.0f, 21.0f, { 0.15f, 0.95f, 0.26f }, { -0.44f, -0.18f, 0.88f }, 0, 1 },
	/* 1: the B-wing lower right, the same way. */
	{ "BWing", 35.0f, -33.0f, 30.0f, { 0.10f, 0.95f, 0.30f }, { -0.42f, -0.24f, 0.87f }, 0, 1 },
	/* 2: the A-wing at the bottom, from behind and above. */
	{ "AWing", 1.0f, -42.0f, 44.0f, { 0.00f, 0.94f, 0.35f }, { 0.00f, -0.35f, 0.94f }, 0, 1 },
	/* 3..5: the three TIE Advanced upper right (3 leads), from the front. */
	{ "TieAdvanced", 33.0f, 30.0f, 75.0f, { 0.10f, -0.95f, -0.30f }, { 0.10f, -0.29f, 0.95f }, 0, 1 },
	{ "TieAdvanced", 21.0f, 26.0f, 75.0f, { 0.10f, -0.95f, -0.30f }, { 0.10f, -0.29f, 0.95f }, 0, 1 },
	{ "TieAdvanced", 33.0f, 21.0f, 75.0f, { 0.10f, -0.95f, -0.30f }, { 0.10f, -0.29f, 0.95f }, 0, 1 },
	/* 6: a fourth TIE Advanced, shot down in the flak by the trio. */
	{ "TieAdvanced", 26.0f, 16.0f, 72.0f, { -0.30f, -0.90f, -0.30f }, { 0.00f, -0.30f, 0.95f }, 0, 1 },
	/* 7: a small B-wing by the dreadnought's stern. */
	{ "BWing", 38.0f, -12.0f, 130.0f, { 0.20f, 0.90f, 0.35f }, { -0.30f, -0.30f, 0.90f }, 0, 1 },
	/* 8: the Super Star Destroyer, its bow lower left (world attitude). */
	{ "SuperStarDestroyer",
	  1.0f,
	  -29.0f,
	  3600.0f,
	  { -0.89f, -0.09f, -0.44f },
	  { -0.33f, -0.55f, 0.77f },
	  0,
	  0 },
	/* 9..11: Star Destroyers in the distance: two right of its stern, one
	 * far left below. */
	{ "ImperialStarDestroyer2", 41.0f, 1.0f, 9000.0f, { -0.70f, -0.70f, 0.00f }, { 0.0f, 0.0f, 1.0f }, 0, 0 },
	{ "ImperialStarDestroyer2",
	  37.0f,
	  11.0f,
	  20000.0f,
	  { -0.80f, -0.55f, 0.15f },
	  { 0.0f, 0.0f, 1.0f },
	  0,
	  0 },
	{ "ImperialStarDestroyer2",
	  -32.0f,
	  -24.0f,
	  16000.0f,
	  { 0.90f, 0.30f, 0.10f },
	  { 0.0f, 0.0f, 1.0f },
	  0,
	  0 },
};

/* The flak burst the fourth TIE Advanced dies in. */
static const float k_bop_blast[4] = { 26.0f, 16.0f, 72.0f, 12.0f };

/* The other red-pink flak bursts: yaw, pitch, metres, diameter. */
static const float k_bop_flak[][4] = {
	{ 19.0f, -18.0f, 160.0f, 14.0f },
	{ 6.0f, -30.0f, 140.0f, 10.0f },
	{ 40.0f, -11.0f, 150.0f, 9.0f },
	{ -5.0f, -24.0f, 260.0f, 7.0f },
};

static const EnvSalvo k_bop_salvos[] = {
	/* The big B-wing's fire, away into the scene. */
	{ 0, 8, ENV_GUNS_BWING, { { 0.002f, 0.012f }, { 0.018f, 0.028f } }, 2 },
	/* The lower B-wing's. */
	{ 1, 8, ENV_GUNS_BWING, { { 0.002f, 0.010f } }, 1 },
};

/* A laser streak frozen between two points given as yaw, pitch, metres. */
typedef struct EnvStreak {
	float from[3], to[3];
	int green;
} EnvStreak;

/* ---- "X-Wing vs. TIE Fighter": its box art -----------------------------
 * The cover of X-Wing vs. TIE Fighter, its craft only (no logo, labels or
 * the pilots' faces behind): an explosion with a pillar of light through
 * it in the middle, an X-wing banking up through the fire, a big X-wing
 * diving past lower right with a green streak across it, a TIE Interceptor
 * lower left facing us, two TIE fighters upper left of the middle, two
 * X-wings coming at us from the explosion's top and the far right, two
 * tiny TIEs far left, a green streak from the right. Cover attitudes
 * (view-relative); the middle, behind the screen on the cover, moves below
 * it, the rest out just far enough to clear the screen and logo. */
static const EnvShip k_xvtart[] = {
	/* Attitudes fitted to the cover (keypoint pose fits for the X-wings,
	 * real-mesh renders compared side by side for the TIEs). */
	/* 0: the big X-wing lower right: nose toward us and to the right, its
	 * top tilted toward us, the left wings up to the right. */
	{ "Xwing", 40.0f, -29.0f, 16.0f, { 0.325f, -0.932f, -0.161f }, { -0.471f, -0.308f, 0.827f }, 0, 1 },
	/* 1: the TIE Interceptor lower left: window toward us, turned a
	 * little to its right and down, wings falling to the right. */
	{ "TieInterceptor", -32.0f, -28.0f, 14.0f, { -0.439f, -0.861f, -0.259f }, { 0.255f, -0.395f, 0.882f }, 0, 1 },
	/* 2: the X-wing in the explosion, coming out of it toward us and to
	 * the lower left. */
	{ "Xwing", -8.0f, -33.0f, 60.0f, { -0.186f, -0.958f, -0.217f }, { 0.514f, -0.283f, 0.810f }, 0, 1 },
	/* 3: the small X-wing at the explosion's top, coming out of it toward
	 * us, down to the left. */
	{ "Xwing", 25.0f, -2.0f, 80.0f, { -0.310f, -0.852f, -0.423f }, { -0.457f, -0.256f, 0.852f }, 0, 1 },
	/* 4: the far X-wing on the right, head on, rolled into a tall X. */
	{ "Xwing", 31.0f, 10.0f, 110.0f, { 0.0f, -0.990f, 0.139f }, { -0.777f, 0.088f, 0.623f }, 0, 1 },
	/* 5: TIE fighter upper left of the middle: toward us, window down to
	 * the lower left, wings near edge on, pylons rising to the right. */
	{ "TieFighter", -26.0f, 12.0f, 50.0f, { 0.296f, -0.814f, -0.500f }, { -0.384f, -0.581f, 0.718f }, 0, 1 },
	/* 6: the TIE fighter left of it, smaller, the same way. */
	{ "TieFighter", -34.0f, 6.0f, 75.0f, { 0.296f, -0.814f, -0.500f }, { -0.258f, -0.572f, 0.778f }, 0, 1 },
	/* 7, 8: the two tiny TIEs far left: head on from below and above. */
	{ "TieFighter", -46.0f, 18.0f, 160.0f, { 0.0f, -0.866f, 0.500f }, { -0.407f, 0.457f, 0.791f }, 0, 1 },
	{ "TieFighter", -40.0f, 19.0f, 160.0f, { 0.075f, -0.863f, -0.500f }, { 0.349f, -0.447f, 0.824f }, 0, 1 },
};

/* The explosion: yaw, pitch, metres, diameter in metres. */
static const float k_xvt_blast[4] = { 0.0f, -30.0f, 90.0f, 55.0f };

/* The laser streaks: green across the big X-wing and in from the right,
 * red from the X-wing in the explosion. */
static const EnvStreak k_xvt_streaks[] = {
	{ { 14.0f, -20.0f, 30.0f }, { 38.0f, -40.0f, 12.0f }, 1 },
	{ { 62.0f, 2.0f, 60.0f }, { 38.0f, 5.0f, 98.0f }, 1 },
	/* The X-wing coming out of the explosion fires: red bolts from two of
	 * its cannons along its nose, down past the Interceptor's wing. */
	{ { -13.7f, -30.0f, 54.8f }, { -25.1f, -34.8f, 35.5f }, 0 },
	{ { -4.5f, -38.3f, 55.2f }, { -11.0f, -48.5f, 36.1f }, 0 },
};

typedef struct EnvSpaceScene {
	const EnvShip* ships;
	int count;
	const EnvSalvo* salvos;
	int salvo_count;
} EnvSpaceScene;

static const EnvSpaceScene k_scene_endor = { k_endor, ENV_ENDOR_COUNT, k_salvos,
											 (int)(sizeof k_salvos / sizeof k_salvos[0]) };
static const EnvSpaceScene k_scene_tieart = { k_tieart, (int)(sizeof k_tieart / sizeof k_tieart[0]), k_tie_salvos,
											  (int)(sizeof k_tie_salvos / sizeof k_tie_salvos[0]) };
static const EnvSpaceScene k_scene_xvtart = { k_xvtart, (int)(sizeof k_xvtart / sizeof k_xvtart[0]), NULL, 0 };
static const EnvSpaceScene k_scene_boxart = { k_boxart, (int)(sizeof k_boxart / sizeof k_boxart[0]), k_box_salvos,
											  (int)(sizeof k_box_salvos / sizeof k_box_salvos[0]) };
static const EnvSpaceScene k_scene_doeart = { k_doeart, (int)(sizeof k_doeart / sizeof k_doeart[0]),
											  k_doe_salvos,
											  (int)(sizeof k_doe_salvos / sizeof k_doe_salvos[0]) };
static const EnvSpaceScene k_scene_bopart = { k_bopart, (int)(sizeof k_bopart / sizeof k_bopart[0]),
											  k_bop_salvos,
											  (int)(sizeof k_bop_salvos / sizeof k_bop_salvos[0]) };

/* The TIE Fighter scene: the X-wing (5) hunted down by a TIE fighter on the
 * right, the Interceptor (0) after the jinking X-wing, two runners, two
 * passers. */
static const int k_tie_shooters[] = { 1, 2, 3, 0, 5 };
static const EnvAnim k_anim_tie = {
	5,
	k_tie_blast,
	{ 1.5f, 1.4f, 1.3f },
	-1,
	ENV_TIE_CHASE_SHIP,
	k_tie_runners,
	ENV_TIE_RUNNERS,
	ENV_TIE_FIRST_PASSER,
	ENV_TIE_PASSERS,
	k_tie_pass,
	k_tie_shooters,
	5,
	NULL,
	8,
};

/* Defender of the Empire: the bomber (0) finishes the A-wing (6); the
 * formation (3..5 on 2) comes in together; two A-wings pass early. */
static const int k_doe_shooters[] = { 1, 2, 3, 4, 5, 6 };
static const signed char k_doe_leader[] = { -1, -1, -1, 2, 2, 2, -1, -1, -1 };
static const float k_doe_pass[2][2] = { { -7.0f, 3.0f }, { 7.5f, -2.0f } };
static const EnvAnim k_anim_doe = {
	6, k_doe_blast, { 1.5f, 1.4f, 1.3f }, 0, -1,           NULL, 0, 7,
	2, k_doe_pass,  k_doe_shooters,       6, k_doe_leader, 7,
};

/* Balance of Power: the B-wings, the A-wing and the small B-wing run in on
 * the dreadnought, the TIE Advanced trio (4, 5 on 3) comes down on them, and
 * the big B-wing (0) shoots a fourth TIE Advanced (6) out of the flak. */
static const int k_bop_shooters[] = { 1, 2, 3, 4, 5, 7 };
static const signed char k_bop_leader[] = { -1, -1, -1, -1, 3, 3, -1, -1, -1, -1, -1, -1 };
static const EnvAnim k_anim_bop = {
	6, k_bop_blast, { 1.7f, 0.75f, 0.95f }, 0, -1, NULL, 0, 0, 0, NULL, k_bop_shooters, 6, k_bop_leader, 8,
};

/* Sprites from the game's resource DATs (group, sprite id) plus the logo
 * and a generated beam profile. */
enum {
	ENV_TEX_FRAME, /* first: the screen frame shows in every scene */
	ENV_TEX_DEATHSTAR,
	ENV_TEX_ENDOR,
	ENV_TEX_BLAST,
	ENV_TEX_FIRE,
	ENV_TEX_LOGO,
	ENV_TEX_BEAM,
	ENV_TEX_NEBULA,      /* generated: the box art's purple nebula */
	ENV_TEX_VADER,       /* vr/vader.rgba: the helmet behind the TIE Fighter scene */
	ENV_TEX_TIE_PILOT,   /* vr/xvt_tie_pilot.rgba: X-Wing vs. TIE backdrop, left */
	ENV_TEX_REBEL_PILOT, /* vr/xvt_rebel_pilot.rgba: X-Wing vs. TIE backdrop, right */
	ENV_TEX_GLOW,        /* generated: a soft round glow (engines, flashes) */
	ENV_TEX_PLANET,      /* generated: Balance of Power's brown world */
	ENV_TEX_BOOM,        /* the explosion's animation frames (ENV_BOOM_FRAMES slots) */
	ENV_TEX_BOOM_LAST = ENV_TEX_BOOM + 11,
	ENV_TEX_BACKDROP, /* the last mission's planets, nebulae and suns */
	ENV_TEX_BACKDROP_LAST = ENV_TEX_BACKDROP + 5,
	ENV_TEX_COUNT
};
#define ENV_BOOM_FRAMES (ENV_TEX_BOOM_LAST - ENV_TEX_BOOM + 1)
#define ENV_BACKDROPS (ENV_TEX_BACKDROP_LAST - ENV_TEX_BACKDROP + 1)

typedef struct EnvTexSource {
	const char* dat; /* under Resdata/ */
	int group, sprite;
} EnvTexSource;

static const EnvTexSource k_tex_sources[ENV_TEX_COUNT] = {
	{ NULL, 0, 0 },            /* vr/frame.rgba */
	{ "DSFire", 6250, -1 },    /* the second Death Star (first frame) */
	{ "Planet", 6026, -1 },    /* a green forest world for Endor */
	{ "Explosions", 2005, 7 },
	{ "Explosions", 2003, 7 },
	{ NULL, 0, 0 },            /* vr/logo.rgba */
	{ NULL, 0, 0 },            /* generated */
	{ NULL, 0, 0 },            /* generated */
	{ NULL, 0, 0 },            /* vr/vader.rgba */
	{ NULL, 0, 0 },            /* vr/xvt_tie_pilot.rgba */
	{ NULL, 0, 0 },            /* vr/xvt_rebel_pilot.rgba */
	{ NULL, 0, 0 },            /* generated */
	[ENV_TEX_BOOM] = { "Explosions", 2005, 1 }, /* frames 1..12 of the fighter explosion */
	/* the backdrops: chosen per mission (s_bt.backdrops) */
};

typedef enum EnvMode {
	ENV_OFF = 0,
	ENV_ENDOR,
	ENV_HANGAR,
	ENV_BOXART,
	ENV_TIEART,
	ENV_XVTART,
	ENV_BATTLE,
	ENV_DOEART,
	ENV_BOPART
} EnvMode;

/* The box arts that play a fight into their cover's moment. */
static int env_played(EnvMode m) { return m == ENV_TIEART || m == ENV_DOEART || m == ENV_BOPART; }

#define ENV_MAX_MESHES 24
#define ENV_MAX_INSTANCES 40
#define ENV_TIE_BOLTS 64
/* InsideHangar hardpoint: the hangar floor (classic launch base). */
#define ENV_HARDPOINT_INSIDE_HANGAR 25
/* Floor below the launch reference when the hardpoint is missing (the
 * explicit heights of floor-standing props put it near here). */
#define ENV_FLOOR_FALLBACK (-870.0f)

typedef struct EnvMesh {
	char opt[64]; /* as the scene names it (alternatives with '|') */
	AeronSceneMesh* mesh;
	float size_z; /* world units */
	float len_m, wid_m, hgt_m; /* the model's extents in metres (y, x, z) */
	int tried;
	/* Its engine glows (model metres, diameter in metres, core colour). */
	int glow_count;
	float glow[8][7];
} EnvMesh;

static struct {
	EnvMode mode;
	EnvMesh meshes[ENV_MAX_MESHES];
	int mesh_count;
	int hangar; /* index of Hangar.opt in meshes */
	float floor_z;
	int laid_out;
	int logged;
	float view_pos[3];
	/* Instances, rebuilt whenever a mesh arrives. */
	int count;
	int mesh_of[ENV_MAX_INSTANCES];
	int ship_of[ENV_MAX_INSTANCES]; /* space scenes: the ship each instance shows */
	/* TIE Fighter box art: its five seconds before the still. */
	float anim_t;
	uint64_t anim_counter;
	int anim_paths;
	struct {
		float v;     /* speed, world units per second */
		float w;     /* turn rate into the pose, rad/s */
		float alpha; /* the last turn's angle (pulling into the pose) */
		float beta;  /* an earlier turn's angle (0: none) */
		float b1;    /* that turn's bank from level */
	} anim_fl[8];
	struct {
		float p[3], d[3], life;
		int green, alive;
	} anim_bolts[ENV_TIE_BOLTS];
	float anim_next_shot[8];
	struct {
		int ok, chaser;
		float lead;   /* seconds ahead of the chaser on its path */
		float tb;     /* the break: barrel roll, then the pull away */
		float v, roll_time, helix;
		float pb[3], nb[3], ub[3]; /* at the break (world units) */
		float pull_up[3];          /* the pull's turn centre direction */
		float pull_w, pull_bank;
		int flyby_done;
	} runner[ENV_TIE_RUNNERS];
	int anim_hunter; /* the TIE that shoots the X-wing down (-1: none) */
	float transform[ENV_MAX_INSTANCES][16];
	uint8_t far[ENV_MAX_INSTANCES];
	uint8_t hidden[ENV_MAX_INSTANCES]; /* not drawn this frame */
	/* Endor sprites. */
	AeronTexture* tex[ENV_TEX_COUNT];
	int tex_w[ENV_TEX_COUNT], tex_h[ENV_TEX_COUNT];
	int tex_tried[ENV_TEX_COUNT];
	/* The frame as its own layer over the screen. */
	AeronDrawList2D* frame_list;
	int frame_drawn, frame_layer_ok;
	float frame_tint, frame_fade;
} s_env;

static float env_float(const char* name, float fallback) {
	const char* v = SDL_getenv(name);
	return v && v[0] ? (float)atof(v) : fallback;
}

static EnvMode env_mode(void) {
	const char* v = SDL_getenv("XWA_XR_ENVIRONMENT");
	if (v && strcmp(v, "0") == 0) {
		return ENV_OFF;
	}
	/* A failed mission is debriefed among the TIE fighters downing an
	 * X-wing; the chosen scene comes back after it. */
	if (MissionDebrief_ShowingFailure()) {
		return ENV_TIEART;
	}
	/* A completed mission is debriefed in the live battle (its own craft);
	 * the chosen scene comes back when the debriefing closes. */
	if (MissionDebrief_ShowingSuccess()) {
		return ENV_BATTLE;
	}
	if (v && SDL_strcasecmp(v, "hangar") == 0) {
		return ENV_HANGAR;
	}
	if (v && (SDL_strcasecmp(v, "xwing") == 0 || SDL_strcasecmp(v, "boxart") == 0)) {
		return ENV_BOXART;
	}
	if (v && SDL_strcasecmp(v, "tiefighter") == 0) {
		return ENV_TIEART;
	}
	if (v && SDL_strcasecmp(v, "xvt") == 0) {
		return ENV_XVTART;
	}
	if (v && SDL_strcasecmp(v, "defender") == 0) {
		return ENV_DOEART;
	}
	if (v && SDL_strcasecmp(v, "balance") == 0) {
		return ENV_BOPART;
	}
	if (v && SDL_strcasecmp(v, "endor") == 0) {
		return ENV_ENDOR;
	}
	if (v && SDL_strcasecmp(v, "battle") == 0) {
		return ENV_BATTLE;
	}
	/* Unset: the TIE Fighter box art. */
	if (!v || !v[0]) {
		return ENV_TIEART;
	}
	/* "random": one of the space scenes, picked once per start. */
	static EnvMode picked = ENV_OFF;
	if (picked == ENV_OFF) {
		static const EnvMode scenes[] = { ENV_ENDOR,  ENV_BOXART, ENV_TIEART, ENV_XVTART,
										  ENV_BATTLE, ENV_DOEART, ENV_BOPART };
		const uint64_t seed = SDL_GetPerformanceCounter() ^ ((uint64_t)time(NULL) * 2654435761u);
		picked = scenes[(seed ^ (seed >> 17) ^ (seed >> 31)) % (sizeof scenes / sizeof scenes[0])];
		Aeron_LogInfo("xwa.remaster", "VR environment: random pick %s",
					  picked == ENV_ENDOR    ? "endor"
					  : picked == ENV_BOXART ? "xwing"
					  : picked == ENV_TIEART ? "tiefighter"
					  : picked == ENV_BATTLE ? "battle"
					  : picked == ENV_DOEART ? "defender"
					  : picked == ENV_BOPART ? "balance"
											 : "xvt");
	}
	return picked;
}

/* The ship list of the selected space scene (NULL in the hangar). */
static const EnvSpaceScene* env_space(void) {
	switch (s_env.mode) {
		case ENV_ENDOR:
			return &k_scene_endor;
		case ENV_BOXART:
			return &k_scene_boxart;
		case ENV_TIEART:
			return &k_scene_tieart;
		case ENV_XVTART:
			return &k_scene_xvtart;
		case ENV_DOEART:
			return &k_scene_doeart;
		case ENV_BOPART:
			return &k_scene_bopart;
		default:
			return NULL;
	}
}

/* The played scene's fight (the TIE Fighter scene's when none is). */
static const EnvAnim* env_anim(void) {
	return s_env.mode == ENV_DOEART ? &k_anim_doe : s_env.mode == ENV_BOPART ? &k_anim_bop : &k_anim_tie;
}

int XwaVrEnv_Enabled(void) { return env_mode() != ENV_OFF; }

/* Bare (stars only) during the startup films and until the scene has
 * loaded; then the scene fades in (s_env_fade 0..1, eased). */
static int s_env_bare = 1;
static float s_env_fade_t; /* linear 0..1 */
static float s_env_fade;   /* eased */
static uint64_t s_env_fade_counter;

/* Every model, sprite and the frame of the scene tried (loaded or not). */
static int env_tex_needed(int slot);
static int env_ready(void) {
	if (s_env.mode == ENV_OFF || !s_env.mesh_count || !s_env.laid_out) {
		return 0;
	}
	for (int i = 0; i < s_env.mesh_count; ++i) {
		if (!s_env.meshes[i].tried) {
			return 0;
		}
	}
	for (int i = 0; i < ENV_TEX_COUNT; ++i) {
		if (env_tex_needed(i) && !s_env.tex_tried[i]) {
			return 0;
		}
	}
	return 1;
}

void XwaVrEnv_SetBare(int bare) {
	const uint64_t now = SDL_GetPerformanceCounter();
	const float dt = s_env_fade_counter
						 ? (float)((double)(now - s_env_fade_counter) / (double)SDL_GetPerformanceFrequency())
						 : 0.0f;
	s_env_fade_counter = now;
	if (bare || !env_ready()) {
		s_env_bare = 1;
		s_env_fade_t = 0.0f;
		(void)Aeron_XrTakeMenuXPressed(); /* no stale X or Y (from flight) on the way in */
		(void)Aeron_XrTakeMenuYPressed();
	} else {
		s_env_bare = 0;
		s_env_fade_t = fminf(1.0f, s_env_fade_t + fminf(dt, 0.1f) / env_float("XWA_XR_ENV_FADE_SECONDS", 1.5f));
	}
	s_env_fade = s_env_fade_t * s_env_fade_t * (3.0f - 2.0f * s_env_fade_t);
}

int XwaVrEnv_Bare(void) { return s_env_bare; }

float XwaVrEnv_Fade(void) { return s_env_bare ? 0.0f : s_env_fade; }

/* The startup title: once per run, when the menu environment first comes
 * up after the startup films, it shows the X-Wing Alliance logo large where
 * the screen will be, "Press A to Continue" under it, and no screen, frame,
 * hands or pointer; the controllers reach nothing (Aeron_XrSetInputBlocked)
 * until A is pressed, then the screen and frame come in. The TIE Fighter
 * box art plays its fight first (A during it jumps to its end). Later
 * (after missions) the menus come straight up. XWA_XR_TITLE=0: no title. */
#define ENV_TIE_REVEAL_SECONDS 0.6f
static struct {
	int done;      /* the title has been passed this run */
	int go;        /* A pressed: the screen is coming in */
	float go_t;    /* seconds since */
	float shown_t; /* seconds the title has been up */
	int pressed;   /* an A press this frame not taken by the title */
	uint64_t counter;
	AeronFontAtlas font;
	int font_state; /* 0 not tried, 1 loaded, -1 failed */
	AeronRenderTarget* text_rt;
	AeronDrawList2D* text_list;
	int text_w, text_h;
	int music_saved; /* the music state before ours (-1: not changed) */
} s_title = { .music_saved = -1 };

/* The game's music (xwa/audio/music.h pulls in iMUSE's headers). */
extern int musicState;
int Music_SetState(int state);
/* MUSIC_STATE_FRONTEND_1260: the debriefing theme the game plays after a
 * failed mission (MissionDebrief_Update; 1270 in the family campaign, 1280
 * after a completed one). */
#define ENV_MUSIC_TITLE 1260

/* X in the menus puts the screen away: it fades out (with its frame and
 * logo), the X-Wing Alliance logo fades in large where it was (as at the
 * startup title), and nothing reaches the menus until X, A or the trigger
 * brings it back. */
static struct {
	int away;    /* put away (input blocked at once) */
	float level; /* 1 shown .. 0 gone */
	float logo;  /* the large logo meanwhile, 0 .. 1 */
} s_dismiss = { 0, 1.0f, 0.0f };
#define ENV_DISMISS_SECONDS 0.4f

/* Y in the menus (or the VR options' Menu Scene) swaps the scene in place:
 * while the next one loads (bare: stars only) the screen stays shown or put
 * away, and the frame, logos and button hints stay up. Set until the
 * environment is released (flight). */
static int s_env_swapped;

/* The screen's state (put away or not) counts: menus with a scene, not the
 * startup films. */
static int env_dismiss_applies(void) { return s_env.mode != ENV_OFF && (!s_env_bare || s_env_swapped); }

/* Opacity of the frame, logos and hints: they fade in with the scene the
 * first time; after a swap they stay. */
static float env_chrome_fade(void) { return s_env_swapped ? 1.0f : s_env_fade; }

static int env_title_active(void) {
	return !s_title.done && s_env.mode != ENV_OFF && env_float("XWA_XR_TITLE", 1.0f) != 0.0f;
}

static float env_screen_reveal(void);
static void env_title_text(AeronCommandBuffer* cmd);
static void env_hint_text(AeronCommandBuffer* cmd);
static void env_prompt_text(AeronCommandBuffer* cmd);
int XwaVrEnv_ScreenHidden(void) { return env_dismiss_applies() && env_screen_reveal() <= 0.0f; }
int XwaVrEnv_InputBlocked(void) { return XwaVrEnv_ScreenHidden() || (env_dismiss_applies() && s_dismiss.away); }
float XwaVrEnv_ScreenFade(void) { return env_dismiss_applies() ? env_screen_reveal() : 1.0f; }

/* The TIE Fighter box art's animation (defined below). */
#define ENV_TIE_ANIM_SECONDS 7.0f
#define ENV_TIE_BOOM_AT (ENV_TIE_ANIM_SECONDS - 1.4f)
#define ENV_TIE_SALVO_SECONDS 0.5f
#define ENV_TIE_BOLT_SPEED 320.0f /* m/s */
/* The X-wing (ship 5) is hit at these times, then blows up at BOOM_AT. */
static const float k_tie_hits[3] = { ENV_TIE_ANIM_SECONDS - 2.6f, ENV_TIE_ANIM_SECONDS - 2.15f,
									 ENV_TIE_ANIM_SECONDS - 1.75f };
/* The hunter's shots that miss before those. */
static const float k_tie_misses[2] = { ENV_TIE_ANIM_SECONDS - 3.6f, ENV_TIE_ANIM_SECONDS - 3.1f };
static void env_tie_pose(int ship, float t, float pos[3], float n[3], float u[3]);
static void env_tie_flight_pose(int ship, float t, float pos[3], float n[3], float u[3]);
static void env_tie_submit_bolts(AeronScene3D* scene);
static void env_es_stop(void);
/* The scene's sounds (see "the TIE art's sound"). */
enum {
	ES_TIE_LASER,
	ES_REBEL_LASER,
	ES_TIE_ENGINE,
	ES_TIE_FLYBY,
	ES_BOOM,
	ES_HIT0,
	ES_HIT1,
	ES_HIT2,
	ES_XW_FLYBY,
	ES_AW_FLYBY,
	ES_COUNT
};
static int es_is_tie(int ship);
static void es_play(int s, const float p[3], float ref_m, float gain, float pitch);

static float env_screen_reveal(void) {
	if (!env_title_active()) {
		const float x = s_dismiss.level;
		return s_dismiss.away ? x * x * (3.0f - 2.0f * x) : fmaxf(x * x * (3.0f - 2.0f * x), 1e-3f);
	}
	/* The title holds until A is pressed. */
	const float x = fminf(fmaxf(s_title.go_t / ENV_TIE_REVEAL_SECONDS, 0.0f), 1.0f);
	return s_title.go ? fmaxf(x * x * (3.0f - 2.0f * x), 1e-3f) : 0.0f;
}

/* The title is up: at once, or for the TIE art once its fight is over. */
static int env_title_shown(void) {
	return env_title_active() && (!env_played(s_env.mode) || s_env.anim_t >= ENV_TIE_ANIM_SECONDS);
}

/* The live battle (defined below). */
static void bt_lighting(XwaVrEnvLighting* out, float sun);
static void bt_cast(void);
static int bt_load_texture(AeronCommandBuffer* cmd, int slot);
static int bt_tex_needed(int slot);

static int env_is_space(EnvMode m) {
	return m == ENV_ENDOR || m == ENV_BOXART || m == ENV_TIEART || m == ENV_XVTART || m == ENV_BATTLE ||
		   m == ENV_DOEART || m == ENV_BOPART;
}

int XwaVrEnv_IsSpace(void) { return env_is_space(env_mode()); }

static void env_set3(float out[3], float x, float y, float z) {
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

int XwaVrEnv_SpaceLighting(XwaVrEnvLighting* out) {
	const EnvMode m = env_mode();
	if (!out || !env_is_space(m)) {
		return 0;
	}
	memset(out, 0, sizeof *out);
	const float sun = env_float("XWA_XR_ENDOR_SUN", 1.6f);
	if (m == ENV_BATTLE) {
		bt_lighting(out, sun);
	} else if (m == ENV_ENDOR) {
		/* A warm sun from the upper left behind the viewer, a faint green
		 * bounce off the moon below right, deep-space ambient. */
		env_set3(out->key_dir, -0.55f, 0.35f, 0.75f);
		env_set3(out->key_color, 1.0f * sun, 0.95f * sun, 0.88f * sun);
		env_set3(out->fill_dir, 0.55f, 0.35f, -0.75f);
		env_set3(out->fill_color, 0.55f * 0.3f, 0.85f * 0.3f, 0.6f * 0.3f);
		env_set3(out->ambient_sides, 0.05f, 0.05f, 0.05f);
		env_set3(out->ambient_top, 0.06f, 0.06f, 0.06f);
		env_set3(out->ambient_bottom, 0.04f, 0.08f, 0.05f);
	} else if (m == ENV_XVTART) {
		/* The XvT cover: the explosion's warm light from ahead below, a cool
		 * blue fill from the upper right, a dark blue ambient. */
		env_set3(out->key_dir, -0.10f, 0.85f, -0.35f);
		env_set3(out->key_color, 1.0f * sun, 0.78f * sun, 0.55f * sun);
		env_set3(out->fill_dir, 0.50f, 0.20f, 0.85f);
		env_set3(out->fill_color, 0.45f * 0.6f, 0.55f * 0.6f, 0.90f * 0.6f);
		env_set3(out->ambient_sides, 0.04f, 0.045f, 0.07f);
		env_set3(out->ambient_top, 0.04f, 0.05f, 0.08f);
		env_set3(out->ambient_bottom, 0.05f, 0.04f, 0.05f);
	} else if (m == ENV_DOEART) {
		/* The Defender of the Empire cover: a cool white light from the upper
		 * left behind the viewer (the bomber's lit faces), the great
		 * explosion's orange glow from ahead below left, a violet ambient. */
		env_set3(out->key_dir, -0.50f, -0.35f, 0.79f);
		env_set3(out->key_color, 0.85f * sun, 0.88f * sun, 1.0f * sun);
		env_set3(out->fill_dir, -0.35f, 0.80f, -0.50f);
		env_set3(out->fill_color, 1.0f * 0.95f, 0.55f * 0.95f, 0.18f * 0.95f);
		env_set3(out->ambient_sides, 0.05f, 0.04f, 0.08f);
		env_set3(out->ambient_top, 0.05f, 0.04f, 0.09f);
		env_set3(out->ambient_bottom, 0.06f, 0.035f, 0.07f);
	} else if (m == ENV_BOPART) {
		/* The Balance of Power cover: a pale lavender sun from the upper left
		 * (the world's and the dreadnought's lit sides), a faint blue fill
		 * from the right, a deep blue-violet ambient. */
		env_set3(out->key_dir, -0.62f, 0.30f, 0.72f);
		env_set3(out->key_color, 0.95f * sun, 0.92f * sun, 1.0f * sun);
		env_set3(out->fill_dir, 0.70f, -0.20f, -0.30f);
		env_set3(out->fill_color, 0.30f * 0.6f, 0.35f * 0.6f, 0.60f * 0.6f);
		env_set3(out->ambient_sides, 0.045f, 0.04f, 0.07f);
		env_set3(out->ambient_top, 0.05f, 0.045f, 0.08f);
		env_set3(out->ambient_bottom, 0.035f, 0.03f, 0.055f);
	} else if (m == ENV_TIEART) {
		/* The TIE Fighter cover: a cool blue-white light from the upper
		 * right, the explosion's orange glow from behind and below the
		 * craft, a violet ambient. */
		env_set3(out->key_dir, 0.45f, 0.25f, 0.85f);
		env_set3(out->key_color, 0.80f * sun, 0.85f * sun, 1.0f * sun);
		env_set3(out->fill_dir, 0.0f, 0.9f, -0.45f);
		env_set3(out->fill_color, 1.0f * 0.9f, 0.55f * 0.9f, 0.2f * 0.9f);
		env_set3(out->ambient_sides, 0.05f, 0.04f, 0.08f);
		env_set3(out->ambient_top, 0.05f, 0.04f, 0.09f);
		env_set3(out->ambient_bottom, 0.05f, 0.03f, 0.07f);
	} else {
		/* The box art: white light from the upper left ahead (it catches the
		 * tops and left sides of the craft flying away), the nebula's
		 * magenta glow from ahead, a violet ambient. */
		env_set3(out->key_dir, -0.45f, 0.55f, 0.70f);
		env_set3(out->key_color, 1.0f * sun, 0.96f * sun, 0.92f * sun);
		env_set3(out->fill_dir, 0.2f, 0.9f, -0.2f);
		env_set3(out->fill_color, 0.75f * 0.45f, 0.30f * 0.45f, 0.85f * 0.45f);
		env_set3(out->ambient_sides, 0.06f, 0.035f, 0.07f);
		env_set3(out->ambient_top, 0.07f, 0.04f, 0.08f);
		env_set3(out->ambient_bottom, 0.04f, 0.025f, 0.05f);
	}
	return 1;
}

static const char* env_craft(void) {
	const char* v = SDL_getenv("XWA_XR_ENV_CRAFT");
	if (v) {
		return v[0] && strcmp(v, "0") != 0 ? v : NULL;
	}
	return "Xwing";
}

static void env_list_add(const char* name) {
	for (int j = 0; j < s_env.mesh_count; ++j) {
		if (SDL_strcasecmp(s_env.meshes[j].opt, name) == 0) {
			return;
		}
	}
	if (s_env.mesh_count < ENV_MAX_MESHES) {
		snprintf(s_env.meshes[s_env.mesh_count].opt, sizeof s_env.meshes[0].opt, "%s", name);
		s_env.mesh_count++;
	}
}

/* The list of unique OPTs. Hangar: the hangar first, then the parked
 * craft, then the props. Endor: the Falcon first. */
static void env_list(void) {
	if (s_env.mesh_count) {
		return;
	}
	s_env.hangar = -1;
	s_env.floor_z = ENV_FLOOR_FALLBACK;
	const EnvSpaceScene* space = env_space();
	if (space) {
		for (int i = 0; i < space->count; ++i) {
			env_list_add(space->ships[i].opt);
		}
		return;
	}
	if (s_env.mode == ENV_BATTLE) {
		bt_cast();
		return;
	}
	env_list_add("Hangar");
	s_env.hangar = 0;
	if (env_craft()) {
		env_list_add(env_craft());
	}
	for (int i = 0; i < ENV_PROP_COUNT; ++i) {
		env_list_add(k_props[i].opt);
	}
}

static int env_find(const char* opt) {
	for (int j = 0; j < s_env.mesh_count; ++j) {
		if (SDL_strcasecmp(s_env.meshes[j].opt, opt) == 0) {
			return s_env.meshes[j].mesh ? j : -1;
		}
	}
	return -1;
}

static void env_load(AeronCommandBuffer* cmd, int index) {
	EnvMesh* m = &s_env.meshes[index];
	AeronFlightModel model;
	m->tried = 1;
	memset(&model, 0, sizeof model);
	/* The first of the '|'-separated names the game data has. */
	int built = 0;
	for (const char* name = m->opt; *name && !built;) {
		const size_t n = strcspn(name, "|");
		char one[64];
		snprintf(one, sizeof one, "%.*s", (int)n, name);
		built = one[0] && XwaRemasterShip_BuildOptModel(one, &model);
		name += n + (name[n] == '|');
	}
	if (!built) {
		Aeron_LogWarn("xwa.remaster", "VR environment: no OPT for '%s'", m->opt);
		return;
	}
	m->size_z = (model.bounds.max.z - model.bounds.min.z) * AERON_OPT_UNITS_PER_METER;
	m->glow_count = 0;
	for (uint32_t g = 0; g < model.engine_glow_count && m->glow_count < 8; ++g) {
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
	m->len_m = model.bounds.max.y - model.bounds.min.y;
	m->wid_m = model.bounds.max.x - model.bounds.min.x;
	m->hgt_m = model.bounds.max.z - model.bounds.min.z;
	if (index == s_env.hangar) {
		int found = 0;
		for (uint32_t c = 0; c < model.component_count && !found; ++c) {
			const AeronFlightComponent* comp = &model.components[c];
			for (uint32_t h = 0; h < comp->hardpoint_count; ++h) {
				if (comp->hardpoints[h].type == ENV_HARDPOINT_INSIDE_HANGAR) {
					s_env.floor_z = comp->hardpoints[h].position.z * AERON_OPT_UNITS_PER_METER;
					found = 1;
					break;
				}
			}
		}
		Aeron_LogInfo("xwa.remaster",
					  "VR environment: hangar bounds x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f, floor %.0f (%s)",
					  model.bounds.min.x * AERON_OPT_UNITS_PER_METER, model.bounds.max.x * AERON_OPT_UNITS_PER_METER,
					  model.bounds.min.y * AERON_OPT_UNITS_PER_METER, model.bounds.max.y * AERON_OPT_UNITS_PER_METER,
					  model.bounds.min.z * AERON_OPT_UNITS_PER_METER, model.bounds.max.z * AERON_OPT_UNITS_PER_METER,
					  (double)s_env.floor_z, found ? "InsideHangar hardpoint" : "fallback");
	}
	AeronSceneMeshCreateStatus status;
	m->mesh = AeronScene_MeshCreate(cmd, &model, m->opt, &status);
	Aeron_FlightModelFree(&model);
	if (!m->mesh) {
		Aeron_LogWarn("xwa.remaster", "VR environment: '%s' mesh creation failed (%d)", m->opt, (int)status);
	}
}

/* Model -> scene transform of an object with Q16 pitch/yaw at `pos`
 * (world units): the Euler compose of FVIEW_calcrotatemove with no roll,
 * consumed like the flight renderer's object transforms (model metres ->
 * world units). */
static void env_object_matrix(int pitch, int yaw, const float pos[3], float m[16]) {
	const float q16 = 2.0f * 3.14159265358979f / 65536.0f;
	const float a = (float)(int16_t)(uint16_t)(0xc000 - pitch) * q16;
	const float b = (float)(int16_t)(uint16_t)(-(int16_t)(uint16_t)yaw) * q16;
	const float cb = cosf(b), sb = sinf(b), ca = cosf(a), sa = sinf(a);
	const float cur0[3] = { cb, sb, 0.0f };
	const float cur1[3] = { -sb * sa, cb * sa, -ca };
	const float cur2[3] = { -sb * ca, cb * ca, sa };
	/* basis rows (cur0, cur2, cur1); world = basis^T * model */
	const float* rows[3] = { cur0, cur2, cur1 };
	const float k = AERON_OPT_UNITS_PER_METER;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			m[i * 4 + j] = k * rows[j][i];
		}
		m[i * 4 + 3] = pos[i];
	}
	m[12] = m[13] = m[14] = 0.0f;
	m[15] = 1.0f;
}

static void env_norm(float v[3]) {
	const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-9f) {
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

static void env_cross(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Unit direction of yaw/pitch degrees (yaw 0 = forward +y, + = right). */
static void env_dir(float yaw, float pitch, float out[3]) {
	const float y = yaw * 3.14159265358979f / 180.0f, p = pitch * 3.14159265358979f / 180.0f;
	out[0] = sinf(y) * cosf(p);
	out[1] = cosf(y) * cosf(p);
	out[2] = sinf(p);
}

/* Model -> scene transform from a nose and an up direction: the model's
 * forward (-y) onto `nose`, its z onto the up made orthogonal, x onto
 * nose x up (the level yaw-0 pose of env_object_matrix is nose +y, up +z). */
static void env_pose_matrix(const float nose_in[3], const float up_in[3], const float pos[3], float m[16]) {
	float n[3] = { nose_in[0], nose_in[1], nose_in[2] }, r[3], u[3];
	env_norm(n);
	env_cross(n, up_in, r);
	env_norm(r);
	env_cross(r, n, u);
	const float k = AERON_OPT_UNITS_PER_METER;
	for (int i = 0; i < 3; ++i) {
		m[i * 4 + 0] = k * r[i];
		m[i * 4 + 1] = -k * n[i];
		m[i * 4 + 2] = k * u[i];
		m[i * 4 + 3] = pos[i];
	}
	m[12] = m[13] = m[14] = 0.0f;
	m[15] = 1.0f;
}

static void env_add(int mesh, int pitch, int yaw, float x, float y, float z) {
	const float pos[3] = { x, y, z };
	if (mesh < 0 || s_env.count >= ENV_MAX_INSTANCES) {
		return;
	}
	s_env.mesh_of[s_env.count] = mesh;
	s_env.far[s_env.count] = 0;
	env_object_matrix(pitch, yaw, pos, s_env.transform[s_env.count]);
	s_env.count++;
}

/* World position (units) of the space scene's ship i. */
static void env_ship_pos(int i, float out[3]) {
	const EnvShip* e = &env_space()->ships[i];
	env_dir(e->yaw, e->pitch, out);
	for (int a = 0; a < 3; ++a) {
		out[a] *= e->dist * AERON_OPT_UNITS_PER_METER;
	}
}

/* Orthonormal nose / right / up of the space scene's ship i, the nose
 * aimed at its target when it has one. */
static void env_ship_basis(int i, float n[3], float r[3], float u[3]) {
	const EnvSpaceScene* sc = env_space();
	const EnvShip* e = &sc->ships[i];
	float up[3];
	memcpy(n, e->nose, 3 * sizeof(float));
	memcpy(up, e->up, sizeof up);
	if (e->view) {
		/* From the frame of the line of sight to the craft. */
		static const float z[3] = { 0.0f, 0.0f, 1.0f };
		float f[3], vr[3], vu[3];
		env_dir(e->yaw, e->pitch, f);
		env_cross(f, z, vr);
		env_norm(vr);
		env_cross(vr, f, vu);
		for (int a = 0; a < 3; ++a) {
			n[a] = vr[a] * e->nose[0] + f[a] * e->nose[1] + vu[a] * e->nose[2];
			up[a] = vr[a] * e->up[0] + f[a] * e->up[1] + vu[a] * e->up[2];
		}
	}
	if (e->aim > 0 && e->aim <= sc->count) {
		float from[3], to[3];
		env_ship_pos(i, from);
		env_ship_pos(e->aim - 1, to);
		for (int a = 0; a < 3; ++a) {
			n[a] = to[a] - from[a];
		}
	}
	env_norm(n);
	env_cross(n, up, r);
	env_norm(r);
	env_cross(r, n, u);
}

static void env_layout(void) {
	s_env.count = 0;
	if (s_env.mode == ENV_BATTLE) {
		return; /* placed every frame by the battle */
	}
	const EnvSpaceScene* space = env_space();
	if (space) {
		for (int i = 0; i < space->count && s_env.count < ENV_MAX_INSTANCES; ++i) {
			const int c = env_find(space->ships[i].opt);
			if (c < 0) {
				continue;
			}
			float pos[3];
			env_ship_pos(i, pos);
			s_env.mesh_of[s_env.count] = c;
			s_env.ship_of[s_env.count] = i;
			s_env.far[s_env.count] = space->ships[i].dist > 150.0f;
			float n[3], r[3], u[3];
			env_ship_basis(i, n, r, u);
			env_pose_matrix(n, u, pos, s_env.transform[s_env.count]);
			s_env.count++;
		}
		return;
	}
	/* The hangar at the launch reference (pitch 0x4000 = level). */
	env_add(env_find("Hangar"), 0x4000, 0, 0.0f, 0.0f, 0.0f);
	/* The player's craft where the classic parks it: 800 units behind the
	 * launch reference, standing on the floor. */
	if (env_craft()) {
		const int c = env_find(env_craft());
		if (c >= 0) {
			env_add(c, 0x4000, 0, 0.0f, -800.0f, s_env.floor_z + 0.5f * s_env.meshes[c].size_z);
		}
	}
	for (int i = 0; i < ENV_PROP_COUNT; ++i) {
		const EnvProp* p = &k_props[i];
		const int c = env_find(p->opt);
		if (c < 0) {
			continue;
		}
		const float z =
			p->z == ENV_ON_FLOOR ? s_env.floor_z + 0.5f * s_env.meshes[c].size_z : (float)p->z;
		env_add(c, 0x4000 + p->pitch, p->yaw, (float)p->x, (float)p->y, z);
	}
}

/* ---- sprite textures -------------------------------------------------- */

static float env_srgb_to_linear(float c) {
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float env_linear_to_srgb(float c) {
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* Straight-alpha sRGB RGBA -> premultiplied (in linear light) sRGB texture,
 * the billboards' PMA convention. */
static int env_make_texture(AeronCommandBuffer* cmd, int slot, const uint8_t* rgba, int w, int h) {
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
		return 0;
	}
	uint8_t* px = (uint8_t*)malloc((size_t)w * (size_t)h * 4u);
	if (!px) {
		return 0;
	}
	float lut[256];
	for (int i = 0; i < 256; ++i) {
		lut[i] = env_srgb_to_linear((float)i / 255.0f);
	}
	for (size_t i = 0; i < (size_t)w * (size_t)h; ++i) {
		const float a = (float)rgba[i * 4 + 3] / 255.0f;
		for (int c = 0; c < 3; ++c) {
			px[i * 4 + c] = (uint8_t)(env_linear_to_srgb(lut[rgba[i * 4 + c]] * a) * 255.0f + 0.5f);
		}
		px[i * 4 + 3] = rgba[i * 4 + 3];
	}
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = w,
		.height = h,
		.mip_count = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_SRGB,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "xwa.vr_env.sprite",
	});
	const int ok = t && Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
		.texture = t,
		.width = w,
		.height = h,
		.raw_data = px,
		.raw_size = (size_t)w * (size_t)h * 4u,
	});
	free(px);
	if (!ok) {
		if (t) {
			Aeron_DestroyTexture(t);
		}
		return 0;
	}
	s_env.tex[slot] = t;
	s_env.tex_w[slot] = w;
	s_env.tex_h[slot] = h;
	return 1;
}

/* The beam profile: a bright core inside a soft glow across V, tapered at
 * both ends along U (additive; colour comes from the tint). */
static int env_make_beam(AeronCommandBuffer* cmd) {
	enum { BW = 64, BH = 32 };
	uint8_t px[BW * BH * 4];
	for (int y = 0; y < BH; ++y) {
		const float v = fabsf(((float)y + 0.5f) / (float)BH * 2.0f - 1.0f);
		for (int x = 0; x < BW; ++x) {
			float along = fminf(((float)x + 0.5f), ((float)BW - (float)x - 0.5f)) / 8.0f;
			along = fminf(along, 1.0f);
			const float core = expf(-v * v * 30.0f), glow = 0.45f * expf(-v * v * 4.0f);
			const float a = fminf(core + glow, 1.0f) * along;
			uint8_t* p = &px[(y * BW + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(a * 255.0f + 0.5f);
			p[3] = 255;
		}
	}
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = BW,
		.height = BH,
		.mip_count = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "xwa.vr_env.beam",
	});
	if (!t || !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
			.texture = t,
			.width = BW,
			.height = BH,
			.raw_data = px,
			.raw_size = sizeof px,
		})) {
		if (t) {
			Aeron_DestroyTexture(t);
		}
		return 0;
	}
	s_env.tex[ENV_TEX_BEAM] = t;
	s_env.tex_w[ENV_TEX_BEAM] = BW;
	s_env.tex_h[ENV_TEX_BEAM] = BH;
	return 1;
}

/* Value noise on a wrapping lattice, for the nebula. */
static float env_hash(int x, int y, int seed) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (float)((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

static float env_noise(float x, float y, int seed) {
	const int xi = (int)floorf(x), yi = (int)floorf(y);
	float fx = x - (float)xi, fy = y - (float)yi;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	const float a = env_hash(xi, yi, seed), b = env_hash(xi + 1, yi, seed);
	const float c = env_hash(xi, yi + 1, seed), d = env_hash(xi + 1, yi + 1, seed);
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float env_fbm(float x, float y, int seed, int octaves) {
	float sum = 0.0f, amp = 0.5f, norm = 0.0f;
	for (int o = 0; o < octaves; ++o) {
		sum += amp * env_noise(x, y, seed + o * 17);
		norm += amp;
		x *= 2.03f;
		y *= 2.03f;
		amp *= 0.5f;
	}
	return sum / norm;
}

/* The box art's nebula: a long magenta-violet cloud, brighter pink
 * filaments in its core, ragged dark lanes, fading to nothing at the edges
 * (straight sRGB RGBA; made once, ~10 ms). */
static int env_make_nebula(AeronCommandBuffer* cmd) {
	enum { NW = 512, NH = 256 };
	uint8_t* px = (uint8_t*)malloc((size_t)NW * NH * 4u);
	if (!px) {
		return 0;
	}
	for (int y = 0; y < NH; ++y) {
		for (int x = 0; x < NW; ++x) {
			const float u = ((float)x + 0.5f) / (float)NW * 2.0f - 1.0f;
			const float v = ((float)y + 0.5f) / (float)NH * 2.0f - 1.0f;
			/* Domain warp for billowing shapes. */
			const float wx = env_fbm(u * 3.0f, v * 1.5f, 7, 4) - 0.5f;
			const float wy = env_fbm(u * 3.0f + 5.2f, v * 1.5f + 1.3f, 11, 4) - 0.5f;
			const float n = env_fbm(u * 4.0f + wx * 2.0f, v * 2.0f + wy * 2.0f, 3, 6);
			const float fil = env_fbm(u * 10.0f + wx * 4.0f, v * 5.0f + wy * 4.0f, 23, 5);
			/* Elliptical falloff, bent a little along its length. */
			const float bend = v - 0.25f * sinf(u * 2.2f);
			const float r = sqrtf(u * u + bend * bend * 1.6f);
			const float edge = fmaxf(0.0f, 1.0f - r);
			float density = fmaxf(0.0f, n - 0.32f) * 2.2f * edge * edge;
			const float lanes = fmaxf(0.0f, 0.55f - env_fbm(u * 6.0f + 3.1f, v * 3.0f - wx * 3.0f, 41, 5)) * 1.8f;
			density = fmaxf(0.0f, density - lanes * 0.35f * edge);
			const float core = powf(fmaxf(0.0f, fil - 0.45f) * 2.0f, 2.0f) * density;
			/* Violet body, magenta-pink to white-hot filaments. */
			float rr = 0.66f * density + 1.0f * core, gg = 0.18f * density + 0.62f * core,
				  bb = 0.80f * density + 0.90f * core;
			const float a = fminf(1.0f, density * 1.4f + core);
			uint8_t* p = &px[((size_t)y * NW + x) * 4];
			/* Straight alpha: the colour without coverage. */
			const float inv = a > 1e-4f ? 1.0f / a : 0.0f;
			p[0] = (uint8_t)(fminf(1.0f, rr * inv) * 255.0f + 0.5f);
			p[1] = (uint8_t)(fminf(1.0f, gg * inv) * 255.0f + 0.5f);
			p[2] = (uint8_t)(fminf(1.0f, bb * inv) * 255.0f + 0.5f);
			p[3] = (uint8_t)(a * 255.0f + 0.5f);
		}
	}
	const int ok = env_make_texture(cmd, ENV_TEX_NEBULA, px, NW, NH);
	free(px);
	return ok;
}

/* Balance of Power's world: a mottled brown globe, cloud bands swirling
 * across it, lit from the upper left, its right limb in night, a faint dark
 * ring's line round it (straight sRGB RGBA; made once). */
static int env_make_planet(AeronCommandBuffer* cmd) {
	enum { PN = 512 };
	uint8_t* px = (uint8_t*)malloc((size_t)PN * PN * 4u);
	if (!px) {
		return 0;
	}
	const float R = 0.80f;
	float light[3] = { -0.80f, 0.40f, 0.45f }; /* x right, y up, z toward the viewer */
	env_norm(light);
	for (int y = 0; y < PN; ++y) {
		for (int x = 0; x < PN; ++x) {
			const float u = ((float)x + 0.5f) / (float)PN * 2.0f - 1.0f;
			const float v = 1.0f - ((float)y + 0.5f) / (float)PN * 2.0f;
			const float r = sqrtf(u * u + v * v);
			float rgb[3] = { 0.0f, 0.0f, 0.0f }, a = 0.0f;
			if (r < R) {
				const float nx = u / R, ny = v / R, nz = sqrtf(fmaxf(0.0f, 1.0f - nx * nx - ny * ny));
				/* Bands tilted a little, warped into swirls. */
				const float bx = nx * 0.95f + ny * 0.31f, by = ny * 0.95f - nx * 0.31f;
				const float w = env_fbm(bx * 2.5f + 3.0f, by * 2.5f, 51, 5);
				const float band = 0.5f + 0.5f * sinf(by * 9.0f + w * 6.0f);
				const float d = env_fbm(bx * 6.0f + w * 2.0f, by * 6.0f - w, 57, 5);
				const float m = fminf(fmaxf(0.45f * band + 0.55f * d, 0.0f), 1.0f);
				static const float dark[3] = { 0.20f, 0.12f, 0.08f }, light_c[3] = { 0.62f, 0.46f, 0.36f },
								   pale[3] = { 0.78f, 0.66f, 0.58f };
				const float lit_dot = nx * light[0] + ny * light[1] + nz * light[2];
				const float lit = fmaxf(0.0f, lit_dot) * 0.95f + 0.03f;
				for (int c = 0; c < 3; ++c) {
					float al = dark[c] + (light_c[c] - dark[c]) * m;
					al += (pale[c] - al) * fmaxf(0.0f, d - 0.62f) * 2.0f;
					rgb[c] = al * lit;
				}
				a = fminf(1.0f, (R - r) * (float)PN * 0.5f);
			}
			/* The ring's thin dark line just outside the limb (and its glint
			 * on the lit side). */
			const float ring = expf(-powf((r - 0.86f) / 0.006f, 2.0f));
			if (ring > 0.01f && r >= R) {
				const float lit = fmaxf(0.0f, -(u / r) * 0.8f + (v / r) * 0.3f);
				rgb[0] = rgb[1] = rgb[2] = 0.0f;
				rgb[0] = 0.10f + 0.25f * lit;
				rgb[1] = 0.08f + 0.18f * lit;
				rgb[2] = 0.10f + 0.20f * lit;
				a = 0.55f * ring;
			}
			uint8_t* p = &px[((size_t)y * PN + x) * 4];
			for (int c = 0; c < 3; ++c) {
				p[c] = (uint8_t)(env_linear_to_srgb(fminf(1.0f, rgb[c])) * 255.0f + 0.5f);
			}
			p[3] = (uint8_t)(fminf(fmaxf(a, 0.0f), 1.0f) * 255.0f + 0.5f);
		}
	}
	const int ok = env_make_texture(cmd, ENV_TEX_PLANET, px, PN, PN);
	free(px);
	return ok;
}

/* Image assets: "XWAL", u32 width, u32 height, straight RGBA. Shipped in
 * the APK (assets/vr/<name>.rgba) or next to the executable; never part of
 * the source tree. `override_env` names a variable that overrides the path.
 *   logo  — the game logo above the screen (XWA_XR_LOGO)
 *   frame — the frame around the 4:3 screen (XWA_XR_FRAME, 0 = none); its
 *           opening is the rectangle k_frame_opening in image pixels.
 *   vader — the helmet behind the TIE Fighter scene (XWA_XR_VADER), grey
 *           (tinted here), faded out at the bottom.
 *   xvt_tie_pilot, xvt_rebel_pilot — the two pilots behind the X-Wing vs.
 *           TIE Fighter scene (XWA_XR_TIE_PILOT, XWA_XR_REBEL_PILOT), the
 *           rebel looking left, vignetted to transparent at the edges. */
static int env_load_rgba(AeronCommandBuffer* cmd, int slot, const char* name, const char* override_env) {
	const char* override = SDL_getenv(override_env);
	if (override && strcmp(override, "0") == 0) {
		return 0;
	}
	char asset[2][64];
	snprintf(asset[0], sizeof asset[0], "vr/%s.rgba", name);
	snprintf(asset[1], sizeof asset[1], "assets/vr/%s.rgba", name);
	const char* paths[] = { override && override[0] ? override : NULL, asset[0], asset[1] };
	for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
		if (!paths[i]) {
			continue;
		}
		size_t size = 0;
		uint8_t* data = (uint8_t*)SDL_LoadFile(paths[i], &size);
		if (!data) {
			continue;
		}
		int ok = 0;
		if (size >= 12 && memcmp(data, "XWAL", 4) == 0) {
			uint32_t w, h;
			memcpy(&w, data + 4, 4);
			memcpy(&h, data + 8, 4);
			if (w && h && w <= 4096 && h <= 4096 && size >= 12 + (size_t)w * h * 4u) {
				ok = env_make_texture(cmd, slot, data + 12, (int)w, (int)h);
			}
		}
		SDL_free(data);
		if (ok) {
			return 1;
		}
	}
	Aeron_LogInfo("xwa.remaster", "VR environment: no %s (vr/%s.rgba)", name, name);
	return 0;
}

static void env_load_dat_sprite(AeronCommandBuffer* cmd, int slot) {
	const EnvTexSource* src = &k_tex_sources[slot];
	/* The one DAT directly (the RESDATA.TXT index would read them all). */
	char path[2][64];
	snprintf(path[0], sizeof path[0], "Resdata/%s.dat", src->dat);
	snprintf(path[1], sizeof path[1], "RESDATA/%s.DAT", src->dat);
	for (char* c = path[1]; *c; ++c) {
		*c = (char)SDL_toupper((unsigned char)*c);
	}
	AeronVfs* vfs = Aeron_GetVfs();
	uint8_t* bytes = NULL;
	size_t size = 0;
	for (int i = 0; i < 2 && !bytes; ++i) {
		if (vfs && AeronVfs_Exists(vfs, AERON_VFS_ROOT_ASSET, path[i]) &&
			!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, path[i], 64u << 20, &bytes, &size)) {
			bytes = NULL;
		}
	}
	if (!bytes) {
		Aeron_LogWarn("xwa.remaster", "VR environment: %s unavailable", path[0]);
		return;
	}
	Xwa2dFrameSet set;
	memset(&set, 0, sizeof set);
	char error[256];
	error[0] = 0;
	const int ok = Xwa2d_DatAppendGroup(bytes, size, (uint16_t)src->group, &set, error, sizeof error);
	free(bytes);
	if (!ok || !set.count) {
		Aeron_LogWarn("xwa.remaster", "VR environment: DAT group %d unavailable: %s", src->group, error);
		Xwa2dFrameSet_Free(&set);
		return;
	}
	const Xwa2dFrame* pick = NULL;
	for (int i = 0; i < set.count; ++i) {
		const Xwa2dFrame* f = &set.frames[i];
		if (src->sprite < 0 ? (!pick || f->sprite_id < pick->sprite_id) : f->sprite_id == src->sprite) {
			pick = f;
		}
	}
	if (pick && pick->rgba) {
		env_make_texture(cmd, slot, pick->rgba, pick->width, pick->height);
	}
	Xwa2dFrameSet_Free(&set);
}

/* The sprites a scene uses. */
static int env_tex_needed(int slot) {
	switch (s_env.mode) {
		case ENV_ENDOR:
			return slot != ENV_TEX_NEBULA && slot != ENV_TEX_VADER && slot != ENV_TEX_TIE_PILOT &&
				   slot != ENV_TEX_REBEL_PILOT && slot < ENV_TEX_GLOW;
		case ENV_BOXART:
			return slot != ENV_TEX_DEATHSTAR && slot != ENV_TEX_ENDOR && slot != ENV_TEX_VADER &&
				   slot != ENV_TEX_TIE_PILOT && slot != ENV_TEX_REBEL_PILOT && slot < ENV_TEX_GLOW;
		case ENV_TIEART:
			/* Its animation also uses the glow and the explosion's frames. */
			return slot != ENV_TEX_DEATHSTAR && slot != ENV_TEX_ENDOR && slot != ENV_TEX_TIE_PILOT &&
				   slot != ENV_TEX_REBEL_PILOT && slot != ENV_TEX_PLANET && slot < ENV_TEX_BACKDROP;
		case ENV_DOEART:
			/* The TIE pilot's helmet in place of Vader's. */
			return slot != ENV_TEX_DEATHSTAR && slot != ENV_TEX_ENDOR && slot != ENV_TEX_VADER &&
				   slot != ENV_TEX_REBEL_PILOT && slot != ENV_TEX_PLANET && slot < ENV_TEX_BACKDROP;
		case ENV_BOPART:
			/* No helmet: the dreadnought before its world. */
			return slot != ENV_TEX_DEATHSTAR && slot != ENV_TEX_ENDOR && slot != ENV_TEX_VADER &&
				   slot != ENV_TEX_TIE_PILOT && slot != ENV_TEX_REBEL_PILOT && slot != ENV_TEX_NEBULA &&
				   slot < ENV_TEX_BACKDROP;
		case ENV_XVTART:
			return slot != ENV_TEX_DEATHSTAR && slot != ENV_TEX_ENDOR && slot != ENV_TEX_NEBULA &&
				   slot != ENV_TEX_VADER && slot < ENV_TEX_GLOW;
		case ENV_BATTLE:
			return bt_tex_needed(slot) || (slot == ENV_TEX_LOGO && env_title_active());
		default:
			return slot == ENV_TEX_FRAME || (slot == ENV_TEX_LOGO && env_title_active());
	}
}

/* One sprite per call; returns 1 when something was loaded. */
static int env_load_sprites(AeronCommandBuffer* cmd) {
	for (int i = 0; i < ENV_TEX_COUNT; ++i) {
		if (s_env.tex_tried[i] || !env_tex_needed(i)) {
			continue;
		}
		s_env.tex_tried[i] = 1;
		if ((s_env.mode == ENV_BATTLE || env_played(s_env.mode)) && bt_load_texture(cmd, i)) {
			/* the battle's own (glow, explosion frames, backdrops) */
		} else if (i == ENV_TEX_BEAM) {
			env_make_beam(cmd);
		} else if (i == ENV_TEX_NEBULA) {
			env_make_nebula(cmd);
		} else if (i == ENV_TEX_PLANET) {
			env_make_planet(cmd);
		} else if (i == ENV_TEX_LOGO) {
			env_load_rgba(cmd, i, "logo", "XWA_XR_LOGO");
		} else if (i == ENV_TEX_FRAME) {
			env_load_rgba(cmd, i, "frame", "XWA_XR_FRAME");
		} else if (i == ENV_TEX_VADER) {
			env_load_rgba(cmd, i, "vader", "XWA_XR_VADER");
		} else if (i == ENV_TEX_TIE_PILOT) {
			env_load_rgba(cmd, i, "xvt_tie_pilot", "XWA_XR_TIE_PILOT");
		} else if (i == ENV_TEX_REBEL_PILOT) {
			env_load_rgba(cmd, i, "xvt_rebel_pilot", "XWA_XR_REBEL_PILOT");
		} else {
			env_load_dat_sprite(cmd, i);
		}
		return 1;
	}
	return 0;
}

/* Another scene in place of this one (Y, or the VR options): everything of
 * the old scene goes, but the frame and logo images (the same for every
 * scene) and the screen's state stay. */
static void env_swap_release(void) {
	static const int keep[] = { ENV_TEX_LOGO, ENV_TEX_FRAME };
	AeronTexture* tex[2];
	int w[2], h[2], tried[2];
	const int away = s_dismiss.away;
	const float level = s_dismiss.level, logo = s_dismiss.logo;
	for (int i = 0; i < 2; ++i) {
		tex[i] = s_env.tex[keep[i]];
		w[i] = s_env.tex_w[keep[i]];
		h[i] = s_env.tex_h[keep[i]];
		tried[i] = s_env.tex_tried[keep[i]];
		s_env.tex[keep[i]] = NULL; /* not destroyed with the rest */
	}
	XwaVrEnv_Release();
	for (int i = 0; i < 2; ++i) {
		s_env.tex[keep[i]] = tex[i];
		s_env.tex_w[keep[i]] = w[i];
		s_env.tex_h[keep[i]] = h[i];
		s_env.tex_tried[keep[i]] = tex[i] ? tried[i] : 0;
	}
	s_dismiss.away = away;
	s_dismiss.level = level;
	s_dismiss.logo = logo;
	s_env_swapped = 1;
}

int XwaVrEnv_Prepare(AeronCommandBuffer* cmd) {
	if (!cmd) {
		return 0;
	}
	const EnvMode mode = env_mode();
	if (mode != s_env.mode) {
		if (s_env.mode != ENV_OFF && mode != ENV_OFF) {
			env_swap_release();
		} else {
			XwaVrEnv_Release();
		}
		s_env.mode = mode;
	}
	if (mode == ENV_OFF) {
		return 0;
	}
	env_list();
	if (env_title_active()) {
		env_title_text(cmd);
	} else {
		env_hint_text(cmd);
		env_prompt_text(cmd);
	}
	/* One OPT (or sprite) per frame. */
	int busy = 0;
	for (int i = 0; i < s_env.mesh_count; ++i) {
		if (!s_env.meshes[i].tried) {
			env_load(cmd, i);
			s_env.laid_out = 0;
			busy = 1;
			break;
		}
	}
	if (!busy) {
		(void)env_load_sprites(cmd);
	}
	if (mode == ENV_HANGAR && !s_env.meshes[s_env.hangar].mesh) {
		if (s_env.meshes[s_env.hangar].tried && !s_env.logged) {
			Aeron_LogWarn("xwa.remaster", "VR environment: Hangar.opt unavailable, menus stay on the plain screen");
			s_env.logged = 1;
		}
		return 0;
	}
	if (!s_env.laid_out) {
		env_layout();
		s_env.laid_out = 1;
	}
	return 1;
}

void XwaVrEnv_View(float pos[3], float rows[9]) {
	/* XWA_XR_ENV_VIEW = x,y,yaw_degrees,eye_height_m (yaw 0 faces +y, the
	 * way the parked craft points; positive turns right). The space scene
	 * is laid out around the viewer at the origin. */
	float x = -500.0f, y = -1150.0f, yaw = 0.0f, eye = 1.7f;
	const char* v = SDL_getenv("XWA_XR_ENV_VIEW");
	if (env_is_space(s_env.mode)) {
		x = y = 0.0f;
	} else if (v && v[0]) {
		sscanf(v, "%f,%f,%f,%f", &x, &y, &yaw, &eye);
	}
	const float r = yaw * 3.14159265358979f / 180.0f;
	const float f[3] = { sinf(r), cosf(r), 0.0f };
	/* XWA_XR_ENV_BACK: the viewer sits this far (metres) behind the scene's
	 * reference point, matching the screen being as much further away than
	 * its original 2.1 m (the Quest default screen distance includes it). */
	/* The X-Wing vs. TIE Fighter scene: XWA_XR_XVT_BACK (0.4 m) further
	 * back still; the screen and frame keep their distance (they go with
	 * the viewer). */
	const float back = (env_float("XWA_XR_ENV_BACK", 1.1f) +
						(s_env.mode == ENV_XVTART ? env_float("XWA_XR_XVT_BACK", 0.4f) : 0.0f)) *
					   AERON_OPT_UNITS_PER_METER;
	pos[0] = x - f[0] * back;
	pos[1] = y - f[1] * back;
	pos[2] = env_is_space(s_env.mode) ? 0.0f : s_env.floor_z + eye * AERON_OPT_UNITS_PER_METER;
	memcpy(s_env.view_pos, pos, sizeof s_env.view_pos);
	rows[0] = f[1];
	rows[1] = -f[0];
	rows[2] = 0.0f; /* right */
	rows[3] = 0.0f;
	rows[4] = 0.0f;
	rows[5] = -1.0f; /* down */
	rows[6] = f[0];
	rows[7] = f[1];
	rows[8] = 0.0f; /* forward */
}

/* ---- billboards --------------------------------------------------------- */

/* The fade quads get: the scene's, or the frame / logo / hints' own while
 * those are drawn (s_quad_chrome). */
static int s_quad_chrome;
static float env_quad_fade(void) { return s_quad_chrome ? env_chrome_fade() : s_env_fade; }

/* Queue a quad: corners (world units) bottom-left, bottom-right, top-right,
 * top-left; tint linear RGBA (HDR allowed). */
static void env_quad_tex(AeronScene3D* scene, AeronTexture* tex, AeronSceneBillboardStage stage,
						 AeronSceneBillboardBlend blend, const float c[4][3], const float tint[4]) {
	if (!tex) {
		return;
	}
	static const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
	AeronSceneBillboardDesc d;
	memset(&d, 0, sizeof d);
	d.texture = tex;
	d.blend = blend;
	d.stage = stage;
	for (int v = 0; v < 4; ++v) {
		memcpy(d.corners[v], c[v], sizeof d.corners[v]);
		memcpy(d.uv[v], uv[v], sizeof d.uv[v]);
		/* PMA: alpha scales the colour too. */
		/* The scene's fade in: PMA fades its opacity (colour with it),
		 * additive its light. */
		for (int k = 0; k < 3; ++k) {
			d.colors[v][k] = tint[k] * (blend == AERON_SCENE_BILLBOARD_BLEND_PMA ? tint[3] : 1.0f) * env_quad_fade();
		}
		d.colors[v][3] = tint[3] * (blend == AERON_SCENE_BILLBOARD_BLEND_PMA ? env_quad_fade() : 1.0f);
	}
	AeronScene_AddBillboard(scene, &d);
}

static void env_quad(AeronScene3D* scene, int slot, AeronSceneBillboardStage stage, AeronSceneBillboardBlend blend,
					 const float c[4][3], const float tint[4]) {
	env_quad_tex(scene, s_env.tex[slot], stage, blend, c, tint);
}

/* Corners of a rectangle at `center` spanned by half-axes `ax` (right) and
 * `ay` (up). */
static void env_rect(const float center[3], const float ax[3], const float ay[3], float c[4][3]) {
	for (int k = 0; k < 3; ++k) {
		c[0][k] = center[k] - ax[k] - ay[k];
		c[1][k] = center[k] + ax[k] - ay[k];
		c[2][k] = center[k] + ax[k] + ay[k];
		c[3][k] = center[k] - ax[k] + ay[k];
	}
}

/* A viewer-facing square of `size` (world units) at `center`, rolled by
 * `roll` degrees. */
static void env_facing(const float center[3], float size, float roll, float c[4][3]) {
	float f[3] = { center[0] - s_env.view_pos[0], center[1] - s_env.view_pos[1], center[2] - s_env.view_pos[2] };
	env_norm(f);
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	float r[3], u[3];
	env_cross(f, z, r);
	env_norm(r);
	env_cross(r, f, u);
	const float rr = roll * 3.14159265358979f / 180.0f, cr = cosf(rr), sr = sinf(rr);
	float ax[3], ay[3];
	for (int k = 0; k < 3; ++k) {
		ax[k] = (r[k] * cr + u[k] * sr) * 0.5f * size;
		ay[k] = (u[k] * cr - r[k] * sr) * 0.5f * size;
	}
	env_rect(center, ax, ay, c);
}

/* Sky sprites are drawn at the far plane; the distance only sets the
 * projection direction. */
#define ENV_SKY_DIST 2.0e6f

static void env_sky_point(float yaw, float pitch, float out[3]) {
	env_dir(yaw, pitch, out);
	for (int k = 0; k < 3; ++k) {
		out[k] = s_env.view_pos[k] + out[k] * ENV_SKY_DIST;
	}
}

/* A sky sprite of `size_deg` angular size toward yaw/pitch. */
static void env_sky_sprite(AeronScene3D* scene, int slot, float yaw, float pitch, float size_deg, float roll,
						   const float tint[4], float corners[4][3]) {
	float center[3];
	env_sky_point(yaw, pitch, center);
	env_facing(center, 2.0f * ENV_SKY_DIST * tanf(size_deg * 3.14159265358979f / 360.0f), roll, corners);
	env_quad(scene, slot, AERON_SCENE_BILLBOARD_STAGE_SKY, AERON_SCENE_BILLBOARD_BLEND_PMA, corners, tint);
}

/* A beam strip from `a` to `b` (world units), `width` wide, facing the
 * viewer. */
static void env_beam(AeronScene3D* scene, AeronSceneBillboardStage stage, const float a[3], const float b[3],
					 float width, const float tint[4]) {
	float along[3], mid[3], side[3];
	for (int k = 0; k < 3; ++k) {
		along[k] = b[k] - a[k];
		mid[k] = 0.5f * (a[k] + b[k]) - s_env.view_pos[k];
	}
	env_cross(along, mid, side);
	env_norm(side);
	float c[4][3];
	for (int k = 0; k < 3; ++k) {
		side[k] *= 0.5f * width;
		c[0][k] = a[k] - side[k];
		c[1][k] = b[k] - side[k];
		c[2][k] = b[k] + side[k];
		c[3][k] = a[k] + side[k];
	}
	env_quad(scene, ENV_TEX_BEAM, stage, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, c, tint);
}

/* The frame's opening (image pixels) that the 4:3 screen fills exactly;
 * the frame image is drawn behind the screen's quad layer, in its plane. */
static const float k_frame_opening[4] = { 114.0f, 144.0f, 1133.0f, 908.0f }; /* x0, y0, x1, y1 */

/* Where the 4:3 picture sits on the virtual screen: the 2D layers are
 * fitted into the screen quad keeping their aspect, centred. Lengths in
 * real metres; `k` converts them to scene units at the head's scale
 * (XWA_XR_WORLD_SCALE scales tracked head motion, so the screen's real
 * distance is k * metres for the stereo to agree with the quad layer). */
typedef struct EnvScreen {
	float dist, w, h, lift, k;
	/* The picture as the frame's opening shows it: the screen's picture
	 * divided by XWA_XR_FRAME_OVERSCAN (1.0: the picture fills the opening
	 * exactly). The opening is the frame's clear 4:3 area enlarged 2 %: the
	 * bezel overlaps under 1 % of the picture's edges (no menu text), dark
	 * backing fills any gap. The
	 * frame sits XWA_XR_FRAME_DEPTH (2 cm) in front of the picture, so the
	 * whole picture is set back in it, scaled so its opening lines up from
	 * the viewer (leaning 20 cm shifts the two by under 8 mm, which the
	 * bezel's overlap still covers). */
	float pic_w, pic_h;
	float frame_dist, frame_scale;
} EnvScreen;

static void env_screen(EnvScreen* out) {
	float dist = 2.1f, sw = 2.1f, sh = 1.575f, lift = 0.0f;
	if (!Aeron_XrVirtualScreenGeometry(&dist, &sw, &sh, &lift)) {
		dist = env_float("AERON_XR_SCREEN_DISTANCE", dist);
		sw = env_float("AERON_XR_SCREEN_WIDTH", sw);
		sh = 0.75f * sw;
		lift = env_float("AERON_XR_SCREEN_HEIGHT_OFFSET", lift);
	}
	out->w = fminf(sw, sh * 4.0f / 3.0f);
	out->h = 0.75f * out->w;
	out->dist = dist;
	out->lift = lift;
	out->k = AERON_OPT_UNITS_PER_METER * env_float("XWA_XR_WORLD_SCALE", 1.0f);
	const float overscan = fmaxf(env_float("XWA_XR_FRAME_OVERSCAN", 1.0f), 0.5f);
	out->pic_w = out->w / overscan;
	out->pic_h = out->h / overscan;
	out->frame_dist = fmaxf(dist - env_float("XWA_XR_FRAME_DEPTH", 0.02f), 0.2f);
	out->frame_scale = dist > 0.01f ? out->frame_dist / dist : 1.0f;
}

/* Height (metres, in the picture's plane) the frame rises above the top of
 * the picture. */
static float env_frame_top(const EnvScreen* sc) {
	if (!s_env.tex[ENV_TEX_FRAME]) {
		return 0.0f;
	}
	return k_frame_opening[1] * sc->pic_w / (k_frame_opening[2] - k_frame_opening[0]);
}

/* The frame quad in head-relative metres: centre (x right, y up, z ahead)
 * and size. */
static void env_frame_quad(const EnvScreen* sc, float centre[3], float size[2]) {
	const float px = sc->frame_scale * sc->pic_w / (k_frame_opening[2] - k_frame_opening[0]);
	const float tw = (float)s_env.tex_w[ENV_TEX_FRAME], th = (float)s_env.tex_h[ENV_TEX_FRAME];
	size[0] = tw * px;
	size[1] = th * px;
	/* Image centre relative to the opening's centre (image y runs down). */
	centre[0] = (0.5f * tw - 0.5f * (k_frame_opening[0] + k_frame_opening[2])) * px;
	centre[1] = sc->lift * sc->frame_scale - (0.5f * th - 0.5f * (k_frame_opening[1] + k_frame_opening[3])) * px;
	centre[2] = sc->frame_dist;
}

/* Fallback when there is no frame layer: the frame as a scene sprite
 * (under the picture, which the compositor draws on top). */
static void env_submit_frame(AeronScene3D* scene) {
	if (!s_env.tex[ENV_TEX_FRAME] || s_env.frame_layer_ok || XwaVrEnv_ScreenHidden()) {
		return;
	}
	EnvScreen sc;
	env_screen(&sc);
	float q[3], size[2];
	env_frame_quad(&sc, q, size);
	const float k = sc.k;
	const float center[3] = { s_env.view_pos[0] + q[0] * k, s_env.view_pos[1] + q[2] * k,
							  s_env.view_pos[2] + q[1] * k };
	const float ax[3] = { 0.5f * size[0] * k, 0.0f, 0.0f };
	const float ay[3] = { 0.0f, 0.0f, 0.5f * size[1] * k };
	float c[4][3];
	env_rect(center, ax, ay, c);
	static const float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	env_quad(scene, ENV_TEX_FRAME, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

int XwaVrEnv_LayerRect(float centre[3], float size[2]) {
	if (!centre || !size) {
		return 0;
	}
	EnvScreen sc;
	env_screen(&sc);
	if (sc.dist <= 0.0f || sc.w <= 0.0f || XwaVrEnv_ScreenHidden()) {
		return 0;
	}
	if (s_env.tex[ENV_TEX_FRAME] && s_env.frame_layer_ok && (!s_env_bare || s_env_swapped)) {
		float q[3];
		env_frame_quad(&sc, q, size);
		centre[0] = q[0];
		centre[1] = q[1];
		centre[2] = -q[2];
		/* The frame always takes in the picture behind it. */
		return 1;
	}
	centre[0] = 0.0f;
	centre[1] = sc.lift;
	centre[2] = -sc.dist;
	size[0] = sc.w;
	size[1] = sc.h;
	return 1;
}

int XwaVrEnv_CommitFrameLayer(AeronCommandBuffer* cmd, float brightness) {
	if ((s_env_bare && !s_env_swapped) || XwaVrEnv_ScreenHidden()) {
		return 0; /* no frame round the startup films, or while the TIE art plays */
	}
	if (!cmd || !s_env.tex[ENV_TEX_FRAME] || s_env.tex_w[ENV_TEX_FRAME] > AERON_XR_FRAME_WIDTH ||
		s_env.tex_h[ENV_TEX_FRAME] > AERON_XR_FRAME_HEIGHT) {
		s_env.frame_layer_ok = 0;
		return 0;
	}
	const float tint = fminf(fmaxf(brightness, 0.0f), 1.0f);
	const float fade = env_chrome_fade() * env_screen_reveal();
	if (!s_env.frame_drawn || fabsf(tint - s_env.frame_tint) > 0.004f || fabsf(fade - s_env.frame_fade) > 0.004f) {
		int w = 0, h = 0;
		AeronRenderTarget* target = Aeron_XrStereoPanelTarget(AERON_XR_PANEL_FRAME, &w, &h);
		if (!target) {
			s_env.frame_layer_ok = 0;
			return 0;
		}
		if (!s_env.frame_list) {
			s_env.frame_list = AeronDrawList_Create(8);
		}
		if (!s_env.frame_list) {
			s_env.frame_layer_ok = 0;
			return 0;
		}
		AeronDrawList_Begin(s_env.frame_list, target, w, h, AERON_DRAWLIST2D_CLEAR, NULL);
		AeronDrawList2DSprite sp;
		memset(&sp, 0, sizeof sp);
		sp.texture = s_env.tex[ENV_TEX_FRAME];
		sp.src_u1 = 1.0f;
		sp.src_v1 = 1.0f;
		sp.dst_w = (float)s_env.tex_w[ENV_TEX_FRAME];
		sp.dst_h = (float)s_env.tex_h[ENV_TEX_FRAME];
		/* Premultiplied: fading the opacity fades the colour with it. */
		sp.tint[0] = sp.tint[1] = sp.tint[2] = tint * fade;
		sp.tint[3] = fade;
		sp.blend = AERON_BLIT2D_BLEND_NONE; /* premultiplied texels as they are */
		sp.filter = AERON_BLIT2D_FILTER_NEAREST;
		AeronDrawList_AddSprite(s_env.frame_list, &sp);
		AeronDrawList_Render(s_env.frame_list, cmd);
		s_env.frame_drawn = 1;
		s_env.frame_tint = tint;
		s_env.frame_fade = fade;
	}
	EnvScreen sc;
	env_screen(&sc);
	float q[3], size[2];
	env_frame_quad(&sc, q, size);
	AeronXrPanel p;
	memset(&p, 0, sizeof p);
	p.rect[2] = s_env.tex_w[ENV_TEX_FRAME];
	p.rect[3] = s_env.tex_h[ENV_TEX_FRAME];
	p.position[0] = q[0];
	p.position[1] = q[1];
	p.position[2] = -q[2]; /* OpenXR: -Z ahead */
	p.orientation[3] = 1.0f;
	p.size[0] = size[0];
	p.size[1] = size[1];
	Aeron_XrStereoCommitPanelLayer(AERON_XR_PANEL_FRAME, &p, 1);
	s_env.frame_layer_ok = 1;
	return 1;
}

/* A sky sprite `w_deg` x `h_deg` toward yaw/pitch, rolled (for the
 * nebula's long clouds). */
static void env_sky_rect(AeronScene3D* scene, int slot, float yaw, float pitch, float w_deg, float h_deg, float roll,
						 const float tint[4]) {
	if (!s_env.tex[slot]) {
		return;
	}
	float center[3], f[3], r[3], u[3];
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	env_sky_point(yaw, pitch, center);
	for (int k = 0; k < 3; ++k) {
		f[k] = center[k] - s_env.view_pos[k];
	}
	env_norm(f);
	env_cross(f, z, r);
	env_norm(r);
	env_cross(r, f, u);
	const float rr = roll * 3.14159265358979f / 180.0f, cr = cosf(rr), sr = sinf(rr);
	const float hw = ENV_SKY_DIST * tanf(fminf(w_deg, 170.0f) * 3.14159265358979f / 360.0f);
	const float hh = ENV_SKY_DIST * tanf(fminf(h_deg, 170.0f) * 3.14159265358979f / 360.0f);
	float ax[3], ay[3], c[4][3];
	for (int k = 0; k < 3; ++k) {
		ax[k] = (r[k] * cr + u[k] * sr) * hw;
		ay[k] = (u[k] * cr - r[k] * sr) * hh;
	}
	env_rect(center, ax, ay, c);
	env_quad(scene, slot, AERON_SCENE_BILLBOARD_STAGE_SKY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

/* The dogfight: each salvo frozen along its shooter's nose. */
/* `reach` 0..1: how far the bolts have come out of the guns yet (the TIE
 * art's animation fires them into the cover's moment); 1 = the cover. */
static void env_submit_salvos(AeronScene3D* scene, float reach) {
	const EnvSpaceScene* sc = env_space();
	const float k = AERON_OPT_UNITS_PER_METER;
	for (int si = 0; si < sc->salvo_count; ++si) {
		const EnvSalvo* sv = &sc->salvos[si];
		float from[3], to[3], n[3], r[3], u[3];
		env_ship_pos(sv->shooter, from);
		env_ship_pos(sv->target, to);
		env_ship_basis(sv->shooter, n, r, u);
		const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
		const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
		if (env_played(s_env.mode)) {
			/* Out of the guns where the craft is now. */
			env_tie_pose(sv->shooter, s_env.anim_t, from, n, u);
			env_cross(n, u, r);
		}
		float reach_end = 0.0f;
		for (int sp = 0; sp < sv->span_count; ++sp) {
			reach_end = fmaxf(reach_end, sv->spans[sp][1]);
		}
		reach_end *= reach;
		static const float xw_guns[4][2] = { { 5.2f, 1.3f }, { -5.2f, 1.3f }, { 5.2f, -1.3f }, { -5.2f, -1.3f } };
		static const float tie_guns[2][2] = { { 0.6f, -1.0f }, { -0.6f, -1.0f } };
		static const float aw_guns[2][2] = { { 3.3f, 0.0f }, { -3.3f, 0.0f } };
		static const float int_guns[2][2] = { { 3.6f, 1.6f }, { -3.6f, 1.6f } };
		static const float red[4] = { 2.4f, 0.35f, 0.25f, 1.0f };
		static const float green[4] = { 0.4f, 2.4f, 0.45f, 1.0f };
		const float(*guns)[2] = xw_guns;
		int gun_count = 4;
		const float* tint = red;
		switch (sv->guns) {
			case ENV_GUNS_TIE:
				guns = tie_guns;
				gun_count = 2;
				tint = green;
				break;
			case ENV_GUNS_AWING:
				guns = aw_guns;
				gun_count = 2;
				break;
			case ENV_GUNS_INTERCEPTOR:
				guns = int_guns;
				gun_count = 2;
				tint = green;
				break;
			default:
				break;
		}
		for (int sp = 0; sp < sv->span_count; ++sp) {
			if (sv->spans[sp][0] >= reach_end) {
				continue;
			}
			const float end = fminf(sv->spans[sp][1], reach_end);
			for (int g = 0; g < gun_count; ++g) {
				float a0[3], a1[3];
				for (int a = 0; a < 3; ++a) {
					const float gun = from[a] + (r[a] * guns[g][0] + u[a] * guns[g][1]) * k;
					a0[a] = gun + n[a] * dist * sv->spans[sp][0];
					a1[a] = gun + n[a] * dist * end;
				}
				env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, a0, a1, 0.4f * k, tint);
			}
		}
	}
}

/* An explosion at `p` (world units), a little toward the viewer so the
 * depth test keeps the near half of a craft inside it visible. `scale` 1 =
 * a TIE's. */
static void env_submit_burst_at(AeronScene3D* scene, const float p[3], float scale) {
	const float k = AERON_OPT_UNITS_PER_METER;
	float f[3], r[3], c[4][3];
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	for (int a = 0; a < 3; ++a) {
		f[a] = p[a] - s_env.view_pos[a];
	}
	env_norm(f);
	env_cross(f, z, r);
	env_norm(r);
	static const float white[4] = { 1.5f, 1.4f, 1.3f, 1.0f };
	float q[3];
	for (int a = 0; a < 3; ++a) {
		q[a] = p[a] + (r[a] * 4.5f - f[a] * 5.0f) * k * scale;
	}
	env_facing(q, 13.0f * k * scale, 0.0f, c);
	env_quad(scene, ENV_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
	for (int a = 0; a < 3; ++a) {
		q[a] = p[a] + (r[a] * 5.5f + z[a] * 1.2f - f[a] * 5.5f) * k * scale;
	}
	env_facing(q, 7.0f * k * scale, 30.0f, c);
	env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
}

/* A ship bursting: the explosion over its right half. */
static void env_submit_burst(AeronScene3D* scene, int ship, float scale) {
	float p[3];
	env_ship_pos(ship, p);
	env_submit_burst_at(scene, p, scale);
}

/* The logo above the virtual screen and its frame. */
/* The logo in the frame's plane: above the frame, or (title) large where
 * the screen will be. `alpha` fades it (premultiplied). */
static void env_submit_logo_at(AeronScene3D* scene, float alpha, int title) {
	if (!s_env.tex[ENV_TEX_LOGO] || alpha <= 0.0f) {
		return;
	}
	s_quad_chrome = 1;
	EnvScreen sc;
	env_screen(&sc);
	float c[4][3];
	const float f = sc.frame_scale;
	const float lw = (title ? 0.9f : 0.6f) * sc.pic_w * f * env_float("XWA_XR_LOGO_SCALE", 1.0f);
	const float lh = lw * (float)s_env.tex_h[ENV_TEX_LOGO] / (float)s_env.tex_w[ENV_TEX_LOGO];
	const float up = title ? sc.lift * f : (sc.lift + 0.5f * sc.pic_h + env_frame_top(&sc)) * f + 0.05f + 0.5f * lh;
	const float center[3] = { s_env.view_pos[0], s_env.view_pos[1] + (sc.frame_dist + 0.02f) * sc.k,
							  s_env.view_pos[2] + up * sc.k };
	const float ax[3] = { 0.5f * lw * sc.k, 0.0f, 0.0f };
	const float ay[3] = { 0.0f, 0.0f, 0.5f * lh * sc.k };
	env_rect(center, ax, ay, c);
	const float tint[4] = { alpha, alpha, alpha, alpha };
	env_quad(scene, ENV_TEX_LOGO, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
	s_quad_chrome = 0;
}

/* Above the frame; hidden behind the startup title until the screen comes. */
static void env_submit_logo(AeronScene3D* scene) { env_submit_logo_at(scene, env_screen_reveal(), 0); }

static void env_submit_endor_sprites(AeronScene3D* scene) {
	const float k = AERON_OPT_UNITS_PER_METER;
	float c[4][3];
	/* Endor's rim below right, then the Death Star over it. */
	static const float endor_tint[4] = { 0.62f, 0.95f, 0.66f, 1.0f };
	env_sky_sprite(scene, ENV_TEX_ENDOR, 40.0f, -48.0f, 70.0f, 15.0f, endor_tint, c);
	static const float ds_tint[4] = { 0.9f, 0.9f, 1.0f, 1.0f };
	float ds[4][3];
	env_sky_sprite(scene, ENV_TEX_DEATHSTAR, ENV_DS_YAW, ENV_DS_PITCH, 22.1f, -10.0f, ds_tint, ds);
	/* The superlaser: from the dish (texel ~0.36, 0.31 of the sprite) to
	 * the burning cruiser. */
	if (s_env.tex[ENV_TEX_DEATHSTAR]) {
		const float u = 0.36f, v = 0.31f;
		float dish[3], target[3];
		for (int a = 0; a < 3; ++a) {
			const float bottom = ds[0][a] + (ds[1][a] - ds[0][a]) * u;
			const float top = ds[3][a] + (ds[2][a] - ds[3][a]) * u;
			dish[a] = top + (bottom - top) * v;
		}
		env_sky_point(k_endor[ENV_BURST].yaw, k_endor[ENV_BURST].pitch, target);
		const float wide = 2.0f * ENV_SKY_DIST * tanf(1.2f * 3.14159265358979f / 360.0f);
		const float thin = 2.0f * ENV_SKY_DIST * tanf(0.35f * 3.14159265358979f / 360.0f);
		static const float glow[4] = { 0.25f, 1.6f, 0.35f, 1.0f };
		static const float core[4] = { 1.2f, 2.2f, 1.2f, 1.0f };
		env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_SKY, dish, target, wide, glow);
		env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_SKY, dish, target, thin, core);
	}
	/* The cruiser bursting. */
	{
		float p[3], q[3];
		env_dir(k_endor[ENV_BURST].yaw, k_endor[ENV_BURST].pitch, p);
		env_dir(k_endor[ENV_BURST].yaw + 1.5f, k_endor[ENV_BURST].pitch + 1.0f, q);
		const float d = (k_endor[ENV_BURST].dist - 200.0f) * k;
		for (int a = 0; a < 3; ++a) {
			p[a] = s_env.view_pos[a] + p[a] * d;
			q[a] = s_env.view_pos[a] + q[a] * (d - 100.0f * k);
		}
		static const float white[4] = { 1.6f, 1.5f, 1.4f, 1.0f };
		env_facing(p, 2400.0f * k, 0.0f, c);
		env_quad(scene, ENV_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
		env_facing(q, 1300.0f * k, 40.0f, c);
		env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
	}
	env_submit_salvos(scene, 1.0f);
	/* The stricken TIE. */
	env_submit_burst(scene, ENV_VICTIM, 1.0f);
	/* Two bolts from the Falcon's top turret toward the TIE below the
	 * screen, frozen in flight. */
	{
		float from[3], to[3];
		env_ship_pos(ENV_FALCON, from);
		env_ship_pos(ENV_FALCON_TARGET, to);
		float up[3] = { k_endor[ENV_FALCON].up[0], k_endor[ENV_FALCON].up[1], k_endor[ENV_FALCON].up[2] };
		env_norm(up);
		for (int a = 0; a < 3; ++a) {
			from[a] += up[a] * 2.5f * k;
		}
		static const float red[4] = { 2.4f, 0.35f, 0.25f, 1.0f };
		static const float spans[2][2] = { { 0.05f, 0.15f }, { 0.24f, 0.34f } };
		for (int b = 0; b < 2; ++b) {
			float a0[3], a1[3];
			for (int a = 0; a < 3; ++a) {
				a0[a] = from[a] + (to[a] - from[a]) * spans[b][0];
				a1[a] = from[a] + (to[a] - from[a]) * spans[b][1];
			}
			env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, a0, a1, 0.6f * k, red);
		}
	}
	env_submit_logo(scene);
}

/* The X-Wing box art: the nebula, the fire and the burst. */
static void env_submit_boxart_sprites(AeronScene3D* scene) {
	/* The nebula: a long magenta cloud from the upper left down through
	 * the view to the lower right, as on the cover. XWA_XR_NEBULA_BRIGHTNESS
	 * scales it. */
	{
		const float b = env_float("XWA_XR_NEBULA_BRIGHTNESS", 1.0f);
		static const struct {
			float yaw, pitch, w, h, roll, gain;
		} clouds[] = {
			{ -10.0f, 12.0f, 120.0f, 62.0f, -32.0f, 1.15f }, /* the main band, behind the screen */
			{ -62.0f, 38.0f, 80.0f, 40.0f, -28.0f, 1.0f },   /* its upper left end */
			{ 42.0f, -30.0f, 90.0f, 44.0f, -38.0f, 0.9f },   /* lower right, behind the big X-wing */
		};
		for (size_t i = 0; i < sizeof clouds / sizeof clouds[0]; ++i) {
			const float g = clouds[i].gain * b;
			const float tint[4] = { g, g, g, 1.0f };
			env_sky_rect(scene, ENV_TEX_NEBULA, clouds[i].yaw, clouds[i].pitch, clouds[i].w, clouds[i].h, clouds[i].roll,
						 tint);
		}
	}
	env_submit_salvos(scene, 1.0f);
	{
		float p[3];
		env_dir(k_box_burst[0], k_box_burst[1], p);
		for (int a = 0; a < 3; ++a) {
			p[a] = s_env.view_pos[a] + p[a] * k_box_burst[2] * AERON_OPT_UNITS_PER_METER;
		}
		env_submit_burst_at(scene, p, 1.6f);
	}
	env_submit_logo(scene);
}

/* A great explosion centred on `at` (yaw, pitch, metres, diameter in
 * metres): the blast with two fire clouds in it. */
/* `spread` scales the fire clouds' offsets (degrees) and `step` their steps
 * toward the viewer (3 m): 1 for the TIE Fighter scene's size. */
static void env_submit_fireball_tint(AeronScene3D* scene, const float at[4], const float white[4],
									 float spread, float step) {
	const float k = AERON_OPT_UNITS_PER_METER;
	float p[3], c[4][3];
	static const struct {
		float dyaw, dpitch, size, roll;
		int tex;
	} puffs[] = {
		{ 0.0f, 0.0f, 1.0f, 0.0f, ENV_TEX_BLAST },
		{ -6.0f, 4.0f, 0.62f, 40.0f, ENV_TEX_FIRE },
		{ 7.0f, -3.0f, 0.55f, -60.0f, ENV_TEX_FIRE },
	};
	for (size_t i = 0; i < sizeof puffs / sizeof puffs[0]; ++i) {
		env_dir(at[0] + puffs[i].dyaw * spread, at[1] + puffs[i].dpitch * spread, p);
		for (int a = 0; a < 3; ++a) {
			p[a] = s_env.view_pos[a] + p[a] * (at[2] - (float)i * 3.0f * step) * k;
		}
		env_facing(p, at[3] * puffs[i].size * k, puffs[i].roll, c);
		env_quad(scene, puffs[i].tex, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
	}
}

static void env_submit_fireball(AeronScene3D* scene, const float at[4]) {
	static const float white[4] = { 1.5f, 1.4f, 1.3f, 1.0f };
	env_submit_fireball_tint(scene, at, white, 1.0f, 1.0f);
}

static void env_submit_streaks(AeronScene3D* scene, const EnvStreak* streaks, int count) {
	const float k = AERON_OPT_UNITS_PER_METER;
	static const float red[4] = { 2.4f, 0.35f, 0.25f, 1.0f };
	static const float green[4] = { 0.4f, 2.4f, 0.45f, 1.0f };
	for (int i = 0; i < count; ++i) {
		float a[3], b[3];
		env_dir(streaks[i].from[0], streaks[i].from[1], a);
		env_dir(streaks[i].to[0], streaks[i].to[1], b);
		for (int c = 0; c < 3; ++c) {
			a[c] = s_env.view_pos[c] + a[c] * streaks[i].from[2] * k;
			b[c] = s_env.view_pos[c] + b[c] * streaks[i].to[2] * k;
		}
		env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, a, b, 0.45f * k, streaks[i].green ? green : red);
	}
}

/* The TIE Fighter box art: the helmet and the clouds behind, the fire, the
 * great explosion with the X-wing breaking up in it. */
static float env_smooth(float x) {
	x = fminf(fmaxf(x, 0.0f), 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

/* The played fight's sprites: the bolts in flight, the cover's salvos
 * leaving the guns in the last half second, the victim hit, the great
 * explosion growing into the cover's with the victim catching fire as it
 * breaks up in it; then the cover's moment, held. Sizes go with the
 * explosion's (the TIE Fighter scene's 58 m). */
static void env_submit_fight(AeronScene3D* scene) {
	const float k = AERON_OPT_UNITS_PER_METER;
	const float t = s_env.anim_t;
	const float T = ENV_TIE_ANIM_SECONDS;
	const EnvAnim* an = env_anim();
	const float* blast = an->blast;
	const float D = blast[3];
	const float white[4] = { an->blast_tint[0], an->blast_tint[1], an->blast_tint[2], 1.0f };
	if (t >= T) {
		/* The cover's moment, held. */
		env_submit_salvos(scene, 1.0f);
		float q[3], c[4][3];
		env_submit_fireball_tint(scene, blast, white, D / blast[2] / (58.0f / 70.0f), D / 58.0f);
		env_ship_pos(an->victim, q);
		env_facing(q, 0.276f * D * k, 70.0f, c);
		env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
		env_facing(q, 0.172f * D * k, -20.0f, c);
		env_quad(scene, ENV_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
		/* (The startup title over the cover's moment: env_submit_title.) */
		env_submit_logo(scene);
		return;
	}
	/* The run-up: bolts in flight, the victim hit, then the blast; the
	 * cover's salvos leave the guns in the last half second. */
	env_tie_submit_bolts(scene);
	if (t > T - ENV_TIE_SALVO_SECONDS) {
		env_submit_salvos(scene, env_smooth((t - (T - ENV_TIE_SALVO_SECONDS)) / ENV_TIE_SALVO_SECONDS));
	}
	float xw[3], n[3], u[3], c[4][3];
	env_tie_pose(an->victim, t, xw, n, u);
	const float* hits = k_tie_hits;
	for (int h = 0; h < 3; ++h) {
		const float a = (t - hits[h]) / 0.35f;
		if (a > 0.0f && a < 1.0f) {
			const float tint[4] = { white[0], white[1], white[2], 1.0f - a };
			env_facing(xw, (0.0517f + 0.0862f * a) * D * k, 40.0f * (float)h, c);
			env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
		}
	}
	if (t >= ENV_TIE_BOOM_AT) {
		/* The great explosion growing into the cover's: the fighter
		 * explosion's frames 1..7 (7 is the cover's blast), the flames
		 * swelling with it, a flash at the start. */
		const float g = (t - ENV_TIE_BOOM_AT) / (T - ENV_TIE_BOOM_AT);
		int frame = (int)(g * 6.99f);
		frame = frame < 0 ? 0 : frame > 6 ? 6 : frame;
		const int slot = s_env.tex[ENV_TEX_BOOM + frame] ? ENV_TEX_BOOM + frame : ENV_TEX_BLAST;
		const float grow = 0.35f + 0.65f * env_smooth(g);
		float p[3];
		env_dir(blast[0], blast[1], p);
		for (int a = 0; a < 3; ++a) {
			p[a] = s_env.view_pos[a] + p[a] * blast[2] * k;
		}
		env_facing(p, D * grow * k, 0.0f, c);
		env_quad(scene, slot, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
		static const struct {
			float dyaw, dpitch, size, roll;
		} puffs[2] = { { -6.0f, 4.0f, 0.62f, 40.0f }, { 7.0f, -3.0f, 0.55f, -60.0f } };
		const float fire = env_smooth((g - 0.25f) / 0.75f);
		/* The puffs' offsets are the TIE Fighter scene's degrees at its size. */
		const float spread = D / blast[2] / (58.0f / 70.0f);
		for (int i = 0; i < 2 && fire > 0.0f; ++i) {
			float q[3];
			env_dir(blast[0] + puffs[i].dyaw * spread, blast[1] + puffs[i].dpitch * spread, q);
			for (int a = 0; a < 3; ++a) {
				q[a] = s_env.view_pos[a] + q[a] * (blast[2] - (float)(i + 1) * 3.0f * D / 58.0f) * k;
			}
			env_facing(q, D * puffs[i].size * fire * k, puffs[i].roll, c);
			env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c,
					 white);
		}
		const float flash = 1.0f - (t - ENV_TIE_BOOM_AT) / 0.6f;
		if (flash > 0.0f && s_env.tex[ENV_TEX_GLOW]) {
			const float f = 1.6f * flash * flash;
			const float tint[4] = { 1.0f * f * white[0] / 1.5f, 0.75f * f * white[1] / 1.4f,
									0.45f * f * white[2] / 1.3f, 1.0f };
			env_facing(p, D * 0.9f * k, 0.0f, c);
			env_quad(scene, ENV_TEX_GLOW, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, c,
					 tint);
		}
		/* The victim catching fire as it breaks up. */
		const float burn = env_smooth((t - ENV_TIE_BOOM_AT - 0.3f) / 1.1f);
		if (burn > 0.0f) {
			env_facing(xw, 0.276f * D * burn * k, 70.0f, c);
			env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
			env_facing(xw, 0.172f * D * burn * k, -20.0f, c);
			env_quad(scene, ENV_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c,
					 white);
		}
	}
	if (env_screen_reveal() >= 1.0f) {
		env_submit_logo(scene); /* the screen shown throughout */
	}
}

/* Purple clouds below (the TIE Fighter and Defender of the Empire covers). */
static void env_submit_low_clouds(AeronScene3D* scene, float gain) {
	static const struct {
		float yaw, pitch, w, h, roll, gain;
	} clouds[] = {
		{ -30.0f, -42.0f, 110.0f, 40.0f, 6.0f, 0.55f },
		{ 35.0f, -40.0f, 100.0f, 36.0f, -8.0f, 0.5f },
		{ 0.0f, -58.0f, 120.0f, 46.0f, 0.0f, 0.45f },
	};
	for (size_t i = 0; i < sizeof clouds / sizeof clouds[0]; ++i) {
		const float g = clouds[i].gain * gain;
		const float tint[4] = { 0.55f * g, 0.45f * g, 1.0f * g, 1.0f };
		env_sky_rect(scene, ENV_TEX_NEBULA, clouds[i].yaw, clouds[i].pitch, clouds[i].w, clouds[i].h,
					 clouds[i].roll, tint);
	}
}

/* The TIE Fighter box art, played: the five seconds before the cover's
 * moment, which then holds (see env_tie_animate); the helmet fades in at
 * the end. */
static void env_submit_tieart_sprites(AeronScene3D* scene) {
	const float t = s_env.anim_t;
	const float T = ENV_TIE_ANIM_SECONDS;
	/* Vader's helmet, looming behind the screen and the fight, a dark
	 * blue-violet; XWA_XR_VADER_BRIGHTNESS scales it. Fades in once the
	 * fight has come to the cover's moment. */
	const float vader = env_smooth((t - T) / env_float("XWA_XR_VADER_FADE_SECONDS", 2.5f));
	if (s_env.tex[ENV_TEX_VADER] && vader > 0.0f) {
		const float b = env_float("XWA_XR_VADER_BRIGHTNESS", 1.0f);
		/* 15 % darker than it was and slightly see-through (premultiplied:
		 * the colour carries the opacity too). */
		const float dark = 0.85f, opacity = 0.8f * vader;
		const float tint[4] = { 0.18f * b * dark * opacity, 0.17f * b * dark * opacity,
								0.42f * b * dark * opacity, opacity };
		const float h = 39.8f; /* 35 %, 20 %, then 15 % smaller than the cover's */
		const float w = h * (float)s_env.tex_w[ENV_TEX_VADER] / (float)s_env.tex_h[ENV_TEX_VADER];
		const float pitch = env_float("XWA_XR_VADER_PITCH", 31.0f); /* the eyes above the screen and logo */
		env_sky_rect(scene, ENV_TEX_VADER, 0.0f, pitch, w, h, 0.0f, tint);
	}
	env_submit_low_clouds(scene, 1.0f);
	env_submit_fight(scene);
}

/* Defender of the Empire, played: the TIE pilot's helmet (the X-Wing vs. TIE
 * Fighter scene's picture, vr/xvt_tie_pilot.rgba) looming behind the screen
 * in the cover's dark warm tones, fading in at the cover's moment as Vader's
 * does; the purple clouds below; the fight. XWA_XR_PILOT_BRIGHTNESS,
 * _PITCH scale and place the helmet; it fades in over XWA_XR_VADER_FADE_SECONDS. */
static void env_submit_doeart_sprites(AeronScene3D* scene) {
	const float t = s_env.anim_t;
	const float T = ENV_TIE_ANIM_SECONDS;
	const float pilot = env_smooth((t - T) / env_float("XWA_XR_VADER_FADE_SECONDS", 2.5f));
	const int slot = ENV_TEX_TIE_PILOT;
	if (s_env.tex[slot] && s_env.tex_h[slot] > 0 && pilot > 0.0f) {
		const float b = env_float("XWA_XR_PILOT_BRIGHTNESS", 1.0f);
		const float opacity = 0.85f * pilot;
		/* The picture is blue-grey: warmed to the cover's bronze and
		 * purple-brown. */
		const float tint[4] = { 0.78f * b * opacity, 0.55f * b * opacity, 0.45f * b * opacity, opacity };
		/* 80 degrees tall at XWA_XR_PILOT_SCALE 1; drawn at 0.65 (35 % smaller
			 * on screen, not in angle). */
			const float h = 2.0f * atanf(env_float("XWA_XR_PILOT_SCALE", 0.65f) * tanf(40.0f * 3.14159265f / 180.0f)) *
							180.0f / 3.14159265f;
		const float w = h * (float)s_env.tex_w[slot] / (float)s_env.tex_h[slot];
		env_sky_rect(scene, slot, 0.0f, env_float("XWA_XR_PILOT_PITCH", 16.0f), w, h, 0.0f, tint);
	}
	env_submit_low_clouds(scene, 1.15f);
	env_submit_fight(scene);
}

static float bt_dot(const float a[3], const float b[3]);
static int bt_norm(float v[3]);

/* The engines' glow of the craft shown, seen from behind (the B-wings' red,
 * the A-wing's yellow-white on the Balance of Power cover). */
static void env_submit_engine_glows(AeronScene3D* scene) {
	if (!s_env.tex[ENV_TEX_GLOW]) {
		return;
	}
	const float k = AERON_OPT_UNITS_PER_METER;
	for (int i = 0; i < s_env.count; ++i) {
		const EnvMesh* m = &s_env.meshes[s_env.mesh_of[i]];
		if (s_env.hidden[i] || !m->glow_count) {
			continue;
		}
		const float* x = s_env.transform[i];
		float back[3] = { x[1], x[5], x[9] }; /* model +y: the craft's rear */
		bt_norm(back);
		for (int g = 0; g < m->glow_count; ++g) {
			const float* o = m->glow[g];
			float p[3], to[3], c[4][3];
			for (int a = 0; a < 3; ++a) {
				p[a] = x[a * 4 + 0] * o[0] + x[a * 4 + 1] * o[1] + x[a * 4 + 2] * o[2] + x[a * 4 + 3];
				to[a] = s_env.view_pos[a] - p[a];
			}
			bt_norm(to);
			const float w = env_smooth(bt_dot(back, to) * 1.6f);
			if (w <= 0.0f) {
				continue;
			}
			/* Just off the nozzle toward the viewer, so the hull hides none of it. */
			for (int a = 0; a < 3; ++a) {
				p[a] += to[a] * 0.6f * k;
			}
			const float b = 1.6f * w;
			const float core[4] = { (0.4f + 0.6f * o[4]) * b, (0.15f + 0.85f * o[5]) * b,
									(0.15f + 0.85f * o[6]) * b, 1.0f };
			env_facing(p, o[3] * 2.0f * k, 0.0f, c);
			env_quad(scene, ENV_TEX_GLOW, AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
					 AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, c, core);
		}
	}
}

/* Balance of Power, played: the brown world behind the dreadnought, the
 * red-pink flak bursting around the fight (more of it as the fight comes to
 * the cover's moment), the B-wings' and A-wing's engines glowing, the fight. */
static void env_submit_bopart_sprites(AeronScene3D* scene) {
	const float k = AERON_OPT_UNITS_PER_METER;
	const float t = s_env.anim_t;
	{
		const float b = env_float("XWA_XR_PLANET_BRIGHTNESS", 1.0f);
		const float tint[4] = { b, b, b, 1.0f };
		float c[4][3];
		env_sky_sprite(scene, ENV_TEX_PLANET, -3.0f, 3.0f, 56.0f, 0.0f, tint, c);
	}
	/* The flak: each burst comes up in its turn over the last seconds,
	 * blooms and holds (the cover's). */
	for (size_t i = 0; i < sizeof k_bop_flak / sizeof k_bop_flak[0]; ++i) {
		const float at = ENV_TIE_ANIM_SECONDS - 3.2f + 0.7f * (float)i;
		const float g = env_smooth((t - at) / 0.5f);
		if (g <= 0.0f) {
			continue;
		}
		float p[3], c[4][3];
		env_dir(k_bop_flak[i][0], k_bop_flak[i][1], p);
		for (int a = 0; a < 3; ++a) {
			p[a] = s_env.view_pos[a] + p[a] * k_bop_flak[i][2] * k;
		}
		static const float pink[4] = { 1.7f, 0.75f, 0.95f, 1.0f };
		env_facing(p, k_bop_flak[i][3] * (0.4f + 0.6f * g) * k, 25.0f * (float)i, c);
		env_quad(scene, ENV_TEX_BLAST, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA,
				 c, pink);
		if (s_env.tex[ENV_TEX_GLOW]) {
			const float glow[4] = { 0.9f * g, 0.25f * g, 0.35f * g, 1.0f };
			env_facing(p, k_bop_flak[i][3] * 1.6f * k, 0.0f, c);
			env_quad(scene, ENV_TEX_GLOW, AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
					 AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, c, glow);
		}
	}
	env_submit_engine_glows(scene);
	env_submit_fight(scene);
}

/* The X-Wing vs. TIE Fighter box art: the explosion and its pillar of
 * light, the fire round the X-wing coming out of it, the green streaks. */
static void env_submit_xvtart_sprites(AeronScene3D* scene) {
	const float k = AERON_OPT_UNITS_PER_METER;
	/* The two pilots looming behind the fight, the TIE pilot on the left and
	 * the rebel on the right, both looking toward the middle; dimmed into the
	 * dark blue of the cover. XWA_XR_PILOTS_BRIGHTNESS scales them. */
	{
		const float b = env_float("XWA_XR_PILOTS_BRIGHTNESS", 1.0f);
		const float tint[4] = { 0.40f * b, 0.42f * b, 0.55f * b, 1.0f };
		static const struct {
			int slot;
			float yaw, h;
		} pilots[] = { { ENV_TEX_TIE_PILOT, -28.0f, 37.3f * 0.95f }, { ENV_TEX_REBEL_PILOT, 32.0f, 29.6f * 0.9f } };
		for (size_t i = 0; i < sizeof pilots / sizeof pilots[0]; ++i) {
			const int slot = pilots[i].slot;
			if (s_env.tex[slot] && s_env.tex_h[slot] > 0) {
				const float h = pilots[i].h;
				const float w = h * (float)s_env.tex_w[slot] / (float)s_env.tex_h[slot];
				env_sky_rect(scene, slot, pilots[i].yaw, 24.0f, w, h, 0.0f, tint);
			}
		}
	}
	/* The pillar of light, straight up and down through the explosion. */
	{
		float lo[3], hi[3];
		env_sky_point(k_xvt_blast[0], -75.0f, lo);
		env_sky_point(k_xvt_blast[0], 72.0f, hi);
		const float wide = 2.0f * ENV_SKY_DIST * tanf(1.3f * 3.14159265358979f / 360.0f);
		const float thin = 2.0f * ENV_SKY_DIST * tanf(0.22f * 3.14159265358979f / 360.0f);
		static const float glow[4] = { 0.55f, 0.22f, 0.05f, 1.0f };
		static const float core[4] = { 2.2f, 1.6f, 0.8f, 1.0f };
		env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_SKY, lo, hi, wide, glow);
		env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_SKY, lo, hi, thin, core);
	}
	env_submit_fireball(scene, k_xvt_blast);
	/* Fire round the X-wing coming out of it. */
	{
		float q[3], c[4][3];
		static const float white[4] = { 1.5f, 1.4f, 1.3f, 1.0f };
		env_ship_pos(2, q);
		for (int a = 0; a < 3; ++a) {
			q[a] += (q[a] - s_env.view_pos[a]) * 0.08f; /* just behind it */
		}
		env_facing(q, 30.0f * k, 15.0f, c);
		env_quad(scene, ENV_TEX_FIRE, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
	}
	env_submit_streaks(scene, k_xvt_streaks, (int)(sizeof k_xvt_streaks / sizeof k_xvt_streaks[0]));
	env_submit_logo(scene);
}

/* ---- "Live Battle": a running battle cast from the last mission ---------
 * The craft of the mission flown last (or, before any, a generic rebel /
 * Imperial clash) fight on around the screen: fighters dogfight in front and
 * to the sides, jumping in from hyperspace and bursting when shot down; the
 * mission's capital ships and station hold the flanks a few kilometres out
 * trading turbolaser fire, its biggest hostile ship burning; the mission's
 * planets, nebulae and suns hang in the sky, the suns lighting the scene.
 * The player's craft flies with its wingmen and is never shot down.
 *
 * A light steering director, not the flight engine: craft turn at a limited
 * rate and bank into their turns, chase with lead and fire in bursts when
 * lined up, break off to evade, and stay out of the cone the screen covers
 * and away from the viewer. Units here are metres (viewer at the origin, x
 * right, y forward, z up); the scene takes world units (k per metre). */

#define BT_MAX_TYPES 12
#define BT_MAX_FIGHTERS 16
#define BT_MAX_BIG 5
#define BT_MAX_BOLTS 128
#define BT_MAX_BOOMS 24
#define BT_MAX_FG 192
#define BT_FIGHTERS_PER_SIDE 6
#define BT_PI 3.14159265358979f

enum { BT_FRIEND = 0, BT_FOE = 1 };
enum { BT_HUNT = 0, BT_EVADE, BT_RETURN };
enum { BT_CLASS_FIGHTER = 0, BT_CLASS_BIG, BT_CLASS_STATION };

typedef struct BtType {
	char opt[64];
	int side, cls;
	float weight;
	int green; /* Imperial: green fire, blue-white engines */
	int hero;  /* the player's craft */
	int count; /* craft of this type in the battle */
} BtType;

typedef struct BtShip {
	int type, side, cls, alive, hero;
	float p[3], f[3], u[3];
	float speed, cruise, turn;
	float hp, hp_max;
	int target, mode;
	float timer, cooldown, respawn, arrive;
	float goal[3];
	float drift, spin; /* capital ships: metres moved; stations: yaw rate */
	float burn;        /* a burning capital ship: time to its next hull fire */
	float wish[3];     /* the course it is settling on (smoothed) */
} BtShip;

/* An asteroid tumbling slowly where the fighters pass. */
typedef struct BtRock {
	int type; /* index into s_bt.rock_opt */
	float p[3], f[3], u[3], v[3], axis[3];
	float spin, scale, radius;
} BtRock;

#define BT_MAX_ROCKS 8
#define BT_ROCK_TYPES 3

typedef struct BtBolt {
	float p[3], d[3];
	float speed, travelled, range, len, width;
	int side, big, alive;
	float color[3];
} BtBolt;

typedef struct BtBoom {
	float p[3], size, t, dur;
	int kind; /* 0 explosion, 1 hit spark, 2 hull fire */
	int alive;
	float roll;
} BtBoom;

typedef struct BtBackdrop {
	int group, sprite;
	float dir[3]; /* toward it (before the yaw turn) */
	float scale;
	float color[3];
	int sun, has_color;
	float yaw, pitch, size_deg; /* where it is drawn */
} BtBackdrop;

static struct {
	int cast;
	char mission[128];
	BtType types[BT_MAX_TYPES];
	int type_count;
	BtShip ships[BT_MAX_FIGHTERS + BT_MAX_BIG];
	int ship_count;
	BtBolt bolts[BT_MAX_BOLTS];
	BtBoom booms[BT_MAX_BOOMS];
	BtBackdrop backdrops[ENV_BACKDROPS];
	int backdrop_count;
	int sun; /* index of the sun lighting the scene, -1 none */
	uint32_t rng;
	uint64_t last_counter;
	float time;
	int started;
	int burning; /* the burning hostile capital ship, -1 none */
	char rock_opt[BT_ROCK_TYPES][64];
	int rock_types;
	BtRock rocks[BT_MAX_ROCKS];
	int rock_count;
	/* statistics (self-test, log) */
	int kills, shots, hits;
} s_bt;

static uint32_t bt_rand(void) {
	uint32_t x = s_bt.rng ? s_bt.rng : 0x9e3779b9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	s_bt.rng = x;
	return x;
}

static float bt_rnd(float lo, float hi) { return lo + (hi - lo) * (float)(bt_rand() & 0xffffffu) / 16777215.0f; }

static float bt_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static float bt_len(const float a[3]) { return sqrtf(bt_dot(a, a)); }

static int bt_norm(float v[3]) {
	const float l = bt_len(v);
	if (!(l > 1e-6f)) {
		return 0;
	}
	v[0] /= l;
	v[1] /= l;
	v[2] /= l;
	return 1;
}

/* Direction toward yaw / pitch degrees from the viewer, times `dist`. */
static void bt_at(float yaw, float pitch, float dist, float out[3]) {
	env_dir(yaw, pitch, out);
	for (int a = 0; a < 3; ++a) {
		out[a] *= dist;
	}
}

/* Rotate v about the unit axis by `angle` radians (Rodrigues). */
static void bt_rotate(float v[3], const float axis[3], float angle) {
	const float c = cosf(angle), s = sinf(angle), d = bt_dot(axis, v);
	float x[3];
	env_cross(axis, v, x);
	for (int a = 0; a < 3; ++a) {
		v[a] = v[a] * c + x[a] * s + axis[a] * d * (1.0f - c);
	}
}

/* ---- casting ------------------------------------------------------------ */

typedef struct BtFg {
	uint8_t craft_type, count, waves, team, iff, player, region;
	int backdrop;
	int16_t mp[3];
	char name[21], special[21];
} BtFg;

static int bt_is_green(const char* opt) {
	char low[64];
	size_t i = 0;
	for (; opt[i] && i + 1 < sizeof low; ++i) {
		low[i] = (char)SDL_tolower((unsigned char)opt[i]);
	}
	low[i] = 0;
	return strncmp(low, "tie", 3) == 0 || strstr(low, "imperial") || strstr(low, "stardestroyer") ||
		   strstr(low, "gunboat") || strstr(low, "missileboat") || strstr(low, "assault") ||
		   strstr(low, "skipray") || strstr(low, "lambda");
}

/* The OPT base name of a mission craft type ("FlightModels\\XWING.OPT" ->
 * "XWING"); 0 when the type has none. */
static int bt_craft_opt(int craft_type, char out[64]) {
	const char* path = FeDiskIo_GetCraftModelName((unsigned int)craft_type);
	if (!path || !path[0]) {
		return 0;
	}
	const char* base = path;
	for (const char* c = path; *c; ++c) {
		if (*c == '\\' || *c == '/') {
			base = c + 1;
		}
	}
	size_t n = 0;
	while (base[n] && base[n] != '.' && n + 1 < 64) {
		out[n] = base[n];
		n++;
	}
	out[n] = 0;
	return n > 0;
}

static int bt_genus(int craft_type) {
	if (craft_type <= 0 || craft_type >= XWA_CRAFT_TYPE_TO_OBJECT_TYPE_COUNT) {
		return -1;
	}
	const unsigned obj = g_objectTypeTables.craftTypeToObjectType[craft_type];
	return obj > 0 && obj < OBJ_Count ? (int)g_modelTypeTable[obj].genusId : -1;
}

static int bt_add_type(const char* opt, int side, int cls, float weight, int hero) {
	for (int i = 0; i < s_bt.type_count; ++i) {
		BtType* t = &s_bt.types[i];
		if (SDL_strcasecmp(t->opt, opt) == 0 && t->side == side && t->cls == cls) {
			t->weight += weight;
			t->hero |= hero;
			return i;
		}
	}
	if (s_bt.type_count >= BT_MAX_TYPES) {
		return -1;
	}
	BtType* t = &s_bt.types[s_bt.type_count];
	memset(t, 0, sizeof *t);
	snprintf(t->opt, sizeof t->opt, "%s", opt);
	t->side = side;
	t->cls = cls;
	t->weight = weight;
	t->hero = hero;
	t->green = bt_is_green(opt);
	return s_bt.type_count++;
}

/* Reads the mission's flight groups and the player team's allegiances. */
static int bt_read_mission(const char* path, BtFg* fgs, int* fg_count, uint8_t allies[10][10]) {
	AeronVfs* vfs = Aeron_GetVfs();
	uint8_t* data = NULL;
	size_t size = 0;
	if (!vfs || !path[0] || !AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, path, 16u << 20, &data, &size)) {
		return 0;
	}
	int ok = 0;
	size_t at = 2 + sizeof(XwaMissionHeader);
	if (size >= at) {
		XwaMissionHeader header;
		memcpy(&header, data + 2, sizeof header);
		const int n = header.numFlightGroups;
		if (n > 0 && n <= BT_MAX_FG && size >= at + (size_t)n * sizeof(XwaFlightGroup)) {
			for (int i = 0; i < n; ++i) {
				XwaFlightGroup fg;
				memcpy(&fg, data + at + (size_t)i * sizeof fg, sizeof fg);
				BtFg* o = &fgs[i];
				memset(o, 0, sizeof *o);
				o->craft_type = fg.craftType;
				o->count = fg.numberOfCraft;
				o->waves = fg.numberOfWaves;
				o->team = fg.team;
				o->iff = fg.iff;
				o->player = fg.playerNumber;
				o->region = fg.missionPointRegions[XWA_FG_POINT_START_1];
				o->backdrop = fg.backdrop;
				for (int a = 0; a < 3; ++a) {
					o->mp[a] = (&fg.missionPoints[XWA_FG_POINT_START_1].x)[a];
				}
				memcpy(o->name, fg.name, 20);
				memcpy(o->special, fg.specialCargo, 20);
			}
			*fg_count = n;
			ok = 1;
			/* Messages, global goals, then the teams. */
			at += (size_t)n * sizeof(XwaFlightGroup);
			at += (size_t)header.numMessages * (2 + sizeof(XwaMessage));
			for (int t = 0; t < 10 && ok && at + 2 <= size; ++t) {
				int16_t goals;
				memcpy(&goals, data + at, 2);
				at += 2 + (size_t)(goals > 0 && goals <= 7 ? goals : 0) * sizeof(XwaGlobalGoal);
			}
			for (int t = 0; t < 10 && at + 2 <= size; ++t) {
				int16_t present;
				memcpy(&present, data + at, 2);
				at += 2;
				if (present && at + sizeof(XwaTeam) <= size) {
					XwaTeam team;
					memcpy(&team, data + at, sizeof team);
					memcpy(allies[t], team.allies, 10);
					at += sizeof team;
				}
			}
		}
	}
	free(data);
	return ok;
}

/* The mission flown last: this session's, else the one remembered from the
 * last session (vr_last_mission.txt in the user folder). */
static void bt_last_mission(char out[128]) {
	extern char g_currentMissionFile[128];
	AeronVfs* vfs = Aeron_GetVfs();
	out[0] = 0;
	const char* force = SDL_getenv("XWA_XR_BATTLE_MISSION");
	if (force && force[0]) {
		snprintf(out, 128, "%s", force);
		return;
	}
	if (g_currentMissionFile[0]) {
		snprintf(out, 128, "%s", g_currentMissionFile);
		if (vfs) {
			uint8_t* old = NULL;
			size_t old_size = 0;
			const int same = AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_USER, "vr_last_mission.txt", 256, &old, &old_size) &&
							 old_size == strlen(out) && memcmp(old, out, old_size) == 0;
			free(old);
			if (!same) {
				(void)AeronVfs_WriteAllAtomic(vfs, AERON_VFS_ROOT_USER, "vr_last_mission.txt", out, strlen(out));
			}
		}
		return;
	}
	uint8_t* saved = NULL;
	size_t saved_size = 0;
	if (vfs && AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_USER, "vr_last_mission.txt", 256, &saved, &saved_size)) {
		const size_t n = saved_size < 127 ? saved_size : 127;
		memcpy(out, saved, n);
		out[n] = 0;
		for (size_t i = 0; i < n; ++i) {
			if (out[i] == '\r' || out[i] == '\n') {
				out[i] = 0;
			}
		}
	}
	free(saved);
}

static void bt_add_backdrop(const BtFg* fg) {
	if (s_bt.backdrop_count >= ENV_BACKDROPS || fg->backdrop <= 0 ||
		fg->backdrop >= XWA_MISSION_BACKDROP_DESCRIPTOR_COUNT) {
		return;
	}
	const unsigned type = g_objectTypeTables.missionBackdropDescriptors[fg->backdrop].modelType;
	if (type == 0 || type >= OBJ_Count) {
		return;
	}
	const ModelTypeInfo* info = &g_modelTypeTable[type];
	if (info->textureGroup < 6000 || info->textureGroup >= 10000 || info->textureGroup == 6250) {
		return; /* not a planet, nebula, galaxy or sun (the Death Star's fire) */
	}
	BtBackdrop* b = &s_bt.backdrops[s_bt.backdrop_count];
	memset(b, 0, sizeof *b);
	b->group = info->textureGroup;
	b->sprite = info->textureGroup >= 7000 ? info->frameCount : 0;
	/* Flight: the start point's direction, y mirrored. */
	b->dir[0] = (float)fg->mp[0];
	b->dir[1] = -(float)fg->mp[1];
	b->dir[2] = (float)fg->mp[2];
	if (!bt_norm(b->dir)) {
		b->dir[1] = 1.0f;
	}
	b->scale = 1.0f;
	if (fg->special[0]) {
		const float v = (float)SDL_atof(fg->special);
		if (v > 0.05f && v < 10.0f && v != 15.5f) {
			b->scale = v;
		}
	}
	/* Suns (Backdrop.dat): the name holds their light colour. */
	b->sun = info->textureGroup >= 9000;
	float r, g, bl;
	if (sscanf(fg->name, "%f %f %f", &r, &g, &bl) == 3 && r >= 0.0f && g >= 0.0f && bl >= 0.0f) {
		b->color[0] = r;
		b->color[1] = g;
		b->color[2] = bl;
		b->has_color = 1;
	}
	s_bt.backdrop_count++;
}

/* Picks the craft: up to three fighter types a side (the player's first),
 * two capital ships a side and one station, weighted by numbers. */
static void bt_cast_mission(const BtFg* fgs, int n, uint8_t allies[10][10], int have_allies) {
	int player = -1;
	for (int i = 0; i < n && player < 0; ++i) {
		if (fgs[i].player == 1) {
			player = i;
		}
	}
	for (int i = 0; i < n && player < 0; ++i) {
		if (fgs[i].player) {
			player = i;
		}
	}
	const int team = player >= 0 ? fgs[player].team : 0;
	const int region = player >= 0 ? fgs[player].region : 0;
	char opt[64];
	if (player >= 0) {
		const int genus = bt_genus(fgs[player].craft_type);
		if ((genus == GENUS_Fighter || genus == GENUS_Transport) && bt_craft_opt(fgs[player].craft_type, opt)) {
			bt_add_type(opt, BT_FRIEND, BT_CLASS_FIGHTER, 1000.0f, 1);
		}
	}
	/* The backdrops where the player starts, then those of the other regions
	 * while there is room. */
	for (int pass = 0; pass < 2; ++pass) {
		for (int i = 0; i < n; ++i) {
			if (fgs[i].craft_type == 0xb7 && (fgs[i].region == region) == (pass == 0)) {
				bt_add_backdrop(&fgs[i]);
			}
		}
	}
	for (int i = 0; i < n; ++i) {
		const BtFg* fg = &fgs[i];
		if (fg->craft_type == 0xb7) {
			continue;
		}
		if (i == player || fg->count == 0) {
			continue;
		}
		const int genus = bt_genus(fg->craft_type);
		int cls;
		float weight = (float)fg->count * (1.0f + (float)fg->waves);
		if (genus == GENUS_Fighter) {
			cls = BT_CLASS_FIGHTER;
		} else if (genus == GENUS_Transport) {
			cls = BT_CLASS_FIGHTER;
			weight *= 0.5f;
		} else if (genus == GENUS_Starship || genus == GENUS_Freighter) {
			cls = BT_CLASS_BIG;
			weight *= genus == GENUS_Starship ? 3.0f : 1.0f;
		} else if (genus == GENUS_Platform) {
			cls = BT_CLASS_STATION;
		} else {
			continue; /* mines, buoys, containers, ... */
		}
		/* Hostile: another team that is not allied (allies: 0 hostile, 1 ally). */
		int side;
		if (fg->team == team) {
			side = BT_FRIEND;
		} else if (have_allies && team < 10 && fg->team < 10 && allies[team][fg->team] == 1) {
			side = BT_FRIEND;
		} else if (have_allies && team < 10 && fg->team < 10 && allies[team][fg->team] != 0) {
			continue; /* neutral */
		} else {
			side = BT_FOE;
		}
		if (fg->region != region) {
			weight *= 0.3f; /* elsewhere in the mission */
		}
		if (bt_craft_opt(fg->craft_type, opt)) {
			bt_add_type(opt, side, cls, weight, 0);
		}
	}
}

/* No mission yet: rebels against the Empire over a random world. */
static void bt_cast_generic(void) {
	bt_add_type("Xwing", BT_FRIEND, BT_CLASS_FIGHTER, 3.0f, 0);
	bt_add_type("Awing", BT_FRIEND, BT_CLASS_FIGHTER, 2.0f, 0);
	bt_add_type("Ywing", BT_FRIEND, BT_CLASS_FIGHTER, 1.0f, 0);
	bt_add_type("TieFighter", BT_FOE, BT_CLASS_FIGHTER, 3.0f, 0);
	bt_add_type("TieInterceptor", BT_FOE, BT_CLASS_FIGHTER, 2.0f, 0);
	bt_add_type("CalamariCruiserNew", BT_FRIEND, BT_CLASS_BIG, 2.0f, 0);
	bt_add_type("Corvette2", BT_FRIEND, BT_CLASS_BIG, 1.0f, 0);
	bt_add_type("ImperialStarDestroyer2", BT_FOE, BT_CLASS_BIG, 2.0f, 0);
	bt_add_type("Frigate2", BT_FOE, BT_CLASS_BIG, 1.0f, 0);
	static const int planets[] = { 6010, 6012, 6020, 6023, 6026, 6030, 6033 };
	BtBackdrop* b = &s_bt.backdrops[0];
	memset(b, 0, sizeof *b);
	b->group = planets[bt_rand() % (sizeof planets / sizeof planets[0])];
	env_dir(40.0f, 18.0f, b->dir);
	b->scale = 1.4f;
	s_bt.backdrop_count = 1;
}

/* Keeps the strongest types: fighters 3 a side, capital ships 2 a side,
 * one station; numbers of fighters by weight. */
static void bt_trim(void) {
	BtType kept[BT_MAX_TYPES];
	int kept_count = 0;
	static const int limits[3] = { 3, 2, 1 };
	for (int cls = 0; cls < 3; ++cls) {
		for (int side = 0; side < 2; ++side) {
			int used[BT_MAX_TYPES] = { 0 };
			const int limit = cls == BT_CLASS_STATION ? (side == 0 ? 1 : 0) : limits[cls];
			for (int k = 0; k < limit; ++k) {
				int best = -1;
				for (int i = 0; i < s_bt.type_count; ++i) {
					const BtType* t = &s_bt.types[i];
					const int match = t->cls == cls && (cls == BT_CLASS_STATION || t->side == side);
					if (match && !used[i] && (best < 0 || t->weight > s_bt.types[best].weight)) {
						best = i;
					}
				}
				if (best < 0) {
					break;
				}
				used[best] = 1;
				kept[kept_count++] = s_bt.types[best];
			}
		}
	}
	memcpy(s_bt.types, kept, sizeof(BtType) * (size_t)kept_count);
	s_bt.type_count = kept_count;
	/* A side with no fighters borrows the generic ones of its colour. */
	for (int side = 0; side < 2; ++side) {
		int have = 0;
		for (int i = 0; i < s_bt.type_count; ++i) {
			have |= s_bt.types[i].cls == BT_CLASS_FIGHTER && s_bt.types[i].side == side;
		}
		if (!have) {
			bt_add_type(side == BT_FRIEND ? "Xwing" : "TieFighter", side, BT_CLASS_FIGHTER, 1.0f, 0);
		}
	}
	/* Fighters per type by weight, the hero type at least two (its wingman). */
	for (int side = 0; side < 2; ++side) {
		float total = 0.0f;
		for (int i = 0; i < s_bt.type_count; ++i) {
			const BtType* t = &s_bt.types[i];
			if (t->cls == BT_CLASS_FIGHTER && t->side == side) {
				total += t->hero ? 0.0f : t->weight;
			}
		}
		int left = BT_FIGHTERS_PER_SIDE;
		for (int i = 0; i < s_bt.type_count; ++i) {
			BtType* t = &s_bt.types[i];
			if (t->cls == BT_CLASS_FIGHTER && t->side == side && t->hero) {
				t->count = 2;
				left -= 2;
			}
		}
		const int share = left;
		for (int pass = 0; pass < 2 && left > 0; ++pass) {
			for (int i = 0; i < s_bt.type_count && left > 0; ++i) {
				BtType* t = &s_bt.types[i];
				if (t->cls != BT_CLASS_FIGHTER || t->side != side || t->hero) {
					continue;
				}
				int want = pass == 0 && total > 0.0f ? (int)floorf((float)share * t->weight / total + 0.5f) : 1;
				want = want < 1 ? 1 : want;
				want = want > left ? left : want;
				t->count += want;
				left -= want;
			}
			if (total <= 0.0f) {
				break;
			}
		}
		/* No non-hero fighters: more wingmen. */
		for (int i = 0; i < s_bt.type_count && left > 0; ++i) {
			BtType* t = &s_bt.types[i];
			if (t->cls == BT_CLASS_FIGHTER && t->side == side) {
				t->count += left;
				left = 0;
			}
		}
	}
	for (int i = 0; i < s_bt.type_count; ++i) {
		if (s_bt.types[i].cls != BT_CLASS_FIGHTER) {
			s_bt.types[i].count = 1;
		}
	}
}

/* The backdrops turned so the biggest sits off to the side above the
 * horizon, the rest keeping their places around it. */
static void bt_place_backdrops(void) {
	int main = -1;
	for (int i = 0; i < s_bt.backdrop_count; ++i) {
		const BtBackdrop* b = &s_bt.backdrops[i];
		if (!b->sun && b->group < 7000 && (main < 0 || b->scale > s_bt.backdrops[main].scale)) {
			main = i;
		}
	}
	float turn = 0.0f;
	if (main >= 0) {
		const float* d = s_bt.backdrops[main].dir;
		const float yaw = atan2f(d[0], d[1]) * 180.0f / BT_PI;
		const float a = 42.0f - yaw, b = -42.0f - yaw;
		turn = fabsf(remainderf(a, 360.0f)) < fabsf(remainderf(b, 360.0f)) ? a : b;
	}
	s_bt.sun = -1;
	for (int i = 0; i < s_bt.backdrop_count; ++i) {
		BtBackdrop* b = &s_bt.backdrops[i];
		const float yaw = atan2f(b->dir[0], b->dir[1]) * 180.0f / BT_PI + turn;
		float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, b->dir[2]))) * 180.0f / BT_PI;
		if (i == main) {
			pitch = fminf(fmaxf(pitch, 8.0f), 30.0f);
		}
		b->yaw = remainderf(yaw, 360.0f);
		b->pitch = fminf(fmaxf(pitch, -60.0f), 70.0f);
		if (b->sun) {
			/* Suns over the viewer's shoulder, away from the main world: the
			 * craft in front are lit on the sides we see. */
			const float away = main >= 0 && remainderf(turn + atan2f(s_bt.backdrops[main].dir[0],
																	   s_bt.backdrops[main].dir[1]) * 180.0f / BT_PI,
														360.0f) > 0.0f
								   ? -1.0f
								   : 1.0f;
			b->yaw = away * (128.0f + 14.0f * (float)i);
			b->pitch = 32.0f + 6.0f * (float)i;
		}
		/* Flight: half extent scale * 1.5 * width at 2048 units. */
		const float half = atanf(1.5f * 256.0f * b->scale / 2048.0f) * 180.0f / BT_PI;
		b->size_deg = 2.0f * half * (b->group >= 7000 && b->group < 9000 ? 2.0f : 1.0f);
		/* A menu backdrop, not a mission: the main world large, the rest
		 * smaller. */
		b->size_deg = fminf(b->size_deg, b->sun ? 12.0f : i == main ? 34.0f : 20.0f);
		if (b->sun && (s_bt.sun < 0 || b->scale > s_bt.backdrops[s_bt.sun].scale)) {
			s_bt.sun = i;
		}
	}
	/* Overlapping ones move apart (round the sky, away from the screen). */
	for (int i = 0; i < s_bt.backdrop_count; ++i) {
		BtBackdrop* b = &s_bt.backdrops[i];
		for (int tries = 0; tries < 12; ++tries) {
			int clash = 0;
			for (int j = 0; j < i && !clash; ++j) {
				const BtBackdrop* o = &s_bt.backdrops[j];
				float d0[3], d1[3];
				env_dir(b->yaw, b->pitch, d0);
				env_dir(o->yaw, o->pitch, d1);
				const float apart = acosf(fminf(1.0f, bt_dot(d0, d1))) * 180.0f / BT_PI;
				clash = apart < 0.55f * (b->size_deg + o->size_deg) + 4.0f;
			}
			if (!clash) {
				break;
			}
			b->yaw = remainderf(b->yaw + (b->yaw >= 0.0f ? 27.0f : -27.0f), 360.0f);
			if (fabsf(b->yaw) > 150.0f) {
				b->pitch = fminf(b->pitch + 20.0f, 60.0f);
			}
		}
		/* Nothing large right behind the screen. */
		if (fabsf(b->yaw) < 26.0f && b->pitch < 34.0f && !b->sun) {
			b->yaw = b->yaw >= 0.0f ? 30.0f : -30.0f;
		}
	}
}

/* The game's asteroid models: AsteroidHR1-6 name rows of the craft lists
 * (FlightModels/Spacecraft0.LST or Equipment0.LST). */
static void bt_asteroid_names(void) {
	AeronVfs* vfs = Aeron_GetVfs();
	static const char* const lists[2] = { "FlightModels/Spacecraft0.LST", "FlightModels/Equipment0.LST" };
	char* rows[2][64];
	int row_count[2] = { 0, 0 };
	uint8_t* data[2] = { NULL, NULL };
	for (int l = 0; l < 2; ++l) {
		size_t size = 0;
		if (!vfs || !AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, lists[l], 1u << 20, &data[l], &size)) {
			data[l] = NULL;
			continue;
		}
		uint8_t* text = (uint8_t*)realloc(data[l], size + 1);
		if (!text) {
			continue;
		}
		data[l] = text;
		text[size] = 0;
		for (char* line = strtok((char*)text, "\r\n"); line && row_count[l] < 64; line = strtok(NULL, "\r\n")) {
			if (line[0]) {
				rows[l][row_count[l]++] = line;
			}
		}
	}
	/* Three of the six, at random. */
	int order[6] = { 0, 1, 2, 3, 4, 5 };
	for (int i = 5; i > 0; --i) {
		const int j = (int)(bt_rand() % (uint32_t)(i + 1));
		const int t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
	for (int k = 0; k < 6 && s_bt.rock_types < BT_ROCK_TYPES; ++k) {
		const ModelTypeInfo* info = &g_modelTypeTable[OBJ_AsteroidHR1 + order[k]];
		const int list = info->textureGroup;
		if (list < 0 || list > 1 || info->frameCount >= row_count[list]) {
			continue;
		}
		const char* path = rows[list][info->frameCount];
		const char* base = path;
		for (const char* c = path; *c; ++c) {
			if (*c == '\\' || *c == '/') {
				base = c + 1;
			}
		}
		size_t n = 0;
		char* out = s_bt.rock_opt[s_bt.rock_types];
		while (base[n] && base[n] != '.' && n + 1 < 64) {
			out[n] = base[n];
			n++;
		}
		out[n] = 0;
		if (n) {
			s_bt.rock_types++;
		}
	}
	free(data[0]);
	free(data[1]);
}

static void bt_cast(void) {
	if (s_bt.cast) {
		return;
	}
	const uint32_t rng = s_bt.rng;
	memset(&s_bt, 0, sizeof s_bt);
	s_bt.rng = rng ? rng : (uint32_t)(SDL_GetPerformanceCounter() ^ ((uint64_t)time(NULL) * 2654435761u)) | 1u;
	s_bt.cast = 1;
	s_bt.sun = -1;
	s_bt.burning = -1;
	bt_last_mission(s_bt.mission);
	static BtFg fgs[BT_MAX_FG];
	uint8_t allies[10][10];
	int n = 0;
	memset(allies, 0, sizeof allies);
	const int have = s_bt.mission[0] && bt_read_mission(s_bt.mission, fgs, &n, allies);
	if (have) {
		bt_cast_mission(fgs, n, allies, 1);
	}
	int fighters = 0;
	for (int i = 0; i < s_bt.type_count; ++i) {
		fighters += s_bt.types[i].cls == BT_CLASS_FIGHTER;
	}
	if (!have || !fighters) {
		s_bt.type_count = 0;
		s_bt.backdrop_count = 0;
		bt_cast_generic();
	}
	bt_trim();
	bt_place_backdrops();
	bt_asteroid_names();
	for (int i = 0; i < s_bt.type_count; ++i) {
		env_list_add(s_bt.types[i].opt);
	}
	for (int i = 0; i < s_bt.rock_types; ++i) {
		env_list_add(s_bt.rock_opt[i]);
	}
	Aeron_LogInfo("xwa.remaster", "VR battle: %s, %d craft types, %d backdrops, %d asteroid models",
				  have ? s_bt.mission : "generic", s_bt.type_count, s_bt.backdrop_count, s_bt.rock_types);
	for (int i = 0; i < s_bt.type_count; ++i) {
		const BtType* t = &s_bt.types[i];
		Aeron_LogInfo("xwa.remaster", "VR battle:   %s %s %s x%d%s", t->side == BT_FRIEND ? "friend" : "foe",
					  t->cls == BT_CLASS_FIGHTER ? "fighter" : t->cls == BT_CLASS_BIG ? "capital" : "station", t->opt,
					  t->count, t->hero ? " (player)" : "");
	}
}

/* ---- textures ------------------------------------------------------------ */

static int bt_tex_needed(int slot) {
	if (slot == ENV_TEX_FRAME || slot == ENV_TEX_LOGO || slot == ENV_TEX_BEAM || slot == ENV_TEX_GLOW ||
		slot == ENV_TEX_BLAST) {
		return 1;
	}
	if (slot >= ENV_TEX_BOOM && slot <= ENV_TEX_BOOM_LAST) {
		return 1;
	}
	return slot >= ENV_TEX_BACKDROP && slot < ENV_TEX_BACKDROP + s_bt.backdrop_count;
}

/* A soft round glow (additive; colour from the tint). */
static int bt_make_glow(AeronCommandBuffer* cmd) {
	enum { G = 64 };
	uint8_t px[G * G * 4];
	for (int y = 0; y < G; ++y) {
		for (int x = 0; x < G; ++x) {
			const float dx = ((float)x + 0.5f) / (float)G * 2.0f - 1.0f, dy = ((float)y + 0.5f) / (float)G * 2.0f - 1.0f;
			const float r2 = dx * dx + dy * dy;
			const float a = r2 >= 1.0f ? 0.0f : fminf(1.0f, expf(-r2 * 9.0f) + 0.35f * expf(-r2 * 2.5f)) * (1.0f - r2);
			uint8_t* p = &px[(y * G + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(a * 255.0f + 0.5f);
			p[3] = 255;
		}
	}
	AeronTexture* t = Aeron_CreateTexture(&(AeronTextureDesc) {
		.width = G,
		.height = G,
		.mip_count = 1,
		.format = AERON_TEXTURE_FORMAT_RGBA8_UNORM,
		.usage = AERON_TEXTURE_USAGE_SAMPLED | AERON_TEXTURE_USAGE_TRANSFER_DST,
		.debug_name = "xwa.vr_env.glow",
	});
	if (!t || !Aeron_UploadTextureDataCmd(cmd, &(AeronTextureUploadDesc) {
			.texture = t,
			.width = G,
			.height = G,
			.raw_data = px,
			.raw_size = sizeof px,
		})) {
		if (t) {
			Aeron_DestroyTexture(t);
		}
		return 0;
	}
	s_env.tex[ENV_TEX_GLOW] = t;
	s_env.tex_w[ENV_TEX_GLOW] = G;
	s_env.tex_h[ENV_TEX_GLOW] = G;
	return 1;
}

/* Reads one DAT group; frames sorted by sprite id. */
static int bt_read_group(int group, Xwa2dFrameSet* set) {
	const char* dat = group < 3000 ? "Explosions"
					  : group < 7000 ? "Planet"
					  : group < 8000 ? "Nebula"
					  : group < 9000 ? "Galaxy"
									 : "Backdrop";
	char path[2][64];
	snprintf(path[0], sizeof path[0], "Resdata/%s.dat", dat);
	snprintf(path[1], sizeof path[1], "RESDATA/%s.DAT", dat);
	for (char* c = path[1]; *c; ++c) {
		*c = (char)SDL_toupper((unsigned char)*c);
	}
	AeronVfs* vfs = Aeron_GetVfs();
	uint8_t* bytes = NULL;
	size_t size = 0;
	for (int i = 0; i < 2 && !bytes; ++i) {
		if (vfs && AeronVfs_Exists(vfs, AERON_VFS_ROOT_ASSET, path[i]) &&
			!AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, path[i], 64u << 20, &bytes, &size)) {
			bytes = NULL;
		}
	}
	if (!bytes) {
		Aeron_LogWarn("xwa.remaster", "VR battle: %s unavailable", path[0]);
		return 0;
	}
	memset(set, 0, sizeof *set);
	char error[256];
	error[0] = 0;
	const int ok = Xwa2d_DatAppendGroup(bytes, size, (uint16_t)group, set, error, sizeof error) && set->count > 0;
	free(bytes);
	if (!ok) {
		Aeron_LogWarn("xwa.remaster", "VR battle: DAT group %d unavailable: %s", group, error);
		Xwa2dFrameSet_Free(set);
	}
	return ok;
}

/* The battle's own textures; returns 1 when `slot` is one of them. */
static int bt_load_texture(AeronCommandBuffer* cmd, int slot) {
	if (slot == ENV_TEX_GLOW) {
		bt_make_glow(cmd);
		return 1;
	}
	if (slot == ENV_TEX_BOOM) {
		/* The whole animation at once (frames 1..12 of group 2005). */
		Xwa2dFrameSet set;
		if (bt_read_group(2005, &set)) {
			for (int f = 0; f < ENV_BOOM_FRAMES; ++f) {
				for (int i = 0; i < set.count; ++i) {
					if (set.frames[i].sprite_id == f + 1 && set.frames[i].rgba) {
						env_make_texture(cmd, ENV_TEX_BOOM + f, set.frames[i].rgba, set.frames[i].width,
										 set.frames[i].height);
					}
				}
				s_env.tex_tried[ENV_TEX_BOOM + f] = 1;
			}
			Xwa2dFrameSet_Free(&set);
		}
		return 1;
	}
	if (slot > ENV_TEX_BOOM && slot <= ENV_TEX_BOOM_LAST) {
		return 1; /* loaded with the first */
	}
	if (slot >= ENV_TEX_BACKDROP && slot <= ENV_TEX_BACKDROP_LAST) {
		const BtBackdrop* b = &s_bt.backdrops[slot - ENV_TEX_BACKDROP];
		Xwa2dFrameSet set;
		if (bt_read_group(b->group, &set)) {
			/* Its sprite when named (nebulae, suns), else the first (a planet's
			 * fully lit face). */
			const Xwa2dFrame* pick = NULL;
			for (int i = 0; i < set.count; ++i) {
				const Xwa2dFrame* f = &set.frames[i];
				if (!f->rgba) {
					continue;
				}
				if (b->sprite ? f->sprite_id == b->sprite : (!pick || f->sprite_id < pick->sprite_id)) {
					pick = f;
				}
			}
			for (int i = 0; i < set.count && !pick; ++i) {
				pick = set.frames[i].rgba ? &set.frames[i] : NULL;
			}
			if (pick && pick->rgba) {
				env_make_texture(cmd, slot, pick->rgba, pick->width, pick->height);
			}
			Xwa2dFrameSet_Free(&set);
		}
		return 1;
	}
	return 0;
}

/* ---- the director --------------------------------------------------------- */

/* The screen's cone (degrees): craft in it are hidden behind the screen. */
static int bt_behind_screen(const float p[3]) {
	const float yaw = atan2f(p[0], p[1]) * 180.0f / BT_PI;
	const float pitch = atan2f(p[2], sqrtf(p[0] * p[0] + p[1] * p[1])) * 180.0f / BT_PI;
	return p[1] > 0.0f && fabsf(yaw) < 23.0f && pitch > -21.0f && pitch < 30.0f;
}

/* A point the fighters fly through: around the screen, not behind it; a
 * quarter of them close by for a pass near the viewer. */
static void bt_pick_goal(float out[3], int side) {
	for (int tries = 0; tries < 12; ++tries) {
		const int close = (bt_rand() & 3) == 0;
		/* Either side; a side's craft favour their own flank a little. */
		const float own = side == BT_FRIEND ? -1.0f : 1.0f;
		const float yaw = (bt_rnd(0.0f, 1.0f) < 0.6f ? own : -own) * bt_rnd(15.0f, 75.0f);
		const float pitch = bt_rnd(-35.0f, 45.0f);
		const float dist = close ? bt_rnd(110.0f, 200.0f) : bt_rnd(152.0f, 752.0f);
		bt_at(yaw, pitch, dist, out);
		if (!bt_behind_screen(out)) {
			return;
		}
	}
	bt_at(side == BT_FRIEND ? -45.0f : 45.0f, 10.0f, 560.0f, out);
}

static const EnvMesh* bt_mesh(const BtShip* sh) {
	const int m = env_find(s_bt.types[sh->type].opt);
	return m >= 0 ? &s_env.meshes[m] : NULL;
}

static float bt_radius(const BtShip* sh) {
	const EnvMesh* m = bt_mesh(sh);
	const float r = m ? 0.5f * fmaxf(m->len_m, fmaxf(m->wid_m, m->hgt_m)) : 0.0f;
	return r > 0.5f ? r : (sh->cls == BT_CLASS_FIGHTER ? 6.0f : 300.0f);
}

/* A fighter (re)joining: from hyperspace on its side's flank. */
static void bt_spawn_fighter(BtShip* sh, float delay) {
	float to[3];
	for (int tries = 0; tries < 16; ++tries) {
		const float yaw = (sh->side == BT_FRIEND ? -1.0f : 1.0f) * bt_rnd(30.0f, 75.0f);
		bt_at(yaw, bt_rnd(-15.0f, 35.0f), bt_rnd(640.0f, 1090.0f), sh->p);
		bt_pick_goal(to, sh->side);
		if (bt_len(to) < 152.0f) {
			continue; /* arrive further out */
		}
		for (int a = 0; a < 3; ++a) {
			sh->f[a] = to[a] - sh->p[a];
		}
		if (!bt_norm(sh->f)) {
			continue;
		}
		/* The jump's run-out (about 200 m) passes well clear of the viewer. */
		const float t = fminf(fmaxf(-bt_dot(sh->p, sh->f), 0.0f), 260.0f);
		float c[3];
		for (int a = 0; a < 3; ++a) {
			c[a] = sh->p[a] + sh->f[a] * t;
		}
		int clear = bt_len(c) > 140.0f;
		/* ... and round the asteroids. */
		for (int r = 0; r < s_bt.rock_count && clear; ++r) {
			const BtRock* rock = &s_bt.rocks[r];
			float rel[3] = { rock->p[0] - sh->p[0], rock->p[1] - sh->p[1], rock->p[2] - sh->p[2] };
			const float tr = fminf(fmaxf(bt_dot(rel, sh->f), 0.0f), 320.0f);
			for (int a = 0; a < 3; ++a) {
				c[a] = sh->p[a] + sh->f[a] * tr - rock->p[a];
			}
			clear = bt_len(c) > rock->radius + 60.0f;
		}
		if (clear) {
			break;
		}
	}
	if (!(bt_len(sh->f) > 0.5f)) {
		sh->f[0] = sh->f[2] = 0.0f;
		sh->f[1] = -1.0f;
	}
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	float r[3];
	env_cross(sh->f, z, r);
	if (!bt_norm(r)) {
		r[0] = 1.0f;
	}
	env_cross(r, sh->f, sh->u);
	bt_norm(sh->u);
	memcpy(sh->goal, to, sizeof to);
	memcpy(sh->wish, sh->f, sizeof sh->wish);
	sh->alive = 0;
	sh->respawn = delay;
	sh->hp = sh->hp_max;
	sh->mode = BT_RETURN;
	sh->timer = bt_rnd(2.0f, 4.0f);
	sh->target = -1;
	sh->cooldown = bt_rnd(0.5f, 1.5f);
}

static void bt_start(void) {
	s_bt.ship_count = 0;
	s_bt.burning = -1;
	int big_index[2] = { 0, 0 };
	for (int i = 0; i < s_bt.type_count; ++i) {
		const BtType* t = &s_bt.types[i];
		for (int c = 0; c < t->count && s_bt.ship_count < BT_MAX_FIGHTERS + BT_MAX_BIG; ++c) {
			BtShip* sh = &s_bt.ships[s_bt.ship_count++];
			memset(sh, 0, sizeof *sh);
			sh->type = i;
			sh->side = t->side;
			sh->cls = t->cls;
			sh->hero = t->hero && c == 0;
			sh->target = -1;
			const EnvMesh* m = bt_mesh(sh);
			const float len = m && m->len_m > 0.5f ? m->len_m : 12.0f;
			if (t->cls == BT_CLASS_FIGHTER) {
				/* Smaller is quicker; Interceptor-like light craft quickest. */
				sh->cruise = fminf(fmaxf(78.0f - len * 1.6f, 38.0f), 72.0f);
				/* Wide, steady turns (radians a second): small craft about
				 * 45 degrees a second, big ones half that. */
				sh->turn = fminf(fmaxf(0.95f - len * 0.015f, 0.4f), 0.8f);
				sh->hp_max = sh->hero ? 1e9f : 4.0f + floorf(len / 6.0f);
				bt_spawn_fighter(sh, 0.4f + bt_rnd(0.0f, 5.0f) + (float)c * 0.6f);
			} else {
				/* Capital ships hold the flanks a few kilometres out, broadside
				 * on, the station further back; they creep forward. */
				const float sgn = t->side == BT_FRIEND ? -1.0f : 1.0f;
				const int k = big_index[t->side]++;
				float yaw, pitch, dist;
				if (t->cls == BT_CLASS_STATION) {
					yaw = sgn * 62.0f;
					pitch = 14.0f;
					dist = fmaxf(5000.0f, len * 4.0f);
					sh->spin = bt_rnd(-1.0f, 1.0f) > 0.0f ? 0.6f : -0.6f;
				} else {
					yaw = sgn * (40.0f + 18.0f * (float)k);
					pitch = 6.0f + 10.0f * (float)k;
					dist = fmaxf(2600.0f + 1400.0f * (float)k, len * 2.6f);
				}
				bt_at(yaw, pitch, dist, sh->p);
				/* Nose across the line of sight, toward the other fleet. */
				float los[3] = { sh->p[0], sh->p[1], 0.0f }, side_dir[3];
				static const float z[3] = { 0.0f, 0.0f, 1.0f };
				bt_norm(los);
				env_cross(los, z, side_dir); /* right of the line of sight */
				const float toward = -sgn; /* friends (left) head right */
				for (int a = 0; a < 3; ++a) {
					sh->f[a] = side_dir[a] * toward * 0.9f + los[a] * 0.3f;
				}
				bt_norm(sh->f);
				memcpy(sh->u, z, sizeof sh->u);
				sh->cruise = t->cls == BT_CLASS_STATION ? 0.0f : bt_rnd(1.5f, 3.0f);
				sh->speed = sh->cruise;
				sh->hp_max = sh->hp = 1e9f;
				sh->alive = 1;
				sh->cooldown = bt_rnd(1.0f, 3.0f);
				if (t->side == BT_FOE && t->cls == BT_CLASS_BIG && s_bt.burning < 0) {
					s_bt.burning = s_bt.ship_count - 1; /* the mission's big hostile, burning */
					sh->burn = bt_rnd(0.5f, 2.0f);
				}
			}
		}
	}
	/* A few asteroids among the fighters, round the screen. */
	s_bt.rock_count = 0;
	for (int i = 0; i < BT_MAX_ROCKS - 1 && s_bt.rock_types > 0; ++i) {
		BtRock* r = &s_bt.rocks[s_bt.rock_count];
		memset(r, 0, sizeof *r);
		r->type = i % s_bt.rock_types;
		const int m = env_find(s_bt.rock_opt[r->type]);
		const float len = m >= 0 ? fmaxf(s_env.meshes[m].len_m, fmaxf(s_env.meshes[m].wid_m, s_env.meshes[m].hgt_m))
								 : 0.0f;
		if (!(len > 0.5f)) {
			continue;
		}
		const float size = i < 2 ? bt_rnd(90.0f, 160.0f) : bt_rnd(25.0f, 80.0f); /* across, metres */
		r->scale = size / len;
		r->radius = 0.5f * size;
		for (int tries = 0; tries < 20; ++tries) {
			const float yaw = (bt_rand() & 1 ? 1.0f : -1.0f) * bt_rnd(26.0f, 110.0f);
			bt_at(yaw, bt_rnd(-40.0f, 40.0f), bt_rnd(200.0f, 1040.0f), r->p);
			int clash = bt_behind_screen(r->p);
			for (int j = 0; j < s_bt.rock_count && !clash; ++j) {
				float d[3] = { r->p[0] - s_bt.rocks[j].p[0], r->p[1] - s_bt.rocks[j].p[1], r->p[2] - s_bt.rocks[j].p[2] };
				clash = bt_len(d) < 3.0f * (r->radius + s_bt.rocks[j].radius);
			}
			if (!clash) {
				break;
			}
		}
		/* Any attitude; a slow tumble about a random axis and a slow drift. */
		bt_at(bt_rnd(-180.0f, 180.0f), bt_rnd(-80.0f, 80.0f), 1.0f, r->f);
		bt_at(bt_rnd(-180.0f, 180.0f), bt_rnd(-80.0f, 80.0f), 1.0f, r->axis);
		float side[3];
		env_cross(r->f, r->axis, side);
		if (!bt_norm(side)) {
			side[0] = 1.0f;
		}
		env_cross(side, r->f, r->u);
		bt_norm(r->u);
		bt_at(bt_rnd(-180.0f, 180.0f), bt_rnd(-30.0f, 30.0f), bt_rnd(0.1f, 0.4f), r->v);
		r->spin = bt_rnd(2.0f, 7.0f) * BT_PI / 180.0f * (r->radius < 40.0f ? 1.6f : 1.0f);
		s_bt.rock_count++;
	}
	s_bt.started = 1;
}

static void bt_boom(const float p[3], float size, float dur, int kind) {
	BtBoom* slot = NULL;
	for (int i = 0; i < BT_MAX_BOOMS && !slot; ++i) {
		if (!s_bt.booms[i].alive) {
			slot = &s_bt.booms[i];
		}
	}
	if (!slot) {
		/* Replace the oldest spark first. */
		float best = -1.0f;
		for (int i = 0; i < BT_MAX_BOOMS; ++i) {
			const float age = s_bt.booms[i].t / s_bt.booms[i].dur + (s_bt.booms[i].kind == 1 ? 1.0f : 0.0f);
			if (age > best) {
				best = age;
				slot = &s_bt.booms[i];
			}
		}
	}
	memset(slot, 0, sizeof *slot);
	memcpy(slot->p, p, sizeof slot->p);
	slot->size = size;
	slot->dur = dur;
	slot->kind = kind;
	slot->alive = 1;
	slot->roll = bt_rnd(0.0f, 360.0f);
}

static void bt_fire(const float from[3], const float dir[3], int side, int big, int green) {
	BtBolt* b = NULL;
	for (int i = 0; i < BT_MAX_BOLTS && !b; ++i) {
		if (!s_bt.bolts[i].alive) {
			b = &s_bt.bolts[i];
		}
	}
	if (!b) {
		return;
	}
	memset(b, 0, sizeof *b);
	memcpy(b->p, from, sizeof b->p);
	memcpy(b->d, dir, sizeof b->d);
	bt_norm(b->d);
	b->side = side;
	b->big = big;
	b->alive = 1;
	b->speed = big ? 1100.0f : 330.0f;
	b->range = big ? 9000.0f : 650.0f;
	b->len = big ? 70.0f : 7.0f;
	b->width = big ? 3.0f : 0.45f;
	if (green) {
		b->color[0] = 0.35f, b->color[1] = 2.4f, b->color[2] = 0.45f;
	} else {
		b->color[0] = 2.4f, b->color[1] = 0.35f, b->color[2] = 0.25f;
	}
	s_bt.shots++;
}

/* Choose whom to chase: the nearest of the few closest foes, spread so no
 * one draws every attacker. */
static int bt_pick_target(int self) {
	const BtShip* me = &s_bt.ships[self];
	int best = -1;
	float best_score = 1e30f;
	for (int i = 0; i < s_bt.ship_count; ++i) {
		const BtShip* o = &s_bt.ships[i];
		if (!o->alive || o->side == me->side || o->cls != BT_CLASS_FIGHTER || o->arrive > 0.2f) {
			continue;
		}
		int chasers = 0;
		for (int j = 0; j < s_bt.ship_count; ++j) {
			chasers += s_bt.ships[j].alive && s_bt.ships[j].target == i;
		}
		float d[3] = { o->p[0] - me->p[0], o->p[1] - me->p[1], o->p[2] - me->p[2] };
		const float score = bt_len(d) * (1.0f + 0.8f * (float)chasers) * bt_rnd(0.8f, 1.25f);
		if (score < best_score) {
			best_score = score;
			best = i;
		}
	}
	return best;
}

/* Turn toward `want` at the craft's turn rate, banking into the turn. */
/* Turn toward `want_in`: the wish is smoothed (about half a second) so it
 * never jumps, small corrections are gentle (the turn rate scales with the
 * angle up to the craft's limit), and the craft banks by how hard it is
 * actually turning, rolling back level on the straight. */
static void bt_steer(BtShip* sh, const float want_in[3], float dt) {
	float want[3] = { want_in[0], want_in[1], want_in[2] };
	if (!bt_norm(want)) {
		return;
	}
	if (!(bt_len(sh->wish) > 0.5f)) {
		memcpy(sh->wish, sh->f, sizeof sh->wish);
	}
	const float blend = 1.0f - expf(-dt / 0.55f);
	for (int a = 0; a < 3; ++a) {
		sh->wish[a] += (want[a] - sh->wish[a]) * blend;
	}
	if (!bt_norm(sh->wish)) {
		memcpy(sh->wish, sh->f, sizeof sh->wish);
	}
	const float c = fminf(1.0f, fmaxf(-1.0f, bt_dot(sh->f, sh->wish)));
	const float angle = acosf(c);
	float axis[3];
	env_cross(sh->f, sh->wish, axis);
	if (!bt_norm(axis)) {
		/* Straight ahead: nothing to do; straight behind: pull up. */
		env_cross(sh->u, sh->f, axis);
		if (angle < 0.5f || !bt_norm(axis)) {
			axis[0] = axis[1] = axis[2] = 0.0f;
		}
	}
	const float rate = fminf(sh->turn, angle * 1.1f);
	if (rate * dt > 1e-6f && bt_len(axis) > 0.5f) {
		bt_rotate(sh->f, axis, rate * dt);
		bt_rotate(sh->u, axis, rate * dt);
	}
	/* Bank: from level (the sky's up), toward the side it turns to, by how
	 * hard it turns (65 degrees at the limit). */
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	float level[3], right[3], lateral[3];
	const float fz = bt_dot(sh->f, z);
	for (int a = 0; a < 3; ++a) {
		level[a] = z[a] - sh->f[a] * fz;
	}
	if (!bt_norm(level)) {
		memcpy(level, sh->u, sizeof level); /* straight up or down: keep it */
	}
	env_cross(sh->f, level, right);
	const float along = bt_dot(sh->wish, sh->f);
	for (int a = 0; a < 3; ++a) {
		lateral[a] = sh->wish[a] - sh->f[a] * along;
	}
	const float side = bt_dot(lateral, right);
	const float bank = (side >= 0.0f ? 1.0f : -1.0f) * 1.13f * (rate / sh->turn) *
					   fminf(1.0f, fabsf(side) / fmaxf(bt_len(lateral), 1e-4f) * 1.5f);
	float up_want[3] = { level[0], level[1], level[2] };
	bt_rotate(up_want, sh->f, bank);
	const float roll_c = fminf(1.0f, fmaxf(-1.0f, bt_dot(sh->u, up_want)));
	float roll_axis[3];
	env_cross(sh->u, up_want, roll_axis);
	const float sgn = bt_dot(roll_axis, sh->f) >= 0.0f ? 1.0f : -1.0f;
	const float roll = fminf(acosf(roll_c), fminf(1.0f, acosf(roll_c) * 1.5f + 0.05f) * dt);
	bt_rotate(sh->u, sh->f, sgn * roll);
	/* Re-orthonormalise. */
	const float uf = bt_dot(sh->u, sh->f);
	for (int a = 0; a < 3; ++a) {
		sh->u[a] -= sh->f[a] * uf;
	}
	bt_norm(sh->u);
	bt_norm(sh->f);
}

static void bt_kill(BtShip* sh) {
	const float r = bt_radius(sh);
	bt_boom(sh->p, 3.2f * r + 14.0f, 1.5f, 0);
	s_bt.kills++;
	bt_spawn_fighter(sh, bt_rnd(4.0f, 9.0f));
}

static void bt_step_fighter(int index, float dt) {
	BtShip* sh = &s_bt.ships[index];
	if (!sh->alive) {
		sh->respawn -= dt;
		if (sh->respawn <= 0.0f) {
			sh->alive = 1;
			sh->arrive = 1.0f; /* out of hyperspace */
			float flash[3];
			for (int a = 0; a < 3; ++a) {
				flash[a] = sh->p[a] + sh->f[a] * 6.0f;
			}
			bt_boom(flash, 3.0f * bt_radius(sh) + 8.0f, 0.35f, 3);
		}
		return;
	}
	sh->timer -= dt;
	sh->cooldown -= dt;
	if (sh->arrive > 0.0f) {
		sh->arrive = fmaxf(0.0f, sh->arrive - dt * 1.5f);
	}
	float want[3] = { sh->f[0], sh->f[1], sh->f[2] };
	BtShip* target = sh->target >= 0 ? &s_bt.ships[sh->target] : NULL;
	if (target && (!target->alive || target->side == sh->side)) {
		target = NULL;
		sh->target = -1;
	}
	if (sh->mode == BT_HUNT) {
		if (!target || sh->timer <= 0.0f) {
			sh->target = bt_pick_target(index);
			target = sh->target >= 0 ? &s_bt.ships[sh->target] : NULL;
			sh->timer = bt_rnd(9.0f, 16.0f);
		}
		if (!target) {
			sh->mode = BT_RETURN;
			bt_pick_goal(sh->goal, sh->side);
			sh->timer = bt_rnd(2.0f, 4.0f);
		} else {
			/* Lead: where the target will be when the bolts get there. */
			float to[3] = { target->p[0] - sh->p[0], target->p[1] - sh->p[1], target->p[2] - sh->p[2] };
			const float dist = bt_len(to);
			const float t_hit = dist / 330.0f;
			float lead[3];
			for (int a = 0; a < 3; ++a) {
				lead[a] = to[a] + target->f[a] * target->speed * t_hit;
			}
			memcpy(want, lead, sizeof want);
			float aim[3] = { lead[0], lead[1], lead[2] };
			bt_norm(aim);
			const float lined_up = bt_dot(aim, sh->f);
			if (lined_up > 0.982f && dist < 420.0f && sh->cooldown <= 0.0f) {
				const EnvMesh* m = bt_mesh(sh);
				const BtType* t = &s_bt.types[sh->type];
				const float w = m && m->wid_m > 0.5f ? m->wid_m : 8.0f;
				const float l = m && m->len_m > 0.5f ? m->len_m : 10.0f;
				float r[3];
				env_cross(sh->f, sh->u, r);
				const float spread = t->green ? 0.12f : 0.42f;
				for (int g = -1; g <= 1; g += 2) {
					float from[3], dir[3];
					for (int a = 0; a < 3; ++a) {
						from[a] = sh->p[a] + sh->f[a] * 0.45f * l + r[a] * (float)g * spread * w -
								  sh->u[a] * (t->green ? 0.12f * w : 0.0f);
						dir[a] = aim[a] + bt_rnd(-0.012f, 0.012f);
					}
					bt_fire(from, dir, sh->side, 0, t->green);
				}
				sh->cooldown = bt_rnd(0.5f, 1.0f);
			}
			/* Overshoot or too close: break away in a long sweep, then come
			 * round again. */
			if (dist < 70.0f || (dist < 180.0f && lined_up < 0.0f)) {
				sh->mode = BT_EVADE;
				for (int tries = 0; tries < 6; ++tries) {
					bt_pick_goal(sh->goal, sh->side);
					float away[3] = { sh->goal[0] - sh->p[0], sh->goal[1] - sh->p[1], sh->goal[2] - sh->p[2] };
					if (bt_len(away) > 400.0f && bt_dot(away, sh->f) > 0.0f) {
						break;
					}
				}
				sh->timer = bt_rnd(5.0f, 9.0f);
			}
		}
	} else {
		/* Evade / return: fly to the goal. */
		for (int a = 0; a < 3; ++a) {
			want[a] = sh->goal[a] - sh->p[a];
		}
		const float d = bt_len(want);
		bt_norm(want);
		if (d < 80.0f || sh->timer <= 0.0f) {
			sh->mode = BT_HUNT;
			sh->timer = 0.0f;
		}
	}
	/* Keep clear of the viewer and of the space behind the screen, and in
	 * the arena. */
	const float view_d = bt_len(sh->p);
	bt_norm(want);
	{
		/* Where the present course passes the viewer in the next 3 s: steer
		 * out from that point while it is closer than 80 m. */
		const float t = fminf(fmaxf(-bt_dot(sh->p, sh->f), 0.0f), sh->speed * 3.0f);
		float c[3];
		for (int a = 0; a < 3; ++a) {
			c[a] = sh->p[a] + sh->f[a] * t;
		}
		const float miss = bt_len(c);
		if (miss < 80.0f) {
			if (!bt_norm(c)) {
				memcpy(c, sh->u, sizeof c);
			}
			const float k = 3.5f * (80.0f - miss) / 80.0f + (view_d < 50.0f ? 2.0f : 0.0f);
			for (int a = 0; a < 3; ++a) {
				want[a] += c[a] * k;
			}
		}
	}
	if (bt_behind_screen(sh->p) && sh->arrive <= 0.0f) {
		/* Sideways out of the cone, the nearer way. */
		const float sgn = sh->p[0] >= 0.0f ? 1.0f : -1.0f;
		want[0] += sgn * 1.4f;
	}
	/* Round the asteroids. */
	for (int i = 0; i < s_bt.rock_count; ++i) {
		const BtRock* r = &s_bt.rocks[i];
		float rel[3] = { r->p[0] - sh->p[0], r->p[1] - sh->p[1], r->p[2] - sh->p[2] };
		const float t = fminf(fmaxf(bt_dot(rel, sh->f), 0.0f), sh->speed * 5.0f);
		float c[3];
		for (int a = 0; a < 3; ++a) {
			c[a] = sh->p[a] + sh->f[a] * t - r->p[a];
		}
		const float clear = r->radius + 70.0f;
		const float miss = bt_len(c);
		if (miss < clear && t > 0.0f) {
			if (!bt_norm(c)) {
				memcpy(c, sh->u, sizeof c);
			}
			for (int a = 0; a < 3; ++a) {
				want[a] += c[a] * 4.0f * (clear - miss) / clear;
			}
		}
	}
	if (view_d > 1184.0f) {
		float home[3];
		bt_at(sh->side == BT_FRIEND ? -35.0f : 35.0f, 10.0f, 368.0f, home);
		for (int a = 0; a < 3; ++a) {
			home[a] -= sh->p[a];
		}
		bt_norm(home);
		const float k = fminf((view_d - 1184.0f) / 288.0f, 2.0f);
		for (int a = 0; a < 3; ++a) {
			want[a] += home[a] * k;
		}
	}
	if (sh->arrive <= 0.0f) {
		bt_steer(sh, want, dt);
	}
	const float target_speed = sh->cruise * (sh->mode == BT_EVADE ? 1.2f : 1.0f);
	sh->speed += (target_speed - sh->speed) * fminf(1.0f, dt * 1.5f);
	const float speed = sh->speed + 900.0f * sh->arrive * sh->arrive;
	for (int a = 0; a < 3; ++a) {
		sh->p[a] += sh->f[a] * speed * dt;
	}
}

static void bt_step_big(int index, float dt) {
	BtShip* sh = &s_bt.ships[index];
	if (sh->spin != 0.0f) {
		static const float z[3] = { 0.0f, 0.0f, 1.0f };
		bt_rotate(sh->f, z, sh->spin * BT_PI / 180.0f * dt);
	}
	if (sh->drift < 350.0f) {
		for (int a = 0; a < 3; ++a) {
			sh->p[a] += sh->f[a] * sh->speed * dt;
		}
		sh->drift += sh->speed * dt;
	}
	const EnvMesh* m = bt_mesh(sh);
	const float len = m && m->len_m > 1.0f ? m->len_m : 600.0f;
	const float wid = m && m->wid_m > 1.0f ? m->wid_m : 300.0f;
	const float hgt = m && m->hgt_m > 1.0f ? m->hgt_m : 150.0f;
	float r[3];
	env_cross(sh->f, sh->u, r);
	/* Turbolasers: at the enemy fleet mostly, sometimes into the fight. */
	sh->cooldown -= dt;
	if (sh->cooldown <= 0.0f && sh->cls == BT_CLASS_BIG) {
		int target = -1;
		const int at_big = (bt_rand() % 100) < 65;
		for (int tries = 0; tries < 8 && target < 0; ++tries) {
			const int i = (int)(bt_rand() % (uint32_t)s_bt.ship_count);
			const BtShip* o = &s_bt.ships[i];
			if (o->alive && o->side != sh->side && (o->cls != BT_CLASS_FIGHTER) == at_big && o->arrive <= 0.0f) {
				target = i;
			}
		}
		if (target >= 0) {
			const BtShip* o = &s_bt.ships[target];
			float from[3], dir[3];
			for (int a = 0; a < 3; ++a) {
				from[a] = sh->p[a] + sh->f[a] * bt_rnd(-0.35f, 0.35f) * len + r[a] * bt_rnd(-0.2f, 0.2f) * wid +
						  sh->u[a] * 0.25f * hgt;
			}
			const float spread = o->cls == BT_CLASS_FIGHTER ? 25.0f : 0.25f * bt_radius(o);
			for (int a = 0; a < 3; ++a) {
				dir[a] = o->p[a] + bt_rnd(-spread, spread) - from[a];
			}
			bt_fire(from, dir, sh->side, 1, s_bt.types[sh->type].green);
		}
		sh->cooldown = bt_rnd(0.4f, 1.1f);
	}
	/* The burning one: fires breaking out along its hull. */
	if (index == s_bt.burning) {
		sh->burn -= dt;
		if (sh->burn <= 0.0f) {
			float p[3];
			for (int a = 0; a < 3; ++a) {
				p[a] = sh->p[a] + sh->f[a] * bt_rnd(-0.4f, 0.4f) * len + r[a] * bt_rnd(-0.25f, 0.25f) * wid +
					   sh->u[a] * bt_rnd(0.0f, 0.3f) * hgt;
			}
			bt_boom(p, bt_rnd(0.06f, 0.12f) * len, bt_rnd(1.2f, 2.2f), 0);
			sh->burn = bt_rnd(1.5f, 4.0f);
		}
	}
}

static void bt_step_bolts(float dt) {
	for (int i = 0; i < BT_MAX_BOLTS; ++i) {
		BtBolt* b = &s_bt.bolts[i];
		if (!b->alive) {
			continue;
		}
		const float step = b->speed * dt;
		float from[3];
		memcpy(from, b->p, sizeof from);
		for (int a = 0; a < 3; ++a) {
			b->p[a] += b->d[a] * step;
		}
		b->travelled += step;
		if (b->travelled > b->range) {
			b->alive = 0;
			continue;
		}
		/* Asteroids stop bolts. */
		for (int r = 0; r < s_bt.rock_count && b->alive; ++r) {
			const BtRock* rock = &s_bt.rocks[r];
			float rel[3] = { rock->p[0] - from[0], rock->p[1] - from[1], rock->p[2] - from[2] };
			const float t = fminf(fmaxf(bt_dot(rel, b->d), 0.0f), step);
			float c[3];
			for (int a = 0; a < 3; ++a) {
				c[a] = from[a] + b->d[a] * t - rock->p[a];
			}
			if (bt_dot(c, c) < 0.6f * rock->radius * rock->radius) {
				float at[3];
				for (int a = 0; a < 3; ++a) {
					at[a] = from[a] + b->d[a] * t;
				}
				bt_boom(at, b->big ? 18.0f : 5.0f, 0.4f, 1);
				b->alive = 0;
			}
		}
		/* Hits: the closest approach of this step to each enemy. */
		for (int s = 0; s < s_bt.ship_count && b->alive; ++s) {
			BtShip* sh = &s_bt.ships[s];
			if (!sh->alive || sh->side == b->side || sh->arrive > 0.3f) {
				continue;
			}
			if (!b->big && sh->cls != BT_CLASS_FIGHTER) {
				continue; /* fighters do not shoot at capital ships */
			}
			const float rad = sh->cls == BT_CLASS_FIGHTER ? bt_radius(sh) * 0.75f : bt_radius(sh) * 0.45f;
			float rel[3] = { sh->p[0] - from[0], sh->p[1] - from[1], sh->p[2] - from[2] };
			const float t = fminf(fmaxf(bt_dot(rel, b->d), 0.0f), step);
			float c[3];
			for (int a = 0; a < 3; ++a) {
				c[a] = from[a] + b->d[a] * t - sh->p[a];
			}
			if (bt_dot(c, c) > rad * rad) {
				continue;
			}
			b->alive = 0;
			s_bt.hits++;
			float at[3];
			for (int a = 0; a < 3; ++a) {
				at[a] = from[a] + b->d[a] * t;
			}
			if (sh->cls != BT_CLASS_FIGHTER) {
				bt_boom(at, fmaxf(12.0f, 0.04f * bt_radius(sh) * 2.0f), 0.9f, 1);
			} else {
				bt_boom(at, fmaxf(2.0f, 0.5f * bt_radius(sh)), 0.35f, 1);
				sh->hp -= b->big ? 4.0f : 1.0f;
				if (sh->hp <= 0.0f) {
					bt_kill(sh);
				}
			}
		}
	}
}

/* The craft start once every model has been tried (their sizes set the
 * speeds and the capital ships' distances). */
static int bt_ready(void) {
	for (int i = 0; i < s_env.mesh_count; ++i) {
		if (!s_env.meshes[i].tried) {
			return 0;
		}
	}
	return 1;
}

static void bt_step(float dt) {
	if (!s_bt.started) {
		if (!bt_ready()) {
			return;
		}
		bt_start();
	}
	s_bt.time += dt;
	for (int i = 0; i < s_bt.ship_count; ++i) {
		if (s_bt.ships[i].cls == BT_CLASS_FIGHTER) {
			bt_step_fighter(i, dt);
		} else {
			bt_step_big(i, dt);
		}
	}
	for (int i = 0; i < s_bt.rock_count; ++i) {
		BtRock* r = &s_bt.rocks[i];
		bt_rotate(r->f, r->axis, r->spin * dt);
		bt_rotate(r->u, r->axis, r->spin * dt);
		/* Drifting toward the viewer or behind the screen: turn the drift
		 * round. */
		float next[3];
		for (int a = 0; a < 3; ++a) {
			next[a] = r->p[a] + r->v[a] * dt;
		}
		if ((bt_len(next) < 180.0f + r->radius && bt_dot(r->v, r->p) < 0.0f) || bt_len(next) > 1200.0f ||
			(bt_behind_screen(next) && !bt_behind_screen(r->p))) {
			for (int a = 0; a < 3; ++a) {
				r->v[a] = -r->v[a];
			}
		} else {
			memcpy(r->p, next, sizeof next);
		}
	}
	bt_step_bolts(dt);
	for (int i = 0; i < BT_MAX_BOOMS; ++i) {
		BtBoom* b = &s_bt.booms[i];
		if (b->alive) {
			b->t += dt;
			b->alive = b->t < b->dur;
		}
	}
}

/* Advances the battle by the real time since the last frame (sub-stepped,
 * capped so a stall does not jump the craft). */
static void bt_advance(void) {
	const uint64_t now = SDL_GetPerformanceCounter();
	float dt = s_bt.last_counter ? (float)((double)(now - s_bt.last_counter) / (double)SDL_GetPerformanceFrequency())
								 : 0.0f;
	s_bt.last_counter = now;
	dt = fminf(fmaxf(dt, 0.0f), 0.1f);
	while (dt > 0.0f) {
		const float h = fminf(dt, 1.0f / 60.0f);
		bt_step(h);
		dt -= h;
	}
}

/* ---- drawing ---------------------------------------------------------------- */

static void bt_lighting(XwaVrEnvLighting* out, float sun) {
	/* The mission's sun when it has one, else a white star upper left. */
	float dir[3] = { -0.55f, -0.45f, 0.7f }; /* over the left shoulder */
	float col[3] = { 1.0f, 0.96f, 0.9f };
	if (s_bt.sun >= 0) {
		const BtBackdrop* b = &s_bt.backdrops[s_bt.sun];
		env_dir(b->yaw, b->pitch, dir);
		if (b->has_color) {
			const float peak = fmaxf(b->color[0], fmaxf(b->color[1], b->color[2]));
			for (int a = 0; a < 3 && peak > 0.0f; ++a) {
				col[a] = b->color[a] / peak;
			}
		}
	}
	for (int a = 0; a < 3; ++a) {
		out->key_dir[a] = dir[a];
		out->key_color[a] = col[a] * sun;
	}
	/* A cool fill from the viewer's side, so craft read against space. */
	env_set3(out->fill_dir, 0.15f, -0.85f, 0.35f);
	env_set3(out->fill_color, 0.30f * 0.9f, 0.34f * 0.9f, 0.45f * 0.9f);
	env_set3(out->ambient_sides, 0.10f, 0.10f, 0.115f);
	env_set3(out->ambient_top, 0.11f, 0.11f, 0.12f);
	env_set3(out->ambient_bottom, 0.075f, 0.075f, 0.09f);
}

static void bt_glow(AeronScene3D* scene, const float p[3], float size, const float tint[4]) {
	float c[4][3];
	env_facing(p, size, 0.0f, c);
	env_quad(scene, ENV_TEX_GLOW, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_ADDITIVE, c, tint);
}

static void bt_submit(AeronScene3D* scene) {
	const float k = AERON_OPT_UNITS_PER_METER;
	/* The mission's sky. */
	for (int i = 0; i < s_bt.backdrop_count; ++i) {
		const BtBackdrop* b = &s_bt.backdrops[i];
		const int slot = ENV_TEX_BACKDROP + i;
		if (!s_env.tex[slot]) {
			continue;
		}
		float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		if (b->sun && b->has_color) {
			for (int a = 0; a < 3; ++a) {
				tint[a] = fminf(b->color[a] * 1.2f, 2.0f);
			}
		}
		float c[4][3];
		env_sky_sprite(scene, slot, b->yaw, b->pitch, b->size_deg, 0.0f, tint, c);
	}
	/* The craft. */
	for (int i = 0; i < s_bt.ship_count; ++i) {
		const BtShip* sh = &s_bt.ships[i];
		const int m = env_find(s_bt.types[sh->type].opt);
		if (!sh->alive || m < 0 || s_env.count >= ENV_MAX_INSTANCES) {
			continue;
		}
		float pos[3];
		for (int a = 0; a < 3; ++a) {
			pos[a] = sh->p[a] * k;
		}
		const int n = s_env.count++;
		s_env.mesh_of[n] = m;
		s_env.far[n] = 1; /* no shadows from the battle */
		env_pose_matrix(sh->f, sh->u, pos, s_env.transform[n]);
		const EnvMesh* em = &s_env.meshes[m];
		if (sh->cls == BT_CLASS_FIGHTER) {
			/* The hyperspace streak behind a craft jumping in. */
			if (sh->arrive > 0.0f) {
				float from[3];
				for (int a = 0; a < 3; ++a) {
					from[a] = (sh->p[a] - sh->f[a] * 420.0f * sh->arrive) * k;
				}
				const float streak[4] = { 1.6f * sh->arrive, 1.8f * sh->arrive, 2.2f * sh->arrive, 1.0f };
				env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, from, pos, fmaxf(em->wid_m, 3.0f) * 0.6f * k,
						 streak);
			}
		}
	}
	/* The asteroids, scaled to their sizes. */
	for (int i = 0; i < s_bt.rock_count && s_env.count < ENV_MAX_INSTANCES; ++i) {
		const BtRock* r = &s_bt.rocks[i];
		const int m = env_find(s_bt.rock_opt[r->type]);
		if (m < 0) {
			continue;
		}
		float pos[3];
		for (int a = 0; a < 3; ++a) {
			pos[a] = r->p[a] * k;
		}
		const int n = s_env.count++;
		s_env.mesh_of[n] = m;
		s_env.far[n] = 1;
		env_pose_matrix(r->f, r->u, pos, s_env.transform[n]);
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 3; ++col) {
				s_env.transform[n][row * 4 + col] *= r->scale;
			}
		}
	}
	/* Laser bolts: wide enough to read at their distance. */
	for (int i = 0; i < BT_MAX_BOLTS; ++i) {
		const BtBolt* b = &s_bt.bolts[i];
		if (!b->alive) {
			continue;
		}
		float a0[3], a1[3];
		const float shown = fminf(b->len, b->travelled);
		for (int a = 0; a < 3; ++a) {
			a0[a] = (b->p[a] - b->d[a] * shown) * k;
			a1[a] = b->p[a] * k;
		}
		const float width = fmaxf(b->width, bt_len(b->p) * 0.0035f) * k;
		const float tint[4] = { b->color[0], b->color[1], b->color[2], 1.0f };
		env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, a0, a1, width, tint);
	}
	/* Explosions, sparks and hyperspace flashes. */
	for (int i = 0; i < BT_MAX_BOOMS; ++i) {
		const BtBoom* b = &s_bt.booms[i];
		if (!b->alive) {
			continue;
		}
		const float u = b->t / b->dur;
		float p[3], c[4][3];
		for (int a = 0; a < 3; ++a) {
			p[a] = b->p[a] * k;
		}
		if (b->kind == 3) {
			const float g = 2.5f * (1.0f - u);
			const float tint[4] = { 0.8f * g, 0.9f * g, 1.2f * g, 1.0f };
			bt_glow(scene, p, b->size * (0.4f + u) * k, tint);
			continue;
		}
		int frame = (int)(u * (float)ENV_BOOM_FRAMES);
		frame = frame < 0 ? 0 : frame >= ENV_BOOM_FRAMES ? ENV_BOOM_FRAMES - 1 : frame;
		const int slot = s_env.tex[ENV_TEX_BOOM + frame] ? ENV_TEX_BOOM + frame : ENV_TEX_BLAST;
		const float grow = b->kind == 1 ? 0.6f + 0.6f * u : 0.75f + 0.45f * u;
		static const float white[4] = { 1.5f, 1.4f, 1.3f, 1.0f };
		env_facing(p, b->size * grow * k, b->roll, c);
		env_quad(scene, slot, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, AERON_SCENE_BILLBOARD_BLEND_PMA, c, white);
		if (b->kind == 0 && u < 0.25f) {
			const float g = 3.0f * (1.0f - u / 0.25f);
			const float flash[4] = { 1.0f * g, 0.8f * g, 0.5f * g, 1.0f };
			bt_glow(scene, p, b->size * 1.6f * k, flash);
		}
	}
	env_submit_logo(scene);
}

/* ---- the TIE Fighter box art, played ------------------------------------
 * Ten seconds of the fight before the cover's moment, flown the way the
 * live battle's craft fly: the nose always along the flight path, rolling
 * only into a turn. Each craft comes in fast on a straight line, rolls as
 * it enters its last turn and pulls through it (turning toward its own top
 * side, the turn's centre above its canopy) straight into its pose on the
 * cover, which is where that turn has brought it. Of a few turn angles and
 * rates the one whose path keeps clearest of the viewer and least behind
 * the screen is taken; the X-wing's and its killer's are picked together so
 * the TIE's guns bear on the X-wing as it is hit. Then the great explosion
 * grows into the cover's and the still holds. */

#define ENV_TIE_CANDIDATES 234 /* 6 last-turn angles x 3 rates x 13 first turns */

static float env_tie_speed(int ship) {
	const char* opt = env_space()->ships[ship].opt;
	float v = 70.0f; /* m/s */
	if (opt && !SDL_strncasecmp(opt, "TieInterceptor", 14)) {
		v = 80.0f;
	} else if (opt && !SDL_strncasecmp(opt, "TieBomber", 9)) {
		v = 55.0f;
	} else if (opt && !SDL_strncasecmp(opt, "TieAdvanced", 11)) {
		v = 85.0f;
	} else if (opt && !SDL_strncasecmp(opt, "BWing", 5)) {
		v = 55.0f;
	} else if (s_env.mode != ENV_TIEART && opt && !SDL_strncasecmp(opt, "AWing", 5)) {
		v = 80.0f;
	} else if (!es_is_tie(ship)) {
		v = 62.0f;
	}
	return v * env_float("XWA_XR_TIEART_SPEED", 1.0f) * AERON_OPT_UNITS_PER_METER;
}

static void env_tie_candidate(int ship, int c) {
	static const float alphas[6] = { 25.0f, 45.0f, 65.0f, 85.0f, 110.0f, 140.0f };
	static const float rates[3] = { 0.7f, 1.0f, 1.3f };
	/* No first turn, or one of 60/120/180 degrees banked 55 or 85 degrees
	 * either way (85: nearly a flat turn across the sky). */
	static const float firsts[13][2] = {
		{ 0.0f, 0.0f },	   { 60.0f, 55.0f },   { 60.0f, -55.0f },  { 60.0f, 85.0f },	{ 60.0f, -85.0f },
		{ 120.0f, 55.0f }, { 120.0f, -55.0f }, { 120.0f, 85.0f },  { 120.0f, -85.0f }, { 180.0f, 55.0f },
		{ 180.0f, -55.0f }, { 180.0f, 85.0f }, { 180.0f, -85.0f },
	};
	const float deg = BT_PI / 180.0f;
	s_env.anim_fl[ship].v = env_tie_speed(ship);
	s_env.anim_fl[ship].alpha = alphas[c % 6] * deg;
	s_env.anim_fl[ship].w = rates[(c / 6) % 3];
	s_env.anim_fl[ship].beta = firsts[(c / 18) % 13][0] * deg;
	s_env.anim_fl[ship].b1 = firsts[(c / 18) % 13][1] * deg;
}

/* The sky's up across nose n (or `fallback` when n is vertical). */
static void env_tie_level(const float n[3], const float fallback[3], float out[3]) {
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	const float nz = bt_dot(n, z);
	for (int a = 0; a < 3; ++a) {
		out[a] = z[a] - n[a] * nz;
	}
	if (!bt_norm(out)) {
		memcpy(out, fallback, 3 * sizeof(float));
	}
}

/* The signed roll about n taking a to b. */
static float env_tie_roll(const float a[3], const float b[3], const float n[3]) {
	float x[3];
	env_cross(a, b, x);
	return atan2f(bt_dot(x, n), bt_dot(a, b));
}

/* How well a path does: clear of the viewer, in view (the last seconds
 * most), not behind the screen. */
static float env_tie_path_score(int ship) {
	const float k = AERON_OPT_UNITS_PER_METER;
	float e[3];
	env_ship_pos(ship, e);
	const float end_m = bt_len(e) / k;
	float closest = 1e9f, roll_max = 0.0f, prev_u[3] = { 0.0f, 0.0f, 0.0f };
	int hidden = 0, seen = 0;
	float out_of_view = 0.0f, steep = 0.0f;
	for (int i = 0; i <= 40; ++i) {
		float p[3], n[3], u[3];
		env_tie_pose(ship, ENV_TIE_ANIM_SECONDS * (float)i / 40.0f * 0.999f, p, n, u);
		if (i > 0) {
			/* Turn plus roll, per second: the attitude's rate. */
			roll_max = fmaxf(roll_max, acosf(fminf(1.0f, bt_dot(prev_u, u))) / (ENV_TIE_ANIM_SECONDS / 40.0f));
		}
		memcpy(prev_u, u, sizeof prev_u);
		for (int a = 0; a < 3; ++a) {
			p[a] /= k;
		}
		const float d = bt_len(p);
		if (i < 40) {
			closest = fminf(closest, d);
		}
		const int late = i >= 24 ? 3 : 1;
		hidden += bt_behind_screen(p) * late;
		const float yaw = atan2f(p[0], p[1]) * 180.0f / BT_PI;
		const float pitch = atan2f(p[2], sqrtf(p[0] * p[0] + p[1] * p[1])) * 180.0f / BT_PI;
		/* In front and near enough to watch. */
		seen += (i >= 8 && fabsf(yaw) < 75.0f && pitch > -45.0f && pitch < 55.0f) * ((d < 350.0f) + 2 * (d < 160.0f)) *
				late;
		/* From 0.4 of the way in (about 4 s before the end) every craft in
		 * the middle of the view: how far out of it. */
		if (i >= 16) {
			out_of_view += fmaxf(fabsf(yaw) - 38.0f, 0.0f) + fmaxf(pitch - 25.0f, 0.0f) + fmaxf(-32.0f - pitch, 0.0f);
		}
		/* No diving in steeply from above (or climbing from below). */
		if (i >= 8) {
			steep += fmaxf(fabsf(n[2]) - 0.5f, 0.0f);
		}
		/* The X-wing's chaser: the chase right in the middle of the view. */
		if (ship == env_anim()->chase_ship && i >= 7 && i <= 30) {
			out_of_view += 2.0f * (fabsf(yaw) + fabsf(pitch + 6.0f));
		}
	}
	/* Snap rolls look wrong: more than about 2 rad/s costs. */
	return fminf(closest, fmaxf(0.7f * end_m, 8.0f)) - (closest < 5.0f ? 100.0f : 0.0f) - 1.5f * (float)hidden +
		   0.12f * (float)seen - 15.0f * fmaxf(roll_max - 2.0f, 0.0f) - 0.35f * out_of_view - 4.0f * steep;
}

/* The runners: each flies its chaser's path 50 m ahead of it, then
 * breaks away: a barrel roll (a roll round a small helix, the nose along
 * the path all the way) if it has one, then a pull out of the forward
 * view. */
static int env_tie_runner_of(int ship) {
	const EnvAnim* an = env_anim();
	for (int r = 0; r < an->runner_count; ++r) {
		if (an->runners[r].ship == ship) {
			return r;
		}
	}
	return -1;
}

/* The ship `ship` flies in formation on (-1: none). */
static int env_tie_leader_of(int ship) {
	const EnvAnim* an = env_anim();
	return an->leader && ship >= 0 && ship < an->animated ? an->leader[ship] : -1;
}

static void env_tie_runner_pose(int r, float t, float pos[3], float n[3], float u[3]);

static void env_tie_runner_plan(void) {
	const float k = AERON_OPT_UNITS_PER_METER;
	const float T = ENV_TIE_ANIM_SECONDS;
	const EnvAnim* an = env_anim();
	for (int r = 0; r < ENV_TIE_RUNNERS; ++r) {
		s_env.runner[r].ok = 0;
	}
	for (int r = 0; r < an->runner_count && r < ENV_TIE_RUNNERS; ++r) {
		__typeof__(s_env.runner[0])* rn = &s_env.runner[r];
		const EnvRunner* kr = &an->runners[r];
		const int chaser = kr->chaser;
		rn->ok = 0;
		if (env_space()->count <= kr->ship || !s_env.anim_fl[chaser].v) {
			continue;
		}
		const float v = s_env.anim_fl[chaser].v;
		const float lead = kr->lead_m * k / v;
		if (kr->weave) {
			rn->chaser = chaser;
			rn->lead = lead;
			rn->v = v;
			rn->tb = ENV_TIE_ANIM_SECONDS;
			rn->flyby_done = 1; /* it never comes close */
			rn->ok = 1;
			continue;
		}
		float tb = T - lead - kr->end_s;
		const float break_m =
			kr->break_m * (r == 0 ? env_float("XWA_XR_RUNNER_BREAK_M", 130.0f) / 130.0f : 1.0f);
		for (float t = 0.0f; break_m > 0.0f && t < tb; t += 0.02f) {
			float p[3], nn[3], uu[3];
			env_tie_flight_pose(chaser, t + lead, p, nn, uu);
			if (bt_len(p) / k < break_m) {
				tb = t;
				break;
			}
		}
		rn->chaser = chaser;
		rn->lead = lead;
		rn->tb = fmaxf(tb, 0.0f);
		rn->v = v;
		rn->roll_time = kr->roll_s;
		rn->helix = 3.0f * k;
		env_tie_flight_pose(chaser, rn->tb + lead, rn->pb, rn->nb, rn->ub);
		const float* away = kr->away;
		const float along = bt_dot(away, rn->nb);
		for (int a = 0; a < 3; ++a) {
			rn->pull_up[a] = away[a] - rn->nb[a] * along;
		}
		if (!bt_norm(rn->pull_up)) {
			memcpy(rn->pull_up, rn->ub, sizeof rn->pull_up);
		}
		rn->pull_w = kr->pull_w;
		rn->pull_bank = env_tie_roll(rn->ub, rn->pull_up, rn->nb);
		rn->flyby_done = 0;
		rn->ok = 1;
	}
}

static void env_tie_runner_pose(int r, float t, float pos[3], float n[3], float u[3]) {
	const __typeof__(s_env.runner[0])* rn = &s_env.runner[r];
	if (!rn->ok) {
		return;
	}
	if (t < rn->tb) {
		env_tie_flight_pose(rn->chaser, t + rn->lead, pos, n, u);
		return;
	}
	const float v = rn->v, D1 = rn->roll_time, Rh = rn->helix;
	const float* nb = rn->nb;
	const float* ub = rn->ub;
	const float s = t - rn->tb;
	if (s < D1) {
		/* The barrel roll: a full roll while flying round a helix whose axis
		 * runs along the old heading, its centre toward the canopy. */
		const float x = s / D1;
		const float phi = 2.0f * BT_PI * x * x * (3.0f - 2.0f * x);
		const float dphi = 2.0f * BT_PI * 6.0f * x * (1.0f - x) / D1;
		memcpy(u, ub, 3 * sizeof(float));
		bt_rotate(u, nb, phi);
		float side[3];
		env_cross(nb, u, side); /* du/dphi */
		for (int a = 0; a < 3; ++a) {
			pos[a] = rn->pb[a] + nb[a] * v * s + Rh * (ub[a] - u[a]);
			n[a] = nb[a] * v - Rh * dphi * side[a];
		}
		bt_norm(n);
		const float d = bt_dot(u, n);
		for (int a = 0; a < 3; ++a) {
			u[a] -= n[a] * d;
		}
		bt_norm(u);
		return;
	}
	/* The pull: rolling onto the turn as it begins, then round. */
	const float s2 = s - D1;
	const float w = rn->pull_w, R = v / w, th = w * s2;
	const float* up = rn->pull_up;
	for (int a = 0; a < 3; ++a) {
		n[a] = nb[a] * cosf(th) + up[a] * sinf(th);
		u[a] = up[a] * cosf(th) - nb[a] * sinf(th);
		pos[a] = rn->pb[a] + nb[a] * v * D1 + R * (up[a] - u[a]);
	}
	const float f = env_smooth(s2 / 0.45f);
	if (f < 1.0f) {
		bt_rotate(u, n, -(1.0f - f) * rn->pull_bank);
	}
}

static void env_tie_paths(void) {
	const EnvSpaceScene* sc = env_space();
	const EnvAnim* an = env_anim();
	const float k = AERON_OPT_UNITS_PER_METER;
	const int count = sc->count < an->animated ? sc->count : an->animated;
	const int victim = an->victim;
	s_env.anim_paths = 1; /* env_tie_pose follows anim_fl from here */
	static float scores[8][ENV_TIE_CANDIDATES];
	for (int i = 0; i < count; ++i) {
		int best = 0;
		s_env.anim_next_shot[i] = 1.2f + 0.27f * (float)i;
		if (env_tie_runner_of(i) >= 0 || env_tie_leader_of(i) >= 0) {
			continue; /* flies after its chaser (env_tie_runner_plan) or on its leader */
		}
		for (int c = 0; c < ENV_TIE_CANDIDATES; ++c) {
			env_tie_candidate(i, c);
			scores[i][c] = env_tie_path_score(i);
			if (scores[i][c] > scores[i][best]) {
				best = c;
			}
		}
		env_tie_candidate(i, best);
	}
	/* The victim's killer: the scene's, or a TIE fighter (any TIE when there
	 * is none). It and the victim take the paths, among their good ones, on
	 * which its guns bear best on the victim while it hits it. */
	s_env.anim_hunter = -1;
	env_tie_runner_plan();
	if (count <= victim) {
		return;
	}
	int fighters = 0;
	for (int i = 0; i < count; ++i) {
		fighters += i != victim && sc->ships[i].opt && !SDL_strncasecmp(sc->ships[i].opt, "TieFighter", 10);
	}
	/* Each one's best few paths only. */
	enum { TOP = 24 };
	int top[8][TOP];
	for (int i = 0; i < count; ++i) {
		for (int j = 0; j < TOP; ++j) {
			top[i][j] = -1;
			for (int c = 0; c < ENV_TIE_CANDIDATES; ++c) {
				int taken = 0;
				for (int q = 0; q < j; ++q) {
					taken |= top[i][q] == c;
				}
				if (!taken && (top[i][j] < 0 || scores[i][c] > scores[i][top[i][j]])) {
					top[i][j] = c;
				}
			}
		}
	}
	float best = -1e9f;
	int best_h = -1, best_hc = 0, best_xc = 0;
	for (int h = 0; h < count; ++h) {
		if (h == victim || env_tie_runner_of(h) >= 0 || env_tie_leader_of(h) >= 0) {
			continue;
		}
		if (an->hunter >= 0) {
			if (h != an->hunter) {
				continue;
			}
		} else if (!es_is_tie(h) || (fighters && SDL_strncasecmp(sc->ships[h].opt, "TieFighter", 10)) ||
				   (fighters > 1 && sc->ships[h].yaw <= 0.0f)) {
			/* The TIE fighter on the right (any TIE when there is none). */
			continue;
		}
		for (int hj = 0; hj < TOP; ++hj) {
			const int hc = top[h][hj];
			env_tie_candidate(h, hc);
			for (int xj = 0; xj < TOP; ++xj) {
				const int xc = top[victim][xj];
				env_tie_candidate(victim, xc);
				float miss = 0.0f, range = 0.0f;
				for (int q = 0; q < 3; ++q) {
					float hp[3], hn[3], hu[3], xp[3], xn[3], xu[3], dir[3];
					env_tie_pose(h, k_tie_hits[q] - 0.15f, hp, hn, hu);
					env_tie_pose(victim, k_tie_hits[q], xp, xn, xu);
					for (int a = 0; a < 3; ++a) {
						dir[a] = xp[a] - hp[a];
					}
					range += bt_len(dir) / k;
					bt_norm(dir);
					miss += acosf(fminf(1.0f, fmaxf(-1.0f, bt_dot(hn, dir)))) * 180.0f / BT_PI;
				}
				miss /= 3.0f;
				range /= 3.0f;
				const float score = scores[h][hc] + scores[victim][xc] - 1.5f * miss -
									(range > 150.0f ? (range - 150.0f) * 0.3f : 0.0f);
				if (score > best) {
					best = score;
					best_h = h;
					best_hc = hc;
					best_xc = xc;
				}
			}
		}
		/* Back to its own best until it wins. */
		int own = 0;
		for (int c = 0; c < ENV_TIE_CANDIDATES; ++c) {
			own = scores[h][c] > scores[h][own] ? c : own;
		}
		env_tie_candidate(h, own);
	}
	{
		int own = 0;
		for (int c = 0; c < ENV_TIE_CANDIDATES; ++c) {
			own = scores[victim][c] > scores[victim][own] ? c : own;
		}
		env_tie_candidate(victim, own);
	}
	if (best_h >= 0) {
		s_env.anim_hunter = best_h;
		env_tie_candidate(best_h, best_hc);
		env_tie_candidate(victim, best_xc);
	}
}

/* The pose at t, flown backwards from the cover's: the last turn (a pull
 * toward the canopy, its centre above it), before it the earlier turn (a
 * pull banked b1 from level), before that the straight run in, level.
 * Rolls happen only where a turn begins: into the first turn, and from
 * its bank into the last turn's. */
/* ---- the jinking X-wing ----------------------------------------------------
 * It flies its chaser's path 50 m ahead of it and breaks off it in three
 * long, smooth banked turns, left, right, left (as the viewer sees it): a
 * sweep out to the left, back across, and straightening onto the path
 * again. Each turn rolls in smoothly and then holds its bank and turn rate,
 * like the A-wing's pull away, each a banked turn (the craft leans into the curve,
 * its lift toward the turn's centre, rolling over as the turn reverses),
 * then slides onto the great explosion's centre as it starts and is gone
 * in it. The chaser follows its weaving a moment late, smaller. Both keep
 * the nose along their path. */
#define ENV_WEAVE_FROM 1.6f
#define ENV_WEAVE_TO (ENV_TIE_BOOM_AT - 0.3f)
#define ENV_WEAVE_SWING_M 40.0f   /* how far out the sweep goes */
#define ENV_WEAVE_ROLL_S 0.6f     /* rolling from one turn into the next */
#define ENV_WEAVE_BANK_DEG 65.0f  /* the bank held through a turn */
#define ENV_WEAVE_SAMPLES 512

/* Smootherstep: its pull (second derivative) has no jumps either, so
 * the lean it gives never snaps. */
static float env_smoother(float x) {
	x = fminf(fmaxf(x, 0.0f), 1.0f);
	return x * x * x * (x * (6.0f * x - 15.0f) + 10.0f);
}

/* The weave's sideways swing at t, 0..1 (0 outside its window): flown
 * from its turn rate, +1 for the first quarter (the turn out), -1 for the
 * middle half (the long turn back), +1 for the last quarter (onto the path
 * again), each change a smooth roll. Even about the middle with no net
 * turn, so it ends on the path, heading along it. */
static float s_weave_acc_max; /* the swing's hardest pull, 1/s^2 */

static float env_weave(float t) {
	static float table[ENV_WEAVE_SAMPLES + 1];
	static int built;
	const float span = ENV_WEAVE_TO - ENV_WEAVE_FROM;
	if (!built) {
		const float q = 0.25f * span, w = ENV_WEAVE_ROLL_S, ds = span / (float)ENV_WEAVE_SAMPLES;
		float rate[ENV_WEAVE_SAMPLES + 1], pos = 0.0f, neg = 0.0f;
		for (int i = 0; i <= ENV_WEAVE_SAMPLES; ++i) {
			const float x = (float)i * ds;
			const float in = env_smoother(x / w) * env_smoother((span - x) / w);
			rate[i] = in * (1.0f - 2.0f * env_smoother((x - q) / w + 0.5f) +
							2.0f * env_smoother((x - 3.0f * q) / w + 0.5f));
			pos += fmaxf(rate[i], 0.0f);
			neg += fmaxf(-rate[i], 0.0f);
		}
		float heading = 0.0f, side = 0.0f, peak = 1e-6f;
		for (int i = 0; i <= ENV_WEAVE_SAMPLES; ++i) {
			table[i] = side;
			peak = fmaxf(peak, fabsf(side));
			heading += (rate[i] < 0.0f ? rate[i] * pos / neg : rate[i]) * ds;
			side += heading * ds;
		}
		for (int i = 0; i <= ENV_WEAVE_SAMPLES; ++i) {
			table[i] /= peak;
		}
		for (int i = 1; i < ENV_WEAVE_SAMPLES; ++i) {
			s_weave_acc_max =
				fmaxf(s_weave_acc_max, fabsf(table[i + 1] - 2.0f * table[i] + table[i - 1]) / (ds * ds));
		}
		built = 1;
	}
	if (t <= ENV_WEAVE_FROM || t >= ENV_WEAVE_TO) {
		return 0.0f;
	}
	const float f = (t - ENV_WEAVE_FROM) / span * (float)ENV_WEAVE_SAMPLES;
	const int i = (int)f;
	if (i >= ENV_WEAVE_SAMPLES) {
		return table[ENV_WEAVE_SAMPLES];
	}
	return table[i] + (table[i + 1] - table[i]) * (f - (float)i);
}

/* The offset (world units) from its base path at t: chased = 0 for the
 * runner, 1 for its chaser. */
static void env_tie_weave_offset(int r, int chased, float t, float out[3]) {
	const int chaser = env_anim()->runners[r].chaser;
	const float* blast = env_anim()->blast;
	const float lead = chased ? 0.0f : s_env.runner[r].lead;
	const float k = AERON_OPT_UNITS_PER_METER;
	float p[3], n[3], u[3], side[3];
	env_tie_flight_pose(chaser, t + lead, p, n, u);
	static const float z[3] = { 0.0f, 0.0f, 1.0f };
	env_cross(n, z, side); /* the heading's right, level */
	if (!bt_norm(side)) {
		env_cross(n, u, side);
		bt_norm(side);
	}
	/* The Interceptor follows its turns a moment late, cutting inside them. */
	const float swing = chased ? 0.55f * ENV_WEAVE_SWING_M * env_weave(t - 0.45f) : ENV_WEAVE_SWING_M * env_weave(t);
	for (int a = 0; a < 3; ++a) {
		out[a] = side[a] * swing * k;
	}
	if (!chased) {
		/* Onto the explosion's centre as it starts. */
		float b[3], at[3], an[3], au[3];
		env_dir(blast[0], blast[1], b);
		env_tie_flight_pose(chaser, ENV_TIE_BOOM_AT + lead, at, an, au);
		const float w = env_smoother((t - (ENV_TIE_BOOM_AT - 2.2f)) / 2.2f);
		for (int a = 0; a < 3; ++a) {
			b[a] = s_env.view_pos[a] + b[a] * blast[2] * k;
			out[a] += (b[a] - at[a]) * w;
		}
	}
}

static void env_tie_weave_pose(int r, int chased, float t, float pos[3], float n[3], float u[3]) {
	const int chaser = env_anim()->runners[r].chaser;
	const float lead = chased ? 0.0f : s_env.runner[r].lead;
	const float h = 0.02f;
	float b0[3], b1[3], b2[3], o0[3], o1[3], o2[3], bn[3], bu[3], tn[3], tu[3];
	env_tie_flight_pose(chaser, t + lead - h, b0, tn, tu);
	env_tie_flight_pose(chaser, t + lead, b1, bn, bu);
	env_tie_flight_pose(chaser, t + lead + h, b2, tn, tu);
	env_tie_weave_offset(r, chased, t - h, o0);
	env_tie_weave_offset(r, chased, t, o1);
	env_tie_weave_offset(r, chased, t + h, o2);
	float acc[3] = { 0.0f, 0.0f, 0.0f };
	for (int a = 0; a < 3; ++a) {
		pos[a] = b1[a] + o1[a];
		n[a] = (b2[a] + o2[a]) - (b0[a] + o0[a]);
	}
	/* The lean follows the turn's pull with a little lag (its pull over
	 * the last fifth of a second): a heavy craft rolls into a turn a moment
	 * behind its stick. */
	for (int q = 0; q < 4; ++q) {
		const float tq = t - 0.03f - 0.06f * (float)q;
		float p0[3], p1[3], p2[3];
		env_tie_weave_offset(r, chased, tq - h, p0);
		env_tie_weave_offset(r, chased, tq, p1);
		env_tie_weave_offset(r, chased, tq + h, p2);
		for (int a = 0; a < 3; ++a) {
			acc[a] += 0.25f * (p2[a] - 2.0f * p1[a] + p0[a]) / (h * h);
		}
	}
	if (!bt_norm(n)) {
		memcpy(n, bn, sizeof bn);
	}
	/* Lift toward the turn: the base attitude's up, banked toward the turn
	 * in proportion to its sideways pull, ENV_WEAVE_BANK_DEG at its
	 * hardest (held through each turn, rolled over smoothly between). */
	float side[3], up[3];
	static const float zup[3] = { 0.0f, 0.0f, 1.0f };
	env_cross(n, zup, side);
	if (!bt_norm(side)) {
		env_cross(n, bu, side);
		bt_norm(side);
	}
	(void)env_weave(0.0f); /* the table (and its hardest pull) built */
	const float a_ref = (chased ? 0.55f : 1.0f) * ENV_WEAVE_SWING_M * AERON_OPT_UNITS_PER_METER * s_weave_acc_max;
	const float pull = a_ref > 0.0f ? fmaxf(-1.0f, fminf(1.0f, bt_dot(acc, side) / a_ref)) : 0.0f;
	/* (Eased up, so the longer, gentler middle turn still leans well in.) */
	const float phi = copysignf(powf(fabsf(pull), 0.6f), pull) * ENV_WEAVE_BANK_DEG * BT_PI / 180.0f;
	const float along = bt_dot(bu, n);
	for (int a = 0; a < 3; ++a) {
		up[a] = bu[a] - n[a] * along;
	}
	if (!bt_norm(up)) {
		memcpy(up, zup, sizeof up);
	}
	float right[3];
	env_cross(n, up, right); /* the rotation of up about the nose toward it */
	for (int a = 0; a < 3; ++a) {
		u[a] = up[a] * cosf(phi) + right[a] * sinf(phi);
	}
	bt_norm(u);
}

/* The runner whose chaser `ship` is (weaving after it), or -1. */
static int env_tie_weave_chaser_of(int ship) {
	const EnvAnim* an = env_anim();
	for (int r = 0; r < an->runner_count && r < ENV_TIE_RUNNERS; ++r) {
		if (an->runners[r].weave && an->runners[r].chaser == ship && s_env.runner[r].ok) {
			return r;
		}
	}
	return -1;
}

/* A passer's pose: a straight line through its passing point, level, the
 * nose along it. */
static void env_tie_passer_pose(int ship, float t, float pos[3], float n[3], float u[3]) {
	const float k = AERON_OPT_UNITS_PER_METER;
	const float* at = env_anim()->pass[ship - env_anim()->first_passer];
	/* Heading toward and past the viewer, its x through 0 at the crossing. */
	float d[3] = { at[0] / ENV_TIE_PASS_CROSS_M, -1.0f, 0.0f };
	bt_norm(d);
	const float s = ENV_TIE_PASS_SPEED * (t - ENV_TIE_PASS_AT);
	for (int a = 0; a < 3; ++a) {
		pos[a] = ((a == 0 ? at[0] : a == 2 ? at[1] : 0.0f) + d[a] * s) * k;
		n[a] = d[a];
	}
	u[0] = 0.0f;
	u[1] = 0.0f;
	u[2] = 1.0f;
}

static int env_tie_is_passer(int ship) {
	const EnvAnim* an = env_anim();
	return an->passers > 0 && ship >= an->first_passer && ship < an->first_passer + an->passers;
}

/* A formation member's pose: its leader's, the formation (the offset and
 * the turn from the leader as on the cover) kept in the leader's frame. */
static void env_tie_follower_pose(int ship, int leader, float t, float pos[3], float n[3], float u[3]) {
	float lp[3], ln[3], lu[3], lr[3];
	float cp[3], cn[3], cr[3], cu[3], fp[3], fn[3], fr[3], fu[3];
	env_tie_pose(leader, t, lp, ln, lu);
	env_cross(ln, lu, lr);
	env_ship_pos(leader, cp);
	env_ship_basis(leader, cn, cr, cu);
	env_ship_pos(ship, fp);
	env_ship_basis(ship, fn, fr, fu);
	float off[3], ob[3], nb[3], ub[3];
	for (int a = 0; a < 3; ++a) {
		off[a] = fp[a] - cp[a];
	}
	/* In the leader's frame on the cover (right, nose, up). */
	ob[0] = bt_dot(off, cr);
	ob[1] = bt_dot(off, cn);
	ob[2] = bt_dot(off, cu);
	nb[0] = bt_dot(fn, cr);
	nb[1] = bt_dot(fn, cn);
	nb[2] = bt_dot(fn, cu);
	ub[0] = bt_dot(fu, cr);
	ub[1] = bt_dot(fu, cn);
	ub[2] = bt_dot(fu, cu);
	for (int a = 0; a < 3; ++a) {
		pos[a] = lp[a] + lr[a] * ob[0] + ln[a] * ob[1] + lu[a] * ob[2];
		n[a] = lr[a] * nb[0] + ln[a] * nb[1] + lu[a] * nb[2];
		u[a] = lr[a] * ub[0] + ln[a] * ub[1] + lu[a] * ub[2];
	}
}

static void env_tie_pose(int ship, float t, float pos[3], float n[3], float u[3]) {
	if (s_env.anim_paths && env_tie_is_passer(ship) && ship < env_space()->count && t < ENV_TIE_ANIM_SECONDS) {
		env_tie_passer_pose(ship, t, pos, n, u);
		return;
	}
	if (s_env.anim_paths && ship < env_anim()->animated && t < ENV_TIE_ANIM_SECONDS) {
		const int leader = env_tie_leader_of(ship);
		if (leader >= 0 && leader != ship) {
			env_tie_follower_pose(ship, leader, t, pos, n, u);
			return;
		}
		const int r = env_tie_runner_of(ship);
		if (r >= 0) {
			if (env_anim()->runners[r].weave) {
				float rr[3];
				env_ship_basis(ship, n, rr, u);
				env_ship_pos(ship, pos);
				if (s_env.runner[r].ok) {
					env_tie_weave_pose(r, 0, t, pos, n, u);
				}
			} else {
				env_tie_flight_pose(ship, t, pos, n, u);
				env_tie_runner_pose(r, t, pos, n, u);
			}
			return;
		}
		const int c = env_tie_weave_chaser_of(ship);
		if (c >= 0) {
			env_tie_weave_pose(c, 1, t, pos, n, u);
			return;
		}
	}
	env_tie_flight_pose(ship, t, pos, n, u);
}

static void env_tie_flight_pose(int ship, float t, float pos[3], float n[3], float u[3]) {
	float r[3];
	env_ship_basis(ship, n, r, u);
	env_ship_pos(ship, pos);
	if (!s_env.anim_paths || ship >= env_anim()->animated || t >= ENV_TIE_ANIM_SECONDS) {
		return;
	}
	if (env_tie_runner_of(ship) >= 0) {
		return; /* runners have no flight of their own */
	}
	const float T = ENV_TIE_ANIM_SECONDS;
	const float v = s_env.anim_fl[ship].v, w = s_env.anim_fl[ship].w;
	const float alpha = s_env.anim_fl[ship].alpha, beta = s_env.anim_fl[ship].beta;
	const float R = v / w;
	const float t2 = alpha / w, t1 = beta / w; /* seconds in each turn */
	const float tau = T - t;
	/* The junction of the turns (J) and the first turn's start (B). */
	float nj[3], uj[3], pj[3], u1[3], nb[3], ub[3], pb[3];
	for (int a = 0; a < 3; ++a) {
		nj[a] = n[a] * cosf(alpha) - u[a] * sinf(alpha);
		uj[a] = u[a] * cosf(alpha) + n[a] * sinf(alpha);
		pj[a] = pos[a] + u[a] * R - uj[a] * R;
	}
	env_tie_level(nj, uj, u1);
	bt_rotate(u1, nj, s_env.anim_fl[ship].b1);
	for (int a = 0; a < 3; ++a) {
		nb[a] = nj[a] * cosf(beta) - u1[a] * sinf(beta);
		ub[a] = u1[a] * cosf(beta) + nj[a] * sinf(beta);
		pb[a] = pj[a] + u1[a] * R - ub[a] * R;
	}
	float level_b[3];
	env_tie_level(nb, ub, level_b);
	const float bank_b = env_tie_roll(level_b, ub, nb); /* into the first turn */
	const float bank_j = env_tie_roll(u1, uj, nj);      /* first turn's bank into the last's */
	/* The roll into a turn starts with it (never on the straight); between
	 * the turns it spans the junction; each is done before the next. */
	float f_b = 1.0f, f_j;
	if (t1 > 0.0f) {
		const float entry = T - t2 - t1;
		const float rt_b = fmaxf(fminf(0.5f + 0.6f * fabsf(bank_b), 0.5f * t1), 1e-3f);
		f_b = env_smooth((t - entry) / rt_b);
		const float half = fmaxf(fminf(fminf(0.25f + 0.3f * fabsf(bank_j), 0.5f * t1), t2), 1e-3f);
		f_j = env_smooth((t - (T - t2 - half)) / (2.0f * half));
	} else {
		const float rt_j = fmaxf(fminf(0.5f + 0.6f * fabsf(bank_j), t2), 1e-3f);
		f_j = env_smooth((t - (T - t2)) / rt_j);
	}
	float geo[3], off;
	if (tau <= t2) {
		const float th = w * tau;
		float n0[3], r0[3], u0[3];
		env_ship_basis(ship, n0, r0, u0);
		for (int a = 0; a < 3; ++a) {
			n[a] = n0[a] * cosf(th) - u0[a] * sinf(th);
			geo[a] = u0[a] * cosf(th) + n0[a] * sinf(th);
			pos[a] = pos[a] + u0[a] * R - geo[a] * R;
		}
		off = -(1.0f - f_j) * bank_j;
	} else if (tau <= t2 + t1) {
		const float th = w * (tau - t2);
		for (int a = 0; a < 3; ++a) {
			n[a] = nj[a] * cosf(th) - u1[a] * sinf(th);
			geo[a] = u1[a] * cosf(th) + nj[a] * sinf(th);
			pos[a] = pj[a] + u1[a] * R - geo[a] * R;
		}
		off = -(1.0f - f_b) * bank_b + f_j * bank_j;
	} else {
		const float run = v * (tau - t2 - t1);
		for (int a = 0; a < 3; ++a) {
			n[a] = nb[a];
			geo[a] = level_b[a];
			pos[a] = pb[a] - nb[a] * run;
		}
		off = f_b * bank_b + f_j * bank_j;
	}
	memcpy(u, geo, sizeof geo);
	if (fabsf(off) > 1e-5f) {
		bt_rotate(u, n, off);
	}
}

static void env_tie_spawn_bolt(const float p[3], const float d[3], float life, int green) {
	for (int b = 0; b < ENV_TIE_BOLTS; ++b) {
		if (!s_env.anim_bolts[b].alive) {
			memcpy(s_env.anim_bolts[b].p, p, 3 * sizeof(float));
			memcpy(s_env.anim_bolts[b].d, d, 3 * sizeof(float));
			s_env.anim_bolts[b].life = life;
			s_env.anim_bolts[b].green = green;
			s_env.anim_bolts[b].alive = 1;
			return;
		}
	}
}

/* The hunter's pair of bolts at the X-wing as it will be at `at` (+ a miss
 * offset in metres), timed to arrive then; fired when its flight time
 * before `at` has come. */
static void env_tie_hunter_shot(float before, float t, float at, float miss_m) {
	const int h = s_env.anim_hunter;
	if (h < 0) {
		return;
	}
	const float k = AERON_OPT_UNITS_PER_METER;
	float hp[3], hn[3], hu[3], x[3], xn[3], xu[3];
	env_tie_pose(env_anim()->victim, at, x, xn, xu);
	env_tie_pose(h, at, hp, hn, hu);
	const float flight =
		sqrtf((x[0] - hp[0]) * (x[0] - hp[0]) + (x[1] - hp[1]) * (x[1] - hp[1]) + (x[2] - hp[2]) * (x[2] - hp[2])) / k /
		ENV_TIE_BOLT_SPEED;
	const float fire = at - flight;
	if (!(before < fire && t >= fire)) {
		return;
	}
	float pos[3], n[3], u[3], r[3];
	env_tie_pose(h, t, pos, n, u);
	env_cross(n, u, r);
	x[2] += miss_m * k;
	x[0] += 0.5f * miss_m * k;
	for (int g = -1; g <= 1; g += 2) {
		float p[3], d[3];
		for (int a = 0; a < 3; ++a) {
			p[a] = pos[a] + (r[a] * (float)g * 0.7f + n[a] * 2.0f) * k;
			d[a] = x[a] - p[a];
		}
		const float len = bt_len(d) / k;
		bt_norm(d);
		/* Gone as they strike (or fly on past for a miss). */
		env_tie_spawn_bolt(p, d, miss_m != 0.0f ? 1.2f : len / ENV_TIE_BOLT_SPEED, es_is_tie(h));
	}
	es_play(es_is_tie(h) ? ES_TIE_LASER : ES_REBEL_LASER, pos, 18.0f, 1.0f, 1.0f);
}

/* ---- the TIE art's sound ---------------------------------------------------
 * The flight's own effects from wave\SfxBlastNew.lst (its line n is effect
 * slot 4 + n, the WAVs in FE_High_Res or FE_Low_Res): the TIE and rebel
 * lasers, the TIE engine, the TIE fly-by and the explosions. Placed by hand
 * (gain over distance, pan to the ears of the head, doppler pitch) so the
 * flight's 3D listener is left alone. Exterior SFX volume for the effects,
 * engine volume for the engines; XWA_XR_SCENE_SFX_VOLUME scales both
 * (0 = silent). */
static const int k_es_slot[ES_COUNT] = { 20, 4, 97, 102, 36, 25, 26, 27, 112, 112 };
static const int k_es_alt_slot[ES_COUNT] = { 0, 0, 0, 101, 0, 0, 0, 0, 111, 111 };
/* Effects taken by file name first (the list's slot as the fallback). */
static const char* const k_es_file[ES_COUNT] = {
	[ES_TIE_FLYBY] = "FLYBYTI3D.WAV",
	[ES_XW_FLYBY] = "FLYBYCORT.WAV",
	[ES_AW_FLYBY] = "FLYBYAW.WAV",
};

static struct {
	int tried;
	AeronClip clip[ES_COUNT];
	float base[ES_COUNT]; /* the list's volume for the effect, 0..1 */
	AeronVoice engine[8];
	int flyby_done[8];
	float prev_dist[8];
	float right[3]; /* the head's right ear, scene axes */
	int has_head;
} s_es;

void XwaVrEnv_SetHeadOrientation(const float q[4]) {
	if (!q) {
		return;
	}
	/* (1,0,0) turned by the head (LOCAL: x right, y up, -z ahead), into the
	 * scene's axes (x right, y ahead, z up). */
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	const float lx = 1.0f - 2.0f * (y * y + z * z), ly = 2.0f * (x * y + w * z), lz = 2.0f * (x * z - w * y);
	s_es.right[0] = lx;
	s_es.right[1] = -lz;
	s_es.right[2] = ly;
	s_es.has_head = 1;
}

static AeronClip es_wav_clip(const uint8_t* d, size_t n) {
	if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) {
		return 0;
	}
	int fmt = 0, ch = 0, rate = 0, bits = 0;
	const uint8_t* pcm = NULL;
	size_t pcm_n = 0;
	for (size_t at = 12; at + 8 <= n;) {
		size_t len = (size_t)d[at + 4] | (size_t)d[at + 5] << 8 | (size_t)d[at + 6] << 16 | (size_t)d[at + 7] << 24;
		const uint8_t* body = d + at + 8;
		if (len > n - (at + 8)) {
			len = n - (at + 8);
		}
		if (!memcmp(d + at, "fmt ", 4) && len >= 16) {
			fmt = body[0] | body[1] << 8;
			ch = body[2] | body[3] << 8;
			rate = body[4] | body[5] << 8 | body[6] << 16 | body[7] << 24;
			bits = body[14] | body[15] << 8;
		} else if (!memcmp(d + at, "data", 4)) {
			pcm = body;
			pcm_n = len;
		}
		at += 8 + len + (len & 1);
	}
	if (fmt != 1 || ch < 1 || ch > 2 || rate <= 0 || (bits != 8 && bits != 16) || !pcm) {
		return 0;
	}
	const size_t frames = pcm_n / (size_t)(ch * bits / 8);
	return frames ? Aeron_AudioClipCreate(pcm, frames, rate, ch, bits == 8 ? AERON_PCM_U8 : AERON_PCM_S16) : 0;
}

static AeronClip es_load_file(AeronVfs* vfs, const char* path) {
	uint8_t* wav = NULL;
	size_t wav_size = 0;
	AeronClip clip = 0;
	if (AeronVfs_Exists(vfs, AERON_VFS_ROOT_ASSET, path) &&
		AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, path, 16u << 20, &wav, &wav_size)) {
		clip = es_wav_clip(wav, wav_size);
		free(wav);
	}
	return clip;
}

static void es_load(void) {
	s_es.tried = 1;
	AeronVfs* vfs = Aeron_GetVfs();
	static const char* const named_dirs[3] = { "wave\\FE_High_Res\\", "wave\\FE_Low_Res\\", "wave\\" };
	for (int i = 0; vfs && i < ES_COUNT; ++i) {
		for (int d = 0; k_es_file[i] && d < 3 && !s_es.clip[i]; ++d) {
			char path[160];
			snprintf(path, sizeof path, "%s%s", named_dirs[d], k_es_file[i]);
			s_es.clip[i] = es_load_file(vfs, path);
			s_es.base[i] = 1.0f;
		}
	}
	uint8_t* list = NULL;
	size_t size = 0;
	if (!vfs || !AeronVfs_ReadAll(vfs, AERON_VFS_ROOT_ASSET, "wave\\SfxBlastNew.lst", 1u << 20, &list, &size)) {
		Aeron_LogInfo("xwa.remaster", "VR environment: no sound (wave\\SfxBlastNew.lst)");
		return;
	}
	/* The list's lines: name [volume [distance]]. */
	char names[128][64];
	int volumes[128];
	int lines = 0;
	size_t at = 0;
	while (at < size && lines < 128) {
		size_t end = at;
		while (end < size && list[end] != '\n') {
			++end;
		}
		size_t e = at;
		while (e < end && list[e] != ' ' && list[e] != '\t' && list[e] != '\r') {
			++e;
		}
		const size_t len = e - at < 63 ? e - at : 63;
		memcpy(names[lines], list + at, len);
		names[lines][len] = '\0';
		volumes[lines] = -1;
		while (e < end && (list[e] == ' ' || list[e] == '\t')) {
			++e;
		}
		if (e < end && list[e] >= '0' && list[e] <= '9') {
			volumes[lines] = atoi((const char*)list + e);
		}
		++lines;
		at = end + 1;
	}
	free(list);
	static const char* const dirs[2] = { "wave\\FE_High_Res\\", "wave\\FE_Low_Res\\" };
	int loaded = 0;
	for (int i = 0; i < ES_COUNT; ++i) {
		if (s_es.clip[i]) {
			++loaded; /* taken by name */
			continue;
		}
		for (int alt = 0; alt < 2 && !s_es.clip[i]; ++alt) {
			const int line = (alt ? k_es_alt_slot[i] : k_es_slot[i]) - 4;
			if (line < 0 || line >= lines || !names[line][0]) {
				continue;
			}
			for (int d = 0; d < 2 && !s_es.clip[i]; ++d) {
				char path[160];
				snprintf(path, sizeof path, "%s%s", dirs[d], names[line]);
				s_es.clip[i] = es_load_file(vfs, path);
			}
			s_es.base[i] = volumes[line] > 0 ? fminf((float)volumes[line] / 127.0f, 1.0f) : 1.0f;
		}
		loaded += s_es.clip[i] != 0;
	}
	Aeron_LogInfo("xwa.remaster", "VR environment: %d of %d scene sounds", loaded, ES_COUNT);
}

static float es_master(int engine) {
	const float v = engine ? (g_gameConfig.sfxEngineEnabled ? g_gameConfig.sfxEngineVolume : 0)
						   : (g_gameConfig.sfxExteriorEnabled ? g_gameConfig.sfxExteriorVolume : 0);
	return fminf(fmaxf((float)v / 10.0f, 0.0f), 1.0f) * fmaxf(env_float("XWA_XR_SCENE_SFX_VOLUME", 1.0f), 0.0f);
}

/* Gain (falling off past ref_m metres) and pan for a sound at p (world
 * units, from the viewer). */
static void es_place(const float p[3], float ref_m, float* gain, float* pan) {
	const float k = AERON_OPT_UNITS_PER_METER;
	const float d = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
	*gain = fminf(1.0f, ref_m / fmaxf(d / k, 0.5f));
	static const float ear[3] = { 1.0f, 0.0f, 0.0f };
	const float* r = s_es.has_head ? s_es.right : ear;
	*pan = d > 0.0f ? 0.8f * (p[0] * r[0] + p[1] * r[1] + p[2] * r[2]) / d : 0.0f;
}

static void es_play(int s, const float p[3], float ref_m, float gain, float pitch) {
	if (!s_es.clip[s]) {
		return;
	}
	float g, pan;
	es_place(p, ref_m, &g, &pan);
	g *= gain * s_es.base[s] * es_master(0);
	if (g > 0.003f) {
		(void)Aeron_AudioVoicePlay(s_es.clip[s], fminf(g, 1.0f), pan, pitch, 0);
	}
}

static void env_es_stop(void) {
	for (int i = 0; i < 8; ++i) {
		if (s_es.engine[i]) {
			Aeron_AudioVoiceStop(s_es.engine[i]);
		}
	}
	memset(s_es.engine, 0, sizeof s_es.engine);
	memset(s_es.flyby_done, 0, sizeof s_es.flyby_done);
	memset(s_es.prev_dist, 0, sizeof s_es.prev_dist);
}

static int es_is_tie(int ship) {
	const char* opt = env_space()->ships[ship].opt;
	return opt && (opt[0] == 'T' || opt[0] == 't') && (opt[1] == 'I' || opt[1] == 'i');
}

/* The engines (fading out once the fight holds) and the fly-bys. */
static void es_update_craft(float t, float dt) {
	const EnvSpaceScene* sc = env_space();
	const float k = AERON_OPT_UNITS_PER_METER;
	const float T = ENV_TIE_ANIM_SECONDS;
	const float hum = t < T ? 1.0f : 1.0f - env_smooth((t - T) / 1.5f);
	for (int i = 0; i < sc->count && i < env_anim()->animated; ++i) {
		if (!es_is_tie(i)) {
			continue;
		}
		float pos[3], n[3], u[3];
		env_tie_pose(i, t, pos, n, u);
		const float d = sqrtf(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]) / k;
		const float closing = s_es.prev_dist[i] > 0.0f && dt > 0.0f ? (d - s_es.prev_dist[i]) / dt : 0.0f;
		s_es.prev_dist[i] = d;
		/* Doppler, a little exaggerated. */
		const float pitch = fminf(fmaxf(300.0f / (300.0f + closing), 0.8f), 1.25f);
		if (s_es.clip[ES_TIE_ENGINE]) {
			float g, pan;
			es_place(pos, 9.0f, &g, &pan);
			g *= 0.55f * hum * s_es.base[ES_TIE_ENGINE] * es_master(1);
			if (hum <= 0.0f) {
				if (s_es.engine[i]) {
					Aeron_AudioVoiceStop(s_es.engine[i]);
					s_es.engine[i] = 0;
				}
			} else if (!s_es.engine[i]) {
				s_es.engine[i] = Aeron_AudioVoicePlayFrom(s_es.clip[ES_TIE_ENGINE], fminf(g, 1.0f), pan, pitch, 1,
														  (size_t)i * 2477u);
			} else {
				Aeron_AudioVoiceSetGain(s_es.engine[i], fminf(g, 1.0f));
				Aeron_AudioVoiceSetPan(s_es.engine[i], pan);
				Aeron_AudioVoiceSetPitch(s_es.engine[i], pitch);
			}
		}
		/* The scream as one comes in close. */
		if (!s_es.flyby_done[i] && t < T && d < 32.0f && closing < -2.0f) {
			s_es.flyby_done[i] = 1;
			es_play(ES_TIE_FLYBY, pos, 25.0f, 1.0f, pitch);
		}
	}
}

/* Advances the clock (from the scene's first visible frame), places the
 * craft, fires the bolts. */
static void env_tie_animate(void) {
	const uint64_t now = SDL_GetPerformanceCounter();
	float dt = s_env.anim_counter
				   ? (float)((double)(now - s_env.anim_counter) / (double)SDL_GetPerformanceFrequency())
				   : 0.0f;
	s_env.anim_counter = now;
	dt = fminf(fmaxf(dt, 0.0f), 0.1f);
	if (!s_env.anim_paths) {
		env_tie_paths();
	}
	if (!s_es.tried) {
		es_load();
	}
	const float T = ENV_TIE_ANIM_SECONDS;
	const EnvAnim* an = env_anim();
	const int victim = an->victim;
	/* On the startup title, A during the fight jumps to the cover's moment. */
	{
		const int pressed = s_title.pressed;
		if (pressed && s_env.anim_t > 0.5f && s_env.anim_t < T) {
			if (s_env.anim_t < ENV_TIE_BOOM_AT) {
				/* Jumped past the blast: still heard. */
				float p[3];
				env_dir(an->blast[0], an->blast[1], p);
				for (int a = 0; a < 3; ++a) {
					p[a] *= an->blast[2] * AERON_OPT_UNITS_PER_METER;
				}
				es_play(ES_BOOM, p, 60.0f, 1.0f, 1.0f);
			}
			s_env.anim_t = T;
		}
	}
	const float before = s_env.anim_t;
	s_env.anim_t = fminf(s_env.anim_t + dt, T + 60.0f);
	const float t = s_env.anim_t;
	for (int i = 0; i < s_env.count; ++i) {
		float pos[3], n[3], u[3];
		env_tie_pose(s_env.ship_of[i], t, pos, n, u);
		env_pose_matrix(n, u, pos, s_env.transform[i]);
		/* The runner is gone by the cover's moment. */
		const int r = env_tie_runner_of(s_env.ship_of[i]);
		s_env.hidden[i] = r >= 0 && (t >= T || !s_env.runner[r].ok ||
									 (an->runners[r].weave && t >= ENV_TIE_BOOM_AT + 0.12f));
		/* The passers are gone once well past the viewer. */
		if (env_tie_is_passer(s_env.ship_of[i]) && t >= ENV_TIE_PASS_AT + 1.5f) {
			s_env.hidden[i] = 1;
		}
	}
	/* Their scream just before they pass. */
	if (before < ENV_TIE_PASS_AT - 0.35f && t >= ENV_TIE_PASS_AT - 0.35f) {
		for (int ship = an->first_passer; ship < an->first_passer + an->passers; ++ship) {
			if (ship < env_space()->count) {
				float pos[3], n[3], u[3];
				env_tie_pose(ship, ENV_TIE_PASS_AT, pos, n, u);
				es_play(ES_AW_FLYBY, pos, 30.0f, 1.1f, ship == an->first_passer ? 1.0f : 1.06f);
			}
		}
	}
	for (int r = 0; r < an->runner_count && r < ENV_TIE_RUNNERS; ++r) {
		if (s_env.runner[r].ok && !s_env.runner[r].flyby_done && t >= s_env.runner[r].tb + 0.25f && t < T) {
			float pos[3], n[3], u[3];
			env_tie_pose(an->runners[r].ship, t, pos, n, u);
			s_env.runner[r].flyby_done = 1;
			es_play(ES_XW_FLYBY, pos, 30.0f, 1.2f, 1.0f);
		}
	}
	/* Bolts: the cover's shooters, the big Interceptor and the X-wing fire
	 * along their noses in bursts until the cover's moment. */
	const float k = AERON_OPT_UNITS_PER_METER;
	for (int si = 0; si < an->shooter_count && t < T - ENV_TIE_SALVO_SECONDS - 0.1f; ++si) {
		const int ship = an->shooters[si];
		if ((ship == victim && t > ENV_TIE_BOOM_AT) ||
			(ship == s_env.anim_hunter && t > k_tie_misses[0] - 1.0f && t < ENV_TIE_BOOM_AT)) {
			continue; /* the hunter's shots at the X-wing are its own */
		}
		if (before < s_env.anim_next_shot[ship] && t >= s_env.anim_next_shot[ship]) {
			float pos[3], n[3], u[3], r[3];
			env_tie_pose(ship, t, pos, n, u);
			env_cross(n, u, r);
			const int green = es_is_tie(ship);
			const char* opt = env_space()->ships[ship].opt;
			const float spread = green                               ? 0.7f
								 : !SDL_strncasecmp(opt, "AWing", 5) ? 3.3f
								 : !SDL_strncasecmp(opt, "BWing", 5) ? 1.6f
																	 : 5.2f;
			es_play(green ? ES_TIE_LASER : ES_REBEL_LASER, pos, 18.0f, 0.8f, 0.96f + 0.08f * (float)(si & 1));
			for (int g = -1; g <= 1; g += 2) {
				float p[3];
				for (int a = 0; a < 3; ++a) {
					p[a] = pos[a] + (r[a] * (float)g * spread + n[a] * 2.0f) * k;
				}
				env_tie_spawn_bolt(p, n, 1.2f, green);
			}
			s_env.anim_next_shot[ship] = t + 0.42f + 0.06f * (float)si;
		}
	}
	/* The hunter: bursts of three at the X-wing, two bursts walking in
	 * (all misses), then three ending in the hits. */
	if (t < ENV_TIE_BOOM_AT) {
		for (int b = 0; b < 5; ++b) {
			const float at = b < 2 ? k_tie_misses[b] : k_tie_hits[b - 2];
			for (int sh = 0; sh < 3; ++sh) {
				const float side = (sh & 1) ? -1.0f : 1.0f;
				const float miss = b < 2 ? side * (7.0f - 1.5f * (float)sh) : sh < 2 ? side * 3.0f : 0.0f;
				env_tie_hunter_shot(before, t, at - 0.13f * (float)(2 - sh), miss);
			}
		}
	}
	for (int b = 0; b < ENV_TIE_BOLTS; ++b) {
		if (!s_env.anim_bolts[b].alive) {
			continue;
		}
		s_env.anim_bolts[b].life -= dt;
		for (int a = 0; a < 3; ++a) {
			s_env.anim_bolts[b].p[a] += s_env.anim_bolts[b].d[a] * ENV_TIE_BOLT_SPEED * k * dt;
		}
		s_env.anim_bolts[b].alive = s_env.anim_bolts[b].life > 0.0f && t < T;
	}
	/* The cover's salvos, the X-wing's hits, the blast. */
	const float k_m = AERON_OPT_UNITS_PER_METER;
	const float salvo_at = T - ENV_TIE_SALVO_SECONDS;
	if (before < salvo_at && t >= salvo_at) {
		const EnvSpaceScene* sc = env_space();
		for (int si = 0; si < sc->salvo_count; ++si) {
			float pos[3], n[3], u[3];
			env_tie_pose(sc->salvos[si].shooter, t, pos, n, u);
			const EnvGuns g = sc->salvos[si].guns;
			es_play(g == ENV_GUNS_TIE || g == ENV_GUNS_INTERCEPTOR ? ES_TIE_LASER : ES_REBEL_LASER, pos, 18.0f, 1.0f,
					1.0f);
		}
	}
	const float* hits = k_tie_hits;
	for (int h = 0; h < 3; ++h) {
		if (before < hits[h] && t >= hits[h]) {
			float pos[3], n[3], u[3];
			env_tie_pose(victim, t, pos, n, u);
			es_play(ES_HIT0 + h, pos, 30.0f, 0.8f, 1.0f);
		}
	}
	if (before < ENV_TIE_BOOM_AT && t >= ENV_TIE_BOOM_AT) {
		float p[3];
		env_dir(an->blast[0], an->blast[1], p);
		for (int a = 0; a < 3; ++a) {
			p[a] *= an->blast[2] * k_m;
		}
		es_play(ES_BOOM, p, 60.0f, 1.0f, 1.0f);
		es_play(ES_HIT2, p, 60.0f, 0.7f, 0.8f);
	}
	/* A TIE screaming past right at the cover's moment: the nearest. */
	if (before < T && t >= T) {
		int nearest = -1;
		float nd = 1e30f, np[3] = { 0.0f, 0.0f, 0.0f };
		for (int i = 0; i < env_space()->count && i < an->animated; ++i) {
			float p[3], n[3], u[3];
			if (!es_is_tie(i)) {
				continue;
			}
			env_tie_pose(i, T - 0.05f, p, n, u);
			const float d = bt_len(p);
			if (d < nd) {
				nd = d;
				nearest = i;
				memcpy(np, p, sizeof np);
			}
		}
		if (nearest >= 0) {
			es_play(ES_TIE_FLYBY, np, 25.0f, 1.2f, 1.0f);
		}
	}
	es_update_craft(t, dt);
}

static void env_tie_submit_bolts(AeronScene3D* scene) {
	const float k = AERON_OPT_UNITS_PER_METER;
	static const float red[4] = { 2.4f, 0.35f, 0.25f, 1.0f };
	static const float green[4] = { 0.4f, 2.4f, 0.45f, 1.0f };
	for (int b = 0; b < ENV_TIE_BOLTS; ++b) {
		if (!s_env.anim_bolts[b].alive) {
			continue;
		}
		float a0[3];
		for (int a = 0; a < 3; ++a) {
			a0[a] = s_env.anim_bolts[b].p[a] - s_env.anim_bolts[b].d[a] * 5.0f * k;
		}
		env_beam(scene, AERON_SCENE_BILLBOARD_STAGE_OVERLAY, a0, s_env.anim_bolts[b].p, 0.4f * k,
				 s_env.anim_bolts[b].green ? green : red);
	}
}

/* ---- the startup title ------------------------------------------------- */

/* The font of the title's prompt and the button hints (loaded once). */
static int env_font(AeronCommandBuffer* cmd) {
	if (s_title.font_state == 0 && cmd) {
		s_title.font_state = -1;
		if (AeronFontAtlas_LoadVfs(&s_title.font, cmd, Aeron_GetVfs(), AERON_VFS_ROOT_RESOURCE, "fonts/setup",
								   4u * 1024u * 1024u)) {
			s_title.font_state = 1;
		} else {
			Aeron_LogWarn("xwa.remaster", "VR environment: no font for the title's prompt and button hints");
		}
	}
	return s_title.font_state > 0;
}

/* A controller button's letter in a ring, centred at (cx, cy), `d` across;
 * the letter in the style's tint scaled by `bright`. */
static void env_draw_key_ring(AeronDrawList2D* list, AeronTextStyle* style, const char* key, float cx, float cy,
							  float d, float bright) {
	AeronDrawList2DRRect r;
	memset(&r, 0, sizeof r);
	r.dst_x = cx - 0.5f * d;
	r.dst_y = cy - 0.5f * d;
	r.dst_w = d;
	r.dst_h = d;
	r.radius_px = 0.5f * d;
	r.border_px = fmaxf(2.0f, 0.075f * d);
	r.soft_px = 1.5f;
	const float a = style->tint[3];
	for (int k = 0; k < 3; ++k) {
		r.border[k] = style->tint[k] * bright * a;
	}
	r.border[3] = a;
	AeronDrawList_AddRRect(list, &r);
	const float w = AeronText_MeasureWidth(style, key, -1);
	const float h = AeronText_LineHeight(style);
	AeronText_Draw(list, style, cx - 0.5f * w, cy - 0.5f * h, AERON_TEXT_LEFT, key, -1, NULL);
}

/* "Press A to Continue" into a texture of its own (once). */
static void env_title_text(AeronCommandBuffer* cmd) {
	if (s_title.text_rt || !cmd || !env_font(cmd)) {
		return;
	}
	s_title.text_w = 1024;
	s_title.text_h = 128;
	s_title.text_rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = s_title.text_w, .height = s_title.text_h, .format = AERON_TEXTURE_FORMAT_RGBA8_UNORM });
	s_title.text_list = AeronDrawList_Create(64);
	if (!s_title.text_rt || !s_title.text_list) {
		s_title.font_state = -1;
		return;
	}
	/* "Press (A) to Continue": the button letter in a ring, as on the
	 * button hints. */
	static const char* const before = "Press";
	static const char* const key = "A";
	static const char* const after = "to Continue";
	AeronTextStyle style;
	memset(&style, 0, sizeof style);
	style.font = &s_title.font;
	style.scale = 1.0f;
	const float tall = AeronText_LineHeight(&style);
	const float space = 0.32f * tall, ring = 1.05f * tall;
	const float wide = AeronText_MeasureWidth(&style, before, -1) + space + ring + space +
					   AeronText_MeasureWidth(&style, after, -1);
	/* As wide as the texture allows, the line no taller than 70 % of it. */
	style.scale = fminf(wide > 0.0f ? 0.92f * (float)s_title.text_w / wide : 1.0f,
						tall > 0.0f ? 0.7f * (float)s_title.text_h / tall : 1.0f);
	style.tint[0] = style.tint[1] = style.tint[2] = style.tint[3] = 1.0f;
	style.filter = AERON_BLIT2D_FILTER_LINEAR;
	AeronDrawList_Begin(s_title.text_list, s_title.text_rt, s_title.text_w, s_title.text_h, AERON_DRAWLIST2D_CLEAR,
						NULL);
	const float k = style.scale;
	const float y = 0.5f * ((float)s_title.text_h - tall * k);
	float x = 0.5f * ((float)s_title.text_w - wide * k);
	AeronText_Draw(s_title.text_list, &style, x, y, AERON_TEXT_LEFT, before, -1, NULL);
	x += (AeronText_MeasureWidth(&style, before, -1) + space * k);
	env_draw_key_ring(s_title.text_list, &style, key, x + 0.5f * ring * k, 0.5f * (float)s_title.text_h, ring * k,
					  1.0f);
	x += (ring + space) * k;
	AeronText_Draw(s_title.text_list, &style, x, y, AERON_TEXT_LEFT, after, -1, NULL);
	AeronDrawList_Render(s_title.text_list, cmd);
}

/* ---- the button hints under the screen ----------------------------------- */

/* One line in small type under the frame, shown with the screen and when
 * it is put away: each button's letter in a ring (tinted), what it does
 * beside it, the two side by side. */
static struct {
	AeronRenderTarget* rt;
	AeronDrawList2D* list;
	int w, h;
	int failed;
} s_hint;

static const char* const k_hint_lines[2][2] = {
	{ "X", "Toggle 2D screen" },
	{ "Y", "Change VR environment" },
};

static void env_hint_text(AeronCommandBuffer* cmd) {
	if (s_hint.rt || s_hint.failed || !cmd || !env_font(cmd)) {
		return;
	}
	s_hint.w = 2048;
	s_hint.h = 128;
	s_hint.rt = Aeron_CreateRenderTarget(
		&(AeronRenderTargetDesc) { .width = s_hint.w, .height = s_hint.h, .format = AERON_TEXTURE_FORMAT_RGBA8_UNORM });
	s_hint.list = AeronDrawList_Create(64);
	if (!s_hint.rt || !s_hint.list) {
		s_hint.failed = 1;
		return;
	}
	AeronTextStyle style;
	memset(&style, 0, sizeof style);
	style.font = &s_title.font;
	style.filter = AERON_BLIT2D_FILTER_LINEAR;
	style.scale = 1.0f;
	/* (X) action    (Y) action, centred; the line at most 62 % of the
	 * texture's height, the whole as wide as it allows. */
	const float line = AeronText_LineHeight(&style);
	const float ring = 1.15f * line, gap = 0.35f * line, between = 1.4f * line;
	float block = between;
	for (int i = 0; i < 2; ++i) {
		block += ring + gap + AeronText_MeasureWidth(&style, k_hint_lines[i][1], -1);
	}
	style.scale = fminf(block > 0.0f ? 0.94f * (float)s_hint.w / block : 1.0f,
						line > 0.0f ? 0.62f * (float)s_hint.h / (1.15f * line) : 1.0f);
	const float k = style.scale;
	const float x0 = 0.5f * ((float)s_hint.w - block * k);
	const float mid = 0.5f * (float)s_hint.h;
	AeronDrawList_Begin(s_hint.list, s_hint.rt, s_hint.w, s_hint.h, AERON_DRAWLIST2D_CLEAR, NULL);
	{
		/* A dark, soft-edged plate: readable over a bright explosion too. */
		AeronDrawList2DRRect plate;
		memset(&plate, 0, sizeof plate);
		const float pad = 0.5f * line * k;
		plate.dst_x = fmaxf(x0 - pad, 2.0f);
		plate.dst_w = fminf(block * k + 2.0f * pad, (float)s_hint.w - 4.0f);
		plate.dst_y = 4.0f;
		plate.dst_h = (float)s_hint.h - 8.0f;
		plate.radius_px = 0.5f * plate.dst_h;
		plate.soft_px = 10.0f;
		for (int c = 0; c < 3; ++c) {
			plate.fill_top[c] = plate.fill_bottom[c] = 0.0f;
		}
		plate.fill_top[3] = plate.fill_bottom[3] = 0.5f;
		AeronDrawList_AddRRect(s_hint.list, &plate);
	}
	float x = x0;
	for (int i = 0; i < 2; ++i) {
		/* The letter and its ring in the menu beam's blue, the action in white. */
		style.tint[0] = 0.55f;
		style.tint[1] = 0.8f;
		style.tint[2] = 1.0f;
		style.tint[3] = 1.0f;
		env_draw_key_ring(s_hint.list, &style, k_hint_lines[i][0], x + 0.5f * ring * k, mid, ring * k, 1.0f);
		x += (ring + gap) * k;
		style.tint[0] = style.tint[1] = style.tint[2] = 0.92f;
		AeronText_Draw(s_hint.list, &style, x, mid - 0.5f * line * k, AERON_TEXT_LEFT, k_hint_lines[i][1], -1, NULL);
		x += AeronText_MeasureWidth(&style, k_hint_lines[i][1], -1) + between * k;
	}
	AeronDrawList_Render(s_hint.list, cmd);
}

/* The loading screen's "Press Any Key to Continue"
 * (g_flightLoadingPrompt, flight_task.c): its own line under the button
 * hints, pulsing (the 2D screen shows it only off the headset). */
/* (xwa/frontend/flight_loading.c, a modern addition there) */
extern const char* g_flightLoadingPrompt;
extern int g_flightLoadingPromptLevel;

static struct {
	AeronRenderTarget* rt;
	AeronDrawList2D* list;
	int w, h;
	int failed;
	const char* text;
} s_prompt;

static void env_prompt_text(AeronCommandBuffer* cmd) {
	const char* text = g_flightLoadingPrompt;
	if (env_float("XWA_XR_PROMPT_UNDER_FRAME", 0.0f) == 0.0f) {
		return; /* drawn on the loading screen itself (flight_loading.c) */
	}
	if (!text || s_prompt.failed || !cmd || (s_prompt.rt && s_prompt.text == text) || !env_font(cmd)) {
		return;
	}
	if (!s_prompt.rt) {
		s_prompt.w = 2048;
		s_prompt.h = 160;
		s_prompt.rt = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
			.width = s_prompt.w, .height = s_prompt.h, .format = AERON_TEXTURE_FORMAT_RGBA8_UNORM });
		s_prompt.list = AeronDrawList_Create(16);
		if (!s_prompt.rt || !s_prompt.list) {
			s_prompt.failed = 1;
			return;
		}
	}
	s_prompt.text = text;
	AeronTextStyle style;
	memset(&style, 0, sizeof style);
	style.font = &s_title.font;
	style.filter = AERON_BLIT2D_FILTER_LINEAR;
	style.scale = 1.0f;
	const float line = AeronText_LineHeight(&style);
	const float width = AeronText_MeasureWidth(&style, text, -1);
	style.scale = fminf(width > 0.0f ? 0.86f * (float)s_prompt.w / width : 1.0f,
						line > 0.0f ? 0.62f * (float)s_prompt.h / line : 1.0f);
	const float k = style.scale;
	AeronDrawList_Begin(s_prompt.list, s_prompt.rt, s_prompt.w, s_prompt.h, AERON_DRAWLIST2D_CLEAR, NULL);
	{
		AeronDrawList2DRRect plate;
		memset(&plate, 0, sizeof plate);
		const float pad = 0.6f * line * k;
		plate.dst_w = fminf(width * k + 2.0f * pad, (float)s_prompt.w - 4.0f);
		plate.dst_x = 0.5f * ((float)s_prompt.w - plate.dst_w);
		plate.dst_y = 4.0f;
		plate.dst_h = (float)s_prompt.h - 8.0f;
		plate.radius_px = 0.5f * plate.dst_h;
		plate.soft_px = 10.0f;
		plate.fill_top[3] = plate.fill_bottom[3] = 0.6f;
		AeronDrawList_AddRRect(s_prompt.list, &plate);
	}
	style.tint[0] = 1.0f;
	style.tint[1] = 0.86f;
	style.tint[2] = 0.3f;
	style.tint[3] = 1.0f;
	AeronText_Draw(s_prompt.list, &style, 0.5f * (float)s_prompt.w, 0.5f * ((float)s_prompt.h - line * k),
				   AERON_TEXT_CENTER, text, -1, NULL);
	AeronDrawList_Render(s_prompt.list, cmd);
}

/* Lower edge of the frame's bezel (image rows; transparent below). */
#define ENV_FRAME_BEZEL_BOTTOM 1032.0f

/* The hints, centred under the frame (or the bare screen), in the frame's
 * plane. */
static void env_submit_hints(AeronScene3D* scene) {
	if (!s_hint.rt || env_title_active() || env_float("XWA_XR_BUTTON_HINTS", 1.0f) == 0.0f) {
		return;
	}
	EnvScreen sc;
	env_screen(&sc);
	const float f = sc.frame_scale;
	const float below = s_env.tex[ENV_TEX_FRAME]
							? (ENV_FRAME_BEZEL_BOTTOM - k_frame_opening[3]) * sc.pic_w /
								  (k_frame_opening[2] - k_frame_opening[0])
							: 0.0f;
	/* Letters about 7.5 cm tall at the screen's 3.2 m (about 1.3 degrees):
	 * the line is 62 % of the texture's height. */
	const float th = 0.14f * env_float("XWA_XR_BUTTON_HINT_SCALE", 1.0f) * sc.dist / 3.2f;
	const float tw = th * (float)s_hint.w / (float)s_hint.h;
	const float up = (sc.lift - 0.5f * sc.pic_h - below) * f - 0.03f - 0.5f * th;
	const float center[3] = { s_env.view_pos[0], s_env.view_pos[1] + (sc.frame_dist + 0.02f) * sc.k,
							  s_env.view_pos[2] + up * sc.k };
	const float ax[3] = { 0.5f * tw * sc.k, 0.0f, 0.0f };
	const float ay[3] = { 0.0f, 0.0f, 0.5f * th * sc.k };
	float c[4][3];
	env_rect(center, ax, ay, c);
	static const float tint[4] = { 1.0f, 1.0f, 1.0f, 0.85f };
	s_quad_chrome = 1;
	env_quad_tex(scene, Aeron_RenderTargetGetTexture(s_hint.rt), AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
				 AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
	/* The hangar prompt, one line further down, a little larger, pulsing
	 * (premultiplied: the tint scales the whole quad). */
	if (env_float("XWA_XR_PROMPT_UNDER_FRAME", 0.0f) != 0.0f && g_flightLoadingPrompt && s_prompt.rt &&
		s_prompt.text == g_flightLoadingPrompt) {
		const float ph = 1.25f * th;
		const float pw = ph * (float)s_prompt.w / (float)s_prompt.h;
		const float pcenter[3] = { center[0], center[1], center[2] - (0.5f * th + 0.02f + 0.5f * ph) * sc.k };
		const float pax[3] = { 0.5f * pw * sc.k, 0.0f, 0.0f };
		const float pay[3] = { 0.0f, 0.0f, 0.5f * ph * sc.k };
		float pc[4][3];
		env_rect(pcenter, pax, pay, pc);
		const int level = g_flightLoadingPromptLevel < 0 ? 0 : g_flightLoadingPromptLevel > 255 ? 255 : g_flightLoadingPromptLevel;
		const float a = (float)level / 255.0f;
		const float ptint[4] = { a, a, a, a };
		env_quad_tex(scene, Aeron_RenderTargetGetTexture(s_prompt.rt), AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
					 AERON_SCENE_BILLBOARD_BLEND_PMA, pc, ptint);
	}
	s_quad_chrome = 0;
}

/* Y: the next scene, for this run (the VR options' Menu Scene saves one).
 * XwaVrEnv_Prepare swaps it in. */
static void env_next_scene(void) {
	static const struct {
		EnvMode mode;
		const char* name;
	} order[] = {
		{ ENV_ENDOR, "endor" },   { ENV_BOXART, "xwing" },    { ENV_TIEART, "tiefighter" },
		{ ENV_XVTART, "xvt" },    { ENV_DOEART, "defender" }, { ENV_BOPART, "balance" },
		{ ENV_BATTLE, "battle" },
	};
	const int n = (int)(sizeof order / sizeof order[0]);
	int at = -1;
	for (int i = 0; i < n; ++i) {
		if (order[i].mode == s_env.mode) {
			at = i;
		}
	}
	const int next = (at + 1) % n;
	SDL_setenv_unsafe("XWA_XR_ENVIRONMENT", order[next].name, 1);
	Aeron_LogInfo("xwa.remaster", "VR environment: %s (Y)", order[next].name);
}

/* Once a frame (the scene up): the title's clock and its A. */
static void env_title_update(void) {
	const uint64_t now = SDL_GetPerformanceCounter();
	float dt = s_title.counter ? (float)((double)(now - s_title.counter) / (double)SDL_GetPerformanceFrequency()) : 0.0f;
	s_title.counter = now;
	dt = fminf(fmaxf(dt, 0.0f), 0.1f);
	s_title.pressed = 0;
	if (!env_title_active()) {
		/* X puts the screen away and brings it back (so do A and the
		 * trigger); Y swaps the scene. */
		/* (Not while the mapping screen waits for a button press.) */
		const int capture = XwaVrButtons_CaptureActive();
		if (Aeron_XrTakeMenuXPressed() && !capture) {
			s_dismiss.away = !s_dismiss.away;
		}
		if (Aeron_XrTakeMenuYPressed() && !capture && !MissionDebrief_ShowingFailure() &&
			!MissionDebrief_ShowingSuccess()) {
			env_next_scene(); /* (a debriefing keeps its scene) */
		}
		if (s_dismiss.away) {
			const AeronInputSnapshot* in = Aeron_InputSnapshot();
			if (Aeron_XrTakeAPressed() || (in && (in->key_pressed[AERON_KEY_RETURN] || in->key_pressed[AERON_KEY_SPACE]))) {
				s_dismiss.away = 0;
			}
			Keyboard_FlushCharBuffer();
			FrontendMouse_ClearClicks();
		}
		const float step = dt / ENV_DISMISS_SECONDS;
		s_dismiss.level = s_dismiss.away ? fmaxf(s_dismiss.level - step, 0.0f) : fminf(s_dismiss.level + step, 1.0f);
		/* The logo comes in as slowly as at the title, goes with the screen's
		 * return. */
		s_dismiss.logo = s_dismiss.away
							 ? fminf(s_dismiss.logo + dt / env_float("XWA_XR_TITLE_FADE_SECONDS", 1.2f), 1.0f)
							 : fmaxf(s_dismiss.logo - step, 0.0f);
		return;
	}
	const AeronInputSnapshot* in = Aeron_InputSnapshot();
	int pressed = Aeron_XrTakeAPressed() || (in && (in->key_pressed[AERON_KEY_RETURN] || in->key_pressed[AERON_KEY_SPACE]));
	/* The title waits for A only. */
	(void)Aeron_XrTakeMenuXPressed();
	(void)Aeron_XrTakeMenuYPressed();
	/* Nothing reaches the hidden menu meanwhile. */
	Keyboard_FlushCharBuffer();
	FrontendMouse_ClearClicks();
	if (env_title_shown()) {
		s_title.shown_t += dt;
		if (!s_title.go && pressed && s_title.shown_t >= 0.4f) {
			s_title.go = 1;
			pressed = 0;
		}
	}
	s_title.pressed = pressed;
	/* The TIE art's startup plays to a debriefing theme (ENV_MUSIC_TITLE);
	 * the menus get theirs back with the screen. */
	if (env_played(s_env.mode) && !s_title.go && s_title.music_saved < 0 &&
		g_gameConfig.datapadMusicEnabled && env_float("XWA_XR_TITLE_MUSIC", 1.0f) != 0.0f) {
		s_title.music_saved = musicState;
		Music_SetState(ENV_MUSIC_TITLE);
	}
	if (s_title.go) {
		s_title.go_t += dt;
		if (s_title.go_t >= ENV_TIE_REVEAL_SECONDS) {
			s_title.done = 1;
		}
	}
	if ((s_title.go || s_title.done) && s_title.music_saved >= 0) {
		if (s_title.music_saved > 0) {
			Music_SetState(s_title.music_saved);
		}
		s_title.music_saved = 0; /* given back, once */
	}
}

/* The logo large where the screen will be, the prompt under it, gently
 * pulsing; both fade out as the screen comes in. */
static void env_submit_title(AeronScene3D* scene) {
	if (!env_title_shown()) {
		return;
	}
	const float a = env_smooth(s_title.shown_t / env_float("XWA_XR_TITLE_FADE_SECONDS", 1.2f)) *
					(1.0f - env_screen_reveal());
	env_submit_logo_at(scene, a, 1);
	if (!s_title.text_rt || a <= 0.0f) {
		return;
	}
	EnvScreen sc;
	env_screen(&sc);
	const float f = sc.frame_scale;
	float logo_h = 0.0f;
	if (s_env.tex[ENV_TEX_LOGO] && s_env.tex_w[ENV_TEX_LOGO] > 0) {
		logo_h = 0.9f * sc.pic_w * f * env_float("XWA_XR_LOGO_SCALE", 1.0f) * (float)s_env.tex_h[ENV_TEX_LOGO] /
				 (float)s_env.tex_w[ENV_TEX_LOGO];
	}
	const float tw = 0.55f * sc.pic_w * f, th = tw * (float)s_title.text_h / (float)s_title.text_w;
	const float up = sc.lift * f - 0.5f * logo_h - 0.03f - 0.5f * th;
	const float center[3] = { s_env.view_pos[0], s_env.view_pos[1] + (sc.frame_dist + 0.02f) * sc.k,
							  s_env.view_pos[2] + up * sc.k };
	const float ax[3] = { 0.5f * tw * sc.k, 0.0f, 0.0f };
	const float ay[3] = { 0.0f, 0.0f, 0.5f * th * sc.k };
	float c[4][3];
	env_rect(center, ax, ay, c);
	const float pulse = a * (0.75f + 0.25f * (0.5f + 0.5f * sinf(2.0f * BT_PI * 0.6f * s_title.shown_t)));
	const float tint[4] = { 1.15f, 1.2f, 1.3f, pulse };
	env_quad_tex(scene, Aeron_RenderTargetGetTexture(s_title.text_rt), AERON_SCENE_BILLBOARD_STAGE_OVERLAY,
				 AERON_SCENE_BILLBOARD_BLEND_PMA, c, tint);
}

/* The screen put away (X): the logo large where it was, as at the title. */
static void env_submit_dismissed(AeronScene3D* scene) {
	if (env_title_active() || s_dismiss.logo <= 0.0f) {
		return;
	}
	env_submit_logo_at(scene, env_smooth(s_dismiss.logo) * (1.0f - env_screen_reveal()), 1);
}

void XwaVrEnv_Submit(AeronScene3D* scene) {
	if (s_env_bare) {
		if (s_env_swapped) {
			/* The next scene loading (Y): the screen's surroundings stay. */
			env_submit_logo_at(scene, env_screen_reveal(), 0);
			env_submit_dismissed(scene);
			env_submit_hints(scene);
		}
		return; /* the startup films: empty space */
	}
	env_title_update();
	if (s_env.mode == ENV_BATTLE) {
		/* The battle moves: its craft are placed again every frame. */
		bt_advance();
		s_env.count = 0;
		bt_submit(scene);
	} else if (env_played(s_env.mode)) {
		env_tie_animate();
	}
	for (int i = 0; i < s_env.count; ++i) {
		if (s_env.hidden[i]) {
			continue;
		}
		AeronSceneMeshInstance inst;
		memset(&inst, 0, sizeof inst);
		inst.mesh = s_env.meshes[s_env.mesh_of[i]].mesh;
		memcpy(inst.transform, s_env.transform[i], sizeof inst.transform);
		memcpy(inst.prev_transform, s_env.transform[i], sizeof inst.transform);
		inst.zero_velocity = 1;
		inst.no_local_lights = 1;
		inst.cull_mode = AERON_CULL_BACK;
		if ((s_env.mode == ENV_HANGAR && i == 0) || s_env.far[i]) {
			/* The enclosed hangar must not shadow its own ceiling light;
			 * distant capital ships stay out of the shadow volume. */
			inst.shadow_flags |= AERON_SCENE_INSTANCE_NO_CAST_SHADOW;
		}
		AeronScene_AddMeshInstance(scene, &inst);
	}
	if (s_env.mode == ENV_ENDOR) {
		env_submit_endor_sprites(scene);
	} else if (s_env.mode == ENV_BOXART) {
		env_submit_boxart_sprites(scene);
	} else if (s_env.mode == ENV_TIEART) {
		env_submit_tieart_sprites(scene);
	} else if (s_env.mode == ENV_XVTART) {
		env_submit_xvtart_sprites(scene);
	} else if (s_env.mode == ENV_DOEART) {
		env_submit_doeart_sprites(scene);
	} else if (s_env.mode == ENV_BOPART) {
		env_submit_bopart_sprites(scene);
	}
	env_submit_title(scene);
	env_submit_dismissed(scene);
	env_submit_hints(scene);
	env_submit_frame(scene);
}

void XwaVrEnv_Release(void) {
	env_es_stop();
	Aeron_XrSetInputBlocked(0);
	Aeron_XrSetScreenFade(1.0f);
	s_dismiss.away = 0;
	s_dismiss.level = 1.0f;
	s_dismiss.logo = 0.0f;
	s_env_swapped = 0;
	for (int i = 0; i < s_env.mesh_count; ++i) {
		if (s_env.meshes[i].mesh) {
			AeronScene_MeshDestroy(s_env.meshes[i].mesh);
		}
	}
	for (int i = 0; i < ENV_TEX_COUNT; ++i) {
		if (s_env.tex[i]) {
			Aeron_DestroyTexture(s_env.tex[i]);
		}
	}
	if (s_env.frame_list) {
		AeronDrawList_Destroy(s_env.frame_list);
	}
	memset(&s_env, 0, sizeof s_env);
	/* Cast again (the next mission may differ); keep the random stream. */
	const uint32_t rng = s_bt.rng;
	memset(&s_bt, 0, sizeof s_bt);
	s_bt.rng = rng;
}
