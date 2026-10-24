#include "aeron/main.h"
#include "aeron/aeron.h"
#include "crash_report.h"
#include "host_config.h"
#include "setup.h"
#include "setup_screen.h"
#include "vr_keyboard.h"
#include "vr_voice.h"
#include "window_icon.h"
#include "xr_tier.h"
#include "xwa/config/game_config.h"
#include "xwa_remaster/vr_cockpit_hud.h"
#include "xwa_remaster/vr_lap_console.h"
#include "xwa_remaster/vr_flight_stick.h"
#include "xwa_runtime/config/modern_vr_options_screen.h"
#include "xwa_remaster/xwa_remaster.h"
#include "xwa_runtime/config/modern_video_options.h"
#include "xwa_runtime/input/controller_mapping.h"
#include "xwa_runtime/input/mouse_flight.h"
#include "xwa_runtime/runtime/port.h"
#include "xwa_runtime/runtime/presentation.h"
#include "xwa_runtime/timing/modern_flight_timing.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

/* OS message boxes are invisible inside an immersive VR app, so the Quest
 * build (and desktop runs with XWA_INGAME_SETUP=1, for testing) shows setup
 * problems and fatal errors on an in-game screen instead. */
static int use_ingame_setup(void) {
#if defined(SDL_PLATFORM_ANDROID)
	return 1;
#else
	const char* value = getenv("XWA_INGAME_SETUP");
	return value && value[0] == '1';
#endif
}

static void ingame_fatal_error(const char* title, const char* message) {
	/* Once a fatal error was requested the frame loop is shutting down and
	 * cannot draw anymore; the log keeps the message. */
	if (!Aeron_FatalErrorRequested()) {
		(void)XwaSetupScreen_Run(title, message, NULL);
	}
}

enum {
	XWA_SETUP_CHOOSE_FOLDER = 1,
	XWA_SETUP_TRY_AGAIN = 2,
	XWA_SETUP_QUIT = 3,
};

static uint64_t presentation_interval_us(void) {
	double rate = Aeron_PresentationRate();
	if (!(rate >= 1.0 && rate <= 1000.0))
		rate = 60.0;
	return (uint64_t)(1000000.0 / rate + 0.5);
}

static void apply_modern_video_options(const XwaModernVideoOptions* options) {
	if (!options) {
		return;
	}
	Aeron_SetFullscreen(options->window_mode == XWA_MODERN_WINDOW_MODE_FULLSCREEN);
	XwaRemaster_SetVideoOptions(options);
}

static int persist_modern_video_options(const XwaModernVideoOptions* options, char* error,
										size_t error_size) {
	return XwaHostConfig_SaveVideoOptions(Aeron_GetVfs(), options, error, error_size);
}

/* Platform-conventional fullscreen toggle chord: Cmd+Ctrl+F on macOS,
 * Alt+Enter on Windows, Alt+Enter or F11 elsewhere. */
static int fullscreen_hotkey_pressed(const AeronInputSnapshot* in) {
	if (!in) {
		return 0;
	}
#if defined(__APPLE__)
	return (in->key_down[AERON_KEY_LGUI] || in->key_down[AERON_KEY_RGUI]) &&
		   (in->key_down[AERON_KEY_LCTRL] || in->key_down[AERON_KEY_RCTRL]) &&
		   in->key_pressed[AERON_KEY_A + ('f' - 'a')];
#else
	const int alt = in->key_down[AERON_KEY_LALT] || in->key_down[AERON_KEY_RALT];
	const int enter = in->key_pressed[AERON_KEY_RETURN] || in->key_pressed[AERON_KEY_KP_ENTER];
#if defined(_WIN32)
	return alt && enter;
#else
	return (alt && enter) || in->key_pressed[AERON_KEY_F11];
#endif
#endif
}

/* Window-mode changes route through the video options module so the apply
 * callback, the options screen, and config persistence stay in agreement. */
static void set_window_mode_option(XwaModernWindowMode mode) {
	XwaModernVideoOptions options;
	XwaModernVideoOptions_Get(&options);
	if (options.window_mode != mode) {
		options.window_mode = mode;
		XwaModernVideoOptions_Set(&options);
	}
}

static void apply_modern_input_options(const XwaModernInputOptions* options) {
	XwaMouseFlight_SetOptions(options);
	if (options) {
		XwaControllerMapping_SetOptions(&options->controller);
		Config_ApplyModernInputOptions(options);
	}
}

static int persist_modern_input_options(const XwaModernInputOptions* options, char* error,
										size_t error_size) {
	return XwaHostConfig_SaveInputOptions(Aeron_GetVfs(), options, error, error_size);
}

static int show_first_launch_prompt(int* cancelled, char* error, size_t error_size) {
	static const AeronMessageBoxButton buttons[] = {
		{ XWA_SETUP_CHOOSE_FOLDER, "Choose Game Folder", 1, 0 },
		{ XWA_SETUP_QUIT, "Quit", 0, 1 },
	};
	const AeronMessageBoxOptions options = {
		.kind = AERON_MESSAGE_BOX_INFORMATION,
		.title = "Set up Neitey's XW: Alliance VR",
		.message = "Neitey's XW: Alliance VR requires data files from an original X-Wing Alliance installation.\n\n"
				   "Select an installed game folder (for example a GOG or Steam install) containing:\n\n"
				   "    FLIGHTMODELS\n"
				   "    FRONTRES\n"
				   "    MISSIONS\n"
				   "    MOVIES\n"
				   "    RESDATA\n"
				   "    WAVE\n\n"
				   "The selected location will be validated and remembered.",
		.buttons = buttons,
		.button_count = sizeof buttons / sizeof buttons[0],
	};
	int selected_button = -1;

	if (!Aeron_ShowMessageBox(&options, &selected_button)) {
		snprintf(error, error_size, "Could not show the first-launch setup prompt.");
		return 0;
	}
	if (selected_button != XWA_SETUP_CHOOSE_FOLDER) {
		*cancelled = 1;
		return 0;
	}
	return 1;
}

static int select_game_data(AeronVfs* vfs, char* selected, size_t selected_capacity, int* cancelled,
							char* error, size_t error_size) {
	const AeronFolderDialogOptions options = {
		.title = "Select your original X-Wing Alliance game directory",
		.accept_label = "Choose folder",
		.cancel_label = "Quit",
	};

	*cancelled = 0;
	for (;;) {
		AeronFolderDialog* dialog = Aeron_ShowOpenFolderDialog(&options);
		AeronFolderDialogStatus status;
		char candidate[XWA_HOST_CONFIG_PATH_CAPACITY];
		char dialog_error[256];
		char validation_error[512];

		if (!dialog) {
			snprintf(error, error_size, "Could not open the folder picker.");
			return 0;
		}
		do {
			Aeron_BeginFrame();
			status = Aeron_PollFolderDialog(dialog, candidate, sizeof candidate, dialog_error,
											sizeof dialog_error);
			if (!Aeron_Present()) {
				snprintf(error, error_size, "Renderer failure while displaying the folder picker: %s",
						 Aeron_RenderLastError());
				Aeron_DestroyFolderDialog(dialog);
				return 0;
			}
			if (status == AERON_FOLDER_DIALOG_WAITING) {
				Aeron_WaitForNextFrame(16667);
			}
		} while (status == AERON_FOLDER_DIALOG_WAITING);
		Aeron_DestroyFolderDialog(dialog);

		if (Aeron_QuitRequested()) {
			*cancelled = 1;
			return 0;
		}
		if (status == AERON_FOLDER_DIALOG_CANCELLED) {
			*cancelled = 1;
			return 0;
		}
		if (status == AERON_FOLDER_DIALOG_ERROR) {
			snprintf(error, error_size, "Folder picker failed: %s",
					 dialog_error[0] ? dialog_error : "unknown platform error");
			return 0;
		}
		if (XwaSetup_ValidateGameData(vfs, candidate, selected, selected_capacity, validation_error,
									  sizeof validation_error)) {
			return 1;
		}
		{
			static const AeronMessageBoxButton buttons[] = {
				{ XWA_SETUP_CHOOSE_FOLDER, "Choose Another Folder", 1, 0 },
				{ XWA_SETUP_QUIT, "Quit", 0, 1 },
			};
			char message[1536];
			int selected_button = -1;
			const AeronMessageBoxOptions message_options = {
				.kind = AERON_MESSAGE_BOX_WARNING,
				.title = "Invalid X-Wing Alliance game directory",
				.message = message,
				.buttons = buttons,
				.button_count = sizeof buttons / sizeof buttons[0],
			};

			snprintf(message, sizeof message,
					 "The selected folder is not a complete X-Wing Alliance data directory.\n\n"
					 "Selected:\n%s\n\n"
					 "%s\n\n"
					 "Select an installed game folder containing FLIGHTMODELS, MISSIONS, and RESDATA.TXT.",
					 candidate, validation_error);
			if (!Aeron_ShowMessageBox(&message_options, &selected_button)) {
				snprintf(error, error_size, "Could not show the game-data validation result.");
				return 0;
			}
			if (selected_button != XWA_SETUP_CHOOSE_FOLDER) {
				*cancelled = 1;
				return 0;
			}
		}
	}
}

/* The PC launcher (tools/launcher) leaves the list of the files it copied
 * in the game folder: "<size>\t<relative/path>" lines after a "#" header.
 * Returns how many listed files are missing and names the first ones in
 * `list`. No list (copied by hand): 0. */
static int install_list_missing(const char* dir, char* list, size_t list_size) {
	char path[1024];
	char line[1200];
	int missing = 0;
	size_t used = 0;
	snprintf(path, sizeof path, "%s/openxwa-install-files.txt", dir);
	FILE* f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	if (list_size) {
		list[0] = '\0';
	}
	while (fgets(line, sizeof line, f)) {
		char* tab = strchr(line, '\t');
		if (line[0] == '#' || !tab) {
			continue;
		}
		char* rel = tab + 1;
		rel[strcspn(rel, "\r\n")] = '\0';
		if (!rel[0] || strstr(rel, "..")) {
			continue;
		}
		char file[1024];
		struct stat st;
		snprintf(file, sizeof file, "%s/%s", dir, rel);
		if (stat(file, &st) == 0) {
			continue;
		}
		if (missing < 12 && used + strlen(rel) + 8 < list_size) {
			used += (size_t)snprintf(list + used, list_size - used, "    %s\n", rel);
		}
		missing++;
	}
	fclose(f);
	if (missing > 12 && used + 32 < list_size) {
		snprintf(list + used, list_size - used, "    ... and %d more\n", missing - 12);
	}
	return missing;
}

static int resolve_game_data(const XwaLaunchOptions* launch, const XwaHostConfig* host_config, char* selected,
							 size_t selected_capacity, int* cancelled, char* error, size_t error_size) {
	AeronVfs* vfs = Aeron_GetVfs();

	*cancelled = 0;
	if (launch->game_data_path[0]) {
		int ok = XwaSetup_ValidateGameData(vfs, launch->game_data_path, selected, selected_capacity, error,
										   error_size);
		if (ok && use_ingame_setup()) {
			char list[1024];
			const int missing = install_list_missing(launch->game_data_path, list, sizeof list);
			if (missing > 0) {
				Aeron_LogWarn("xwa.config", "%d game files from the launcher's copy are missing", missing);
				snprintf(error, error_size,
						 "%d of the X-Wing Alliance files copied from your PC are missing on the headset:\n\n"
						 "%s\n"
						 "Connect the headset to the PC and run the launcher (OpenXWA-VR-Launcher.exe) again:\n"
						 "it copies only the missing files. Then choose Try Again.",
						 missing, list);
				return 0;
			}
		}
		if (!ok && use_ingame_setup() && access(launch->game_data_path, R_OK | X_OK) != 0) {
			/* (android_default_game_data could not move it aside) */
			snprintf(error, error_size,
					 "Neitey's XW: Alliance VR cannot read its game folder on the headset:\n%s\n\n"
					 "It was created over USB before the game had started once.\n"
					 "Run the launcher (OpenXWA-VR-Launcher.exe) on your PC again with the headset on:\n"
					 "it repairs the folder and copies the game files again. Then choose Try Again.",
					 launch->game_data_path);
			return 0;
		}
		if (!ok && use_ingame_setup()) {
			char detail[512];
			snprintf(detail, sizeof detail, "%s", error);
			/* The install guide, in the headset: the first thing a new player
			 * sees (packaging/quest/QUEST-INSTALL.txt step 3 says the same). */
			snprintf(error, error_size,
					 "Neitey's XW: Alliance VR needs your own X-Wing Alliance files (GOG or Steam, about 1 GB).\n"
					 "Copy everything inside your installed game folder into this folder on the headset:\n"
					 "%s\n\n"
					 "1. On the PC, find the game folder: the one with RESDATA.TXT, FLIGHTMODELS and MISSIONS,\n"
					 "    e.g. C:\\GOG Games\\Star Wars - X-Wing Alliance\n"
					 "2. Connect the headset to the PC with a USB-C cable and allow USB debugging in the headset.\n"
					 "3. Copy the files, either way:\n"
					 "    - adb, in PowerShell:  adb push \"<game folder>\\.\" %s\n"
					 "      (get adb with: winget install Google.PlatformTools; SideQuest and Meta Quest\n"
					 "      Developer Hub bring it too)\n"
					 "    - SideQuest: its file browser, into Android/data/org.openxwa.quest/files/XWA\n"
					 "    Windows Explorer usually can't see Android/data on the headset: use adb or SideQuest.\n"
					 "4. RESDATA.TXT must end up directly in .../files/XWA, not in a subfolder.\n"
					 "5. Choose Try Again below.\n\n"
					 "Missing now: %s",
					 launch->game_data_path, launch->game_data_path, detail);
		}
		return ok;
	}
	if (host_config->game_data_path[0]) {
		if (XwaSetup_ValidateGameData(vfs, host_config->game_data_path, selected, selected_capacity, error,
									  error_size)) {
			return 1;
		}
		Aeron_LogWarn("xwa.config", "saved game-data path is no longer valid: %s", error);
		for (;;) {
			static const AeronMessageBoxButton buttons[] = {
				{ XWA_SETUP_TRY_AGAIN, "Try Again", 1, 0 },
				{ XWA_SETUP_CHOOSE_FOLDER, "Choose Another Folder", 0, 0 },
				{ XWA_SETUP_QUIT, "Quit", 0, 1 },
			};
			char message[1536];
			int selected_button = -1;
			const AeronMessageBoxOptions options = {
				.kind = AERON_MESSAGE_BOX_WARNING,
				.title = "Original X-Wing Alliance game data not found",
				.message = message,
				.buttons = buttons,
				.button_count = sizeof buttons / sizeof buttons[0],
			};

			snprintf(message, sizeof message,
					 "The previously configured game-data folder is no longer available or "
					 "does not contain the required files.\n\n"
					 "Configured folder:\n%s\n\n"
					 "%s\n\n"
					 "The folder may have moved, or its drive may be disconnected.",
					 host_config->game_data_path, error);
			if (!Aeron_ShowMessageBox(&options, &selected_button)) {
				snprintf(error, error_size, "Could not show the saved game-data error.");
				return 0;
			}
			if (selected_button == XWA_SETUP_TRY_AGAIN) {
				if (XwaSetup_ValidateGameData(vfs, host_config->game_data_path, selected, selected_capacity,
											  error, error_size)) {
					return 1;
				}
				continue;
			}
			if (selected_button == XWA_SETUP_CHOOSE_FOLDER) {
				return select_game_data(vfs, selected, selected_capacity, cancelled, error, error_size);
			}
			*cancelled = 1;
			return 0;
		}
	}
	if (!show_first_launch_prompt(cancelled, error, error_size)) {
		return 0;
	}
	return select_game_data(vfs, selected, selected_capacity, cancelled, error, error_size);
}

#if defined(SDL_PLATFORM_ANDROID)
#include <SDL3/SDL_system.h>
#include <dirent.h>
#include <sys/stat.h>

/* Quest has no folder picker inside an immersive app. The game data lives in
 * the app's external files directory, which needs no storage permission and
 * is reachable with `adb push` or a PC file manager:
 *   /sdcard/Android/data/<package>/files/XWA
 * The folder is created on first launch so it shows up for copying. */
/* Quest settings file: KEY=VALUE lines applied as environment variables
 * before startup (the headset has no way to set them otherwise). */
/* Set when openxwa-quest.cfg did not exist yet (a fresh install): the
 * recommended VR settings are then written into it (main). */
static int s_android_fresh_settings;

static void android_load_settings(void) {
	const char* external = SDL_GetAndroidExternalStoragePath();
	char path[1024];
	char line[2048]; /* HUD layouts are long lines */
	FILE* f;
	if (!external || !external[0]) {
		return;
	}
	if (!SDL_getenv("AERON_LOG_FILE")) {
		/* The previous run's log stays as openxwa-prev.log (what happened
		 * before a restart is usually the interesting part). */
		char prev[1024];
		snprintf(path, sizeof path, "%s/openxwa.log", external);
		snprintf(prev, sizeof prev, "%s/openxwa-prev.log", external);
		remove(prev);
		rename(path, prev);
		SDL_setenv_unsafe("AERON_LOG_FILE", path, 1);
	}
	/* Native crashes: fault + backtrace, kept until the next crash. */
	snprintf(path, sizeof path, "%s/openxwa-crash.txt", external);
	XwaCrashReport_Install(path);
	snprintf(path, sizeof path, "%s/openxwa-quest.cfg", external);
	f = fopen(path, "r");
	if (!f) {
		f = fopen(path, "w");
		s_android_fresh_settings = f != NULL;
		if (f) {
			fputs("# Neitey's XW: Alliance VR settings. Remove the leading '#' to change a value.\n"
				  "# Headset class, picked from the GPU (low: Quest 2, Pico 4; mid: Quest 3,\n"
				  "# Pico 4 Ultra; high: Steam Frame); it sets the defaults of the eye\n"
				  "# pixel budget, refresh rate and foveation below. Force it with:\n"
				  "#XWA_XR_TIER=mid\n"
				  "# Per-eye render resolution (lower = faster):\n"
				  "#AERON_XR_RESOLUTION_SCALE=1.1\n"
				  "# Per-eye pixel budget: caps the resolution above (low 2300000,\n"
				  "# mid 3700000, high 6800000):\n"
				  "#AERON_XR_EYE_PIXELS=3700000\n"
				  "# Size cap of the three HUD images, as a fraction of the recommended eye\n"
				  "# size (a higher eye resolution then costs no extra HUD work):\n"
				  "#AERON_XR_HUD_RESOLUTION=0.9\n"
				  "# Display refresh rate in Hz (0 = the headset's setting; 72 gives the\n"
				  "# most time per frame):\n"
				  "#AERON_XR_REFRESH_RATE=0\n"
				  "# Fixed foveated rendering of the 3D view: 0 (off), low, medium, high.\n"
				  "# The edges of the view are shaded in coarser blocks; HUD, panels and text\n"
				  "# are not affected (restart to change):\n"
				  "#XWA_XR_FOVEATION=low\n"
				  "# Steam Frame: Valve's own foveation, which follows the eyes: off (ours\n"
				  "# above does the fixed foveation), fixed (Valve's, without eye\n"
				  "# tracking) or eye (Valve's, eye-tracked):\n"
				  "#XWA_XR_FRAME_FOVEATION=off\n"
				  "# GPU/CPU clock floor requested from the headset: 0, low, high, boost:\n"
				  "#AERON_XR_PERF_LEVEL=high\n"
				  "# 0 = turn off the GPU optimizations (A/B test; also in the VR options):\n"
				  "#XWA_XR_GPU_OPT=1\n"
				  "# 0 = ship textures uncompressed instead of ASTC (compressed once, kept\n"
				  "# in astc-cache):\n"
				  "#XWA_XR_ASTC=1\n"
				  "# Mesh shading: lite (cheaper, for the Quest) or full (the desktop shader):\n"
				  "#XWA_XR_SHADING=lite\n"
				  "# Anisotropic texture filtering cap (1 = trilinear only):\n"
				  "#XWA_XR_ANISOTROPY=2\n"
				  "# 1 = flight renders straight into the headset images, tonemapped in\n"
				  "# the shaders: no bloom, less GPU memory traffic:\n"
				  "#XWA_XR_DIRECT_OUTPUT=0\n"
				  "# Bloom: 1 = one for both eyes, eye = per eye, 0 = off:\n"
				  "#XWA_XR_BLOOM=1\n"
				  "# 0 = no GPU timing (the log lists each kind of render pass's ms per frame):\n"
				  "#XWA_XR_GPU_TIMING=1\n"
				  "# 0 = classic draw order (sky first) instead of near meshes first:\n"
				  "#XWA_XR_DRAW_ORDER=1\n"
				  "# 1 = dynamic lights from lasers/explosions (costs GPU time in combat):\n"
				  "#XWA_POINT_LIGHTS=0\n"
				  "# Directional shadows in the 3D cockpit (1 = on, costs GPU time):\n"
				  "#XWA_XR_SHADOWS=0\n"
				  "# 0 = render shadows again for the right eye (default: shared):\n"
				  "#XWA_XR_SHADOW_REUSE=1\n"
				  "# 0 = keep the game running while the headset is off / menu open:\n"
				  "#XWA_XR_AUTO_PAUSE=1\n"
				  "# 0 = everything on the flat virtual screen (no 3D cockpit):\n"
				  "#XWA_XR_STEREO=1\n"
				  "# Lower = cockpit and ships feel bigger (try 0.8):\n"
				  "#XWA_XR_WORLD_SCALE=1.0\n"
				  "# Stereo depth of the world outside the cockpit (1 = real, try 3-6;\n"
				  "# higher shows depth at combat range but makes ships look like models):\n"
				  "#XWA_XR_DEPTH_BOOST=1.0\n"
				  "# HUD: cockpit (panels on the cockpit), helmet (follows your head), fixed:\n"
				  "#XWA_XR_HUD_LAYOUT=cockpit\n"
				  "# Cockpit model: xwa (X-Wing Alliance's own) or revival (the optional\n"
				  "# X-Wing Revival cockpits, for the crafts that have one):\n"
				  "#XWA_XR_COCKPIT_MODEL=xwa\n"
				  "# Star dot size (1 = flat-screen stars magnified; default 0.33):\n"
				  "#XWA_XR_STAR_SIZE=0.33\n"
				  "# Lap console (touch panel below the view; look down to use it):\n"
				  "#XWA_XR_CONSOLE=1\n"
				  "#XWA_XR_CONSOLE_PITCH=-52\n"
				  "#XWA_XR_CONSOLE_DISTANCE=0.52\n"
				  "# Where you put it with the grip handles (Reset in the VR options):\n"
				  "#XWA_XR_CONSOLE_POSE=\n"
				  "# Radial menu: hold the left grip, point, pull the left trigger (0 = off):\n"
				  "#XWA_XR_WHEEL=1\n"
				  "# The game runs at 45 % speed while the wheel is open (0 = off):\n"
				  "#XWA_XR_WHEEL_SLOWMO=1\n"
				  "# Side panel on your right (tactical map, voice commands, button\n"
				  "# mapping; 0 = off) and the tab it shows:\n"
				  "#XWA_XR_SIDE_PANEL=1\n"
				  "#XWA_XR_PANEL_TAB=map\n"
				  "# HUD panels you move with the grip are kept per cockpit as\n"
				  "# XWA_XR_HUD_POSE_<COCKPIT>=... (Reset in the VR options).\n"
				  "# 1 = screen-space ambient occlusion in VR (darkens around the hands):\n"
				  "#XWA_XR_SSAO=0\n"
				  "# Your hands: gloves, controllers (with labels of what the buttons do\n"
				  "# when you look at them) or off:\n"
				  "#XWA_XR_HANDS=controllers\n"
				  "#XWA_XR_CONTROLLER_LABELS=1\n"
				  "# The headset's own controller model where it has one (generic = ours):\n"
				  "#XWA_XR_CONTROLLER_MODEL=headset\n"
				  "# Move the gloves in the controller's frame, metres x,y,z\n"
				  "# (+x right, +y up, +z back):\n"
				  "#XWA_XR_GLOVE_OFFSET=0,0,0\n"
				  "# Glove size (1 = the model's own large hand):\n"
				  "#XWA_XR_GLOVE_SCALE=0.85\n"
				  "# Menus, briefings and films float in a scene: tiefighter (TIE\n"
				  "# Fighter, the default), random (one of the six box arts or the\n"
				  "# battle, picked at each start), endor (XW: Alliance), xwing\n"
				  "# (X-Wing), xvt (X-Wing vs. TIE Fighter), defender (Defender of the\n"
				  "# Empire), balance (Balance of Power), battle (a live battle cast\n"
				  "# from your last mission), hangar (the carrier hangar) or 0 (off).\n"
				  "# Space scene brightness (1 = full) and logo size:\n"
				  "#XWA_XR_ENVIRONMENT=tiefighter\n"
				  "# In the menus X puts the screen away and brings it back, Y changes\n"
				  "# the scene (for this run). Their hints under the screen (0 = off):\n"
				  "#XWA_XR_BUTTON_HINTS=1\n"
				  "# 3D ship previews on the menu screen (mission setup, tech room):\n"
				  "# 0 = flat, 0.5 subtle, 1 normal, 1.6 strong:\n"
				  "#XWA_XR_SHIP_3D=0.5\n"
				  "#XWA_XR_ENDOR_BRIGHTNESS=0.8\n"
				  "#XWA_XR_LOGO_SCALE=1.0\n"
				  "# Hangar view: x,y (hangar units, ~41 per metre), turn in degrees, eye\n"
				  "# height in metres; parked craft OPT name (0 = none):\n"
				  "#XWA_XR_ENV_VIEW=-500,-1150,0,1.7\n"
				  "#XWA_XR_ENV_CRAFT=Xwing\n"
				  "# Hangar brightness behind the screen (1 = full, 0.4 = 60 % darker):\n"
				  "#XWA_XR_ENV_BRIGHTNESS=0.4\n"
				  "# Menu laser pointer opacity (1 = solid):\n"
				  "#XWA_XR_BEAM_OPACITY=0.5\n"
				  "# Flight stick between your knees (0 = off): squeeze the right grip on\n"
				  "# it to fly, the left grip on it to move it. Full tilt in degrees;\n"
				  "# 1 = twisting it rolls; where you put it (Reset in the VR options):\n"
				  "#XWA_XR_FLIGHT_STICK=1\n"
				  "#XWA_XR_STICK_ANGLE=20\n"
				  "#XWA_XR_STICK_TWIST=0\n"
				  "#XWA_XR_STICK_POS=\n"
				  "# Left thumbstick: degrees either side of straight forward/back that\n"
				  "# move the throttle; everywhere else rolls:\n"
				  "#AERON_XR_THROTTLE_ZONE=30\n"
				  "# Thumbstick response in flight (Stick Response): 1.0 = direct (linear),\n"
				  "# 1.5 medium, 2.0 = precise (default; small movements turn gently, full\n"
				  "# deflection still at full rate), 2.6 very precise, up to 4.0:\n"
				  "#XWA_XR_STICK_CURVE=2.0\n"
				  "# 1 = right thumbstick back pushes the nose down (inverted pitch):\n"
				  "#AERON_XR_INVERT_PITCH=0\n"
				  "# 1 = left-handed controls: the controllers trade roles (sticks,\n"
				  "# triggers, grips, A/B with X/Y); the flight stick stays on the right:\n"
				  "#AERON_XR_LEFT_HANDED=0\n"
				  "# 1 = the thumbsticks swap what they do in flight (left stick: pitch /\n"
				  "# yaw; right stick: roll, throttle); every button stays where it is:\n"
				  "#XWA_XR_SWAP_STICKS=0\n"
				  "# Voice commands: double-tap a thumb rest, then speak (doubletap), or\n"
				  "# speak while the thumb is on the rest (hold); 0 = off:\n"
				  "#XWA_XR_VOICE=doubletap\n"
				  "# Helmet HUD distance in metres and size:\n"
				  "#XWA_XR_HUD_DISTANCE=3.0\n"
				  "#XWA_XR_HUD_SCALE=0.65\n"
				  "# Cockpit size about your head: every cockpit, and one cockpit by its\n"
				  "# model name in capitals (Y-wing and A-wing default to 0.9):\n"
				  "#XWA_XR_COCKPIT_SCALE=1.0\n"
				  "#XWA_XR_COCKPIT_SCALE_XWINGCOCKPIT=1.0\n"
				  "# Screen distance in metres; the menu scene's viewer sits XWA_XR_ENV_BACK\n"
				  "# metres behind its reference point:\n"
				  "#AERON_XR_SCREEN_DISTANCE=3.2\n"
				  "#XWA_XR_ENV_BACK=1.1\n"
				  "#AERON_XR_SCREEN_WIDTH=2.1\n"
				  "# How far the menu picture sits back in its frame (metres):\n"
				  "#XWA_XR_FRAME_DEPTH=0.02\n"
				  "#AERON_XR_THROTTLE_RATE=0.8\n"
				  "# Skip the Quest render defaults (SSAO/MSAA/... follow the in-game options):\n"
				  "#XWA_QUEST_PROFILE=0\n",
				  f);
			fclose(f);
		}
		return;
	}
	while (fgets(line, sizeof line, f)) {
		char* eq;
		char* end;
		char* key = line;
		while (*key == ' ' || *key == '\t') {
			key++;
		}
		if (*key == '#' || *key == '\0' || *key == '\n' || *key == '\r') {
			continue;
		}
		eq = strchr(key, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		end = eq + 1 + strcspn(eq + 1, "\r\n");
		*end = '\0';
		SDL_setenv_unsafe(key, eq + 1, 1);
	}
	fclose(f);
	{
		const char* res = SDL_getenv("AERON_XR_RESOLUTION_SCALE");
		const char* fov = SDL_getenv("XWA_XR_FOVEATION");
		Aeron_LogInfo("xwa.config", "settings from %s: resolution %s, foveation %s", path, res ? res : "(default)",
					  fov ? fov : "(default)");
	}
}

/* Foveation shim (packaging/quest/vkshim): its log, its state, and the
 * marker file that is present while a start with it has not proven itself. */
static char g_shim_guard[1024];
static int (*g_shim_take_log)(char* out, int size);
static int (*g_shim_foveating)(void);
static void (*g_shim_frame_mark)(void);

static void shim_frame(void) {
	static int frames;
	if (g_shim_frame_mark) {
		g_shim_frame_mark(); /* its GPU timing reports per frame */
	}
	if (g_shim_take_log) {
		char text[4096];
		if (g_shim_take_log(text, (int)sizeof text) > 0) {
			for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
				if (line[0] == '!') {
					Aeron_LogWarn("vkshim", "%s", line + 1);
				} else {
					Aeron_LogInfo("vkshim", "%s", line + 1);
				}
			}
		}
	}
	/* Proven: the device and the first frames came up (a broken shim fails
	 * there; the menus need not use the foveated pass at all). */
	if (g_shim_guard[0] && ++frames > 300) {
		remove(g_shim_guard);
		g_shim_guard[0] = '\0';
	}
}

/* Steam Frame: the Android layer (Lepton) puts Valve's foveation layer
 * (VK_LAYER_VALVE_fdm_injection, XR_APILAYER_VALVE_fdm_injection) into every
 * app. It follows the eyes, and where the eye tracking restarts its sharp area
 * jumps from frame to frame: the picture jitters. XWA_XR_FRAME_FOVEATION:
 * off (default: the layer told to stay out; the game's own fixed foveation,
 * XWA_XR_FOVEATION, does the job), fixed (Valve's without the eye tracking),
 * eye (Valve's as it comes). The layer reads these variables in the game's
 * process; they are set before anything loads Vulkan or OpenXR. Variables
 * already set (from the Steam launch options) are kept. Elsewhere they mean
 * nothing. */
static void steam_frame_foveation(void) {
	const char* mode = SDL_getenv("XWA_XR_FRAME_FOVEATION");
	if (!mode || !mode[0]) {
		mode = "off";
	}
	if (SDL_strcasecmp(mode, "eye") == 0) {
		Aeron_LogInfo("xwa.config", "Steam Frame foveation: Valve's, eye-tracked");
		return;
	}
	setenv("FDM_DEBUG", "disable_offsets", 0); /* no gaze-driven offsets */
	if (SDL_strcasecmp(mode, "fixed") != 0) {
		setenv("FDM", "0", 0);
		setenv("DISABLE_VULKAN_FDM_INJECTION_LAYER", "1", 0);
		setenv("DISABLE_XR_APILAYER_VALVE_FDM_INJECTION", "1", 0);
		setenv("DISABLE_XR_APILAYER_VALVE_fdm_injection", "1", 0);
	}
	Aeron_LogInfo("xwa.config", "Steam Frame foveation: %s (FDM_DEBUG=%s, FDM=%s)",
				  SDL_strcasecmp(mode, "fixed") == 0 ? "Valve's, fixed (no eye tracking)"
													 : "Valve's off, the game's own fixed foveation",
				  getenv("FDM_DEBUG") ? getenv("FDM_DEBUG") : "", getenv("FDM") ? getenv("FDM") : "");
}

/* Read before Aeron_Init: the XR swapchains are sized at session start. */
static void android_xr_defaults(void) {
	const char* profile = SDL_getenv("XWA_QUEST_PROFILE");
	steam_frame_foveation();
	if (profile && profile[0] == '0') {
		return;
	}
	/* Per headset: eye pixel budget, refresh rate, foveation (before the
	 * foveation shim is set up below). */
	XwaXrTier_ApplyDefaults();
	if (!SDL_getenv("AERON_XR_RESOLUTION_SCALE")) {
		SDL_setenv_unsafe("AERON_XR_RESOLUTION_SCALE", "1.1", 1);
	}
	if (!SDL_getenv("AERON_XR_SCREEN_DISTANCE")) {
		/* 1.1 m further than the 2.1 m it was; the menu scene's viewer sits
		 * as much behind its reference point (XWA_XR_ENV_BACK), so the
		 * screen keeps its place in the scene. */
		SDL_setenv_unsafe("AERON_XR_SCREEN_DISTANCE", "3.2", 1);
	}
	if (!SDL_getenv("AERON_XR_SCREEN_PIXELS")) {
		/* A 4:3 virtual screen: the game's menus, briefings and films fill
		 * it (and its frame) edge to edge instead of being pillarboxed. */
		SDL_setenv_unsafe("AERON_XR_SCREEN_PIXELS", "1920x1440", 1);
	}
	{
		/* Fixed foveated rendering: SDL loads Vulkan through the OpenXWA shim
		 * (packaging/quest/vkshim), which adds a fragment density map to the
		 * 3D scene's pass. Only when the shim is there to load. */
		const char* fov = SDL_getenv("XWA_XR_FOVEATION");
		const char* external = SDL_GetAndroidExternalStoragePath();
		FILE* guard = NULL;
		if (external && external[0]) {
			snprintf(g_shim_guard, sizeof g_shim_guard, "%s/openxwa-foveation-start", external);
			guard = fopen(g_shim_guard, "r");
		}
		if (guard) {
			/* The last start with the shim never got going: run without it and
			 * leave it off (the VR options turn it back on). */
			SDL_setenv_unsafe("AERON_TEXTURE_ASTC", "0", 1);
			fclose(guard);
			remove(g_shim_guard);
			g_shim_guard[0] = '\0';
			SDL_setenv_unsafe("XWA_XR_FOVEATION", "0", 1);
			XwaModernVrOptions_SaveValue("XWA_XR_FOVEATION", "0");
			Aeron_LogWarn("xwa.config", "foveation: the last start with it failed, so it is now off");
		} else if (!(fov && (fov[0] == '0' || SDL_strcasecmp(fov, "off") == 0)) ||
				   !(SDL_getenv("XWA_XR_GPU_TIMING") && SDL_getenv("XWA_XR_GPU_TIMING")[0] == '0')) {
			/* Also with foveation off: the shim then only times the GPU. */
			void* shim = dlopen("libopenxwa_vkshim.so", RTLD_NOW | RTLD_LOCAL);
			if (shim && dlsym(shim, "vkGetInstanceProcAddr")) {
				SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, "libopenxwa_vkshim.so");
				g_shim_take_log = (int (*)(char*, int))dlsym(shim, "OpenXWA_VkShim_TakeLog");
				g_shim_foveating = (int (*)(void))dlsym(shim, "OpenXWA_VkShim_Foveating");
				g_shim_frame_mark = (void (*)(void))dlsym(shim, "OpenXWA_VkShim_FrameMark");
				if (g_shim_guard[0] && (guard = fopen(g_shim_guard, "w")) != NULL) {
					fclose(guard);
				}
				Aeron_LogInfo("xwa.config", "Vulkan through the foveation shim (XWA_XR_FOVEATION=%s)",
							  fov && fov[0] ? fov : "low");
			} else {
				g_shim_guard[0] = '\0';
				Aeron_LogWarn("xwa.config", "foveation shim not found: %s", dlerror());
				SDL_setenv_unsafe("AERON_TEXTURE_ASTC", "0", 1); /* the shim enables the GPU feature */
			}
		} else {
			g_shim_guard[0] = '\0';
			SDL_setenv_unsafe("AERON_TEXTURE_ASTC", "0", 1);
		}
	}
	{
		/* GPU Optimizations off (VR options A/B switch): no clock floor either. */
		const char* opt = SDL_getenv("XWA_XR_GPU_OPT");
		if (opt && opt[0] == '0' && !SDL_getenv("AERON_XR_PERF_LEVEL")) {
			SDL_setenv_unsafe("AERON_XR_PERF_LEVEL", "0", 1);
		}
	}
}

/* After the GPU device exists: a runtime that created the Vulkan device with
 * the system loader went around the shim, so there is no foveation and the
 * ASTC feature was never enabled (textures stay uncompressed). */
static void android_shim_check(void) {
	int (*seen)(void) = NULL;
	void* shim;
	if (!g_shim_take_log) {
		return; /* not using the shim */
	}
	shim = dlopen("libopenxwa_vkshim.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
	if (shim) {
		*(void**)&seen = dlsym(shim, "OpenXWA_VkShim_DeviceSeen");
	}
	if (seen && !seen()) {
		SDL_setenv_unsafe("AERON_TEXTURE_ASTC", "0", 1);
		Aeron_LogWarn("xwa.config", "the OpenXR runtime created the Vulkan device without the shim: no foveation, "
									"no ASTC textures");
	}
}

static void android_default_game_data(XwaLaunchOptions* launch) {
	const char* external = SDL_GetAndroidExternalStoragePath();
	char path[XWA_HOST_CONFIG_PATH_CAPACITY];
	if (launch->game_data_path[0] || !external || !external[0]) {
		return;
	}
	snprintf(path, sizeof path, "%s/XWA", external);
	{
		/* A folder made over adb before the app ever ran belongs to the
		 * shell user and the app cannot read it (or anything copied into
		 * it). Move it aside and make our own: the PC launcher then sees
		 * the files missing and copies them again. */
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid != getuid() && access(path, R_OK | X_OK) != 0) {
			char aside[XWA_HOST_CONFIG_PATH_CAPACITY + 32];
			snprintf(aside, sizeof aside, "%s/XWA-unreadable-%ld", external, (long)time(NULL));
			if (rename(path, aside) == 0) {
				Aeron_LogWarn("xwa.config", "game-data folder %s belonged to uid %u, not to the app: moved to %s",
							  path, (unsigned)st.st_uid, aside);
			} else {
				Aeron_LogError("xwa.config", "game-data folder %s belongs to uid %u and cannot be read or moved (%s)",
							   path, (unsigned)st.st_uid, strerror(errno));
			}
		}
	}
	mkdir(path, 0775);
	snprintf(launch->game_data_path, sizeof launch->game_data_path, "%s", path);
	Aeron_LogInfo("xwa.config", "Android game-data folder: %s", path);
}

/* Pilots the PC launcher copied over (.plt files in files/pilot-import) move into
 * the app's own pilot folder. A pilot file already on the headset is kept
 * (its progress wins); the copy is removed either way. */
static void android_import_pilots(void) {
	const char* external = SDL_GetAndroidExternalStoragePath();
	char dir[1024];
	if (!external || !external[0]) {
		return;
	}
	snprintf(dir, sizeof dir, "%s/pilot-import", external);
	DIR* d = opendir(dir);
	if (!d) {
		return;
	}
	AeronVfs* vfs = Aeron_GetVfs();
	struct dirent* e;
	while ((e = readdir(d)) != NULL) {
		const char* name = e->d_name;
		const size_t len = strlen(name);
		if (len < 5 || len > 64 || SDL_strcasecmp(name + len - 4, ".plt") != 0 || strchr(name, '/')) {
			continue;
		}
		char src[1200];
		snprintf(src, sizeof src, "%s/%s", dir, name);
		if (AeronVfs_Exists(vfs, AERON_VFS_ROOT_USER, name)) {
			Aeron_LogInfo("xwa.config", "pilot import: %s is already on the headset, kept that one", name);
			remove(src);
			continue;
		}
		size_t size = 0;
		void* data = SDL_LoadFile(src, &size);
		if (!data) {
			Aeron_LogWarn("xwa.config", "pilot import: could not read %s", src);
			continue;
		}
		if (AeronVfs_WriteAllAtomic(vfs, AERON_VFS_ROOT_USER, name, data, size)) {
			Aeron_LogInfo("xwa.config", "pilot import: %s (%zu bytes)", name, size);
			remove(src);
		} else {
			Aeron_LogWarn("xwa.config", "pilot import: could not write %s", name);
		}
		SDL_free(data);
	}
	closedir(d);
}
#endif

/* Mobile/standalone render defaults. Applied to options the player has not
 * set explicitly, so the in-game Video Options still win. On by default on
 * Android; XWA_QUEST_PROFILE=1 previews it on PC VR. */
static void apply_quest_render_profile(XwaHostConfig* host) {
	const char* env = SDL_getenv("XWA_QUEST_PROFILE");
#if defined(SDL_PLATFORM_ANDROID)
	const int enabled = !(env && env[0] == '0');
#else
	const int enabled = env && env[0] == '1';
#endif
	unsigned int* mask = &host->video_options_override_mask;
	if (!enabled) {
		return;
	}
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_SSAO)) {
		host->video_options.ssao_quality = XWA_MODERN_SSAO_OFF;
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_SSAO;
	}
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_FSR)) {
		host->video_options.fsr_upscaling = XWA_MODERN_FSR_OFF;
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_FSR;
	}
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_MOTION_BLUR)) {
		host->video_options.motion_blur_quality = XWA_MODERN_MOTION_BLUR_OFF;
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_MOTION_BLUR;
	}
#if defined(SDL_PLATFORM_ANDROID)
	{
		/* Once per install: a saved MSAA from before the 2x default (off, or
		 * the old 4x default, which any Video Options save wrote out) becomes
		 * 2x (foveation and direct output work best with MSAA). A chosen FSR
		 * stays, and later choices in the Video Options stay. */
		const char* done = SDL_getenv("XWA_QUEST_MSAA_DEFAULT");
		if (!(done && done[0] == '2')) {
			int settled = 1;
			if ((*mask & XWA_MODERN_VIDEO_OVERRIDE_MSAA) && host->video_options.fsr_upscaling == XWA_MODERN_FSR_OFF &&
				(host->video_options.msaa == XWA_MODERN_MSAA_OFF || host->video_options.msaa == XWA_MODERN_MSAA_4X)) {
				char error[256];
				const char* was = host->video_options.msaa == XWA_MODERN_MSAA_OFF ? "off" : "4x";
				host->video_options.msaa = XWA_MODERN_MSAA_2X;
				if (XwaHostConfig_SaveVideoMsaa(Aeron_GetVfs(), XWA_MODERN_MSAA_2X, error, sizeof error)) {
					Aeron_LogInfo("xwa.config", "saved MSAA %s changed to the new 2x default", was);
				} else {
					settled = 0; /* 2x for this run; tried again next start */
					Aeron_LogWarn("xwa.config", "MSAA 2x default not saved: %s", error);
				}
			}
			if (settled) {
				XwaModernVrOptions_SaveValue("XWA_QUEST_MSAA_DEFAULT", "2");
			}
		}
	}
#endif
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_MSAA)) {
		/* 2x: on the tiled mobile GPU MSAA resolves on chip, and it is what
		 * the foveation density map and direct output attach to. */
		host->video_options.msaa = XWA_MODERN_MSAA_2X;
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_MSAA;
	}
	if (!(*mask & XWA_MODERN_VIDEO_OVERRIDE_HDR)) {
		host->video_options.hdr_output = 0;
		*mask |= XWA_MODERN_VIDEO_OVERRIDE_HDR;
	}
	if (!SDL_getenv("XWA_XR_SHADOWS")) {
		SDL_setenv_unsafe("XWA_XR_SHADOWS", "0", 1);
	}
	if (!SDL_getenv("XWA_XR_ANISOTROPY")) {
		SDL_setenv_unsafe("XWA_XR_ANISOTROPY", "2", 1);
	}
	/* XWA_XR_ASTC (default 1): ship textures compressed to ASTC 4x4, which
	 * the Quest's GPU reads natively (a quarter of the uncompressed
	 * traffic). Compressed once per texture and kept in astc-cache next to
	 * the game data, so only the first load of a ship pays for it. */
	if (!SDL_getenv("AERON_TEXTURE_ASTC")) {
		const char* astc = SDL_getenv("XWA_XR_ASTC");
		SDL_setenv_unsafe("AERON_TEXTURE_ASTC", astc && astc[0] == '0' ? "0" : "1", 1);
	}
#if defined(SDL_PLATFORM_ANDROID)
	if (!SDL_getenv("AERON_ASTC_CACHE_DIR")) {
		const char* external = SDL_GetAndroidExternalStoragePath();
		if (external && external[0]) {
			char dir[1024];
			snprintf(dir, sizeof dir, "%s/astc-cache", external);
			SDL_setenv_unsafe("AERON_ASTC_CACHE_DIR", dir, 1);
		}
	}
#endif
	/* XWA_XR_SHADING: lite (default) = the cheaper mesh shader, full = the
	 * desktop one. */
	if (!SDL_getenv("AERON_PBR_SHADER")) {
		const char* shading = SDL_getenv("XWA_XR_SHADING");
		SDL_setenv_unsafe("AERON_PBR_SHADER", shading && strcmp(shading, "full") == 0 ? "full" : "lite", 1);
	}
	/* Laser and explosion point lights are what saturates the Quest GPU in
	 * dogfights (each one is shaded per pixel, per eye). */
	if (!SDL_getenv("XWA_POINT_LIGHTS")) {
		SDL_setenv_unsafe("XWA_POINT_LIGHTS", "0", 1);
	}
	Aeron_LogInfo("xwa.config", "Quest render profile: SSAO off, FSR off, motion blur off, MSAA %s, HDR off, "
							   "stereo shadows %s, point lights %s",
				  (const char* const[]) { "off", "2x", "4x", "8x" }[host->video_options.msaa & 3],
				  SDL_getenv("XWA_XR_SHADOWS"),
				  SDL_getenv("XWA_POINT_LIGHTS"));
}

/* Host frame CPU split for the VR log (see the main loop). */
static struct {
	double tick_ms, remaster_ms;
	int frames;
	uint64_t window_ns;
} s_frame_prof;

int main(int argc, char** argv) {
	AeronConfig config;
	XwaHostConfig host_config;
	XwaLaunchOptions launch;
	char selected_game_data[XWA_HOST_CONFIG_PATH_CAPACITY];
	char config_error[2048]; /* also the install guide (resolve_game_data) */
	XwaModernWindowMode initial_window_mode;
	int exit_code;
	int setup_cancelled;

	if (!XwaLaunchOptions_Parse(argc, argv, &launch, config_error, sizeof config_error)) {
		Aeron_LogError("xwa.config", "%s", config_error);
		return 2;
	}
#if defined(SDL_PLATFORM_ANDROID)
	android_load_settings();
	if (s_android_fresh_settings) {
		/* A fresh install starts from the recommended VR settings (what
		 * "Restore VR Defaults" sets); the headset tier fills in the rest. */
		XwaModernVrOptions_RestoreDefaults();
	}
	android_xr_defaults();
#endif
	memset(&config, 0, sizeof(config));
	config.org_name = "TotallyOpen";
	config.app_name = "OpenXWA";
	config.resource_root = launch.resource_root[0] ? launch.resource_root : NULL;
	config.resource_path = "resources";
	config.shader_path = "shaders";
	config.window_title = "Neitey's XW: Alliance VR";
	config.window_icon_bmp = xwa_window_icon_bmp;
	config.window_icon_bmp_size = sizeof(xwa_window_icon_bmp);
	/* Windowed size auto-fits the primary display. The logical frame starts
	 * at the 16:9 default and tracks the window aspect once the main loop
	 * runs (XwaPresentation_SyncToWindow). */
	config.logical_width = XWA_PRESENTATION_WIDTH;
	config.logical_height = XWA_PRESENTATION_HEIGHT;
	config.presentation_mode = AERON_PRESENTATION_ASPECT_FIT;
	config.clear_color_enabled = 1;
	config.clear_color_rgba[0] = 0.0f;
	config.clear_color_rgba[1] = 0.0f;
	config.clear_color_rgba[2] = 0.0f;
	config.clear_color_rgba[3] = 1.0f;
	config.xr_enabled = launch.xr_enabled;
#if defined(SDL_PLATFORM_ANDROID)
	/* Meta Quest standalone: always an OpenXR app. */
	config.xr_enabled = 1;
#endif

	if (!Aeron_Init(&config)) {
		return 1;
	}
	if (use_ingame_setup()) {
		Aeron_SetFatalErrorHandler(ingame_fatal_error);
	}
#if defined(SDL_PLATFORM_ANDROID)
	android_shim_check();
	android_default_game_data(&launch);
	android_import_pilots();
#endif
	if (!XwaHostConfig_Load(Aeron_GetVfs(), &host_config, config_error, sizeof config_error)) {
		char message[1536];
		snprintf(message, sizeof message, "%s\nConfiguration path: %s/config.yaml", config_error,
				 Aeron_UserPath());
		Aeron_LogError("xwa.config", "%s", message);
		Aeron_FatalError("Neitey's XW: Alliance VR", message);
		Aeron_Shutdown();
		return 1;
	}
	apply_quest_render_profile(&host_config);
	initial_window_mode = XWA_MODERN_WINDOW_MODE_FULLSCREEN;
	if (host_config.video_options_override_mask & XWA_MODERN_VIDEO_OVERRIDE_WINDOW_MODE) {
		initial_window_mode = host_config.video_options.window_mode;
	}
	while (!resolve_game_data(&launch, &host_config, selected_game_data, sizeof selected_game_data,
							  &setup_cancelled, config_error, sizeof config_error)) {
		if (!setup_cancelled && use_ingame_setup() && launch.game_data_path[0]) {
			Aeron_LogWarn("xwa.config", "%s", config_error);
			const int choice = XwaSetupScreen_Run("X-Wing Alliance game files needed", config_error, "Try Again");
			if (choice == XWA_SETUP_SCREEN_RETRY) {
				continue;
			}
			/* FAILED falls through to the fatal-error path (logged, exit 1). */
			setup_cancelled = choice == XWA_SETUP_SCREEN_QUIT;
		}
		if (!setup_cancelled) {
			Aeron_LogError("xwa.config", "%s", config_error);
			Aeron_FatalError("Neitey's XW: Alliance VR", config_error);
		}
		Aeron_Shutdown();
		return setup_cancelled ? 0 : 1;
	}
	if (strcmp(host_config.game_data_path, selected_game_data) != 0 &&
		!XwaHostConfig_SaveGameDataPath(Aeron_GetVfs(), selected_game_data, config_error,
										sizeof config_error)) {
		Aeron_LogError("xwa.config", "%s", config_error);
		Aeron_FatalError("Neitey's XW: Alliance VR", config_error);
		Aeron_Shutdown();
		return 1;
	}
	Aeron_SetFullscreen(initial_window_mode == XWA_MODERN_WINDOW_MODE_FULLSCREEN);
	/* The setup dialogs above are separate OS windows; on some platforms
	 * (macOS) closing them does not re-key the game window, which would start
	 * the game inside its unfocused pause. Ask for input focus back. */
	Aeron_RaiseWindow();
	Aeron_LogInfo("xwa.config", "game data: %s", selected_game_data);
	Aeron_LogInfo("xwa.config", "resources: %s", Aeron_ResourceRoot());
	Aeron_LogInfo("xwa.config", "flight simulation step: %d ticks", host_config.flight_simulation_step_ticks);
	Aeron_LogInfo("xwa.config", "OPT smoothing angle: %.3g degrees",
				  (double)host_config.model_smooth_angle_degrees);
	Aeron_LogInfo("xwa.config", "OPT emissive strength: %.3g",
				  (double)host_config.model_opt_emissive_strength);
	Aeron_LogInfo("xwa.config", "OPT projectile emissive strength: %.3g",
				  (double)host_config.model_opt_projectile_emissive_strength);
	Aeron_LogInfo("xwa.config", "engine emissive strength: %.3g",
				  (double)host_config.model_engine_emissive_strength);
	Aeron_LogInfo("xwa.config", "force OPT models: %s", host_config.force_opt_models ? "yes" : "no");
	Aeron_LogInfo("xwa.config", "prefer original 2D assets: %s",
				  host_config.prefer_original_2d ? "yes" : "no");
	const XwaRemasterInitOptions remaster_options = {
		.opt_smooth_angle_degrees = host_config.model_smooth_angle_degrees,
		.opt_emissive_strength = host_config.model_opt_emissive_strength,
		.opt_projectile_emissive_strength = host_config.model_opt_projectile_emissive_strength,
		.engine_emissive_strength = host_config.model_engine_emissive_strength,
		.force_opt_models = host_config.force_opt_models,
		.prefer_original_2d = host_config.prefer_original_2d,
		.video_options = host_config.video_options,
		.video_options_override_mask = host_config.video_options_override_mask,
	};
	if (!XwaRemaster_Init(&remaster_options)) {
		Aeron_FatalError("Neitey's XW: Alliance VR",
						 "Required remaster resources could not be loaded. See the log for details.");
		XwaRemaster_Shutdown();
		Aeron_Shutdown();
		return 1;
	}
	{
		XwaModernVideoOptions effective_video_options;
		XwaRemaster_GetVideoOptions(&effective_video_options);
		effective_video_options.window_mode = initial_window_mode;
		XwaModernVideoOptions_Configure(&effective_video_options, apply_modern_video_options,
										persist_modern_video_options);
	}
	XwaModernInputOptions_Configure(&host_config.input_defaults, &host_config.input_options,
									apply_modern_input_options, persist_modern_input_options);
	apply_modern_input_options(&host_config.input_options);
	Aeron_LogInfo(
		"xwa.config", "mouse flight control: %s (%s mode, sensitivity %d, invert Y: %s)",
		host_config.input_options.mouse_flight_enabled ? "on" : "off",
		host_config.input_options.mouse_mode == XWA_MODERN_MOUSE_MODE_POSITION ? "position" : "rate",
		host_config.input_options.mouse_sensitivity, host_config.input_options.mouse_invert_y ? "yes" : "no");

	XwaPort_SetCommandLine(launch.game_command_line);
	XwaModernFlightTiming_Configure(host_config.flight_simulation_step_ticks);
	if (!XwaPort_Init()) {
		XwaRemaster_Shutdown();
		Aeron_Shutdown();
		return 1;
	}

	exit_code = 0;
	/* Cmd+P host pause (the TIE shell model): the game tick is skipped
	 * — sim and snapshots freeze — while the HD remaster keeps
	 * rendering the frozen snapshot every frame (debug UI and
	 * motion-blur inspection stay live). The toggle frame also
	 * skips the tick, so the P press edge never reaches the classic
	 * input path (edges pump inside the tick). */
	{
		int paused = 0;
		int last_fullscreen = Aeron_Fullscreen();
		/* VR: pause while the headset is off or the system menu is open
		 * (XWA_XR_AUTO_PAUSE=0 keeps the game running). */
		const char* xr_auto_pause_env = getenv("XWA_XR_AUTO_PAUSE");
		const int xr_auto_pause = !(xr_auto_pause_env && xr_auto_pause_env[0] == '0');
		int xr_paused = 0;
		while (!Aeron_QuitRequested() && !XwaPort_ShouldQuit()) {
			const int32_t delta_us = Aeron_BeginFrame();
#if defined(SDL_PLATFORM_ANDROID)
			shim_frame();
#endif
			/* Before the game reads input: the VR keyboard consumes its clicks. */
			XwaVrKeyboard_BeginFrame(delta_us);
			/* Pauses after a second without focus: the Steam Frame drops
			 * focus for about half a second now and then (its wear sensor),
			 * which must not stop the game. Resumes at once. */
			static int64_t xr_unfocused_us;
			if (xr_auto_pause && Aeron_XrIsUnfocused()) {
				xr_unfocused_us += delta_us > 0 ? delta_us : 0;
			} else {
				xr_unfocused_us = 0;
			}
			const int xr_unfocused = xr_unfocused_us >= 1000000;
			if (xr_unfocused != xr_paused) {
				xr_paused = xr_unfocused;
				Aeron_AudioSetPaused(paused || xr_paused);
				Aeron_LogInfo("xwa", "%s", xr_paused ? "paused (headset not focused)" : "resumed (headset focused)");
			}
			const AeronInputSnapshot* in = Aeron_InputSnapshot();
			int toggled = 0;
			if (in) {
				XwaPresentation_SyncToWindow(in->window_width, in->window_height);
			}
			/* Reconcile OS-initiated fullscreen transitions (e.g. the macOS
			 * green button) into the stored option. Edge-triggered on the
			 * observed state so an in-flight transition is never fought. */
			const int fullscreen = Aeron_Fullscreen();
			if (fullscreen != last_fullscreen) {
				last_fullscreen = fullscreen;
				set_window_mode_option(fullscreen ? XWA_MODERN_WINDOW_MODE_FULLSCREEN
												  : XWA_MODERN_WINDOW_MODE_WINDOWED);
			}
			/* The toggle frame skips the game tick like the other host
			 * chords so the Enter/F/F11 edge never reaches classic input. */
			if (fullscreen_hotkey_pressed(in)) {
				set_window_mode_option(fullscreen ? XWA_MODERN_WINDOW_MODE_WINDOWED
												  : XWA_MODERN_WINDOW_MODE_FULLSCREEN);
				toggled = 1;
			}
			const int gui_mod = in && (in->key_down[AERON_KEY_LGUI] || in->key_down[AERON_KEY_RGUI]);
			if (gui_mod && in->key_pressed[AERON_KEY_A + ('p' - 'a')]) {
				paused = !paused;
				toggled = 1;
				Aeron_AudioSetPaused(paused || xr_paused);
				Aeron_LogInfo("xwa", "%s", paused ? "paused" : "resumed");
			}
			/* Ctrl+Alt+L is a portable alias for the original Scroll Lock cockpit
			 * mouse-look action. Queue the original action so its gameplay gates remain
			 * authoritative, and skip this tick so the chord cannot leak into flight. */
			if (in && (in->key_down[AERON_KEY_LCTRL] || in->key_down[AERON_KEY_RCTRL]) &&
				(in->key_down[AERON_KEY_LALT] || in->key_down[AERON_KEY_RALT]) &&
				in->key_pressed[AERON_KEY_A + ('l' - 'a')] && XwaPort_QueueMouseLookToggle()) {
				toggled = 1;
			}
			/* Ctrl+Alt+M releases the flight mouse capture back to the OS and
			 * re-captures it. Handled here, and skipping the tick like the pause
			 * toggle, so the M edge never reaches the classic input path. */
			if (in && (in->key_down[AERON_KEY_LCTRL] || in->key_down[AERON_KEY_RCTRL]) &&
				(in->key_down[AERON_KEY_LALT] || in->key_down[AERON_KEY_RALT]) &&
				in->key_pressed[AERON_KEY_A + ('m' - 'a')]) {
				XwaPort_ToggleMouseCapture();
				toggled = 1;
			}
			XwaRemaster_BeginFrame(in);
			if (Aeron_QuitRequested() || XwaPort_ShouldQuit()) {
				break;
			}
			XwaVrConsole_BeginFrame((float)delta_us * 1e-6f);
			if (paused || toggled || xr_paused) {
				XwaPort_PausedFrame();
			} else {
				/* Lap console presses from the last frame become game actions. */
				XwaVrConsoleAction console_action;
				int console_arg;
				while (XwaVrConsole_PollAction(&console_action, &console_arg)) {
					XwaPort_VrConsoleAction(console_action, console_arg);
				}
				/* Voice commands (push-to-talk on the thumb rests). */
				XwaVrVoice_Update();
				/* Slow motion (45 % speed) while the quick wheel is open, eased
				 * in and out; the game decides where it applies. */
				{
					static float slow;
					const char* slow_env = SDL_getenv("XWA_XR_WHEEL_SLOWMO");
					const int slow_on = !(slow_env && slow_env[0] == '0');
					const float dt = (float)delta_us * 1e-6f;
					const int want = XwaVrConsole_WheelOpen() && slow_on;
					slow = want ? fminf(1.0f, slow + dt / 0.15f) : fmaxf(0.0f, slow - dt / 0.25f);
					const float k = slow * slow * (3.0f - 2.0f * slow);
					XwaPort_SetSimTimeScale(1.0f - 0.55f * k);
				}
				const uint64_t tick_t0 = SDL_GetTicksNS();
				XwaPort_Tick(delta_us);
				s_frame_prof.tick_ms += (double)(SDL_GetTicksNS() - tick_t0) * 1e-6;
			}
			{
				XwaVrConsoleState console_state;
				XwaPort_GetVrConsoleState(&console_state);
				XwaVrConsole_SetState(&console_state);
			}
			if (Aeron_QuitRequested() || XwaPort_ShouldQuit()) {
				break;
			}
			const uint64_t remaster_t0 = SDL_GetTicksNS();
			XwaRemaster_Frame(delta_us);
			{
				/* VR: the game's tick and the renderer's frame, every 5 s
				 * (XWA_XR_CPU_PROFILE=0: off). */
				const uint64_t now = SDL_GetTicksNS();
				s_frame_prof.remaster_ms += (double)(now - remaster_t0) * 1e-6;
				s_frame_prof.frames++;
				if (!s_frame_prof.window_ns) {
					s_frame_prof.window_ns = now;
				}
				if (now - s_frame_prof.window_ns >= 5000000000ull) {
					const char* env = SDL_getenv("XWA_XR_CPU_PROFILE");
					if (Aeron_XrIsPresenting() && !(env && env[0] == '0') && s_frame_prof.frames > 0) {
						Aeron_LogInfo("xwa", "VR frame CPU (ms/frame): game tick %.2f, renderer %.2f",
									  s_frame_prof.tick_ms / s_frame_prof.frames,
									  s_frame_prof.remaster_ms / s_frame_prof.frames);
					}
					memset(&s_frame_prof, 0, sizeof s_frame_prof);
					s_frame_prof.window_ns = now;
				}
			}
			XwaVrConsole_EndFrame();
			XwaVrStick_EndFrame();
			{
				/* The player put the lap console somewhere new: keep it there. */
				char placement[160];
				if (XwaVrConsole_TakePlacement(placement, (int)sizeof placement)) {
					XwaModernVrOptions_SaveValue("XWA_XR_CONSOLE_POSE", placement);
				}
				/* The flight stick moved. */
				char stick_pos[64];
				if (XwaVrStick_TakePlacement(stick_pos, (int)sizeof stick_pos)) {
					XwaModernVrOptions_SaveValue("XWA_XR_STICK_POS", stick_pos);
				}
				/* The side panel shows another tab. */
				char panel_tab[16];
				if (XwaVrConsole_TakePanelTab(panel_tab, (int)sizeof panel_tab)) {
					XwaModernVrOptions_SaveValue("XWA_XR_PANEL_TAB", panel_tab);
				}
				/* A HUD panel moved: this ship's layout. */
				char layout_key[96];
				char layout[1024];
				if (XwaVrCockpitHud_TakeLayout(layout_key, (int)sizeof layout_key, layout, (int)sizeof layout)) {
					XwaModernVrOptions_SaveValue(layout_key, layout);
				}
			}
			if (Aeron_QuitRequested() || XwaPort_ShouldQuit()) {
				break;
			}
			XwaVrKeyboard_Submit();
			if (!Aeron_Present()) {
				Aeron_RequestFatalRendererError("frame presentation");
				break;
			}
			uint64_t wake_us = presentation_interval_us();
			if (!paused && !xr_paused) {
				const uint64_t task_us = XwaPort_NextWakeDelayUs();
				if (task_us < wake_us)
					wake_us = task_us;
			}
			Aeron_WaitForNextFrame(wake_us);
		}
	}

	exit_code = Aeron_FatalErrorRequested() ? 1 : XwaPort_GetExitCode();
	XwaVrKeyboard_Shutdown();
	XwaVrVoice_Shutdown();
	XwaRemaster_Shutdown();
	XwaPort_Shutdown();
	Aeron_Shutdown();

	return exit_code;
}
