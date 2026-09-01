# Aliens Versus Predator: VR and flat (non-VR)

This is based on the code from _atsb_ over at [atsb/NakedAvP](https://github.com/atsb/NakedAVP) v1.2.3. I've ported to Android and for it to use OpenXR for VR headsets like the Meta Quest and PCVR. Confirmed to work with Quest 1 (v50 OS), Quest 2 and Quest 3 running v2.x OS. Game asset files are NOT included. Instructions on how to add them can be found below.

> [!TIP]
> Check the new 0.7 update on releases page. PCVR support added! More details on the Releases page.

> [!CAUTION]
> This build is in playable state as Marine and Predator. I'm still looking at how to do the wall climbing on Aliens level. I'm not primarily a programmer, this was done with a lot of help from various ai like Claude, Copilot and Gemini.

> [!NOTE]
> Builds include flat versions so can multiplay on a local LAN against others including Quest/PCVR users!! Mac coming soon.

![Screenshot of Aliens Versus Predator: VR menu playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/avpvr-quest-menu.jpg)

![Screenshot of Aliens Versus Predator: VR in-game playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/avpvr-quest-marine.jpg)

Short video of it in action on a Quest 2 here on [YouTube](https://youtu.be/IxnrIYhSEMs).

[![Watch the video](https://i.ytimg.com/vi_webp/IxnrIYhSEMs/maxresdefault.webp)](https://youtu.be/IxnrIYhSEMs)

> [!IMPORTANT]
> You need to supply the games asset files. Buy the game on Steam (often on sale) or find cd/downloads of Aliens Versus Predator Gold Edition. It has to be the Gold Edition as the standard versions 'language.txt' file crashes the game. You can use the standard versions files if you use the Gold Edition language.txt, the videos are different! Check [eBay](https://www.ebay.co.uk/sch/i.html?_nkw=aliens+versus+predator+gold+edition&_sacat=0&_from=R40&_trksid=m570.l1313&_odkw=aliens+versus+predator+gold&_osacat=0&_sop=15) or [GOG](https://www.gog.com/en/game/aliens_versus_predator_classic_2000) or [Steam](https://store.steampowered.com/app/3730/Aliens_versus_Predator_Classic_2000/).

## Sections
**[Step-by-step for Quest standalone](#step-by-step-for-quest-standalone)**

**[Step-by-step for Windows and Linux (both PCVR and flat)](#step-by-step-for-windows-and-linux-both-pcvr-and-flat)**

**[Add CD music](#cd-music)**

**[Controls](#controls)**

**[Extra features](#extra-features)**

**[To do](#to-do)**

## Step-by-step for Quest standalone
Copying the game assets. Its the same for all devices:
1. Install the original pc game from disk/download (such as Steam or GOG) like normal on your pc.
2. Copy the games assets from **C:\Program Files (x86)\Fox\Aliens versus Predator\** or from your Steam/GOG game directory. You need the following:

![Screenshot of assets files](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/assets-files.png)

4. Files and folders all need to be lower case. So to make that easier and in one go, download the [lowercase.ps1](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/assets/lowercase.ps1) powershell script or find it in the assets folder of this project. Place the script into the folder where the assets are.
5. Run the script in Powershell (type **lowercase.ps1**) and all files will now be lowercase. On windows, if you get a security error, use this command: **powershell -ExecutionPolicy Bypass -File .\lowercase.ps1**. On Linux, run it as: **pwsh ./lowercase.ps1**. (Install **pwsh** first if it's not there - **sudo apt install powershell** or via Snap, depending on distro.)
6. Now plug your headset in via usb.
7. Download the Aliens Versus Predator: VR release apk and install it with [SideQuest](https://sidequestvr.com/setup-howto) using the "Install APK file from folder on computer". (Your headset probably should be in [Developer](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/) mode already):

![Screenshot of apk install](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/sidequest_install.png)

6. After install completes, you should see this in the "Currently installed apps":

![Screenshot of apk location](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/sidequest_installed.png)

7. On the headset, run the **Aliens Versus Predator: VR** in Unknown Sources first, this will crash out as the assets haven't been added yet, this is normal and this sets the folders and their permissions in place. 
8. Now still in SideQuest, go to "Manage files on the headset".
9. Navigate to "sdcard/Android/data/com.bassquake.quest.avpvr/files". If installing on phone or tv, use the folder "sdcard/Android/data/com.bassquake.android.avpvr/files". (On build 0.5 and older the path is sdcard/Android/data/com.bassquake.avpvr/files).
10. Copy all the game assets into that files folder. The layout should be like so on your device:

![Screenshot of assets location](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/sidequest_files.png)

11. Now on your headset, go to Unknown Sources on the headset and click on Aliens Versus Predator: VR to play!

## Step by step for Windows and Linux (both PCVR and flat)

To run Aliens Versus Predator on Windows/Linux, download the zip from Releases page and extract into where the game assets are. It should look like this in screenshot below:

![Screenshot of assets location](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/assets-files-windows.png)

### Flat
Where you've saved the game assets folder, copy the **avp_x64.exe** or **avp_x86.exe** or **avp_arm64.exe** into it and simply double click the exe to run. Same for Linux. For Android devices, install the apk either via SideQuest or from a usb plugged into a Android TV for example, then copy assets into the data folder like how it's done for Quest.
### PCVR
Where you've saved the game assets folder, copy the **avp_x64vr.exe** into it and then add to Steam Library as a non-Steam game:

![Screenshot of adding non-Steam game](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/steam-add-app.png)

Rename the shortcut by going to Properties:

![Screenshot of Steam options](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/steam-add-options.png)

Then name the shortcut seen here:

![Screenshot of Shortcut naming](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/steam-custom-name.png)

To customise the images in Steam Library so it looks nicer like mine or use your own, download the extra zip file **steamvr-custom-images.zip** in Releases page for 0.7, unzip the images from steamvr-custom-images.zip somewhere. Then click the gear icon and select Properties:

![Screenshot of Steam options](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/steam-add-options.png)

Choose Customization and change images. The image files are named the same as the artwork title:

![Screenshot of Steam options](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/steam-add-images.png)

Finally, to play, on your headset, run Steam Link and navigate to the game in your library. Simply click Play! (Meta Link and Virtual Desktop is untested).

See below if want to include CD music if you have the physical game second CD...

## CD music
You can skip this if you don't have the audio cd that comes with the game, it'll just play without background music. Or provide your own music! Would need 15 tracks.
- Create a folder in **sdcard/Android/data/com.bassquake.quest.avpvr/files/** called **cd_tracks**.
- Or on Windows just create a folder called **cd_tracks** where the exe and the assets are.
- Rip the CD's audio tracks to Ogg Vorbis 160kbps and copy them over into the **cd_tracks** folder on your headset or Windows game location. Make sure they're lowercase and exactly named as track01.ogg, track02.ogg etc.

## Controls

### For VR

![Control layout](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/master/captures/avpvr-controllers.jpg)

> [!NOTE]
> If you have Manual Reload on in the Controls setting of the game, to trigger it, you almost touch controllers close together.

I will probably add ability to customise all these.

### For flat

Flat versions uses standard keyboard and mouse or joystick. They can be customised in the game. 

> [!NOTE]
> You may have to add WASD yourself as the game originally uses arrow keys.

## Extra features
- Can choose different frame rate in Video/Video Options, runs fine in 120fps mode!
- Can turn on/off the cross-hair in Video/Video Options.
- Frame rate counter can be toggled in Video/Video Options.
- Antialiasing options added for MSAA in Video/Video Options.
- Texture Filtering options added in Video/Video Options.
- Apparently works with the [HD AvP Redux Mod](https://www.moddb.com/mods/aliens-versus-predator-classic-redux/news/avp-classic-redux-20-released) pack! Edit 1: Seems there’s an issue with the HUD and crosshair for Marine with the Redux Mod. Edit 2: Fixed in 0.7.
- Cheat modes can be toggled now for those faint of heart!

## To do
- ~~Fix Battery Saver crash.~~ Fixed.
- ~~Fix audio direction.~~ Fixed.
- ~~Extra options for VR specific things like adding blinders or change snap/smooth turning etc.~~ Added.
- ~~Add rumble effects to controllers.~~ Added.
- ~~Add CD music.~~ Added.
- ~~Maybe add anti-aliasing options but not sure if really needed.~~ Added MSAA options in Audio/Video Options screen.
- ~~Add multiplayer functionality if possible.~~ Added.
- ~~Add cheat modes.~~ Added.
- ~~Add ability to adjust HUD.~~ Added.
- ~~Add Manual Reload option.~~ Added.
- ~~Add Quest 1 compatibility.~~ Added.
- ~~Add recalibrate option to right long press Meta button.~~ Added.
- ~~Add PCVR.~~ Added.
- To add correct wall walking for Aliens on Quest. Works fine on Windows. 
- Add option for left handed users.
- Add ability to customise controller key mapping.
- Customise some objects such as marines weapons as it has 2 hands attached to the main weapon which is a bit weird.
- Add shadow/fog effects?
