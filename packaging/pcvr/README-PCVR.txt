Neitey's XW: Alliance VR (PCVR) @VERSION@ - X-Wing Alliance in PC VR
=====================================================================

Play your own copy of Star Wars: X-Wing Alliance in VR on a Windows PC with
a PC VR headset: SteamVR headsets (Valve Index, HTC Vive, ...), Meta Quest
through Quest Link / Air Link (or Virtual Desktop / Steam Link), Windows
Mixed Reality and other OpenXR headsets. The same program also runs as the
normal flat-screen game (Windows, OpenXR).

Neitey's XW: Alliance VR is based on OpenXWA
(https://github.com/elyosh/OpenXWA) by Adrien Moulin, the open-source
reimplementation of X-Wing Alliance.

This is a FIRST PCVR build: it is the same VR mode as the Quest version, but
it has not been tested on a real PC headset yet. Please report what works and
what does not (see "Problems?" below).

No game files are included. You need your own installed X-Wing Alliance
(Steam, GOG or the original CD version).


Requirements
------------
- Windows 10 or 11, 64-bit, with a DirectX 12 capable GPU (any GeForce GTX
  900 / Radeon RX 400 or newer; VR wants a GTX 1070 / RX 5700 class GPU).
- An OpenXR runtime set as the ACTIVE one:
    SteamVR:        SteamVR -> Settings -> OpenXR -> "Set SteamVR as OpenXR
                    runtime" (on older versions: Settings -> Developer).
    Meta Quest Link: Meta Quest Link app (formerly Oculus app) -> Settings ->
                    General -> OpenXR Runtime -> "Set Meta Quest Link as
                    active".
    Windows Mixed Reality: the Mixed Reality Portal / OpenXR Tools for WMR.
  Only one runtime can be active; the game uses whichever one is.
- The original X-Wing Alliance game folder (the one holding ALLIANCE,
  FLIGHTMODELS and MISSIONS), e.g.
    C:\Program Files (x86)\Steam\steamapps\common\Star Wars X-Wing Alliance
    C:\GOG Games\Star Wars - X-Wing Alliance


Install
-------
1. Unzip this folder anywhere (not into the game folder; it never changes
   the original game).
2. Start OpenXWA.exe once on the monitor (no headset needed): it asks for the
   X-Wing Alliance folder and remembers it. Or give it on the command line
   (also works with "OpenXWA VR.bat"):
     OpenXWA.exe --game-data "C:\GOG Games\Star Wars - X-Wing Alliance"
   Windows may say "Windows protected your PC" (the program is not
   code-signed): "More info" -> "Run anyway".


Starting in VR
--------------
- Start your VR runtime (SteamVR, or connect the Quest with Link / Air Link)
  and put the headset on.
- Double-click "OpenXWA VR.bat" (it runs OpenXWA.exe --xr).
- Or run OpenXWA.exe --xr from a shortcut, or add the line AERON_XR=1 to
  %APPDATA%\TotallyOpen\OpenXWA\openxwa-vr.cfg to always start in VR.
- Without --xr (plain double-click on OpenXWA.exe) it is the flat-screen game.
- If no OpenXR runtime or headset is found, the game says so in the log and
  starts on the monitor instead.
While in VR the desktop window shows a mirror of the game.


Controls (Meta Quest Touch controllers; Index and others below)
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
  Taking the headset off or opening the runtime's menu pauses the game.
  VR Options -> Swap Thumbsticks: the two sticks trade what they do in
  flight (left: pitch / yaw, right: roll, throttle); buttons stay put.
  VR Options -> Stick Response: Precise (the default) makes small stick
  movements turn gently for fine aim, full deflection still turns at full
  rate; Direct is linear, Very Precise even gentler.
  Side panel on your right: tactical map, voice commands and the button
  mapping; touch a tab (or point and pull the trigger). Touch the map to
  zoom.
  VR settings and help: game menu -> VR Options.

Other controllers (bindings suggested by the game; SteamVR lets you rebind
them in its controller settings):
  Valve Index:  as Touch, left A/B = X/Y, game menu = press the LEFT
                trackpad, voice = double-tap a trackpad with the thumb.
  HTC Vive wands: trackpad = stick (press = stick click), grip buttons =
                grip, left menu button = game menu, right menu button = A.
                B / X / Y are not bound (use the keyboard or rebind).
  Windows Mixed Reality: thumbstick = stick, right trackpad press = A,
                right menu = B, left trackpad press = X, left menu = game
                menu. HP Reverb G2: as Touch.
  Keyboard, mouse and gamepads work as in the flat-screen game.


Settings and log files
----------------------
In %APPDATA%\TotallyOpen\OpenXWA (paste that into the Explorer address bar):
  openxwa.log       the log of the last run (openxwa-prev.log: the one before)
  openxwa-vr.cfg    the VR settings (written by the VR Options; a few more
                    are described in the file, e.g. SDL_GPU_DRIVER=vulkan to
                    use Vulkan instead of Direct3D 12)
  config.yaml       the game's settings, pilots are next to it


Voice commands
--------------
Speech recognition runs on the PC (no internet), with the model in the voice
folder and sherpa-onnx-c-api.dll / onnxruntime.dll. It uses the default
Windows microphone (with Quest Link: set the headset's microphone as the
default recording device). Turn it off in VR Options -> Voice Commands.


Problems?
---------
Please send openxwa.log (see above) with a short description: headset,
runtime (SteamVR / Quest Link / ...), GPU, what happened.


Licence
-------
OpenXWA and Neitey's XW: Alliance VR are free software under the GNU GPL
version 3 (LICENSE.txt). Source: https://github.com/neitey/OpenXWA-Quest (branch pcvr)
Third-party components and their licences: THIRD-PARTY-NOTICES.txt and the
licenses folder.
Star Wars and X-Wing Alliance are trademarks of their owners; this is an
unofficial fan project and contains no game data.
