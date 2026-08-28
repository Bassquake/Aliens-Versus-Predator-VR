/* KJL 14:43:27 10/6/97 - d3d_hud.cpp

	Things just got too messy with ddplat.cpp & d3_func.cpp,
	so this file will hold all Direct 3D hud code.

 */
extern "C" {

// Mysterious definition required by objbase.h 
// (included via one of the include files below)
// to start definition of obscure unique in the
// universe IDs required  by Direct3D before it
// will deign to cough up with anything useful...

#include "3dc.h"
#include "module.h"
#include "stratdef.h"
#include "gamedef.h"
#include "bh_types.h"

#include "hudgfx.h"
#include "huddefs.h"
//#include "hud_data.h"
#include "kshape.h"
#include "chnktexi.h"


#include "hud_layout.h"
#include "language.h"


extern void D3D_RenderHUDString_Centred(char *stringPtr, int centreX, int y, int colour);
extern void D3D_RenderHUDNumber_Centred(unsigned int number,int x,int y,int colour);

extern "C++" 									  
{
#include "r2base.h"
#include "pcmenus.h"
//#include "projload.hpp" // c++ header which ignores class definitions/member functions if __cplusplus is not defined ?
#include "chnkload.hpp" // c++ header which ignores class definitions/member functions if __cplusplus is not defined ?
};

#include "d3d_hud.h"


#define UseLocalAssert No
#include "ourasert.h"
											

#include "vision.h"
#define RGBLIGHT_MAKE(rr,gg,bb) \
( \
	LCCM_NORMAL == d3d_light_ctrl.ctrl ? \
		RGB_MAKE(rr,gg,bb) \
	: LCCM_CONSTCOLOUR == d3d_light_ctrl.ctrl ? \
		RGB_MAKE(MUL_FIXED(rr,d3d_light_ctrl.r),MUL_FIXED(gg,d3d_light_ctrl.g),MUL_FIXED(bb,d3d_light_ctrl.b)) \
	: \
		RGB_MAKE(d3d_light_ctrl.GetR(rr),d3d_light_ctrl.GetG(gg),d3d_light_ctrl.GetB(bb)) \
)
#define RGBALIGHT_MAKE(rr,gg,bb,aa) \
( \
		RGBA_MAKE(rr,gg,bb,aa) \
)


void D3D_DrawHUDFontCharacter(HUDCharDesc *charDescPtr);
void D3D_DrawHUDDigit(HUDCharDesc *charDescPtr);

extern void YClipMotionTrackerVertices(struct VertexTag *v1, struct VertexTag *v2);
extern void XClipMotionTrackerVertices(struct VertexTag *v1, struct VertexTag *v2);
/* HUD globals */
extern SCREENDESCRIPTORBLOCK ScreenDescriptorBlock;

extern enum HUD_RES_ID HUDResolution;

signed int HUDTranslucencyLevel=64;

static int MotionTrackerHalfWidth;
static int MotionTrackerTextureSize;
static int MotionTrackerCentreY;
static int MotionTrackerCentreX;
static int MT_BlipHeight;
static int MT_BlipWidth;
static HUDImageDesc BlueBar;


int HUDImageNumber;
int SpecialFXImageNumber;
int SmokyImageNumber;
int ChromeImageNumber;
int CloudyImageNumber;
int BurningImageNumber;
int HUDFontsImageNumber;
int RebellionLogoImageNumber;
int FoxLogoImageNumber;
int MotionTrackerScale;
int PredatorVisionChangeImageNumber;
int PredatorNumbersImageNumber;
int StaticImageNumber;
int AlienTongueImageNumber;
int AAFontImageNumber;
int WaterShaftImageNumber;


int HUDScaleFactor;

static struct HUDFontDescTag HUDFontDesc[] =
{
	//MARINE_HUD_FONT_BLUE,
	{
		225,//XOffset
		24,//Height
		16,//Width
	},
	//MARINE_HUD_FONT_RED,
	{
		242,//XOffset
		24,//Height
		14,//Width
	},
	//MARINE_HUD_FONT_MT_SMALL,
	{
		232,//XOffset
		12,//Height
		8,//Width
	},
	//MARINE_HUD_FONT_MT_BIG,
	{
		241,//XOffset
		24,//Height
		14,//Width
	},
};
#define BLUE_BAR_WIDTH ((203-0)+1)
#define BLUE_BAR_HEIGHT ((226-195)+1)

void D3D_BLTDigitToHUD(char digit, int x, int y, int font);


void Draw_HUDImage(HUDImageDesc *imageDescPtr)
{
	struct VertexTag quadVertices[4];
	int scaledWidth;
	int scaledHeight;

	if (imageDescPtr->Scale == ONE_FIXED)
	{
		scaledWidth = imageDescPtr->Width;
		scaledHeight = imageDescPtr->Height;
	}
	else
	{
		scaledWidth = MUL_FIXED(imageDescPtr->Scale,imageDescPtr->Width);
		scaledHeight = MUL_FIXED(imageDescPtr->Scale,imageDescPtr->Height);
	}

	/* Rescale the UVs — and ONLY the UVs — when an HD texture pack has replaced
	   this atlas with a bigger one. TopLeftU/V and Width/Height are absolute
	   pixels in the stock atlas, but Width/Height ALSO drive the on-screen quad
	   above, so scaling the fields themselves would blow up the HUD's size. */
	int uvU0 = imageDescPtr->TopLeftU;
	int uvV0 = imageDescPtr->TopLeftV;
	int uvU1 = imageDescPtr->TopLeftU + imageDescPtr->Width;
	int uvV1 = imageDescPtr->TopLeftV + imageDescPtr->Height;
	{
		int uvScale = HUD_AtlasUVScale(imageDescPtr->ImageNumber);
		if (uvScale != ONE_FIXED)
		{
			uvU0 = MUL_FIXED(uvU0, uvScale);
			uvV0 = MUL_FIXED(uvV0, uvScale);
			uvU1 = MUL_FIXED(uvU1, uvScale);
			uvV1 = MUL_FIXED(uvV1, uvScale);
		}
	}

	quadVertices[0].U = uvU0;
	quadVertices[0].V = uvV0;
	quadVertices[1].U = uvU1;
	quadVertices[1].V = uvV0;
	quadVertices[2].U = uvU1;
	quadVertices[2].V = uvV1;
	quadVertices[3].U = uvU0;
	quadVertices[3].V = uvV1;
	
	quadVertices[0].X = imageDescPtr->TopLeftX;
	quadVertices[0].Y = imageDescPtr->TopLeftY;
	quadVertices[1].X = imageDescPtr->TopLeftX + scaledWidth;
	quadVertices[1].Y = imageDescPtr->TopLeftY;
	quadVertices[2].X = imageDescPtr->TopLeftX + scaledWidth;
	quadVertices[2].Y = imageDescPtr->TopLeftY + scaledHeight;
	quadVertices[3].X = imageDescPtr->TopLeftX;
	quadVertices[3].Y = imageDescPtr->TopLeftY + scaledHeight;
		
	D3D_HUDQuad_Output
	(
		imageDescPtr->ImageNumber,
		quadVertices,
		RGBALIGHT_MAKE
		(
			imageDescPtr->Red,
			imageDescPtr->Green,
			imageDescPtr->Blue,
			imageDescPtr->Translucency
		)
	);
}


void D3D_InitialiseMarineHUD(void)
{
	//SelectGenTexDirectory(ITI_TEXTURE);

	/* set game mode: different though for multiplayer game */
	if(AvP.Network==I_No_Network)
		cl_pszGameMode = "marine";
	else
		cl_pszGameMode = "multip";

	/* load HUD gfx of correct resolution */
	{
		HUDResolution = HUD_RES_MED;
		HUDImageNumber = CL_LoadImageOnce("Huds\\Marine\\MarineHUD.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
		HUD_SetAtlasStockSize(HUDImageNumber, 256); /* authored size; see hud_layout.h */
		MotionTrackerHalfWidth = 127/2;
		MotionTrackerTextureSize = 128;

		/* Every UV into this atlas is an ABSOLUTE PIXEL COORDINATE assuming the
		   stock 256x256 MarineHUD.RIM: the tracker spans 1..129, the blue bar
		   starts at V=223, the gunsight/crosshair sits at U=227. An HD texture
		   pack that enlarges the atlas therefore leaves all of them describing a
		   sub-rectangle, and the art renders magnified — HD Redux ships a
		   1024x1024 replacement, so everything drawn from it came out 4x too big
		   (the motion tracker and crosshair most visibly). Draw_HUDImage rescales
		   the UVs it emits; the tracker below builds its own, so scale the size it
		   uses for them here. MotionTrackerHalfWidth is deliberately NOT scaled —
		   that one drives the on-screen geometry, not the texture lookup. */
		MotionTrackerTextureSize = MUL_FIXED(MotionTrackerTextureSize,
		                                     HUD_AtlasUVScale(HUDImageNumber));

		BlueBar.ImageNumber = HUDImageNumber;
		BlueBar.TopLeftX = 0;
		BlueBar.TopLeftY = ScreenDescriptorBlock.SDB_Height-40;
		BlueBar.TopLeftU = 1;
		BlueBar.TopLeftV = 223;
		BlueBar.Red = 255;
		BlueBar.Green = 255;
		BlueBar.Blue = 255;

		BlueBar.Height = BLUE_BAR_HEIGHT;
		BlueBar.Width = BLUE_BAR_WIDTH;

		/* motion tracker blips */
		MT_BlipHeight = 12;
		MT_BlipWidth = 12;

		/* load in sfx */
		SpecialFXImageNumber = CL_LoadImageOnce("Common\\partclfx.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
		HUD_SetAtlasStockSize(SpecialFXImageNumber, 256); /* authored size; see hud_layout.h */

//		SpecialFXImageNumber = CL_LoadImageOnceEx("flame1",IRF_D3D,DDSCAPS_SYSTEMMEMORY,0);;
//		SpecialFXImageNumber = CL_LoadImageOnceEx("star",IRF_D3D,DDSCAPS_SYSTEMMEMORY,0);;
//		SmokyImageNumber = CL_LoadImageOnceEx("smoky",IRF_D3D,DDSCAPS_SYSTEMMEMORY,0);

	}

	/* centre of motion tracker */
	MotionTrackerCentreY = BlueBar.TopLeftY;
	MotionTrackerCentreX = BlueBar.TopLeftX+(BlueBar.Width/2);
	MotionTrackerScale = 65536;

	/* Scale the HUD off HEIGHT, not width, and against 540 rather than 480.
	   MEASURED, not derived. The 1999 source (and this file, until 2026-08-27)
	   used SDB_Width/640, which is the same number at every 4:3 mode but
	   over-scales on a wide display — it judges the HUD against width when the eye
	   judges it against height. Switching the axis gave height/480 = 2.25 at
	   1920x1080, still visibly bigger than the retail Classic 2000 build.
	   Measuring both at 1920x1080 settled it: the HUD digits (22px tall before
	   scaling) render 45px in this port against 40px in retail, and the green
	   Health/Armor digits give the same 1.125 ratio, so retail is running an
	   effective factor of exactly 2.0. 540 = 480 * 1.125 reproduces that.
	   The retail binary is NOT this source — the re-release added widescreen
	   support and evidently changed HUD scaling with it — so its actual formula is
	   unknown and 2.0 at 1080p is the only data point. This stays proportional to
	   height, so other resolutions are an extrapolation: 2.67 at 1440p, 4.0 at
	   2160p. If retail turns out to clamp instead of scale, this needs revisiting.
	   Note this is deliberately no longer bit-identical to 1999 on 4:3 modes
	   (1024x768 gives 1.42 where the original gave 1.60). */
	HUDScaleFactor = DIV_FIXED(ScreenDescriptorBlock.SDB_Height,540);

	#if UseGadgets
//	MotionTrackerGadget::SetCentre(r2pos(100,100));
	#endif
}

void LoadCommonTextures(void)
{
//	PredatorVisionChangeImageNumber = CL_LoadImageOnce("HUDs\\Predator\\predvisfx.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
	if(AvP.Network==I_No_Network)
	{
		switch(AvP.PlayerType)
		{
			case I_Predator:
			{
				PredatorNumbersImageNumber = CL_LoadImageOnce("HUDs\\Predator\\predNumbers.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
				HUD_SetAtlasStockSize(PredatorNumbersImageNumber, 256); /* authored size; see hud_layout.h */
				StaticImageNumber = CL_LoadImageOnce("Common\\static.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
				HUD_SetAtlasStockSize(StaticImageNumber, 128); /* authored size; see hud_layout.h */
				break;
			}
			case I_Alien:
			{
				AlienTongueImageNumber = CL_LoadImageOnce("HUDs\\Alien\\AlienTongue.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
				HUD_SetAtlasStockSize(AlienTongueImageNumber, 128); /* authored size; see hud_layout.h */
				break;
			}
			case I_Marine:
			{
				StaticImageNumber = CL_LoadImageOnce("Common\\static.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
				HUD_SetAtlasStockSize(StaticImageNumber, 128); /* authored size; see hud_layout.h */
//			   	ChromeImageNumber = CL_LoadImageOnce("Common\\water2.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
				break;
			}
			default:
				break;
		}
	}
	else
	{
   		PredatorNumbersImageNumber = CL_LoadImageOnce("HUDs\\Predator\\predNumbers.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
   		HUD_SetAtlasStockSize(PredatorNumbersImageNumber, 256); /* authored size; see hud_layout.h */
   		StaticImageNumber = CL_LoadImageOnce("Common\\static.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
   		HUD_SetAtlasStockSize(StaticImageNumber, 128); /* authored size; see hud_layout.h */
		AlienTongueImageNumber = CL_LoadImageOnce("HUDs\\Alien\\AlienTongue.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
		HUD_SetAtlasStockSize(AlienTongueImageNumber, 128); /* authored size; see hud_layout.h */
	  //	ChromeImageNumber = CL_LoadImageOnce("Common\\water2.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
	}
	
	HUDFontsImageNumber = CL_LoadImageOnce("Common\\HUDfonts.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
	HUD_SetAtlasStockSize(HUDFontsImageNumber, 128); /* authored size; see hud_layout.h */
	SpecialFXImageNumber = CL_LoadImageOnce("Common\\partclfx.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE/*|LIO_TRANSPARENT*/);
	HUD_SetAtlasStockSize(SpecialFXImageNumber, 256); /* authored size; see hud_layout.h */
	CloudyImageNumber = CL_LoadImageOnce("Common\\cloudy.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
	HUD_SetAtlasStockSize(CloudyImageNumber, 128); /* authored size; see hud_layout.h */
	BurningImageNumber = CL_LoadImageOnce("Common\\burn.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE);
	HUD_SetAtlasStockSize(BurningImageNumber, 128); /* authored size; see hud_layout.h */
//	RebellionLogoImageNumber = CL_LoadImageOnce("Common\\logo.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
//	FoxLogoImageNumber = CL_LoadImageOnce("Common\\foxlogo.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
	
	
	{
		extern char LevelName[];
		if (!strcmp(LevelName,"invasion_a"))
		{
#if !ALIEN_DEMO
		   	ChromeImageNumber = CL_LoadImageOnce("Envrnmts\\Invasion\\water2.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
			WaterShaftImageNumber = CL_LoadImageOnce("Envrnmts\\Invasion\\water-shaft.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
#else /* alien demo has these in common */
		   	ChromeImageNumber = CL_LoadImageOnce("Common\\water2.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
			WaterShaftImageNumber = CL_LoadImageOnce("Common\\water-shaft.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
#endif			
		}
		else if (!strcmp(LevelName,"genshd1"))
		{
			WaterShaftImageNumber = CL_LoadImageOnce("Envrnmts\\GenShd1\\colonywater.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
		}
		else if (!strcmp(LevelName,"fall")||!strcmp(LevelName,"fall_m"))
		{
			ChromeImageNumber = CL_LoadImageOnce("Envrnmts\\fall\\stream_water.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
		}
		else if (!strcmp(LevelName,"derelict"))
		{
			ChromeImageNumber = CL_LoadImageOnce("Envrnmts\\derelict\\water.RIM",LIO_D3DTEXTURE|LIO_RELATIVEPATH|LIO_RESTORABLE|LIO_TRANSPARENT);
		}
		
	}

	#if 1
	{
		extern void InitDrawTest(void);
		InitDrawTest();
	}
	#endif

}

#ifdef AVP_XR
extern "C" {
	int VR_IsIn3DMode(void);
	extern VIEWDESCRIPTORBLOCK *Global_VDB_Ptr;
}
#endif

void D3D_BLTMotionTrackerToHUD(int scanLineSize)
{

	struct VertexTag quadVertices[4];
	int widthCos,widthSin;

	BlueBar.TopLeftY = ScreenDescriptorBlock.SDB_Height-MUL_FIXED(MotionTrackerScale,40);
	MotionTrackerCentreY = BlueBar.TopLeftY;
	MotionTrackerCentreX = BlueBar.TopLeftX+MUL_FIXED(MotionTrackerScale,(BlueBar.Width/2));
	BlueBar.Scale = MotionTrackerScale;

	int motionTrackerScaledHalfWidth = MUL_FIXED(MotionTrackerScale*3,MotionTrackerHalfWidth/2);

	{
		int yaw = Player->ObEuler.EulerY;
		#ifdef AVP_XR
		/* In VR the body doesn't turn with the head, so the tracker dish graphic never
		 * spins to match where the player looks. Use the view yaw (head + snap turn)
		 * instead - same source as the blip rotation so dish and blips stay aligned.
		 * Forward world dir = VDB_Mat column 3 (mat13,_,mat33). */
		if (VR_IsIn3DMode())
			yaw = ArcTan(Global_VDB_Ptr->VDB_Mat.mat13, Global_VDB_Ptr->VDB_Mat.mat33);
		#endif
		int angle = 4095 - yaw;

		widthCos = MUL_FIXED
				   (
				   		motionTrackerScaledHalfWidth,
				   		GetCos(angle)
				   );
		widthSin = MUL_FIXED
				   (
				   		motionTrackerScaledHalfWidth,
						GetSin(angle)
				   );
	}			
	
	/* I've put these -1s in here to help clipping 45 degree cases,
	where two vertices can end up around the clipping line of Y=0 */
	quadVertices[0].X = (-widthCos - (-widthSin));
	quadVertices[0].Y = (-widthSin + (-widthCos)) -1;
	quadVertices[0].U = 1;
	quadVertices[0].V = 1;
	quadVertices[1].X = (widthCos - (-widthSin));
	quadVertices[1].Y = (widthSin + (-widthCos)) -1;
	quadVertices[1].U = 1+MotionTrackerTextureSize;
	quadVertices[1].V = 1;
	quadVertices[2].X = (widthCos - widthSin);
	quadVertices[2].Y = (widthSin + widthCos) -1;
	quadVertices[2].U = 1+MotionTrackerTextureSize;
	quadVertices[2].V = 1+MotionTrackerTextureSize;
	quadVertices[3].X = ((-widthCos) - widthSin);
	quadVertices[3].Y = ((-widthSin) + widthCos) -1;
	quadVertices[3].U = 1;							   
	quadVertices[3].V = 1+MotionTrackerTextureSize;

	/* clip to Y<=0 */
	YClipMotionTrackerVertices(&quadVertices[0],&quadVertices[1]);
	YClipMotionTrackerVertices(&quadVertices[1],&quadVertices[2]);
	YClipMotionTrackerVertices(&quadVertices[2],&quadVertices[3]);
	YClipMotionTrackerVertices(&quadVertices[3],&quadVertices[0]);

	/* translate into screen coords */
	quadVertices[0].X += MotionTrackerCentreX;
	quadVertices[1].X += MotionTrackerCentreX;
	quadVertices[2].X += MotionTrackerCentreX;
	quadVertices[3].X += MotionTrackerCentreX;
	quadVertices[0].Y += MotionTrackerCentreY;
	quadVertices[1].Y += MotionTrackerCentreY;
	quadVertices[2].Y += MotionTrackerCentreY;
	quadVertices[3].Y += MotionTrackerCentreY;
	
	/* dodgy offset 'cos I'm not x clipping */
	if (quadVertices[0].X==-1) quadVertices[0].X = 0;
	if (quadVertices[1].X==-1) quadVertices[1].X = 0;
	if (quadVertices[2].X==-1) quadVertices[2].X = 0;
	if (quadVertices[3].X==-1) quadVertices[3].X = 0;
		
	/* check u & v are >0 */
	if (quadVertices[0].V<0) quadVertices[0].V = 0;
	if (quadVertices[1].V<0) quadVertices[1].V = 0;
	if (quadVertices[2].V<0) quadVertices[2].V = 0;
	if (quadVertices[3].V<0) quadVertices[3].V = 0;

	if (quadVertices[0].U<0) quadVertices[0].U = 0;
	if (quadVertices[1].U<0) quadVertices[1].U = 0;
	if (quadVertices[2].U<0) quadVertices[2].U = 0;
	if (quadVertices[3].U<0) quadVertices[3].U = 0;

	D3D_HUD_Setup();
	D3D_HUDQuad_Output(HUDImageNumber,quadVertices,RGBALIGHT_MAKE(255,255,255,HUDTranslucencyLevel));
	
	#if 1
	{
		HUDImageDesc imageDesc;

		imageDesc.ImageNumber = HUDImageNumber;
		imageDesc.Scale = MUL_FIXED(MotionTrackerScale*3,scanLineSize/2);
		imageDesc.TopLeftX = MotionTrackerCentreX - MUL_FIXED(motionTrackerScaledHalfWidth,scanLineSize);
		imageDesc.TopLeftY = MotionTrackerCentreY - MUL_FIXED(motionTrackerScaledHalfWidth,scanLineSize);
		imageDesc.TopLeftU = 1;
		imageDesc.TopLeftV = 132;
		imageDesc.Height = 64;
		imageDesc.Width = 128;
		imageDesc.Red = 255;
		imageDesc.Green = 255;
		imageDesc.Blue = 255;
		imageDesc.Translucency = HUDTranslucencyLevel;

 		Draw_HUDImage(&imageDesc);
	}
	#endif

	/* KJL 16:14:29 30/01/98 - draw bottom bar of MT */
	{
		BlueBar.Translucency = HUDTranslucencyLevel;
		Draw_HUDImage(&BlueBar);
	}
	
	D3D_BLTDigitToHUD(ValueOfHUDDigit[MARINE_HUD_MOTIONTRACKER_UNITS],17, -4, MARINE_HUD_FONT_MT_SMALL);	  
	D3D_BLTDigitToHUD(ValueOfHUDDigit[MARINE_HUD_MOTIONTRACKER_TENS],9, -4, MARINE_HUD_FONT_MT_SMALL);	  
	D3D_BLTDigitToHUD(ValueOfHUDDigit[MARINE_HUD_MOTIONTRACKER_HUNDREDS],-9, -4, MARINE_HUD_FONT_MT_BIG);
    D3D_BLTDigitToHUD(ValueOfHUDDigit[MARINE_HUD_MOTIONTRACKER_THOUSANDS],-25, -4,MARINE_HUD_FONT_MT_BIG);	
}


void D3D_BLTMotionTrackerBlipToHUD(int x, int y, int brightness)
{
	HUDImageDesc imageDesc;
	int frame;
	int motionTrackerScaledHalfWidth = MUL_FIXED(MotionTrackerScale*3,MotionTrackerHalfWidth/2);
    
	GLOBALASSERT(brightness<=65536);
	
	frame = (brightness*5)/65537;
	GLOBALASSERT(frame>=0 && frame<5);
	
    frame = 4 - frame; // frames bloody wrong way round
	imageDesc.ImageNumber = HUDImageNumber;
	imageDesc.Scale = MUL_FIXED(MotionTrackerScale*3,(brightness+ONE_FIXED)/4);
	imageDesc.TopLeftX = MotionTrackerCentreX - MUL_FIXED(MT_BlipWidth/2,imageDesc.Scale) + MUL_FIXED(x,motionTrackerScaledHalfWidth);
	imageDesc.TopLeftY = MotionTrackerCentreY - MUL_FIXED(MT_BlipHeight/2,imageDesc.Scale) - MUL_FIXED(y,motionTrackerScaledHalfWidth);
	imageDesc.TopLeftU = 227;
	imageDesc.TopLeftV = 187;
	imageDesc.Height = MT_BlipHeight;
	imageDesc.Width = MT_BlipWidth;
	{
		int trans = MUL_FIXED(brightness*2,HUDTranslucencyLevel);
		if (trans>255) trans = 255;
		imageDesc.Translucency = trans;
	}
	imageDesc.Red = 255;
	imageDesc.Green = 255;
	imageDesc.Blue = 255;
	if (imageDesc.TopLeftX<0) /* then we need to clip */
	{
		imageDesc.Width += imageDesc.TopLeftX;
		imageDesc.TopLeftU -= imageDesc.TopLeftX;
		imageDesc.TopLeftX = 0;
	}
	Draw_HUDImage(&imageDesc);
}
extern void D3D_BlitWhiteChar(int x, int y, unsigned char c)
{
	HUDImageDesc imageDesc;
	
//	if (c>='a' && c<='z') c-='a'-'A';

//	if (c<' ' || c>'_') return;
	if (c==' ') return;

	#if 0
	imageDesc.ImageNumber = HUDFontsImageNumber;

	imageDesc.TopLeftX = x;
	imageDesc.TopLeftY = y;
	imageDesc.TopLeftU = 1+((c-32)&15)*7;
	imageDesc.TopLeftV = 2+((c-32)>>4)*11;
	imageDesc.Height = 8;
	imageDesc.Width = 5;
	#else
	imageDesc.ImageNumber = AAFontImageNumber;

	imageDesc.TopLeftX = x;
	imageDesc.TopLeftY = y;
	imageDesc.TopLeftU = 1+((c-32)&15)*16;
	imageDesc.TopLeftV = 1+((c-32)>>4)*16;
	imageDesc.Height = 15;
	imageDesc.Width = 15;
	#endif
	imageDesc.Scale = ONE_FIXED;
	imageDesc.Translucency = 255;
	imageDesc.Red = 255;
	imageDesc.Green = 255;
	imageDesc.Blue = 255;

	Draw_HUDImage(&imageDesc);
} 

void D3D_DrawHUDFontCharacter(HUDCharDesc *charDescPtr)
{
	HUDImageDesc imageDesc;

  //	if (charDescPtr->Character<' ' || charDescPtr->Character>'_') return;
	if (charDescPtr->Character == ' ') return;

	imageDesc.ImageNumber = AAFontImageNumber;

	imageDesc.TopLeftX = charDescPtr->X-1;
	imageDesc.TopLeftY = charDescPtr->Y-1;
	imageDesc.TopLeftU = 0+((charDescPtr->Character-32)&15)*16;
	imageDesc.TopLeftV = 0+((charDescPtr->Character-32)>>4)*16;
	imageDesc.Height = HUD_FONT_HEIGHT+2;
	imageDesc.Width = HUD_FONT_WIDTH+2;

	imageDesc.Scale = ONE_FIXED;
	imageDesc.Translucency = charDescPtr->Alpha;
	imageDesc.Red = charDescPtr->Red;
	imageDesc.Green = charDescPtr->Green;
	imageDesc.Blue = charDescPtr->Blue;

	Draw_HUDImage(&imageDesc);
	
}
void D3D_DrawHUDDigit(HUDCharDesc *charDescPtr)
{
	HUDImageDesc imageDesc;

	imageDesc.ImageNumber = HUDFontsImageNumber;

	imageDesc.TopLeftX = charDescPtr->X;
	imageDesc.TopLeftY = charDescPtr->Y;

	if (charDescPtr->Character<8)
	{
		imageDesc.TopLeftU = 1+(charDescPtr->Character)*16;
		imageDesc.TopLeftV = 81;
	}
	else
	{
		imageDesc.TopLeftU = 1+(charDescPtr->Character-8)*16;
		imageDesc.TopLeftV = 81+24;
	}


	imageDesc.Height = HUD_DIGITAL_NUMBERS_HEIGHT;
	imageDesc.Width = HUD_DIGITAL_NUMBERS_WIDTH;
	imageDesc.Scale = ONE_FIXED;
	imageDesc.Translucency = charDescPtr->Alpha;
	imageDesc.Red = charDescPtr->Red;
	imageDesc.Green = charDescPtr->Green;
	imageDesc.Blue = charDescPtr->Blue;

	Draw_HUDImage(&imageDesc);
	
}
void D3D_DrawHUDPredatorDigit(HUDCharDesc *charDescPtr, int scale)
{
	HUDImageDesc imageDesc;

	imageDesc.ImageNumber = PredatorNumbersImageNumber;

	imageDesc.TopLeftX = charDescPtr->X;
	imageDesc.TopLeftY = charDescPtr->Y;

	if (charDescPtr->Character<5)
	{
		imageDesc.TopLeftU = (charDescPtr->Character)*51;
		imageDesc.TopLeftV = 1;
	}
	else
	{
		imageDesc.TopLeftU = (charDescPtr->Character-5)*51;
		imageDesc.TopLeftV = 52;
	}


	imageDesc.Height = 50;
	imageDesc.Width = 50;
	imageDesc.Scale = scale;
	imageDesc.Translucency = charDescPtr->Alpha;
	imageDesc.Red = charDescPtr->Red;
	imageDesc.Green = charDescPtr->Green;
	imageDesc.Blue = charDescPtr->Blue;

	Draw_HUDImage(&imageDesc);
	
}

void D3D_BLTDigitToHUD(char digit, int x, int y, int font)
{
	HUDImageDesc imageDesc;
	struct HUDFontDescTag *FontDescPtr;
	int gfxID;

	switch (font)
	{
		case MARINE_HUD_FONT_MT_SMALL:
	  	case MARINE_HUD_FONT_MT_BIG:
		{
		   	gfxID = MARINE_HUD_GFX_TRACKERFONT;
			imageDesc.Scale = MotionTrackerScale;
			x = MUL_FIXED(x,MotionTrackerScale) + MotionTrackerCentreX;
			y = MUL_FIXED(y,MotionTrackerScale) + MotionTrackerCentreY;
			break;
		}
		case MARINE_HUD_FONT_RED:
		case MARINE_HUD_FONT_BLUE:
		{
			if (x<0) x+=ScreenDescriptorBlock.SDB_Width;
		   	gfxID = MARINE_HUD_GFX_NUMERALS;
			imageDesc.Scale=ONE_FIXED;
			break;
		}
		case ALIEN_HUD_FONT:
		{
			gfxID = ALIEN_HUD_GFX_NUMBERS;
			imageDesc.Scale=ONE_FIXED;
			break;
		}
		default:
			LOCALASSERT(0);
			break;
	}

	
	if (HUDResolution == HUD_RES_LO)
	{
		FontDescPtr = &HUDFontDesc[font];
	}
	else if (HUDResolution == HUD_RES_MED)
	{
		FontDescPtr = &HUDFontDesc[font];
	}
	else
	{
		FontDescPtr = &HUDFontDesc[font];
	}



	imageDesc.ImageNumber = HUDImageNumber;
	imageDesc.TopLeftX = x;
	imageDesc.TopLeftY = y;
	imageDesc.TopLeftU = FontDescPtr->XOffset;
	imageDesc.TopLeftV = digit*(FontDescPtr->Height+1)+1;
	
	imageDesc.Height = FontDescPtr->Height;
	imageDesc.Width = FontDescPtr->Width;
	imageDesc.Translucency = HUDTranslucencyLevel;
	imageDesc.Red = 255;
	imageDesc.Green = 255;
	imageDesc.Blue = 255;

	Draw_HUDImage(&imageDesc);

}

void D3D_BLTGunSightToHUD(int screenX, int screenY, enum GUNSIGHT_SHAPE gunsightShape)
{
  	HUDImageDesc imageDesc;
	int gunsightSize=13;

	screenX = (screenX-(gunsightSize/2));
  	screenY = (screenY-(gunsightSize/2));
  	
	imageDesc.ImageNumber = HUDImageNumber;
	imageDesc.TopLeftX = screenX;
	imageDesc.TopLeftY = screenY;
	imageDesc.TopLeftU = 227;
	imageDesc.TopLeftV = 131+(gunsightShape*(gunsightSize+1));
	imageDesc.Height = gunsightSize;
	imageDesc.Width = gunsightSize;
	imageDesc.Scale = ONE_FIXED;
	imageDesc.Translucency = 128;
	imageDesc.Red = 255;
	imageDesc.Green = 255;
	imageDesc.Blue = 255;

	Draw_HUDImage(&imageDesc);
}

void Render_HealthAndArmour(unsigned int health, unsigned int armour)
{
	unsigned int healthColour;
	unsigned int armourColour;

	if (AvP.PlayerType == I_Marine)
	{										  
		int xCentre = MUL_FIXED(HUDLayout_RightmostTextCentre,HUDScaleFactor)+ScreenDescriptorBlock.SDB_Width;
		healthColour = HUDLayout_Colour_MarineGreen;
		armourColour = HUDLayout_Colour_MarineGreen;
		D3D_RenderHUDString_Centred
		(
			GetTextString(TEXTSTRING_INGAME_HEALTH),
			xCentre,
			MUL_FIXED(HUDLayout_Health_TopY,HUDScaleFactor),
			HUDLayout_Colour_BrightWhite
		);
		D3D_RenderHUDNumber_Centred
		(
			health,
			xCentre,
			MUL_FIXED(HUDLayout_Health_TopY+HUDLayout_Linespacing,HUDScaleFactor),
			healthColour
		);	
		D3D_RenderHUDString_Centred
		(
			GetTextString(TEXTSTRING_INGAME_ARMOUR),
			xCentre,
			MUL_FIXED(HUDLayout_Armour_TopY,HUDScaleFactor),
			HUDLayout_Colour_BrightWhite
		);
		D3D_RenderHUDNumber_Centred
		(
			armour,
			xCentre,
			MUL_FIXED(HUDLayout_Armour_TopY+HUDLayout_Linespacing,HUDScaleFactor),
			armourColour
		);	
	}
	else
	{										  
		if (health>100)
		{
			healthColour = HUDLayout_Colour_BrightWhite;
		}
		else
		{
			int r = ((health)*128)/100;
			healthColour = 0xff000000 + ((128-r)<<16) + (r<<8);
		}
		if (armour>100)
		{
			armourColour = HUDLayout_Colour_BrightWhite;
		}
		else
		{
			int r = ((armour)*128)/100;
			armourColour = 0xff000000 + ((128-r)<<16) + (r<<8);
		}

		{
		
   			struct VertexTag quadVertices[4];
			int scaledWidth;
			int scaledHeight;
			int x,y;

			if (health<100)
			{
				scaledWidth = WideMulNarrowDiv(ScreenDescriptorBlock.SDB_Width,health,100);
				scaledHeight = scaledWidth/32;
			}
			else
			{
				scaledWidth = ScreenDescriptorBlock.SDB_Width;
				scaledHeight = scaledWidth/32;
			}
			x = (ScreenDescriptorBlock.SDB_Width - scaledWidth)/2;
			y = ScreenDescriptorBlock.SDB_Height - ScreenDescriptorBlock.SDB_Width/32 + x/32;

			quadVertices[0].U = 8;
			quadVertices[0].V = 5;
			quadVertices[1].U = 57;//255;
			quadVertices[1].V = 5;
			quadVertices[2].U = 57;//255;
			quadVertices[2].V = 55;//255;
			quadVertices[3].U = 8;
			quadVertices[3].V = 55;//255;
			
			quadVertices[0].X = x;
			quadVertices[0].Y = y;
			quadVertices[1].X = x + scaledWidth;
			quadVertices[1].Y = y;
			quadVertices[2].X = x + scaledWidth;
			quadVertices[2].Y = y + scaledHeight;
			quadVertices[3].X = x;
			quadVertices[3].Y = y + scaledHeight;
				
			D3D_HUDQuad_Output
			(
				SpecialFXImageNumber,// AlienEnergyBarImageNumber,
				quadVertices,
				0xff003fff
			);
		
			health = (health/2);
			if (health<0) health=0;

			if (health<100)
			{
				scaledWidth = WideMulNarrowDiv(ScreenDescriptorBlock.SDB_Width,health,100);
				scaledHeight = scaledWidth/32;
			}
			else
			{
				scaledWidth = ScreenDescriptorBlock.SDB_Width;
				scaledHeight = scaledWidth/32;
			}
	
			x = (ScreenDescriptorBlock.SDB_Width - scaledWidth)/2;
			y = ScreenDescriptorBlock.SDB_Height - ScreenDescriptorBlock.SDB_Width/32 + x/32;
	
			quadVertices[0].X = x;
			quadVertices[0].Y = y;
			quadVertices[1].X = x + scaledWidth;
			quadVertices[1].Y = y;
			quadVertices[2].X = x + scaledWidth;
			quadVertices[2].Y = y + scaledHeight;
			quadVertices[3].X = x;
			quadVertices[3].Y = y + scaledHeight;
	
			D3D_HUDQuad_Output
			(
				SpecialFXImageNumber,// AlienEnergyBarImageNumber,
				quadVertices,
				0xffffffff
			);
			
		}

	}
	

		
} 
void Render_MarineAmmo(enum TEXTSTRING_ID ammoText, enum TEXTSTRING_ID magazinesText, unsigned int magazines, enum TEXTSTRING_ID roundsText, unsigned int rounds, int primaryAmmo)
{
	int xCentre = MUL_FIXED(HUDLayout_RightmostTextCentre,HUDScaleFactor)+ScreenDescriptorBlock.SDB_Width;
	if(!primaryAmmo) xCentre+=MUL_FIXED(HUDScaleFactor,HUDLayout_RightmostTextCentre*2);

	D3D_RenderHUDString_Centred
	(
		GetTextString(ammoText),
		xCentre,
		ScreenDescriptorBlock.SDB_Height - MUL_FIXED(HUDScaleFactor,HUDLayout_AmmoDesc_TopY),
		HUDLayout_Colour_BrightWhite
	);
	D3D_RenderHUDString_Centred
	(
		GetTextString(magazinesText),
		xCentre,
		ScreenDescriptorBlock.SDB_Height -MUL_FIXED(HUDScaleFactor, HUDLayout_Magazines_TopY),
		HUDLayout_Colour_BrightWhite
	);
	D3D_RenderHUDNumber_Centred
	(
		magazines,
		xCentre,
		ScreenDescriptorBlock.SDB_Height - MUL_FIXED(HUDScaleFactor,HUDLayout_Magazines_TopY - HUDLayout_Linespacing),
		HUDLayout_Colour_MarineRed
	);	
	D3D_RenderHUDString_Centred
	(
		GetTextString(roundsText),
		xCentre,
		ScreenDescriptorBlock.SDB_Height - MUL_FIXED(HUDScaleFactor,HUDLayout_Rounds_TopY),
		HUDLayout_Colour_BrightWhite
	);
	D3D_RenderHUDNumber_Centred
	(
		rounds,
		xCentre,
		ScreenDescriptorBlock.SDB_Height - MUL_FIXED(HUDScaleFactor,HUDLayout_Rounds_TopY - HUDLayout_Linespacing),
		HUDLayout_Colour_MarineRed
	);	

		
} 
void DrawPredatorEnergyBar(void)
{
	PLAYER_STATUS *playerStatusPtr= (PLAYER_STATUS *) (Player->ObStrategyBlock->SBdataptr);
	PLAYER_WEAPON_DATA *weaponPtr = &(playerStatusPtr->WeaponSlot[playerStatusPtr->SelectedWeaponSlot]);
	int maxHeight = ScreenDescriptorBlock.SDB_Height*3/4;
	int h;
	{
		h = MUL_FIXED(DIV_FIXED(playerStatusPtr->FieldCharge,PLAYERCLOAK_MAXENERGY),maxHeight);
		
		r2rect rectangle
		(
			ScreenDescriptorBlock.SDB_Width+HUDLayout_RightmostTextCentre*3/2,
			ScreenDescriptorBlock.SDB_Height-h,
			ScreenDescriptorBlock.SDB_Width+HUDLayout_RightmostTextCentre/2,
			ScreenDescriptorBlock.SDB_Height
			
		);
		rectangle . AlphaFill
		(
			0xff, // unsigned char R,
			0x00,// unsigned char G,
			0x00,// unsigned char B,
		   	128 // unsigned char translucency
		);
	}
	if (weaponPtr->WeaponIDNumber == WEAPON_PRED_SHOULDERCANNON)
	{
		h = MUL_FIXED(playerStatusPtr->PlasmaCasterCharge,maxHeight);
			
		r2rect rectangle
		(
			ScreenDescriptorBlock.SDB_Width+HUDLayout_RightmostTextCentre*3,
			ScreenDescriptorBlock.SDB_Height-h,
			ScreenDescriptorBlock.SDB_Width+HUDLayout_RightmostTextCentre*2,
			ScreenDescriptorBlock.SDB_Height
			
		);
		rectangle . AlphaFill
		(
			0x00, // unsigned char R,
			0xff,// unsigned char G,
			0xff,// unsigned char B,
		   	128 // unsigned char translucency
		);
	}

}

};
