#include "xwa_runtime/input/controller_mapping.h"

#include "aeron/aeron.h"

#include <SDL3/SDL_stdinc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
	CONTROLLER_AXIS_RANGE = 65535,
	CONTROLLER_AXIS_CENTER = 32768,
};

typedef struct XwaControllerMappingState {
	XwaControllerOptions options;
	int configured;
	uint32_t previous_instance_id;
	uint32_t digital_axis_buttons;
} XwaControllerMappingState;

static XwaControllerMappingState g_controllerMapping;

/* VR flight with the Touch controllers (the thumbstick response curve). */
static struct {
	int flight;
	int stick_held;
	float stick[3]; /* the grabbed flight stick: x, y, roll (-1..1) */
} g_xrCurve;

static int ControllerMapping_ProfileEqual(const XwaControllerProfile* lhs, const XwaControllerProfile* rhs) {
	int i;

	if (lhs->pov_source != rhs->pov_source) {
		return 0;
	}
	for (i = 0; i < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++i) {
		if (lhs->axes[i].source != rhs->axes[i].source || lhs->axes[i].invert != rhs->axes[i].invert ||
			lhs->axes[i].deadzone != rhs->axes[i].deadzone) {
			return 0;
		}
	}
	for (i = 0; i < XWA_CONTROLLER_LOGICAL_BUTTON_COUNT; ++i) {
		if (lhs->buttons[i].kind != rhs->buttons[i].kind || lhs->buttons[i].index != rhs->buttons[i].index ||
			lhs->buttons[i].threshold != rhs->buttons[i].threshold) {
			return 0;
		}
	}
	return memcmp(lhs->actions, rhs->actions, sizeof(lhs->actions)) == 0;
}

static int ControllerMapping_OptionsEqual(const XwaControllerOptions* lhs, const XwaControllerOptions* rhs) {
	return strcmp(lhs->device.guid, rhs->device.guid) == 0 &&
		   strcmp(lhs->device.path, rhs->device.path) == 0 && lhs->device.ordinal == rhs->device.ordinal &&
		   lhs->rumble_enabled == rhs->rumble_enabled &&
		   ControllerMapping_ProfileEqual(&lhs->gamepad, &rhs->gamepad) &&
		   ControllerMapping_ProfileEqual(&lhs->joystick, &rhs->joystick);
}

const AeronControllerSnapshot* XwaControllerMapping_SelectedController(void) {
	const AeronInputSnapshot* input = Aeron_InputSnapshot();

	if (!input || !g_controllerMapping.configured) {
		return NULL;
	}
#if defined(__ANDROID__)
	/* On a standalone headset Android also lists system input devices as
	 * joysticks; with no device chosen in the options, fly with the VR
	 * controllers rather than whichever device happens to be first. */
	if (!g_controllerMapping.options.device.guid[0] && !g_controllerMapping.options.device.path[0]) {
		static const AeronControllerSelector touch = { .path = "openxr:touch" };
		const AeronControllerSnapshot* controller = Aeron_SelectController(input, &touch);
		if (controller) {
			return controller;
		}
	}
#endif
	return Aeron_SelectController(input, &g_controllerMapping.options.device);
}

static const XwaControllerProfile* ControllerMapping_Profile(const XwaControllerOptions* options,
															 const AeronControllerSnapshot* controller) {
	return controller->kind == AERON_CONTROLLER_KIND_GAMEPAD ? &options->gamepad : &options->joystick;
}

static int16_t ControllerMapping_AxisValue(const AeronControllerSnapshot* controller, int source) {
	if (source < 0) {
		return 0;
	}
	if (controller->kind == AERON_CONTROLLER_KIND_GAMEPAD) {
		return source < AERON_GAMEPAD_AXIS_COUNT ? controller->gamepad_axes[source] : 0;
	}
	return source < controller->axis_count && source < AERON_CONTROLLER_AXIS_MAX
			   ? controller->raw_axes[source]
			   : 0;
}

static int ControllerMapping_IsTrigger(const AeronControllerSnapshot* controller, int source) {
	return controller->kind == AERON_CONTROLLER_KIND_GAMEPAD &&
		   (source == AERON_GAMEPAD_AXIS_LEFT_TRIGGER || source == AERON_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

static uint32_t ControllerMapping_CenteredAxis(int16_t value, int invert, float deadzone) {
	double normalized = value < 0 ? (double)value / 32768.0 : (double)value / 32767.0;
	double magnitude;
	uint32_t mapped;

	magnitude = normalized < 0.0 ? -normalized : normalized;
	if (magnitude <= deadzone) {
		return CONTROLLER_AXIS_CENTER;
	}
	/* Shift SDL's complete signed range onto the complete WinMM range without
	 * losing the negative endpoint. */
	mapped = (uint32_t)((int32_t)value + CONTROLLER_AXIS_CENTER);
	return invert ? CONTROLLER_AXIS_RANGE - mapped : mapped;
}

static uint32_t ControllerMapping_TriggerAxis(int16_t value, int invert, float deadzone) {
	double normalized = (double)value / 32767.0;

	if (normalized < 0.0) {
		normalized = 0.0;
	} else if (normalized > 1.0) {
		normalized = 1.0;
	}
	if (normalized <= deadzone) {
		normalized = 0.0;
	}
	if (invert) {
		normalized = 1.0 - normalized;
	}
	return (uint32_t)(normalized * CONTROLLER_AXIS_RANGE);
}

static uint8_t ControllerMapping_Hat(const AeronControllerSnapshot* controller,
									 const XwaControllerProfile* profile) {
	uint8_t hat = AERON_CONTROLLER_HAT_CENTERED;

	if (controller->kind == AERON_CONTROLLER_KIND_GAMEPAD) {
		if (!profile->pov_source) {
			return hat;
		}
		if (controller->gamepad_buttons & (1u << AERON_GAMEPAD_BUTTON_DPAD_UP)) {
			hat |= AERON_CONTROLLER_HAT_UP;
		}
		if (controller->gamepad_buttons & (1u << AERON_GAMEPAD_BUTTON_DPAD_RIGHT)) {
			hat |= AERON_CONTROLLER_HAT_RIGHT;
		}
		if (controller->gamepad_buttons & (1u << AERON_GAMEPAD_BUTTON_DPAD_DOWN)) {
			hat |= AERON_CONTROLLER_HAT_DOWN;
		}
		if (controller->gamepad_buttons & (1u << AERON_GAMEPAD_BUTTON_DPAD_LEFT)) {
			hat |= AERON_CONTROLLER_HAT_LEFT;
		}
		return hat;
	}
	if (profile->pov_source >= 0 && profile->pov_source < controller->hat_count &&
		profile->pov_source < AERON_CONTROLLER_HAT_MAX) {
		return controller->raw_hats[profile->pov_source];
	}
	return hat;
}

static int ControllerMapping_PovDirection(uint8_t hat) {
	/* XWA has four POV actions. Vertical wins for diagonal hats. */
	if (hat & AERON_CONTROLLER_HAT_UP) {
		return 0;
	}
	if (hat & AERON_CONTROLLER_HAT_DOWN) {
		return 2;
	}
	if (hat & AERON_CONTROLLER_HAT_RIGHT) {
		return 1;
	}
	if (hat & AERON_CONTROLLER_HAT_LEFT) {
		return 3;
	}
	return -1;
}

static int ControllerMapping_HasPov(const AeronControllerSnapshot* controller,
									const XwaControllerProfile* profile) {
	if (controller->kind == AERON_CONTROLLER_KIND_GAMEPAD) {
		return profile->pov_source != 0;
	}
	return profile->pov_source >= 0 && profile->pov_source < controller->hat_count;
}

static void ControllerMapping_LogUnavailableSources(const AeronControllerSnapshot* controller) {
	const XwaControllerProfile* profile;
	int unavailable_axes = 0;
	int unavailable_buttons = 0;
	int unavailable_pov = 0;
	int i;

	if (!controller || controller->kind != AERON_CONTROLLER_KIND_JOYSTICK) {
		return;
	}
	profile = &g_controllerMapping.options.joystick;
	for (i = 0; i < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++i) {
		unavailable_axes += profile->axes[i].source >= controller->axis_count;
	}
	for (i = 0; i < XWA_CONTROLLER_LOGICAL_BUTTON_COUNT; ++i) {
		const AeronControllerDigitalSource* binding = &profile->buttons[i];
		if (binding->kind == AERON_CONTROLLER_DIGITAL_BUTTON) {
			unavailable_buttons += binding->index >= controller->button_count;
		} else if (binding->kind == AERON_CONTROLLER_DIGITAL_AXIS_POSITIVE ||
				   binding->kind == AERON_CONTROLLER_DIGITAL_AXIS_NEGATIVE) {
			unavailable_axes += binding->index >= controller->axis_count;
		}
	}
	unavailable_pov = profile->pov_source >= controller->hat_count;
	if (unavailable_axes || unavailable_buttons || unavailable_pov) {
		Aeron_LogWarn("xwa.input",
					  "Controller '%s' mapping references unavailable controls (%d axes, %d buttons, "
					  "invalid POV=%s)",
					  controller->name, unavailable_axes, unavailable_buttons,
					  unavailable_pov ? "yes" : "no");
	}
}

static void ControllerMapping_MapSnapshot(const XwaControllerOptions* options,
										  const AeronControllerSnapshot* controller, int has_focus,
										  uint32_t previous_axis_buttons, uint32_t* axis_buttons,
										  XwaControllerLogicalState* state) {
	const XwaControllerProfile* profile;
	int logical;

	if (axis_buttons) {
		*axis_buttons = 0;
	}
	if (!state) {
		return;
	}
	memset(state, 0, sizeof(*state));
	state->pov_direction = -1;
	for (logical = 0; logical < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++logical) {
		state->axes[logical] = CONTROLLER_AXIS_CENTER;
		state->source_axes[logical] = -1;
	}
	if (!options || !controller || !controller->connected) {
		return;
	}
	profile = ControllerMapping_Profile(options, controller);
	state->has_pov = ControllerMapping_HasPov(controller, profile);
	for (logical = 0; logical < XWA_CONTROLLER_LOGICAL_AXIS_COUNT; ++logical) {
		const XwaControllerAxisBinding* binding = &profile->axes[logical];
		const int16_t value = ControllerMapping_AxisValue(controller, binding->source);
		state->source_axes[logical] = (int8_t)binding->source;
		state->source_axis_values[logical] = value;
		if (!has_focus) {
			continue;
		}
		state->axes[logical] =
			ControllerMapping_IsTrigger(controller, binding->source)
				? ControllerMapping_TriggerAxis(value, binding->invert, binding->deadzone)
				: ControllerMapping_CenteredAxis(value, binding->invert, binding->deadzone);
	}
	if (!has_focus) {
		return;
	}
	for (logical = 0; logical < XWA_CONTROLLER_LOGICAL_BUTTON_COUNT; ++logical) {
		const AeronControllerDigitalSource* binding = &profile->buttons[logical];
		const uint32_t bit = 1u << logical;
		if (Aeron_ControllerDigitalSourceDown(controller, binding, (previous_axis_buttons & bit) != 0)) {
			state->buttons |= 1u << logical;
			if (axis_buttons && binding->kind != AERON_CONTROLLER_DIGITAL_BUTTON) {
				*axis_buttons |= bit;
			}
		}
	}
	state->pov_direction = ControllerMapping_PovDirection(ControllerMapping_Hat(controller, profile));
}

void XwaControllerMapping_MapSnapshot(const XwaControllerOptions* options,
									  const AeronControllerSnapshot* controller, int has_focus,
									  XwaControllerLogicalState* state) {
	ControllerMapping_MapSnapshot(options, controller, has_focus, 0, NULL, state);
}

void XwaControllerMapping_SetOptions(const XwaControllerOptions* options) {
	const uint32_t old_instance_id = XwaControllerMapping_SelectedInstanceId();
	const uint32_t previous_instance_id = g_controllerMapping.previous_instance_id;

	if (options && g_controllerMapping.configured &&
		ControllerMapping_OptionsEqual(options, &g_controllerMapping.options)) {
		return;
	}
	if (old_instance_id != 0) {
		Aeron_RumbleController(old_instance_id, 0, 0, 0);
	}
	memset(&g_controllerMapping, 0, sizeof(g_controllerMapping));
	/* Option changes must not masquerade as physical device changes. */
	g_controllerMapping.previous_instance_id = previous_instance_id;
	if (!options) {
		return;
	}
	g_controllerMapping.options = *options;
	g_controllerMapping.configured = 1;
	if ((options->device.guid[0] || options->device.path[0]) && !XwaControllerMapping_SelectedController()) {
		Aeron_LogWarn("xwa.input", "Configured controller is unavailable (GUID '%s', path '%s', ordinal %d)",
					  options->device.guid, options->device.path, options->device.ordinal);
	}
}

uint32_t XwaControllerMapping_SelectedInstanceId(void) {
	const AeronControllerSnapshot* controller = XwaControllerMapping_SelectedController();
	return controller ? controller->instance_id : 0;
}

int XwaControllerMapping_SelectedHasRumble(void) {
	const AeronControllerSnapshot* controller = XwaControllerMapping_SelectedController();
	return controller && controller->has_rumble;
}

int XwaControllerMapping_Rumble(uint16_t low_frequency_rumble, uint16_t high_frequency_rumble,
								uint32_t duration_ms) {
	const AeronControllerSnapshot* controller = XwaControllerMapping_SelectedController();
	const int stopping = (low_frequency_rumble == 0 && high_frequency_rumble == 0) || duration_ms == 0;

	if (!controller || !controller->has_rumble) {
		if (!stopping) {
			Aeron_LogWarn("xwa.input", "Rumble requested while the selected controller is %s",
						  controller ? "unsupported" : "disconnected");
		}
		return 0;
	}
	if (!stopping && !g_controllerMapping.options.rumble_enabled) {
		return 0;
	}
	return Aeron_RumbleController(controller->instance_id, low_frequency_rumble, high_frequency_rumble,
								  duration_ms);
}

int XwaControllerMapping_ConsumeSelectionChange(void) {
	const uint32_t instance_id = XwaControllerMapping_SelectedInstanceId();
	if (instance_id == g_controllerMapping.previous_instance_id) {
		return 0;
	}
	if (g_controllerMapping.previous_instance_id != 0) {
		Aeron_RumbleController(g_controllerMapping.previous_instance_id, 0, 0, 0);
	}
	Aeron_LogInfo("xwa.input", "Active controller changed from %u to %u",
				  g_controllerMapping.previous_instance_id, instance_id);
	g_controllerMapping.previous_instance_id = instance_id;
	g_controllerMapping.digital_axis_buttons = 0;
	ControllerMapping_LogUnavailableSources(XwaControllerMapping_SelectedController());
	return 1;
}

void XwaControllerMapping_SetXrFlight(int enabled) { g_xrCurve.flight = enabled != 0; }

void XwaControllerMapping_SetXrVirtualStick(int held, float x, float y, float roll) {
	g_xrCurve.stick_held = held != 0;
	g_xrCurve.stick[0] = held ? x : 0.0f;
	g_xrCurve.stick[1] = held ? y : 0.0f;
	g_xrCurve.stick[2] = held ? roll : 0.0f;
}

double XwaControllerMapping_XrStickCurveExponent(void) {
	const char* env = SDL_getenv("XWA_XR_STICK_CURVE");
	double exponent = env && env[0] ? atof(env) : 2.0;

	if (!(exponent >= 1.0)) {
		exponent = 1.0;
	}
	return exponent > 4.0 ? 4.0 : exponent;
}

int16_t XwaControllerMapping_XrStickCurve(int16_t value, double deadzone, double exponent) {
	const double full = value < 0 ? 32768.0 : 32767.0;
	const double a = (value < 0 ? -(double)value : (double)value) / full;
	double t;
	long out;

	if (exponent <= 1.0 || a <= deadzone || deadzone >= 0.99) {
		return value;
	}
	t = (a - deadzone) / (1.0 - deadzone);
	out = lround((deadzone + (1.0 - deadzone) * pow(t > 1.0 ? 1.0 : t, exponent)) * full);
	return (int16_t)(value < 0 ? -out : out);
}

static double ControllerMapping_AxisUnit(int16_t value) { return value < 0 ? value / 32768.0 : value / 32767.0; }

static int16_t ControllerMapping_UnitAxis(double value) {
	if (value >= 1.0) {
		return 32767;
	}
	if (value <= -1.0) {
		return -32768;
	}
	return (int16_t)lround(value < 0.0 ? value * 32768.0 : value * 32767.0);
}

void XwaControllerMapping_XrStickRadial(int16_t x, int16_t y, double deadzone, double exponent, int16_t out[2]) {
	static const double k_outer = 0.95; /* the last 5 % of the throw is full */
	const double fx = ControllerMapping_AxisUnit(x);
	const double fy = ControllerMapping_AxisUnit(y);
	const double r = sqrt(fx * fx + fy * fy);
	const double major = fabs(fx) > fabs(fy) ? fabs(fx) : fabs(fy);
	double t, m, scale;

	if (deadzone < 0.0) {
		deadzone = 0.0;
	}
	if (deadzone >= 0.9) {
		out[0] = x;
		out[1] = y;
		return;
	}
	if (r <= deadzone || major <= 0.0) {
		out[0] = out[1] = 0;
		return;
	}
	t = (r - deadzone) / (k_outer - deadzone);
	t = t > 1.0 ? 1.0 : t;
	m = deadzone + (1.0 - deadzone) * (exponent > 1.0 ? pow(t, exponent) : t);
	/* Round gate onto square gate: the axis pushed further gets m, the other
	 * keeps the direction's ratio (a full diagonal is full on both). */
	scale = m / major;
	out[0] = ControllerMapping_UnitAxis(fx * scale);
	out[1] = ControllerMapping_UnitAxis(fy * scale);
}

/* Response curve of the Touch thumbsticks in VR flight (XWA_XR_STICK_CURVE,
 * default 2.0 "Precise"; 1 = linear): small deflections turn gently for fine
 * aim, full deflection still gives the full turn rate. Pitch, yaw and roll
 * only (not the throttle), curved beyond the larger of the binding's dead
 * zone and the game's own (Joystick_PollRawAxes: a twentieth of the X/Y
 * range, a tenth of R's, i.e. 10 % / 20 % of the deflection), so the craft
 * answers as soon as the stick leaves it, just gently.
 *
 * The steering thumbstick (the virtual gamepad's left stick) is handled as
 * one stick (XwaControllerMapping_XrStickRadial): curving each axis on its
 * own turned a round-gate diagonal (0.71 per axis) into half the rate per
 * axis, and even linear a thumbstick never reaches the full diagonal of the
 * game's square-gate joystick. Now a diagonal turns as fast in pitch and yaw
 * as straight up or across.
 *
 * The grabbed virtual flight stick shares the right thumbstick's axes
 * (whichever is pushed further); an axis it drives stays linear. Mouse,
 * desktop gamepads and joysticks never come here (only the "openxr:touch"
 * controller). */
static int ControllerMapping_XrVirtualStickDrives(int source, int16_t value) {
	float stick = 0.0f;

	if (source == AERON_GAMEPAD_AXIS_LEFTX) {
		stick = g_xrCurve.stick[0];
	} else if (source == AERON_GAMEPAD_AXIS_LEFTY) {
		stick = g_xrCurve.stick[1];
	} else if (source == AERON_GAMEPAD_AXIS_RIGHTX) {
		stick = g_xrCurve.stick[2];
	}
	return g_xrCurve.stick_held && stick != 0.0f && fabsf(stick) * 32767.0f + 64.0f >= (float)abs(value);
}

static void ControllerMapping_ApplyXrStickCurve(const AeronControllerSnapshot* controller,
												XwaControllerLogicalState* state) {
	static const int k_axes[] = { XWA_CONTROLLER_AXIS_YAW, XWA_CONTROLLER_AXIS_PITCH, XWA_CONTROLLER_AXIS_ROLL };
	static const double k_game_deadzone[] = { 0.10, 0.10, 0.20 };
	const XwaControllerProfile* profile;
	double exponent;
	double steer_deadzone = 0.0;
	int16_t steer[2];
	int steer_driven = 0;

	if (!g_xrCurve.flight || !controller || controller->kind != AERON_CONTROLLER_KIND_GAMEPAD ||
		strcmp(controller->path, "openxr:touch") != 0) {
		return;
	}
	exponent = XwaControllerMapping_XrStickCurveExponent();
	profile = &g_controllerMapping.options.gamepad;
	/* The steering stick's radial dead zone: the largest of its flight axes'. */
	for (size_t i = 0; i < sizeof k_axes / sizeof k_axes[0]; ++i) {
		const XwaControllerAxisBinding* binding = &profile->axes[k_axes[i]];
		double deadzone;

		if (binding->source != AERON_GAMEPAD_AXIS_LEFTX && binding->source != AERON_GAMEPAD_AXIS_LEFTY) {
			continue;
		}
		deadzone = binding->deadzone > k_game_deadzone[i] ? (double)binding->deadzone : k_game_deadzone[i];
		steer_deadzone = deadzone > steer_deadzone ? deadzone : steer_deadzone;
		if (ControllerMapping_XrVirtualStickDrives(binding->source, state->source_axis_values[k_axes[i]])) {
			steer_driven = 1;
		}
	}
	XwaControllerMapping_XrStickRadial(controller->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTX],
									   controller->gamepad_axes[AERON_GAMEPAD_AXIS_LEFTY], steer_deadzone, exponent,
									   steer);
	for (size_t i = 0; i < sizeof k_axes / sizeof k_axes[0]; ++i) {
		const XwaControllerAxisBinding* binding = &profile->axes[k_axes[i]];
		const int16_t value = state->source_axis_values[k_axes[i]];
		const double deadzone =
			binding->deadzone > k_game_deadzone[i] ? (double)binding->deadzone : k_game_deadzone[i];
		int16_t curved;

		if (binding->source < 0 || ControllerMapping_IsTrigger(controller, binding->source) ||
			ControllerMapping_XrVirtualStickDrives(binding->source, value)) {
			continue;
		}
		if (!steer_driven && binding->source == AERON_GAMEPAD_AXIS_LEFTX) {
			curved = steer[0];
		} else if (!steer_driven && binding->source == AERON_GAMEPAD_AXIS_LEFTY) {
			curved = steer[1];
		} else if (exponent > 1.0) {
			curved = XwaControllerMapping_XrStickCurve(value, deadzone, exponent);
		} else {
			continue;
		}
		state->axes[k_axes[i]] = ControllerMapping_CenteredAxis(curved, binding->invert, binding->deadzone);
	}
}

int XwaControllerMapping_GetState(XwaControllerLogicalState* state) {
	const AeronInputSnapshot* input = Aeron_InputSnapshot();
	const AeronControllerSnapshot* controller = XwaControllerMapping_SelectedController();

	ControllerMapping_MapSnapshot(&g_controllerMapping.options, controller, input && input->has_focus,
								  g_controllerMapping.digital_axis_buttons,
								  &g_controllerMapping.digital_axis_buttons, state);
	if (state && controller && controller->connected && input && input->has_focus) {
		ControllerMapping_ApplyXrStickCurve(controller, state);
	}
	return controller != NULL;
}

void XwaControllerMapping_CopySelectedActions(uint16_t actions[XWA_CONTROLLER_ACTION_COUNT]) {
	const AeronControllerSnapshot* controller;
	const XwaControllerProfile* profile;

	if (!actions) {
		return;
	}
	controller = XwaControllerMapping_SelectedController();
	profile = controller && controller->kind == AERON_CONTROLLER_KIND_JOYSTICK
				  ? &g_controllerMapping.options.joystick
				  : &g_controllerMapping.options.gamepad;
	memcpy(actions, profile->actions, sizeof(profile->actions));
}
