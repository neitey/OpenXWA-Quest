# OpenXWA-VR Launcher (PC) - plan

Windows PC app (one portable .exe, no install) that puts OpenXWA and the
user's own X-Wing Alliance game files on a standalone headset over USB.
Quest, Pico and Steam Frame use the same steps. Mac/Linux later.
Implemented in this folder (Go, standard library only; build.sh): a window UI
(embedded page in an Edge/Chrome app window, web/index.html) or --console.
This file records the original design; see the code for the current flow
(ownership checks, Steam Frame via Lepton).

Since 1.0.0-rc6 the game files are not pushed one by one: on Quest
(Horizon OS 14) every file and folder adb makes in the app's files folder
belongs to the "shell" user and the app cannot read it. The APK has a
helper (SetupProvider, see appimport.go) that adb reaches without starting
the game. rc7: the launcher packs the files into zips of 128 MB, copies
each with adb push into the app's obb folder (where the app can read what
adb copies), and the app unpacks it into its files folder. If the app
cannot read the obb folder, the zips are streamed into the app instead
(exec-in content write; adb's input is kept open until every byte has
arrived, since closing it early crashed the adb server in rc6). A dropped
USB connection is waited for (90 s). The headset can stay on the desk.
Older apps without the helper fall back to pushing.

## Wi-Fi (launcher-wifi)

A Steam Frame user's Frame never showed up over USB on Windows, and the
launcher only offered Wi-Fi after it had found the Frame over USB. Now
the connection step comes first for every headset (wifi.go,
routeQuestion): "USB cable", "USB, then switch to Wi-Fi" (Quest/Pico:
after the USB connection works, read the wlan0 address, `adb tcpip 5555`,
`adb connect <ip>:5555`; lasts until the headset sleeps or restarts) or
"Wi-Fi" (name or IP address; remembered in launcher.ini as `connection`
and `wifi_host`). While waiting for USB, a "Use Wi-Fi instead" button
stays available.

Wi-Fi: the name is resolved first (IPv4 preferred), port 5555 is probed
(open / refused / timeout / unreachable) and, if refused, 5556-5599 are
scanned at once (other Lepton instances); then `adb connect` and a
getprop check, waiting for "Allow USB debugging?" when unauthorized.
Each failure kind has its own text (name not found -> use the IP;
refused -> Lepton not running / Quest Wi-Fi debugging off; timeout ->
same network, client isolation). Port 22 answering marks a Steam Frame.

Steam Frame through SSH (sshtunnel.go), since Lepton may listen on the
Frame's loopback only (reported by Frame Control, against Valve's docs):
the system OpenSSH client runs
`ssh -T -L 127.0.0.1:<free>:127.0.0.1:5555 steamos@<frame> "ss -ltn; echo
marker; cat"` and adb connects to 127.0.0.1:<free>. The remote command
lists Lepton's ports (the tunnel is restarted for another port if 5555 is
not among them) and keeps the session open until the launcher closes
ssh's input, so the tunnel also ends when the launcher dies. Own
known_hosts file (accept-new; "Forget the old key" after a reset),
password/keyboard-interactive only, one prompt, keep-alives. The
Developer Mode password: window UI -> a password field; ssh runs without
a console (DETACHED_PROCESS / setsid) with SSH_ASKPASS = the launcher exe
itself (SSH_ASKPASS_REQUIRE=force for OpenSSH 8.4+, DISPLAY for older
ones), which fetches the password once from a loopback listener with a
random token. If ssh never asks askpass (older Windows OpenSSH builds),
ssh is started again in its own console window for the user to type into.
--console: ssh asks in the launcher's console. The password lives in
memory for the run only (to restart a dropped tunnel); never logged or
saved.

Robustness: a dropped network connection is waited for 3 minutes with
`adb connect` (and tunnel restart) every few seconds; imports resend the
batch without using up a try, pushes continue and the verify step copies
what is missing. The progress shows speed and time left. Everything
(route, probes, ports, adb output, ssh command line, ssh output and exit
codes, askpass prompts) goes to the launcher log.

To verify on real hardware: Frame USB visibility on Windows, direct
`adb connect frame:5555` vs. the SSH tunnel, Windows OpenSSH askpass vs.
the console-window fallback, Lepton's prompt over the tunnel, and Quest
`adb tcpip` switching and Wi-Fi drops during a copy.

## Distribution

One zip per release on GitHub Releases (neitey/OpenXWA-Quest):

    OpenXWA-Quest-<version>.zip
      OpenXWA-VR-Launcher.exe
      OpenXWA-quest-<version>.apk
      README.txt
      THIRD-PARTY-NOTICES.txt
      LICENSE.txt               (GPL-3)

plus `SHA256SUMS.txt` as a separate release asset. The launcher installs
the APK next to it. No game files are ever included. Launcher source
lives in this repo (GPL-3). 1.0 ships unsigned (README explains the
SmartScreen warning); apply to SignPath Foundation for free signing,
which requires the .exe to be built by GitHub Actions from the public
repo. The APK keeps being signed locally; its key stays out of the repo.

## adb

1. Reuse an existing adb: PATH, SideQuest, Meta Quest Developer Hub,
   Android SDK platform-tools.
2. Otherwise download Google's platform-tools
   (https://dl.google.com/android/repository/platform-tools-latest-windows.zip)
   after showing the Android SDK License Agreement and getting the user's
   own "I agree". Unpack into the launcher's own folder. Never bundled
   (SDK licence section 3.4 forbids redistribution).

## Install flow

1. Find the game: Steam library folders, GOG registry entries, original
   CD registry entry; several found -> user picks; none -> "Browse...".
   Validate the folder (RESDATA.TXT and the data folders present).
2. Connect: wait for the headset, prompt "allow USB debugging" until
   authorised; read the device model (Quest / Pico / Steam Frame).
3. Install or update the APK (`adb install -r`). On a signature mismatch,
   stop and explain; never uninstall (that deletes the game files).
4. Let the app create its own folder (rc6+: through its helper).
5. Build the PC file list with sizes; skip files the port doesn't use
   (.exe, .dll).
6. Ask whether to bring existing PC pilot files over.
7. Create all folders on the headset first, in one shell call.
8. Push folder by folder with a progress bar.
9. Verify: list the headset folder with sizes, compare, re-push missing or
   wrong-size files individually, up to 3 tries; name any file that still
   fails.
10. Write a file list (manifest) on the headset; the app checks it at start
    and lists exactly which files are missing.
11. Done: button to start the game, or Library -> Unknown sources.

Running it again updates the app and copies only missing/changed files.
"Collect logs" pulls openxwa.log for bug reports.

## Update check

- At launcher start (and from a "Check for updates" button), request
  `https://api.github.com/repos/neitey/OpenXWA-Quest/releases/latest`.
  No account or token needed while the repo is public (GitHub allows 60
  unauthenticated requests per hour per IP; one per start is plenty).
- Compare the release tag (e.g. `v1.0.1`) with the launcher's own
  version. Release candidates (`-rcN`) are only offered when
  "Include test versions" is ticked.
- Newer version found: show version, date and the release notes, and ask
  "Download now?" [Download] [Later] [Skip this version].
- Download: the release zip into the user's Downloads folder with a
  progress bar; check its SHA-256 against `SHA256SUMS.txt` from the same
  release; extract to a new folder next to the zip.
- Then offer "Open the new version now". The new launcher continues with
  the normal install/update flow (app update + changed files only).
  The running launcher never replaces itself.
- No internet or GitHub unreachable: skip silently; the button reports
  the error. No telemetry: the only data sent is the request itself.
- Setting "Check for updates at start" (on by default).
