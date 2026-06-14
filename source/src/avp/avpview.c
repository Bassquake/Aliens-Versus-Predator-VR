#ifdef __ANDROID__
#include <stdbool.h>
#include <stdint.h>
#include <jni.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <khronos/openxr/openxr.h>
#include <khronos/openxr/openxr_platform.h>

/* Mirror of VRSwapchain from main.c — keep in sync */
typedef struct {
    XrSwapchain                   swapchain;
    XrSwapchainImageOpenGLESKHR  *images;
    uint32_t                       image_count;
    XrExtent2Di                    size;
} VRSwapchain;

extern XrView     *xr_views;
extern uint32_t    view_count;
extern bool        xr_enabled;
extern int         xr_session_running;
extern VRSwapchain *vr_swapchains;
extern int ViewportWidth;
extern int ViewportHeight;
extern XrFrameState* VR_GetFrameState(void);
/* Swapchain helpers from main.c */
extern uint32_t VR_AcquireAndWaitSwapchainImage(int eye);
extern void     VR_ReleaseSwapchainImage(int eye);
extern GLuint   VR_GetSwapchainImageTexture(int eye, uint32_t idx);
/* Controller grip poses from main.c */
extern XrPosef xr_grip_pose_left;
extern XrPosef xr_grip_pose_right;
extern int     xr_grip_left_valid;
extern int     xr_grip_right_valid;
extern int     xr_trigger_right_pressed;      /* 1 while right trigger is held */
extern int     xr_grip_right_squeeze_pressed; /* 1 while right grip is squeezed */
extern void    XR_Haptic_Right(float amplitude, float duration_ms);

#endif

#include "3dc.h"

#include "inline.h"
#include "module.h"
#include "gamedef.h"
#include "stratdef.h"
#include "dynblock.h"
#include "bh_types.h"
#include "avpview.h"
#include "opengl.h"

#include "kshape.h"
#include "kzsort.h"
#include "frustum.h"
#include "vision.h"
#include "lighting.h"
#include "weapons.h"
#include "sfx.h"
#include "fmv.h"
#include "particle.h"
#include "sequnces.h"
/* character extents data so you know where the player's eyes are */
#include "extents.h"
#include "avp_userprofile.h"

#define UseLocalAssert Yes
#include "ourasert.h"

#ifdef __ANDROID__
#include <GLES3/gl3.h>
#include <SDL3/SDL.h>

/* Per-eye FBO resources — color attachment is the XR swapchain texture (attached per frame) */
static GLuint eye_fbo[2]      = {0, 0};
static GLuint eye_depth_rb[2] = {0, 0};
static int    eye_fbo_w       = 0;
static int    eye_fbo_h       = 0;

/* MSAA (anti-aliasing) for the per-eye 3D render. Uses
 * GL_EXT_multisampled_render_to_texture so the multisampled colour resolves
 * implicitly into the single-sample XR swapchain texture — tile-friendly on the
 * Quest GPU (no separate resolve blit). Falls back to no-MSAA when the extension
 * is absent or the setting is Off, in which case the render path is byte-for-byte
 * identical to before. */
extern int MSAASampleIndex;        /* menu setting: 0=off,1=2x,2=4x (main.c)  */
extern int MSAA_SampleCount(void); /* setting -> 0/2/4 sample count (main.c)  */

typedef void (*PFN_glRenderbufferStorageMultisampleEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*PFN_glFramebufferTexture2DMultisampleEXT)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
static PFN_glRenderbufferStorageMultisampleEXT  p_glRenderbufferStorageMultisampleEXT  = NULL;
static PFN_glFramebufferTexture2DMultisampleEXT p_glFramebufferTexture2DMultisampleEXT = NULL;
static int msaa_ext_checked   = 0;
static int msaa_ext_available = 0;
static int eye_fbo_samples    = 0;   /* sample count the eye FBOs were last built with */

static void VR_InitMSAAProcs(void)
{
    if (msaa_ext_checked) return;
    msaa_ext_checked = 1;

    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    if (exts && SDL_strstr(exts, "GL_EXT_multisampled_render_to_texture")) {
        p_glRenderbufferStorageMultisampleEXT =
            (PFN_glRenderbufferStorageMultisampleEXT)SDL_GL_GetProcAddress("glRenderbufferStorageMultisampleEXT");
        p_glFramebufferTexture2DMultisampleEXT =
            (PFN_glFramebufferTexture2DMultisampleEXT)SDL_GL_GetProcAddress("glFramebufferTexture2DMultisampleEXT");
        msaa_ext_available = (p_glRenderbufferStorageMultisampleEXT &&
                              p_glFramebufferTexture2DMultisampleEXT) ? 1 : 0;
    }
    SDL_Log("VR: MSAA (EXT_multisampled_render_to_texture) %s",
            msaa_ext_available ? "available" : "unavailable");
}

/* Effective sample count given the menu setting and extension support (0 = off). */
static int VR_EffectiveMSAASamples(void)
{
    VR_InitMSAAProcs();
    return msaa_ext_available ? MSAA_SampleCount() : 0;
}

/* Set to 1 during AvpShowViewsVR so kshape.c can skip the 4/3 Y-axis correction */
int vr_is_rendering = 0;
/* Set to 1 before a new game starts; AvpShowViewsVR resets the room-scale anchor
 * and computes xr_snap_yaw so the player starts facing the correct game direction. */
int vr_recalibrate = 1;
/* Game-logic camera position before per-eye IPD offset — used for LOS checks in VR. */
VECTORCH vr_base_world = {0, 0, 0};

#ifdef __ANDROID__
/* Game-space controller hand poses — updated each frame before the per-eye render loop.
 * Read by weapons.c to drive weapon/hand position from controller tracking. */
VECTORCH vr_right_hand_world = {0, 0, 0};
MATRIXCH  vr_right_hand_mat  = {ONE_FIXED,0,0, 0,ONE_FIXED,0, 0,0,ONE_FIXED};
int       vr_right_hand_valid = 0;
VECTORCH vr_left_hand_world = {0, 0, 0};
MATRIXCH  vr_left_hand_mat  = {ONE_FIXED,0,0, 0,ONE_FIXED,0, 0,0,ONE_FIXED};
int       vr_left_hand_valid = 0;
#endif

void UpdateCamera(void);

void VR_InitEyeFBOs(int w, int h)
{
    eye_fbo_w = w;
    eye_fbo_h = h;

    int samples = VR_EffectiveMSAASamples();
    eye_fbo_samples = samples;

    for (int i = 0; i < 2; i++) {
        if (eye_fbo[i])      { glDeleteFramebuffers(1,  &eye_fbo[i]);      eye_fbo[i]      = 0; }
        if (eye_depth_rb[i]) { glDeleteRenderbuffers(1, &eye_depth_rb[i]); eye_depth_rb[i] = 0; }

        /* Depth+stencil renderbuffer (multisampled to match the colour attachment
         * when MSAA is active — both must share the same sample count). */
        glGenRenderbuffers(1, &eye_depth_rb[i]);
        glBindRenderbuffer(GL_RENDERBUFFER, eye_depth_rb[i]);
        if (samples > 0)
            p_glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);

        /* FBO — color attachment is attached per-frame from the XR swapchain */
        glGenFramebuffers(1, &eye_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, eye_fbo[i]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, eye_depth_rb[i]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    SDL_Log("VR: Eye FBOs %dx%d ready (MSAA %dx)", w, h, samples);
}

/* Rebuild the eye FBOs if the MSAA menu setting changed since they were created.
 * Called once per frame before the per-eye render loop. Cheap no-op when unchanged. */
static void VR_UpdateEyeFBOMSAA(void)
{
    if (eye_fbo_w == 0) return; /* not initialised yet */
    if (VR_EffectiveMSAASamples() != eye_fbo_samples)
        VR_InitEyeFBOs(eye_fbo_w, eye_fbo_h);
}

/* Used by MOTIONBLUR path only (always-write for BSP painter's order). */
void VR_DepthBSP(void)
{
    if (!vr_is_rendering) return;
    FlushRenderBuffer();
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
}

/* Flush batched BSP, then switch to GL_LEQUAL + depth write for entity rendering.
   Entities test against BSP depth and write their own depth so they can occlude
   each other correctly. */
void VR_DepthObjects(void)
{
    if (!vr_is_rendering) return;
    FlushRenderBuffer();
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
}

/* Flush batched entities, then restore GL_LEQUAL + depth write for the next BSP module. */
void VR_AfterObjects(void)
{
    if (!vr_is_rendering) return;
    FlushRenderBuffer();
    glDepthMask(GL_TRUE);
}

#endif /* __ANDROID__ */

/* KJL 13:59:05 04/19/97 - avpview.c
 *
 *	This is intended to be an AvP-specific streamlined version of view.c. 
 */
																		
extern void AllNewModuleHandler(void);
extern SCREENDESCRIPTORBLOCK ScreenDescriptorBlock;

DISPLAYBLOCK *OnScreenBlockList[maxobjects];
int NumOnScreenBlocks;

extern DISPLAYBLOCK *ActiveBlockList[];
extern int NumActiveBlocks;

extern int ScanDrawMode;
/* JH 13/5/97 */
extern int DrawMode;
extern int ZBufferMode;

//extern DPID MultiplayerObservedPlayer;
extern int MultiplayerObservedPlayer;

#if SupportMorphing
MORPHDISPLAY MorphDisplay;
#endif

#if SupportModules
SCENEMODULE **Global_ModulePtr = 0;
MODULE *Global_MotherModule;
char *ModuleCurrVisArray = 0;
char *ModulePrevVisArray = 0;
char *ModuleTempArray = 0;
char *ModuleLocalVisArray = 0;
int ModuleArraySize = 0;
#endif

/* KJL 11:12:10 06/06/97 - orientation */
MATRIXCH LToVMat;
EULER LToVMat_Euler;
MATRIXCH WToLMat = {1,};
VECTORCH LocalView;

/* KJL 11:16:37 06/06/97 - lights */
VECTORCH LocalLightCH;
int NumLightSourcesForObject;
LIGHTBLOCK *LightSourcesForObject[MaxLightsPerObject];
int GlobalAmbience;
int LightScale=ONE_FIXED;
int DrawingAReflection;

int *Global_ShapePoints;
int **Global_ShapeItems;
int *Global_ShapeNormals;
int *Global_ShapeVNormals;
int **Global_ShapeTextures;
VIEWDESCRIPTORBLOCK *Global_VDB_Ptr;
DISPLAYBLOCK *Global_ODB_Ptr;
SHAPEHEADER *Global_ShapeHeaderPtr;
EXTRAITEMDATA *Global_EID_Ptr;
int *Global_EID_IPtr;


extern float CameraZoomScale;
extern int CameraZoomLevel;
int AlienBiteAttackInProgress;

/* phase for cloaked objects */
int CloakingPhase;
extern int NormalFrameTime;
extern int ShowFrameRate;

int LeanScale;
EULER deathTargetOrientation={0,0,0};

extern int GetSingleColourForPrimary(int Colour);
extern void ColourFillBackBuffer(int FillColour);

static void ModifyHeadOrientation(void);
int AVPViewVolumePlaneTest(CLIPPLANEBLOCK *cpb, DISPLAYBLOCK *dblockptr, int obr);



void UpdateRunTimeLights(void)
{
	extern int NumActiveBlocks;
	extern DISPLAYBLOCK *ActiveBlockList[];
	int numberOfObjects = NumActiveBlocks;

	while (numberOfObjects--)
	{
		DISPLAYBLOCK *dispPtr = ActiveBlockList[numberOfObjects];

		if( (dispPtr->SpecialFXFlags & SFXFLAG_ONFIRE)
		  ||((dispPtr->ObStrategyBlock)&&(dispPtr->ObStrategyBlock->SBDamageBlock.IsOnFire)) )
			AddLightingEffectToObject(dispPtr,LFX_OBJECTONFIRE);

		UpdateObjectLights(dispPtr);
	}

	HandleLightElementSystem();
}																			
void LightSourcesInRangeOfObject(DISPLAYBLOCK *dptr)
{

	DISPLAYBLOCK **aptr;
	DISPLAYBLOCK *dptr2;
	LIGHTBLOCK *lptr;
	VECTORCH llocal;
	int i, j;


	aptr = ActiveBlockList;


	NumLightSourcesForObject = 0;


	/*

	Light Sources attached to other objects

	*/

	for(i = NumActiveBlocks;
		i!=0 && NumLightSourcesForObject < MaxLightsPerObject; i--) {

		dptr2 = *aptr++;

		if(dptr2->ObNumLights) {

			for(j = 0; j < dptr2->ObNumLights
				&& NumLightSourcesForObject < MaxLightsPerObject; j++) {

				lptr = dptr2->ObLights[j];

				if (!lptr->LightBright || !(lptr->RedScale||lptr->GreenScale||lptr->BlueScale))
				{
					 continue;
				}

				if ((CurrentVisionMode == VISION_MODE_IMAGEINTENSIFIER) && (lptr->LightFlags & LFlag_PreLitSource))
					 continue;
//				lptr->LightFlags |= LFlag_NoSpecular;

		   		if(!(dptr->ObFlags3 & ObFlag3_PreLit &&
					lptr->LightFlags & LFlag_PreLitSource))
				{
					{
						VECTORCH vertexToLight;
						int distanceToLight;

						if (DrawingAReflection)
						{
							vertexToLight.vx = (MirroringAxis - lptr->LightWorld.vx) - dptr->ObWorld.vx;
						}
						else
						{
							vertexToLight.vx = lptr->LightWorld.vx - dptr->ObWorld.vx;
						}
						vertexToLight.vy = lptr->LightWorld.vy - dptr->ObWorld.vy;
						vertexToLight.vz = lptr->LightWorld.vz - dptr->ObWorld.vz;

						distanceToLight = Approximate3dMagnitude(&vertexToLight);

						#if 0
						if (CurrentVisionMode == VISION_MODE_IMAGEINTENSIFIER)
							distanceToLight /= 2;
						#endif

						if(distanceToLight < (lptr->LightRange + dptr->ObRadius) )
						{

							LightSourcesForObject[NumLightSourcesForObject] = lptr;
							NumLightSourcesForObject++;

							/* Transform the light position to local space */

							llocal = vertexToLight;

							RotateAndCopyVector(&llocal, &lptr->LocalLP, &WToLMat);

						}


					}

				}

			}

		}

	}

	{
		extern LIGHTELEMENT LightElementStorage[];
		extern int NumActiveLightElements;
		int i = NumActiveLightElements;
		LIGHTELEMENT *lightElementPtr = LightElementStorage;
		while(i--)
		{
			LIGHTBLOCK *lptr = &(lightElementPtr->LightBlock);
			VECTORCH vertexToLight;
			int distanceToLight;

			vertexToLight.vx = lptr->LightWorld.vx - dptr->ObWorld.vx;
			vertexToLight.vy = lptr->LightWorld.vy - dptr->ObWorld.vy;
			vertexToLight.vz = lptr->LightWorld.vz - dptr->ObWorld.vz;

			distanceToLight = Approximate3dMagnitude(&vertexToLight);

			#if 0
			if (CurrentVisionMode == VISION_MODE_IMAGEINTENSIFIER)
				distanceToLight /= 2;
			#endif

			if(distanceToLight < (lptr->LightRange + dptr->ObRadius) )
			{

				LightSourcesForObject[NumLightSourcesForObject] = lptr;
				NumLightSourcesForObject++;

				/* Transform the light position to local space */
				llocal = vertexToLight;
				RotateAndCopyVector(&llocal, &lptr->LocalLP, &WToLMat);

			}

			lightElementPtr++;
		}
	}
}

int LightIntensityAtPoint(VECTORCH *pointPtr)
{
	int intensity = 0;
	int i, j;
	
	DISPLAYBLOCK **activeBlockListPtr = ActiveBlockList;
	for(i = NumActiveBlocks; i != 0; i--) {
		DISPLAYBLOCK *dispPtr = *activeBlockListPtr++;
		
		if (dispPtr->ObNumLights) {
			for(j = 0; j < dispPtr->ObNumLights; j++) {
				LIGHTBLOCK *lptr = dispPtr->ObLights[j];
				VECTORCH disp = lptr->LightWorld;
				int dist;
				
				disp.vx -= pointPtr->vx;
				disp.vy -= pointPtr->vy;
				disp.vz -= pointPtr->vz;
				
				dist = Approximate3dMagnitude(&disp);
				
				if (dist<lptr->LightRange) {
					intensity += WideMulNarrowDiv(lptr->LightBright,lptr->LightRange-dist,lptr->LightRange);
				}
			}
		}
	}
	if (intensity>ONE_FIXED) intensity=ONE_FIXED;
	else if (intensity<GlobalAmbience) intensity=GlobalAmbience;
	
	/* KJL 20:31:39 12/1/97 - limit how dark things can be so blood doesn't go green */
	if (intensity<10*256) intensity = 10*256;

	return intensity;
}

EULER HeadOrientation = {0,0,0};

static void ModifyHeadOrientation(void)
{
	extern int NormalFrameTime;
	#define TILT_THRESHOLD 128
	PLAYER_STATUS *playerStatusPtr;
    
	/* get the player status block ... */
	playerStatusPtr = (PLAYER_STATUS *) (Player->ObStrategyBlock->SBdataptr);
    GLOBALASSERT(playerStatusPtr);
  
    if (!playerStatusPtr->IsAlive && !MultiplayerObservedPlayer)
	{
		int decay = NormalFrameTime>>6;
		
		HeadOrientation.EulerX &= 4095;
	   	HeadOrientation.EulerX -= decay;
		if(HeadOrientation.EulerX < 3072)
			HeadOrientation.EulerX = 3072;

	}
	else
	{
		int decay = NormalFrameTime>>8;
		if(HeadOrientation.EulerX > 2048)
		{
			if (HeadOrientation.EulerX < 4096 - TILT_THRESHOLD)
				HeadOrientation.EulerX = 4096 - TILT_THRESHOLD;

		   	HeadOrientation.EulerX += decay;
			if(HeadOrientation.EulerX > 4095)
				HeadOrientation.EulerX =0;
		}
		else
		{
			if (HeadOrientation.EulerX > TILT_THRESHOLD)
				HeadOrientation.EulerX = TILT_THRESHOLD;

		   	HeadOrientation.EulerX -= decay;
			if(HeadOrientation.EulerX < 0)
				HeadOrientation.EulerX =0;
		}

		if(HeadOrientation.EulerY > 2048)
		{
			if (HeadOrientation.EulerY < 4096 - TILT_THRESHOLD)
				HeadOrientation.EulerY = 4096 - TILT_THRESHOLD;

		   	HeadOrientation.EulerY += decay;
			if(HeadOrientation.EulerY > 4095)
				HeadOrientation.EulerY =0;
		}
		else
		{
			if (HeadOrientation.EulerY > TILT_THRESHOLD)
				HeadOrientation.EulerY = TILT_THRESHOLD;

		   	HeadOrientation.EulerY -= decay;
			if(HeadOrientation.EulerY < 0)
				HeadOrientation.EulerY =0;
		}
		
		if(HeadOrientation.EulerZ > 2048)
		{
			if (HeadOrientation.EulerZ < 4096 - TILT_THRESHOLD)
				HeadOrientation.EulerZ = 4096 - TILT_THRESHOLD;

		   	HeadOrientation.EulerZ += decay;
			if(HeadOrientation.EulerZ > 4095)
				HeadOrientation.EulerZ =0;
		}
		else
		{
			if (HeadOrientation.EulerZ > TILT_THRESHOLD)
				HeadOrientation.EulerZ = TILT_THRESHOLD;

		   	HeadOrientation.EulerZ -= decay;
			if(HeadOrientation.EulerZ < 0)
				HeadOrientation.EulerZ =0;
		}
	}
}

void InteriorType_Body()
{
	DISPLAYBLOCK *subjectPtr = Player;
	extern int NormalFrameTime;

	static int verticalSpeed = 0;
	static int zAxisTilt=0;
	STRATEGYBLOCK *sbPtr;
	DYNAMICSBLOCK *dynPtr;
	
	sbPtr = subjectPtr->ObStrategyBlock;
	LOCALASSERT(sbPtr);
	dynPtr = sbPtr->DynPtr;	
	LOCALASSERT(dynPtr);
    
	ModifyHeadOrientation();
	{
		/* eye offset */
		VECTORCH ioff;
		COLLISION_EXTENTS *extentsPtr = 0;
		PLAYER_STATUS *playerStatusPtr= (PLAYER_STATUS *) (sbPtr->SBdataptr);

		switch(AvP.PlayerType)
		{
			case I_Marine:
				extentsPtr = &CollisionExtents[CE_MARINE];
				break;
				
			case I_Alien:
				extentsPtr = &CollisionExtents[CE_ALIEN];
				break;
			
			case I_Predator:
				extentsPtr = &CollisionExtents[CE_PREDATOR];
				break;
		}
		
		/* set player state */
		if (playerStatusPtr->ShapeState == PMph_Standing)
		{
			ioff.vy = extentsPtr->StandingTop;
		}
		else
		{
			ioff.vy = extentsPtr->CrouchingTop;
		}

		if (LANDOFTHEGIANTS_CHEATMODE)
		{
			ioff.vy/=4;
		}
		if (!playerStatusPtr->IsAlive && !MultiplayerObservedPlayer)
		{
			extern int deathFadeLevel;
			
			ioff.vy = MUL_FIXED(deathFadeLevel*4-3*ONE_FIXED,ioff.vy);

			if (ioff.vy>-100)
			{
				ioff.vy = -100;
			}
		}

				
		ioff.vx = 0;
		ioff.vz = 0;//-extentsPtr->CollisionRadius*2;
		ioff.vy += verticalSpeed/16+200;

		RotateVector(&ioff, &subjectPtr->ObMat);
		AddVector(&ioff, &Global_VDB_Ptr->VDB_World);
		
		#if 0
		{
			static int i=-10;
			i=-i;
			ioff.vx = MUL_FIXED(GetSin((CloakingPhase/5)&4095),i);
			ioff.vy = MUL_FIXED(GetCos((CloakingPhase/3)&4095),i);
			ioff.vz = 0;

			RotateVector(&ioff, &subjectPtr->ObMat);
			AddVector(&ioff, &Global_VDB_Ptr->VDB_World);


		}
		#endif
	}
	{
		EULER orientation;
		MATRIXCH matrix;

		orientation = HeadOrientation;

	  orientation.EulerZ += (zAxisTilt>>8);
	  orientation.EulerZ &= 4095;
		
		if (NAUSEA_CHEATMODE)
		{
			orientation.EulerZ = (orientation.EulerZ+GetSin((CloakingPhase/2)&4095)/256)&4095;
			orientation.EulerX = (orientation.EulerX+GetSin((CloakingPhase/2+500)&4095)/512)&4095;
			orientation.EulerY = (orientation.EulerY+GetSin((CloakingPhase/3+800)&4095)/512)&4095;
		}
		// The next test drops the matrix multiply if the orientation is close to zero
		// There is an inaccuracy problem with the Z angle at this point
					 
		if (orientation.EulerX != 0 || orientation.EulerY != 0 || 
					(orientation.EulerZ > 1 && orientation.EulerZ <	4095))
		{
			CreateEulerMatrix(&orientation, &matrix);
			MatrixMultiply(&Global_VDB_Ptr->VDB_Mat, &matrix, &Global_VDB_Ptr->VDB_Mat);
	 	}

	}
	
	{
		VECTORCH relativeVelocity;
		
		/* get subject's total velocity */
		{
			MATRIXCH worldToLocalMatrix;

			/* make world to local matrix */
			worldToLocalMatrix = subjectPtr->ObMat;
			TransposeMatrixCH(&worldToLocalMatrix);													   

			relativeVelocity.vx = dynPtr->Position.vx - dynPtr->PrevPosition.vx;		
			relativeVelocity.vy = dynPtr->Position.vy - dynPtr->PrevPosition.vy;
			relativeVelocity.vz = dynPtr->Position.vz - dynPtr->PrevPosition.vz;
			/* rotate into object space */

			RotateVector(&relativeVelocity,&worldToLocalMatrix);
		}	 
		
		{
			int targetingSpeed = 10*NormalFrameTime;
	
			/* KJL 14:08:50 09/20/96 - the targeting is FRI, but care has to be taken
			   at very low frame rates to ensure that you can't overshoot */
			if (targetingSpeed > 65536)	targetingSpeed=65536;
					
			zAxisTilt += MUL_FIXED
				(
					DIV_FIXED
					(
						MUL_FIXED(relativeVelocity.vx,LeanScale),
						NormalFrameTime
					)-zAxisTilt,
					targetingSpeed
				);

			{
				static int previousVerticalSpeed = 0;
				int difference;

				if (relativeVelocity.vy >= 0)
				{ 
					difference = DIV_FIXED
					(
						previousVerticalSpeed - relativeVelocity.vy,
						NormalFrameTime
					);
				}
				else difference = 0;

				if (verticalSpeed < difference) verticalSpeed = difference;
				
			 	if(verticalSpeed > 150*16) verticalSpeed = 150*16;
				
				verticalSpeed -= NormalFrameTime>>2;
				if (verticalSpeed < 0) verticalSpeed = 0;				
				
				previousVerticalSpeed = relativeVelocity.vy;
			}
	 	}
	}
}

void UpdateCamera(void)
{
	PLAYER_STATUS *playerStatusPtr= (PLAYER_STATUS *) (Player->ObStrategyBlock->SBdataptr);
	int cos = GetCos(playerStatusPtr->ViewPanX);
	int sin = GetSin(playerStatusPtr->ViewPanX);
	MATRIXCH mat;
	DISPLAYBLOCK *dptr_s = Player;

	Global_VDB_Ptr->VDB_World = dptr_s->ObWorld;
	Global_VDB_Ptr->VDB_Mat = dptr_s->ObMat;

	mat.mat11 = ONE_FIXED;		 
	mat.mat12 = 0;
	mat.mat13 = 0;
	mat.mat21 = 0;	  	
	mat.mat22 = cos;	  	
	mat.mat23 = -sin;	  	
	mat.mat31 = 0;	  	
	mat.mat32 = sin;	  	
	mat.mat33 = cos;	  	
 	MatrixMultiply(&Global_VDB_Ptr->VDB_Mat,&mat,&Global_VDB_Ptr->VDB_Mat);

		
	InteriorType_Body();
}

void AVPGetInViewVolumeList(VIEWDESCRIPTORBLOCK *VDB_Ptr)
{
	DISPLAYBLOCK **activeblocksptr;
	int t;
	#if (SupportModules && SupportMultiCamModules)
	int MVis;
	#endif

	/* Initialisation */
	NumOnScreenBlocks = 0;

	/* Scan the Active Blocks List */
	activeblocksptr = &ActiveBlockList[0];

	for(t = NumActiveBlocks; t!=0; t--)
	{
		DISPLAYBLOCK *dptr = *activeblocksptr++;
	
		if (dptr==Player) continue;
		MVis = Yes;
		if(dptr->ObMyModule)
		{
			MODULE *mptr = dptr->ObMyModule;
			if(ModuleCurrVisArray[mptr->m_index] != 2) MVis = No;
			else
			{
				extern int NumberOfLandscapePolygons;
				SHAPEHEADER *shapePtr = GetShapeData(dptr->ObShape);
				NumberOfLandscapePolygons+=shapePtr->numitems;
			}

		}
		if (!(dptr->ObFlags&ObFlag_NotVis) && MVis) 
		{
			MakeVector(&dptr->ObWorld, &VDB_Ptr->VDB_World, &dptr->ObView);
			RotateVector(&dptr->ObView, &VDB_Ptr->VDB_Mat);

			/* Screen Test */
			#if MIRRORING_ON
			if (MirroringActive || dptr->HModelControlBlock || dptr->SfxPtr)
			{
				OnScreenBlockList[NumOnScreenBlocks++] = dptr;
			}
			else if (ObjectWithinFrustrum(dptr))
			{
				OnScreenBlockList[NumOnScreenBlocks++] = dptr;
			}
			#else
			if(dptr->SfxPtr || dptr->HModelControlBlock || ObjectWithinFrustrum(dptr))
			{
				OnScreenBlockList[NumOnScreenBlocks++] = dptr;
			}
			else
			{
				if(dptr->HModelControlBlock)
				{
					DoHModelTimer(dptr->HModelControlBlock);
				}
			}
			#endif
		}
		
	}
}

void ReflectObject(DISPLAYBLOCK *dPtr)
{
	dPtr->ObWorld.vx = MirroringAxis - dPtr->ObWorld.vx;
	dPtr->ObMat.mat11 = -dPtr->ObMat.mat11;
	dPtr->ObMat.mat21 = -dPtr->ObMat.mat21;
	dPtr->ObMat.mat31 = -dPtr->ObMat.mat31;
}

void CheckIfMirroringIsRequired(void);
void AvpShowViews(void)
{
	FlushD3DZBuffer();

	UpdateAllFMVTextures();


	/* Update attached object positions and orientations etc. */
	UpdateCamera();

	/* Initialise the global VMA */
//	GlobalAmbience=655;
//	textprint("Global Ambience: %d\n",GlobalAmbience);

	/* Prepare the View Descriptor Block for use in ShowView() */

	PrepareVDBForShowView(Global_VDB_Ptr);
	PlatformSpecificShowViewEntry(Global_VDB_Ptr, &ScreenDescriptorBlock);
	TranslationSetup();

	{
		extern void ThisFramesRenderingHasBegun(void);
		ThisFramesRenderingHasBegun();
		D3D_DrawBackdrop();
	}

	/* Now we know where the camera is, update the modules */

	#if SupportModules
	AllNewModuleHandler();
//	ModuleHandler(Global_VDB_Ptr);
	#endif

	#if MIRRORING_ON
	CheckIfMirroringIsRequired();
	#endif

	/* Do lights */
	UpdateRunTimeLights();
	if (AvP.PlayerType==I_Alien)
	{
		MakeLightElement(&Player->ObWorld,LIGHTELEMENT_ALIEN_TEETH);
		MakeLightElement(&Player->ObWorld,LIGHTELEMENT_ALIEN_TEETH2);
	}

//	GlobalAmbience=ONE_FIXED/4;
	/* Find out which objects are in the View Volume */
	AVPGetInViewVolumeList(Global_VDB_Ptr);

	if (AlienBiteAttackInProgress)
	{
		CameraZoomScale += (float)NormalFrameTime/65536.0f;
		if (CameraZoomScale > 1.0f)
		{
			AlienBiteAttackInProgress = 0;
			CameraZoomScale = 1.0f;
		}
	}

	/* update players weapon */
	UpdateWeaponStateMachine();
	/* lights associated with the player may have changed */
	UpdateObjectLights(Player);


	if(NumOnScreenBlocks)
	{
	 	/* KJL 12:13:26 02/05/97 - divert rendering for AvP */
		KRenderItems(Global_VDB_Ptr);
	}

	PlatformSpecificShowViewExit(Global_VDB_Ptr, &ScreenDescriptorBlock);

	#if SupportZBuffering
	if ((ScanDrawMode != ScanDrawDirectDraw) &&	(ZBufferMode != ZBufferOff))
	{
		/* KJL 10:25:44 7/23/97 - this offset is used to push back the normal game gfx,
		so that the HUD can be drawn over the top without sinking into walls, etc. */
		HeadUpDisplayZOffset = 0;
	}
	#endif
}

void InitCameraValues(void)
{
	extern VIEWDESCRIPTORBLOCK *ActiveVDBList[];
	Global_VDB_Ptr = ActiveVDBList[0];

	HeadOrientation.EulerX = 0;
	HeadOrientation.EulerY = 0;
	HeadOrientation.EulerZ = 0;

	CameraZoomScale = 1.0f;
	CameraZoomLevel=0;
}



/*

 Prepare the View Descriptor Block for use in ShowView() and others.

 If there is a display block attached to the view, update the view location
 and orientation.

*/

void PrepareVDBForShowView(VIEWDESCRIPTORBLOCK *VDB_Ptr)
{
	EULER e;

	
	/* Get the View Object Matrix, transposed */
 	TransposeMatrixCH(&VDB_Ptr->VDB_Mat);

	/* Get the Matrix Euler Angles */
	MatrixToEuler(&VDB_Ptr->VDB_Mat, &VDB_Ptr->VDB_MatrixEuler);
	
	/* Get the Matrix Euler Angles */
	MatrixToEuler(&VDB_Ptr->VDB_Mat, &e);

	/* Create the "sprite" matrix" */
	e.EulerX = 0;
	e.EulerY = 0;
	e.EulerZ = (-e.EulerZ) & wrap360;
	
	CreateEulerMatrix(&e, &VDB_Ptr->VDB_SpriteMat);
}

   
/*

 This function updates the position and orientation of the lights attached
 to an object.

 It must be called after the object has completed its movements in a frame,
 prior to the call to the renderer.

*/

void UpdateObjectLights(DISPLAYBLOCK *dptr)
{

	int i;
	LIGHTBLOCK *lptr;
	LIGHTBLOCK **larrayptr = &dptr->ObLights[0];


	for(i = dptr->ObNumLights; i!=0; i--)
	{
		/* Get a light */
		lptr = *larrayptr++;

		/* Calculate the light's location */
		if(!(lptr->LightFlags & LFlag_AbsPos))
		{
			CopyVector(&dptr->ObWorld, &lptr->LightWorld);
     	}
		LOCALASSERT(lptr->LightRange!=0);
		lptr->BrightnessOverRange = DIV_FIXED(MUL_FIXED(lptr->LightBright,LightScale),lptr->LightRange);
	}
	

}














/****************************************************************************/

/*

 Find out which light sources are in range of the object.

*/




/*

 Initialise the Renderer

*/

void InitialiseRenderer(void)
{
	InitialiseObjectBlocks();
	InitialiseStrategyBlocks();

	InitialiseTxAnimBlocks();

	InitialiseLightBlocks();
	InitialiseVDBs();

	/* KJL 14:46:42 09/09/98 */
	InitialiseLightIntensityStamps();
}





/*

 General View Volume Test for Objects and Sub-Object Trees

 This function returns returns "Yes" / "True" for an if()

*/

int AVPViewVolumeTest(VIEWDESCRIPTORBLOCK *VDB_Ptr, DISPLAYBLOCK *dblockptr)
{
	int obr = dblockptr->ObRadius;

	/* Perform the view volume plane tests */

	if(
	AVPViewVolumePlaneTest(&VDB_Ptr->VDB_ClipZPlane, dblockptr, obr) &&
	AVPViewVolumePlaneTest(&VDB_Ptr->VDB_ClipLeftPlane, dblockptr, obr) &&
	AVPViewVolumePlaneTest(&VDB_Ptr->VDB_ClipRightPlane, dblockptr, obr) &&
	AVPViewVolumePlaneTest(&VDB_Ptr->VDB_ClipUpPlane, dblockptr, obr) &&
	AVPViewVolumePlaneTest(&VDB_Ptr->VDB_ClipDownPlane, dblockptr, obr))
		return Yes;

	else
		return No;

}
/*

 View Volume Plane Test

 Make the ODB VSL relative to the VDB Clip Plane POP and dot the resultant
 vector with the Clip Plane Normal.

*/

int AVPViewVolumePlaneTest(CLIPPLANEBLOCK *cpb, DISPLAYBLOCK *dblockptr, int obr)
{
	VECTORCH POPRelObView;

	MakeVector(&dblockptr->ObView, &cpb->CPB_POP, &POPRelObView);

	if(DotProduct(&POPRelObView, &cpb->CPB_Normal) < obr) return Yes;
	else return No;
}


#if MIRRORING_ON
void CheckIfMirroringIsRequired(void)
{
	extern char LevelName[];
	extern MODULE * playerPherModule;

	MirroringActive = 0;
	#if 0
	if ( (!stricmp(LevelName,"e3demo")) || (!stricmp(LevelName,"e3demosp")) )
	{
		int numOfObjects = NumActiveBlocks;

		while(numOfObjects)
		{
			DISPLAYBLOCK *objectPtr = ActiveBlockList[--numOfObjects];
			MODULE *modulePtr = objectPtr->ObMyModule;

			/* if it's a module, which isn't inside another module */
			if (modulePtr && modulePtr->name)
			{
				if(!stricmp(modulePtr->name,"marine01b"))
				{
					if(ModuleCurrVisArray[modulePtr->m_index] == 2)
					{
						MirroringActive = 1;
						MirroringAxis = -149*2;
						break;
					}
				}
			}
		}
	
		if (playerPherModule && playerPherModule->name)
		{
			textprint("<%s>\n",playerPherModule->name);
			if((!stricmp(playerPherModule->name,"predator"))
			 ||(!stricmp(playerPherModule->name,"predator01"))
			 ||(!stricmp(playerPherModule->name,"predator03"))
			 ||(!stricmp(playerPherModule->name,"predator02")) )
			{
				MirroringActive = 1;
				MirroringAxis = -7164*2;
			}
		}
	}
	else
	#endif 
	#if 1
	if (!stricmp(LevelName,"derelict"))
	{
		if (playerPherModule && playerPherModule->name)
		{
			if((!stricmp(playerPherModule->name,"start"))
			 ||(!stricmp(playerPherModule->name,"start-en01")) )
			{
				MirroringActive = 1;
				MirroringAxis = -5596*2;
			}
		}
	}
	#endif
}
#endif

#define MinChangeInXSize 8
void MakeViewingWindowSmaller(void)
{
	extern VIEWDESCRIPTORBLOCK *Global_VDB_Ptr;
	int MinChangeInYSize = (ScreenDescriptorBlock.SDB_Height*MinChangeInXSize)/ScreenDescriptorBlock.SDB_Width;
	
	if (Global_VDB_Ptr->VDB_ClipLeft<ScreenDescriptorBlock.SDB_Width/2-16)
	{
		Global_VDB_Ptr->VDB_ClipLeft +=MinChangeInXSize;
		Global_VDB_Ptr->VDB_ClipRight -=MinChangeInXSize;
		Global_VDB_Ptr->VDB_ClipUp +=MinChangeInYSize;
		Global_VDB_Ptr->VDB_ClipDown -=MinChangeInYSize;
	}
	if(AvP.PlayerType == I_Alien)
	{
		Global_VDB_Ptr->VDB_ProjX = (Global_VDB_Ptr->VDB_ClipRight - Global_VDB_Ptr->VDB_ClipLeft)/4;
		Global_VDB_Ptr->VDB_ProjY = (Global_VDB_Ptr->VDB_ClipDown - Global_VDB_Ptr->VDB_ClipUp)/4;
	}
	else
	{
		Global_VDB_Ptr->VDB_ProjX = (Global_VDB_Ptr->VDB_ClipRight - Global_VDB_Ptr->VDB_ClipLeft)/2;
		Global_VDB_Ptr->VDB_ProjY = (Global_VDB_Ptr->VDB_ClipDown - Global_VDB_Ptr->VDB_ClipUp)/2;
	}
	//BlankScreen(); 
}

void MakeViewingWindowLarger(void)
{
	extern VIEWDESCRIPTORBLOCK *Global_VDB_Ptr;
	int MinChangeInYSize = (ScreenDescriptorBlock.SDB_Height*MinChangeInXSize)/ScreenDescriptorBlock.SDB_Width;

	if (Global_VDB_Ptr->VDB_ClipLeft>0)
	{
		Global_VDB_Ptr->VDB_ClipLeft -=MinChangeInXSize;
		Global_VDB_Ptr->VDB_ClipRight +=MinChangeInXSize;
		Global_VDB_Ptr->VDB_ClipUp -=MinChangeInYSize;
		Global_VDB_Ptr->VDB_ClipDown +=MinChangeInYSize;
	}
	if(AvP.PlayerType == I_Alien)
	{
		Global_VDB_Ptr->VDB_ProjX = (Global_VDB_Ptr->VDB_ClipRight - Global_VDB_Ptr->VDB_ClipLeft)/4;
		Global_VDB_Ptr->VDB_ProjY = (Global_VDB_Ptr->VDB_ClipDown - Global_VDB_Ptr->VDB_ClipUp)/4;
	}
	else
	{
		Global_VDB_Ptr->VDB_ProjX = (Global_VDB_Ptr->VDB_ClipRight - Global_VDB_Ptr->VDB_ClipLeft)/2;
		Global_VDB_Ptr->VDB_ProjY = (Global_VDB_Ptr->VDB_ClipDown - Global_VDB_Ptr->VDB_ClipUp)/2;
	}
}


extern void AlienBiteAttackHasHappened(void)
{
	extern int AlienTongueOffset;
	extern int AlienTeethOffset;

	AlienBiteAttackInProgress = 1;

	CameraZoomScale = 0.25f;
	AlienTongueOffset = ONE_FIXED;
	AlienTeethOffset = 0;
}

#ifdef __ANDROID__

int vr_eye_index = 0; /* 0 = first/left eye, 1 = second/right; read by MaintainHUD */

void AvpShowViewsVR(void)
{
    extern SDL_GLContext context;
    SDL_GLContext current = SDL_GL_GetCurrentContext();

#if !defined(NDEBUG)
    while (glGetError() != GL_NO_ERROR) {}
#endif
    RestoreGameShaderState();

    if (!xr_enabled || !xr_session_running || view_count == 0 || vr_swapchains == NULL) {
        AvpShowViews();
        return;
    }

    /* xr_views already located by VR_WaitAndBeginFrame() in main.c */
    extern void VR_InvalidateTextureCache(void);

#define GAME_UNITS_PER_METRE 2200
    /* GLCHECK calls glGetError() which forces a CPU↔GPU pipeline sync on tiled
     * mobile GPUs (Adreno on Quest) — gate to debug builds only. */
#if defined(NDEBUG)
    #define GLCHECK(label) ((void)0)
#else
    #define GLCHECK(label) do { GLenum _e = glGetError(); if (_e) SDL_Log("GL err %s: 0x%x", label, _e); } while(0)
#endif

    GLint saved_vp[4];
    glGetIntegerv(GL_VIEWPORT, saved_vp);

    UpdateCamera();
    VECTORCH base_world = Global_VDB_Ptr->VDB_World;
    MATRIXCH  base_mat  = Global_VDB_Ptr->VDB_Mat;
    vr_base_world = base_world;

    UpdateAllFMVTextures();

#if !defined(NDEBUG)
    while (glGetError() != GL_NO_ERROR) {}
#endif

    FlushD3DZBuffer();
    ThisFramesRenderingHasBegun();
    VR_InvalidateTextureCache();
#if MIRRORING_ON
    CheckIfMirroringIsRequired();
#endif

    /* Head centre — used for room-scale X/Z reference. */
    float eye_mid_x = 0.0f, eye_mid_y = 0.0f, eye_mid_z = 0.0f;
    if (view_count >= 2) {
        eye_mid_x = (xr_views[0].pose.position.x + xr_views[1].pose.position.x) * 0.5f;
        eye_mid_y = (xr_views[0].pose.position.y + xr_views[1].pose.position.y) * 0.5f;
        eye_mid_z = (xr_views[0].pose.position.z + xr_views[1].pose.position.z) * 0.5f;
    }

    /* Capture the head's STAGE-space X/Z origin once so room-scale X/Z deltas are
     * applied relative to where the player started, not the room-track origin.
     * Y is not captured here — it uses absolute STAGE height for floor alignment. */
    static float ref_head_x = 0.0f, ref_head_y = 0.0f, ref_head_z = 0.0f;
    static bool  ref_captured = false;
    if (vr_recalibrate) {
        ref_captured  = false;
        vr_recalibrate = 0;
    }
    if (!ref_captured && view_count > 0) {
        ref_head_x   = eye_mid_x;
        ref_head_y   = eye_mid_y;
        ref_head_z   = eye_mid_z;
        ref_captured = true;

        /* Align VR heading: set xr_snap_yaw so the player's current physical
         * forward maps to the game's intended starting facing direction.
         *
         * Physical HMD forward in game-space XZ (game X = OpenXR X, game Z = -OpenXR Z):
         *   fwd_x = q*(0,0,-1).x = -2*(qx*qz + qw*qy)
         *   fwd_z = -(q*(0,0,-1).z) = 1 - 2*(qx^2 + qy^2)
         *
         * Game matrix convention: mat13 = -sin(yaw), mat33 = cos(yaw)
         * so game_yaw = atan2(-mat13, mat33).
         *
         * Snap formula: effective_yaw = physical_yaw + snap_yaw
         * → snap_yaw = game_yaw - physical_yaw */
        if (view_count >= 1) {
            float qx = xr_views[0].pose.orientation.x;
            float qy = xr_views[0].pose.orientation.y;
            float qz = xr_views[0].pose.orientation.z;
            float qw = xr_views[0].pose.orientation.w;
            float fwd_x = -2.0f * (qx * qz + qw * qy);
            float fwd_z =  1.0f - 2.0f * (qx * qx + qy * qy);
            float mag = SDL_sqrtf(fwd_x * fwd_x + fwd_z * fwd_z);
            if (mag > 0.001f) { fwd_x /= mag; fwd_z /= mag; }
            float phys_yaw_rad  = SDL_atan2f(fwd_x, fwd_z);
            float game_yaw_rad  = SDL_atan2f(-(float)base_mat.mat13, (float)base_mat.mat33);
            int phys_yaw_units  = (int)(phys_yaw_rad * 2048.0f / SDL_PI_F);
            int game_yaw_units  = (int)(game_yaw_rad * 2048.0f / SDL_PI_F);
            xr_snap_yaw = (game_yaw_units - phys_yaw_units) & 4095;
            SDL_Log("VR heading calib: phys=%.1f° game=%.1f° snap_yaw=%d",
                    phys_yaw_rad * 180.0f / SDL_PI_F,
                    game_yaw_rad * 180.0f / SDL_PI_F,
                    xr_snap_yaw);
        }
    }

    /* Scale STAGE Y so the user's physical standing height maps to the game
     * character's eye height.  Player->ObWorld is the feet (Bottom=0 in extents),
     * base_world is the eye; their difference is the in-game eye-to-floor distance.
     * Dividing by ref_head_y (user's physical standing height in metres) gives a
     * units-per-metre scale that makes both the VR floor and game scale feel correct. */
    int   game_eye_to_floor = Player->ObWorld.vy - base_world.vy; /* game units, > 0 upright */
    float vr_y_scale = (ref_head_y > 0.01f && game_eye_to_floor > 0)
        ? ((float)game_eye_to_floor / ref_head_y)
        : (float)GAME_UNITS_PER_METRE;

    /* Reset hand pose validity; updated below from controller tracking. */
    vr_right_hand_valid = 0;
    vr_left_hand_valid  = 0;

    /* Convert a grip XrPosef to game world coordinates.
     * Same convention as eye poses: X=OpenXR X, Y=-OpenXR Y, Z=-OpenXR Z;
     * quatx negated for handedness; snap_yaw applied to X/Z delta. */
    #define GRIP_TO_GAME(pose, valid_flag, out_world, out_mat) \
    do { \
        float gdx = (pose).position.x - ref_head_x; \
        float gdz = (pose).position.z - ref_head_z; \
        if (xr_snap_yaw != 0) { \
            float snap_rad = (float)xr_snap_yaw * SDL_PI_F / 2048.0f; \
            float snap_s = SDL_sinf(snap_rad), snap_c = SDL_cosf(snap_rad); \
            float rdx = gdx * snap_c - gdz * snap_s; \
            float rdz = gdx * snap_s + gdz * snap_c; \
            gdx = rdx; gdz = rdz; \
        } \
        (out_world).vx = base_world.vx + (int)(gdx * vr_y_scale); \
        (out_world).vy = Player->ObWorld.vy - (int)((pose).position.y * vr_y_scale); \
        (out_world).vz = base_world.vz - (int)(gdz * vr_y_scale); \
        QUAT gq; \
        gq.quatw =  (int)((pose).orientation.w * ONE_FIXED); \
        gq.quatx = -(int)((pose).orientation.x * ONE_FIXED); \
        gq.quaty =  (int)((pose).orientation.y * ONE_FIXED); \
        gq.quatz =  (int)((pose).orientation.z * ONE_FIXED); \
        MATRIXCH gm; \
        QuatToMat(&gq, &gm); \
        if (xr_snap_yaw != 0) { \
            int snap_s = GetSin(xr_snap_yaw), snap_c = GetCos(xr_snap_yaw); \
            MATRIXCH snap_mat; \
            snap_mat.mat11 =  snap_c; snap_mat.mat12 = 0;         snap_mat.mat13 = -snap_s; \
            snap_mat.mat21 = 0;       snap_mat.mat22 = ONE_FIXED; snap_mat.mat23 = 0; \
            snap_mat.mat31 =  snap_s; snap_mat.mat32 = 0;         snap_mat.mat33 =  snap_c; \
            MATRIXCH snapped; \
            MatrixMultiply(&snap_mat, &gm, &snapped); \
            gm = snapped; \
        } \
        (out_mat) = gm; \
        (valid_flag) = 1; \
    } while(0)

    if (xr_grip_right_valid)
        GRIP_TO_GAME(xr_grip_pose_right, vr_right_hand_valid, vr_right_hand_world, vr_right_hand_mat);
    if (xr_grip_left_valid)
        GRIP_TO_GAME(xr_grip_pose_left, vr_left_hand_valid, vr_left_hand_world, vr_left_hand_mat);

    #undef GRIP_TO_GAME

    vr_is_rendering = 1;

    /* FPS counter — updated every 0.5 s to avoid flicker.
       Format: "60/72" = measured game fps / headset target hz from OpenXR. */
    static Uint64 fps_freq    = 0;
    static Uint64 fps_last    = 0;
    static int    fps_count   = 0;
    static float  fps_current = 0.0f;
    char fps_str[24];
    if (!fps_freq) fps_freq = SDL_GetPerformanceFrequency();
    Uint64 fps_now = SDL_GetPerformanceCounter();
    fps_count++;
    if (!fps_last) fps_last = fps_now;
    if ((fps_now - fps_last) >= fps_freq / 2) {
        fps_current = fps_count * (float)fps_freq / (float)(fps_now - fps_last);
        fps_count = 0;
        fps_last  = fps_now;
    }
    {
        XrFrameState *fs = VR_GetFrameState();
        float target_hz = (fs && fs->predictedDisplayPeriod > 0)
                          ? (1000000000.0f / (float)fs->predictedDisplayPeriod)
                          : 0.0f;
        SDL_snprintf(fps_str, sizeof(fps_str), "%.0f fps/%.0f Hz", fps_current, target_hz);
    }

    /* VR has ~50° half-FOV; the hardcoded NORM frustum uses 45° and incorrectly
       culls objects near the edge, especially from the IPD-offset eye. WIDE uses
       63.4° which comfortably covers any current VR headset FOV. */
    enum FrustrumType saved_frustrum = GetFrustrumType();
    SetFrustrumType(FRUSTRUM_TYPE_WIDE);
    /* Apply any change to the MSAA setting before rendering this frame's eyes. */
    VR_UpdateEyeFBOMSAA();
    for (int eye = 0; eye < (int)view_count; eye++) {

        /* X/Z: room-scale delta relative to where we started (ref_head_x/z).
         * Y: absolute STAGE height — Player->ObWorld.vy is the game floor (feet),
         *    so camera = game_floor - physical_eye_height, aligning game floor with
         *    the VR environment floor regardless of game character height.
         * After a snap turn the X/Z delta is rotated into game coordinates. */
        float phys_dx = xr_views[eye].pose.position.x - ref_head_x;
        float phys_dz = xr_views[eye].pose.position.z - ref_head_z;
        if (xr_snap_yaw != 0) {
            float snap_rad = (float)xr_snap_yaw * SDL_PI_F / 2048.0f;
            float snap_s   = SDL_sinf(snap_rad);
            float snap_c   = SDL_cosf(snap_rad);
            /* phys_dz is in OpenXR space (+Z = backward), game +Z = -OpenXR Z.
             * Convert to game coords before rotating: gx=phys_dx, gz=-phys_dz. */
            float rdx =  phys_dx * snap_c - phys_dz * snap_s;
            float rdz =  phys_dx * snap_s + phys_dz * snap_c;
            phys_dx = rdx;
            phys_dz = rdz;
        }
        Global_VDB_Ptr->VDB_World.vx = base_world.vx
            + (int)(phys_dx * vr_y_scale);
        Global_VDB_Ptr->VDB_World.vy = Player->ObWorld.vy
            - (int)(xr_views[eye].pose.position.y * vr_y_scale);
        Global_VDB_Ptr->VDB_World.vz = base_world.vz
            - (int)(phys_dz * vr_y_scale);

        QUAT q;
        q.quatw =  (int)(xr_views[eye].pose.orientation.w * ONE_FIXED);
        q.quatx = -(int)(xr_views[eye].pose.orientation.x * ONE_FIXED);
        q.quaty =  (int)(xr_views[eye].pose.orientation.y * ONE_FIXED);
        q.quatz =  (int)(xr_views[eye].pose.orientation.z * ONE_FIXED);
        QuatToMat(&q, &Global_VDB_Ptr->VDB_Mat);

        /* Post-multiply by snap rotation in world space.
         * MatrixMultiply(A,B,C) -> C = B*A, so this gives VDB_Mat * snap_mat.
         * After PrepareVDBForShowView transposes VDB_Mat, rendering applies
         * snap_mat in world space before camera, preserving head-tracking axes. */
        if (xr_snap_yaw != 0) {
            int snap_s = GetSin(xr_snap_yaw);
            int snap_c = GetCos(xr_snap_yaw);
            MATRIXCH snap_mat;
            snap_mat.mat11 =  snap_c; snap_mat.mat12 = 0;         snap_mat.mat13 = -snap_s;
            snap_mat.mat21 = 0;       snap_mat.mat22 = ONE_FIXED; snap_mat.mat23 = 0;
            snap_mat.mat31 =  snap_s; snap_mat.mat32 = 0;         snap_mat.mat33 =  snap_c;
            MATRIXCH snapped;
            MatrixMultiply(&snap_mat, &Global_VDB_Ptr->VDB_Mat, &snapped);
            Global_VDB_Ptr->VDB_Mat = snapped;
        }

        /* Acquire this eye's swapchain image and attach as FBO color target */
        Uint32 sc_img_idx = VR_AcquireAndWaitSwapchainImage(eye);
        if (sc_img_idx == UINT32_MAX) continue; /* image not ready — skip this eye, don't attach an invalid texture */
        GLuint sc_tex     = VR_GetSwapchainImageTexture(eye, sc_img_idx);
        glBindFramebuffer(GL_FRAMEBUFFER, eye_fbo[eye]);
        if (eye_fbo_samples > 0)
            /* Multisampled render straight into the swapchain texture; the tiler
             * resolves implicitly when the FBO is unbound at end of the eye pass. */
            p_glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                   GL_TEXTURE_2D, sc_tex, 0, eye_fbo_samples);
        else
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, sc_tex, 0);
        glViewport(0, 0, eye_fbo_w, eye_fbo_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* Save and override SDB */
        SCREENDESCRIPTORBLOCK saved_sdb = ScreenDescriptorBlock;
        ScreenDescriptorBlock.SDB_Width    = eye_fbo_w;
        ScreenDescriptorBlock.SDB_Height   = eye_fbo_h;
        ScreenDescriptorBlock.SDB_CentreX  = eye_fbo_w / 2;
        ScreenDescriptorBlock.SDB_CentreY  = eye_fbo_h / 2;
        ScreenDescriptorBlock.SDB_ClipLeft = 0;
        ScreenDescriptorBlock.SDB_ClipRight= eye_fbo_w;
        ScreenDescriptorBlock.SDB_ClipUp   = 0;
        ScreenDescriptorBlock.SDB_ClipDown = eye_fbo_h;

        /* Save and override VDB */
        int saved_ClipLeft  = Global_VDB_Ptr->VDB_ClipLeft;
        int saved_ClipRight = Global_VDB_Ptr->VDB_ClipRight;
        int saved_ClipUp    = Global_VDB_Ptr->VDB_ClipUp;
        int saved_ClipDown  = Global_VDB_Ptr->VDB_ClipDown;
        int saved_ProjX     = Global_VDB_Ptr->VDB_ProjX;
        int saved_ProjY     = Global_VDB_Ptr->VDB_ProjY;
        int saved_CentreX   = Global_VDB_Ptr->VDB_CentreX;
        int saved_CentreY   = Global_VDB_Ptr->VDB_CentreY;

        Global_VDB_Ptr->VDB_ClipLeft  = 0;
        Global_VDB_Ptr->VDB_ClipRight = eye_fbo_w;
        Global_VDB_Ptr->VDB_ClipUp    = 0;
        Global_VDB_Ptr->VDB_ClipDown  = eye_fbo_h;
        Global_VDB_Ptr->VDB_CentreX   = eye_fbo_w / 2;
        Global_VDB_Ptr->VDB_CentreY   = eye_fbo_h / 2;
        float tan_hx = (SDL_tanf(SDL_fabsf(xr_views[eye].fov.angleLeft))
                       + SDL_tanf(SDL_fabsf(xr_views[eye].fov.angleRight))) * 0.5f;
        float tan_hy = (SDL_tanf(SDL_fabsf(xr_views[eye].fov.angleUp))
                       + SDL_tanf(SDL_fabsf(xr_views[eye].fov.angleDown))) * 0.5f;
        Global_VDB_Ptr->VDB_ProjX = (tan_hx > 0.01f) ? (int)((eye_fbo_w * 0.5f) / tan_hx) : eye_fbo_w / 4;
        Global_VDB_Ptr->VDB_ProjY = (tan_hy > 0.01f) ? (int)((eye_fbo_h * 0.5f) / tan_hy) : eye_fbo_h / 4;

        PrepareVDBForShowView(Global_VDB_Ptr);
        PlatformSpecificShowViewEntry(Global_VDB_Ptr, &ScreenDescriptorBlock);
        TranslationSetup();
		RestoreGameShaderState();

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        D3D_DrawBackdrop();                             GLCHECK("DrawBackdrop");
        AllNewModuleHandler();                          GLCHECK("AllNewModuleHandler");
        /* Light updates and weapon state run once per frame (eye 0 only).
         * Running them per-eye causes:
         *   - LightBlockDeallocation() to remove the muzzle flash after eye 0,
         *     then UpdateWeaponStateMachine() re-adds it with a different FastRandom()
         *     value → asymmetric flash brightness between eyes.
         *   - Light element LifeTime decremented twice per frame → elements die too fast.
         *   - Weapon animation stepping twice per frame → double speed. */
        if (eye == 0) {
            UpdateRunTimeLights();
            if (AvP.PlayerType == I_Alien) {
                MakeLightElement(&Player->ObWorld, LIGHTELEMENT_ALIEN_TEETH);
                MakeLightElement(&Player->ObWorld, LIGHTELEMENT_ALIEN_TEETH2);
            }
        }
        AVPGetInViewVolumeList(Global_VDB_Ptr);
        if (eye == 0) {
            /* Inject right-trigger primary fire before the weapon state machine reads it. */
            {
                extern int RealFrameTime;
                static int haptic_ms_remaining = 0;
                static int prev_trigger = 0;
                if (xr_trigger_right_pressed) {
                    PLAYER_STATUS *ps = (PLAYER_STATUS *)Player->ObStrategyBlock->SBdataptr;
                    ps->Mvt_InputRequests.Flags.Rqst_FirePrimaryWeapon = 1;
                    haptic_ms_remaining -= RealFrameTime;
                    if (!prev_trigger || haptic_ms_remaining <= 0) {
                        XR_Haptic_Right(0.7f, 80.0f);
                        haptic_ms_remaining = 80;
                    }
                } else {
                    haptic_ms_remaining = 0;
                }
                prev_trigger = xr_trigger_right_pressed;
            }
            /* Inject right-grip secondary fire. */
            {
                extern int RealFrameTime;
                static int sec_haptic_ms_remaining = 0;
                static int prev_squeeze = 0;
                if (xr_grip_right_squeeze_pressed) {
                    PLAYER_STATUS *ps = (PLAYER_STATUS *)Player->ObStrategyBlock->SBdataptr;
                    ps->Mvt_InputRequests.Flags.Rqst_FireSecondaryWeapon = 1;
                    sec_haptic_ms_remaining -= RealFrameTime;
                    if (!prev_squeeze || sec_haptic_ms_remaining <= 0) {
                        XR_Haptic_Right(0.5f, 80.0f);
                        sec_haptic_ms_remaining = 80;
                    }
                } else {
                    sec_haptic_ms_remaining = 0;
                }
                prev_squeeze = xr_grip_right_squeeze_pressed;
            }
            /* VR aiming: set GunMuzzleSightX/Y from the physical controller aim direction so
             * CalculateWhereGunIsPointing (called inside UpdateWeaponStateMachine) derives the
             * correct GunMuzzleDirectionInWS for raycasting.  Without this, they default to 0
             * (top-left of the screen) because HandleMarineWeapon is skipped in VR.
             *
             * After the Rx(+90°) barrel fix, weapon local Z (forward) = old grip local Y,
             * which is row 2 of vr_right_hand_mat: (mat21, mat22, mat23).
             * Rotate that world-space direction to view space, then invert the projection
             * formula used by CalculateWhereGunIsPointing to get the sight position. */
            if (vr_right_hand_valid &&
                (AvP.PlayerType == I_Marine || AvP.PlayerType == I_Predator)) {
                extern int GunMuzzleSightX, GunMuzzleSightY;
                VECTORCH aim_vs;
                aim_vs.vx = vr_right_hand_mat.mat21;
                aim_vs.vy = vr_right_hand_mat.mat22;
                aim_vs.vz = vr_right_hand_mat.mat23;
                RotateVector(&aim_vs, &Global_VDB_Ptr->VDB_Mat);   /* world → view */
                if (aim_vs.vz > 0) {
                    int px = Global_VDB_Ptr->VDB_ProjX;
                    int py = Global_VDB_Ptr->VDB_ProjY;
                    int vz = aim_vs.vz;
                    /* Invert CalculateWhereGunIsPointing:
                     *   vx = (SightX - SDB_Width<<15) / ProjX
                     *   vy = ((SightY - SDB_Height<<15) / ProjY) * 3/4  (note 3/4 fudge) */
                    GunMuzzleSightX = (aim_vs.vx * px / vz) * ONE_FIXED
                                    + (ScreenDescriptorBlock.SDB_Width  << 15);
                    GunMuzzleSightY = (aim_vs.vy * py * 4 / (vz * 3)) * ONE_FIXED
                                    + (ScreenDescriptorBlock.SDB_Height << 15);
                }
            }
            UpdateWeaponStateMachine();
        }
        UpdateObjectLights(Player);
        if (NumOnScreenBlocks) KRenderItems(Global_VDB_Ptr);

        /* Flush remaining world geometry. */
        FlushRenderBuffer();
        glDepthMask(GL_TRUE);

        /* Particles, light flares, and decals for this eye.
           Rendered before the weapon with depth write disabled: light halos appear
           on top of world geometry, but the weapon (rendered next) paints over them
           so halos cannot bleed through the player's hands or held weapon. */
        glDepthMask(GL_FALSE);
        if (eye == 0)
            HandleParticleSystem();
        else
            RenderParticlesOnly();
        FlushRenderBuffer();
        glDepthMask(GL_TRUE);

        /* In VR, HandleMarineWeapon/HandlePredatorWeapon are skipped by MaintainHUD
         * (which itself is skipped in VR) to avoid doubling up the screen-space 2D
         * overlay. Render the weapon here instead, once per eye, positioned at the
         * physical right controller grip relative to this eye's tracking position.
         * Rendered after particles so the weapon naturally occludes any light halos. */
        if (AvP.PlayerType == I_Marine || AvP.PlayerType == I_Predator || AvP.PlayerType == I_Alien) {
            extern DISPLAYBLOCK PlayersWeapon;
            extern void RenderThisDisplayblock(DISPLAYBLOCK *dbPtr);
            extern DISPLAYBLOCK PlayersWeaponMuzzleFlash;

            /* The Alien's "weapon" is its claws (WEAPON_ALIEN_CLAW): a first-person
             * arm/claw HModel whose "Camera Root" bone is the eye (see
             * GetHierarchicalWeapon in weapons.c), so its visible claws are offset
             * from the model root by PlayersWeaponCameraOffset. We controller-attach
             * it like the Marine/Predator guns (see the is_alien branch below), but
             * the gun-specific barrel pitch / muzzle flash / idle-freeze don't apply. */
            const int is_alien = (AvP.PlayerType == I_Alien);

            /* Fetch weapon state once; used for recoil shake and muzzle flash. */
            PLAYER_STATUS *ps = (PLAYER_STATUS *)Player->ObStrategyBlock->SBdataptr;
            PLAYER_WEAPON_DATA *wpn = &ps->WeaponSlot[ps->SelectedWeaponSlot];
            TEMPLATE_WEAPON_DATA *tw = &TemplateWeapon[wpn->WeaponIDNumber];

            if (PlayersWeapon.ObShape || PlayersWeapon.HModelControlBlock) {
                if (is_alien) {
                    /* The claw rig is a first-person HModel: its visible claws are
                     * offset from the model root by PlayersWeaponCameraOffset (the
                     * "Camera Root" bone == the eye in the original first-person view).
                     * To controller-attach it like the Marine/Predator guns, we anchor
                     * the rig to the right-controller frame instead of the camera frame,
                     * reusing that same offset so the claws sit relative to the hand the
                     * way they sat relative to the eye.
                     *
                     * Derivation mirrors the working head-locked placement (the else
                     * branch), which sets root_world = VDB_World + RotateVector(off, ObMat)
                     * with ObMat = transpose(VDB_Mat).  Swapping the camera frame for the
                     * controller frame gives ObMat = vr_right_hand_mat and
                     * root_world = vr_right_hand_world + RotateVector(off, vr_right_hand_mat).
                     * ObView is then the world->view transform of root_world for this eye. */
                    extern VECTORCH PlayersWeaponCameraOffset;
                    VECTORCH off = PlayersWeaponCameraOffset;
                    off.vx += tw->RestPosition.vx + wpn->PositionOffset.vx;
                    off.vy += tw->RestPosition.vy + wpn->PositionOffset.vy;
                    off.vz += tw->RestPosition.vz + wpn->PositionOffset.vz;

                    if (vr_right_hand_valid) {
                        /* Controller-attached: rig root = hand + off rotated into the
                         * controller frame; orientation follows the controller. */
                        VECTORCH rootw = off;
                        RotateVector(&rootw, &vr_right_hand_mat);
                        rootw.vx += vr_right_hand_world.vx;
                        rootw.vy += vr_right_hand_world.vy;
                        rootw.vz += vr_right_hand_world.vz;
                        PlayersWeapon.ObWorld = rootw;
                        VECTORCH ov;
                        ov.vx = rootw.vx - Global_VDB_Ptr->VDB_World.vx;
                        ov.vy = rootw.vy - Global_VDB_Ptr->VDB_World.vy;
                        ov.vz = rootw.vz - Global_VDB_Ptr->VDB_World.vz;
                        RotateVector(&ov, &Global_VDB_Ptr->VDB_Mat);
                        PlayersWeapon.ObView = ov;
                        PlayersWeapon.ObMat  = vr_right_hand_mat;
                    } else {
                        /* Head-locked fallback when the controller pose is unavailable,
                         * so the claws never vanish. Mirrors PositionPlayersWeapon()'s
                         * melee branch, per eye. */
                        PlayersWeapon.ObView = off;
                        PlayersWeapon.ObMat  = Global_VDB_Ptr->VDB_Mat;
                        TransposeMatrixCH(&PlayersWeapon.ObMat);
                        VECTORCH wo = off;
                        RotateVector(&wo, &PlayersWeapon.ObMat);
                        PlayersWeapon.ObWorld.vx = Global_VDB_Ptr->VDB_World.vx + wo.vx;
                        PlayersWeapon.ObWorld.vy = Global_VDB_Ptr->VDB_World.vy + wo.vy;
                        PlayersWeapon.ObWorld.vz = Global_VDB_Ptr->VDB_World.vz + wo.vz;
                    }
                } else if (vr_right_hand_valid) {
                    /* Pull the weapon anchor back along the aim direction so the model
                     * sits over the physical controller rather than ahead of it.
                     * mat21/22/23 of vr_right_hand_mat is the barrel/aim direction in world space.
                     * 300 game units ≈ 13 cm (GAME_UNITS_PER_METRE = 2200). Increase to pull further back. */
                    const int VR_WEAPON_PULLBACK = 300;
                    VECTORCH anchor;
                    anchor.vx = vr_right_hand_world.vx - ((vr_right_hand_mat.mat21 * VR_WEAPON_PULLBACK) >> 16);
                    anchor.vy = vr_right_hand_world.vy - ((vr_right_hand_mat.mat22 * VR_WEAPON_PULLBACK) >> 16);
                    anchor.vz = vr_right_hand_world.vz - ((vr_right_hand_mat.mat23 * VR_WEAPON_PULLBACK) >> 16);
                    PlayersWeapon.ObWorld = anchor;
                    /* View-space position for HModel path: world-to-view of
                     * (anchor - eye_world).  RotateVector(v, VDB_Mat) = VDB_Mat^T * v
                     * which is exactly the ViewMatrix rotation (world → view). */
                    VECTORCH diff;
                    diff.vx = anchor.vx - Global_VDB_Ptr->VDB_World.vx;
                    diff.vy = anchor.vy - Global_VDB_Ptr->VDB_World.vy;
                    diff.vz = anchor.vz - Global_VDB_Ptr->VDB_World.vz;
                    RotateVector(&diff, &Global_VDB_Ptr->VDB_Mat);
                    PlayersWeapon.ObView = diff;
                    /* Orientation: reuse GRIP_TO_GAME result (snap-compensated). */
                    PlayersWeapon.ObMat = vr_right_hand_mat;
                    /* Barrel fix: weapon model barrel along local +Y; Rx(+90°)
                     * left-multiplied pitches it forward to align with view -Z. */
                    {
                        MATRIXCH m = PlayersWeapon.ObMat;
                        PlayersWeapon.ObMat.mat21 = -m.mat31;
                        PlayersWeapon.ObMat.mat22 = -m.mat32;
                        PlayersWeapon.ObMat.mat23 = -m.mat33;
                        PlayersWeapon.ObMat.mat31 =  m.mat21;
                        PlayersWeapon.ObMat.mat32 =  m.mat22;
                        PlayersWeapon.ObMat.mat33 =  m.mat23;
                    }
                    /* Recoil shake: StateDependentMovement computes PositionOffset
                     * in camera view space each game tick.  Rotate it to world space
                     * and add it on top of the controller anchor so the weapon
                     * visibly kicks on each shot without drifting from the hand. */
                    if (wpn->PositionOffset.vx | wpn->PositionOffset.vy | wpn->PositionOffset.vz) {
                        MATRIXCH inv_view = Global_VDB_Ptr->VDB_Mat;
                        TransposeMatrixCH(&inv_view);
                        VECTORCH recoil = { wpn->PositionOffset.vx,
                                            wpn->PositionOffset.vy,
                                            wpn->PositionOffset.vz };
                        RotateVector(&recoil, &inv_view);
                        PlayersWeapon.ObWorld.vx += recoil.vx;
                        PlayersWeapon.ObWorld.vy += recoil.vy;
                        PlayersWeapon.ObWorld.vz += recoil.vz;
                        PlayersWeapon.ObView.vx += wpn->PositionOffset.vx;
                        PlayersWeapon.ObView.vy += wpn->PositionOffset.vy;
                        PlayersWeapon.ObView.vz += wpn->PositionOffset.vz;
                    }
                }
                /* Freeze HModel bone animation during idle poses only: suppresses
                 * walk/sway on weapon but lets fire/reload sub-sequences play.
                 * Rules:
                 *   - Never freeze during tweening — the tween timer must advance
                 *     or the transition to the fire sequence never completes.
                 *   - Marine/Predator sub-sequence enums share integer values so
                 *     the idle check must be gated on AvP.PlayerType. */
                int saved_ti = 0;
                if (!is_alien && PlayersWeapon.HModelControlBlock) {
                    HMODELCONTROLLER *hmc = PlayersWeapon.HModelControlBlock;
                    saved_ti = hmc->timer_increment;
                    int is_idle = 0;
                    if (hmc->Tweening == Controller_NoTweening) {
                        int sq = hmc->Sub_Sequence;
                        if (AvP.PlayerType == I_Marine)
                            is_idle = (sq == (int)MHSS_Stationary || sq == (int)MHSS_Fidget
                                    || sq == (int)MHSS_Come      || sq == (int)MHSS_Go
                                    || sq == (int)MHSS_Right_Out || sq == (int)MHSS_Left_Out);
                        else
                            is_idle = (sq == (int)PHSS_Stand || sq == (int)PHSS_Run
                                    || sq == (int)PHSS_Come  || sq == (int)PHSS_Go);
                    }
                    if (is_idle)
                        hmc->timer_increment = 0;
                }
                RenderThisDisplayblock(&PlayersWeapon);
                if (!is_alien && PlayersWeapon.HModelControlBlock)
                    PlayersWeapon.HModelControlBlock->timer_increment = saved_ti;

                /* Muzzle flash: PositionPlayersWeaponMuzzleFlash reads the barrel
                 * bone world position (PWMFSDP->World_Offset) set by DoHModel
                 * above — must follow RenderThisDisplayblock.
                 * DrawMuzzleFlash is an immediate GL draw (not a queued particle)
                 * so it must be called once per eye. */
                int firing =
                    (wpn->CurrentState == WEAPONSTATE_FIRING_PRIMARY)
                    || ((wpn->WeaponIDNumber == WEAPON_MARINE_PISTOL
                      || wpn->WeaponIDNumber == WEAPON_TWO_PISTOLS)
                        && wpn->CurrentState == WEAPONSTATE_FIRING_SECONDARY);
                if (tw->MuzzleFlashShapeName && !tw->PrimaryIsMeleeWeapon && firing) {
                    PositionPlayersWeaponMuzzleFlash();
                    if (AvP.PlayerType == I_Marine) {
                        VECTORCH dir = { PlayersWeaponMuzzleFlash.ObMat.mat31,
                                         PlayersWeaponMuzzleFlash.ObMat.mat32,
                                         PlayersWeaponMuzzleFlash.ObMat.mat33 };
                        enum MUZZLE_FLASH_ID fid =
                            (wpn->WeaponIDNumber == WEAPON_SMARTGUN)
                                ? MUZZLE_FLASH_SMARTGUN
                            : (wpn->WeaponIDNumber == WEAPON_FRISBEE_LAUNCHER)
                                ? MUZZLE_FLASH_SKEETER
                            : MUZZLE_FLASH_AMORPHOUS;
                        DrawMuzzleFlash(&PlayersWeaponMuzzleFlash.ObWorld, &dir, fid);
                    } else {
                        RenderThisDisplayblock(&PlayersWeaponMuzzleFlash);
                    }
                }
            }
        }

        PlatformSpecificShowViewExit(Global_VDB_Ptr, &ScreenDescriptorBlock);
        HeadUpDisplayZOffset = 0;

        /* Head-locked HUD: render into the eye FBO while still bound.
           Use a VR-specific virtual SDB smaller than the 640×480 window SDB so
           HUD elements (tracker/health/ammo) land in the comfortable central zone
           of the eye FOV rather than at the extreme periphery.
           The GL viewport is still eye_fbo_w × eye_fbo_h so clip [-1,1] maps to
           the full eye FBO — only the virtual coordinate space shrinks. */
        {
            SCREENDESCRIPTORBLOCK hud_sdb = saved_sdb;
            hud_sdb.SDB_Width    = 640;
            hud_sdb.SDB_Height   = 680;
            hud_sdb.SDB_CentreX  = 320;
            hud_sdb.SDB_CentreY  = 340;
            hud_sdb.SDB_ClipLeft  = 0;
            hud_sdb.SDB_ClipRight = 0;
            hud_sdb.SDB_ClipUp    = 0;
            hud_sdb.SDB_ClipDown  = 0;
            ScreenDescriptorBlock = hud_sdb;
        }
        /* ── VR HUD tuning ──────────────────────────────────────────
           vr_hud_clip_scale  : overall size  (1.0 = full, 0.5 = half)
           vr_hud_offset_x    : base shift left/right  (+0.1 = right)
           vr_hud_offset_y    : shift up/down           (+0.1 = up)
           VR_HUD_STEREO_DEPTH: per-eye convergence offset that places the
                                HUD at a perceived depth rather than infinity.
                                Larger = closer. 0.0 = optical infinity.
                                0.014 ≈ 2 m, 0.02 ≈ 1.5 m, 0.028 ≈ 1 m    */
        #define VR_HUD_STEREO_DEPTH 0.02f
        vr_hud_clip_scale = 0.50f;
        vr_hud_offset_x   = (eye == 0) ? +VR_HUD_STEREO_DEPTH : -VR_HUD_STEREO_DEPTH;
        vr_hud_offset_y   = -0.15f;
        vr_eye_index = eye;
        /* Recompute GunMuzzleSightX/Y in HUD-pixel space so the crosshair drawn by
         * MaintainHUD lands at the same clip-space position as the 3D aim point.
         *
         * D3D_HUDQuad_Output maps HUD pixel (x,y) to clip space:
         *   clip_x = (x - CentreX) / CentreX * vr_hud_clip_scale + vr_hud_offset_x
         *   clip_y = -(y - CentreY) / CentreY * vr_hud_clip_scale + vr_hud_offset_y
         * The 3D perspective projects the aim direction to clip space:
         *   clip_x =  aim_vs.vx / aim_vs.vz * VDB_ProjX / (eye_fbo_w * 0.5)
         *   clip_y = -aim_vs.vy / aim_vs.vz * VDB_ProjY / (eye_fbo_h * 0.5)
         * Inverting the HUD formula gives the HUD pixel that matches each clip coord. */
        if (vr_right_hand_valid &&
            (AvP.PlayerType == I_Marine || AvP.PlayerType == I_Predator)) {
            extern int GunMuzzleSightX, GunMuzzleSightY;
            VECTORCH aim_vs;
            aim_vs.vx = vr_right_hand_mat.mat21;
            aim_vs.vy = vr_right_hand_mat.mat22;
            aim_vs.vz = vr_right_hand_mat.mat23;
            RotateVector(&aim_vs, &Global_VDB_Ptr->VDB_Mat);   /* world → view */
            if (aim_vs.vz > 0) {
                float aim_clip_x = (float)aim_vs.vx / aim_vs.vz
                                 * (float)Global_VDB_Ptr->VDB_ProjX / (eye_fbo_w * 0.5f);
                float aim_clip_y = -(float)aim_vs.vy / aim_vs.vz
                                 * (float)Global_VDB_Ptr->VDB_ProjY / (eye_fbo_h * 0.5f);
                int cx = ScreenDescriptorBlock.SDB_CentreX;
                int cy = ScreenDescriptorBlock.SDB_CentreY;
                int hud_x = cx + (int)((float)cx * (aim_clip_x - vr_hud_offset_x) / vr_hud_clip_scale);
                int hud_y = cy - (int)((float)cy * (aim_clip_y - vr_hud_offset_y) / vr_hud_clip_scale);
                GunMuzzleSightX = hud_x << 16;
                GunMuzzleSightY = hud_y << 16;
            }
        }
        MaintainHUD();
        if (ShowFrameRate) RenderString(fps_str, 20, 20, 0xFFFFFFFF);
        vr_hud_clip_scale = 1.0f;
        vr_hud_offset_x   = 0.0f;
        vr_hud_offset_y   = 0.0f;
        /* Flush remaining batched HUD geometry into the eye FBO */
        FlushRenderBuffer();

        /* Restore */
        Global_VDB_Ptr->VDB_ClipLeft  = saved_ClipLeft;
        Global_VDB_Ptr->VDB_ClipRight = saved_ClipRight;
        Global_VDB_Ptr->VDB_ClipUp    = saved_ClipUp;
        Global_VDB_Ptr->VDB_ClipDown  = saved_ClipDown;
        Global_VDB_Ptr->VDB_ProjX     = saved_ProjX;
        Global_VDB_Ptr->VDB_ProjY     = saved_ProjY;
        Global_VDB_Ptr->VDB_CentreX   = saved_CentreX;
        Global_VDB_Ptr->VDB_CentreY   = saved_CentreY;
        ScreenDescriptorBlock = saved_sdb;

        /* Detach swapchain texture and release — compositor will composite this frame */
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        VR_ReleaseSwapchainImage(eye);

    }

    vr_is_rendering = 0;
    SetFrustrumType(saved_frustrum);
    Global_VDB_Ptr->VDB_World = base_world;
    Global_VDB_Ptr->VDB_Mat   = base_mat;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(saved_vp[0], saved_vp[1], saved_vp[2], saved_vp[3]);
}
#endif /* __ANDROID__ */