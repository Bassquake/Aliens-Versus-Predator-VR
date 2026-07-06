#ifndef AVP_OPENGL_H
#define AVP_OPENGL_H

#include "kshape.h"

void InitOpenGL();
void ThisFramesRenderingHasBegun();
void ThisFramesRenderingHasFinished();
void D3D_SkyPolygon_Output(POLYHEADER *inputPolyPtr, RENDERVERTEX *renderVerticesPtr);
void D3D_DrawBackdrop();
void D3D_FadeDownScreen(int brightness, int colour);
void RenderString(char *stringPtr, int x, int y, int colour);
void RenderStringCentred(char *stringPtr, int centreX, int y, int colour);
void RenderStringVertically(char *stringPtr, int centreX, int bottomY, int colour);
void D3D_DecalSystem_Setup();
void D3D_DecalSystem_End();
void FlushRenderBuffer(void);
void SecondFlushD3DZBuffer();
void D3D_PlayerDamagedOverlay(int intensity);
void D3D_PredatorScreenInversionOverlay();
void D3D_ScreenInversionOverlay();
void D3D_DrawColourBar(int yTop, int yBottom, int rScale, int gScale, int bScale);
void InitGameShader(void);
void RestoreGameShaderState(void);
void OGL_RegenerateMipmaps(void);

#ifndef __ANDROID__
/* Desktop FSR 1 spatial upscaling. The in-game frame is rendered into a low-res
   FBO, then EASU-upscaled + RCAS-sharpened to the window at present time.
   Gated by FSRQualityIndex (0 = off → these are no-ops, native rendering). */
void FSR_SetOutputSize(int w, int h); /* window size; call on (re)size */
void FSR_BeginFrame(void);            /* bind low-res FBO before the scene renders */
void FSR_Resolve(void);               /* upscale low-res FBO to the backbuffer     */
void FSR_AbortFrame(void);            /* discard a pending FBO (e.g. menu present)  */
#endif

#ifdef __ANDROID__
/* Clip-space HUD controls — set during MaintainHUD() in VR, reset afterwards.
   vr_hud_clip_scale: < 1.0 shrinks toward centre (1.0 = no scale).
   vr_hud_offset_x/y: shift entire HUD left/right/up/down in clip space. */
extern float vr_hud_clip_scale;
extern float vr_hud_offset_x;
extern float vr_hud_offset_y;

/* Set GL viewport to 640x480 when in VR 2D mode so readback is 1:1. */
void VR_Set2DViewport(void);
/* Returns non-zero during 3D VR gameplay (eye FBOs active, not 2D menu/loading mode). */
int VR_IsIn3DMode(void);
/* HMD horizontal heading for locomotion — ONE_FIXED (65536) scale sin/cos of game yaw.
 * Updated each frame from xr_views; pmove.c uses these to rotate movement velocity. */
extern int xr_hmd_move_sin;
extern int xr_hmd_move_cos;
extern int xr_snap_yaw;

/* Game-space controller hand poses — computed in avpview.c before per-eye loop.
 * vr_right_hand_world/mat are in game world coordinates; ObMat is local-to-world.
 * Valid flags are 0 when the controller is not tracked. */
extern VECTORCH vr_right_hand_world;
extern MATRIXCH  vr_right_hand_mat;
extern int       vr_right_hand_valid;
extern VECTORCH vr_left_hand_world;
extern MATRIXCH  vr_left_hand_mat;
extern int       vr_left_hand_valid;

/* --- VR weapon / hand alignment tuning ----------------------------------
 * Nudge the weapon (and its held-hand geometry) relative to the physical
 * right controller. The three offsets are applied in the controller's LOCAL
 * frame, so they move/rotate with your hand. Units are game units
 * (GAME_UNITS_PER_METRE = 2200, so ~2.2 units = 1 mm; 300 units ~= 13 cm).
 *
 *   FORWARD  along the aim/barrel axis. NEGATIVE pulls the weapon back over
 *            the hand (this is the old "pullback"; -300 = the previous 13 cm).
 *   RIGHT    +right / -left across the grip.
 *   UP       +up / -down along the grip.
 *   PITCH_DEG  extra barrel tilt in degrees on top of the fixed +90 barrel
 *              alignment: +tips the muzzle down, -tips it up.
 *
 * Signs/axes depend on the runtime grip pose — if a nudge goes the "wrong"
 * way, flip its sign. Both the visible weapon (avpview.c) and the shot spawn
 * point (weapons.c) read these via VR_ComputeWeaponAnchor(), so they stay in
 * sync. Marine / Predator guns only (the Alien claw rig is placed separately).
 *
 * These four values are the shared DEFAULT. Each weapon sits slightly
 * differently in the hand, so per-weapon overrides live in the
 * vr_weapon_offset[] table in avpview.c — edit a weapon's row there to tune
 * just that gun; anything left at VR_WPN_DEFAULT uses the values below. */
#define VR_WEAPON_OFFSET_FORWARD  (-300)   /* was VR_WEAPON_PULLBACK = 300 */
#define VR_WEAPON_OFFSET_RIGHT    0
#define VR_WEAPON_OFFSET_UP       0
#define VR_WEAPON_PITCH_DEG       0

/* One weapon's alignment offsets (game units; pitch in degrees). Same meaning
 * as the VR_WEAPON_OFFSET_* defaults above. */
typedef struct { int forward, right, up, pitch_deg; } VR_WEAPON_OFFSET;

/* Fill *out_world / *out_mat with the controller-attached weapon transform for
 * weapon `weaponID` (right-controller pose + that weapon's offsets + barrel
 * alignment). Caller must have checked vr_right_hand_valid. */
void VR_ComputeWeaponAnchor(int weaponID, VECTORCH *out_world, MATRIXCH *out_mat);

/* --- Alien claw rig alignment tuning ------------------------------------
 * Same idea as the weapon offsets above, but a SEPARATE set: the claw rig is
 * placed from the eye/"Camera Root" offset (not a gun grip) and its model
 * orientation follows the controller directly with no barrel fix, so it needs
 * its own values. Offsets are in the controller local frame; PITCH_DEG tilts
 * the claws about the local X axis (+claws down / -claws up). Units and sign
 * conventions match the weapon offsets. Alien only. */
#define VR_CLAW_OFFSET_FORWARD  -900
#define VR_CLAW_OFFSET_RIGHT    -200
#define VR_CLAW_OFFSET_UP       0
#define VR_CLAW_PITCH_DEG       45
#endif

#endif
