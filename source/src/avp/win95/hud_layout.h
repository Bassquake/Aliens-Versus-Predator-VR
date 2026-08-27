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

/* Size the HUD's absolute-pixel UVs were authored against (the stock
   MarineHUD.RIM is 256x256: tracker 1..129, blue bar V=223, gunsight U=227).
   An HD texture pack replaces that atlas with a larger image, leaving every one
   of those UVs describing a sub-rectangle so the art renders magnified — HD
   Redux ships 1024x1024, i.e. 4x. HUD_AtlasUVScale returns actual/stock in
   16.16 (ONE_FIXED when there is nothing to correct) so UVs can be rescaled to
   whatever was actually loaded. Defined in opengl.c, which is where the texture
   object lives; declared here because hud_layout.h is the header both it and
   d3d_hud.cpp already share. */
#define HUD_ATLAS_STOCK_SIZE 256
int HUD_AtlasUVScale(int imageNumber, int stockSize);
#ifdef __cplusplus
};
#endif
