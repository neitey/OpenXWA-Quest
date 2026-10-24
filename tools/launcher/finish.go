package main

import (
	"os"
	"path/filepath"
	"strings"
	"time"
)

// doneMenu is the last step. Its Close button doubles as the final "Press
// Enter to close", so main does not ask again. withStart adds "Start
// the game" (not in --logs mode).
func (a *app) doneMenu(withStart bool) {
	title, text := "All done", "Put on the headset and open "+productName+" (Library -> Unknown sources)."
	if a.lepton != "" {
		text = "On the headset, " + productName + " is on the Android home screen in the\n" +
			"\"Lepton Development\" window. (A copy installed another way may live in a\n" +
			"different Lepton instance with its own files; use the one installed here.)\n\n" +
			"Closing Lepton Development deletes the app and the copied game files unless\n" +
			"its Launch Options (Steam Library > Lepton Development > Properties) are:\n" +
			"    LEPTON_NO_CLEANUP=1 %command%\n\n" +
			"With this setting they should stay. If it was not set yet: set it, start\n" +
			"Lepton Development again and run this launcher again."
	}
	if !withStart {
		title, text = "Logs collected", "Attach the files in the logs folder to your bug report."
	}
	for {
		var opts []Option
		if withStart {
			opts = append(opts, Option{ID: "start", Label: "Start the game on the headset", Key: "s", Style: "primary"})
		}
		opts = append(opts, Option{ID: "logs", Label: "Collect logs", Key: "l"})
		if a.logsDir != "" {
			opts = append(opts, Option{ID: "openlogs", Label: "Open logs folder", Key: "o"})
		}
		opts = append(opts, Option{ID: "close", Label: "Close", Key: ""})
		ans := ui.Ask(Question{Kind: "buttons", Title: title, Text: text, Options: opts, Default: "close"})
		switch ans.ID {
		case "start":
			out, err := a.adb.shell(30*time.Second, "am start -n "+appActivity)
			if err != nil || strings.Contains(out, "Error") {
				say("Could not start the game: %s", strings.TrimSpace(out))
			} else {
				say("%s is starting on the headset. Put it on!", productName)
			}
		case "logs":
			a.collectLogs()
		case "openlogs":
			ui.OpenPath(a.logsDir)
		default:
			a.skipPause = true
			return
		}
		if ans.Closed {
			a.skipPause = true
			return
		}
	}
}

// collectLogs pulls the app's logs and its settings file (openxwa-quest.cfg:
// the VR options and the hand-placed HUD, console and stick positions) from
// the headset into
// <exe dir>/logs-<date-time>/ together with the launcher's own log.
// Without a headset only the launcher's log is saved.
func (a *app) collectLogs() {
	dir := filepath.Join(a.exeDir, "logs-"+time.Now().Format("20060102-150405"))
	if err := os.MkdirAll(dir, 0o755); err != nil {
		dir = filepath.Join(os.TempDir(), filepath.Base(dir))
		if err := os.MkdirAll(dir, 0o755); err != nil {
			say("Could not create a folder for the logs: %v", err)
			return
		}
	}
	got := 0
	if a.adb != nil && a.adb.Serial != "" {
		// openxwa-crash.txt is only there after a crash (kept until the next one).
		for _, name := range []string{"openxwa.log", "openxwa-prev.log", "openxwa-crash.txt", "openxwa-quest.cfg"} {
			if _, err := a.adb.run(2*time.Minute, "pull", a.remoteBase()+"/"+name, filepath.Join(dir, name)); err == nil &&
				(name == "openxwa.log" || name == "openxwa-prev.log") {
				got++
			}
		}
		// The system log has what the game's own log cannot: the OpenXR
		// loader's and runtime's messages (why VR did not start) and crashes.
		if out, err := a.adb.shell(2*time.Minute, "logcat -d -v time -t 30000"); err == nil && strings.TrimSpace(out) != "" {
			_ = os.WriteFile(filepath.Join(dir, "logcat.txt"), []byte(out), 0o644)
		}
		if got == 0 {
			say("The game has not written a log on the headset yet (start it once first).")
		}
	} else {
		say("No headset is connected, so only the launcher's own log is saved.")
	}
	if lp := logPath(); lp != "" {
		if data, err := os.ReadFile(lp); err == nil {
			_ = os.WriteFile(filepath.Join(dir, logFileName), data, 0o644)
		}
	}
	a.logsDir = dir
	say("Logs saved in: %s", dir)
	say("Please attach the files in that folder to your bug report.")
}
