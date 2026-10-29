/* Optional X-Wing Revival 3D cockpits in the VR eyes, in place of XWA's
 * cockpit OPTs (VR Options, "Cockpit Model"; XWA's own cockpits are the
 * default).
 *
 * The cockpits are by Raptor007 (X-Wing Revival) and the X-Wing Alliance
 * Upgrade team. They are bundled in the APK but are not part of this
 * repository: packaging/quest/tools/
 * gen_revival_cockpits.py converts them at package time into GLBs
 * whose vertices are metres in the pilot's eye frame (x right, y down,
 * z forward after Aeron's glTF import), the eye at the origin:
 *
 *   xwing.glb ywing.glb awing.glb bwing.glb tiefighter.glb
 *   tieinterceptor.glb tiebomber.glb falcon.glb
 *
 * They are read from the APK assets folder revival_cockpits/ on the headset
 * and from the revival_cockpits folder next to the game on PC. A craft with
 * no Revival cockpit, or a missing file, keeps XWA's cockpit.
 *
 * XWA_XR_COCKPIT_MODEL=xwa|revival  the VR option; read every frame.
 * XWA_XR_REVIVAL_COCKPITS=<dir>     another folder with the GLBs.
 * XWA_XR_REVIVAL_SCALE=<k>          size factor about the eye (default 1). */
#ifndef XWA_REMASTER_VR_REVIVAL_COCKPIT_H
#define XWA_REMASTER_VR_REVIVAL_COCKPIT_H

#include "aeron/render.h"
#include "aeron/scene/mesh.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Revival mesh replacing the XWA cockpit `cockpit_model_name` for the
 * player craft `object_type` (the TIE Interceptor picks its own model), or
 * NULL: XWA's cockpits chosen, no model for this one, or it failed to load
 * (then XWA's cockpit is drawn). Loads on first use. */
AeronSceneMesh* XwaVrRevivalCockpit_Mesh(AeronCommandBuffer* cmd, const char* cockpit_model_name,
										 uint16_t object_type);

/* Model matrix (row-major, model metres -> scene units) from the XWA
 * cockpit's own matrix for this frame (`cockpit_matrix`: the player craft's
 * basis in the OPT frame, x right, -y forward, z up), the GLB's eye axes
 * mapped onto it and its origin (the Revival eye point) moved to the eye
 * `camera_local`, times XWA_XR_REVIVAL_SCALE. The mapping is a reflection:
 * draw the mesh with front faces culled. */
void XwaVrRevivalCockpit_Matrix(const float cockpit_matrix[16], const float camera_local[3], float out[16]);

/* Name under which the VR cockpit HUD keeps its panel layout while a Revival
 * cockpit is shown ("Revival" + the model's file name, e.g. REVIVALXWING),
 * else `fallback`. */
const char* XwaVrRevivalCockpit_HudName(const char* fallback);

/* Frame start: no Revival cockpit drawn yet this frame. */
void XwaVrRevivalCockpit_BeginFrame(void);

void XwaVrRevivalCockpit_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
