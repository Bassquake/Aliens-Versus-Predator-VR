# Aliens Versus Predator: VR

Fork of NakedAVP to use OpenXR for VR headets like the Meta Quest. This is early stage as lots of bugs and fixes are likely needed.

![Screenshot of Aliens Versus Predator: VR menu playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/avpvr-quest-menu.jpg)

![Screenshot of Aliens Versus Predator: VR in-game playing on Meta Quest 2](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/avpvr-quest-marine.jpg)

Video of it in action on a Quest 2 on [YouTube](https://www.youtube.com/watch?v=a33hBV9m_ks).

> [!NOTE]
> This is based on the code from _atsb_ over at [atsb/NakedAvP](https://github.com/atsb/NakedAVP). I've ported to Android and for it to use OpenXR for VR headsets. I have added binaries as an apk for Android 16 VR headsets. They do not include the game files as its not allowed. See important note below about game assets and there are instructions on how to add them.

> [!IMPORTANT]
> You need to supply the games asset files. Buy the game or find cd/downloads of Aliens Versus Predator Gold Edition. It has to be the Gold Edition as the standard versions 'language.txt' file crashes the game. You can use the standard versions files if you use the Gold Edition language.txt, the videos are different! Check [eBay](https://www.ebay.co.uk/sch/i.html?_nkw=aliens+versus+predator+gold+edition&_sacat=0&_from=R40&_trksid=m570.l1313&_odkw=aliens+versus+predator+gold&_osacat=0&_sop=15) or [GOG](https://www.gog.com/en/game/aliens_versus_predator_classic_2000) or [Steam](https://store.steampowered.com/app/3730/Aliens_versus_Predator_Classic_2000/).

## Step-by-step
- Install the original pc game from disk/download like normal.
- Download the [lowercase.ps1](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/assets/lowercase.ps1) powershell script or find it in the assets folder of this project. Place the script into the folder where all game files are (usually 'C:\Program Files (x86)\Fox\Aliens versus Predator').
- Run the script and all files will now be lowercase.
- Plug your headset in via usb.
- Download the Aliens Versus Predator: VR release apk and install it with [SideQuest](https://sidequestvr.com/setup-howto) using the "Install APK file from folder on computer". (Your headset probably should be in Developer mode):
![Screenshot of apk install](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/sidequest_install.png)

- After install completes, you should see this in the "Currently installed apps":

![Screenshot of apk location](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/sidequest_installed.png)

- Now still in SideQuest, go to "Manage files on the headset".
- Navigate to "sdcard/Android/data".
- Create a folder called "com.bassquake.avpvr" and inside that create a folder called "files". The folder layout should be like so on your device:

![Screenshot of assets location](https://github.com/Bassquake/Aliens-Versus-Predator-VR/blob/main/captures/sidequest_files.png)

# Run it!
Go to Unknown Sources on the headset and click on Aliens Versus Predator:VR. Have fun!

## Controls
> [!NOTE]
> At the moment I've only concentrated on the Marine level as that was my favourite! Alien and Predator will be looked at soon.

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

## Extra features
- Can choose different framerate in Video/Video Options, runs fine in 120fps mode!
- Can turn on/off the cross-hair in Video/Video Options
- Framerate counter can be toggled in Video/Video Options

# To do
- Add rumble effects to controllers
- Add cd music
- Adjust menu as its a bit close
- Add ability to customise controller key mapping 
- Customise some objects such as marines weapons as it has 2 hands attached to the main weapon which is a bit weird.
