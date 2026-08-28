#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "fixer.h"

#include "strtab.hpp"

#include "3dc.h"
#include "inline.h"
#include "awtexld.h"
#include "chnktexi.h"
#include "hud_layout.h"

#include "avp_menus.h"

#define UseLocalAssert Yes
#include "ourasert.h"
#include "ffstdio.h"

char AAFontWidths[256];

extern SDL_Surface *surface;
extern SCREENDESCRIPTORBLOCK ScreenDescriptorBlock;

extern int CloudTable[128][128];
extern int CloakingPhase;

/* --- HD texture-pack headroom for the additive menu compositor -------------
   Every bright element of the frontend is blended ADDITIVELY into the 640x480
   surface (see RenderMenuText and DrawAvPMenuGfx): the glyphs, the selected
   item's glow, and the backdrop all sum and clamp at white. That only holds
   together because the stock art is dark -- menus\starfield.rim is nearly
   black, the glow peaks at 86/255, and menus\fractal.rim (which becomes
   CloudTable, the per-pixel modulation the text is drawn through) averages
   just over half brightness.

   An HD pack replaces all three with much brighter art and the sum saturates,
   so the text renders as flat pure white and the glow as a solid slab. Measured
   against AvP Redux 2.0's tex37.ffl, which is what the game actually loads
   (the fastfile is consulted before the loose graphics\ tree):

     menus\fractal.rim   red mean 139/255 (0.546) -> 247/255 (0.970), and 93%
                         of its pixels now sit above 0.9, so the cloud
                         modulation is gone and every glyph pixel is added at
                         essentially full strength.
     menus\glowy_*.rim   peak channel 86 -> 198, i.e. the glow is ~2.3x hotter
                         (and green rather than blue-grey).

   Both are normalised back to the stock levels below, which restores the
   headroom the compositor assumes while leaving the pack's hue and shape
   alone. The two constants are MEASURED from the stock assets, and both
   corrections are exact no-ops on stock art -- the scale works out to 1.0 --
   so the unmodded frontend is byte-identical. */
#define MENU_CLOUD_STOCK_MEAN  35810   /* stock fractal.rim red mean, 16.16 (139.34/255) */
#define MENU_GLOW_STOCK_PEAK   86      /* stock glowy_*.rim peak channel, 0..255         */

/* Scale applied to the glow's alpha in RenderMenuText / RenderMenuText_Clipped,
   16.16. ONE_FIXED (no change) for stock art; measured from the loaded glow in
   LoadAllAvPMenuGfx below. */
static int MenuGlowScale = ONE_FIXED;

AVPMENUGFX AvPMenuGfxStorage[MAX_NO_OF_AVPMENUGFXS] =
{
	{"menus\\fractal.rim"},
	{"common\\aa_font.rim"},// Warning! Texture from common used

	{"menus\\copyright.rim"},

	{"menus\\fiandrd.rim"},
	{"menus\\presents.rim"},
	{"menus\\aliensvpredator.rim"},
	
	{"menus\\sliderbar.rim"},//AVPMENUGFX_SLIDERBAR,
	{"menus\\slider.rim"},//AVPMENUGFX_SLIDER,

	{"menus\\starfield.rim"},
	{"menus\\aliens.rim"},
	{"menus\\alien.rim"},
	{"menus\\marine.rim"},
	{"menus\\predator.rim"},

	{"menus\\glowy_left.rim"},
	{"menus\\glowy_middle.rim"},
	{"menus\\glowy_right.rim"},
	
	// Marine level
	{"menus\\marineepisode1.rim"},
	{"menus\\marineepisode2.rim"},
	{"menus\\marineepisode3.rim"},
	{"menus\\marineepisode4.rim"},
	{"menus\\marineepisode5.rim"},
	{"menus\\marineepisode6.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	
	// Predator level
	{"menus\\predatorepisode1.rim"},
	{"menus\\predatorepisode2.rim"},
	{"menus\\predatorepisode3.rim"},
	{"menus\\predatorepisode4.rim"},
	{"menus\\predatorepisode5.rim"},
	{"menus\\predatorepisode5.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},

	// Alien level
	{"menus\\alienepisode2.rim"},
	{"menus\\alienepisode4.rim"},
	{"menus\\alienepisode1.rim"},
	{"menus\\alienepisode3.rim"},
	{"menus\\alienepisode5.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},
	{"menus\\bonus.rim"},

	// Splash screens
#if MARINE_DEMO
	{"marinesplash\\splash00.rim"},
	{"marinesplash\\splash01.rim"},
	{"marinesplash\\splash02.rim"},
	{"marinesplash\\splash03.rim"},
	{"marinesplash\\splash04.rim"},
	{"marinesplash\\splash05.rim"},
#elif ALIEN_DEMO
	{"aliensplash\\splash00.rim"},
	{"aliensplash\\splash01.rim"},
	{"aliensplash\\splash02.rim"},
	{"aliensplash\\splash03.rim"},
	{"aliensplash\\splash04.rim"},
	{"aliensplash\\splash05.rim"},
#else
	{"predatorsplash\\splash00.rim"},
	{"predatorsplash\\splash01.rim"},
	{"predatorsplash\\splash02.rim"},
	{"predatorsplash\\splash03.rim"},
	{"predatorsplash\\splash04.rim"},
	{"predatorsplash\\splash05.rim"},
#endif
};

static void DrawAvPMenuGlowyBar(int topleftX, int topleftY, int alpha, int length)
{
	enum AVPMENUGFX_ID menuGfxID = AVPMENUGFX_GLOWY_MIDDLE;
	unsigned char *srcPtr;
	unsigned short *destPtr;
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	
	gfxPtr = &AvPMenuGfxStorage[menuGfxID];
	image = gfxPtr->ImagePtr;
	srcPtr = image->buf;
	
	if (ScreenDescriptorBlock.SDB_Width - topleftX < length) {
		length = ScreenDescriptorBlock.SDB_Width - topleftX;
	}
	if (length<0) length = 0;
	
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	if (alpha>ONE_FIXED) {
		int x, y;
		
		for (y=topleftY; y<gfxPtr->Height+topleftY; y++) {
			destPtr = (unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch) + topleftX;
			
			for (x=0; x<length; x++) {
				*destPtr =	((srcPtr[0]>>3)<<11) |
						((srcPtr[1]>>2)<<5 ) |
						((srcPtr[2]>>3));
				destPtr++;
			}
			
			srcPtr += image->w * 4;
		}
	} else {
		int x, y;
		
		for (y=topleftY; y<gfxPtr->Height+topleftY; y++) {
			destPtr = ((unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch)) + topleftX;
			
			for (x=0; x<length; x++) {
				if (srcPtr[0] || srcPtr[1] || srcPtr[2]) {
					unsigned int destR, destG, destB;
						
					destR = (*destPtr & 0xF800)>>8;
					destG = (*destPtr & 0x07E0)>>3;
					destB = (*destPtr & 0x001F)<<3;
						
					destR += MUL_FIXED(alpha, srcPtr[0]);
					destG += MUL_FIXED(alpha, srcPtr[1]);
					destB += MUL_FIXED(alpha, srcPtr[2]);
					if (destR > 0x00FF) destR = 0x00FF;
					if (destG > 0x00FF) destG = 0x00FF;
					if (destB > 0x00FF) destB = 0x00FF;
						
					*destPtr =	((destR>>3)<<11) |
							((destG>>2)<<5 ) |
							((destB>>3));
				}
				
				destPtr++;
			}
			
			srcPtr += image->w * 4;
		}
	}
		
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
}

static void DrawAvPMenuGlowyBar_Clipped(int topleftX, int topleftY, int alpha, int length, int topY, int bottomY)
{
	enum AVPMENUGFX_ID menuGfxID = AVPMENUGFX_GLOWY_MIDDLE;
	unsigned char *srcPtr;
	unsigned short *destPtr;
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	
	gfxPtr = &AvPMenuGfxStorage[menuGfxID];
	image = gfxPtr->ImagePtr;
	srcPtr = image->buf;
	
	if (length<0) length = 0;
	
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	if (alpha>ONE_FIXED) {
		int x, y;
		
		for (y=topleftY; y<gfxPtr->Height+topleftY; y++) {
			if(y>=topY && y<=bottomY) {
				destPtr = (unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch) + topleftX;
			
				for (x=0; x<length; x++) {
					*destPtr =	((srcPtr[0]>>3)<<11) |
							((srcPtr[1]>>2)<<5 ) |
							((srcPtr[2]>>3));
					destPtr++;
				}	
			}
			srcPtr += image->w * 4;
		}
	} else {
		int x, y;
		
		for (y=topleftY; y<gfxPtr->Height+topleftY; y++) {
			if(y>=topY && y<=bottomY) {
				destPtr = ((unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch)) + topleftX;
			
				for (x=0; x<length; x++) {				
					if (srcPtr[0] || srcPtr[1] || srcPtr[2]) {
						unsigned int destR, destG, destB;
						
						destR = (*destPtr & 0xF800)>>8;
						destG = (*destPtr & 0x07E0)>>3;
						destB = (*destPtr & 0x001F)<<3;
						
						destR += MUL_FIXED(alpha, srcPtr[0]);
						destG += MUL_FIXED(alpha, srcPtr[1]);
						destB += MUL_FIXED(alpha, srcPtr[2]);
						if (destR > 0x00FF) destR = 0x00FF;
						if (destG > 0x00FF) destG = 0x00FF;
						if (destB > 0x00FF) destB = 0x00FF;
						
						*destPtr =	((destR>>3)<<11) |
								((destG>>2)<<5 ) |
								((destB>>3));
					}
				
					destPtr++;
				}
			}	
			srcPtr += image->w * 4;
		}
	}
		
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
}

typedef struct AVPIndexedFont
{
	AVPMENUGFX info;	/* graphic info */
	int swidth;		/* width for space */
	int ascii;		/* ascii code for initial character */
	int height;		/* height per character */
	
	int numchars;
	int FontWidth[256];
} AVPIndexedFont;

AVPIndexedFont IntroFont_Light;

static void LoadMenuFont()
{
	AVPMENUGFX *gfxPtr;
	char buffer[100];
	size_t fastFileLength;
	void const *pFastFileData;
	
	IntroFont_Light.height = 33;
	IntroFont_Light.swidth = 5;
	IntroFont_Light.ascii = 32;
		
	gfxPtr = &IntroFont_Light.info;
	
	CL_GetImageFileName(buffer, 100, "menus\\introfont.rim", LIO_RELATIVEPATH);
	
	pFastFileData = ffreadbuf(buffer, &fastFileLength);
	
	if (pFastFileData) {
		gfxPtr->ImagePtr = AwCreateSurface(
			"pxfXY",
			pFastFileData,
			fastFileLength,
			AW_TLF_TRANSP|AW_TLF_CHROMAKEY,
			&(gfxPtr->Width),
			&(gfxPtr->Height)
		);
	} else {
		gfxPtr->ImagePtr = AwCreateSurface(
			"sfXY",
			buffer,
			AW_TLF_TRANSP|AW_TLF_CHROMAKEY,
			&(gfxPtr->Width),
			&(gfxPtr->Height)
		);
	}
	
	GLOBALASSERT(gfxPtr->ImagePtr);
	GLOBALASSERT(gfxPtr->Width>0);
	GLOBALASSERT(gfxPtr->Height>0);
	
	gfxPtr->hBackup = 0;
	
{
	D3DTexture *image = gfxPtr->ImagePtr;
	unsigned char *srcPtr = image->buf;
	int c;
	
	if ((image->w != 30) || ((image->h % 33) != 0)) {
		fprintf(stderr, "ERROR: I am going to give up now, because I don't like your font!\n");
		fprintf(stderr, "Font Size: %d x %d\n", image->w, image->h);
		exit(EXIT_FAILURE);
	}
	
	IntroFont_Light.numchars = image->h / 33;	
	IntroFont_Light.FontWidth[32] = 5;
	
	for (c=33; c<(32+IntroFont_Light.numchars); c++) {
		int x,y;
		int y1 = 1+(c-32)*33;
		
		IntroFont_Light.FontWidth[c]=31;
		
		for (x=29; x>0; x--) {
			int blank = 1;
			
			for (y=y1; y<y1+31; y++) {
				unsigned char *s = &srcPtr[(x + y*image->w) * 4];
				if (s[2]) {
					blank = 0;
					break;
				}
			}
			
			if (blank) {
				IntroFont_Light.FontWidth[c]--;
			} else {
				break;
			}
		}
	}
}

}

static void UnloadMenuFont()
{
	ReleaseDDSurface(IntroFont_Light.info.ImagePtr);
	IntroFont_Light.info.ImagePtr = NULL;
}

int LengthOfMenuText(const char *textPtr)
{
	int width = 0;
	
	while (textPtr && *textPtr) {
		width += IntroFont_Light.FontWidth[(unsigned char) *textPtr];
		
		textPtr++;
	}
	return width;
}

int LengthOfSmallMenuText(char *textPtr)
{
	int width = 0;
	
	while (textPtr && *textPtr) {
		width += AAFontWidths[(unsigned char) *textPtr];
		
		textPtr++;
	}
	return width;
}

int RenderMenuText(char *textPtr, int sx, int sy, int alpha, enum AVPMENUFORMAT_ID format)
{
	int width;
	
	width = LengthOfMenuText(textPtr);
	
	switch(format) {
		default:
			GLOBALASSERT("UNKNOWN TEXT FORMAT"==0);
			return 0;
		case AVPMENUFORMAT_LEFTJUSTIFIED:
			break;
		case AVPMENUFORMAT_RIGHTJUSTIFIED:
			sx -= width;
			break;
		case AVPMENUFORMAT_CENTREJUSTIFIED:
			sx -= width / 2;
			break;
	}
	
	LOCALASSERT(x>0);
	
	if (alpha >BRIGHTNESS_OF_DARKENED_ELEMENT) {
		int size = width - 18;
		int glowAlpha = MUL_FIXED(alpha, MenuGlowScale);

		if (size<18) size = 18;

		DrawAvPMenuGfx(AVPMENUGFX_GLOWY_LEFT,sx+18,sy-8,glowAlpha,AVPMENUFORMAT_RIGHTJUSTIFIED);
		DrawAvPMenuGlowyBar(sx+18,sy-8,glowAlpha,size-18);
		DrawAvPMenuGfx(AVPMENUGFX_GLOWY_RIGHT,sx+size,sy-8,glowAlpha,AVPMENUFORMAT_LEFTJUSTIFIED);
	}
{
	unsigned char *srcPtr;
	unsigned short *destPtr;
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	
	gfxPtr = &IntroFont_Light.info;
	image = gfxPtr->ImagePtr;
	
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return 0; /* ... */
		}
	}
	
	while( *textPtr ) {
		char c = *textPtr++;

		if (c>=' ') {
			unsigned int topLeftU = 0;
			unsigned int topLeftV = 1+(c-32)*33;
			unsigned int x, y;
			unsigned int width = IntroFont_Light.FontWidth[(unsigned char) c];
			unsigned int remainder = 0;
			unsigned int stride = width;

			if (image->w > width) {
				remainder = image->w - width;
			} else {
				stride = image->w;
			}

			srcPtr = &image->buf[(topLeftU+topLeftV*image->w)*4];

			for (y=sy; y<33+sy; y++) {
				destPtr = (unsigned short *)(((unsigned char *)surface->pixels)+y*surface->pitch) + sx;
				
				for (x=stride; x>0; x--) {
					if (srcPtr[0] || srcPtr[1] || srcPtr[2]) {
						unsigned int destR, destG, destB;
						
						int r = CloudTable[(x+sx+CloakingPhase/64)&127][(y+CloakingPhase/128)&127];
						r = MUL_FIXED(alpha, r);
						
						destR = (*destPtr & 0xF800)>>8;
						destG = (*destPtr & 0x07E0)>>3;
						destB = (*destPtr & 0x001F)<<3;
						
						destR += MUL_FIXED(r, srcPtr[0]);
						destG += MUL_FIXED(r, srcPtr[1]);
						destB += MUL_FIXED(r, srcPtr[2]);
						if (destR > 0x00FF) destR = 0x00FF;
						if (destG > 0x00FF) destG = 0x00FF;
						if (destB > 0x00FF) destB = 0x00FF;
						
						*destPtr =	((destR>>3)<<11) |
								((destG>>2)<<5 ) |
								((destB>>3));
					}
					srcPtr += 4;
					destPtr++;
				}
				srcPtr += remainder * 4;
			}
			sx += width;
		}
	}

	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
	
	return sx;
}
	
}

int RenderMenuText_Clipped(char *textPtr, int sx, int sy, int alpha, enum AVPMENUFORMAT_ID format, int topY, int bottomY)
{	
	int width = LengthOfMenuText(textPtr);
	
	switch(format) {
		default:
			GLOBALASSERT("UNKNOWN TEXT FORMAT"==0);
			return 0;
		case AVPMENUFORMAT_LEFTJUSTIFIED:
			break;
		case AVPMENUFORMAT_RIGHTJUSTIFIED:
			sx -= width;
			break;
		case AVPMENUFORMAT_CENTREJUSTIFIED:
			sx -= width / 2;
			break;
	}
	
	LOCALASSERT(x>0);
	
	if (alpha > BRIGHTNESS_OF_DARKENED_ELEMENT) {
		int size = width - 18;
		int glowAlpha = MUL_FIXED(alpha, MenuGlowScale);

		if (size<18) size = 18;

		DrawAvPMenuGfx_Clipped(AVPMENUGFX_GLOWY_LEFT,sx+18,sy-8,glowAlpha,AVPMENUFORMAT_RIGHTJUSTIFIED,topY,bottomY);
		DrawAvPMenuGlowyBar_Clipped(sx+18,sy-8,glowAlpha,size-18,topY,bottomY);
		DrawAvPMenuGfx_Clipped(AVPMENUGFX_GLOWY_RIGHT,sx+size,sy-8,glowAlpha,AVPMENUFORMAT_LEFTJUSTIFIED,topY,bottomY);
	}
{
	unsigned char *srcPtr;
	unsigned short *destPtr;
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	
	gfxPtr = &IntroFont_Light.info;
	image = gfxPtr->ImagePtr;
	
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return 0; /* ... */
		}
	}
	
	while( *textPtr ) {
		char c = *textPtr++;

		if (c>=' ') {
			unsigned int topLeftU = 0;
			unsigned int topLeftV = 1+(c-32)*33;
			unsigned int x, y;
			unsigned int width = IntroFont_Light.FontWidth[(unsigned char) c];
			unsigned int remainder = 0;
			unsigned int stride = width;

			if (image->w > width) {
				remainder = image->w - width;
			} else {
				stride = image->w;
			}

			srcPtr = &image->buf[(topLeftU+topLeftV*image->w)*4];

			for (y=sy; y<33+sy; y++) {
				if(y>=topY && y<=bottomY) {
					destPtr = (unsigned short *)(((unsigned char *)surface->pixels)+y*surface->pitch) + sx;
				
					for (x=stride; x>0; x--) {
						if (srcPtr[0] || srcPtr[1] || srcPtr[2]) {
							unsigned int destR, destG, destB;
						
							int r = CloudTable[(x+sx+CloakingPhase/64)&127][(y+CloakingPhase/128)&127];
							r = MUL_FIXED(alpha, r);
							
							destR = (*destPtr & 0xF800)>>8;
							destG = (*destPtr & 0x07E0)>>3;
							destB = (*destPtr & 0x001F)<<3;
						
							destR += MUL_FIXED(r, srcPtr[0]);
							destG += MUL_FIXED(r, srcPtr[1]);
							destB += MUL_FIXED(r, srcPtr[2]);
							if (destR > 0x00FF) destR = 0x00FF;
							if (destG > 0x00FF) destG = 0x00FF;
							if (destB > 0x00FF) destB = 0x00FF;
						
							*destPtr =	((destR>>3)<<11) |
									((destG>>2)<<5 ) |
									((destB>>3));
						}
						srcPtr += 4;
						destPtr++;
					}
					srcPtr += remainder * 4;
				} else {
					srcPtr += image->w * 4;
				}
			}
			sx += width;
		}
	}

	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
	
	return sx;
}

}

static int RenderSmallFontString(char *textPtr,int sx,int sy,int alpha, int red, int green, int blue)
{
	unsigned char *srcPtr;
	unsigned short *destPtr;
	int alphaR = MUL_FIXED(alpha,red);
	int alphaG = MUL_FIXED(alpha,green);
	int alphaB = MUL_FIXED(alpha,blue);
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	
	gfxPtr = &AvPMenuGfxStorage[AVPMENUGFX_SMALL_FONT];
	image = gfxPtr->ImagePtr;
	
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return 0; /* ... */
		}
	}
	
	while( *textPtr ) {
		char c = *textPtr++;

		if (c>=' ') {
			int topLeftU = 1+((c-32)&15)*16;
			int topLeftV = 1+((c-32)>>4)*16;
			int x, y;
			
			srcPtr = &image->buf[(topLeftU+topLeftV*image->w)*4];
			
			for (y=sy; y<HUD_FONT_HEIGHT+sy; y++) {
				destPtr = (unsigned short *)(((unsigned char *)surface->pixels)+y*surface->pitch) + sx;
				
				for (x=0; x<HUD_FONT_WIDTH; x++) {
					if (srcPtr[0] || srcPtr[1] || srcPtr[2]) {
						unsigned int destR, destG, destB;
						
						destR = (*destPtr & 0xF800)>>8;
						destG = (*destPtr & 0x07E0)>>3;
						destB = (*destPtr & 0x001F)<<3;
						
						destR += MUL_FIXED(alphaR, srcPtr[0]);
						destG += MUL_FIXED(alphaG, srcPtr[1]);
						destB += MUL_FIXED(alphaB, srcPtr[2]);
						if (destR > 0x00FF) destR = 0x00FF;
						if (destG > 0x00FF) destG = 0x00FF;
						if (destB > 0x00FF) destB = 0x00FF;
						
						*destPtr =	((destR>>3)<<11) |
								((destG>>2)<<5 ) |
								((destB>>3));
					}
					srcPtr += 4;
					destPtr++;
				}
				srcPtr += (image->w - HUD_FONT_WIDTH) * 4;	
			}
			sx += AAFontWidths[(unsigned char) c];
		}
	}

	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
	
	return sx;
}

void RenderSmallFontString_Wrapped(const char *textPtr,RECT* area,int alpha,int* output_x,int* output_y)
{
	unsigned char *srcPtr;
	unsigned short *destPtr;
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	int wordWidth;
	int sx=area->left;
	int sy=area->top;
	
	gfxPtr = &AvPMenuGfxStorage[AVPMENUGFX_SMALL_FONT];
	image = gfxPtr->ImagePtr;

/*
Determine area used by text , so we can draw it centrally
*/                        	
{
	const char *textPtr2=textPtr;
	while (*textPtr2) {
		int widthFromSpaces=0;
		int widthFromChars=0;
		
		while(*textPtr2 && *textPtr2==' ') {
			widthFromSpaces+=AAFontWidths[(unsigned char) *textPtr2++];
		}
		
		while(*textPtr2 && *textPtr2!=' ') {
			widthFromChars+=AAFontWidths[(unsigned char) *textPtr2++];
		}
		
		wordWidth=widthFromSpaces+widthFromChars;
		
		if(wordWidth> area->right-sx) {
			if(wordWidth >area->right-area->left) {
				int extraLinesNeeded=0;
				
				wordWidth-=(area->right-sx);
				
				sy+=HUD_FONT_HEIGHT;
				sx=area->left;
				
				extraLinesNeeded=wordWidth/(area->right-area->left);
				
				sy+=HUD_FONT_HEIGHT*extraLinesNeeded;
				wordWidth %= (area->right-area->left);
				
				if(sy+HUD_FONT_HEIGHT> area->bottom) break;
			} else {
				sy+=HUD_FONT_HEIGHT;
				sx=area->left;
				
				if(sy+HUD_FONT_HEIGHT> area->bottom) break;
				
				if(wordWidth> area->right-sx) break;
				
				wordWidth-=widthFromSpaces;
			}
		}
		sx+=wordWidth;
	}
	
	if(sy==area->top) {
		sx=area->left+ (area->right-sx)/2;
	} else {
		sx=area->left;
	}
	
	sy+=HUD_FONT_HEIGHT;
	if(sy<area->bottom) {
		sy=area->top + (area->bottom-sy)/2;
	} else {
		sy=area->top;
	}
}

	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	while ( *textPtr ) {
		const char* textPtr2=textPtr;
		wordWidth=0;
		
		while(*textPtr2 && *textPtr2==' ') {
			wordWidth+=AAFontWidths[(unsigned char) *textPtr2++];
		}
		
		while(*textPtr2 && *textPtr2!=' ') {
			wordWidth+=AAFontWidths[(unsigned char) *textPtr2++];
		}
		
		if(wordWidth> area->right-sx) {
			if(wordWidth>area->right - area->left) {
				/* 
				  word is too long too fit on one line 
				  so we'll just have to allow it to be split
				 */
			} else {
				sy+=HUD_FONT_HEIGHT;
				sx=area->left;
				
				if(sy+HUD_FONT_HEIGHT> area->bottom) break;
				
				if(wordWidth> area->right-sx) break;
				
				while(*textPtr && *textPtr==' ') {
					textPtr++;
				}
			}
		}
		
		while(*textPtr && *textPtr==' ') {
			sx+=AAFontWidths[(unsigned char) *textPtr++];
		}
		
		if(sx>area->right) {
			while(sx>area->right) {
				sx-=(area->right-area->left);
				sy+=HUD_FONT_HEIGHT;
			}
			
			if(sy+HUD_FONT_HEIGHT> area->bottom) break;
		}
		
		while(*textPtr && *textPtr!=' ') {
			char c = *textPtr++;
			int letterWidth = AAFontWidths[(unsigned char) c];
			
			if(sx+letterWidth>area->right) {
				sx=area->left;
				sy+=HUD_FONT_HEIGHT;
				
				if(sy+HUD_FONT_HEIGHT> area->bottom) break;
			}
			
			if (c>=' ' && c<='z') {
				int topLeftU = 1+((c-32)&15)*16;
				int topLeftV = 1+((c-32)>>4)*16;
				int x, y;
				
				srcPtr = &image->buf[(topLeftU+topLeftV*image->w)*4];
				
				for (y=sy; y<HUD_FONT_HEIGHT+sy; y++) {
					destPtr = (unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch) + sx;
					
					for (x=0; x<HUD_FONT_WIDTH; x++) {
						if (srcPtr[0] || srcPtr[1] || srcPtr[2]) {
							unsigned int destR, destG, destB;
						
							destR = (*destPtr & 0xF800)>>8;
							destG = (*destPtr & 0x07E0)>>3;
							destB = (*destPtr & 0x001F)<<3;
						
							destR += MUL_FIXED(alpha, srcPtr[0]);
							destG += MUL_FIXED(alpha, srcPtr[1]);
							destB += MUL_FIXED(alpha, srcPtr[2]);
							if (destR > 0x00FF) destR = 0x00FF;
							if (destG > 0x00FF) destG = 0x00FF;
							if (destB > 0x00FF) destB = 0x00FF;
						
							*destPtr =	((destR>>3)<<11) |
									((destG>>2)<<5 ) |
									((destB>>3));
						}
						srcPtr += 4;
						destPtr++;
					}
					srcPtr += (image->w - HUD_FONT_WIDTH) * 4;	
				}
				sx += AAFontWidths[(unsigned char) c];
			}
		}
	}
	
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
	
	if(output_x) *output_x=sx;
	if(output_y) *output_y=sy;
}

int RenderSmallMenuText(char *textPtr, int x, int y, int alpha, enum AVPMENUFORMAT_ID format)
{
	int length;
	char *ptr;
	
	switch(format) {
		default:
			GLOBALASSERT("UNKNOWN TEXT FORMAT"==0);
			return 0;
		case AVPMENUFORMAT_LEFTJUSTIFIED:
			break;
		case AVPMENUFORMAT_RIGHTJUSTIFIED:
			length = 0;
			ptr = textPtr;
			
			while (*ptr) {
				length+=AAFontWidths[(unsigned char) *ptr++];
			}
			
			x -= length;
			break;
		case AVPMENUFORMAT_CENTREJUSTIFIED:
			length = 0;
			ptr = textPtr;
			
			while (*ptr) {
				length+=AAFontWidths[(unsigned char) *ptr++];
			}
			
			x -= length / 2;
			break;
	}
	
	LOCALASSERT(x>0);
	
	return RenderSmallFontString(textPtr,x,y,alpha,ONE_FIXED,ONE_FIXED,ONE_FIXED);
}

int RenderSmallMenuText_Coloured(char *textPtr, int x, int y, int alpha, enum AVPMENUFORMAT_ID format, int red, int green, int blue)
{
	int length;
	char *ptr;
	
	switch(format) {
		default:
			GLOBALASSERT("UNKNOWN TEXT FORMAT"==0);
			return 0;
		case AVPMENUFORMAT_LEFTJUSTIFIED:
			break;
		case AVPMENUFORMAT_RIGHTJUSTIFIED:
			length = 0;
			ptr = textPtr;
	
			while (*ptr) {
				length+=AAFontWidths[(unsigned char) *ptr++];
			}
			
			x -= length;
			break;
		case AVPMENUFORMAT_CENTREJUSTIFIED:
			length = 0;
			ptr = textPtr;
			
			while (*ptr) {
				length+=AAFontWidths[(unsigned char) *ptr++];
			}
			
			x -= length / 2;
			break;
	}
	
	LOCALASSERT(x>0);
	
	return RenderSmallFontString(textPtr,x,y,alpha,red,green,blue);			
}

static void CalculateWidthsOfAAFont()
{
	unsigned char *srcPtr;
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	int c;
	
	gfxPtr = &AvPMenuGfxStorage[AVPMENUGFX_SMALL_FONT];
	image = gfxPtr->ImagePtr;
	
	srcPtr = image->buf;
	
	AAFontWidths[32]=3;
	
	for (c=33; c<255; c++) {
		int x,y;
		int x1 = 1+((c-32)&15)*16;
		int y1 = 1+((c-32)>>4)*16;
		
		AAFontWidths[c]=17;
		
		for (x=x1+HUD_FONT_WIDTH; x>x1; x--) {
			int blank = 1;
			
			for (y=y1; y<y1+HUD_FONT_HEIGHT; y++) {
				unsigned char *s = &srcPtr[(x + y*image->w) * 4];
				if (s[2] >= 0x80) {
					blank = 0;
					break;
				}
			}
			
			if (blank) {
				AAFontWidths[c]--;
			} else {
				break;
			}
		}
	}
}

void RenderKeyConfigRectangle(int alpha)
{
	int x1 = 10;
	int x2 = ScreenDescriptorBlock.SDB_Width-10;
	int y1 = ScreenDescriptorBlock.SDB_Height/2+25-115;
	int y2 = ScreenDescriptorBlock.SDB_Height/2+25-115+250;
	int x,y;
	unsigned short c, *destPtr;
	
	c =	((MUL_FIXED(0xFF,alpha)>>3)<<11) |
		((MUL_FIXED(0xFF,alpha)>>2)<<5 ) |
		((MUL_FIXED(0xFF,alpha)>>3));
	
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	y = y1;
	destPtr = (unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch) + x1;
	for (x=x1; x<=x2; x++) {
		*destPtr |= c;
		destPtr++;
	}
	
	y = y2;
	destPtr = (unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch) + x1;
	for (x=x1; x<=x2; x++) {
		*destPtr |= c;
		destPtr++;
	}
	
	for (y=y1+1; y<y2; y++) {
		destPtr = (unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch) + x1;
		*destPtr |= c;
	}
	
	for (y=y1+1; y<y2; y++) {
		destPtr = (unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch) + x2;
		*destPtr |= c;
	}
	
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
}

void RenderHighlightRectangle(int x1, int y1, int x2, int y2, int r, int g, int b)
{
	int x, y;
	unsigned short c;

	c = 	((r>>3)<<11) |
		((g>>2)<<5 ) |
		((b>>3));

	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}

	for (y=y1; y<=y2; y++) {
		unsigned short *destPtr = (unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch) + x1;
		
		for (x = x1; x <= x2; x++) {
			*destPtr |= c;
			
			destPtr++;
		}
	}
	
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
}

void LoadAvPMenuGfx(enum AVPMENUGFX_ID menuGfxID)
{
	AVPMENUGFX *gfxPtr;
	char buffer[100];
	size_t fastFileLength;
	void const *pFastFileData;
	
	GLOBALASSERT(menuGfxID < MAX_NO_OF_AVPMENUGFXS);
		
	gfxPtr = &AvPMenuGfxStorage[menuGfxID];
	
	CL_GetImageFileName(buffer, 100, gfxPtr->FilenamePtr, LIO_RELATIVEPATH);

	pFastFileData = ffreadbuf(buffer, &fastFileLength);
	
	if (pFastFileData) {
		gfxPtr->ImagePtr = AwCreateSurface(
			"pxfXY",
			pFastFileData,
			fastFileLength,
			AW_TLF_TRANSP|AW_TLF_CHROMAKEY,
			&(gfxPtr->Width),
			&(gfxPtr->Height)
		);
	} else {
		gfxPtr->ImagePtr = AwCreateSurface(
			"sfXY",
			buffer,
			AW_TLF_TRANSP|AW_TLF_CHROMAKEY,
			&(gfxPtr->Width),
			&(gfxPtr->Height)
		);
	}
	
	GLOBALASSERT(gfxPtr->ImagePtr);
	GLOBALASSERT(gfxPtr->Width>0);
	GLOBALASSERT(gfxPtr->Height>0);
	
	gfxPtr->hBackup = 0;
}

static void ReleaseAvPMenuGfx(enum AVPMENUGFX_ID menuGfxID)
{
	AVPMENUGFX *gfxPtr;
	
	GLOBALASSERT(menuGfxID < MAX_NO_OF_AVPMENUGFXS);
	
	
	gfxPtr = &AvPMenuGfxStorage[menuGfxID];
	
	GLOBALASSERT(gfxPtr);
	GLOBALASSERT(gfxPtr->ImagePtr);
	
	ReleaseDDSurface(gfxPtr->ImagePtr);
	
	gfxPtr->ImagePtr = NULL;
}

void LoadAllAvPMenuGfx()
{
	int i;
	
	for (i = 0; i < AVPMENUGFX_WINNER_SCREEN; i++) {
		LoadAvPMenuGfx(i);
	}
	
	LoadMenuFont();
{
	unsigned char *srcPtr;
	AVPMENUGFX *gfxPtr = &AvPMenuGfxStorage[AVPMENUGFX_CLOUDY];
	D3DTexture *image;
	
	int x, y;
	
	image = gfxPtr->ImagePtr;
	srcPtr = image->buf;

	/* CloudTable is a fixed 128x128; clamp rather than trusting the loaded
	   image, since a texture pack is free to ship a different size here and
	   the original loop would run straight off the end of the array. */
	{
		int rows = gfxPtr->Height;
		int cols = gfxPtr->Width;

		if (rows > 128) rows = 128;
		if (cols > 128) cols = 128;

		for (y=0; y<rows; y++) {
			unsigned char *rowPtr = srcPtr;

			for (x=0; x<cols; x++) {

				int r = rowPtr[0];

				r = DIV_FIXED(r, 0xFF);
				CloudTable[x][y]=r;

				rowPtr += 4;
			}

			srcPtr += image->w * 4;
		}

		/* Pull an over-bright replacement fractal back to the stock average so
		   the additive text keeps its headroom (see the note by
		   MENU_CLOUD_STOCK_MEAN). Only ever darkens, and the 1/16 margin keeps
		   stock art -- whose mean IS the reference -- from being touched at all. */
		{
			int total = 0;
			int mean;
			int scale = ONE_FIXED;

			for (y=0; y<128; y++)
				for (x=0; x<128; x++)
					total += CloudTable[x][y] >> 8;   /* >>8 so 16384 entries cannot overflow */

			mean = (total / (128*128)) << 8;

			if (mean > MENU_CLOUD_STOCK_MEAN + MENU_CLOUD_STOCK_MEAN/16) {
				scale = DIV_FIXED(MENU_CLOUD_STOCK_MEAN, mean);

				for (y=0; y<128; y++)
					for (x=0; x<128; x++)
						CloudTable[x][y] = MUL_FIXED(CloudTable[x][y], scale);
			}

			/* Logged unconditionally so a pack that looks wrong can be diagnosed
			   without re-deriving these numbers by hand. Stock reads
			   "mean 0.543 -> scale 1.000 (untouched)". */
			SDL_Log("MENU: fractal %dx%d -> CloudTable mean %.3f (stock %.3f) -> scale %.3f%s",
			        cols, rows,
			        (double)mean / (double)ONE_FIXED,
			        (double)MENU_CLOUD_STOCK_MEAN / (double)ONE_FIXED,
			        (double)scale / (double)ONE_FIXED,
			        scale == ONE_FIXED ? " (untouched)" : "");
		}
	}
}

/* Match an HD pack's glow intensity to the stock art's, preserving its hue.
   Reads the peak channel of the stretched middle section, which is the part
   that covers most of the bar. ONE_FIXED (no change) when the glow is stock. */
{
	AVPMENUGFX *glowPtr = &AvPMenuGfxStorage[AVPMENUGFX_GLOWY_MIDDLE];
	D3DTexture *glowImage = glowPtr->ImagePtr;

	MenuGlowScale = ONE_FIXED;

	if (!glowImage || !glowImage->buf) {
		/* No glow art loaded at all — a pack whose fastfile omits it and which
		   has no loose copy either. Nothing to scale; say so rather than
		   silently reporting a peak of 0. */
		SDL_Log("MENU: glowy_middle absent -> glow scale 1.000 (untouched)");
	} else {
		unsigned char *p = glowImage->buf;
		int peak = 0;
		int x, y;

		for (y=0; y<glowPtr->Height; y++) {
			unsigned char *rowPtr = p;

			for (x=0; x<glowPtr->Width; x++) {
				if (rowPtr[0] > peak) peak = rowPtr[0];
				if (rowPtr[1] > peak) peak = rowPtr[1];
				if (rowPtr[2] > peak) peak = rowPtr[2];
				rowPtr += 4;
			}

			p += glowImage->w * 4;
		}

		if (peak > MENU_GLOW_STOCK_PEAK)
			MenuGlowScale = DIV_FIXED(MENU_GLOW_STOCK_PEAK, peak);

		/* Stock reads "peak 86 (stock 86) -> scale 1.000 (untouched)". */
		SDL_Log("MENU: glowy_middle %dx%d peak %d (stock %d) -> glow scale %.3f%s",
		        glowPtr->Width, glowPtr->Height, peak, MENU_GLOW_STOCK_PEAK,
		        (double)MenuGlowScale / (double)ONE_FIXED,
		        MenuGlowScale == ONE_FIXED ? " (untouched)" : "");
	}
}

	CalculateWidthsOfAAFont();
}
	
void LoadAllSplashScreenGfx()
{
	int i;
	
	for (i = AVPMENUGFX_SPLASH_SCREEN1; i < MAX_NO_OF_AVPMENUGFXS; i++) {
		LoadAvPMenuGfx(i);
	}
}

void InitialiseMenuGfx()
{
	int i;
	
	for (i = 0; i < MAX_NO_OF_AVPMENUGFXS; i++) {
		AvPMenuGfxStorage[i].ImagePtr = NULL;
	}	
}

void ReleaseAllAvPMenuGfx()
{
	int i;
	
	for (i = 0; i < MAX_NO_OF_AVPMENUGFXS; i++) {
		if (AvPMenuGfxStorage[i].ImagePtr) {
			ReleaseAvPMenuGfx(i);
		}
	}

	UnloadMenuFont();	
}

void DrawAvPMenuGfx(enum AVPMENUGFX_ID menuGfxID, int topleftX, int topleftY, int alpha,enum AVPMENUFORMAT_ID format)
{
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	unsigned char *srcPtr;
	unsigned short *destPtr;
	int length;

	GLOBALASSERT(menuGfxID < MAX_NO_OF_AVPMENUGFXS);
	
	gfxPtr = &AvPMenuGfxStorage[menuGfxID];
	image = gfxPtr->ImagePtr;
	
	switch(format) {
		default:
			GLOBALASSERT("UNKNOWN TEXT FORMAT"==0);
			return;
		case AVPMENUFORMAT_LEFTJUSTIFIED:
			break;
		case AVPMENUFORMAT_RIGHTJUSTIFIED:
			topleftX -= gfxPtr->Width;
			break;
		case AVPMENUFORMAT_CENTREJUSTIFIED:
			topleftX -= gfxPtr->Width/2;
			break;
	}
	
	srcPtr = (unsigned char *)image->buf;
	length = gfxPtr->Width;
	
	if (ScreenDescriptorBlock.SDB_Width - topleftX < length) {
		length = ScreenDescriptorBlock.SDB_Width - topleftX;
	}
	if (length <= 0) return;
			
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	if (alpha > ONE_FIXED) {
		int x, y;
		
		for (y=topleftY; y<gfxPtr->Height+topleftY; y++) {
			destPtr = ((unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch)) + topleftX;
			
			for (x=0; x<length; x++) {
				*destPtr =	((srcPtr[0]>>3)<<11) |
						((srcPtr[1]>>2)<<5 ) |
						((srcPtr[2]>>3));
				srcPtr += 4;
				destPtr++;
			}
			
			srcPtr += (image->w - length) * 4;
		}
	} else {
		int x, y;
		
		for (y=topleftY; y<gfxPtr->Height+topleftY; y++) {
			destPtr = ((unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch)) + topleftX;
			
			for (x=0; x<length; x++) {
				if (srcPtr[0] || srcPtr[1] || srcPtr[2]) {
					unsigned int destR, destG, destB;
						
					destR = (*destPtr & 0xF800)>>8;
					destG = (*destPtr & 0x07E0)>>3;
					destB = (*destPtr & 0x001F)<<3;
						
					destR += MUL_FIXED(alpha, srcPtr[0]);
					destG += MUL_FIXED(alpha, srcPtr[1]);
					destB += MUL_FIXED(alpha, srcPtr[2]);
					if (destR > 0x00FF) destR = 0x00FF;
					if (destG > 0x00FF) destG = 0x00FF;
					if (destB > 0x00FF) destB = 0x00FF;
						
					*destPtr =	((destR>>3)<<11) |
							((destG>>2)<<5 ) |
							((destB>>3));
				}
				
				srcPtr += 4;
				destPtr++;
			}
			
			srcPtr += (image->w - length) * 4;
		}
	}
		
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
}

void DrawAvPMenuGfx_CrossFade(enum AVPMENUGFX_ID menuGfxID,enum AVPMENUGFX_ID menuGfxID2,int alpha)
{
	AVPMENUGFX *gfxPtr, *gfxPtr2;
	D3DTexture *image, *image2;
	unsigned char *srcPtr, *srcPtr2;
	unsigned short *destPtr;
	int length;
	
	GLOBALASSERT(menuGfxID < MAX_NO_OF_AVPMENUGFXS);
	GLOBALASSERT(menuGfxID2 < MAX_NO_OF_AVPMENUGFXS);
	
	gfxPtr = &AvPMenuGfxStorage[menuGfxID];
	image = gfxPtr->ImagePtr;
	gfxPtr2 = &AvPMenuGfxStorage[menuGfxID2];
	image2 = gfxPtr2->ImagePtr;

	srcPtr = image->buf;
	srcPtr2 = image2->buf;
	
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	length = 640;
	
	if (alpha == ONE_FIXED) {
		int x, y;
		
		for (y=0; y<480; y++) {
			destPtr = ((unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch));
			
			for (x=0; x<640; x++) {
				*destPtr =	((srcPtr[0]>>3)<<11) |
						((srcPtr[1]>>2)<<5 ) |
						((srcPtr[2]>>3));
				srcPtr += 4;
				destPtr++;
			}
			srcPtr += (image->w - length) * 4;
		}
	} else {
		int x, y;
		
		for (y=0; y<480; y++) {
			destPtr = ((unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch));
			
			for (x=0; x<640; x++) {
				unsigned int srcR1, srcR2;
				unsigned int srcG1, srcG2;
				unsigned int srcB1, srcB2;
				
				srcR1 = srcPtr[0];
				srcR2 = srcPtr2[0];
				srcG1 = srcPtr[1];
				srcG2 = srcPtr2[1];
				srcB1 = srcPtr[2];
				srcB2 = srcPtr2[2];
				
				srcR2 = MUL_FIXED(ONE_FIXED-alpha,srcR2)+MUL_FIXED(alpha,srcR1);
				srcG2 = MUL_FIXED(ONE_FIXED-alpha,srcG2)+MUL_FIXED(alpha,srcG1);
				srcB2 = MUL_FIXED(ONE_FIXED-alpha,srcB2)+MUL_FIXED(alpha,srcB1);
				
				*destPtr =	((srcR2>>3)<<11) |
						((srcG2>>2)<<5 ) |
						((srcB2>>3));
				srcPtr += 4;
				srcPtr2 += 4;
				destPtr++;
			}
		}
	}
	
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
	
}

void DrawAvPMenuGfx_Faded(enum AVPMENUGFX_ID menuGfxID, int topleftX, int topleftY, int alpha,enum AVPMENUFORMAT_ID format)
{
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	unsigned char *srcPtr;
	unsigned short *destPtr;
	int length;

	GLOBALASSERT(menuGfxID < MAX_NO_OF_AVPMENUGFXS);
	
	gfxPtr = &AvPMenuGfxStorage[menuGfxID];
	image = gfxPtr->ImagePtr;
	
	switch(format) {
		default:
			GLOBALASSERT("UNKNOWN TEXT FORMAT"==0);
			return;
		case AVPMENUFORMAT_LEFTJUSTIFIED:
			break;
		case AVPMENUFORMAT_RIGHTJUSTIFIED:
			topleftX -= gfxPtr->Width;
			break;
		case AVPMENUFORMAT_CENTREJUSTIFIED:
			topleftX -= gfxPtr->Width/2;
			break;
	}
	
	srcPtr = (unsigned char *)image->buf;
	length = gfxPtr->Width;
	
	if (ScreenDescriptorBlock.SDB_Width - topleftX < length) {
		length = ScreenDescriptorBlock.SDB_Width - topleftX;
	}
	if (length <= 0) return;
			
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	{
		int x, y;
		
		for (y=topleftY; y<gfxPtr->Height+topleftY; y++) {
			destPtr = ((unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch)) + topleftX;
			
			for (x=0; x<length; x++) {
				unsigned int srcR,srcG,srcB;
				
				if (srcPtr[0] || srcPtr[1] || srcPtr[2]) {
					srcR = MUL_FIXED(srcPtr[0], alpha);
					srcG = MUL_FIXED(srcPtr[1], alpha);
					srcB = MUL_FIXED(srcPtr[2], alpha);
					*destPtr =	((srcR>>3)<<11) |
							((srcG>>2)<<5 ) |
							((srcB>>3));
				}
				
				srcPtr += 4;
				destPtr++;
			}
			
			srcPtr += (image->w - length) * 4;
		}
	}
		
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
}

void DrawAvPMenuGfx_Clipped(enum AVPMENUGFX_ID menuGfxID, int topleftX, int topleftY, int alpha,enum AVPMENUFORMAT_ID format, int topY, int bottomY)
{
	AVPMENUGFX *gfxPtr;
	D3DTexture *image;
	unsigned char *srcPtr;
	unsigned short *destPtr;
	int length;

	GLOBALASSERT(menuGfxID < MAX_NO_OF_AVPMENUGFXS);
	
	gfxPtr = &AvPMenuGfxStorage[menuGfxID];
	image = gfxPtr->ImagePtr;
	
	switch(format) {
		default:
			GLOBALASSERT("UNKNOWN TEXT FORMAT"==0);
			return;
		case AVPMENUFORMAT_LEFTJUSTIFIED:
			break;
		case AVPMENUFORMAT_RIGHTJUSTIFIED:
			topleftX -= gfxPtr->Width;
			break;
		case AVPMENUFORMAT_CENTREJUSTIFIED:
			topleftX -= gfxPtr->Width/2;
			break;
	}
	
	srcPtr = (unsigned char *)image->buf;
	length = gfxPtr->Width;
	
	if (ScreenDescriptorBlock.SDB_Width - topleftX < length) {
		length = ScreenDescriptorBlock.SDB_Width - topleftX;
	}
	if (length <= 0) return;
			
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	if (alpha > ONE_FIXED) {
		int x, y;
		
		for (y=topleftY; y<gfxPtr->Height+topleftY; y++) {
			destPtr = ((unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch)) + topleftX;
			
			if(y>=topY && y<=bottomY) {
				for (x=0; x<length; x++) {
					*destPtr =	((srcPtr[0]>>3)<<11) |
							((srcPtr[1]>>2)<<5 ) |
							((srcPtr[2]>>3));
					srcPtr += 4;
					destPtr++;
				}		
				srcPtr += (image->w - length) * 4;
			} else {
				srcPtr += image->w * 4;
			}
		}
	} else {
		int x, y;
		
		for (y=topleftY; y<gfxPtr->Height+topleftY; y++) {
			destPtr = ((unsigned short *)(((unsigned char *)surface->pixels) + y*surface->pitch)) + topleftX;
			
			if(y>=topY && y<=bottomY) {
				for (x=0; x<length; x++) {
					if (srcPtr[0] || srcPtr[1] || srcPtr[2]) {
						unsigned int destR, destG, destB;
						
						destR = (*destPtr & 0xF800)>>8;
						destG = (*destPtr & 0x07E0)>>3;
						destB = (*destPtr & 0x001F)<<3;
						
						destR += MUL_FIXED(alpha, srcPtr[0]);
						destG += MUL_FIXED(alpha, srcPtr[1]);
						destB += MUL_FIXED(alpha, srcPtr[2]);
						if (destR > 0x00FF) destR = 0x00FF;
						if (destG > 0x00FF) destG = 0x00FF;
						if (destB > 0x00FF) destB = 0x00FF;
						
						*destPtr =	((destR>>3)<<11) |
								((destG>>2)<<5 ) |
								((destB>>3));
					}
					srcPtr += 4;
					destPtr++;
				}
				srcPtr += (image->w - length) * 4;
			} else {
				srcPtr += image->w * 4;
			}
		}
	}
		
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
}

int HeightOfMenuGfx(enum AVPMENUGFX_ID menuGfxID)
{
	return AvPMenuGfxStorage[menuGfxID].Height;
}

void FadedScreen(int alpha)
{
	int x, y;
	unsigned short *ptr;
	
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	for (y = 60; y < surface->h-60; y++) {
		ptr = (unsigned short *)(((unsigned char *)surface->pixels)+y*surface->pitch);
		for (x = 0; x < surface->w; x++) {
			unsigned int srcR, srcG, srcB;
			
			srcR = (*ptr & 0xF800) >> 11;
			srcG = (*ptr & 0x07E0) >> 5;
			srcB = (*ptr & 0x001F);
			
			srcR = MUL_FIXED(srcR, alpha);
			srcG = MUL_FIXED(srcG, alpha);
			srcB = MUL_FIXED(srcB, alpha);
			*ptr =	((srcR>>3)<<11) |
				((srcG>>2)<<5 ) |
				((srcB>>3));
			ptr++;
		}
	}
	
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
}

void ClearScreenToBlack()
{	
	int x, y;
	unsigned short *ptr;
	
	if (SDL_MUSTLOCK(surface)) {
		if (SDL_LockSurface(surface) < 0) {
			return; /* ... */
		}
	}
	
	for (y = 0; y < surface->h; y++) {
		ptr = (unsigned short *)(((unsigned char *)surface->pixels)+y*surface->pitch);
		for (x = 0; x < surface->w; x++) {
			*ptr = 0;
			
			ptr++;
		}
	}
	
	if (SDL_MUSTLOCK(surface)) {
		SDL_UnlockSurface(surface);
	}
}
