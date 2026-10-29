/* VR voice commands; see vr_voice.h. */

#include "vr_voice.h"

#include "aeron/aeron.h"
#include "voice_core.h"
#include "xwa_runtime/runtime/port.h"
#include "xwa_runtime/runtime/vr_buttons.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VV_RATE 16000
#define VV_MAX_SECONDS 7
#define VV_MAX_SAMPLES (VV_RATE * VV_MAX_SECONDS)
#define VV_DOUBLE_TAP_S 0.45f
#define VV_HOLD_MIN_S 0.25f
#define VV_NO_SPEECH_S 3.0f  /* double-tap: give up if nothing is said */
#define VV_END_SILENCE_S 0.7f /* double-tap: done after this much quiet */
#define VV_FRAME 320          /* 20 ms */

/* Model files shipped in the APK's assets/voice/, copied out on first use
 * (the recognizer wants file paths). */
static const char* const k_model_files[] = { "encoder.onnx", "decoder.onnx", "joiner.onnx", "tokens.txt" };

enum { VV_MODE_OFF, VV_MODE_DOUBLE_TAP, VV_MODE_HOLD };
enum { VV_IDLE, VV_LISTENING, VV_RECOGNIZING };
enum { VV_SETUP_NONE, VV_SETUP_PERMISSION, VV_SETUP_LOADING, VV_SETUP_READY, VV_SETUP_FAILED };

static struct {
	int setup;
	int permission; /* -1 asked, 0 denied, 1 granted */
	SDL_AudioStream* mic;
	SDL_Thread* worker;
	SDL_Semaphore* wake;
	SDL_Mutex* lock;
	int quit;
	int job_load, job_recognize;
	int load_result;
	char load_error[256];
	int result_ready;
	char heard[256];

	int state;
	int hand;   /* the hand that started listening */
	float* buf; /* captured audio */
	int count;
	float listen_s, quiet_s;
	int speech;
	float noise;
	int frame_fill;
	double frame_energy;

	int touch_prev[2];
	float since_down[2]; /* seconds since the thumb last touched down */
	int button_prev[2];  /* a button with "Voice command" held (vr_buttons.h) */
	int by_button;       /* listening was started by such a button */
	float held_s;
	uint64_t last_ns;
	int logged_no_sensor;
} s_vv;

static int vv_mode(void) {
	const char* v = SDL_getenv("XWA_XR_VOICE");
	if (!v || !v[0]) {
		return VV_MODE_DOUBLE_TAP;
	}
	if (v[0] == '0' || SDL_strcasecmp(v, "off") == 0) {
		return VV_MODE_OFF;
	}
	return SDL_strcasecmp(v, "hold") == 0 ? VV_MODE_HOLD : VV_MODE_DOUBLE_TAP;
}

/* ---- setup (worker thread) ---------------------------------------------- */

/* The model: a voice-en folder next to the game data (copied there like the
 * game files), else the copy out of the APK in internal storage. */
static const char* vv_external_model_dir(void) {
	static char dir[1024];
	dir[0] = '\0';
#if defined(SDL_PLATFORM_ANDROID)
	const char* external = SDL_GetAndroidExternalStoragePath();
	if (external && external[0]) {
		char probe[1100];
		SDL_PathInfo info;
		snprintf(dir, sizeof dir, "%s/voice-en", external);
		snprintf(probe, sizeof probe, "%s/encoder.onnx", dir);
		if (!SDL_GetPathInfo(probe, &info) || info.size == 0) {
			dir[0] = '\0';
		}
	}
#endif
	return dir;
}

static const char* vv_model_dir(void) {
	static char dir[1024];
#if defined(SDL_PLATFORM_ANDROID)
	const char* external = vv_external_model_dir();
	if (external[0]) {
		return external;
	}
	const char* internal = SDL_GetAndroidInternalStoragePath();
	snprintf(dir, sizeof dir, "%s/voice-en", internal ? internal : ".");
#else
	const char* env = SDL_getenv("XWA_VOICE_MODEL_DIR");
	const char* base = SDL_GetBasePath();
#if defined(_WIN32)
	/* PCVR: the voice folder next to OpenXWA.exe. */
	if (!(env && env[0]) && base) {
		snprintf(dir, sizeof dir, "%svoice", base);
		return dir;
	}
#endif
	(void)base;
	snprintf(dir, sizeof dir, "%s", env && env[0] ? env : "voice");
#endif
	return dir;
}

/* Copies assets/voice/<file> out of the APK unless an equal-sized copy is
 * already there. */
static int vv_extract(const char* dir, char* error, int error_size) {
#if defined(SDL_PLATFORM_ANDROID)
	SDL_CreateDirectory(dir);
	for (size_t i = 0; i < sizeof k_model_files / sizeof k_model_files[0]; ++i) {
		char src[128], dst[1100];
		snprintf(src, sizeof src, "voice/%s", k_model_files[i]);
		snprintf(dst, sizeof dst, "%s/%s", dir, k_model_files[i]);
		SDL_IOStream* in = SDL_IOFromFile(src, "rb");
		if (!in) {
			snprintf(error, (size_t)error_size,
					 "no speech model: put the voice-en folder next to the game data (or use an APK with %s)", src);
			return 0;
		}
		const Sint64 size = SDL_GetIOSize(in);
		SDL_PathInfo info;
		if (SDL_GetPathInfo(dst, &info) && (Sint64)info.size == size) {
			SDL_CloseIO(in);
			continue;
		}
		SDL_IOStream* out = SDL_IOFromFile(dst, "wb");
		if (!out) {
			SDL_CloseIO(in);
			snprintf(error, (size_t)error_size, "cannot write %s", dst);
			return 0;
		}
		static char chunk[1 << 16];
		size_t n;
		int ok = 1;
		while ((n = SDL_ReadIO(in, chunk, sizeof chunk)) > 0) {
			if (SDL_WriteIO(out, chunk, n) != n) {
				ok = 0;
				break;
			}
		}
		SDL_CloseIO(in);
		SDL_CloseIO(out);
		if (!ok) {
			SDL_RemovePath(dst);
			snprintf(error, (size_t)error_size, "cannot write %s (disk full?)", dst);
			return 0;
		}
	}
	return 1;
#else
	(void)dir;
	(void)error;
	(void)error_size;
	return 1;
#endif
}

static int SDLCALL vv_worker(void* unused) {
	(void)unused;
	for (;;) {
		SDL_WaitSemaphore(s_vv.wake);
		SDL_LockMutex(s_vv.lock);
		const int quit = s_vv.quit, load = s_vv.job_load, recognize = s_vv.job_recognize;
		s_vv.job_load = s_vv.job_recognize = 0;
		SDL_UnlockMutex(s_vv.lock);
		if (quit) {
			break;
		}
		if (load) {
			char error[256] = "";
			const char* dir = vv_model_dir();
			const uint64_t t0 = SDL_GetTicks();
			int ok = vv_external_model_dir()[0] ? 1 : vv_extract(dir, error, sizeof error);
			if (ok) {
#if defined(SDL_PLATFORM_ANDROID)
				const char* lib = "libsherpa-onnx-c-api.so";
#elif defined(_WIN32)
				const char* lib = SDL_getenv("XWA_VOICE_LIB") ? SDL_getenv("XWA_VOICE_LIB") : "sherpa-onnx-c-api.dll";
#else
				const char* lib = SDL_getenv("XWA_VOICE_LIB") ? SDL_getenv("XWA_VOICE_LIB") : "libsherpa-onnx-c-api.so";
#endif
				ok = XwaVoice_Load(lib, dir, 2, error, sizeof error);
			}
			if (ok) {
				Aeron_LogInfo("xwa.voice", "voice commands ready, model %s (%llu ms)", dir,
							  (unsigned long long)(SDL_GetTicks() - t0));
			}
			SDL_LockMutex(s_vv.lock);
			s_vv.load_result = ok ? 1 : -1;
			snprintf(s_vv.load_error, sizeof s_vv.load_error, "%s", error);
			SDL_UnlockMutex(s_vv.lock);
		}
		if (recognize) {
			char heard[256];
			const uint64_t t0 = SDL_GetTicks();
			XwaVoice_Recognize(s_vv.buf, s_vv.count, heard, sizeof heard);
			Aeron_LogInfo("xwa.voice", "heard \"%s\" (%.1f s of audio, %llu ms)", heard,
						  (double)s_vv.count / VV_RATE, (unsigned long long)(SDL_GetTicks() - t0));
			SDL_LockMutex(s_vv.lock);
			snprintf(s_vv.heard, sizeof s_vv.heard, "%s", heard);
			s_vv.result_ready = 1;
			SDL_UnlockMutex(s_vv.lock);
		}
	}
	return 0;
}

static void SDLCALL vv_permission(void* userdata, const char* permission, bool granted) {
	(void)userdata;
	(void)permission;
	s_vv.permission = granted ? 1 : 0;
}

/* Starts what is needed, a step per frame; 1 once usable. */
static int vv_setup(void) {
	switch (s_vv.setup) {
		case VV_SETUP_READY:
			return 1;
		case VV_SETUP_FAILED:
			return 0;
		case VV_SETUP_NONE:
#if defined(SDL_PLATFORM_ANDROID)
			s_vv.permission = -1;
			if (!SDL_RequestAndroidPermission("android.permission.RECORD_AUDIO", vv_permission, NULL)) {
				s_vv.permission = 0;
			}
#else
			s_vv.permission = 1;
#endif
			s_vv.setup = VV_SETUP_PERMISSION;
			return 0;
		case VV_SETUP_PERMISSION:
			if (s_vv.permission < 0) {
				return 0;
			}
			if (s_vv.permission == 0) {
				Aeron_LogWarn("xwa.voice", "no microphone permission: voice commands are off");
				XwaPort_ShowFlightNote("[Voice] no microphone permission");
				s_vv.setup = VV_SETUP_FAILED;
				return 0;
			}
			{
				const SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, VV_RATE };
				s_vv.mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL);
				if (!s_vv.mic) {
					Aeron_LogWarn("xwa.voice", "no microphone: %s", SDL_GetError());
					s_vv.setup = VV_SETUP_FAILED;
					return 0;
				}
				SDL_PauseAudioStreamDevice(s_vv.mic);
			}
			s_vv.buf = (float*)malloc(sizeof(float) * VV_MAX_SAMPLES);
			s_vv.lock = SDL_CreateMutex();
			s_vv.wake = SDL_CreateSemaphore(0);
			s_vv.worker = s_vv.buf && s_vv.lock && s_vv.wake ? SDL_CreateThread(vv_worker, "voice", NULL) : NULL;
			if (!s_vv.worker) {
				s_vv.setup = VV_SETUP_FAILED;
				return 0;
			}
			SDL_LockMutex(s_vv.lock);
			s_vv.job_load = 1;
			SDL_UnlockMutex(s_vv.lock);
			SDL_SignalSemaphore(s_vv.wake);
			s_vv.setup = VV_SETUP_LOADING;
			return 0;
		case VV_SETUP_LOADING: {
			SDL_LockMutex(s_vv.lock);
			const int result = s_vv.load_result;
			char error[256];
			snprintf(error, sizeof error, "%s", s_vv.load_error);
			SDL_UnlockMutex(s_vv.lock);
			if (result == 0) {
				return 0;
			}
			if (result < 0) {
				Aeron_LogWarn("xwa.voice", "voice commands are off: %s", error);
				s_vv.setup = VV_SETUP_FAILED;
				return 0;
			}
			s_vv.setup = VV_SETUP_READY;
			return 1;
		}
		default:
			return 0;
	}
}

/* ---- push-to-talk and capture (frame thread) ---------------------------- */

static void vv_start(int hand, int pulse) {
	s_vv.state = VV_LISTENING;
	s_vv.hand = hand;
	s_vv.count = 0;
	s_vv.listen_s = s_vv.quiet_s = 0.0f;
	s_vv.speech = 0;
	s_vv.noise = 0.004f;
	s_vv.frame_fill = 0;
	s_vv.frame_energy = 0.0;
	SDL_ClearAudioStream(s_vv.mic);
	SDL_ResumeAudioStreamDevice(s_vv.mic);
	if (pulse) {
		Aeron_XrHapticPulse(hand, 0.35f, 40);
	}
}

static void vv_stop(int recognize) {
	SDL_PauseAudioStreamDevice(s_vv.mic);
	if (recognize && s_vv.count > VV_RATE / 5) {
		s_vv.state = VV_RECOGNIZING;
		SDL_LockMutex(s_vv.lock);
		s_vv.job_recognize = 1;
		SDL_UnlockMutex(s_vv.lock);
		SDL_SignalSemaphore(s_vv.wake);
	} else {
		s_vv.state = VV_IDLE;
		if (recognize) {
			XwaPort_ShowFlightNote("[Voice] nothing heard");
			Aeron_XrHapticPulse(s_vv.hand, 0.15f, 120);
		}
	}
}

/* Pulls the microphone's samples; a simple loudness gate tells when the
 * speaker started and stopped (double-tap mode). */
static void vv_capture(float dt) {
	float tmp[2048];
	int got;
	while ((got = SDL_GetAudioStreamData(s_vv.mic, tmp, (int)sizeof tmp)) > 0) {
		const int n = got / (int)sizeof(float);
		for (int i = 0; i < n; ++i) {
			if (s_vv.count < VV_MAX_SAMPLES) {
				s_vv.buf[s_vv.count++] = tmp[i];
			}
			s_vv.frame_energy += (double)tmp[i] * tmp[i];
			if (++s_vv.frame_fill == VV_FRAME) {
				const float rms = (float)sqrt(s_vv.frame_energy / VV_FRAME);
				const float gate = fmaxf(0.012f, s_vv.noise * 3.0f);
				if (rms > gate) {
					s_vv.speech = 1;
					s_vv.quiet_s = 0.0f;
				} else {
					if (!s_vv.speech) {
						s_vv.noise = 0.9f * s_vv.noise + 0.1f * rms;
					}
					s_vv.quiet_s += (float)VV_FRAME / VV_RATE;
				}
				s_vv.frame_fill = 0;
				s_vv.frame_energy = 0.0;
			}
		}
	}
	s_vv.listen_s += dt;
}

static void vv_result(void) {
	SDL_LockMutex(s_vv.lock);
	const int ready = s_vv.result_ready;
	char heard[256];
	snprintf(heard, sizeof heard, "%s", s_vv.heard);
	s_vv.result_ready = 0;
	SDL_UnlockMutex(s_vv.lock);
	if (!ready) {
		return;
	}
	s_vv.state = VV_IDLE;
	int action = 0, arg = 0;
	const char* label = NULL;
	char note[80];
	if (XwaVoice_Match(heard, &action, &arg, &label)) {
		XwaPort_VrConsoleAction((XwaVrConsoleAction)action, arg);
		snprintf(note, sizeof note, "[Voice] %s", label);
		XwaPort_ShowFlightNote(note);
		Aeron_XrHapticPulse(s_vv.hand, 0.6f, 60);
	} else {
		if (heard[0]) {
			snprintf(note, sizeof note, "[Voice] not understood: \"%.48s\"", heard);
		} else {
			snprintf(note, sizeof note, "[Voice] nothing heard");
		}
		XwaPort_ShowFlightNote(note);
		Aeron_XrHapticPulse(s_vv.hand, 0.15f, 120);
	}
}

void XwaVrVoice_Update(void) {
	const uint64_t now_ns = SDL_GetTicksNS();
	float dt = s_vv.last_ns ? (float)((double)(now_ns - s_vv.last_ns) * 1e-9) : 0.0f;
	s_vv.last_ns = now_ns;
	dt = dt > 0.25f ? 0.25f : dt;
	const int mode = vv_mode();
	const int presenting = mode != VV_MODE_OFF && Aeron_XrIsPresenting();
	/* Ask for the microphone and load the model already in the menus, so
	 * the permission prompt does not interrupt a mission. */
	const int ready = presenting && vv_setup();
	/* In flight, and in the map (to close it by voice). */
	const int flying = presenting && (XwaPort_VrInFlight() || XwaPort_VrInMap());
	if (!flying) {
		if (s_vv.state == VV_LISTENING) {
			vv_stop(0);
		}
		if (s_vv.state == VV_RECOGNIZING) {
			vv_result(); /* finish quietly; the note shows if still flying */
		}
		s_vv.touch_prev[0] = s_vv.touch_prev[1] = 0;
		s_vv.button_prev[0] = s_vv.button_prev[1] = 1; /* a held button must be let go first */
		return;
	}
	if (!ready) {
		return;
	}
	if (s_vv.state == VV_RECOGNIZING) {
		vv_result();
		return;
	}

	int touch[2];
	int button[2];
	/* The thumb rests (Touch), and any button the mapping gave "Voice
	 * command" (the Steam Frame's left bumper by default): a press starts
	 * listening, in hold mode for as long as it is held. */
	const int buttons = XwaVrButtons_VoiceButtons();
	for (int h = 0; h < 2; ++h) {
		touch[h] = Aeron_XrThumbrestTouch(h);
		button[h] = buttons && XwaVrButtons_VoiceHeld(h);
		s_vv.since_down[h] += dt;
	}
	if (touch[0] < 0 && touch[1] < 0 && !buttons) {
		if (!s_vv.logged_no_sensor) {
			Aeron_LogWarn("xwa.voice", "no thumb rest touch sensor and no voice button: voice push-to-talk unavailable");
			s_vv.logged_no_sensor = 1;
		}
		return;
	}

	if (s_vv.state == VV_IDLE) {
		int started = 0;
		for (int h = 0; h < 2 && !started; ++h) {
			if (button[h] && !s_vv.button_prev[h]) {
				vv_start(h, 1);
				s_vv.by_button = 1;
				s_vv.held_s = 0.0f;
				started = 1;
			}
		}
		for (int h = 0; h < 2 && !started; ++h) {
			const int down = touch[h] == 1 && !s_vv.touch_prev[h];
			if (mode == VV_MODE_DOUBLE_TAP && down) {
				if (s_vv.since_down[h] < VV_DOUBLE_TAP_S) {
					vv_start(h, 1);
					s_vv.by_button = 0;
					s_vv.since_down[h] = 1e3f;
					break;
				}
				s_vv.since_down[h] = 0.0f;
			} else if (mode == VV_MODE_HOLD && down) {
				vv_start(h, 0); /* the thumb may just be resting: buzz once it stays */
				s_vv.by_button = 0;
				s_vv.held_s = 0.0f;
				break;
			}
		}
	} else if (s_vv.state == VV_LISTENING) {
		vv_capture(dt);
		const int held = s_vv.by_button ? button[s_vv.hand] : touch[s_vv.hand] == 1;
		if (mode == VV_MODE_HOLD) {
			if (!s_vv.by_button && s_vv.held_s < VV_HOLD_MIN_S && s_vv.held_s + dt >= VV_HOLD_MIN_S) {
				Aeron_XrHapticPulse(s_vv.hand, 0.35f, 40);
			}
			s_vv.held_s += dt;
			if (!held) {
				/* A brief touch is the thumb passing by, not talk. */
				vv_stop(s_vv.held_s >= VV_HOLD_MIN_S && s_vv.speech);
			} else if (s_vv.count >= VV_MAX_SAMPLES) {
				vv_stop(1);
			}
		} else {
			/* Done: a tap on a thumb rest (or the button again), a pause after
			 * speaking, or full. */
			int tapped = 0;
			for (int h = 0; h < 2; ++h) {
				tapped |= touch[h] == 1 && !s_vv.touch_prev[h];
				tapped |= button[h] && !s_vv.button_prev[h];
			}
			if (tapped || (s_vv.speech && s_vv.quiet_s >= VV_END_SILENCE_S) || s_vv.count >= VV_MAX_SAMPLES) {
				vv_stop(1);
			} else if (!s_vv.speech && s_vv.listen_s >= VV_NO_SPEECH_S) {
				vv_stop(1); /* "nothing heard" */
			}
		}
	}
	for (int h = 0; h < 2; ++h) {
		s_vv.touch_prev[h] = touch[h] == 1;
		s_vv.button_prev[h] = button[h];
	}
}

void XwaVrVoice_Shutdown(void) {
	if (s_vv.worker) {
		SDL_LockMutex(s_vv.lock);
		s_vv.quit = 1;
		SDL_UnlockMutex(s_vv.lock);
		SDL_SignalSemaphore(s_vv.wake);
		SDL_WaitThread(s_vv.worker, NULL);
	}
	if (s_vv.mic) {
		SDL_DestroyAudioStream(s_vv.mic);
	}
	XwaVoice_Unload();
	if (s_vv.wake) {
		SDL_DestroySemaphore(s_vv.wake);
	}
	if (s_vv.lock) {
		SDL_DestroyMutex(s_vv.lock);
	}
	free(s_vv.buf);
	memset(&s_vv, 0, sizeof s_vv);
}
