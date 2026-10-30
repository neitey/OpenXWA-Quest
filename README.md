# Neitey's XW: Alliance VR

*Star Wars: X-Wing Alliance* in virtual reality, standalone on Meta Quest 3 /
3S and Steam Frame: no PC needed while you play. Pico is supported but
experimental. Latest builds also include PCVR Zip!

Neitey's XW: Alliance VR is based on
[OpenXWA](https://github.com/elyosh/OpenXWA), the open-source
reimplementation of the 1999 game by Adrien Moulin. It runs the
original game data with a VR cockpit, motion controllers and a full immersive VR menu
environment.

[![Neitey's XW: Alliance VR gameplay on Meta Quest 3: click for the video](assets/vr/gameplay.gif)](assets/vr/openxwa-vr-trailer.mp4)

*Recorded on a Meta Quest 3. Click the picture for an 80-second video.*

![In the X-wing cockpit, with the HUD panels around the canopy](assets/vr/cockpit_flight.jpg)

> [!IMPORTANT]
> No content from the original game is included. You need your own installed
> *X-Wing Alliance* on a Windows PC, from
> [GOG](https://www.gog.com/en/game/star_wars_xwing_alliance) or
> [Steam](https://store.steampowered.com/app/361670/STAR_WARS__XWing_Alliance/)
> (the original CD version works too).

## Features

- VR cockpits for the playable craft, with the HUD placed per cockpit
- A touch console on your lap: energy, shields, targeting, wingmen and ship
  commands at your fingertips
- Side panel on your right: tactical map, voice commands and the button
  mapping
- Radial menu (hold a grip) for wingman orders and other commands
- Voice commands, recognised on the headset: double-tap a thumb rest and
  say e.g. "attack my target"
- Motion controllers: flight stick and throttle lever in the cockpit, or the
  thumbsticks
- Laser pointer for every menu, and a virtual keyboard for pilot names
- Immersive 3D menu environments - re-creating the iconic box arts of X-wing games in full 3D.
- Death menu with Restart Mission and an in-mission Quick Save / Quick Load
- Pause menu pages for Review Briefing and Mission Hints
- Your PC pilots can be brought along

Gameplay video: https://www.reddit.com/r/OculusQuest/s/OZG4dItx3W

## Screenshots

| | |
|---|---|
| ![The touch console on the pilot's lap](assets/vr/cockpit_console.jpg) | ![The radial menu, opened with a grip](assets/vr/radial_menu.jpg) |
| Touch console: energy, targeting, wingmen, ship | Radial menu: hold a grip, point, pull the trigger |
| ![The tactical map on the side panel](assets/vr/tactical_map.jpg) | ![Controller labels showing each button](assets/vr/controller_labels.jpg) |
| Side panel with the tactical map | Every button labelled on the controller |

![The VR menu environment around the game screen](assets/vr/menu_environment.jpg)
Immersive 3D Menu Environments.

## Install

You need a headset in Developer Mode (Meta Horizon phone app -> Devices ->
your headset -> Headset settings -> Developer Mode) and a USB-C cable that
carries data.

1. Download `OpenXWA-Quest-<version>.zip` from
   [Releases](https://github.com/neitey/OpenXWA-Quest/releases/latest) and
   unzip it on your Windows PC.
2. Connect the headset by USB and run `OpenXWA-VR-Launcher.exe`.
3. The launcher finds your X-Wing Alliance, installs the app, copies the game
   files (about a minute) and checks them. Put the headset on once to allow
   USB debugging ("Always allow from this computer"); after that it can stay
   on the desk.
4. In the headset: Library -> "Unknown sources" -> Neitey's XW: Alliance VR.

Updating works the same way: run the new launcher; it updates the app and
copies only what changed. Your pilots and settings stay. `README.txt` in the
zip has the manual install steps without the launcher.

Windows may warn that the launcher is not code-signed: "More info" -> "Run
anyway". If it cannot find Google's adb tool (SideQuest and Meta Quest
Developer Hub bring their own), it offers to download it from Google.

**Steam Frame:** turn on Developer Mode (Steam Settings > System) and choose
"Steam Frame (into its Steam Library)" in the launcher. It copies the game to
the headset over Wi-Fi (SSH, with the Developer Mode password; needs Windows'
OpenSSH Client) and adds it to the Steam Library, where it stays like any
other game: no "Lepton Development" needed, nothing is deleted when it closes.
The older "Lepton Development" way (USB or Wi-Fi) still works too.

## Controls

Intuitive command wheel & touch interactive holo-console give access to +25 inputs (Shields, energy, targeting).

Additional 20 voice commands are implemented. All commands can be looked up on the in-cockpit console.

| Control | Action |
|---|---|
| Right stick | Pitch / yaw |
| Left stick | Roll (X), throttle (Y) |
| Right trigger | Fire |
| Left trigger | Change weapon |
| Right grip | Countermeasures |
| Left grip (hold) | Radial menu |
| A / B | Next / previous target |
| X | Target closest enemy |
| Y | Target my attacker |
| Right stick click | Target in sights |
| Left stick click | Link weapons |
| Left menu button | Game menu |
| Thumb rest (double-tap, then speak) | Voice command |

In menus the right controller is a laser pointer. Taking the headset off or
opening the Meta menu pauses the game. VR settings and a controller help page
are in the game menu under **VR Options**. There, **Swap Thumbsticks**
(`XWA_XR_SWAP_STICKS=1`) trades what the two sticks do in flight (left stick:
pitch / yaw; right stick: roll, throttle) while every button stays where it
is; **Controls: Left-handed** mirrors both controllers, buttons included.

## Problems

Run the launcher and choose **Collect logs** at the end (or start it with
`--logs`); it saves the game's log files next to the launcher. Please include
them with a report.

## What's next?

Multiplayer, new VR environments, Tie-Fighter support.

## Building

The Android build uses the Android NDK with CMake (`packaging/quest`); the
PC launcher is a Go program in `tools/launcher` (`build.sh`). PC VR through
OpenXR is part of the same source. The upstream build notes are below.

## Credits

- **AI assisted coding:** Neitey
- **VR design, menu environments & QA:** MarkD
- **OpenXWA:** Adrien Moulin and contributors

VR-specific code was generated using Claude AI; additional artwork was created
using ChatGPT. Third-party components and their licences are listed in
`packaging/quest/THIRD-PARTY-NOTICES.txt`.

## Licence

GPL version 3 (`LICENSE`). Star Wars and X-Wing Alliance are trademarks of
their owners; this is an unofficial fan project and contains no game data.

## About OpenXWA (upstream)

[![GitHub Release](https://img.shields.io/github/v/release/elyosh/OpenXWA)](https://github.com/elyosh/OpenXWA/releases/latest)
[![Visit our Discord server](https://img.shields.io/discord/1533001488391995442)](https://discord.gg/WBvYzczWfG)

OpenXWA is an open-source reimplementation of *Star Wars: X-Wing Alliance*
for Windows, macOS, and Linux. It runs the original game data natively on
current systems while preserving the original experience and offering
optional modern enhancements. The sections below are from its README.

### Graphics

OpenXWA offers classic and modern graphics modes. Classic mode preserves the
original game's appearance, while modern mode adds high-resolution rendering
and advanced lighting.

Modern graphics include:

- native widescreen rendering at the display resolution
- cascaded directional shadows, ambient occlusion, bloom, and motion blur
- anisotropic texture filtering
- 2x, 4x, or 8x MSAA
- AMD FidelityFX FSR 3.1.4 temporal anti-aliasing and upscaling
- HDR output

Original OPT models work in both modes. Modern mode can also load optional
replacement models, textures, interface art, and videos without modifying the
original game data.

### Smoother flight and modern controls

Single-player flight can run at a higher simulation rate for smoother motion
and more responsive input on modern displays. A classic timing mode remains
available when the original timing is preferred.

OpenXWA adds mouse flight control in both Virtual Stick and Direct modes, with
adjustable sensitivity and Y-axis inversion. Flight can be played with a mouse
and keyboard, so a joystick or gamepad is no longer required. Modern gamepads
and joysticks are also supported, with force-feedback effects translated to
gamepad rumble.

### Getting started

1. Download the latest package for your platform from
   [GitHub Releases](https://github.com/elyosh/OpenXWA/releases/latest).
2. Extract or install the package and launch OpenXWA.
3. Select the folder containing a complete *X-Wing Alliance* installation when
   prompted.
4. To fly with a mouse, open **Game Controller Options** and enable
   **Mouse Flight Control**.

OpenXWA validates and remembers the selected installation. The folder must use
the installed game layout found in a GOG or Steam installation. For a raw CD
copy, first merge the contents of the `ALLIANCE` directory into the root.

Open **Video Options**, then **OpenXWA Video Options**, to configure the
renderer and display settings. Mouse and controller settings are available
under **Game Controller Options**.

Useful shortcuts:

| Key | Action |
|---|---|
| `F5` | Switch between modern and classic graphics |
| `F2` | Switch between split comparison and the selected view |
| `Ctrl`+`Alt`+`M` | Release or recapture the mouse during flight |

For unattended or development launches, pass the game-data folder on the
command line:

```sh
OpenXWA --game-data /path/to/xwa-data
```

### Supported platforms

| Platform | Target | Graphics backend |
|---|---|---|
| Windows | x86-64 | Direct3D 12 or Vulkan |
| macOS | macOS 13 or later; arm64 or x86-64 | Metal |
| Linux | x86-64; glibc 2.35 or later | Vulkan |

### Current state

Every original function not dedicated to multiplayer has been reimplemented,
including the digital iMUSE audio engine.

OpenXWA remains under active development. Bugs and differences from the
original game are still possible despite this level of coverage. Multiplayer
is not implemented.

### OpenTIE

Fans of Totally Games' space simulators may also be interested in
[OpenTIE](https://github.com/elyosh/OpenTIE), an open-source reimplementation
of *Star Wars: TIE Fighter* for Windows, macOS, and Linux.

### Community

Join the [TotallyOpen Discord server](https://discord.gg/WBvYzczWfG) to discuss
OpenXWA, OpenTIE, development, and the Totally Games flight simulators.

### System requirements

- a 64-bit system with a modern GPU
- a complete installation of *X-Wing Alliance*
- a mouse and keyboard, gamepad, or joystick for flight

Release packages include the required runtime libraries. Keep the executable,
libraries, resources, and shader directories together when moving an
installation.

### Building from source

The build requires CMake 3.20 or later, a C/C++ toolchain, SDL3 3.4, zstd,
FFmpeg, and SDL_shadercross. Release packaging pins its dependencies and
provides the reference for reproducible builds.

Platform-specific instructions are available for
[Windows](packaging/windows/README.md),
[macOS](packaging/macos/README.md), and
[Linux](packaging/linux/README.md).
