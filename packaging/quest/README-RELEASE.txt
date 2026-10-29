Neitey's XW: Alliance VR @VERSION@ - X-Wing Alliance in standalone VR
=====================================================================

Play your own copy of Star Wars: X-Wing Alliance on Meta Quest 3 / 3S.
Pico and Steam Frame are supported but not tested yet.

Neitey's XW: Alliance VR is based on OpenXWA
(https://github.com/elyosh/OpenXWA) by Adrien Moulin, the open-source
reimplementation of X-Wing Alliance.

You need your own installed X-Wing Alliance (Steam, GOG or the original
CD version) on a Windows PC. No game files are included in this download.


Before you start (once)
-----------------------
1. Turn on Developer Mode for the headset:
   Meta Horizon phone app -> Devices -> your headset -> Headset settings ->
   Developer Mode (needs a free Meta developer account).
2. Have a USB-C cable that carries data (not a charge-only cable).


Install (and update)
--------------------
1. Unzip this folder anywhere on the PC.
2. Connect the headset to the PC with the USB cable.
3. Double-click OpenXWA-VR-Launcher.exe and follow the steps in the window.
   It finds your X-Wing Alliance, installs the app on the headset, copies
   the game files (about 1 GB, a few minutes), checks every file arrived
   and asks whether to bring your PC pilots along.
   Put on the headset when it asks you to allow USB debugging (tick
   "Always allow from this computer").
4. Put on the headset: Library -> filter "Unknown sources" ->
   Neitey's XW: Alliance VR.

After you have allowed USB debugging once, the headset can stay on the desk
(switched on, cable connected) while the launcher copies: the app
receives and unpacks the files itself, without starting the game.

Steam Frame: turn on Developer Mode. Once, before starting "Lepton
Development": in your Steam Library, open its Properties and set Launch
Options to
   LEPTON_NO_CLEANUP=1 %command%
Without this, closing Lepton Development deletes the app and the copied
game files; with it they should stay. Then start "Lepton Development"
and keep it running; the launcher connects to it over the USB cable (or
Wi-Fi). Neitey's XW: Alliance VR appears in the Lepton Development
window's Android home screen. Not tested on a Steam Frame yet.

Windows may say "Windows protected your PC": the launcher is not
code-signed yet. Click "More info", then "Run anyway".

The launcher needs Google's adb tool. If it can't find one (SideQuest and
Meta Quest Developer Hub bring their own), it offers to download it from
Google; you accept Google's licence terms yourself.

Updating: download the new release and run its launcher. It updates the
app and copies only missing or changed files; your pilots and settings
stay. The launcher also tells you when a new version is out.

If the game was found somewhere unusual: drag the X-Wing Alliance folder
onto OpenXWA-VR-Launcher.exe, or paste its path when the launcher asks.


Controls
--------
  Right stick: pitch / yaw          Left stick: roll (X), throttle (Y)
  Right trigger: fire               Right stick click: target in sights
  Right grip: countermeasures       A / B: next / previous target
  X: target closest enemy           Y: target my attacker
  Left trigger: change weapon       Left stick click: link weapons
  Left grip (hold): radial menu     Left menu button: game menu (Esc)
  Thumb rest (double-tap, then speak): voice command, e.g. "attack my
  target" (VR Options -> Voice Commands: double-tap, hold or off)
  In menus the right controller is a laser pointer.
  Taking the headset off or opening the Meta menu pauses the game.
  VR Options -> Swap Thumbsticks: the two sticks trade what they do in
  flight (left: pitch / yaw, right: roll, throttle); buttons stay put.
  VR Options -> Stick Response: Precise (the default) makes small stick
  movements turn gently for fine aim, full deflection still turns at full
  rate; Direct is linear, Very Precise even gentler.
  Side panel on your right: tactical map, voice commands and the button
  mapping; touch a tab (or point and pull the trigger). Touch the map to
  zoom.
  VR settings and help: game menu -> VR Options.


Problems?
---------
Run the launcher and choose "collect logs" at the end (or start it with
--logs): it copies the game's log files into a folder next to the
launcher. Please send those along with your report.

Uninstalling the app (Settings or adb uninstall) also deletes the copied
game files and your pilots on the headset.


Manual install without the launcher
-----------------------------------
Files copied with "adb push" belong to the wrong user on Quest, so the game
cannot read them. Let the app unpack a zip instead:
  adb install -r OpenXWA-quest-@VERSION@.apk
  Zip the CONTENTS of your X-Wing Alliance folder into xwa.zip (select
  everything inside the folder -> Send to -> Compressed (zipped) folder), so
  RESDATA.TXT sits at the top of the zip. Then:
  adb shell content call --uri content://org.openxwa.quest.setup --method mkdirs --arg XWA
  adb push xwa.zip /sdcard/Android/obb/org.openxwa.quest/openxwa-import.zip
  adb shell content call --uri content://org.openxwa.quest.setup --method unzip --arg XWA
The last command prints the number of files unpacked; the app deletes the
zip afterwards.


Licence
-------
OpenXWA and Neitey's XW: Alliance VR are free software under the GNU GPL
version 3 (LICENSE.txt). Source: https://github.com/neitey/OpenXWA-Quest
Third-party components and their licences: THIRD-PARTY-NOTICES.txt.
Star Wars and X-Wing Alliance are trademarks of their owners; this is an
unofficial fan project and contains no game data.
