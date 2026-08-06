#include "version.h"

extern void NewOnScreenMessage(unsigned char *messagePtr);

/* Shown by --version (main.c) and the in-game version console command. version.c is
   built unconditionally for every platform, so this string must stay platform-neutral —
   it used to say "Linux" and was reported verbatim by the Windows and Android builds
   too. Keep the version here in step with platform/windows/Resource.rc (FILEVERSION /
   FileVersion) and platform/android/app/build.gradle.kts (versionName / versionCode). */
const char *AvPVersionString = "Aliens vs Predator \n     Build 0.6 \n     Based on Rebellion Developments AvP Gold source \n";

void GiveVersionDetails(void)
{
	NewOnScreenMessage((unsigned char *)AvPVersionString);
}
