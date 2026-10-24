#ifndef XWA_RUNTIME_INPUT_CONTROLLER_MAPPING_H
#define XWA_RUNTIME_INPUT_CONTROLLER_MAPPING_H

#include "aeron/input.h"
#include "xwa_runtime/config/modern_input_options.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XwaControllerLogicalState {
	uint32_t axes[XWA_CONTROLLER_LOGICAL_AXIS_COUNT];
	uint32_t buttons;
	int pov_direction; /* -1 centered, otherwise up/right/down/left = 0..3. */
	int has_pov;
	int8_t source_axes[XWA_CONTROLLER_LOGICAL_AXIS_COUNT];
	int16_t source_axis_values[XWA_CONTROLLER_LOGICAL_AXIS_COUNT];
} XwaControllerLogicalState;

void XwaControllerMapping_SetOptions(const XwaControllerOptions* options);
const AeronControllerSnapshot* XwaControllerMapping_SelectedController(void);
uint32_t XwaControllerMapping_SelectedInstanceId(void);
int XwaControllerMapping_SelectedHasRumble(void);
int XwaControllerMapping_Rumble(uint16_t low_frequency_rumble, uint16_t high_frequency_rumble,
								uint32_t duration_ms);
int XwaControllerMapping_ConsumeSelectionChange(void);
int XwaControllerMapping_GetState(XwaControllerLogicalState* state);

/* VR flight: while enabled (set every frame), GetState puts the Touch
 * controllers' pitch, yaw and roll through the thumbstick response curve
 * (XWA_XR_STICK_CURVE, 1.0 direct .. 4.0, default 2.0). */
void XwaControllerMapping_SetXrFlight(int enabled);
/* The grabbed VR flight stick's deflection as handed to
 * Aeron_XrSetVirtualStick: an axis it drives stays linear. */
void XwaControllerMapping_SetXrVirtualStick(int held, float x, float y, float roll);
double XwaControllerMapping_XrStickCurveExponent(void);
/* One axis value (SDL range) through the curve: unchanged up to `deadzone`
 * (0..1 of full deflection), then deadzone + (1 - deadzone) * t^exponent,
 * so full deflection stays full. */
int16_t XwaControllerMapping_XrStickCurve(int16_t value, double deadzone, double exponent);
/* The steering thumbstick (x, y in SDL range) as one stick: the curve acts
 * on its deflection (radius, a radial dead zone, the last 5 % of the throw
 * counts as full) and the direction is kept; the round gate is then
 * stretched onto the square gate of the joystick the game was made for, so
 * the axis pushed further gets the curved deflection and a full diagonal is
 * full pitch and full yaw. exponent 1 = no curve (gate mapping only). */
void XwaControllerMapping_XrStickRadial(int16_t x, int16_t y, double deadzone, double exponent, int16_t out[2]);
void XwaControllerMapping_CopySelectedActions(uint16_t actions[XWA_CONTROLLER_ACTION_COUNT]);

/* Pure conversion entry point used by the runtime facade and unit tests. */
void XwaControllerMapping_MapSnapshot(const XwaControllerOptions* options,
									  const AeronControllerSnapshot* controller, int has_focus,
									  XwaControllerLogicalState* state);

#ifdef __cplusplus
}
#endif

#endif
