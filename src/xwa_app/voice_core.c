/* Voice commands: recognizer and matcher; see voice_core.h. */

#include "voice_core.h"
#include <SDL3/SDL_stdinc.h>

#include "third_party/sherpa-onnx/c-api.h"
#include "xwa_remaster/vr_console_protocol.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define XV_NO_DLOPEN 1
#else
#include <dlfcn.h>
#endif

/* ---- recognizer -------------------------------------------------------- */

/* A non-streaming model: push-to-talk hands it the whole utterance, which it
 * hears far better than a streaming model hears the start of a short
 * command. */
typedef const SherpaOnnxOfflineRecognizer* (*PfnCreateRecognizer)(const SherpaOnnxOfflineRecognizerConfig*);
typedef void (*PfnDestroyRecognizer)(const SherpaOnnxOfflineRecognizer*);
typedef const SherpaOnnxOfflineStream* (*PfnCreateStream)(const SherpaOnnxOfflineRecognizer*);
typedef void (*PfnDestroyStream)(const SherpaOnnxOfflineStream*);
typedef void (*PfnAcceptWaveform)(const SherpaOnnxOfflineStream*, int32_t, const float*, int32_t);
typedef void (*PfnDecode)(const SherpaOnnxOfflineRecognizer*, const SherpaOnnxOfflineStream*);
typedef const SherpaOnnxOfflineRecognizerResult* (*PfnGetResult)(const SherpaOnnxOfflineStream*);
typedef void (*PfnDestroyResult)(const SherpaOnnxOfflineRecognizerResult*);

static struct {
	void* lib;
	PfnCreateRecognizer create_recognizer;
	PfnDestroyRecognizer destroy_recognizer;
	PfnCreateStream create_stream;
	PfnDestroyStream destroy_stream;
	PfnAcceptWaveform accept;
	PfnDecode decode;
	PfnGetResult get_result;
	PfnDestroyResult destroy_result;
	const SherpaOnnxOfflineRecognizer* recognizer;
} s_xv;

int XwaVoice_Ready(void) { return s_xv.recognizer != NULL; }

int XwaVoice_Load(const char* lib, const char* model_dir, int threads, char* error, int error_size) {
	if (s_xv.recognizer) {
		return 1;
	}
#if defined(XV_NO_DLOPEN)
	(void)lib;
	(void)model_dir;
	(void)threads;
	snprintf(error, (size_t)error_size, "voice commands are not available on this platform");
	return 0;
#else
	if (!s_xv.lib) {
		s_xv.lib = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
		if (!s_xv.lib) {
			snprintf(error, (size_t)error_size, "cannot load %s: %s", lib, dlerror());
			return 0;
		}
	}
#define XV_SYM(field, name)                                                                                        \
	do {                                                                                                         \
		*(void**)(&s_xv.field) = dlsym(s_xv.lib, name);                                                          \
		if (!s_xv.field) {                                                                                       \
			snprintf(error, (size_t)error_size, "%s: missing %s", lib, name);                                    \
			return 0;                                                                                            \
		}                                                                                                        \
	} while (0)
	XV_SYM(create_recognizer, "SherpaOnnxCreateOfflineRecognizer");
	XV_SYM(destroy_recognizer, "SherpaOnnxDestroyOfflineRecognizer");
	XV_SYM(create_stream, "SherpaOnnxCreateOfflineStream");
	XV_SYM(destroy_stream, "SherpaOnnxDestroyOfflineStream");
	XV_SYM(accept, "SherpaOnnxAcceptWaveformOffline");
	XV_SYM(decode, "SherpaOnnxDecodeOfflineStream");
	XV_SYM(get_result, "SherpaOnnxGetOfflineStreamResult");
	XV_SYM(destroy_result, "SherpaOnnxDestroyOfflineRecognizerResult");
#undef XV_SYM

	char encoder[1024], decoder[1024], joiner[1024], tokens[1024];
	snprintf(encoder, sizeof encoder, "%s/encoder.onnx", model_dir);
	snprintf(decoder, sizeof decoder, "%s/decoder.onnx", model_dir);
	snprintf(joiner, sizeof joiner, "%s/joiner.onnx", model_dir);
	snprintf(tokens, sizeof tokens, "%s/tokens.txt", model_dir);
	SherpaOnnxOfflineRecognizerConfig config;
	memset(&config, 0, sizeof config);
	config.feat_config.sample_rate = 16000;
	config.feat_config.feature_dim = 80;
	config.model_config.transducer.encoder = encoder;
	config.model_config.transducer.decoder = decoder;
	config.model_config.transducer.joiner = joiner;
	config.model_config.tokens = tokens;
	config.model_config.num_threads = threads > 0 ? threads : 1;
	config.model_config.provider = "cpu";
	config.model_config.debug = SDL_getenv("XWA_VOICE_DEBUG") ? 1 : 0;
	config.decoding_method = "greedy_search";
	config.max_active_paths = 4;
	s_xv.recognizer = s_xv.create_recognizer(&config);
	if (!s_xv.recognizer) {
		snprintf(error, (size_t)error_size, "the speech model in %s did not load", model_dir);
		return 0;
	}
	return 1;
#endif
}

void XwaVoice_Recognize(const float* samples, int count, char* out, int size) {
	if (!out || size <= 0) {
		return;
	}
	out[0] = '\0';
	if (!s_xv.recognizer || !samples || count <= 0) {
		return;
	}
	const SherpaOnnxOfflineStream* stream = s_xv.create_stream(s_xv.recognizer);
	if (!stream) {
		return;
	}
	/* Silence either side, as the sherpa-onnx tools pad. */
	static const float pad[4800] = { 0 };
	s_xv.accept(stream, 16000, pad, (int32_t)(sizeof pad / sizeof pad[0]));
	s_xv.accept(stream, 16000, samples, count);
	s_xv.accept(stream, 16000, pad, (int32_t)(sizeof pad / sizeof pad[0]));
	s_xv.decode(s_xv.recognizer, stream);
	const SherpaOnnxOfflineRecognizerResult* r = s_xv.get_result(stream);
	if (r) {
		int n = 0;
		for (const char* c = r->text; c && *c && n + 1 < size; ++c) {
			out[n++] = (char)tolower((unsigned char)*c);
		}
		out[n] = '\0';
		s_xv.destroy_result(r);
	}
	s_xv.destroy_stream(stream);
}

void XwaVoice_Unload(void) {
	if (s_xv.recognizer) {
		s_xv.destroy_recognizer(s_xv.recognizer);
		s_xv.recognizer = NULL;
	}
	/* The library stays loaded (onnxruntime does not like being unloaded). */
}

/* ---- commands ---------------------------------------------------------- */

typedef struct XvCommand {
	int action;
	int arg;
	const char* label;      /* the confirmation */
	const char* phrases[8]; /* what can be said, lower case */
} XvCommand;

static const XvCommand k_commands[] = {
	/* Wingmen. */
	{ XWA_VRC_WING_ATTACK, 0, "Wingmen: attack my target",
	  { "attack my target", "go after my target", "take out my target", "engage my target", "attack target",
		"attack" } },
	{ XWA_VRC_WING_COVER, 0, "Wingmen: cover me",
	  { "cover me", "protect me", "watch my back", "cover my six", "help me", "defend me" } },
	{ XWA_VRC_WING_IGNORE, 0, "Wingmen: ignore my target",
	  { "ignore my target", "ignore target", "ignore", "leave my target", "leave it alone", "forget my target" } },
	{ XWA_VRC_WING_EVADE, 0, "Wingmen: evade",
	  { "evade", "evasive action", "evasive maneuvers", "break off", "break formation", "scatter" } },
	{ XWA_VRC_WING_WAIT, 0, "Wingmen: wait",
	  { "wait", "hold position", "stand by", "hold on", "stay here", "hold up" } },
	{ XWA_VRC_WING_GO_AHEAD, 0, "Wingmen: go ahead",
	  { "go ahead", "proceed", "carry on", "continue", "proceed with mission", "continue mission" } },
	{ XWA_VRC_WING_HOME, 0, "Wingmen: head home",
	  { "return to base", "go home", "head home", "head back", "back to base", "return home" } },
	{ XWA_VRC_WING_REPORT, 0, "Wingmen: report in",
	  { "report in", "status report", "report status", "sound off", "check in", "report" } },
	{ XWA_VRC_WING_RESUPPLY, 0, "Call for resupply",
	  { "call resupply", "request resupply", "resupply", "need supplies", "call for supplies", "reload" } },
	{ XWA_VRC_WING_REINFORCE, 0, "Call for reinforcements",
	  { "call reinforcements", "request reinforcements", "reinforcements", "need backup", "send backup",
		"call for backup" } },
	/* Turrets (gunner seats, auto-firing turrets). */
	{ XWA_VRC_GUNNER_ATTACK, 0, "Gunners: attack my target",
	  { "gunner attack my target", "turret attack my target", "gunner attack", "turret attack",
		"gunner engage my target", "turret engage", "gunner fire at my target", "turret open fire" } },
	/* Targeting. */
	{ XWA_VRC_TARGET_NEAREST_ENEMY, 0, "Target closest enemy",
	  { "target closest enemy", "closest enemy", "nearest enemy", "target nearest enemy", "nearest fighter",
		"enemy" } },
	{ XWA_VRC_TARGET_NEXT, 0, "Next target",
	  { "next target", "target next", "cycle target", "another target", "next one" } },
	{ XWA_VRC_TARGET_PREV, 0, "Previous target",
	  { "previous target", "last target", "target previous", "go back target" } },
	{ XWA_VRC_TARGET_ATTACKER, 0, "Target my attacker",
	  { "target my attacker", "my attacker", "who is attacking me", "who is shooting at me", "who is shooting me",
		"target attacker" } },
	{ XWA_VRC_TARGET_OBJECTIVE, 0, "Target objective",
	  { "target objective", "objective", "nearest objective", "mission objective", "mission target",
		"target mission critical objective", "mission critical", "target mission critical" } },
	{ XWA_VRC_TARGET_TARGETS_ATTACKER, 0, "Target my target's attacker",
	  { "who is attacking my target", "target my target attacker", "my target attacker", "attacker of my target",
		"who is shooting my target", "who is shooting at my target", "target attacker of my target" } },
	{ XWA_VRC_TARGET_INCOMING, 0, "Target incoming missile",
	  { "target incoming missile", "target incoming", "incoming missile", "incoming torpedo", "incoming warhead",
		"incoming", "target missile", "missile lock" } },
	{ XWA_VRC_TARGET_NEWEST, 0, "Target newest craft",
	  { "newest target", "newest craft", "new arrival", "newest", "new target", "latest arrival" } },
	{ XWA_VRC_TARGET_CLEAR, 0, "Target cleared",
	  { "clear target", "cancel target", "no target", "drop target", "deselect target", "clear lock" } },
	{ XWA_VRC_TARGET_NAV, 0, "Target nav buoy",
	  { "nav buoy", "target nav buoy", "nav point", "next nav buoy", "navigation buoy", "hyperspace buoy" } },
	{ XWA_VRC_SUBSYSTEM_NEXT, 0, "Next subsystem",
	  { "next subsystem", "target subsystem", "subsystem", "next component", "target component" } },
	/* Ship. */
	{ XWA_VRC_MATCH_SPEED, 0, "Match speed",
	  { "match speed", "match target speed", "match their speed", "match velocity", "match it" } },
	{ XWA_VRC_HYPERSPACE, 0, "Hyperspace",
	  { "hyperspace", "jump to hyperspace", "make the jump", "punch it", "engage hyperdrive", "light speed" } },
	{ XWA_VRC_COUNTERMEASURE, 0, "Countermeasures",
	  { "countermeasures", "flares", "chaff", "deploy countermeasures", "drop flares", "decoy" } },
	{ XWA_VRC_SFOILS, 0, "S-foils",
	  { "s foils", "toggle s foils", "lock s foils", "open s foils", "close s foils", "attack position" } },
	{ XWA_VRC_LASERS_TO_SHIELDS, 0, "Lasers to shields",
	  { "lasers to shields", "transfer lasers to shields", "boost shields", "recharge shields", "power to shields",
		"energy to shields" } },
	{ XWA_VRC_SHIELDS_TO_LASERS, 0, "Shields to lasers",
	  { "shields to lasers", "transfer shields to lasers", "boost lasers", "recharge lasers", "power to lasers",
		"energy to lasers" } },
	{ XWA_VRC_FIRE_LINK, 0, "Link weapons",
	  { "link weapons", "fire link", "link fire", "fire mode", "link lasers", "change fire mode", "dual fire" } },
	/* The beam weapon (tractor, jamming or decoy beam) is not in the weapon
	 * cycle: it has its own key (B). */
	{ XWA_VRC_BEAM_TOGGLE, 0, "Beam weapon",
	  { "beam weapon", "toggle beam", "beam on", "beam off", "tractor beam", "jamming beam", "decoy beam",
		"fire beam" } },
	{ XWA_VRC_CYCLE_WARHEAD, 0, "Change weapon",
	  { "change weapon", "next weapon", "switch weapon", "cycle warheads", "change warhead", "switch warhead" } },
	{ XWA_VRC_CONVERGENCE, 0, "Convergence", { "convergence", "laser convergence", "converge" } },
	{ XWA_VRC_MAP, 0, "Map", { "map", "open map", "show map", "close map", "tactical map", "map view" } },
	{ XWA_VRC_DOCK, 0, "Dock", { "dock", "begin docking", "start docking", "board", "land" } },
	{ XWA_VRC_PICKUP, 0, "Pick up", { "pick up", "pick it up", "grab it", "pick up cargo", "tow it" } },
	{ XWA_VRC_RELEASE, 0, "Release", { "release cargo", "release", "drop cargo", "drop it", "let it go" } },
	/* The game's "press Space" prompts (end the mission, confirm a request,
	 * a wingman's "attack my target"). */
	{ XWA_VRC_CONFIRM, 0, "Confirm", { "confirm", "accept", "affirmative", "acknowledge", "yes" } },
};

/* Words that settle which command it is: when one is heard, only commands
 * with that word in a phrase are considered ("gunner, attack my target" is
 * not the wingmen's "attack my target"). */
static const char* const k_marker_words[] = { "gunner", "turret", "incoming", "critical", "who", "attacker", "beam" };

/* Heard words that mean the same to us, and words that change nothing
 * ("please", "the", who is addressed). Applied to the phrases too. */
static const char* const k_same[][2] = {
	{ "closest", "nearest" },   { "near", "nearest" },      { "nearby", "nearest" },
	{ "hostile", "enemy" },     { "hostiles", "enemy" },    { "bandit", "enemy" },
	{ "bandits", "enemy" },     { "enemies", "enemy" },     { "foe", "enemy" },
	{ "targets", "target" },    { "attacking", "attack" },  { "attacks", "attack" },
	{ "engaging", "engage" },   { "laser", "lasers" },      { "cannons", "lasers" },
	{ "guns", "lasers" },       { "shield", "shields" },    { "countermeasure", "countermeasures" },
	{ "counter", "countermeasures" }, { "flare", "flares" }, { "decoys", "decoy" },
	{ "foil", "foils" },        { "previous", "previous" }, { "prior", "previous" },
	{ "latest", "newest" },     { "goal", "objective" },    { "objectives", "objective" },
	{ "docking", "dock" },      { "subsystems", "subsystem" }, { "component", "subsystem" },
	{ "components", "subsystem" }, { "part", "subsystem" }, { "reinforcement", "reinforcements" },
	{ "backups", "backup" },    { "supply", "supplies" },   { "evasive", "evasive" },
	{ "maneuver", "maneuvers" }, { "manoeuvres", "maneuvers" }, { "rtb", "return to base" },
	{ "shooting", "shooting" }, { "whos", "who is" },       { "who's", "who is" },
	{ "its", "it" },            { "speeds", "speed" },
	{ "gunners", "gunner" },    { "turrets", "turret" },    { "target's", "target" },
	{ "missiles", "missile" },  { "torpedoes", "torpedo" }, { "warheads", "warhead" },
	{ "beams", "beam" },        { "bean", "beam" },         { "beem", "beam" },
	{ "jammer", "jamming" },    { "jam", "jamming" },
	/* How the recognizer tends to spell "s-foils". */
	{ "sfoils", "foils" },      { "spoils", "foils" },      { "espoils", "foils" },
	{ "aspoils", "foils" },     { "foyles", "foils" },      { "foyle", "foils" },
	{ "foyls", "foils" },
	/* Nav buoys: "navigation", "navpoint", and what "buoy" is often heard as. */
	{ "navigation", "nav" },    { "navpoint", "nav point" }, { "navi", "nav" },
	{ "buoys", "buoy" },        { "boy", "buoy" },          { "boys", "buoy" },
	{ "bouy", "buoy" },         { "bui", "buoy" },          { "points", "point" },
	{ "beacon", "buoy" },       { "waypoint", "nav point" },
};

static const char* const k_filler[] = {
	"please", "now", "the", "a",      "an",     "uh",    "um",      "okay", "ok",  "hey", "computer", "all",
	"guys",   "you", "everyone", "team", "squad", "flight", "wing", "wingmen", "wingman", "wingmates", "group",
	"lead",   "and", "just",  "that",   "this",   "go",    "be",      "can",  "will", "would", "could", "for",
	"to",     "into", "s",    "shoot",  "deploy", "put",
};
#define XV_COMMANDS ((int)(sizeof k_commands / sizeof k_commands[0]))
#define XV_MAX_WORDS 12

int XwaVoice_CommandCount(void) { return XV_COMMANDS; }

const char* XwaVoice_CommandPhrase(int index) {
	return index >= 0 && index < XV_COMMANDS ? k_commands[index].phrases[0] : NULL;
}

/* Adds `word` (after the synonyms; fillers dropped) to `words`. */
static int xv_add_word(char words[XV_MAX_WORDS][24], int n, const char* word) {
	for (size_t i = 0; i < sizeof k_filler / sizeof k_filler[0]; ++i) {
		if (strcmp(word, k_filler[i]) == 0) {
			return n;
		}
	}
	for (size_t i = 0; i < sizeof k_same / sizeof k_same[0]; ++i) {
		if (strcmp(word, k_same[i][0]) == 0) {
			/* The replacement may be several words ("rtb"). */
			char copy[48];
			snprintf(copy, sizeof copy, "%s", k_same[i][1]);
			for (char* t = strtok(copy, " "); t && n < XV_MAX_WORDS; t = strtok(NULL, " ")) {
				snprintf(words[n++], 24, "%s", t);
			}
			return n;
		}
	}
	if (n < XV_MAX_WORDS) {
		snprintf(words[n++], 24, "%s", word);
	}
	return n;
}

/* Words of `text`: lower case letters, digits and apostrophes ("who's");
 * "s-foils" is "s foils". Synonyms are folded, fillers dropped. */
static int xv_words(const char* text, char words[XV_MAX_WORDS][24]) {
	int n = 0, len = 0;
	char word[24];
	for (const char* c = text;; ++c) {
		const int letter = *c && (isalnum((unsigned char)*c) || *c == '\'');
		if (letter && len < 23) {
			word[len++] = (char)tolower((unsigned char)*c);
		} else if (!letter && len > 0) {
			word[len] = '\0';
			len = 0;
			n = xv_add_word(words, n, word);
			if (n == XV_MAX_WORDS) {
				break;
			}
		}
		if (!*c) {
			break;
		}
	}
	return n;
}

/* 0 (same) .. 1 (nothing alike): edit distance over the longer length. */
static float xv_word_cost(const char* a, const char* b) {
	const int la = (int)strlen(a), lb = (int)strlen(b);
	int d[24][24];
	for (int i = 0; i <= la; ++i) {
		d[i][0] = i;
	}
	for (int j = 0; j <= lb; ++j) {
		d[0][j] = j;
	}
	for (int i = 1; i <= la; ++i) {
		for (int j = 1; j <= lb; ++j) {
			const int sub = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
			const int del = d[i - 1][j] + 1, ins = d[i][j - 1] + 1;
			d[i][j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
		}
	}
	const int m = la > lb ? la : lb;
	return m ? (float)d[la][lb] / (float)m : 0.0f;
}

/* Alignment cost of the heard words against a phrase: a word swapped for a
 * look-alike costs its spelling distance, a missing phrase word 1, an extra
 * heard word 0.6 (0.15 before the phrase, 0.25 after: "uh", "please").
 * Normalised by the phrase length. */
static float xv_phrase_cost(char heard[XV_MAX_WORDS][24], int nh, const char* phrase) {
	char p[XV_MAX_WORDS][24];
	const int np = xv_words(phrase, p);
	if (np == 0) {
		return 9.0f;
	}
	float d[XV_MAX_WORDS + 1][XV_MAX_WORDS + 1];
	for (int i = 0; i <= nh; ++i) {
		d[i][0] = 0.15f * (float)i; /* leading extra words ("uh", "so") */
	}
	for (int j = 1; j <= np; ++j) {
		d[0][j] = (float)j;
	}
	for (int i = 1; i <= nh; ++i) {
		for (int j = 1; j <= np; ++j) {
			const float c = xv_word_cost(heard[i - 1], p[j - 1]);
			const float sub = d[i - 1][j - 1] + (c < 0.5f ? c : 1.0f);
			const float extra = d[i - 1][j] + (j == np ? 0.25f : 0.6f);
			const float missing = d[i][j - 1] + 1.0f;
			float best = sub < extra ? sub : extra;
			best = best < missing ? best : missing;
			/* Two heard words for one phrase word ("sub system"). */
			if (i >= 2) {
				char joined[48];
				snprintf(joined, sizeof joined, "%s%s", heard[i - 2], heard[i - 1]);
				if (strlen(joined) < 24) {
					const float cj = xv_word_cost(joined, p[j - 1]);
					const float merged = d[i - 2][j - 1] + (cj < 0.5f ? cj : 1.0f) + 0.1f;
					best = best < merged ? best : merged;
				}
			}
			d[i][j] = best;
		}
	}
	return d[nh][np] / (float)np;
}

int XwaVoice_Match(const char* heard, int* action, int* arg, const char** label) {
	char words[XV_MAX_WORDS][24];
	const int nh = heard ? xv_words(heard, words) : 0;
	if (nh == 0) {
		return 0;
	}
	/* A marker word heard (or close to it): only commands that use it. */
	const char* marker = NULL;
	for (size_t m = 0; m < sizeof k_marker_words / sizeof k_marker_words[0] && !marker; ++m) {
		for (int w = 0; w < nh; ++w) {
			if (xv_word_cost(words[w], k_marker_words[m]) <= 0.2f) {
				marker = k_marker_words[m];
				break;
			}
		}
	}
	float cost_of[XV_COMMANDS];
	float best = 9.0f;
	int best_i = -1;
	for (int i = 0; i < XV_COMMANDS; ++i) {
		float cost = 9.0f;
		int has_marker = marker == NULL;
		for (int k = 0; k < 8 && k_commands[i].phrases[k]; ++k) {
			const float c = xv_phrase_cost(words, nh, k_commands[i].phrases[k]);
			cost = c < cost ? c : cost;
			if (marker && strstr(k_commands[i].phrases[k], marker)) {
				has_marker = 1;
			}
		}
		cost_of[i] = has_marker ? cost : 9.0f;
		if (cost_of[i] < best) {
			best = cost_of[i];
			best_i = i;
		}
	}
	/* The runner-up among the other actions. */
	float second = 9.0f;
	for (int i = 0; i < XV_COMMANDS; ++i) {
		if (best_i >= 0 && k_commands[i].action != k_commands[best_i].action && cost_of[i] < second) {
			second = cost_of[i];
		}
	}
	/* Close enough, and clearly closer than any other command. */
	if (best_i < 0 || best > 0.34f || second - best < 0.12f) {
		return 0;
	}
	if (action) {
		*action = k_commands[best_i].action;
	}
	if (arg) {
		*arg = k_commands[best_i].arg;
	}
	if (label) {
		*label = k_commands[best_i].label;
	}
	return 1;
}
