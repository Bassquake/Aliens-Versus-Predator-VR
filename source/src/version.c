#include "version.h"
#include "avp_version.h"

extern void NewOnScreenMessage(unsigned char *messagePtr);

/* Shown by --version (main.c) and the in-game version console command. version.c is
   built unconditionally for every platform, so this string must stay platform-neutral —
   it used to say "Linux" and was reported verbatim by the Windows and Android builds
   too. The number comes from AVP_VERSION_STRING (avp/avp_version.h), which says what else
   has to be kept in step with it. */
const char *AvPVersionString = "Aliens vs Predator \n     Build " AVP_VERSION_STRING " \n     Based on Rebellion Developments AvP Gold source \n";

void GiveVersionDetails(void)
{
	NewOnScreenMessage((unsigned char *)AvPVersionString);
}
