#ifndef _avp_version_h_
#define _avp_version_h_ 1

/* The port's version, shown at the bottom of the Extra Cheats menu.
 *
 * KEEP IN SYNC with `versionName` (and bump `versionCode`) in
 * platform/android/app/build.gradle.kts, which names the APK
 * (avpvr-<version>-<flavor>-<abi>-release.apk), AND with the four version fields of
 * VS_VERSION_INFO in platform/windows/Resource.rc (FILEVERSION, PRODUCTVERSION and the
 * two string VALUEs) - the version Windows shows in the exe's Properties. Gradle cannot read this
 * header and CMake does not feed the Android version, so the two are updated by hand -
 * there is deliberately only this one copy on the C side, so every desktop target and
 * the in-game display agree with each other. */
#define AVP_VERSION_STRING "1.2"

#endif
