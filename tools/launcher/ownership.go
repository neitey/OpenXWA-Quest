package main

import (
	"fmt"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

// The game folder on the headset must belong to the OpenXWA app. A folder
// made over USB (adb runs as the "shell" user) before OpenXWA ever started
// belongs to shell, and the app cannot read it or anything copied into it.
// So the launcher never creates the XWA folder itself: the app does, the
// first time it starts.

const shellUID = 2000

// owner is a file's owner as "stat -c" shows it.
type owner struct {
	name string
	uid  int // -1 if unknown
}

func (o owner) String() string {
	if o.uid < 0 {
		return o.name
	}
	return fmt.Sprintf("%s (uid %d)", o.name, o.uid)
}

// parseOwners reads "uid|name|path" lines from stat -c '%u|%U|%n' for the
// asked paths. stat prints the paths as given; if the names do not match
// but every path got a line, the lines are matched by order instead.
func parseOwners(out string, paths []string) map[string]owner {
	m := map[string]owner{}
	var order []owner
	for _, line := range strings.Split(out, "\n") {
		parts := strings.SplitN(strings.TrimRight(line, "\r"), "|", 3)
		if len(parts) != 3 {
			continue
		}
		uid, err := strconv.Atoi(strings.TrimSpace(parts[0]))
		if err != nil {
			continue
		}
		o := owner{name: strings.TrimSpace(parts[1]), uid: uid}
		m[parts[2]] = o
		order = append(order, o)
	}
	matched := 0
	for _, p := range paths {
		if _, ok := m[p]; ok {
			matched++
		}
	}
	if matched < len(paths) && len(order) == len(paths) {
		m = map[string]owner{}
		for i, p := range paths {
			m[p] = order[i]
		}
	}
	return m
}

// sameOwner compares by uid when both are known, else by name.
func sameOwner(a, b owner) bool {
	if a.uid >= 0 && b.uid >= 0 {
		return a.uid == b.uid
	}
	return a.name != "" && a.name == b.name
}

// foreign reports an owner the app cannot use. A folder the app made itself
// shows the owner its files get on this headset: on the Steam Frame (Lepton)
// that is root, not the app's uid, so whatever matches it is the app's.
func (a *app) foreign(o owner) bool {
	if a.appOwner != nil && sameOwner(o, *a.appOwner) {
		return false
	}
	return foreignOwner(o, a.appUID())
}

// learnAppOwner lets the app's helper make an empty folder and notes its
// owner (see foreign). Without the helper it is learnt from the XWA folder
// once the app has made it (waitForAppFolder).
func (a *app) learnAppOwner() {
	if a.appOwner != nil || !a.helper {
		return
	}
	probe := a.remoteBase() + "/" + ownerProbe
	if err := a.helperMkdirs(ownerProbe); err != nil {
		logf("owner probe: %v", err)
		return
	}
	m := a.remoteOwners(probe)
	a.adb.shell(30*time.Second, "rmdir "+shQuote(probe))
	if o, ok := m[probe]; ok {
		a.noteAppOwner(o, "a folder the app's helper made")
	}
}

const ownerProbe = ".openxwa-owner"

func (a *app) noteAppOwner(o owner, from string) {
	a.appOwner = &o
	logf("%s belongs to %v; files with that owner are the app's", from, o)
	if !sameOwner(o, owner{uid: a.appUID()}) {
		logf("note: that is not the app's uid (%d)", a.uid)
	}
}

var appUserRe = regexp.MustCompile(`^u\d+_a\d+$`)

// foreignOwner reports an owner the app cannot read as: the shell user, or a
// different uid than the app's own (when that is known).
func foreignOwner(o owner, appUID int) bool {
	if o.name == "shell" || o.uid == shellUID {
		return true
	}
	if appUID > 0 && o.uid >= 0 {
		return o.uid != appUID
	}
	if o.uid >= 0 && o.uid < 10000 {
		return !appUserRe.MatchString(o.name)
	}
	return false
}

// Android 13 and older print "userId=", Android 14 "appId=".
var userIDRe = regexp.MustCompile(`(?m)^\s*(?:userId|appId)=(\d+)`)

// parseAppUID reads the app's uid from dumpsys package output.
func parseAppUID(dumpsys string) int {
	i := strings.Index(dumpsys, "Package ["+appPackage+"]")
	if i < 0 {
		return 0
	}
	if m := userIDRe.FindStringSubmatch(dumpsys[i:]); m != nil {
		n, _ := strconv.Atoi(m[len(m)-1])
		return n
	}
	return 0
}

// appUID returns the app's Linux uid (0 if unknown), asked once.
func (a *app) appUID() int {
	if a.uid == 0 {
		out, _ := a.adb.shell(60*time.Second, "dumpsys package "+appPackage)
		a.uid = parseAppUID(out)
		logf("OpenXWA app uid: %d", a.uid)
	}
	return a.uid
}

// remoteOwners stats the given headset paths (missing ones are left out).
func (a *app) remoteOwners(paths ...string) map[string]owner {
	var q []string
	for _, p := range paths {
		q = append(q, shQuote(p))
	}
	out, _ := a.adb.shell(60*time.Second, "stat -c '%u|%U|%n' "+strings.Join(q, " ")+" 2>/dev/null")
	m := parseOwners(out, paths)
	if len(m) == 0 && len(paths) > 0 {
		// No stat -c: fall back to ls -ld (owner name only).
		for _, p := range paths {
			o, _ := a.adb.shell(30*time.Second, "ls -ld "+shQuote(p))
			if f := strings.Fields(firstLine(o)); len(f) >= 3 && strings.HasPrefix(f[0], "d") || len(f) >= 3 && strings.HasPrefix(f[0], "-") {
				m[p] = owner{name: f[2], uid: -1}
			}
		}
	}
	return m
}

// prepareAppFolder is step 5: make sure the app's own XWA folder exists and
// belongs to the app. Folders made over USB in earlier runs are removed and
// the app is started to make them again.
func (a *app) prepareAppFolder() error {
	fixed := false
	a.tryHelper() // also makes XWA when it has the helper
	a.learnAppOwner()
	for {
		if appCtx.Err() != nil {
			return errClosed
		}
		base, game := a.remoteBase(), a.remoteGame()
		if !a.remoteHasEntry(base, "XWA") {
			if a.helper {
				if err := a.helperMkdirs("XWA"); err != nil {
					return fail("The app could not create its game folder on the headset:\n  %v", err)
				}
				if !a.remoteHasEntry(base, "XWA") {
					return fail("The app created its game folder, but the launcher cannot see it.\n" +
						"Please collect the logs and report this, naming your headset model.")
				}
				continue
			}
			if err := a.waitForAppFolder(); err != nil {
				return err
			}
			continue
		}
		owners := a.remoteOwners(base, game, a.remotePilots())
		logf("owners: %v", owners)
		o, known := owners[game]
		if known && a.foreign(o) {
			if fixed {
				return fail("The game folder on the headset was made again, but it still belongs to\n"+
					"%s instead of the app. Please collect the logs and report this,\n"+
					"naming your headset model.", o)
			}
			if err := a.fixForeignFolder(o, owners); err != nil {
				return err
			}
			fixed = true
			continue
		}
		if !known {
			logf("could not read the owner of %s; continuing", game)
		}
		if p, ok := owners[a.remotePilots()]; ok && a.foreign(p) {
			logf("pilot-import belongs to %v; removing it", p)
			a.adb.shell(60*time.Second, "rm -rf "+shQuote(a.remotePilots()))
		}
		break
	}
	a.cleanupUnreadableCopies()
	if a.helper {
		if err := a.removeShellLeftovers(); err != nil {
			return err
		}
	} else if err := a.probeCopyOwnership(); err != nil {
		return err
	}
	say("The game folder on the headset is ready.")
	return nil
}

// fixForeignFolder explains the problem, then removes the unusable folders.
func (a *app) fixForeignFolder(o owner, owners map[string]owner) error {
	logf("XWA belongs to %v, not to the app (uid %d)", o, a.uid)
	ans := ui.Ask(Question{
		Kind:  "buttons",
		Title: "The game folder on the headset cannot be used",
		Text: "The XWA folder on the headset was created over USB before the app ever\n" +
			"started, so the game cannot read it (it belongs to " + o.name + ").\n\n" +
			"It will be removed and made again by the app, then the game files are\n" +
			"copied again. Your pilots and settings in the game are not touched.",
		Options: []Option{
			{ID: "yes", Label: "Remove and make it again", Key: "y", Style: "primary"},
			{ID: "no", Label: "Cancel", Key: "n"},
		},
		Default:       "yes",
		ConsolePrompt: "Remove the folder and make it again? (Y/n)",
	})
	if ans.Closed && appCtx.Err() != nil {
		return errClosed
	}
	if ans.ID != "yes" {
		return fail("The XWA folder on the headset cannot be read by the app, so the game\n" +
			"files cannot be used. Try again and choose \"Remove and make it again\".")
	}
	say("Removing the unusable game folder from the headset...")
	if out, err := a.adb.shell(10*time.Minute, "rm -rf "+shQuote(a.remoteGame())); err != nil {
		if err == errClosed {
			return err
		}
		return fail("Could not remove the old folder on the headset:\n  %s", strings.TrimSpace(out))
	}
	if p, ok := owners[a.remotePilots()]; ok && a.foreign(p) {
		a.adb.shell(60*time.Second, "rm -rf "+shQuote(a.remotePilots()))
	}
	return nil
}

// waitForAppFolder starts OpenXWA and waits (as long as it takes) until it
// has made its XWA folder. Horizon OS only really starts the app while the
// headset is worn, so the user is asked to put it on.
func (a *app) waitForAppFolder() error {
	say("%s has to start once on the headset to create its own\n"+
		"folder. Put the headset on now; it starts by itself.", productName)
	q := Question{
		Kind:  "buttons",
		Title: "Put the headset on",
		Text: productName + " has to start once on the headset to create its own\n" +
			"folder. Put the headset on; it starts by itself. If it does not, open it from\n" +
			"Library -> Unknown sources. The launcher continues as soon as it has started.",
		Options: []Option{
			{ID: "again", Label: "Try again", Style: "primary"},
			{ID: "cancel", Label: "Cancel"},
		},
	}
	if a.lepton != "" {
		q.Title = "Keep Lepton Development running"
		q.Text = "The game has to start once to create its own folder. It starts in the\n" +
			"\"Lepton Development\" window on the headset; keep that window open (put the\n" +
			"headset on if needed). The launcher continues as soon as it has started."
	}
	offer := ui.Offer(q)
	defer ui.ClearOffer()
	var lastStart time.Time
	started := time.Now()
	for {
		if time.Since(lastStart) >= 15*time.Second {
			if !lastStart.IsZero() {
				if _, console := ui.(*consoleUI); console {
					say("Still waiting for the game to start on the headset (put it on; Ctrl+C to stop)...")
				}
			}
			a.adb.shell(30*time.Second, "am start -n "+appActivity)
			lastStart = time.Now()
		}
		select {
		case ans := <-offer:
			if ans.ID == "cancel" {
				return fail("The game has not started on the headset yet, so its folder does not exist.\n" +
					"Put the headset on, then choose Try again.")
			}
			lastStart = time.Time{} // start it again right now
			offer = ui.Offer(q)
		case <-time.After(time.Second):
		case <-appCtx.Done():
			return errClosed
		}
		if a.remoteHasEntry(a.remoteBase(), "XWA") {
			break
		}
	}
	logf("the app created its folder after %.0f s", time.Since(started).Seconds())
	sleepCtx(2 * time.Second) // let it finish starting before it is stopped
	a.adb.shell(30*time.Second, "am force-stop "+appPackage)
	say("The game created its folder. It was closed again so the files can be copied;\n" +
		"you can take the headset off.")
	return nil
}

// cleanupUnreadableCopies offers to delete XWA-unreadable-* folders: newer
// OpenXWA versions move a folder they cannot read aside under that name.
func (a *app) cleanupUnreadableCopies() {
	out, err := a.adb.shell(30*time.Second, "ls "+shQuote(a.remoteBase()))
	if err != nil {
		return
	}
	var dirs []string
	for _, l := range strings.Split(out, "\n") {
		if l = strings.TrimSpace(l); strings.HasPrefix(l, "XWA-unreadable-") {
			dirs = append(dirs, a.remoteBase()+"/"+l)
		}
	}
	if len(dirs) == 0 {
		return
	}
	var q []string
	for _, d := range dirs {
		q = append(q, shQuote(d))
	}
	size := ""
	if du, err := a.adb.shell(5*time.Minute, "du -sk "+strings.Join(q, " ")); err == nil {
		var kb int64
		for _, l := range strings.Split(du, "\n") {
			if f := strings.Fields(l); len(f) >= 1 {
				n, _ := strconv.ParseInt(f[0], 10, 64)
				kb += n
			}
		}
		size = " (" + humanSize(kb*1024) + ")"
	}
	logf("unreadable old copies: %q%s", dirs, size)
	copies := "1 old copy"
	if len(dirs) > 1 {
		copies = fmt.Sprintf("%d old copies", len(dirs))
	}
	if !askYesNo("Old unreadable copies found",
		fmt.Sprintf("The headset has %s of the game folder that the game could not read%s.\nDelete to free the space?",
			copies, size), true) {
		return
	}
	if _, err := a.adb.shell(10*time.Minute, "rm -rf "+strings.Join(q, " ")); err == nil {
		say("Deleted the old copies.")
	}
}

// probeCopyOwnership checks what owner new folders get on this headset:
// made with mkdir, and made by adb push. Copying uses the way that gives the
// app's ownership; if neither does, copying would be useless, so stop.
func (a *app) probeCopyOwnership() error {
	probe := a.remoteGame() + "/.openxwa-probe"
	rm := "rm -rf " + shQuote(probe)
	a.adb.shell(30*time.Second, rm)
	defer a.adb.shell(30*time.Second, rm)

	a.adb.shell(30*time.Second, "mkdir "+shQuote(probe))
	m := a.remoteOwners(probe)
	o, ok := m[probe]
	a.mkdirOK = ok && !a.foreign(o)
	logf("probe: a folder made with mkdir belongs to %v (usable: %v)", o, a.mkdirOK)
	a.adb.shell(30*time.Second, rm)

	tmp, err := os.CreateTemp("", "openxwa-probe-*.txt")
	if err != nil {
		return nil
	}
	tmp.WriteString("probe\n")
	tmp.Close()
	defer os.Remove(tmp.Name())
	dir := probe + "/sub"
	a.adb.run(60*time.Second, "push", tmp.Name(), dir+"/probe.txt")
	m = a.remoteOwners(dir, dir+"/probe.txt")
	d, okD := m[dir]
	f, okF := m[dir+"/probe.txt"]
	pushOK := okD && okF && !a.foreign(d) && !a.foreign(f)
	logf("probe: adb push made a folder owned by %v and a file owned by %v (usable: %v)", d, f, pushOK)
	if !okD || !okF {
		logf("probe: could not read the owners; assuming copying works")
		return nil
	}
	if !pushOK {
		return fail("This headset gives files copied over USB to the wrong owner (%s), so\n"+
			"the game could not read them. Nothing was copied. Please collect the logs and\n"+
			"report this, naming your headset model.", f)
	}
	return nil
}

// finalOwnershipCheck runs before "All done": the game folder, RESDATA.TXT,
// each top-level folder and one deep file must belong to the app. bad lists
// the wrong top-level entries below XWA; xwaBad is set when XWA itself is.
func (a *app) finalOwnershipCheck(files []gameFile) (bad []string, xwaBad bool, err error) {
	ld, _ := a.adb.shell(30*time.Second, "ls -ld "+shQuote(a.remoteBase())+" "+shQuote(a.remoteGame()))
	logf("final check:\n%s", ld)
	paths := []string{a.remoteGame()}
	tops := map[string]bool{}
	deepest := ""
	for _, f := range files {
		if strings.EqualFold(f.Rel, "RESDATA.TXT") {
			paths = append(paths, a.remoteGame()+"/"+f.Rel)
		}
		if i := strings.IndexByte(f.Rel, '/'); i > 0 {
			tops[f.Rel[:i]] = true
		}
		if strings.Count(f.Rel, "/") > strings.Count(deepest, "/") {
			deepest = f.Rel
		}
	}
	var topList []string
	for t := range tops {
		topList = append(topList, t)
	}
	sort.Strings(topList)
	for _, t := range topList {
		paths = append(paths, a.remoteGame()+"/"+t)
	}
	if deepest != "" {
		paths = append(paths, a.remoteGame()+"/"+deepest)
	}
	owners := a.remoteOwners(paths...)
	var msgs []string
	badTop := map[string]bool{}
	for _, p := range paths {
		o, ok := owners[p]
		if !ok {
			logf("final check: no owner for %s", p)
			continue
		}
		if !a.foreign(o) {
			continue
		}
		rel := strings.TrimPrefix(p, a.remoteGame()+"/")
		if p == a.remoteGame() {
			xwaBad = true
			rel = "XWA"
		} else {
			top := rel
			if i := strings.IndexByte(rel, '/'); i >= 0 {
				top = rel[:i]
			}
			if !badTop[top] {
				badTop[top] = true
				bad = append(bad, top)
			}
		}
		msgs = append(msgs, rel+" belongs to "+o.String())
	}
	logf("final check: %d paths, %d not owned by the app", len(owners), len(msgs))
	if len(msgs) > 0 {
		sort.Strings(msgs)
		if len(msgs) > 10 {
			msgs = append(msgs[:10], "...")
		}
		e := &userError{retryStep: 5, msg: fmt.Sprintf("The game files were copied, but the game cannot read them because they do\n"+
			"not belong to the app:\n  %s\n\nTry again: the launcher removes the unusable folder and copies the files\n"+
			"again. If this keeps happening, please collect the logs and report it.", strings.Join(msgs, "\n  "))}
		return bad, xwaBad, e
	}
	say("Checked: the game files belong to the app, so the game can read them.")
	return nil, false, nil
}
