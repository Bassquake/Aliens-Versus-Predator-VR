extern "C"
{

#include <math.h>

#include "3dc.h"
#include "module.h"
#include "inline.h"
#include "gammacontrol.h"

/* main.c. Non-VR builds get a stub returning 0, so this links everywhere. The
   whole file is already inside an extern "C" block, so no linkage spec here. */
extern int VR_HeadsetActive(void);

static int ActualGammaSetting;
int RequestedGammaSetting;

unsigned char GammaValues[256];

void InitialiseGammaSettings(int gamma)
{
	ActualGammaSetting = gamma+1;
	RequestedGammaSetting = gamma;
	UpdateGammaSettings();
}

void UpdateGammaSettings(void)
{
	if (RequestedGammaSetting==ActualGammaSetting) return;

	/* Map the 0..255 slider (128 = neutral, matching the old curve) to a real
	   gamma exponent applied as out = in^exponent. Above 128 brightens
	   (exponent < 1), below 128 darkens (exponent > 1). The exponential mapping
	   gives a much more pronounced effect across the slider than the old gentle
	   double-quadratic, which stayed close to identity except in the midtones.
	   The /96 divisor sets the strength: the ends reach roughly gamma 0.4
	   (brightest) and 2.5 (darkest). */
	int g = RequestedGammaSetting;

	/* The VR headset display renders noticeably darker than the flat-screen
	   build, crushing shadow detail: the same slider value looks roughly a
	   quarter of the range darker in the headset (flat-screen at ~25% matches VR
	   at ~50%). Bias the gamma brighter on VR by that amount so mid-slider lifts
	   the shadows to match the flat-screen look. Still fully adjustable — the
	   slider can go darker or brighter from there.

	   Runtime test, not #ifdef __ANDROID__. The condition is "the display is a
	   headset", which that guard got wrong in both directions: PCVR headsets
	   never got the compensation, and the non-VR Android phone flavor
	   (AVP_DISABLE_XR) got it despite rendering to a flat screen. A PCVR exe
	   running flat with no runtime is likewise excluded.

	   VR_HeadsetActive() rather than VR_SessionActive(): gamma is computed once
	   from InitialiseGammaSettings, right after SetOGLVideoMode, and the early-out
	   above means it is not recomputed unless the slider moves. XR init has
	   finished by then (xr_enabled set) but the session is not RUNNING yet, so
	   VR_SessionActive() would still be false and the bias would never apply. */
	if (VR_HeadsetActive())
	{
		g += 64;
	}

	float exponent = powf(2.0f, (float)(128 - g) / 96.0f);

	for (int i=0; i<=255; i++)
	{
		float in  = (float)i / 255.0f;
		float gammaed = powf(in, exponent);

		/* Weight the gamma change toward the dark end so it lifts (or deepens)
		   the shadows without touching the highlights. The weight is ~1 at black
		   and falls to 0 at white, so bright areas stay near identity instead of
		   being pushed up and washing out/over-exposing. Squaring the falloff
		   concentrates the effect further into the shadows and protects the
		   highlights (and upper midtones) more strongly. */
		float falloff = 1.0f - in;
		float weight  = falloff * falloff;
		float out = in + (gammaed - in) * weight;

		int a = (int)(out * 255.0f + 0.5f);

		if (a<0) a=0;
		if (a>255) a=255;

		GammaValues[i]=(unsigned char)a;
	}

	ActualGammaSetting=RequestedGammaSetting;

}

};
