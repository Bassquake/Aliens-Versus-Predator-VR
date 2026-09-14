/* KJL 15:17:31 10/12/98 - user profile stuff */
#include <stddef.h>   /* offsetof, for the short-profile check below */
#include "list_tem.hpp"
extern "C"
{
#include "3dc.h"
#include "inline.h"
#include "module.h"
#include "stratdef.h"

#include "avp_userprofile.h"
#include "padinput.h"
#include "language.h"
#include "gammacontrol.h"
#include <SDL3/SDL.h>
#include "opengl.h"   /* VR_ACTION / VR_SOURCE and the VRBinding table */

/* The profile is a raw blob, so its binding array is sized by a literal. Catch any
   drift between that and the enum at compile time rather than by silently writing
   past it. */
typedef char VRBindingProfileSizeCheck[
    (VR_SPECIES_COUNT <= 3 && VR_ACT_COUNT <= 12) ? 1 : -1];
#include "psnd.h"
#include "cd_player.h"

#define UseLocalAssert Yes
#include "ourasert.h"

 // Edmond
#include "pldnet.h"
#include <time.h>

static int LoadUserProfiles(void);

static void EmptyUserProfilesList(void);
static void InsertProfileIntoList(AVP_USER_PROFILE *profilePtr);
static int ProfileIsMoreRecent(AVP_USER_PROFILE *profilePtr, AVP_USER_PROFILE *profileToTestAgainstPtr);
static void SetDefaultProfileOptions(AVP_USER_PROFILE *profilePtr);

extern int SmackerSoundVolume;
extern int EffectsSoundVolume;
extern int MoviesAreActive;
extern int IntroOutroMoviesAreActive;
extern char MP_PlayerName[];
extern int AutoWeaponChangeOn;
extern int ShowCrosshair;
extern int ShowFrameRate;
extern int VRRefreshRateIndex;
extern int VRRefreshRateHz;
extern int VR_GetRefreshRateIndexForHz(float hz);
extern int VR_GetRefreshRateCount(void);
extern int MSAASampleIndex;
extern int AnisotropicFilterIndex;
extern int TextureFilterIndex;
extern int NPOTMipmapsEnabled;
extern int DesktopMirrorIndex;
extern int VRTurnMode;
extern int VRSnapAngleIndex;
extern int VRSmoothTurnSpeed;
extern int VRSmoothDeadzone;
extern int VRVignetteOn;
extern int VRClimbVignetteOn;
extern int VRClimbVignetteStrength;
extern int MarineLeftArmVisible;
extern int VRBinding[VR_SPECIES_COUNT][VR_ACT_COUNT];
extern int VRMoveDeadzone;
extern int VRWorldScaleIndex;
extern int VRVignetteStrength;
extern int GiveAllWeaponsCheatEnabled;
extern int GodModeCheatEnabled;
extern int EnemySpeedAlien;
extern int EnemySpeedMarine;
extern int EnemySpeedPredator;
extern int HUDInsetLevel;
extern int ManualReloadEnabled;


List<AVP_USER_PROFILE *> UserProfilesList;
static AVP_USER_PROFILE DefaultUserProfile = 
{
	"",
};
static AVP_USER_PROFILE *CurrentUserProfilePtr;

extern void ExamineSavedUserProfiles(void)
{
	// delete any existing profiles
	EmptyUserProfilesList();
	
	// load any available user profiles
	LoadUserProfiles();
	
	// make a fake entry to allow creating new user profiles
	AVP_USER_PROFILE *profilePtr = new AVP_USER_PROFILE;
	*profilePtr = DefaultUserProfile;

	profilePtr->FileTime = time(NULL);

	strncpy(profilePtr->Name,GetTextString(TEXTSTRING_USERPROFILE_NEW),MAX_SIZE_OF_USERS_NAME);
	profilePtr->Name[MAX_SIZE_OF_USERS_NAME]=0;
	SetDefaultProfileOptions(profilePtr);

	InsertProfileIntoList(profilePtr);
}

extern int NumberOfUserProfiles(void)
{
	int n = UserProfilesList.size();

	LOCALASSERT(n>0);

	return n-1;
}

extern AVP_USER_PROFILE *GetFirstUserProfile(void)
{
	CurrentUserProfilePtr=UserProfilesList.first_entry();
	return CurrentUserProfilePtr;
}

extern AVP_USER_PROFILE *GetNextUserProfile(void)
{
	if (CurrentUserProfilePtr == UserProfilesList.last_entry())
	{
		CurrentUserProfilePtr = UserProfilesList.first_entry();
	}
	else
	{
		CurrentUserProfilePtr = UserProfilesList.next_entry(CurrentUserProfilePtr);
	}
	return CurrentUserProfilePtr;
}

static void EmptyUserProfilesList(void)
{
	while (UserProfilesList.size())
	{
		delete UserProfilesList.first_entry();
		UserProfilesList.delete_first_entry();
	}
}

extern int SaveUserProfile(AVP_USER_PROFILE *profilePtr)
{
	char *filename = new char [strlen(USER_PROFILES_PATH)+strlen(profilePtr->Name)+strlen(USER_PROFILES_SUFFIX)+1];
	strcpy(filename,USER_PROFILES_PATH);
	strcat(filename,profilePtr->Name);
	strcat(filename,USER_PROFILES_SUFFIX);

	FILE* file=OpenGameFile(filename, FILEMODE_WRITEONLY, FILETYPE_CONFIG);
	delete [] filename;
	if (!file) return 0;
	
	SaveSettingsToUserProfile(profilePtr);

	fwrite(profilePtr,sizeof(AVP_USER_PROFILE),1,file);
	fclose(file);

	return 1;
}

extern void DeleteUserProfile(int number)
{
	AVP_USER_PROFILE *profilePtr = GetFirstUserProfile();

	for (int i=0; i<number; i++) profilePtr = GetNextUserProfile();

	char *filename = new char [strlen(USER_PROFILES_PATH)+strlen(profilePtr->Name)+strlen(USER_PROFILES_SUFFIX)+1];
	strcpy(filename,USER_PROFILES_PATH);
	strcat(filename,profilePtr->Name);
	strcat(filename,USER_PROFILES_SUFFIX);

	DeleteGameFile(filename);

	delete [] filename;
	{
		int i;
		filename = new char [100];

		for (i=0; i<NUMBER_OF_SAVE_SLOTS; i++)
		{
			sprintf(filename,"%s%s_%d.sav",USER_PROFILES_PATH,profilePtr->Name,i+1);
			DeleteGameFile(filename);
		}
		delete [] filename;
	}
}

static void InsertProfileIntoList(AVP_USER_PROFILE *profilePtr)
{
	if (UserProfilesList.size())
	{
		AVP_USER_PROFILE *profileInListPtr = GetFirstUserProfile();

		for (int i=0; i<UserProfilesList.size(); i++, profileInListPtr = GetNextUserProfile())
		{
			if (ProfileIsMoreRecent(profilePtr,profileInListPtr))
			{
				UserProfilesList.add_entry_before(profilePtr,profileInListPtr);
				return;
			}
		}
	}
	UserProfilesList.add_entry(profilePtr);
}

static int ProfileIsMoreRecent(AVP_USER_PROFILE *profilePtr, AVP_USER_PROFILE *profileToTestAgainstPtr)
{
	if (difftime(profilePtr->FileTime, profileToTestAgainstPtr->FileTime) > 0.0) {
		return 1; /* first file newer than file to test */
	} else {
		return 0;
	}
}

static int LoadUserProfiles(void)
{
	void *gd;
	GameDirectoryFile *gdf;
	
	gd = OpenGameDirectory(USER_PROFILES_PATH, USER_PROFILES_WILDCARD_NAME, FILETYPE_CONFIG);
	if (gd == NULL) {
		CreateGameDirectory(USER_PROFILES_PATH); /* maybe it didn't exist.. */
		return 0;
	}

	int nPathLen = strlen(USER_PROFILES_PATH);
	
	while ((gdf = ScanGameDirectory(gd)) != NULL) {
		if ((gdf->attr & FILEATTR_DIRECTORY) != 0)
			continue;
		if ((gdf->attr & FILEATTR_READABLE) == 0)
			continue;
		
		char * pszFullPath = new char [nPathLen+strlen(gdf->filename)+1];
		strcpy(pszFullPath, USER_PROFILES_PATH);
		strcat(pszFullPath, gdf->filename);
			
		FILE *rif_file;
		rif_file = OpenGameFile(pszFullPath, FILEMODE_READONLY, FILETYPE_CONFIG);
		if(rif_file==NULL)
		{
			delete[] pszFullPath;
			continue;
		}

		AVP_USER_PROFILE *profilePtr = new AVP_USER_PROFILE;

		/* Accept a profile written by an EARLIER version.
		 *
		 * This used to demand an exact size match, so the moment the struct gained the
		 * controller fields every existing .prf stopped loading and simply vanished from
		 * the profile list, taking the player's settings, key bindings and best times
		 * with it.
		 *
		 * The controller block is appended at the END of the struct (see the note there),
		 * so an older file is a prefix of the current one: zero the buffer first, read
		 * whatever the file holds, and the fields it does not reach stay zero. Every one
		 * of them is stored +1 precisely so that zero reads as "never written" and
		 * decodes to its default.
		 *
		 * The floor is everything up to that block - a file shorter than that is from
		 * before some earlier field and cannot be interpreted, so it is still rejected.
		 * A LONGER file (written by a future version) is read up to our size and the
		 * remainder ignored, which is the same contract in the other direction. */
		memset(profilePtr, 0, sizeof(AVP_USER_PROFILE));
		{
			const size_t minimumSize = offsetof(AVP_USER_PROFILE, PadBindingPlus1);
			size_t got = fread(profilePtr, 1, sizeof(AVP_USER_PROFILE), rif_file);

			if (got < minimumSize)
			{
				fclose(rif_file);
				delete[] pszFullPath;
				delete profilePtr;
				continue;
			}
			if (got < sizeof(AVP_USER_PROFILE))
				SDL_Log("PROFILE: '%s' predates controller support (%u of %u bytes) - "
				        "loaded, controller settings defaulted",
				        pszFullPath, (unsigned)got, (unsigned)sizeof(AVP_USER_PROFILE));
		}

		profilePtr->FileTime = gdf->timestamp;
	
		InsertProfileIntoList(profilePtr);
		fclose(rif_file);
		delete[] pszFullPath;
	}
	
	CloseGameDirectory(gd);
	
	return 1;
}

static void SetDefaultProfileOptions(AVP_USER_PROFILE *profilePtr)
{
	// set Gamma
	RequestedGammaSetting = 128;
	
	// controls
	MarineInputPrimaryConfig = DefaultMarineInputPrimaryConfig;
	MarineInputSecondaryConfig = DefaultMarineInputSecondaryConfig;
	PredatorInputPrimaryConfig = DefaultPredatorInputPrimaryConfig;
	PredatorInputSecondaryConfig = DefaultPredatorInputSecondaryConfig;
	AlienInputPrimaryConfig = DefaultAlienInputPrimaryConfig;
	AlienInputSecondaryConfig = DefaultAlienInputSecondaryConfig;
	ControlMethods = DefaultControlMethods;
	JoystickControlMethods = DefaultJoystickControlMethods;
	
	SmackerSoundVolume = ONE_FIXED/512;
	EffectsSoundVolume = VOLUME_DEFAULT;
	CDPlayerVolume = CDDA_VOLUME_DEFAULT;
	MoviesAreActive = 1;
	IntroOutroMoviesAreActive = 1; 
	AutoWeaponChangeOn = TRUE;
	ShowCrosshair = 1;
	ShowFrameRate = 0;
	VRRefreshRateIndex = 0;
	VRRefreshRateHz = 0;    /* unset; resolved against the headset's list at session start */
	MSAASampleIndex = 1; /* 2x by default */
	VRTurnMode = 0; /* snap turn by default */
	VRSnapAngleIndex = 1; /* 45 degrees by default */
	VRSmoothTurnSpeed = 5; /* mid speed by default (0..10) */
	VRSmoothDeadzone = 4; /* 0.2 deflection by default (0..10) */
	VRVignetteOn = 1; /* comfort vignette on by default */
	VRClimbVignetteOn = 1; /* wall-walk transition vignette on by default */
	VRClimbVignetteStrength = 5;
	MarineLeftArmVisible = 1; /* Marine's left arm shown by default */
	VRMoveDeadzone = 2;
	PadVertSensitivity = 10;
	PadHorizSensitivity = 10;
	PadInvertVertical = 0;
	Pad_ResetBindings();
	VRWorldScaleIndex = VR_WORLD_SCALE_DEFAULT_INDEX;
	/* Bindings keep whatever main.c initialised them to: those ARE the defaults. */
	VRVignetteStrength = 5; /* mid strength by default (0..10) */
	GiveAllWeaponsCheatEnabled = 0; /* "give all weapons" cheat off by default */
	GodModeCheatEnabled = 0; /* "god mode" cheat off by default */
	EnemySpeedAlien = 10;    /* enemy speed sliders default to full speed (10 = 1.0) */
	EnemySpeedMarine = 10;
	EnemySpeedPredator = 10;
	HUDInsetLevel = 0;       /* "Adjust HUD elements" defaults to level 1 (current layout) */
	ManualReloadEnabled = 0; /* "Manual Reload" defaults to Off */
	/* All three reproduce the filtering the port had before these were options. */
	AnisotropicFilterIndex = 0; /* 16x, matching the old always-maximum behaviour */
	TextureFilterIndex = 0;     /* trilinear, the old hardcoded min filter */
	NPOTMipmapsEnabled = 0;     /* NPOT textures were never mipped before */
	DesktopMirrorIndex = 0;     /* mirror every frame, as it did before the option */

	strcpy(MP_PlayerName, "Player");

	SetToDefaultDetailLevels();
	
	{
		int a,b;

		for (a=0; a<I_MaxDifficulties; a++) {
			for (b=0; b<AVP_ENVIRONMENT_END_OF_LIST; b++) {
				profilePtr->PersonalBests[a][b]=DefaultLevelGameStats;
			}
		}
	}

	SaveSettingsToUserProfile(profilePtr);
}
			
/* Replace a control setting that cannot have come from the menus with its default.
 *
 * A profile is an fwrite'n struct read straight back off disk with no version tag and
 * no checksum, so ANY file in user_profiles is trusted to fill these fields. A .prf
 * written by a build whose struct layout differed (or simply a truncated or corrupt
 * one) lands arbitrary ints in them, and they are not inert: the menu turns the mouse
 * sensitivities into a screen coordinate for the slider graphic, so a wild value used
 * to crash the game on opening Mouse Configuration rather than merely looking wrong.
 *
 * Checked against the same bounds the menu itself enforces - the sliders run 0..
 * DEFAULT_MOUSE?_SENSITIVITY*3 and the rest are yes/no text sliders - so anything this
 * rejects was unreachable through the UI and is corruption by definition. Per field
 * rather than all-or-nothing, since a single bad byte should not discard a whole
 * profile's settings. */
static void SanitiseControlMethods(CONTROL_METHODS *cm)
{
	if (cm->MouseXSensitivity > (unsigned int)(DEFAULT_MOUSEX_SENSITIVITY*3))
		cm->MouseXSensitivity = DefaultControlMethods.MouseXSensitivity;
	if (cm->MouseYSensitivity > (unsigned int)(DEFAULT_MOUSEY_SENSITIVITY*3))
		cm->MouseYSensitivity = DefaultControlMethods.MouseYSensitivity;

	if (cm->VAxisIsMovement      > 1) cm->VAxisIsMovement      = DefaultControlMethods.VAxisIsMovement;
	if (cm->HAxisIsTurning       > 1) cm->HAxisIsTurning       = DefaultControlMethods.HAxisIsTurning;
	if (cm->FlipVerticalAxis     > 1) cm->FlipVerticalAxis     = DefaultControlMethods.FlipVerticalAxis;
	if (cm->AutoCentreOnMovement > 1) cm->AutoCentreOnMovement = DefaultControlMethods.AutoCentreOnMovement;
}

/* The joystick equivalent. Every field here is a yes/no toggle except the two
   trackerball sensitivities, which the 1999 menus never exposed a range for. */
static void SanitiseJoystickControlMethods(JOYSTICK_CONTROL_METHODS *jm)
{
	unsigned int *flags[] = {
		&jm->JoystickEnabled,
		&jm->JoystickVAxisIsMovement,     &jm->JoystickHAxisIsTurning,
		&jm->JoystickFlipVerticalAxis,
		&jm->JoystickPOVVAxisIsMovement,  &jm->JoystickPOVHAxisIsTurning,
		&jm->JoystickPOVFlipVerticalAxis,
		&jm->JoystickRudderEnabled,       &jm->JoystickRudderAxisIsTurning,
		&jm->JoystickTrackerBallEnabled,  &jm->JoystickTrackerBallFlipVerticalAxis,
	};
	unsigned int i;

	for (i = 0; i < sizeof(flags)/sizeof(flags[0]); i++)
		if (*flags[i] > 1) *flags[i] = 0;

	if (jm->JoystickTrackerBallHorizontalSensitivity > 1000)
		jm->JoystickTrackerBallHorizontalSensitivity = DefaultJoystickControlMethods.JoystickTrackerBallHorizontalSensitivity;
	if (jm->JoystickTrackerBallVerticalSensitivity > 1000)
		jm->JoystickTrackerBallVerticalSensitivity = DefaultJoystickControlMethods.JoystickTrackerBallVerticalSensitivity;
}

extern void GetSettingsFromUserProfile(void)
{
	RequestedGammaSetting = UserProfilePtr->GammaSetting;

	MarineInputPrimaryConfig = 		UserProfilePtr->MarineInputPrimaryConfig;
	MarineInputSecondaryConfig = 	UserProfilePtr->MarineInputSecondaryConfig;
	PredatorInputPrimaryConfig = 	UserProfilePtr->PredatorInputPrimaryConfig;
	PredatorInputSecondaryConfig = 	UserProfilePtr->PredatorInputSecondaryConfig;
	AlienInputPrimaryConfig = 		UserProfilePtr->AlienInputPrimaryConfig;
	AlienInputSecondaryConfig = 	UserProfilePtr->AlienInputSecondaryConfig;
	ControlMethods = 				UserProfilePtr->ControlMethods;
	JoystickControlMethods = 		UserProfilePtr->JoystickControlMethods;
	SanitiseControlMethods(&ControlMethods);
	SanitiseJoystickControlMethods(&JoystickControlMethods);
	MenuDetailLevelOptions = 		UserProfilePtr->DetailLevelSettings;
	SmackerSoundVolume =			UserProfilePtr->SmackerSoundVolume;
	EffectsSoundVolume =			UserProfilePtr->EffectsSoundVolume;
	CDPlayerVolume = 				UserProfilePtr->CDPlayerVolume;
	MoviesAreActive =				UserProfilePtr->MoviesAreActive;
	IntroOutroMoviesAreActive =		UserProfilePtr->IntroOutroMoviesAreActive;
	AutoWeaponChangeOn = 			!UserProfilePtr->AutoWeaponChangeDisabled;
	ShowCrosshair =				!UserProfilePtr->ShowCrosshairDisabled;
	ShowFrameRate =				!UserProfilePtr->ShowFrameRateDisabled;
	/* The RATE is the stored value; the slider index is derived from it against
	   whatever list this headset reports. Do NOT round-trip the index through
	   the legacy VRRefreshRateIndex field: it is a 2-bit bitfield, so any index
	   above 3 wraps to 0 on save and the setting silently reverts to the lowest
	   rate (120 Hz is index 4 on a Quest 2, which reports 60/72/80/90/120).
	   VR_GetRefreshRateIndexForHz returns 0 before enumeration has run, and the
	   session-ready path recomputes it once the list is known. */
	VRRefreshRateHz =			UserProfilePtr->VRRefreshRateHz;
	/* Derived unconditionally: VR_GetRefreshRateIndexForHz now resolves against the
	   applier's fallback table when the headset list is empty, so this no longer
	   collapses a saved rate to index 0 the way it did before enumeration had run.
	   If enumeration succeeds later, the session-ready path recomputes the index
	   from VRRefreshRateHz, which is the value actually stored. */
	VRRefreshRateIndex =			VR_GetRefreshRateIndexForHz((float)VRRefreshRateHz);
	MSAASampleIndex =			UserProfilePtr->MSAASampleIndex;
	AnisotropicFilterIndex =		UserProfilePtr->AnisotropicFilterIndex;
	TextureFilterIndex =			UserProfilePtr->TextureFilterIndex;
	NPOTMipmapsEnabled =			UserProfilePtr->NPOTMipmapsEnabled;
	DesktopMirrorIndex =			UserProfilePtr->DesktopMirrorIndex;
	VRTurnMode =				UserProfilePtr->VRTurnMode;
	VRSnapAngleIndex =			UserProfilePtr->VRSnapAngleIndex;
	VRSmoothTurnSpeed =			UserProfilePtr->VRSmoothTurnSpeed;
	VRSmoothDeadzone =			UserProfilePtr->VRSmoothDeadzone;
	VRVignetteOn =				UserProfilePtr->VRVignetteOn;
	/* Stored inverted so a zeroed Padding byte in an older profile reads as On. */
	VRClimbVignetteOn =			!UserProfilePtr->VRClimbVignetteDisabled;
	/* 0 means the byte predates the option; anything else is the value plus one. */
	VRClimbVignetteStrength =		UserProfilePtr->VRClimbVignetteStrengthPlus1
						? UserProfilePtr->VRClimbVignetteStrengthPlus1 - 1
						: 5;
	/* Stored inverted so a zeroed Padding byte in an older profile reads as On. */
	MarineLeftArmVisible =			!UserProfilePtr->MarineLeftArmHidden;
	VRMoveDeadzone =			UserProfilePtr->VRMoveDeadzonePlus1
					? UserProfilePtr->VRMoveDeadzonePlus1 - 1 : 2;
	VRWorldScaleIndex =			UserProfilePtr->VRWorldScaleIndexPlus1
					? UserProfilePtr->VRWorldScaleIndexPlus1 - 1
					: VR_WORLD_SCALE_DEFAULT_INDEX;
	if (VRWorldScaleIndex < 0 || VRWorldScaleIndex > VR_WORLD_SCALE_MAX_INDEX)
		VRWorldScaleIndex = VR_WORLD_SCALE_DEFAULT_INDEX;
	{
		/* Stored as source+1; 0 means the profile predates the bindings, so that
		   action keeps its default rather than becoming unbound. */
		/* Load, then VALIDATE - a stored set is only used if the whole species'
		   row is sane. The binding block's layout has already changed shape more
		   than once (a flat [16] became [3][12], neighbouring fields came and
		   went), and a profile written by an older build decodes those bytes as
		   something else entirely: an action can come back bound to a control that
		   is held most of the time, which reads in-game as a button firing itself.
		   Rejecting the row wholesale and keeping the defaults is recoverable;
		   half-applying a garbled set is not. */
		int sp, i;
		for (sp = 0; sp < VR_SPECIES_COUNT; sp++)
		{
			int candidate[VR_ACT_COUNT];
			int ok = 1;

			for (i = 0; i < VR_ACT_COUNT; i++)
			{
				int stored = (i < 12) ? UserProfilePtr->VRBindingPlus1[sp][i] : 0;
				/* 0 = never written; keep this action's default. */
				candidate[i] = stored ? stored - 1 : VRBinding[sp][i];
				if (candidate[i] < 0 || candidate[i] >= VR_SRC_COUNT) ok = 0;
			}
			/* No control may drive two actions - Unbound excepted. */
			for (i = 0; ok && i < VR_ACT_COUNT; i++)
			{
				int j;
				if (candidate[i] == VR_SRC_NONE) continue;
				for (j = i + 1; j < VR_ACT_COUNT; j++)
					if (candidate[j] == candidate[i]) { ok = 0; break; }
			}

			if (ok)
				for (i = 0; i < VR_ACT_COUNT; i++) VRBinding[sp][i] = candidate[i];
			else
				SDL_Log("PROFILE: controller bindings for species %d were not usable "
				        "(older or corrupt profile) - defaults kept", sp);
		}
	}
	/* Game controller. Same all-or-nothing validation as the VR bindings above: an
	   out-of-range or duplicated set is rejected wholesale rather than half-applied,
	   because a partly-garbled map is worse than the defaults. */
	PadVertSensitivity  = UserProfilePtr->PadVertSensitivityPlus1
	                    ? UserProfilePtr->PadVertSensitivityPlus1 - 1 : PAD_VERT_SENSITIVITY_DEFAULT;
	PadHorizSensitivity = UserProfilePtr->PadHorizSensitivityPlus1
	                    ? UserProfilePtr->PadHorizSensitivityPlus1 - 1 : PAD_HORIZ_SENSITIVITY_DEFAULT;
	PadInvertVertical   = UserProfilePtr->PadInvertVerticalPlus1
	                    ? UserProfilePtr->PadInvertVerticalPlus1 - 1 : PAD_INVERT_VERTICAL_DEFAULT;
	if (PadVertSensitivity  < 0 || PadVertSensitivity  > 20) PadVertSensitivity  = 10;
	if (PadHorizSensitivity < 0 || PadHorizSensitivity > 20) PadHorizSensitivity = 10;
	if (PadInvertVertical   < 0 || PadInvertVertical   > 1)  PadInvertVertical   = 0;
	for (int psp = 0; psp < PAD_SPECIES_COUNT; psp++)
	{
		int candidate[PAD_ACT_COUNT];
		int ok = 1, i;

		for (i = 0; i < PAD_ACT_COUNT; i++)
		{
			int stored = (i < 16) ? UserProfilePtr->PadBindingPlus1[psp][i] : 0;
			candidate[i] = stored ? stored - 1 : PadBinding[psp][i];
			if (candidate[i] < 0 || candidate[i] >= PAD_SRC_COUNT) ok = 0;
		}
		/* Duplicates are UNBOUND rather than rejected, which is the difference from the
		   VR set above.
		   The menu no longer creates them - Pad_CycleBinding skips a source already in
		   use, exactly as the VR side does - so a clash in a file comes from a profile
		   written before that existed. Rejecting the whole set, as the VR path does,
		   would throw away the player's entire map over one bad row; leaving it alone
		   would let one button keep firing two actions. Clearing the later row is the
		   middle course: everything else survives, the clash is gone, and the cleared
		   row shows as Unbound so it is visible rather than silent.

		   FIRST occurrence wins, and the action order is not arbitrary - fire, jump and
		   crouch come before the situational abilities, so a clash is resolved in favour
		   of the more fundamental control. Unbound is exempt: any number of actions may
		   sit on nothing. */
		if (ok)
		{
			for (i = 0; i < PAD_ACT_COUNT; i++)
			{
				int j;
				/* Start and Back are reserved by the frontend and no longer offered,
				   so a binding on one can only come from an older profile. Start in
				   particular would fire its action every time the in-game menu was
				   opened. */
				if (candidate[i] > PAD_SRC_LAST_BINDABLE)
				{
					SDL_Log("PROFILE: species %d action %d was bound to a reserved "
					        "control (older profile) - unbound", psp, i);
					candidate[i] = PAD_SRC_NONE;
				}
				if (candidate[i] == PAD_SRC_NONE) continue;
				for (j = 0; j < i; j++)
				{
					if (candidate[j] == candidate[i])
					{
						SDL_Log("PROFILE: species %d actions %d and %d shared one "
						        "control (older profile) - action %d unbound",
						        psp, j, i, i);
						candidate[i] = PAD_SRC_NONE;
						break;
					}
				}
			}
			for (i = 0; i < PAD_ACT_COUNT; i++) PadBinding[psp][i] = candidate[i];
		}
		else
			SDL_Log("PROFILE: controller button bindings for species %d were not usable "
			        "(older or corrupt profile) - defaults kept", psp);
	}
	VRVignetteStrength =			UserProfilePtr->VRVignetteStrength;
	GiveAllWeaponsCheatEnabled =		UserProfilePtr->GiveAllWeaponsCheat;
	GodModeCheatEnabled =			UserProfilePtr->GodModeCheat;
	/* Stored as (10 - speed) so a fresh/old profile (0) loads as full speed (10). */
	EnemySpeedAlien =			10 - UserProfilePtr->EnemySpeedAlien;
	EnemySpeedMarine =			10 - UserProfilePtr->EnemySpeedMarine;
	EnemySpeedPredator =			10 - UserProfilePtr->EnemySpeedPredator;
	HUDInsetLevel =				UserProfilePtr->HUDInsetLevel;
	ManualReloadEnabled =			UserProfilePtr->ManualReloadEnabled;
   	strncpy(MP_PlayerName,UserProfilePtr->MultiplayerCallsign,15);

	SetDetailLevelsFromMenu();
}

extern void SaveSettingsToUserProfile(AVP_USER_PROFILE *profilePtr)
{
	profilePtr->GammaSetting = RequestedGammaSetting;

	profilePtr->MarineInputPrimaryConfig =		MarineInputPrimaryConfig;
	profilePtr->MarineInputSecondaryConfig =	MarineInputSecondaryConfig;
	profilePtr->PredatorInputPrimaryConfig =	PredatorInputPrimaryConfig;
	profilePtr->PredatorInputSecondaryConfig =	PredatorInputSecondaryConfig;
	profilePtr->AlienInputPrimaryConfig =		AlienInputPrimaryConfig;
	profilePtr->AlienInputSecondaryConfig =		AlienInputSecondaryConfig;
	profilePtr->ControlMethods =				ControlMethods;
	profilePtr->JoystickControlMethods =		JoystickControlMethods;
	profilePtr->DetailLevelSettings =			MenuDetailLevelOptions;
	profilePtr->SmackerSoundVolume =			SmackerSoundVolume;	
	profilePtr->EffectsSoundVolume =			EffectsSoundVolume;
	profilePtr->CDPlayerVolume = 				CDPlayerVolume;
	profilePtr->MoviesAreActive =				MoviesAreActive;
	profilePtr->IntroOutroMoviesAreActive =		IntroOutroMoviesAreActive;
	profilePtr->AutoWeaponChangeDisabled =	!AutoWeaponChangeOn;
	profilePtr->ShowCrosshairDisabled =	!ShowCrosshair;
	profilePtr->ShowFrameRateDisabled =	!ShowFrameRate;
	/* Only the rate is stored. The legacy 2-bit index field is left at 0 — see
	   the note in GetSettingsFromUserProfile. */
	profilePtr->VRRefreshRateIndex =	0;
	profilePtr->VRRefreshRateHz =	(unsigned char)(VRRefreshRateHz > 255 ? 255 : VRRefreshRateHz);
	profilePtr->MSAASampleIndex =		MSAASampleIndex;
	profilePtr->AnisotropicFilterIndex =	AnisotropicFilterIndex;
	profilePtr->TextureFilterIndex =	TextureFilterIndex;
	profilePtr->NPOTMipmapsEnabled =	NPOTMipmapsEnabled;
	profilePtr->DesktopMirrorIndex =	DesktopMirrorIndex;
	profilePtr->VRTurnMode =		VRTurnMode;
	profilePtr->VRSnapAngleIndex =		VRSnapAngleIndex;
	profilePtr->VRSmoothTurnSpeed =		VRSmoothTurnSpeed;
	profilePtr->VRSmoothDeadzone =		VRSmoothDeadzone;
	profilePtr->VRVignetteOn =		VRVignetteOn;
	profilePtr->VRClimbVignetteDisabled =	!VRClimbVignetteOn;
	profilePtr->VRClimbVignetteStrengthPlus1 = (unsigned char)(VRClimbVignetteStrength + 1);
	profilePtr->MarineLeftArmHidden =	!MarineLeftArmVisible;
	profilePtr->VRMoveDeadzonePlus1 =	(unsigned char)(VRMoveDeadzone + 1);
	profilePtr->VRWorldScaleIndexPlus1 =	(unsigned char)(VRWorldScaleIndex + 1);
	{
		int sp, i;
		for (sp = 0; sp < VR_SPECIES_COUNT; sp++)
			for (i = 0; i < VR_ACT_COUNT && i < 12; i++)
				profilePtr->VRBindingPlus1[sp][i] = (unsigned char)(VRBinding[sp][i] + 1);
	}
	profilePtr->PadVertSensitivityPlus1 =	(unsigned char)(PadVertSensitivity + 1);
	profilePtr->PadHorizSensitivityPlus1 =	(unsigned char)(PadHorizSensitivity + 1);
	profilePtr->PadInvertVerticalPlus1 =	(unsigned char)(PadInvertVertical + 1);
	{
		int sp, i;
		for (sp = 0; sp < PAD_SPECIES_COUNT; sp++)
			for (i = 0; i < PAD_ACT_COUNT && i < 16; i++)
				profilePtr->PadBindingPlus1[sp][i] = (unsigned char)(PadBinding[sp][i] + 1);
	}
	profilePtr->VRVignetteStrength =	VRVignetteStrength;
	profilePtr->GiveAllWeaponsCheat =	GiveAllWeaponsCheatEnabled;
	profilePtr->GodModeCheat =		GodModeCheatEnabled;
	/* Stored as (10 - speed) so full speed (10) writes 0, matching a fresh profile. */
	profilePtr->EnemySpeedAlien =		10 - EnemySpeedAlien;
	profilePtr->EnemySpeedMarine =		10 - EnemySpeedMarine;
	profilePtr->EnemySpeedPredator =	10 - EnemySpeedPredator;
	profilePtr->HUDInsetLevel =		HUDInsetLevel;
	profilePtr->ManualReloadEnabled =	ManualReloadEnabled;
   	strncpy(profilePtr->MultiplayerCallsign,MP_PlayerName,15);
}

extern void FixCheatModesInUserProfile(AVP_USER_PROFILE *profilePtr)
{
	int a;

	for (a=0; a<MAX_NUMBER_OF_CHEATMODES; a++) {
		if (profilePtr->CheatMode[a]==2) {
			profilePtr->CheatMode[a]=1;
		}
	}

}

}; // extern "C"
