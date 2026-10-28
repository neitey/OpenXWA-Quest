#ifndef XWA_RUNTIME_MODERN_VR_BUTTONS_SCREEN_H
#define XWA_RUNTIME_MODERN_VR_BUTTONS_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* VR Options > Remap Buttons (the button mapping, vr_mapping.h).
 * Draws and updates the page; returns nonzero when the player goes back. */
int XwaModernVrButtonsScreen_Update(int menu_center_x, int* cursor_row);
/* Back to its first page (when the options screen is entered anew). */
void XwaModernVrButtonsScreen_Reset(void);
/* The default (Classic) buttons, applied and saved. */
void XwaModernVrButtons_RestoreDefaults(void);

#ifdef __cplusplus
}
#endif

#endif
