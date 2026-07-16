/*KJL***************************************
*    Language Internationalization Code    *
***************************************KJL*/
#include "3dc.h"
#include "inline.h"
#include "module.h"
#include "gamedef.h"


#include "langenum.h"
#include "language.h"
#include "huffman.hpp"

// DHM 12 Nov 97: hooks for C++ string handling code:
#include "strtab.hpp"

#define UseLocalAssert Yes
#include "ourasert.h"
#include "avp_menus.h"


#ifdef AVP_DEBUG_VERSION
	#define USE_LANGUAGE_TXT 0
#else
	#define USE_LANGUAGE_TXT 1
#endif

static char EmptyString[]="";

static char *TextStringPtr[MAX_NO_OF_TEXTSTRINGS] = { EmptyString };
static char *TextBufferPtr;

void InitTextStrings(void)
{
	char *filename;
	char *textPtr;
	int i;

	/* language select here! */
	GLOBALASSERT(AvP.Language>=0);
	GLOBALASSERT(AvP.Language<I_MAX_NO_OF_LANGUAGES);
	
#if MARINE_DEMO
	filename = "menglish.txt";
#elif ALIEN_DEMO
	filename = "aenglish.txt";
#elif USE_LANGUAGE_TXT
	filename = "language.txt";
#else
	filename = LanguageFilename[AvP.Language];
#endif
	TextBufferPtr = LoadTextFile(filename);
		
	if (TextBufferPtr == NULL) {
		/* NOTE:
		   if this load fails, then most likely the game is not 
		   installed correctly. 
		   SBF
		  */ 
		fprintf(stderr, "ERROR: unable to load %s language text file\n",
			 filename);
		exit(1);
	}
	
	if (!strncmp (TextBufferPtr, "REBCRIF1", 8))
	{
		textPtr = (char*)HuffmanDecompress((HuffmanPackage*)(TextBufferPtr)); 		
		DeallocateMem(TextBufferPtr);
		TextBufferPtr=textPtr;
	}
	else
	{
		textPtr = TextBufferPtr;
	}

	AddToTable( EmptyString );

	for (i=1; i<MAX_NO_OF_TEXTSTRINGS; i++)
	{	
		/* scan for a quote mark */
		while (*textPtr++ != '"')  {
			if (*textPtr == '@') {
				// should be an error as this language file
				// doesn't match the game.
				return; /* '@' should be EOF */
			}
		}

		/* now pointing to a text string after quote mark*/
		TextStringPtr[i] = textPtr;

		/* scan for a quote mark */
		while (*textPtr != '"')
		{	
			textPtr++;
		}

		/* change quote mark to zero terminator */
		*textPtr = 0;
		textPtr++;

		AddToTable( TextStringPtr[i] );
	}
}

void KillTextStrings(void)
{
	UnloadTextFile(LanguageFilename[AvP.Language],TextBufferPtr);

	UnloadTable();
}

char *GetTextString(enum TEXTSTRING_ID stringID)
{
	// Not good.
	// These strings do not exist in data.
	if (stringID > MIN_NEW_TEXTSTRINGS && stringID < MAX_NEW_TEXTSTRINGS) {
		switch (stringID) {
			case TEXTSTRING_MAINMENU_EXITGAME_HELP_NEW: return "Quit the game.";
			case TEXTSTRING_MAINMENU_CREDITS_NEW: return "Credits";
			case TEXTSTRING_MAINMENU_CREDITS_HELP_NEW: return "View the credits.";
			case TEXTSTRING_AVOPTIONS_CROSSHAIR: return "Show Crosshair";
			case TEXTSTRING_AVOPTIONS_CROSSHAIR_HELP: return "Crosshair helps with aiming.";
			case TEXTSTRING_AVOPTIONS_FRAMERATE: return "Show FPS";
			case TEXTSTRING_AVOPTIONS_FRAMERATE_HELP: return "Displays an FPS counter at top left.";
			case TEXTSTRING_AVOPTIONS_VR_REFRESH_RATE: return "Refresh Rate";
			case TEXTSTRING_AVOPTIONS_VR_REFRESH_RATE_HELP: return "Sets the refresh rate of the headset.";
			case TEXTSTRING_VR_REFRESH_72:  return "72 Hz (Default)";
			case TEXTSTRING_VR_REFRESH_80:  return "80 Hz";
			case TEXTSTRING_VR_REFRESH_90:  return "90 Hz";
			case TEXTSTRING_VR_REFRESH_120: return "120 Hz";
			case TEXTSTRING_FPS_OFF: return "Off (Default)";
			case TEXTSTRING_FPS_ON:  return "On";
			case TEXTSTRING_AVOPTIONS_MSAA:     return "Anti-Aliasing (MSAA)";
			case TEXTSTRING_AVOPTIONS_MSAA_HELP: return "Adjust the multi-sample anti-aliasing to reduce jaggies. May get frame dips if set at highest.";
			case TEXTSTRING_AVOPTIONS_MSAA_OFF: return "Off";
			case TEXTSTRING_AVOPTIONS_MSAA_2X:  return "2x (Default)";
			case TEXTSTRING_AVOPTIONS_MSAA_4X:  return "4x";
			case TEXTSTRING_AVOPTIONS_FSR:              return "FSR Upscaling";
			case TEXTSTRING_AVOPTIONS_FSR_OFF:          return "Off";
			case TEXTSTRING_AVOPTIONS_FSR_ULTRAQUALITY: return "Ultra Quality";
			case TEXTSTRING_AVOPTIONS_FSR_QUALITY:      return "Quality";
			case TEXTSTRING_AVOPTIONS_FSR_BALANCED:     return "Balanced";
			case TEXTSTRING_AVOPTIONS_FSR_PERFORMANCE:  return "Performance";
			case TEXTSTRING_CONTROLLERCONFIG_TITLE: return "Controller Configuration";
			case TEXTSTRING_CONTROLLERCONFIG_HELP:  return "Configure your controllers for playing the game.";
			case TEXTSTRING_CONTROLLERCONFIG_BACK:  return "Back";
			case TEXTSTRING_VRTURN_MODE:        return "Turning Mode";
			case TEXTSTRING_VRTURN_MODE_HELP:   return "Snap rotates in fixed steps; Smooth rotates continuously.";
			case TEXTSTRING_VRTURN_SNAP:        return "Snap (Default)";
			case TEXTSTRING_VRTURN_SMOOTH:      return "Smooth";
			case TEXTSTRING_VRSNAP_ANGLE:       return "Snap Turn Angle";
			case TEXTSTRING_VRSNAP_ANGLE_HELP:  return "How far each snap turn rotates. Used in Snap mode.";
			case TEXTSTRING_VRSNAP_30:          return "30 Degrees";
			case TEXTSTRING_VRSNAP_45:          return "45 Degrees (Default)";
			case TEXTSTRING_VRSNAP_60:          return "60 Degrees";
			case TEXTSTRING_VRSNAP_90:          return "90 Degrees";
			case TEXTSTRING_VRSMOOTH_SPEED:     return "Smooth Turn Speed";
			case TEXTSTRING_VRSMOOTH_SPEED_HELP:return "How fast you rotate. Used in Smooth mode.";
			case TEXTSTRING_VRSMOOTH_DEADZONE:     return "Smooth Turn Deadzone";
			case TEXTSTRING_VRSMOOTH_DEADZONE_HELP:return "How far the stick must move before turning. Used in Smooth mode.";
			case TEXTSTRING_VRVIGNETTE:          return "Comfort Vignette";
			case TEXTSTRING_VRVIGNETTE_HELP:     return "Darkens your peripheral vision while smooth turning to reduce motion sickness.";
			case TEXTSTRING_VRVIGNETTE_OFF:      return "Off";
			case TEXTSTRING_VRVIGNETTE_ON:       return "On (Default)";
			case TEXTSTRING_VRVIGNETTE_STRENGTH: return "Vignette Strength";
			case TEXTSTRING_VRVIGNETTE_STRENGTH_HELP:return "How much the peripheral vision closes in while smooth turning.";
			case TEXTSTRING_USERPROFILE_HELP_VR: return "Press X or A button to auto-select a name and again to Continue.";
			case TEXTSTRING_USERPROFILE_HELP_VR_CONTINUE: return "Press A to select a profile, or B to delete a profile.";
			case TEXTSTRING_USERPROFILE_HELP_VR_NEW: return "Create a New Profile.";
			case TEXTSTRING_SAVEPROGRESS_TITLE: return "Save progress?";
			case TEXTSTRING_MAINMENU_CHEATS: return "Extra Cheats";
			case TEXTSTRING_MAINMENU_CHEATS_HELP: return "Cheat options for single-player (not Skirmish or Multiplayer).";
			case TEXTSTRING_CHEATS_GIVEALLWEAPONS: return "Give all weapons";
			case TEXTSTRING_CHEATS_GIVEALLWEAPONS_HELP: return "Start each Single Player level with every weapon for your species.";
			case TEXTSTRING_CHEATS_GIVEALLWEAPONS_OFF: return "Off (Default)";
			case TEXTSTRING_CHEATS_GIVEALLWEAPONS_ON:  return "On";
			case TEXTSTRING_CHEATS_GODMODE: return "Invincibility";
			case TEXTSTRING_CHEATS_GODMODE_HELP: return "Become immortal in Single Player (not Skirmish or Multiplayer).";
			case TEXTSTRING_CHEATS_GODMODE_NO:  return "No (Default)";
			case TEXTSTRING_CHEATS_GODMODE_YES: return "Yes";
			default: break;
		}
	}

	if (stringID == TEXTSTRING_MAINMENU_SUBTITLE)
#ifdef __ANDROID__
		return "VR Edition";
#else
		return "Classic 2000";
#endif

	LOCALASSERT(stringID<MAX_NO_OF_TEXTSTRINGS);
	if (stringID < MAX_NO_OF_TEXTSTRINGS) {
		return TextStringPtr[stringID];
	}
	return EmptyString;
}
