#ifndef XWA_REMASTER_VR_COCKPIT_HUD_H
#define XWA_REMASTER_VR_COCKPIT_HUD_H

/*
 * VR cockpit HUD: the HUD's instruments as separate panels
 * placed on the cockpit (radar pods beside the canopy, targeting computer
 * on the dash, MFDs on the side consoles, ...) instead of one sheet in
 * front of the eyes. The HUD is still drawn as usual into the head HUD
 * target; each panel is a rectangle of that target shown as its own
 * OpenXR quad (Aeron_XrStereoCommitPanels). Reticle, brackets and target
 * arrows stay on the far world quad.
 */

#include "aeron/render.h"
#include "aeron/xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Finds the panel rectangles in this frame's prepared HUD (call after
 * XwaRemasterHud_PrepareFrame for a target_w x target_h HUD) and fills
 * `out` with their cockpit placements. With `cmd_separate` the targeting
 * computer was drawn to the aux HUD target (XwaRemasterHud_SetCmdSplit). `world_scale` is XWA_XR_WORLD_SCALE:
 * panels move and scale with the cockpit. Returns the panel count. */
int XwaVrCockpitHud_Build(int target_w, int target_h, float world_scale, int cmd_separate, const char* cockpit,
						  AeronXrPanel* out, int max_out);

/* The panels can be picked up with the grip and put anywhere (6DoF); the
 * layout is kept per cockpit (`cockpit` above: the cockpit model's name)
 * as XWA_XR_HUD_POSE_<COCKPIT>, shared by every craft flown from that
 * cockpit. After a move, hands out that key and value once, to be saved. */
int XwaVrCockpitHud_TakeLayout(char* key, int key_size, char* value, int value_size);

/* An outline around the panels a hand is on or holds, drawn into the HUD
 * targets above the HUD. Prepare outside a render pass; Render inside the
 * HUD pass (aux: the targeting computer's own target). */
void XwaVrCockpitHud_PrepareOverlay(AeronCommandBuffer* cmd, int target_w, int target_h);
void XwaVrCockpitHud_RenderOverlay(AeronCommandBuffer* cmd, AeronRenderPass* pass, AeronRenderTarget* target, int aux);

void XwaVrCockpitHud_Shutdown(void);

/* The player's seat position (VR Options "Seat Position",
 * XWA_XR_SEAT_POSITION): metres further back from the window, -0.2 (closer)
 * .. 0.2 (further), 0 by default. The cockpit (flight.c) and these panels
 * move away from the eye by it, so the panels stay on their cockpit spots;
 * hand-placed panels are stored as at 0 and follow it too. */
float XwaVrCockpitHud_SeatBack(void);

#ifdef __cplusplus
}
#endif

#endif
