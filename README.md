# Aliens Versus Predator: VR

Fork of NakedAVP to use OpenXR for VR headets like the Meta Quest. This is early stage as lots of bugs and fixes are likely needed.

![Screenshot of Aliens Versus Predator: VR menu playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/avpvr-quest-menu.jpg)

![Screenshot of Aliens Versus Predator: VR in-game playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/avpvr-quest-marine.jpg)

Video of it in action on a Quest 2 on [YouTube](https://www.youtube.com/watch?v=a33hBV9m_ks).

## Notes
This is based on the code from _atsb_ over at [atsb/NakedAvP](https://github.com/atsb/NakedAVP). I've ported to Android and for it to use OpenXR for VR headsets. I have added binaries as an apk for Android 16 VR headsets. They do not include the game files as its not allowed. See important note below about game assets and there are instructions on how to add them.

## Extra features
- Can choose different framerate (in Video/Video Options). Runs fine in 120fps mode!
- Can turn on/off the cross-hair (in Video/Video Options)

Project files are in:
```
\platform
    \android <-- Select this folder in Android Studio
```

> [!IMPORTANT]
> You need to supply the games asset files. Buy the game or find cd/disk of Aliens Versus Predator Gold Edition. It has to be the Gold Edition as the standard versions' language.txt file crashes the game. (Check [eBay](https://www.ebay.co.uk/sch/i.html?_nkw=aliens+vs+predator+1999&_sacat=0&_odkw=aliens+vs+predator+2000&_osacat=0&_sop=15) or [GOG](https://www.gog.com/en/game/aliens_versus_predator_classic_2000) or [Steam](https://store.steampowered.com/app/3730/Aliens_versus_Predator_Classic_2000/). Install it to your pc and then copy the games files into the provided 'assets' folder in this project. **ALL** folder and filenames needs to be lowercase (see below on how to easily do this). SDL3, FFmpeg and OpenXR libraries is already included in this project.

The necessary game folders and files to put in the 'assets' folder are:
```
\avp_huds
\avp_rifs
\fastfile
\fmvs
\mpconfig
\user_profiles
cd tracks.txt
credits.txt
default.cfg
language.txt
```

### Lowercase the game files
Go to the assets data folder in powershell and run lowercase.ps1. All files should now be lowercase.

At the moment I've only concentrated on the Marine level as that was my favourite! Alien and Predator will be looked at soon.

### Keys for Marine level (probably mostly works for Alien and Predator too)
Controls are as follow (Marine level):
- Foward is Up on Left Controller Joystick
- Backwards is Down on Left Controller Joystick
- Left is you tuen your head left
- Right is you turn your head right
- Strafe is Left and Right on Left Controller Joystick
- Look Up is you look up
- Look Down is you look down
- Crouch is Thumbstick on Left Controller Joystick
- Jump is B on Right Controller
- Operate is A on Right Controller
- Fire Primary is Trigger on Right Controller
- Fire Secondary is Grip on Right Controller
- Next Weapon is Thumbstick on Right Joystick
- Image Intensifier is Y on Left Controller
- Throw Flare is Trigger on Left Controller

I will probably add ability to customise all these.

If you downloaded the apk and just want to get started without compiling, install the apk as normal, then copy the game files into the 'files' folder on your android device. You'll need some file management program on your pc such as the [SDK Platform-Tools](https://developer.android.com/tools/releases/platform-tools). Android is a bit of a bugbear when it comes to files and its permissions!

The folder layout should be like so on your device:
```
/
    >data
        >data
            >com.bassquake.avpvr
                >files <-- game files go in here
```

If you find you get a "Configuration file not found" or similar and the files are definitely in the right place, its likely due to permissions of the files. You'll need to do the following:

Enter terminal in Android or PowerShell with your phone connected:
```
adb shell
su
restorecon -R /data/data/com.bassquake.avpvr/
```

That should now work and try to relaunch the app again.

##Building the apk

When you're building your own apk, the game files will be auto added to the apk if you've copied the game assets into 'assets'. Final apk is copied into build/android folder (game files are compressed into it). You only need to install the apk as normal on the quest by copying the apk to the device 'Downloads' folder and then install it on the device. You'll likely need to have [developer mode](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/) active on the Quest.

# To do
- Add rumble effects to controllers
- Add ability to customise controller key mapping
- Adjust menu as its a bit close
- Customise some objects such as marines weapons as it has 2 hands attached
- Add cd music
