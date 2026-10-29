// The launcher of Neitey's XW: Alliance VR (OpenXWA-VR-Launcher.exe): puts
// the VR app (based on OpenXWA) and the user's own X-Wing Alliance game
// files on a standalone VR headset over USB, using adb.
//
// The design is recorded in PLAN.md. Standard library only. The user sees
// either a small web page in an app window (default) or the console
// (--console).
package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// version is set at build time: -ldflags "-X main.version=1.0.0".
var version = "dev"

// The product name players read (window, texts). File names, the package id
// and the repo keep "OpenXWA".
const (
	productName  = "Neitey's XW: Alliance VR"
	launcherName = productName + " Launcher"
)

const (
	appName        = "OpenXWA" // technical (folder names), not shown to players
	appPackage     = "org.openxwa.quest"
	appActivity    = "org.openxwa.quest/org.openxwa.quest.OpenXwaActivity"
	defaultStorage = "/sdcard"
	appDataRel     = "/Android/data/org.openxwa.quest/files"
	manifestName   = "openxwa-install-files.txt"
	manifestHeader = "# OpenXWA install file list v1"
)

// appCtx is cancelled when the launcher must stop (the window was closed);
// running adb commands are then stopped.
var appCtx, appCancel = context.WithCancel(context.Background())

// errClosed: the user closed the window; stop quietly.
var errClosed = errors.New("the launcher window was closed")

type options struct {
	help        bool
	showVersion bool
	logsOnly    bool
	noUpdate    bool
	console     bool
	noWindow    bool
	gameArg     string
}

// userError carries a message meant for the user (already friendly).
type userError struct {
	msg       string
	restart   bool // offer "Restart connection" (adb problems)
	retryStep int  // "Try again" goes back to this step (1-based) instead
}

func (e *userError) Error() string { return e.msg }

func fail(format string, args ...any) error {
	return &userError{msg: fmt.Sprintf(format, args...)}
}

func usage() string {
	return launcherName + ` ` + version + `

Puts ` + productName + ` (based on OpenXWA) and your own X-Wing Alliance
game files on your VR headset (Meta Quest, Pico, Steam Frame) over a USB
cable or Wi-Fi.

Usage:
  OpenXWA-VR-Launcher [options] [game folder]

You can also drag your X-Wing Alliance folder onto OpenXWA-VR-Launcher.exe.

Options:
  --console           use a console window instead of the launcher window
  --no-update-check   do not look for a new launcher version
  --logs              only collect the game's logs from the headset
  --no-window         do not open a window; print the launcher's address
  --version           show the launcher version
  --help              show this help
`
}

func parseArgs(args []string) (options, error) {
	var o options
	for _, a := range args {
		switch strings.ToLower(a) {
		case "--help", "-help", "-h", "/?", "/h":
			o.help = true
		case "--version", "-version", "-v":
			o.showVersion = true
		case "--logs", "-logs":
			o.logsOnly = true
		case "--no-update-check", "-no-update-check":
			o.noUpdate = true
		case "--console", "-console":
			o.console = true
		case "--no-window", "-no-window":
			o.noWindow = true
		default:
			if strings.HasPrefix(a, "-") {
				if _, err := os.Stat(a); err != nil {
					return o, fmt.Errorf("unknown option %q", a)
				}
			}
			if o.gameArg == "" {
				o.gameArg = a
			}
		}
	}
	return o, nil
}

func main() {
	if runAskpass() { // started by ssh as SSH_ASKPASS (see sshtunnel.go)
		return
	}
	opts, err := parseArgs(os.Args[1:])
	if err != nil || opts.help || opts.showVersion {
		setupConsole() // the Windows build has no console of its own
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		fmt.Fprint(os.Stderr, "\n"+usage())
		os.Exit(2)
	}
	if opts.help {
		fmt.Print(usage())
		return
	}
	if opts.showVersion {
		fmt.Println(launcherName + " " + version)
		return
	}

	a := newApp()
	openLog(a.exeDir)
	logf("OpenXWA-VR Launcher %s starting; exe dir %s; args %q", version, a.exeDir, os.Args[1:])

	var closed <-chan struct{}
	if !opts.console {
		w, err := startWebUI()
		if err != nil {
			logf("could not start the web UI (%v); using the console", err)
			opts.console = true
		} else {
			ui = w
			closed = w.Closed()
			w.openWindow(a.exeDir, opts.noWindow)
		}
	}
	if opts.console {
		setupConsole()
		ui = newConsoleUI(os.Stdin)
	}

	done := make(chan int, 1)
	go func() { done <- a.run(opts) }()
	var code int
	select {
	case code = <-done:
	case <-closed:
		// The window was closed: give the flow a moment to stop its adb
		// command, then leave.
		select {
		case <-done:
		case <-time.After(8 * time.Second):
			logf("the flow did not stop in time; exiting anyway")
		}
		code = 0
	}
	logf("exit code %d", code)
	ui.Finish(code, !a.skipPause)
	closeLog()
	os.Exit(code)
}

func exeDirectory() string {
	exe, err := os.Executable()
	if err != nil {
		wd, _ := os.Getwd()
		return wd
	}
	if r, err := filepath.EvalSymlinks(exe); err == nil {
		exe = r
	}
	return filepath.Dir(exe)
}

// app holds the state of one launcher run.
type app struct {
	exeDir    string
	adb       *ADB
	skipPause bool // the final question was already the closing one

	apkPath     string
	apkVersion  string
	gameDir     string
	storage     string // "/sdcard" or a fallback such as "/storage/emulated/0"
	logsDir     string // last folder logs were collected into
	uid         int    // the app's Linux uid on the headset (0 = not known yet)
	appOwner    *owner // owner shown for a folder the app made itself (nil = not seen yet)
	mkdirOK     bool   // folders made with mkdir belong to the app on this headset
	helper      bool   // the app's helper (SetupProvider) copies the files
	helperTried bool
	importMode  string // "" (try obb), "obb" (works), "stream" (fallback)

	// Steam Frame: the Android layer (Lepton) is reached over a second adb
	// connection.
	lepton      string // its serial, e.g. "localhost:5555" or "frame:5555"
	frameUSB    string // the SteamOS USB serial
	forwardPort int    // port forwarded over USB (0 = none)

	// Wi-Fi (wifi.go, sshtunnel.go).
	route   string        // usb, usbwifi or wifi (the connection step's choice)
	netHost string        // the headset's name or IP address over Wi-Fi
	tunnel  *sshTunnel    // Steam Frame through SSH (nil = none)
	lib     *frameLibrary // Steam Frame as a Steam Library game (steamlib.go); nil = the adb routes
}

func newApp() *app {
	return &app{exeDir: exeDirectory(), storage: defaultStorage}
}

// Headset folders, below the storage root that works on this headset.
func (a *app) remoteBase() string   { return a.storage + appDataRel }
func (a *app) remoteGame() string   { return a.remoteBase() + "/XWA" }
func (a *app) remotePilots() string { return a.remoteBase() + "/pilot-import" }

// run executes the whole flow and returns the process exit code.
func (a *app) run(opts options) int {
	say("%s %s", launcherName, version)
	say("Puts %s and your own X-Wing Alliance files on your VR headset.", productName)
	err := a.flow(opts)
	a.cleanupLepton()
	switch {
	case err == nil:
		return 0
	case errors.Is(err, errClosed) || appCtx.Err() != nil:
		logf("stopped: %v", err)
		return 0
	default:
		logf("stopped with error: %v", err)
		return 1
	}
}

// either picks the Steam Library route's step (a.lib set by connect) or
// the adb one.
func (a *app) either(frame, adb func() error) func() error {
	return func() error {
		if a.lib != nil {
			return frame()
		}
		return adb()
	}
}

type step struct {
	title string
	run   func() error
}

func (a *app) flow(opts options) error {
	if opts.logsOnly {
		return a.runSteps([]step{
			{"Finding adb", a.setupADB},
			{"Connecting to the headset", a.connect},
			{"Collecting logs", func() error { a.collectLogs(); a.doneMenu(false); return nil }},
		})
	}

	steps := []step{
		{"Finding adb", func() error {
			if err := a.findAPK(); err != nil {
				return err
			}
			return a.setupADB()
		}},
		{"Finding your X-Wing Alliance folder", func() error { return a.chooseGame(opts.gameArg) }},
		{"Connecting to the headset", a.connect},
		{"Installing the app", a.either(a.frameInstall, a.installApp)},
		{"Preparing the game folder on the headset", a.either(func() error { return nil }, a.prepareAppFolder)},
		{"Pilots", a.either(a.frameCopyPilots, a.copyPilots)},
		{"Copying the game files", a.either(a.frameCopyGame, a.copyGame)},
		{"Done", func() error { a.doneMenu(true); return nil }},
	}
	if !opts.noUpdate {
		if w, ok := ui.(*webUI); ok {
			var titles []string
			for _, s := range steps {
				titles = append(titles, s.title)
			}
			w.SetSteps(titles) // show the steps while the update check runs
			w.SetHeadline("Checking for updates")
		}
		if a.checkForUpdates() {
			a.skipPause = true
			return nil
		}
	}
	return a.runSteps(steps)
}

// runSteps runs the steps in order. When one fails, an error card offers
// "Try again" (re-runs that step), logs and Close.
func (a *app) runSteps(steps []step) error {
	var titles []string
	for _, s := range steps {
		titles = append(titles, s.title)
	}
	ui.SetSteps(titles)
	for i := 0; i < len(steps); {
		if appCtx.Err() != nil {
			return errClosed
		}
		ui.SetStep(i+1, "current")
		logf("=== step %d of %d: %s", i+1, len(steps), steps[i].title)
		err := steps[i].run()
		ui.Progress(nil)
		if err == nil {
			ui.SetStep(i+1, "done")
			i++
			continue
		}
		if errors.Is(err, errClosed) || appCtx.Err() != nil {
			return errClosed
		}
		ui.SetStep(i+1, "error")
		logf("step %q failed: %v", steps[i].title, err)
		switch a.errorCard(err) {
		case "retry":
			var ue *userError
			if errors.As(err, &ue) && ue.retryStep > 0 && ue.retryStep <= i+1 {
				i = ue.retryStep - 1
			}
			continue
		case "restart":
			a.restartConnection()
			continue
		}
		a.skipPause = true
		return err
	}
	return nil
}

// errorCard shows the problem and returns "retry", "restart" or "close".
func (a *app) errorCard(err error) string {
	msg := err.Error()
	restart := false
	var ue *userError
	if errors.As(err, &ue) {
		restart = ue.restart && a.adb != nil
	} else {
		msg = "Something went wrong: " + msg
	}
	for {
		opts := []Option{{ID: "retry", Label: "Try again", Key: "r", Style: "primary"}}
		if restart {
			opts = append(opts, Option{ID: "restart", Label: "Restart connection", Key: "c"})
		}
		opts = append(opts,
			Option{ID: "logs", Label: "Collect logs", Key: "l"},
			Option{ID: "openlog", Label: "Open log file", Key: "o"})
		if a.logsDir != "" {
			opts = append(opts, Option{ID: "openlogs", Label: "Open logs folder", Key: "f"})
		}
		opts = append(opts, Option{ID: "close", Label: "Close", Key: ""})
		text := msg
		if _, console := ui.(*consoleUI); console && logPath() != "" {
			text += "\n\nDetails are in the log file: " + logPath()
		}
		ans := ui.Ask(Question{Kind: "buttons", Error: true, Title: "That did not work", Text: text, Options: opts, Default: "close"})
		if ans.Closed {
			return "close"
		}
		switch ans.ID {
		case "retry", "restart":
			return ans.ID
		case "logs":
			a.collectLogs()
		case "openlog":
			ui.OpenPath(logPath())
		case "openlogs":
			ui.OpenPath(a.logsDir)
		default:
			return "close"
		}
	}
}
