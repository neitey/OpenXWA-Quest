package main

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

type adbDevice struct {
	serial string
	state  string // device, unauthorized, offline, authorizing, "no permissions", ...
	model  string // from "model:Quest_3" in devices -l
}

// parseDevices reads the output of "adb devices -l".
func parseDevices(out string) []adbDevice {
	var devs []adbDevice
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "List of devices") || strings.HasPrefix(line, "*") || strings.HasPrefix(line, "adb ") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		d := adbDevice{serial: fields[0], state: fields[1]}
		if d.state == "no" && len(fields) > 2 && fields[2] == "permissions" {
			d.state = "no permissions"
		}
		for _, f := range fields[2:] {
			if strings.HasPrefix(f, "model:") {
				d.model = strings.ReplaceAll(strings.TrimPrefix(f, "model:"), "_", " ")
			}
		}
		devs = append(devs, d)
	}
	return devs
}

// stateRank orders the states so the most useful message wins when several
// devices are listed.
var stateRank = map[string]int{
	"unauthorized": 9, "no permissions": 8, "offline": 7, "authorizing": 6, "connecting": 5,
	"recovery": 4, "sideload": 4, "rescue": 4, "bootloader": 4, "host": 2,
}

// waitState picks the state that describes the situation ("none" if no
// device is listed at all).
func waitState(devs []adbDevice) string {
	best, rank := "none", -1
	for _, d := range devs {
		r, ok := stateRank[d.state]
		if !ok {
			r = 1 // unknown states
		}
		if r > rank {
			best, rank = d.state, r
		}
	}
	return best
}

// waitMessage tells the user what to do for a device state.
func waitMessage(state string) string {
	switch state {
	case "none":
		return "Connect the headset to this computer with a USB-C cable and switch it on.\n" +
			"(Developer mode must be on for the headset. See README.txt.)"
	case "unauthorized":
		return "The headset is connected but has not allowed this computer yet.\n" +
			"Put on the headset and look for the \"Allow USB debugging?\" prompt. Choose\n" +
			"Allow (tick \"Always allow from this computer\"). On Steam Frame the prompt may\n" +
			"appear in the Android (Lepton) environment.\n" +
			"This is needed once per computer; after that the headset can stay on the desk."
	case "offline":
		return "The headset is connected but not responding (offline).\n" +
			"Unplug the cable and plug it in again, or try another USB port or cable."
	case "authorizing":
		return "The headset is checking this computer's key... please wait."
	case "connecting":
		return "Connecting to the headset... please wait."
	case "no permissions":
		return "adb is not allowed to use the headset's USB connection.\n" +
			"On Linux, set up the udev rules for Android devices and reconnect.\n" +
			"On Windows, check that the headset's USB driver is installed."
	case "recovery", "sideload", "rescue", "bootloader":
		return "The headset is in " + state + " mode. Restart it normally and reconnect."
	case "host":
		return "adb sees another computer instead of a headset. Connect the headset with\n" +
			"a USB-C cable."
	default:
		return "The headset reports an unusual state (" + state + ").\n" +
			"Unplug the cable and plug it in again."
	}
}

// stuckStates are states where restarting the connection may help.
var stuckStates = map[string]bool{"offline": true, "unauthorized": true, "authorizing": true, "connecting": true}

// deviceDisplayName turns getprop values into "Meta Quest 3", "PICO 4", ...
func deviceDisplayName(manufacturer, model string) string {
	manufacturer = strings.TrimSpace(manufacturer)
	model = strings.TrimSpace(model)
	if model == "" {
		if manufacturer == "" {
			return "headset"
		}
		return manufacturer + " headset"
	}
	brand := manufacturer
	lb := strings.ToLower(brand)
	if lb == "oculus" || lb == "meta" || strings.Contains(lb, "facebook") {
		brand = "Meta"
	}
	lm := strings.ToLower(model)
	if brand == "" || strings.HasPrefix(lm, strings.ToLower(brand)) || strings.HasPrefix(lm, "oculus") || strings.HasPrefix(lm, "meta") {
		return model
	}
	return brand + " " + model
}

// restartConnection restarts the adb server and asks adb to reconnect
// devices that are stuck offline.
func (a *app) restartConnection() {
	say("Restarting the connection to the headset...")
	serial := a.adb.Serial
	a.adb.Serial = ""
	a.adb.run(30*time.Second, "kill-server")
	sleepCtx(time.Second)
	a.adb.run(60*time.Second, "start-server")
	a.adb.run(30*time.Second, "reconnect", "offline")
	a.adb.Serial = serial
	a.reconnectNet()
}

// connect is step 3: ask how to reach the headset (USB cable, USB then
// Wi-Fi, or Wi-Fi), wait until a headset is connected and allowed, then
// check that its shell and storage really work.
func (a *app) connect() error {
	if a.tunnel != nil {
		a.tunnel.stop()
		a.tunnel = nil
	}
	a.adb.Serial = ""
	if _, err := a.adb.run(60*time.Second, "start-server"); err != nil {
		if err == errClosed {
			return err
		}
		say("adb had trouble starting; trying anyway.")
	}
	route, err := a.chooseRoute()
	if err != nil {
		return err
	}
	a.route = route
	a.lib = nil
	if route == routeFrame {
		err := a.frameConnect()
		if err == errBackToUSB {
			return a.connect() // ask the route again
		}
		return err
	}
	if route == routeWiFi {
		err := a.wifiFlow()
		if err == nil {
			return a.checkDevice()
		}
		if err != errBackToUSB {
			return err
		}
		logf("Wi-Fi: back to waiting for USB")
	}
	if err := a.waitUSB(); err != nil {
		if err == errWiFiConnected {
			return a.checkDevice()
		}
		return err
	}
	logf("using device %s", a.adb.Serial)
	if err := a.checkDevice(); err != nil {
		return err
	}
	if a.route == routeUSBWiFi && a.lepton == "" && !isNetSerial(a.adb.Serial) {
		if err := a.switchToWiFi(); err != nil {
			if err == errClosed {
				return err
			}
			logf("switch to Wi-Fi failed: %v", err)
			say("%v", err)
		}
	}
	return nil
}

// errWiFiConnected: while waiting for USB, the user connected over Wi-Fi.
var errWiFiConnected = errors.New("connected over Wi-Fi")

// usbWaitOffer is the button card shown while waiting for USB.
func usbWaitOffer(state string, stuck bool) Question {
	wifi := Option{ID: "wifi", Label: "Use Wi-Fi instead"}
	if !stuck {
		return Question{
			Kind:    "buttons",
			Title:   "No USB connection?",
			Text:    "Steam Frame: Wi-Fi is recommended (its USB connection may not show up on\nWindows).",
			Options: []Option{wifi},
		}
	}
	return Question{
		Kind:  "buttons",
		Title: "Still not connected",
		Text:  "The headset has been \"" + state + "\" for a while. Restarting the connection often helps.",
		Options: []Option{
			{ID: "restart", Label: "Restart connection", Style: "primary"},
			{ID: "again", Label: "Try again"},
			wifi,
		},
	}
}

// waitUSB waits until one USB headset is ready and sets a.adb.Serial. A
// "Use Wi-Fi instead" button stays available (errWiFiConnected).
func (a *app) waitUSB() error {
	lastOut, lastState, lastMsg := "\x00", "", ""
	stateSince := time.Now()
	var offer <-chan Answer
	offeredStuck, autoReconnected := false, false
	offerShown := false
	failures := 0
	defer ui.ClearOffer()
	for {
		if appCtx.Err() != nil {
			return errClosed
		}
		out, err := a.adb.runQuiet(20*time.Second, "devices", "-l")
		if out != lastOut {
			logf("adb devices -l changed:\n%s", out)
			lastOut = out
		}
		if err != nil {
			if err == errClosed {
				return err
			}
			failures++
			if failures == 5 {
				say("adb does not answer. If another program uses adb (SideQuest, Meta Quest\n" +
					"Developer Hub, Android Studio), close it and try again.")
			}
			sleepCtx(2 * time.Second)
			continue
		}
		failures = 0
		devs := parseDevices(out)
		var ready []adbDevice
		for _, d := range devs {
			if d.state == "device" {
				ready = append(ready, d)
			}
		}
		if len(ready) > 1 {
			// Wi-Fi entries left from earlier runs do not count while a
			// USB headset is there.
			if usb := usbOnly(ready); len(usb) > 0 && len(usb) < len(ready) {
				logf("ignoring network devices while waiting for USB: %v", ready)
				ready = usb
			}
		}
		if len(ready) == 1 {
			a.adb.Serial = ready[0].serial
			return nil
		}
		if len(ready) > 1 {
			ui.ClearOffer()
			if s := a.pickAndroid(ready); s != "" {
				a.adb.Serial = s
				return nil
			}
			a.adb.Serial = a.chooseDevice(ready)
			if a.adb.Serial == "" {
				return errClosed
			}
			return nil
		}

		state := waitState(devs)
		if state != lastState {
			lastState, stateSince = state, time.Now()
			if offeredStuck {
				ui.ClearOffer()
				offer, offeredStuck, offerShown = nil, false, false
			}
		}
		if msg := waitMessage(state); msg != lastMsg {
			say("%s", msg)
			lastMsg = msg
		}
		stuck := stuckStates[state] && time.Since(stateSince) > 20*time.Second
		if stuck && !offeredStuck {
			offeredStuck, offerShown = true, true
			offer = ui.Offer(usbWaitOffer(state, true))
			if offer == nil && state == "offline" && !autoReconnected {
				// Console: try once by itself.
				autoReconnected = true
				say("Trying to reconnect...")
				a.adb.run(30*time.Second, "reconnect", "offline")
			}
		} else if !offerShown {
			offerShown = true
			offer = ui.Offer(usbWaitOffer(state, false))
		}
		select {
		case ans := <-offer:
			offer, offeredStuck, offerShown = nil, false, false
			ui.ClearOffer()
			stateSince = time.Now()
			lastMsg = ""
			switch ans.ID {
			case "restart":
				a.restartConnection()
			case "wifi":
				logf("waiting for USB: the user chose Wi-Fi")
				err := a.wifiFlow()
				if err == nil {
					return errWiFiConnected
				}
				if err != errBackToUSB {
					return err
				}
			}
		case <-time.After(2 * time.Second):
		case <-appCtx.Done():
			return errClosed
		}
	}
}

// usbOnly returns the devices that are not network connections.
func usbOnly(devs []adbDevice) []adbDevice {
	var out []adbDevice
	for _, d := range devs {
		if !isNetSerial(d.serial) {
			out = append(out, d)
		}
	}
	return out
}

// chooseDevice lets the user pick one of several connected devices.
func (a *app) chooseDevice(ready []adbDevice) string {
	var opts []Option
	for i, d := range ready {
		name := d.model
		if name == "" {
			name = "Device"
		}
		opts = append(opts, Option{ID: d.serial, Label: fmt.Sprintf("%s (%s)", name, d.serial), Key: fmt.Sprint(i + 1)})
	}
	ans := ui.Ask(Question{Kind: "buttons", Title: "Several devices are connected", Text: "Which one is your headset?",
		Options: opts, Default: ready[0].serial})
	if ans.Closed && appCtx.Err() != nil {
		return ""
	}
	return ans.ID
}

// shellProblem explains a failed "adb shell".
func shellProblem(out string) string {
	l := strings.ToLower(out)
	switch {
	case strings.Contains(l, "error: closed"):
		return "The headset closed the connection (\"error: closed\"). This often happens\n" +
			"right after allowing USB debugging, or when another program (SideQuest, Meta\n" +
			"Quest Developer Hub, Android Studio) uses adb at the same time."
	case strings.Contains(l, "offline"):
		return "The headset went offline. Unplug the cable and plug it in again, or try\n" +
			"another USB port or cable."
	case strings.Contains(l, "unauthorized"):
		return "The headset has not allowed this computer. Put it on and choose Allow."
	case strings.Contains(l, "more than one device"):
		return "More than one device is connected. Unplug the other devices."
	case strings.Contains(l, "not found"):
		return "The headset disappeared. Check the USB cable or Wi-Fi connection."
	}
	return "The headset does not answer commands yet."
}

// checkDevice runs right after a device reports "device": some devices say
// so before their shell is ready. It also finds where shared storage is.
func (a *app) checkDevice() error {
	var out string
	var err error
	ok := false
	for try := 1; try <= 5; try++ {
		out, err = a.adb.run(20*time.Second, "shell", "echo ok")
		if err == errClosed {
			return err
		}
		if err == nil && strings.Contains(out, "ok") {
			ok = true
			break
		}
		logf("shell check %d of 5 failed: %v", try, err)
		if !sleepCtx(time.Duration(try) * 1500 * time.Millisecond) {
			return errClosed
		}
	}
	if !ok {
		return &userError{
			msg: "The headset was found, but the connection could not be established.\n\n" +
				shellProblem(out) + "\n\nUse \"Restart connection\", or unplug and replug the cable, then try again.",
			restart: true,
		}
	}

	// An Android device answers getprop with its SDK level. Steam Frame's USB
	// connection is its Linux system (SteamOS) instead; OpenXWA runs in its
	// Android layer (Lepton), reached through a second adb connection.
	sdk, sdkErr := a.adb.shell(20*time.Second, "getprop ro.build.version.sdk")
	if sdkErr == errClosed {
		return sdkErr
	}
	if _, err := strconv.Atoi(firstLine(sdk)); err != nil {
		logf("no Android SDK level from %s (%q, %v); checking for SteamOS", a.adb.Serial, strings.TrimSpace(sdk), sdkErr)
		if !a.isSteamOS() {
			return &userError{msg: "The connected device does not answer like an Android headset.\n" +
				"Please collect the logs and report this, naming your headset model.", restart: true}
		}
		if err := a.connectLepton(a.adb.Serial); err != nil {
			return err
		}
	}

	prop := func(name string) string {
		o, _ := a.adb.shell(20*time.Second, "getprop "+name)
		return firstLine(o)
	}
	model, manu := prop("ro.product.model"), prop("ro.product.manufacturer")
	logf("device: serial=%s manufacturer=%q model=%q device=%q sdk=%q release=%q build=%q",
		a.adb.Serial, manu, model, prop("ro.product.device"), prop("ro.build.version.sdk"),
		prop("ro.build.version.release"), prop("ro.build.fingerprint"))
	via := ""
	if isNetSerial(a.adb.Serial) && a.forwardPort == 0 {
		via = " over Wi-Fi"
		if a.tunnel != nil {
			via = " through SSH"
		}
	}
	if a.lepton != "" {
		say("Found: Steam Frame (Android layer at %s%s)", a.lepton, via)
	} else {
		say("Found: %s%s", deviceDisplayName(manu, model), via)
	}
	a.uid = 0
	return a.detectStorage()
}

// storageProbe prints DIR/LIST/NOLIST/MISSING for <root>/Android/data.
func storageProbe(root string) string {
	d := shQuote(root + "/Android/data")
	return "if [ -d " + d + " ]; then echo DIR; if ls " + d + " >/dev/null 2>&1; then echo LIST; else echo NOLIST; fi; else echo MISSING; fi"
}

// detectStorage finds the shared storage path that works on this headset:
// /sdcard normally, else $EXTERNAL_STORAGE or /storage/emulated/0.
func (a *app) detectStorage() error {
	env, _ := a.adb.shell(20*time.Second, "echo $EXTERNAL_STORAGE")
	env = firstLine(env)
	var cands []string
	for _, c := range []string{"/sdcard", env, "/storage/emulated/0", "/storage/self/primary"} {
		if strings.HasPrefix(c, "/") && !contains(cands, c) {
			cands = append(cands, c)
		}
	}
	for _, root := range cands {
		out, err := a.adb.shell(20*time.Second, storageProbe(root))
		if err == errClosed {
			return err
		}
		logf("storage probe %s: %q (%v)", root, strings.TrimSpace(out), err)
		if !strings.Contains(out, "DIR") {
			continue
		}
		a.storage = root
		if root != defaultStorage {
			say("Using the headset storage at %s.", root)
		}
		if strings.Contains(out, "NOLIST") {
			logf("Android/data exists but cannot be listed by adb (permission denied)")
			say("Note: this headset does not let adb list Android/data. The launcher will\n" +
				"still try to use the game folder directly.")
		}
		return nil
	}
	logf("no storage root with Android/data found (EXTERNAL_STORAGE=%q)", env)
	return &userError{msg: "The headset's shared storage (where the game keeps its files) was not found.\n" +
		"Restart the headset and try again. If it keeps happening, please collect the\n" +
		"logs and report it, naming your headset model.", restart: true}
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

func firstLine(s string) string {
	s = strings.TrimSpace(s)
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		s = s[:i]
	}
	return strings.TrimSpace(s)
}
