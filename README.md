# Aliens Versus Predator: VR

Fork of NakedAVP to use OpenXR for VR headets like the Meta Quest. This is early stage as lots of bugs and fixes are likely needed and I've only been focusing on the Marine level at the moment.

![Screenshot of Aliens Versus Predator: VR menu playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/avpvr-quest-menu.jpg)

![Screenshot of Aliens Versus Predator: VR in-game playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/avpvr-quest-marine.jpg)

Short video of it in action on a Quest 2 here on [YouTube](https://youtu.be/IxnrIYhSEMs).

[![Watch the video](https://img.youtube.com/vi/IxnrIYhSEMs/0.jpg)](https://youtu.be/IxnrIYhSEMs)

> [!NOTE]
> This is based on the code from _atsb_ over at [atsb/NakedAvP](https://github.com/atsb/NakedAVP) v1.2.3. I've ported to Android and for it to use OpenXR for VR headsets. I have added binaries as an apk for Android 16 VR headsets. They do not include the game files as its not allowed. See important note below about game assets and there are instructions on how to add them.

> [!IMPORTANT]
> You need to supply the games asset files. Buy the game or find cd/downloads of Aliens Versus Predator Gold Edition. It has to be the Gold Edition as the standard versions 'language.txt' file crashes the game. You can use the standard versions files if you use the Gold Edition language.txt, the videos are different! Check [eBay](https://www.ebay.co.uk/sch/i.html?_nkw=aliens+versus+predator+gold+edition&_sacat=0&_from=R40&_trksid=m570.l1313&_odkw=aliens+versus+predator+gold&_osacat=0&_sop=15) or [GOG](https://www.gog.com/en/game/aliens_versus_predator_classic_2000) or [Steam](https://store.steampowered.com/app/3730/Aliens_versus_Predator_Classic_2000/).

## Step-by-step
1. Install the original pc game from disk/download like normal on your pc.
2. Copy the games assets from **C:\Program Files (x86)\Fox\Aliens versus Predator\**. You need the following:

![Screenshot of assets files](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/assets-files.png)

4. Download the [lowercase.ps1](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/assets/lowercase.ps1) powershell script or find it in the assets folder of this project. Place the script into where the assets are.
5. Run the script in Powershell (type **lowercase.ps1**) and all files will now be lowercase.
6. Plug your headset in via usb.
7. Download the Aliens Versus Predator: VR release apk and install it with [SideQuest](https://sidequestvr.com/setup-howto) using the "Install APK file from folder on computer". (Your headset probably should be in [Developer](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/) mode already):

![Screenshot of apk install](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/sidequest_install.png)

6. After install completes, you should see this in the "Currently installed apps":

![Screenshot of apk location](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/sidequest_installed.png)

7. Run the Aliens Versus Predator:VR in Unknown Sources. This will crash out as the assets havent been added yet, this is to set the folders in place. 
8. Now still in SideQuest, go to "Manage files on the headset".
9. Navigate to "sdcard/Android/data/com.bassquake.avpvr/files".
10. Copy all the game assets into that files folder. The layout should be like so on your device:

![Screenshot of assets location](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/sidequest_files.png)

## CD Music
You can skip this if you don't have the audio cd that comes with the game, it'll just play without background music. Or provide your own music! Would need 15 tracks.
- Create a folder in **sdcard/Android/data/com.bassquake.avpvr/files/** called **cd_tracks**.
- Rip the CD's audio tracks to Ogg Vorbis 160kbps and copy them over into the **cd_tracks** folder on your headset. Make sure they're lowercase and exactly named as track01.ogg, track02.ogg etc.

## Run it!
Go to Unknown Sources on the headset and click on Aliens Versus Predator:VR. Have fun!
> [!CAUTION]
> This is early stage and have only really focused on the Marine campaign. I'm not really much of a programmer, this was done with a lot of help from various ai like Claude, Copilot and Gemini.

## Controls
> [!NOTE]
> At the moment I've only concentrated on the Marine level as that was my favourite! Alien and Predator will be looked at soon.

### Keys for Marine level (probably mostly works for Alien and Predator too)
Controls are as follow (Marine level):

![Control layout](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/avpvr-controllers.jpg)

I will add ability to customise all these.

## Extra features
- Can choose different framerate in Video/Video Options, runs fine in 120fps mode!
- Can turn on/off the cross-hair in Video/Video Options.
- Framerate counter can be toggled in Video/Video Options.

## To do
- Fix any visual issues that come up.
- Extra options for vr specific things like adding blinders or change snap/smooth turning etc.
- ~~Add rumble effects to controllers.~~
- ~~Add cd music.~~
- Add ability to customise controller key mapping.
- Add shadow effects.
- Maybe add anti-aliasing options but not sure if really needed.
- Customise some objects such as marines weapons as it has 2 hands attached to the main weapon which is a bit weird.
- Change the menu to be more immersive.
- Add option for left handed users.
