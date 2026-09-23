#include "3dc.h"
#include "module.h"
#include "inline.h"

#include "stratdef.h"
#include "gamedef.h"
#include "dynblock.h"

#include "particle.h"
#include "sfx.h"
#include "detaillevels.h"
#include "bh_types.h"
#include "bh_ais.h"
#include "bh_pred.h"
#include "bh_corpse.h"
#include "lighting.h"
#define UseLocalAssert Yes
#include "ourasert.h"

#ifdef AVP_XR
extern int VR_IsIn3DMode(void);
extern int vr_eye_index; /* 0 = left eye, 1 = right eye, during per-eye VR rendering */
#endif

static SFXBLOCK SfxBlockStorage[MAX_NO_OF_SFX_BLOCKS];
static int NumFreeSfxBlocks;
static SFXBLOCK *FreeSfxBlockList[MAX_NO_OF_SFX_BLOCKS];
static SFXBLOCK **FreeSfxBlockListPtr;



/*KJL***************************************************************************
* FUNCTIONS TO ALLOCATE AND DEALLOCATE SFX BLOCKS - KJL 12:02:14 11/13/96 *
***************************************************************************KJL*/
void InitialiseSfxBlocks(void)
{
	SFXBLOCK *freeBlockPtr = SfxBlockStorage;
	int blk;

	for(blk=0; blk < MAX_NO_OF_SFX_BLOCKS; blk++) 
	{								
		FreeSfxBlockList[blk] = freeBlockPtr++;
	}

	FreeSfxBlockListPtr = &FreeSfxBlockList[MAX_NO_OF_SFX_BLOCKS-1];
	NumFreeSfxBlocks = MAX_NO_OF_SFX_BLOCKS;
}


SFXBLOCK* AllocateSfxBlock(void)
{
	SFXBLOCK *sfxPtr = 0; /* Default to null ptr */

	if (NumFreeSfxBlocks) 
	{
		sfxPtr = *FreeSfxBlockListPtr--;
		NumFreeSfxBlocks--;
	}
	else
	{
		/* unable to allocate a sfxamics block I'm afraid; 
		   MAX_NO_OF_SFX_BLOCKS is too low */
   	  //LOCALASSERT(NumFreeSfxBlocks);
		textprint("No Free SFX blocks!\n");
	}

	return sfxPtr;
}


void DeallocateSfxBlock(SFXBLOCK *sfxPtr)
{
	GLOBALASSERT(sfxPtr);
	*(++FreeSfxBlockListPtr) = sfxPtr;
	NumFreeSfxBlocks++;
}



DISPLAYBLOCK *CreateSFXObject(enum SFX_ID sfxID)
{
	DISPLAYBLOCK *dispPtr = CreateActiveObject();
	
	if (dispPtr)
	{
		SFXBLOCK *sfxPtr = AllocateSfxBlock();

		if (sfxPtr)
		{
			dispPtr->SfxPtr = sfxPtr;
			sfxPtr->SfxID = sfxID;
		}
		else
		{
			/* damn, we've got a DISPLAYBLOCK, but were unable to get a SFXBLOCK;
			   this means we must dealloc the DISPLAYBLOCK and return NULL to indicate 
			   failure.
			*/
			DestroyActiveObject(dispPtr);
			dispPtr = 0;
		}
	}

	return dispPtr;
}


void DrawSfxObject(DISPLAYBLOCK *dispPtr)
{
	SFXBLOCK *sfxPtr;
	
	GLOBALASSERT(dispPtr);
	
	sfxPtr = dispPtr->SfxPtr;
	GLOBALASSERT(sfxPtr);
	

	switch(sfxPtr->SfxID)
	{
		case SFX_MUZZLE_FLASH_AMORPHOUS:
		{
			if (!sfxPtr->EffectDrawnLastFrame)
			{
				VECTORCH direction;

				direction.vx = dispPtr->ObMat.mat31;
				direction.vy = dispPtr->ObMat.mat32;
				direction.vz = dispPtr->ObMat.mat33;
				DrawMuzzleFlash(&dispPtr->ObWorld,&direction,MUZZLE_FLASH_AMORPHOUS);
			}
			/* Strobe toggle - flickers the flash every other DRAW. In VR the scene is
			 * rendered once per eye, so toggling per-eye would draw the flash in the
			 * left eye then skip the right. Only toggle on the right (last) eye so both
			 * eyes show the same frame's strobe state. */
			#ifdef AVP_XR
			if (!VR_IsIn3DMode() || vr_eye_index != 0)
			#endif
			sfxPtr->EffectDrawnLastFrame=!sfxPtr->EffectDrawnLastFrame;

			break;
		}
		case SFX_MUZZLE_FLASH_SMARTGUN:
		{
			VECTORCH direction;

			direction.vx = dispPtr->ObMat.mat31;
			direction.vy = dispPtr->ObMat.mat32;
			direction.vz = dispPtr->ObMat.mat33;
			DrawMuzzleFlash(&dispPtr->ObWorld,&direction,MUZZLE_FLASH_SMARTGUN);
			break;
		}
		case SFX_MUZZLE_FLASH_SKEETER:
		{
			if (!sfxPtr->EffectDrawnLastFrame)
			{
				VECTORCH direction;

				direction.vx = dispPtr->ObMat.mat31;
				direction.vy = dispPtr->ObMat.mat32;
				direction.vz = dispPtr->ObMat.mat33;
				DrawMuzzleFlash(&dispPtr->ObWorld,&direction,MUZZLE_FLASH_SKEETER);
			}
			/* Strobe toggle - flickers the flash every other DRAW. In VR the scene is
			 * rendered once per eye, so toggling per-eye would draw the flash in the
			 * left eye then skip the right. Only toggle on the right (last) eye so both
			 * eyes show the same frame's strobe state. */
			#ifdef AVP_XR
			if (!VR_IsIn3DMode() || vr_eye_index != 0)
			#endif
			sfxPtr->EffectDrawnLastFrame=!sfxPtr->EffectDrawnLastFrame;

			break;
		}
		case SFX_FRISBEE_PLASMA_BOLT:
		{
			VECTORCH direction;
			direction.vx = dispPtr->ObMat.mat31;
			direction.vy = dispPtr->ObMat.mat32;
			direction.vz = dispPtr->ObMat.mat33;
			DrawFrisbeePlasmaBolt(&dispPtr->ObWorld,&direction);

			break;
		}
		case SFX_PREDATOR_PLASMA_BOLT:
		{
			VECTORCH direction;
			direction.vx = dispPtr->ObMat.mat31;
			direction.vy = dispPtr->ObMat.mat32;
			direction.vz = dispPtr->ObMat.mat33;
			DrawPredatorPlasmaBolt(&dispPtr->ObWorld,&direction);

			break;
		}
		case SFX_SMALL_PREDATOR_PLASMA_BOLT:
		{
			VECTORCH direction;
			direction.vx = dispPtr->ObMat.mat31;
			direction.vy = dispPtr->ObMat.mat32;
			direction.vz = dispPtr->ObMat.mat33;
			DrawSmallPredatorPlasmaBolt(&dispPtr->ObWorld,&direction);

			break;
		}

		default:
		{
			GLOBALASSERT(0);
			break;
		}
	}
}

void HandleSfxForObject(DISPLAYBLOCK *dispPtr)
{
	STRATEGYBLOCK *sbPtr = dispPtr->ObStrategyBlock;

	if (dispPtr->SpecialFXFlags & SFXFLAG_ONFIRE)
	{
		HandleObjectOnFire(dispPtr);
	}

	if (sbPtr)
	{
		if(sbPtr->I_SBtype == I_BehaviourNetCorpse)
		{
			NETCORPSEDATABLOCK *corpseDataPtr = (NETCORPSEDATABLOCK *)sbPtr->SBdataptr;
		
			if(!( (dispPtr->SpecialFXFlags & SFXFLAG_MELTINGINTOGROUND)&&(dispPtr->ObFlags2 < ONE_FIXED) )
				&& corpseDataPtr->This_Death->Electrical && ((FastRandom()&255)==0))	
			{
				VECTORCH velocity;
				velocity.vx = (FastRandom()&2047)-1024;
				velocity.vy = (FastRandom()&2047)-1024;
				velocity.vz = (FastRandom()&2047)-1024;
				MakeParticle(&dispPtr->ObWorld,&velocity,PARTICLE_SPARK);	
				velocity.vx = (FastRandom()&2047)-1024;
				velocity.vy = (FastRandom()&2047)-1024;
				velocity.vz = (FastRandom()&2047)-1024;
				MakeParticle(&dispPtr->ObWorld,&velocity,PARTICLE_SPARK);	
				MakeLightElement(&dispPtr->ObWorld,LIGHTELEMENT_ELECTRICAL_SPARKS);
				
			}
		}
		
	}
}


/* Fire particles PER SECOND per burning object, replacing a flat 5 per FRAME.
 *
 * 5 per frame was both too many and frame-rate dependent. Measured on flat at 60fps:
 * ~10 objects alight produced 2983 live particles and a 17.1ms particle pass inside a
 * 16.7ms frame - and because the count was per frame, VR at 90Hz spawned 50% MORE of
 * them into a 11.1ms budget, which is why the headset suffered worst.
 *
 * 300/s is exactly what 5 per frame gave at 60fps, so a single burning object looks the
 * same as it always did on a 60Hz screen; it is now the same at any frame rate, which is
 * the actual bug fixed. The pool taper below is what stops a dozen of them together
 * swamping the frame. */
#define FIRE_PARTICLES_PER_SECOND   300
/* Above this fraction of the particle pool, burning objects start thinning out; at the
   top of the range they stop emitting. Chosen so one or two fires never taper (they sit
   near the idle few hundred) and only a big multi-object blaze does - the case that
   actually costs frames. */
#define FIRE_TAPER_START_PERCENT    20
#define FIRE_TAPER_END_PERCENT      55

void HandleObjectOnFire(DISPLAYBLOCK *dispPtr)
{
	int objectIsDisappearing;
	extern int NormalFrameTime;
	int noRequired = 1;
	int i;
	int fireRate;
	VECTORCH velocity;
	
	if (!dispPtr->ObShape) return;

	if (dispPtr->ObShapeData->shaperadius<=LocalDetailLevels.AlienEnergyViewThreshold) return;

	#if 1
	{
		DYNAMICSBLOCK *dynPtr;
		STRATEGYBLOCK *sbPtr;
		
	   	sbPtr = dispPtr->ObStrategyBlock;
		LOCALASSERT(sbPtr);
		dynPtr = sbPtr->DynPtr;
		LOCALASSERT(sbPtr);

		
		velocity.vx = DIV_FIXED((dynPtr->Position.vx-dynPtr->PrevPosition.vx)*3,NormalFrameTime*4);
		velocity.vy = DIV_FIXED((dynPtr->Position.vy-dynPtr->PrevPosition.vy)*3,NormalFrameTime*4);
		velocity.vz = DIV_FIXED((dynPtr->Position.vz-dynPtr->PrevPosition.vz)*3,NormalFrameTime*4);

		if (dispPtr==sbPtr->SBdptr)	noRequired = 5;

	}
	#else
	velocity.vx = 0;
	velocity.vy = 0;
	velocity.vz = 0;
	#endif
	
	objectIsDisappearing = ( (dispPtr->SpecialFXFlags & SFXFLAG_MELTINGINTOGROUND) &&(dispPtr->ObFlags2 <= ONE_FIXED) )	;

	/* Per-frame count from a per-SECOND rate, tapered by how full the pool already is.
	 *
	 * noRequired arrives as 5 for the object's own displayblock and 1 otherwise (set
	 * above); scale that ratio onto the rate rather than discarding it, so the
	 * distinction the original drew is preserved.
	 *
	 * The fractional part is applied probabilistically - at 90Hz the exact figure is
	 * 3.33 particles per frame, and truncating to 3 would quietly drop 10% of the fire.
	 * Over a few frames the average comes out right. */
	fireRate = (FIRE_PARTICLES_PER_SECOND * noRequired) / 5;
	{
		int load = ParticleSystemLoad();
		int cap  = ParticleSystemCapacity();
		int lo   = (cap * FIRE_TAPER_START_PERCENT) / 100;
		int hi   = (cap * FIRE_TAPER_END_PERCENT)   / 100;

		if (load >= hi)      fireRate = 0;
		else if (load > lo)  fireRate = (fireRate * (hi - load)) / (hi - lo);
	}
	{
		/* rate * frametime, in 16.16 */
		int want = MUL_FIXED(fireRate << 16, NormalFrameTime);
		noRequired = want >> 16;
		if ((FastRandom() & 65535) < (want & 65535)) noRequired++;
	}

	/* TWO loops, one per particle type, NOT one loop spawning both.
	 *
	 * PARTICLE_FIRE draws with TRANSLUCENCY_GLOWING and PARTICLE_IMPACTSMOKE with
	 * TRANSLUCENCY_INVCOLOUR, and CheckTranslucencyModeIsCorrect (opengl.c) FLUSHES THE
	 * TRIANGLE BATCH on every change of mode. Spawning them alternately laid them down
	 * alternating in the particle pool, the render walks the pool in order, so the mode
	 * flipped on almost every particle - about 2200 batch flushes, i.e. 2200 draw calls,
	 * in a single frame. Measured at ~7us per particle (15.4ms for 2203 particles against
	 * 0.85ms for 226), and being driver/CPU cost it was identical on a Quest and on a
	 * fast desktop GPU.
	 *
	 * Allocation is sequential, so emitting all of one type and then all of the other
	 * puts each cohort in a contiguous run and collapses those flushes to a couple per
	 * cohort. Nothing about the appearance changes - same particles, same positions, same
	 * count; only the order they are created in. */
	{
		VECTORCH position;

		for (i=0; i<noRequired; i++)
		{
			position.vx = dispPtr->ObWorld.vx+(FastRandom()&255)-128;
			position.vy = dispPtr->ObWorld.vy+(FastRandom()&255)-128;
			position.vz = dispPtr->ObWorld.vz+(FastRandom()&255)-128;

			if (objectIsDisappearing)
			{
				if ((FastRandom()&65535) < dispPtr->ObFlags2)
					MakeParticle(&(position), &velocity, PARTICLE_FIRE);
			}
			else
			{
				MakeParticle(&(position), &velocity, PARTICLE_FIRE);
			}
		}

		for (i=0; i<noRequired; i++)
		{
			position.vx = dispPtr->ObWorld.vx+(FastRandom()&255)-128;
			position.vy = dispPtr->ObWorld.vy+(FastRandom()&255)-128;
			position.vz = dispPtr->ObWorld.vz+(FastRandom()&255)-128;

			if ((FastRandom()&65535) > 32768)
			{
				MakeParticle(&(position), &velocity, PARTICLE_IMPACTSMOKE);
			}
		}
	}

}
