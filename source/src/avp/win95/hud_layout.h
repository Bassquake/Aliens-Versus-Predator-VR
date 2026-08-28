/* KJL 17:58:47 18/04/98 - layout defines */

#define HUD_FONT_WIDTH 15
//5
#define HUD_FONT_HEIGHT 15
//8
#define HUD_DIGITAL_NUMBERS_WIDTH 14
#define HUD_DIGITAL_NUMBERS_HEIGHT 22

#define HUDLayout_RightmostTextCentre	-40

#define	HUDLayout_Health_TopY			10
#define	HUDLayout_Armour_TopY			60

/* KJL 15:28:12 09/06/98 - the following are pixels from the bottom of the screen */
#define HUDLayout_Rounds_TopY			40
#define HUDLayout_Magazines_TopY 		90
#define HUDLayout_AmmoDesc_TopY 		115


#define HUDLayout_Colour_BrightWhite	0xffffffff
#define HUDLayout_Colour_MarineGreen	((255<<24)+(95<<16)+(179<<8)+(39))
#define HUDLayout_Colour_MarineRed	((255<<24)+(255<<16))
#define HUDLayout_Linespacing			16

#ifdef __cplusplus
extern "C"
{
#endif
extern char AAFontWidths[];

/* HUD atlas UVs are ABSOLUTE PIXELS against the size the art was authored at
   (stock MarineHUD.RIM is 256x256: tracker 1..129, blue bar V=223, gunsight
   U=227). An HD texture pack replaces an atlas with a larger image, leaving
   every one of those UVs describing a sub-rectangle so the art renders
   magnified — HD Redux ships a 1024x1024 MarineHUD, i.e. 4x.

   HUD_AtlasUVScale returns actual/stock in 16.16 (ONE_FIXED when there is
   nothing to correct) so UVs can be rescaled to whatever was actually loaded.

   The stock size is PER ATLAS and must be registered with
   HUD_SetAtlasStockSize; an unregistered atlas is never rescaled. Do not
   reintroduce a single global reference: Draw_HUDImage is shared by atlases
   that are 256x256 (MarineHUD, predNumbers, partclfx) AND ones that are
   128x128 (HUDfonts, static, AlienTongue, cloudy, burn), so one constant is
   guaranteed to corrupt the other half's UVs — on stock assets, not just
   modded ones.

   Both live in opengl.c, where the texture object is; declared here because
   hud_layout.h is the header it and d3d_hud.cpp already share. */
void HUD_SetAtlasStockSize(int imageNumber, int stockSize);
int  HUD_AtlasUVScale(int imageNumber);
#ifdef __cplusplus
};
#endif
