#ifndef XWA_RUNTIME_MODERN_VR_OPTIONS_SCREEN_H
#define XWA_RUNTIME_MODERN_VR_OPTIONS_SCREEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* VR settings screen (main menu > VR Options). The settings are
 * the XWA_XR_* / XWA_POINT_LIGHTS / AERON_XR_RESOLUTION_SCALE environment
 * values that openxwa-quest.cfg sets at startup: a change takes effect by
 * updating the environment (the renderer reads them every frame) and is
 * written back to openxwa-quest.cfg when the screen is left. */

/* Nonzero when the screen is offered (VR presenting, or an Android build). */
int XwaModernVrOptions_Available(void);
/* The optional X-Wing Revival cockpits are installed (their GLBs in the
 * APK's revival_cockpits assets, next to the game on PC, or in
 * XWA_XR_REVIVAL_COCKPITS): only then does VR Options offer "Cockpit Model"
 * and the credits name them. Looked up once. */
int XwaModernVrOptions_RevivalCockpitsInstalled(void);

/* Draws and updates the screen. Returns nonzero when the caller should
 * return to the parent video menu. */
int XwaModernVrOptionsScreen_Update(int menu_center_x, int* cursor_row);

/* Writes KEY=value into the VR settings file (Android, Windows; elsewhere a
 * no-op). */
void XwaModernVrOptions_SaveValue(const char* key, const char* value);
/* The same for several keys in one go (any count). */
void XwaModernVrOptions_SaveValues(const char* const* keys, const char* const* values, int count);
/* "Restore VR Defaults": every VR option and hand-placed spot back to the
 * recommended settings (applied now and saved; "(restart)" options take
 * effect at the next start). */
void XwaModernVrOptions_RestoreDefaults(void);

/* The VR settings file (KEY=VALUE lines applied as environment variables at
 * start, written by the VR options): openxwa-quest.cfg in the app's external
 * storage on Quest, openxwa-vr.cfg in the user folder on Windows (PCVR).
 * 0 where there is none. */
int XwaModernVrOptions_SettingsPath(char* out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
