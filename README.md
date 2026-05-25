# Aliens Versus Predator: VR

Fork of NakedAVP to use OpenXR for VR headets like the Meta Quest.

![Screenshot of Aliens Versus Predator: VR menu playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/avpvr-quest-menu.jpg)

![Screenshot of Aliens Versus Predator: VR in-game playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/avpvr-quest-marine.jpg)

Video of it in action on a Quest on [YouTube](https://www.youtube.com/watch?v=a33hBV9m_ks).

## Notes
This is based on the code from _atsb_ over at [atsb/NakedAvP](https://github.com/atsb/NakedAVP). I've rejigged the project files and created and fixed some ARM specific errors for Android project too. I have added binaries for Android and Windows but they do not include the game files as its not allowed. See important note below about game assets and there are instructions on how to add them.

Project files are in:
```
\platform
    \android <-- Open in this folder in Android Studio
    \windows <-- Open the solution file in here for Visual Studio
```

> [!IMPORTANT]
> You need to supply the games asset files. Buy the game or find cd/disk of Aliens vs Predator (1999) or Aliens vs Predator Classic 2000 or Aliens vs Predator Gold Edition. (Check [eBay](https://www.ebay.co.uk/sch/i.html?_nkw=aliens+vs+predator+1999&_sacat=0&_odkw=aliens+vs+predator+2000&_osacat=0&_sop=15) or [GOG](https://www.gog.com/en/game/aliens_versus_predator_classic_2000) or [Steam](https://store.steampowered.com/app/3730/Aliens_versus_Predator_Classic_2000/). Install it and and copy the games files into the 'assets' folder. Install the apk on your android, or go to build/windows where the exe and required dlls are already there. **ALL** folder and filenames needs to be lowercase (see below on how to easily do this). SDL3 is already included in this project.

### Lowercase the game files
Go to the assets data folder in powershell and run:
```
Get-ChildItem -File | Rename-Item -NewName { $_.Name.ToLower() }
```
All files should now be from CREDITS.txt to credits.txt etc.

> [!TIP]
>Best way to play is connect a Bluetooth keyboard and mouse to your phone/Quest as I haven't added touchscreen scrolling yet. Also recommend to turn brightness all the way up in the games video settings.

### Keys
- WASD keys to move.
- Esc to quit or go back.
- . to throw flare.
- / to turn infrared view on.
- Spacebar to activate switches.

# Android
This will work on any Android device above version 7.0 (Nougat). Also works on Quest for extra large screen gameplay!!

If you downloaded the apk and just want to get started without compiling, install the apk as normal, then copy the game files into the 'files' folder on your android device. You'll need some file management program on your pc such as the [SDK Platform-Tools](https://developer.android.com/tools/releases/platform-tools). Android is a bit of a bugbear when it comes to files and its permissions!

The folder layout should be like so on your device:
```
/
    >data
        >data
            >com.bassquake.avp
                >files <-- game files go in here
```

If you find you get a "Configuration file not found" or similar and the files are definitely in the right place, its likely due to permissions of the files. You'll need to do the following:

Enter terminal in Android or PowerShell with your phone connected:
```
adb shell
su
restorecon -R /data/data/com.bassquake.avp/
```

That should now work and try to relaunch the app again.

When you're building your own apk, the game files will be auto added to the apk if you've copied the game assets into a folder called 'assets'. Final apk is copied into build/android folder (game files are already compressed into it). You only need to install the apk as normal on the phone/quest by copying the apk to the device 'Downloads' folder and then install it on the device. On Quest and other Android devices you'll likely need to have [developer mode](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/) on. Usually its just tapping the About in Settings multiple times until it says "Developer mode enabled).

# Windows
I like to use cmake-gui from [cmake](https://cmake.org/download/) to create the Visual Studios project files. In "Where is the source code?" point to the \source folder, in "Where to build the binaries" point to \platform\windows\x64 or x86, then hit Configure, when done, hit Generate. You can now open the solution file in Visual Studio. I use v2022. You might have to repoint the library and includes folders if it complains they're missing. All headers and library files are in the source folder under 'extern'.

For Windows, same as Android, add the game files into the 'assets' folder, compile as normal and the final exe and the required sdl3 dll and game files will be in the build/windows folder. Just run the exe and go!

The final compiled file structure should be like so:
```
\.avp
\avp_huds
\avp_rifs
\fastfile
\fmvs
\graphics
\mpconfig
\shape_rifs
\sound
\tools
\userprofiles
avp.exe
SDL3.dll
etc...
```
> [!IMPORTANT]
> Make sure you know what your final device OS version and CPU is! For Android its usually arm64-v8a but some Android TVs are 32bit like mine was, so would be armeabi-v7a and has to be version 7.0 or above. For Windows the choice is x86 or x64, probably runs from XP upwards (not tested except Windows 10).

# To do
- Add support for using touchscreen to move around.
- Get videos working in-game.
- Untested Linux support.
- Add support for MacOS for Intel and M chips needs adding later.
- Add Arm support for Windows.
