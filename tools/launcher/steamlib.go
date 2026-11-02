package main

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"io"
	"os"
	"path"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// The Steam Frame as a Steam Library game ("Steam Frame" route).
//
// Valve's Android layer (Lepton) runs each game in its own container with
// its data in an ordinary folder on the headset, started by a small script.
// The launcher puts the app, that script and the game files into one folder
// over SSH and adds the game to the Steam Library through Steam's devkit
// interface (the way Valve's Devkit Management Tool does; Steam adds such an
// entry while it runs, no restart). Nothing is deleted when the game closes,
// "Lepton Development" is not needed, and no adb is involved.
//
//	~/Applications/neitey-xw-alliance-vr/
//	    launch.sh                   what Steam starts: sets the paths, runs "lepton start"
//	    lepton-app/game.apk         the app
//	    lepton-data/                the game's data; Android's /sdcard is lepton-data/external
//	        external/Android/data/org.openxwa.quest/files/   XWA, pilot-import, openxwa.log, the settings
//	    lepton-shaders/             shader cache
//	    launch.log                  Lepton's output of the last start
//	~/devkit-game/<game id>/launch.sh -> the script above (Steam's devkit entry)
//
// What Lepton reads (STEAM_COMPAT_*, SteamAppId, IS_PARENT) is documented
// by the FramePort project's runtime reference and by Lepton's own launches.

const (
	frameAppDirRel = "Applications/neitey-xw-alliance-vr"
	frameGameID    = "Neiteys_XW_Alliance_VR" // Steam's devkit ids: letters, digits, "_"
	leptonSteamID  = "3029110"                // "Lepton" in the Steam Library
	frameProbeEnd  = "__XWA_PROBE_END__"
)

type frameLibrary struct {
	ssh          *sshRunner
	home         string
	steam        string // ~/.local/share/Steam
	lepton       string // the lepton binary, "" when not installed
	users        []string
	steamRunning bool
	appDir       string
	appid        uint32 // the Steam shortcut's id, 0 = not in the Library yet
	apkSize      int64  // game.apk on the Frame (0 = none)
	apkSHA       string
	availKB      int64
	hostname     string
}

func (l *frameLibrary) filesDir() string {
	return l.appDir + "/lepton-data/external/Android/data/" + appPackage + "/files"
}
func (l *frameLibrary) gameDir() string   { return l.filesDir() + "/XWA" }
func (l *frameLibrary) pilotsDir() string { return l.filesDir() + "/pilot-import" }
func (l *frameLibrary) apkPath() string   { return l.appDir + "/lepton-app/game.apk" }

// steam://rungameid/ id of a shortcut: the appid in the high word.
func (l *frameLibrary) runGameID() uint64 { return uint64(l.appid)<<32 | 0x02000000 }

// frameProbeScript reports what the launcher needs to know, as key=value
// lines (POSIX sh; the Frame's shell is bash).
func frameProbeScript() string {
	return `H=$HOME; echo "home=$H"
S="$H/.local/share/Steam"; [ -d "$S" ] || S="$H/.steam/steam"; echo "steam=$S"
echo "hostname=$(hostname 2>/dev/null)"
L=""
if [ -x "$S/steamapps/common/Lepton/lepton" ]; then L="$S/steamapps/common/Lepton/lepton"; fi
if [ -z "$L" ] && [ -f "$S/steamapps/libraryfolders.vdf" ]; then
  grep -o '"path"[[:space:]]*"[^"]*"' "$S/steamapps/libraryfolders.vdf" | sed 's/^"path"[[:space:]]*"//; s/"$//' | while IFS= read -r p; do
    if [ -x "$p/steamapps/common/Lepton/lepton" ]; then echo "lepton=$p/steamapps/common/Lepton/lepton"; fi
  done
fi
[ -n "$L" ] && echo "lepton=$L"
[ -f "$H/.steam/steam.token" ] && [ -p "$(readlink -f "$H/.steam/steam.pipe" 2>/dev/null)" ] && echo steam_running=1
for u in "$S"/userdata/*/; do u=${u%/}; u=${u##*/}; case "$u" in ''|'*'|0) ;; *) echo "user=$u";; esac; done
D="$H/` + frameAppDirRel + `"; echo "dir=$D"
if [ -f "$D/lepton-app/game.apk" ]; then echo "apk_size=$(stat -c %s "$D/lepton-app/game.apk")"; echo "apk_sha=$(sha256sum "$D/lepton-app/game.apk" | cut -c1-64)"; fi
echo "avail_kb=$(df -Pk "$H" | awk 'NR==2 {print $4}')"
echo ` + frameProbeEnd
}

func parseFrameProbe(out string) (*frameLibrary, bool) {
	l := &frameLibrary{}
	done := false
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimRight(line, "\r")
		if line == frameProbeEnd {
			done = true
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		switch k {
		case "home":
			l.home = v
		case "steam":
			l.steam = v
		case "hostname":
			l.hostname = v
		case "lepton":
			if l.lepton == "" {
				l.lepton = v
			}
		case "steam_running":
			l.steamRunning = v == "1"
		case "user":
			l.users = append(l.users, v)
		case "dir":
			l.appDir = v
		case "apk_size":
			l.apkSize, _ = strconv.ParseInt(v, 10, 64)
		case "apk_sha":
			l.apkSHA = v
		case "avail_kb":
			l.availKB, _ = strconv.ParseInt(v, 10, 64)
		}
	}
	return l, done && l.home != "" && l.appDir != ""
}

// ---------------------------------------------------------------- connecting

func frameHostQuestion(value, problem string) Question {
	return Question{
		Kind:  "text",
		Title: "Steam Frame",
		Text: "The game goes into the Steam Frame's Steam Library: the launcher copies the\n" +
			"app and your game files to the headset over SSH and adds the game to the\n" +
			"Library. You then start it like any other game. Nothing is deleted when it\n" +
			"closes, and \"Lepton Development\" is not needed.\n\n" +
			"Developer Mode must be on (Steam Settings > System), with the password you\n" +
			"set there. The PC and the headset must be on the same network.\n\n" +
			"Enter the headset's name or IP address (\"frame\" usually works; the IP\n" +
			"address is in Quick Settings or Steam Settings > Internet).",
		Problem:     problem,
		Value:       value,
		Placeholder: "frame or 192.168.1.23",
		Options: []Option{
			{ID: "connect", Label: "Connect", Style: "primary"},
			{ID: "back", Label: "Back"},
		},
		Default:       "connect",
		ConsolePrompt: "Steam Frame name or IP address (Enter = " + value + ", b = back):",
	}
}

func (a *app) askFrameHost(problem string) (string, error) {
	value := a.launcherSettings().get("wifi_host", "")
	if value == "" {
		value = "frame"
	}
	ans := ui.Ask(frameHostQuestion(value, problem))
	if ans.Closed {
		return "", errClosed
	}
	host := strings.TrimSpace(ans.Text)
	if ans.ID == "back" || strings.EqualFold(host, "b") {
		return "", errBackToUSB
	}
	if host == "" {
		host = value
	}
	logf("Steam Frame host entered: %q", host)
	return host, nil
}

func askFramePassword(problem string) (string, error) {
	ans := ui.Ask(Question{
		Kind:  "text",
		Title: "Developer Mode password",
		Text: "The launcher logs in to the Steam Frame over SSH (user \"steamos\").\n\n" +
			"Enter the Developer Mode password (the one you set when you turned on\n" +
			"Developer Mode). It is used for this connection only and is not saved.",
		Problem:     problem,
		Secret:      true,
		Placeholder: "Developer Mode password",
		Options: []Option{
			{ID: "connect", Label: "Connect", Style: "primary"},
			{ID: "back", Label: "Back"},
		},
		Default:       "connect",
		ConsolePrompt: "Developer Mode password:",
	})
	if ans.Closed {
		return "", errClosed
	}
	if ans.ID == "back" {
		return "", errBackToUSB
	}
	return ans.Text, nil
}

// frameConnect is the "Steam Frame" route's connection step: SSH only.
func (a *app) frameConnect() error {
	problem, pwProblem, host := "", "", ""
	var r *sshRunner
	for {
		if host == "" {
			h, err := a.askFrameHost(problem)
			if err != nil {
				return err
			}
			host, problem = h, ""
			r = nil
		}
		if r == nil {
			rr, err := newSSHRunner(host)
			if err != nil {
				var wp *wifiProblem
				if errors.As(err, &wp) && (wp.kind == "nossh" || wp.kind == "nowindow") {
					return fail("%s", wp.msg)
				}
				problem, host = err.Error(), ""
				continue
			}
			r = rr
		}
		if r.mode == "askpass" && r.password == "" {
			pw, err := askFramePassword(pwProblem)
			if err != nil {
				return err
			}
			r.password, pwProblem = pw, ""
		}
		say("Connecting to %s over SSH...", host)
		out, err := r.sh(90*time.Second, frameProbeScript())
		if err != nil {
			if err == errClosed {
				return err
			}
			kind := sshErrorKind(err)
			logf("Steam Frame probe failed (%s): %v", kind, err)
			switch kind {
			case "sshauth":
				pwProblem = sshProblemText("sshauth", host)
				r.password = ""
				continue
			case "hostkey":
				if a.askForgetHostKey(host) {
					forgetHostKey(sshKnownHostsPath(), r.host)
					continue
				}
				return errClosed
			case "notfound", "sshrefused", "timeout", "unreachable":
				problem, host = sshProblemText(kind, host), ""
				continue
			case "noaskpass":
				return fail("%s", sshProblemText("nowindow", host))
			}
			switch a.frameProblemCard("The connection to the Steam Frame did not work", err.Error()) {
			case "retry":
				continue
			case "host":
				host = ""
				continue
			}
			return errClosed
		}
		l, ok := parseFrameProbe(out)
		if !ok {
			logf("probe output:\n%s", out)
			switch a.frameProblemCard("The Steam Frame answered, but not as expected", "The launcher could not read what the headset reported. Details are in the\nlauncher log.") {
			case "retry":
				continue
			case "host":
				host = ""
				continue
			}
			return errClosed
		}
		l.ssh = r
		a.lib = l
		a.netHost = host
		a.saveSetting("wifi_host", host)
		logf("Steam Frame %q: home %s, steam %s, lepton %q, users %v, steam running %v, app folder %s, apk %d bytes, free %d MB",
			l.hostname, l.home, l.steam, l.lepton, l.users, l.steamRunning, l.appDir, l.apkSize, l.availKB/1024)
		say("Connected to the Steam Frame (%s).", l.hostname)
		if err := a.frameCheckLepton(); err != nil {
			return err
		}
		return nil
	}
}

func (a *app) frameProblemCard(title, text string) string {
	ans := ui.Ask(Question{Kind: "buttons", Error: true, Title: title, Text: text,
		Options: []Option{
			{ID: "retry", Label: "Try again", Key: "", Style: "primary"},
			{ID: "host", Label: "Change the address", Key: "a"},
			{ID: "close", Label: "Close", Key: "c"},
		}, Default: "retry"})
	if ans.Closed && appCtx.Err() != nil {
		return "close"
	}
	return ans.ID
}

// frameCheckLepton makes sure Valve's Lepton is installed on the Frame;
// Steam can be asked to install it (the user confirms in the headset).
func (a *app) frameCheckLepton() error {
	l := a.lib
	for l.lepton == "" {
		ans := ui.Ask(Question{Kind: "buttons", Title: "Lepton is not installed on the Steam Frame",
			Text: "The game runs in \"Lepton\", Valve's Android layer, which is a free item in\n" +
				"the Steam Library. The launcher can ask Steam on the headset to install it;\n" +
				"put the headset on and confirm the install there, then choose Check again.\n" +
				"(Starting \"Lepton Development\" once also installs it.)",
			Options: []Option{
				{ID: "install", Label: "Ask Steam to install Lepton", Key: "i", Style: "primary"},
				{ID: "check", Label: "Check again", Key: "c"},
				{ID: "close", Label: "Close", Key: ""},
			}, Default: "install"})
		if ans.Closed && appCtx.Err() != nil {
			return errClosed
		}
		switch ans.ID {
		case "install":
			if _, err := l.ssh.sh(30*time.Second, "steam -ifrunning steam://install/"+leptonSteamID+" >/dev/null 2>&1 || true"); err != nil && err == errClosed {
				return err
			}
			say("Asked Steam on the headset to install Lepton. Confirm it in the headset.")
			fallthrough
		case "check":
			out, err := l.ssh.sh(60*time.Second, frameProbeScript())
			if err != nil {
				if err == errClosed {
					return err
				}
				return fail("The connection to the Steam Frame was lost: %v", err)
			}
			if n, ok := parseFrameProbe(out); ok {
				l.lepton = n.lepton
			}
		default:
			return errClosed
		}
	}
	return nil
}

// ------------------------------------------------------------------ uploads

// tarItem is one file for an upload: from disk (Abs) or from memory (Data).
type tarItem struct {
	Rel  string
	Abs  string
	Data []byte
	Size int64
}

func tarItemsOf(files []gameFile) []tarItem {
	out := make([]tarItem, len(files))
	for i, f := range files {
		out[i] = tarItem{Rel: f.Rel, Abs: f.Abs, Size: f.Size}
	}
	return out
}

// uploadTar streams the items as a tar archive into `dir` on the Frame
// (created if needed; the files group-writable, as Lepton's app writes
// through the folder's group). prog may be nil.
func (l *frameLibrary) uploadTar(dir string, items []tarItem, prog *copyProgress, timeout time.Duration) error {
	pr, pw := io.Pipe()
	go func() {
		tw := tar.NewWriter(pw)
		var werr error
		lastDir := ""
		for _, it := range items {
			if appCtx.Err() != nil {
				werr = errClosed
				break
			}
			if prog != nil {
				if d := path.Dir(it.Rel); d != lastDir {
					lastDir = d
					if d == "." {
						d = ""
					}
					prog.setFolder(d)
				}
			}
			hdr := &tar.Header{Name: it.Rel, Mode: 0o664, Size: it.Size, ModTime: time.Now(), Format: tar.FormatPAX}
			if err := tw.WriteHeader(hdr); err != nil {
				werr = err
				break
			}
			if it.Data != nil {
				if _, err := tw.Write(it.Data); err != nil {
					werr = err
					break
				}
			} else {
				f, err := os.Open(it.Abs)
				if err != nil {
					werr = err
					break
				}
				n, err := io.CopyN(tw, f, it.Size)
				f.Close()
				if prog != nil {
					prog.addBytes(n)
				}
				if err != nil {
					if err == io.EOF {
						err = fmt.Errorf("%s is shorter than expected", it.Rel)
					}
					werr = err
					break
				}
			}
			if prog != nil {
				if it.Data != nil {
					prog.addBytes(it.Size)
				}
				prog.addFiles(1)
			}
		}
		if werr == nil {
			werr = tw.Close()
		}
		pw.CloseWithError(werr)
	}()
	q := shQuote(dir)
	remote := "umask 002 && mkdir -p " + q + " && tar -x --no-same-owner --no-same-permissions -C " + q + " -f - && chmod -R ug+rwX " + q
	_, err := l.ssh.run(timeout, remote, pr, nil)
	pr.CloseWithError(errors.New("upload ended"))
	return err
}

// writeFile puts text into a file on the Frame.
func (l *frameLibrary) writeFile(p, text string, exec bool) error {
	mode := "644"
	if exec {
		mode = "755"
	}
	q := shQuote(p)
	_, err := l.ssh.run(60*time.Second, "mkdir -p "+shQuote(path.Dir(p))+" && cat > "+q+" && chmod "+mode+" "+q,
		strings.NewReader(text), nil)
	return err
}

func (l *frameLibrary) listRemote(dir string) (map[string]int64, error) {
	q := shQuote(dir)
	out, err := l.ssh.sh(10*time.Minute, "if [ -d "+q+" ]; then cd "+q+" && find . -type f -printf '%s|%P\\n'; fi; echo "+listEndMarker)
	if err != nil {
		return nil, err
	}
	files, complete := parseRemoteList(out)
	if !complete {
		return nil, errors.New("the file list from the headset was cut short")
	}
	return files, nil
}

// ------------------------------------------------------------ the app itself

func sha256File(p string) (string, int64, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", 0, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return "", 0, err
	}
	return hex.EncodeToString(h.Sum(nil)), n, nil
}

// launchScript is what Steam starts: Lepton with the game's folders.
func (l *frameLibrary) launchScript() string {
	appid := l.appid
	return "#!/bin/bash\n" +
		"# Starts " + productName + " in Lepton (Valve's Android layer on the Steam Frame).\n" +
		"# Written by " + launcherName + " " + version + "; run it again to update the game.\n" +
		"set -u\n" +
		"app_dir=" + shQuote(l.appDir) + "\n" +
		"lepton=" + shQuote(l.lepton) + "\n" +
		"pkg=" + appPackage + "\n" +
		"[ -f \"$app_dir/lepton-app/game.apk\" ] || { echo \"The app is missing at $app_dir\" >&2; exit 1; }\n" +
		"# Android inside Lepton cannot create the app's own folders: make them here.\n" +
		"d=\"$app_dir/lepton-data/external/Android/data/$pkg\"\n" +
		"mkdir -p \"$d/files\" \"$d/cache\" \"$app_dir/lepton-data/internal\" \"$app_dir/lepton-shaders\"\n" +
		"chmod -R ug+rwX \"$app_dir/lepton-data\" 2>/dev/null || true\n" +
		"export SteamAppId=" + strconv.FormatUint(uint64(appid), 10) + "\n" +
		"export STEAM_COMPAT_INSTALL_PATH=\"$app_dir/lepton-app\"\n" +
		"export STEAM_COMPAT_DATA_PATH=\"$app_dir/lepton-data\"\n" +
		"export STEAM_COMPAT_SHADER_PATH=\"$app_dir/lepton-shaders\"\n" +
		"export STEAM_COMPAT_LIBRARY_PATHS=\"$app_dir\"\n" +
		"export XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\"\n" +
		"export IS_PARENT=true\n" +
		"child=\n" +
		"stop() { trap - EXIT INT TERM; [ -n \"$child\" ] && kill -TERM -- \"-$child\" 2>/dev/null; podman kill \"lepton-steamlaunch-$SteamAppId\" >/dev/null 2>&1; }\n" +
		"trap stop EXIT; trap 'exit 130' INT; trap 'exit 143' TERM\n" +
		"setsid \"$lepton\" start >\"$app_dir/launch.log\" 2>&1 &\n" +
		"child=$!\n" +
		"wait $child\n"
}

// frameInstall is the "Installing the app" step: the APK, the start script
// and the Steam Library entry.
func (a *app) frameInstall() error {
	l := a.lib
	sha, size, err := sha256File(a.apkPath)
	if err != nil {
		return fail("Could not read %s: %v", a.apkPath, err)
	}
	if l.apkSize == size && l.apkSHA == sha {
		say("The app on the Steam Frame is already version %s.", a.apkVersion)
	} else {
		if l.availKB > 0 && size/1024 > l.availKB-200*1024 {
			return fail("The Steam Frame has only %s free; the app needs %s.", humanSize(l.availKB*1024), humanSize(size))
		}
		say("Copying the app (%s, %s) to the Steam Frame...", a.apkVersion, humanSize(size))
		items := []tarItem{{Rel: "lepton-app/game.apk", Abs: a.apkPath, Size: size}}
		prog := newCopyProgress("Copying the app", []gameFile{{Rel: items[0].Rel, Abs: a.apkPath, Size: size}})
		err := l.uploadTar(l.appDir, items, prog, 30*time.Minute)
		prog.finish()
		if err != nil {
			if err == errClosed {
				return err
			}
			return fail("Could not copy the app to the Steam Frame:\n  %v", err)
		}
		l.apkSize, l.apkSHA = size, sha
	}
	if err := l.writeFile(l.appDir+"/launch.sh", l.launchScript(), true); err != nil {
		if err == errClosed {
			return err
		}
		return fail("Could not write the start script on the Steam Frame:\n  %v", err)
	}
	// The data folders now, so the game's first start finds its folder.
	if _, err := l.ssh.sh(60*time.Second, "umask 002 && mkdir -p "+shQuote(l.gameDir())+" "+shQuote(l.appDir+"/lepton-shaders")+" "+shQuote(l.appDir+"/lepton-data/internal")); err != nil && err == errClosed {
		return err
	}
	return a.frameRegister()
}

// frameRegister adds the game to the Steam Library (or finds the entry from
// an earlier run) and writes the start script with the Library id.
func (a *app) frameRegister() error {
	l := a.lib
	if appid := l.findShortcut(); appid != 0 {
		l.appid = appid
		logf("the game is in the Steam Library already (appid %d)", appid)
	} else {
		if !l.steamRunning {
			say("WARNING: Steam is not running on the headset, so the game could not be added\n" +
				"to the Steam Library. Run the launcher again with the headset awake.")
			return nil
		}
		say("Adding the game to the Steam Library on the headset...")
		if err := l.devkitRegister(); err != nil {
			if err == errClosed {
				return err
			}
			return fail("Steam on the headset did not add the game to the Library:\n  %v\n\nPut the headset on (Steam must be running), then try again.", err)
		}
		for i := 0; i < 20 && l.appid == 0; i++ {
			time.Sleep(500 * time.Millisecond)
			l.appid = l.findShortcut()
		}
		if l.appid == 0 {
			say("WARNING: Steam added the game but has not saved the entry yet. It should be in\n" +
				"the Library; if it is not after a restart of the headset, run the launcher again.")
		} else {
			say("The game is in the Steam Library (as \"Devkit Game: %s\"; rename it in Steam if\nyou like).", frameGameID)
		}
	}
	if l.appid != 0 {
		if err := l.writeFile(l.appDir+"/launch.sh", l.launchScript(), true); err != nil && err == errClosed {
			return err
		}
		a.frameGridArt()
	}
	return nil
}

// devkitRegister adds the game through Steam's devkit interface: the game
// folder under ~/devkit-game, then a create-shortcut request on Steam's
// command pipe, answered through a response file.
func (l *frameLibrary) devkitRegister() error {
	script := `set -e
H=$HOME; G=` + frameGameID + `; D="$H/devkit-game"
mkdir -p "$D/$G"
ln -sfn ` + shQuote(l.appDir+"/launch.sh") + ` "$D/$G/launch.sh"
printf '["launch.sh"]' > "$D/$G-argv.json"
printf '{"steam_play": "0", "compat_tool": ""}' > "$D/$G-settings.json"
tok=$(cat "$H/.steam/steam.token")
R=$(mktemp -d /tmp/xwa-devkit.XXXXXX); resp="$R/response"
enc() { printf '%s' "$1" | sed 's|/|%2F|g'; }
pipe=$(readlink -f "$H/.steam/steam.pipe")
printf 'devkit-1 steam://devkit-1/%s/create-shortcut?response=%s&gameid=%s&directory=%s\n' "$tok" "$(enc "$resp")" "$G" "$(enc "$D")" > "$pipe"
i=0
while [ $i -lt 80 ]; do
  sleep 0.25; i=$((i+1))
  if [ -f "$resp.error" ]; then echo "ERROR $(cat "$resp.error")"; rm -rf "$R"; exit 0; fi
  if [ -f "$resp" ] && [ ! -f "$resp.lock" ]; then echo "OK $(cat "$resp")"; rm -rf "$R"; exit 0; fi
done
echo TIMEOUT; rm -rf "$R"`
	out, err := l.ssh.sh(2*time.Minute, script)
	if err != nil {
		return err
	}
	out = strings.TrimSpace(out)
	logf("devkit create-shortcut: %s", out)
	switch {
	case strings.HasPrefix(out, "OK"):
		return nil
	case strings.HasPrefix(out, "ERROR"):
		return errors.New(strings.TrimSpace(strings.TrimPrefix(out, "ERROR")))
	}
	return errors.New("Steam did not answer the request")
}

// findShortcut looks for the game's devkit entry in every account's
// shortcuts.vdf and returns its appid (0 = none).
func (l *frameLibrary) findShortcut() uint32 {
	for _, u := range l.users {
		var buf bytes.Buffer
		p := l.steam + "/userdata/" + u + "/config/shortcuts.vdf"
		if _, err := l.ssh.run(60*time.Second, "cat "+shQuote(p)+" 2>/dev/null || true", nil, &buf); err != nil {
			continue
		}
		if buf.Len() == 0 {
			continue
		}
		root, err := vdfDecode(buf.Bytes())
		if err != nil {
			logf("shortcuts.vdf of account %s: %v", u, err)
			continue
		}
		if appid := vdfFindDevkit(root, frameGameID); appid != 0 {
			return appid
		}
	}
	return 0
}

// frameGridArt gives the Library entry pictures from the APK: its box art
// (assets/vr/boxart.png) as the portrait, its splash as the landscape and
// hero images, its icon. Steam shows them after its next Library refresh.
func (a *app) frameGridArt() {
	l := a.lib
	zr, err := zip.OpenReader(a.apkPath)
	if err != nil {
		return
	}
	defer zr.Close()
	read := func(name string) []byte {
		for _, f := range zr.File {
			if f.Name == name {
				rc, err := f.Open()
				if err != nil {
					return nil
				}
				defer rc.Close()
				b, _ := io.ReadAll(rc)
				return b
			}
		}
		return nil
	}
	splash := read("assets/vr_splash.png")
	icon := read("res/mipmap-xxxhdpi/ic_launcher.png")
	var items []tarItem
	id := strconv.FormatUint(uint64(l.appid), 10)
	if splash != nil {
		items = append(items, tarItem{Rel: id + ".png", Data: splash, Size: int64(len(splash))},
			tarItem{Rel: id + "_hero.png", Data: splash, Size: int64(len(splash))})
	}
	portrait := read("assets/vr/boxart.png") // the box art; older APKs: a tile with the icon
	if portrait == nil {
		portrait = portraitTile(icon)
	}
	if portrait != nil {
		items = append(items, tarItem{Rel: id + "p.png", Data: portrait, Size: int64(len(portrait))})
	}
	if icon != nil {
		items = append(items, tarItem{Rel: id + "_icon.png", Data: icon, Size: int64(len(icon))})
	}
	if len(items) == 0 {
		return
	}
	for _, u := range l.users {
		if err := l.uploadTar(l.steam+"/userdata/"+u+"/config/grid", items, nil, 2*time.Minute); err != nil {
			logf("grid art for account %s: %v", u, err)
		}
	}
}

// portraitTile is a 600x900 PNG: the icon, doubled, on a dark tile.
func portraitTile(iconPNG []byte) []byte {
	if iconPNG == nil {
		return nil
	}
	icon, err := png.Decode(bytes.NewReader(iconPNG))
	if err != nil {
		return nil
	}
	const w, h = 600, 900
	out := image.NewNRGBA(image.Rect(0, 0, w, h))
	draw.Draw(out, out.Bounds(), image.NewUniform(color.NRGBA{10, 14, 24, 255}), image.Point{}, draw.Src)
	b := icon.Bounds()
	scale := 2
	for (b.Dx()*scale > w-80 || b.Dy()*scale > h-80) && scale > 1 {
		scale--
	}
	ox := (w - b.Dx()*scale) / 2
	oy := (h - b.Dy()*scale) / 2
	for y := 0; y < b.Dy()*scale; y++ {
		for x := 0; x < b.Dx()*scale; x++ {
			c := icon.At(b.Min.X+x/scale, b.Min.Y+y/scale)
			r, g, bb, al := c.RGBA()
			if al == 0 {
				continue
			}
			out.Set(ox+x, oy+y, color.NRGBA64{uint16(r), uint16(g), uint16(bb), uint16(al)})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, out); err != nil {
		return nil
	}
	return buf.Bytes()
}

// ------------------------------------------------------- binary VDF reading

// Steam's shortcuts.vdf: a binary KeyValues file. Types: 0 map, 1 string,
// 2 int32; 8 ends a map. Keys are case-insensitive.
type vdfNode struct {
	kind int // 0 map, 1 string, 2 int
	str  string
	num  uint32
	kids map[string]*vdfNode
}

func vdfDecode(data []byte) (*vdfNode, error) {
	pos := 0
	cstr := func() (string, error) {
		end := bytes.IndexByte(data[pos:], 0)
		if end < 0 {
			return "", errors.New("unterminated string")
		}
		s := string(data[pos : pos+end])
		pos += end + 1
		return s, nil
	}
	var node func() (*vdfNode, error)
	node = func() (*vdfNode, error) {
		n := &vdfNode{kids: map[string]*vdfNode{}}
		for {
			if pos >= len(data) {
				return nil, errors.New("truncated")
			}
			kind := int(data[pos])
			pos++
			if kind == 8 {
				return n, nil
			}
			key, err := cstr()
			if err != nil {
				return nil, err
			}
			switch kind {
			case 0:
				v, err := node()
				if err != nil {
					return nil, err
				}
				n.kids[strings.ToLower(key)] = v
			case 1:
				s, err := cstr()
				if err != nil {
					return nil, err
				}
				n.kids[strings.ToLower(key)] = &vdfNode{kind: 1, str: s}
			case 2:
				if pos+4 > len(data) {
					return nil, errors.New("truncated int")
				}
				n.kids[strings.ToLower(key)] = &vdfNode{kind: 2, num: binary.LittleEndian.Uint32(data[pos:])}
				pos += 4
			default:
				return nil, fmt.Errorf("value type %d not supported", kind)
			}
		}
	}
	return node()
}

// vdfFindDevkit returns the appid of the shortcut whose DevkitGameID is
// gameid (0 when there is none).
func vdfFindDevkit(root *vdfNode, gameid string) uint32 {
	sc := root.kids["shortcuts"]
	if sc == nil {
		return 0
	}
	for _, e := range sc.kids {
		if e.kind != 0 {
			continue
		}
		gid := e.kids["devkitgameid"]
		id := e.kids["appid"]
		if gid != nil && gid.kind == 1 && gid.str == gameid && id != nil && id.kind == 2 {
			return id.num
		}
	}
	return 0
}

// ------------------------------------------------------------ the game files

// frameCopyGame copies the game files that are missing or differ in size.
func (a *app) frameCopyGame() error {
	l := a.lib
	say("Checking the game folder...")
	files, skipped, err := walkGame(a.gameDir)
	if err != nil {
		return fail("Could not read the game folder:\n  %v", err)
	}
	logf("%d game files, %d skipped", len(files), skipped)
	say("%d game files (%s).", len(files), humanSize(totalSize(files)))
	for round := 1; round <= 3; round++ {
		say("Checking what is already on the headset...")
		remote, err := l.listRemote(l.gameDir())
		if err != nil {
			if err == errClosed {
				return err
			}
			return fail("Could not read the files on the headset:\n  %v", err)
		}
		todo := filesToCopy(files, remote)
		if len(todo) == 0 {
			say("All %d game files are on the headset.", len(files))
			break
		}
		if round > 1 {
			say("%s did not arrive correctly. Copying again (try %d of 3)...", plural(len(todo), "file"), round)
		} else if len(todo) < len(files) {
			say("%d of %d files are there already; copying the other %s (%s).", len(files)-len(todo), len(files),
				plural(len(todo), "file"), humanSize(totalSize(todo)))
		}
		need := totalSize(todo)
		if l.availKB > 0 && need/1024 > l.availKB-100*1024 {
			return fail("The Steam Frame has only %s free; the game files need %s.", humanSize(l.availKB*1024), humanSize(need))
		}
		prog := newCopyProgress("Copying the game files", todo)
		err = l.uploadTar(l.gameDir(), tarItemsOf(todo), prog, 4*time.Hour)
		prog.finish()
		if err != nil {
			if err == errClosed {
				return err
			}
			if round == 3 {
				return fail("Could not copy the game files to the Steam Frame:\n  %v\n\nCheck the Wi-Fi connection and run the launcher again; it only copies what\nis missing.", err)
			}
			logf("upload round %d failed: %v", round, err)
		}
		if round == 3 {
			remote, err := l.listRemote(l.gameDir())
			if err == nil {
				if bad := filesToCopy(files, remote); len(bad) > 0 {
					return fail("%s could not be copied to the Steam Frame (for example %s).\nRun the launcher again; it only copies what is missing.", plural(len(bad), "file"), bad[0].Rel)
				}
			}
		}
	}
	text := manifestText(files)
	if err := l.uploadTar(l.gameDir(), []tarItem{{Rel: manifestName, Data: []byte(text), Size: int64(len(text))}}, nil, 2*time.Minute); err != nil {
		if err == errClosed {
			return err
		}
		logf("install file list: %v", err)
	}
	return nil
}

// frameCopyPilots offers to bring the PC pilots over.
func (a *app) frameCopyPilots() error {
	l := a.lib
	files := pilotFiles(a.gameDir)
	if len(files) == 0 {
		say("No pilot files in the game folder. Nothing to do.")
		return nil
	}
	logf("pilots: %v", pilotNames(files))
	ans := ui.Ask(Question{
		Kind:  "buttons",
		Title: "Copy your pilots?",
		Text: "Pilots found in your game folder: " + strings.Join(pilotNames(files), ", ") + ".\n" +
			"The VR game keeps its own pilots. Copying brings your PC pilots over; they\n" +
			"are imported the next time the game starts on the headset.",
		Options: []Option{
			{ID: "yes", Label: "Copy pilots", Key: "y"},
			{ID: "no", Label: "Don't copy", Key: "n", Style: "primary"},
		},
		Default:       "no",
		ConsolePrompt: "Copy these pilots to the headset? (y/N)",
	})
	if ans.ID != "yes" {
		say("Pilots are not copied.")
		return nil
	}
	prog := newCopyProgress("Copying pilots", files)
	err := l.uploadTar(l.pilotsDir(), tarItemsOf(files), prog, 10*time.Minute)
	prog.finish()
	if err != nil {
		if err == errClosed {
			return err
		}
		say("WARNING: the pilots could not be copied (%v). The game files are copied anyway.", err)
		return nil
	}
	say("Pilots copied. The game brings them in the next time it starts.")
	return nil
}

// --------------------------------------------------------- start and logs

// frameStartGame starts the game through Steam on the headset.
func (a *app) frameStartGame() {
	l := a.lib
	if l.appid == 0 {
		say("The game has no Library id yet: start it from the Steam Library on the headset.")
		return
	}
	_, err := l.ssh.sh(30*time.Second, "steam -ifrunning steam://rungameid/"+strconv.FormatUint(l.runGameID(), 10)+" >/dev/null 2>&1 || true")
	if err != nil && err != errClosed {
		say("Could not ask Steam to start the game: %v", err)
		return
	}
	say("%s is starting on the headset through Steam. Put it on!", productName)
}

// frameCollectLogs pulls the game's logs, settings and Lepton's launch log
// into dir. Returns how many files arrived.
func (a *app) frameCollectLogs(dir string) int {
	l := a.lib
	remote := "tar -c --ignore-failed-read -f - -C " + shQuote(l.filesDir()) +
		" openxwa.log openxwa-prev.log openxwa-crash.txt openxwa-quest.cfg -C " + shQuote(l.appDir) + " launch.log 2>/dev/null; true"
	var buf bytes.Buffer
	if _, err := l.ssh.run(5*time.Minute, remote, nil, &buf); err != nil {
		logf("collecting logs over SSH: %v", err)
	}
	got := 0
	tr := tar.NewReader(&buf)
	for {
		hdr, err := tr.Next()
		if err != nil {
			break
		}
		name := filepath.Base(hdr.Name)
		if hdr.Typeflag != tar.TypeReg || name == "" || name == "." {
			continue
		}
		b, _ := io.ReadAll(tr)
		if err := os.WriteFile(filepath.Join(dir, name), b, 0o644); err == nil {
			got++
		}
	}
	return got
}
