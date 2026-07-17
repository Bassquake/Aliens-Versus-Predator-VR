/* KJL 15:17:31 10/12/98 - user profile stuff */
#include "list_tem.hpp"
extern "C"
{
#include "3dc.h"
#include "inline.h"
#include "module.h"
#include "stratdef.h"

#include "avp_userprofile.h"
#include "language.h"
#include "gammacontrol.h"
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
extern int MSAASampleIndex;
extern int FSRQualityIndex;
extern int VRTurnMode;
extern int VRSnapAngleIndex;
extern int VRSmoothTurnSpeed;
extern int VRSmoothDeadzone;
extern int VRVignetteOn;
extern int VRVignetteStrength;
extern int GiveAllWeaponsCheatEnabled;
extern int GodModeCheatEnabled;
extern int EnemySpeedAlien;
extern int EnemySpeedMarine;
extern int EnemySpeedPredator;


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
			
		if (fread(profilePtr, 1, sizeof(AVP_USER_PROFILE), rif_file) != sizeof(AVP_USER_PROFILE))
		{
	       		fclose(rif_file);
			delete[] pszFullPath;
			delete profilePtr;
			continue;
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
	MSAASampleIndex = 1; /* 2x by default */
	FSRQualityIndex = 0; /* desktop FSR off by default */
	VRTurnMode = 0; /* snap turn by default */
	VRSnapAngleIndex = 1; /* 45 degrees by default */
	VRSmoothTurnSpeed = 5; /* mid speed by default (0..10) */
	VRSmoothDeadzone = 4; /* 0.2 deflection by default (0..10) */
	VRVignetteOn = 1; /* comfort vignette on by default */
	VRVignetteStrength = 5; /* mid strength by default (0..10) */
	GiveAllWeaponsCheatEnabled = 0; /* "give all weapons" cheat off by default */
	GodModeCheatEnabled = 0; /* "god mode" cheat off by default */
	EnemySpeedAlien = 10;    /* enemy speed sliders default to full speed (10 = 1.0) */
	EnemySpeedMarine = 10;
	EnemySpeedPredator = 10;

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
	MenuDetailLevelOptions = 		UserProfilePtr->DetailLevelSettings;
	SmackerSoundVolume =			UserProfilePtr->SmackerSoundVolume;
	EffectsSoundVolume =			UserProfilePtr->EffectsSoundVolume;
	CDPlayerVolume = 				UserProfilePtr->CDPlayerVolume;
	MoviesAreActive =				UserProfilePtr->MoviesAreActive;
	IntroOutroMoviesAreActive =		UserProfilePtr->IntroOutroMoviesAreActive;
	AutoWeaponChangeOn = 			!UserProfilePtr->AutoWeaponChangeDisabled;
	ShowCrosshair =				!UserProfilePtr->ShowCrosshairDisabled;
	ShowFrameRate =				!UserProfilePtr->ShowFrameRateDisabled;
	VRRefreshRateIndex =			UserProfilePtr->VRRefreshRateIndex;
	MSAASampleIndex =			UserProfilePtr->MSAASampleIndex;
	FSRQualityIndex =			UserProfilePtr->FSRQualityIndex;
	VRTurnMode =				UserProfilePtr->VRTurnMode;
	VRSnapAngleIndex =			UserProfilePtr->VRSnapAngleIndex;
	VRSmoothTurnSpeed =			UserProfilePtr->VRSmoothTurnSpeed;
	VRSmoothDeadzone =			UserProfilePtr->VRSmoothDeadzone;
	VRVignetteOn =				UserProfilePtr->VRVignetteOn;
	VRVignetteStrength =			UserProfilePtr->VRVignetteStrength;
	GiveAllWeaponsCheatEnabled =		UserProfilePtr->GiveAllWeaponsCheat;
	GodModeCheatEnabled =			UserProfilePtr->GodModeCheat;
	/* Stored as (10 - speed) so a fresh/old profile (0) loads as full speed (10). */
	EnemySpeedAlien =			10 - UserProfilePtr->EnemySpeedAlien;
	EnemySpeedMarine =			10 - UserProfilePtr->EnemySpeedMarine;
	EnemySpeedPredator =			10 - UserProfilePtr->EnemySpeedPredator;
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
	profilePtr->VRRefreshRateIndex =	VRRefreshRateIndex;
	profilePtr->MSAASampleIndex =		MSAASampleIndex;
	profilePtr->FSRQualityIndex =		FSRQualityIndex;
	profilePtr->VRTurnMode =		VRTurnMode;
	profilePtr->VRSnapAngleIndex =		VRSnapAngleIndex;
	profilePtr->VRSmoothTurnSpeed =		VRSmoothTurnSpeed;
	profilePtr->VRSmoothDeadzone =		VRSmoothDeadzone;
	profilePtr->VRVignetteOn =		VRVignetteOn;
	profilePtr->VRVignetteStrength =	VRVignetteStrength;
	profilePtr->GiveAllWeaponsCheat =	GiveAllWeaponsCheatEnabled;
	profilePtr->GodModeCheat =		GodModeCheatEnabled;
	/* Stored as (10 - speed) so full speed (10) writes 0, matching a fresh profile. */
	profilePtr->EnemySpeedAlien =		10 - EnemySpeedAlien;
	profilePtr->EnemySpeedMarine =		10 - EnemySpeedMarine;
	profilePtr->EnemySpeedPredator =	10 - EnemySpeedPredator;
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
