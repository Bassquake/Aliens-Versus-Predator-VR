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
#endif

#endif
