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

/* Texture filtering settings, driven by the AV Options menu. All three are 0 by
   default, and 0 reproduces the port's behaviour from before they existed (see
   the note in opengl.c — the profile blob stores them in previously-zero bytes,
   so 0 has to mean "as before"). Applied to already-loaded textures by
   OGL_ApplyTextureFilterSettings, which ThisFramesRenderingHasBegun calls
   whenever one of them changes. */
extern int AnisotropicFilterIndex; /* 0=16x(default) 1=8x 2=4x 3=2x 4=off       */
extern int TextureFilterIndex;     /* 0=trilinear(default) 1=bilinear 2=nearest */
extern int NPOTMipmapsEnabled;     /* 0=off(default) 1=on                       */
void OGL_ApplyTextureFilterSettings(void);

#ifndef __ANDROID__
/* Desktop MSAA. The frame is rendered into a multisampled FBO at window
   resolution and blitted down onto the backbuffer at present time.
   Gated by MSAASampleIndex (0 = off → these are no-ops, native rendering).
   Replaced the FSR 1 upscaler, which hooked the same three points. */
void MSAA_SetOutputSize(int w, int h); /* window size; call on (re)size            */
void MSAA_BeginFrame(void);            /* bind the MS FBO before the scene renders */
void MSAA_Resolve(void);               /* resolve the MS FBO to the backbuffer     */
void MSAA_AbortFrame(void);            /* discard a pending FBO (e.g. menu present)*/
#endif

/* ---- VR controller bindings ------------------------------------------------
 *
 * Every remappable gameplay action, and every physical control it can be put on.
 * The consumer sites call VR_Action(VR_ACT_x) instead of reading a specific
 * xr_*_pressed global, so a binding change takes effect with no other plumbing.
 *
 * LEVEL vs EDGE is a property of the SOURCE, not of the action, and the two are
 * not interchangeable. The triggers, grips, A/B/Y and the stick clicks report
 * "held"; X and the stick up/down report a press EDGE (one frame per press).
 * Binding a hold-style action such as Jetpack to an edge source therefore gives a
 * single tick rather than continuous thrust. The defaults below keep every action
 * on the kind of source it was written for, which is why they match the original
 * hard-coded bindings exactly. */
enum VR_ACTION {
    VR_ACT_FIRE_PRIMARY = 0,
    VR_ACT_FIRE_SECONDARY,
    VR_ACT_JUMP,
    VR_ACT_CROUCH,
    VR_ACT_OPERATE,
    VR_ACT_VISION,
    VR_ACT_TAUNT,
    VR_ACT_SPECIAL,        /* Marine jetpack / Predator disc recall */
    VR_ACT_FLARE,          /* Marine flare, an edge action */
    VR_ACT_NEXT_WEAPON,
    VR_ACT_PREV_WEAPON,
    VR_ACT_COUNT
};

enum VR_SOURCE {
    VR_SRC_NONE = 0,
    VR_SRC_R_TRIGGER,
    VR_SRC_R_GRIP,
    VR_SRC_A,
    VR_SRC_B,
    VR_SRC_L_TRIGGER,
    VR_SRC_L_GRIP,
    VR_SRC_X,
    VR_SRC_Y,
    VR_SRC_L_STICK_CLICK,
    VR_SRC_R_STICK_UP,
    VR_SRC_R_STICK_DOWN,
    /* Appended, never inserted: a stored binding is a source INDEX, so putting a new
       one in the middle would silently re-point every saved binding after it. */
    VR_SRC_R_STICK_CLICK,
    VR_SRC_COUNT
};

/* Bindings are PER SPECIES: each plays differently enough to want its own layout,
   and the three menus mirror the per-species key configuration the flat game already
   has. Indexed [AvP.PlayerType][action] - I_Marine/I_Predator/I_Alien are 0/1/2, so
   the player type indexes this directly. Menu-editable, profile-stored. */
#define VR_SPECIES_COUNT 3
extern int VRBinding[VR_SPECIES_COUNT][VR_ACT_COUNT];

/* The same table as first shipped. The Controller Configuration menu marks whichever
   value matches it as "(Default)", so a player who has remapped something can always
   see what it started as. */
extern const int VRBindingDefault[VR_SPECIES_COUNT][VR_ACT_COUNT];

/* Current state of whatever is bound to this action. 0 when unbound. */
extern int VR_Action(int action);

/* World Scale, as a menu setting.
 *
 * The menu system's sliders are integer-only, so the setting is an INDEX and the scale
 * is derived from it: 1.00 + index*0.05, giving 1.00 .. 1.50 in 0.05 steps.
 * VR_WORLD_SCALE_DEFAULT_INDEX is 1.30. Keep the three in step - the range is stated
 * once here and everything else derives from it.
 *
 * The range was 0.10 .. 3.00 while it was being tuned. It is narrowed to the band that
 * is actually usable: below 1.00 the character/reach scaling stands down entirely (all
 * of it is gated on > 1.001) so the low half of the slider only shrank the player
 * against an unscaled world, and the far end was never playable.
 *
 * Changing MIN or STEP RE-MEANS EVERY STORED INDEX - the profile keeps the index, not
 * the value (VRWorldScaleIndexPlus1), so an old profile's number now decodes to a
 * different scale. The load-time range check in avp_userprofile.cpp catches anything
 * past the new maximum and falls back to the default; a stored index that still fits
 * silently becomes a different scale, which is the accepted cost of a range change. */
#define VR_WORLD_SCALE_MIN            1.00f
#define VR_WORLD_SCALE_STEP           0.05f
#define VR_WORLD_SCALE_MAX_INDEX      10        /* 1.00 + 10*0.05 = 1.50 */
#define VR_WORLD_SCALE_DEFAULT_INDEX  6         /* 1.00 +  6*0.05 = 1.30 */
#define VR_WorldScaleFromIndex(i)     (VR_WORLD_SCALE_MIN + (i) * VR_WORLD_SCALE_STEP)

extern int   VRWorldScaleIndex;   /* menu-editable, profile-stored */
extern float vr_world_scale;      /* what the eye pass actually multiplies by */

#ifdef AVP_XR
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
/* In-world hand tuner: set to 1 to build it in, 0 to leave it out entirely.
 *
 * A DEV TOOL, off by default so it cannot be reached in a shipped build. It edits
 * vr_weapon_offset[] and vr_left_hand_trim[] live from inside the game (toggle with
 * left stick click + B; right stick picks a field and changes it; the values are
 * drawn beside the frame rate and logged as VRTUNE lines). It PERSISTS NOTHING -
 * copy the logged rows back into the tables when you are happy with them.
 *
 * Turning it off compiles out the tuner, its HUD readout and its input handling; the
 * tables themselves stay exactly as written here. */

/* How long the weapon takes to ease between the hand-held pose and the released
 * (animated) pose - reloads, medicomp use, shoulder-cannon fire. Without it the
 * weapon jumps between the two in a single frame. */
#define VR_FREE_BLEND_SECS 0.18f

#define AVP_VR_HAND_TUNER 0

/* In-world WORLD SCALE tuner. Same idea and the same cost as the hand tuner above:
 * a dev tool for finding a number on-device, compiled out entirely when 0. Toggled
 * separately so both can be built independently. */
#define AVP_VR_WORLD_TUNER 0

#define VR_WEAPON_OFFSET_FORWARD  (-300)   /* was VR_WEAPON_PULLBACK = 300 */
#define VR_WEAPON_OFFSET_RIGHT    0
#define VR_WEAPON_OFFSET_UP       0
#define VR_WEAPON_PITCH_DEG       0
#define VR_WEAPON_ROLL_DEG        0
#define VR_WEAPON_YAW_DEG         0

/* One weapon's alignment offsets (game units; angles in degrees). Same meaning
 * as the VR_WEAPON_OFFSET_* defaults above.
 *
 * All three angles are about the WEAPON's own axes, applied after the barrel fix:
 *   pitch  tips the muzzle up          (about X, the weapon's right)
 *   roll   rotates the weapon clockwise about the barrel
 *   yaw    swings the muzzle outward   (about the weapon's up)
 * Every row of vr_weapon_offset[] spells out all six fields; a row that stops early
 * simply gets 0 for the rest, so shortened rows stay valid. */
typedef struct { int forward, right, up, pitch_deg, roll_deg, yaw_deg; } VR_WEAPON_OFFSET;

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
