package main

import (
	"fmt"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// gameFile is one file of the PC game folder that belongs on the headset.
type gameFile struct {
	Rel  string // relative path with forward slashes, as on the PC
	Abs  string // full local path
	Size int64
}

var (
	skipExact    = map[string]bool{"desktop.ini": true, "thumbs.db": true}
	skipExts     = map[string]bool{".exe": true, ".dll": true, ".plt": true, ".lnk": true, ".ico": true, ".url": true, ".tmp": true}
	skipPrefixes = []string{"goggame-", "unins", "steam_api"}
)

// skipFileName reports files the port does not use (programs, shortcuts,
// store/installer files) and pilots, which are handled separately.
func skipFileName(name string) bool {
	l := strings.ToLower(name)
	if strings.HasPrefix(l, ".") || skipExact[l] || skipExts[path.Ext(l)] {
		return true
	}
	for _, p := range skipPrefixes {
		if strings.HasPrefix(l, p) {
			return true
		}
	}
	return false
}

// walkGame lists the files to copy, sorted by relative path.
func walkGame(root string) (files []gameFile, skipped int, err error) {
	err = filepath.WalkDir(root, func(p string, d fs.DirEntry, werr error) error {
		if werr != nil {
			return werr
		}
		if d.IsDir() {
			if p != root && strings.HasPrefix(d.Name(), ".") {
				return filepath.SkipDir
			}
			return nil
		}
		if !d.Type().IsRegular() {
			skipped++
			return nil
		}
		info, ierr := d.Info()
		if ierr != nil {
			return ierr
		}
		if skipFileName(d.Name()) || isHiddenSystemFile(info) {
			skipped++
			return nil
		}
		rel, rerr := filepath.Rel(root, p)
		if rerr != nil {
			return rerr
		}
		files = append(files, gameFile{Rel: filepath.ToSlash(rel), Abs: p, Size: info.Size()})
		return nil
	})
	sort.Slice(files, func(i, j int) bool { return files[i].Rel < files[j].Rel })
	return files, skipped, err
}

const listEndMarker = "__OPENXWA_LIST_END__"

// remoteListCommand prints "size|./relative/path" for every file below dir,
// then an end marker so a cut-off listing is noticed. A missing dir lists
// nothing.
func remoteListCommand(dir string) string {
	q := shQuote(dir)
	return "if [ -d " + q + " ]; then cd " + q + " && find . -type f -exec stat -c '%s|%n' {} +; fi; echo " + listEndMarker
}

// parseRemoteList turns the listing into lower-case relative path -> size
// (the headset's shared storage ignores case). complete is false when the
// end marker is missing.
func parseRemoteList(out string) (files map[string]int64, complete bool) {
	files = map[string]int64{}
	for _, line := range strings.Split(out, "\n") {
		line = strings.TrimRight(line, "\r")
		if strings.TrimSpace(line) == listEndMarker {
			complete = true
			continue
		}
		i := strings.IndexByte(line, '|')
		if i <= 0 {
			continue
		}
		size, err := strconv.ParseInt(strings.TrimSpace(line[:i]), 10, 64)
		if err != nil {
			continue
		}
		rel := strings.TrimPrefix(line[i+1:], "./")
		if rel == "" {
			continue
		}
		files[strings.ToLower(rel)] = size
	}
	return files, complete
}

// filesToCopy returns the local files that are missing on the headset or
// have a different size there.
func filesToCopy(local []gameFile, remote map[string]int64) []gameFile {
	var out []gameFile
	for _, f := range local {
		if size, ok := remote[strings.ToLower(f.Rel)]; !ok || size != f.Size {
			out = append(out, f)
		}
	}
	return out
}

// mkdirCommands builds "mkdir -p 'a' 'b' ..." commands for the folders that
// the files need, leaving out folders that a deeper one already creates,
// with each command shorter than maxLen characters.
func mkdirCommands(base string, files []gameFile, maxLen int) []string {
	dirs := map[string]bool{}
	for _, f := range files {
		if d := path.Dir(f.Rel); d != "." {
			dirs[d] = true
		}
	}
	parents := map[string]bool{}
	for d := range dirs {
		for p := path.Dir(d); p != "."; p = path.Dir(p) {
			parents[p] = true
		}
	}
	var leaves []string
	for d := range dirs {
		if !parents[d] {
			leaves = append(leaves, d)
		}
	}
	sort.Strings(leaves)
	var cmds []string
	cur := ""
	for _, d := range leaves {
		arg := shQuote(base + "/" + d)
		if cur != "" && len(cur)+1+len(arg) > maxLen {
			cmds = append(cmds, cur)
			cur = ""
		}
		if cur == "" {
			cur = "mkdir -p"
		}
		cur += " " + arg
	}
	if cur != "" {
		cmds = append(cmds, cur)
	}
	return cmds
}

// pushJob is one "adb push" call: a whole top-level folder or one file.
type pushJob struct {
	label  string
	local  string
	remote string
	files  []gameFile // the files this job is expected to bring over
}

const folderPushThreshold = 200

// planPushes groups the files by top-level folder. Folders with many files
// to copy are pushed as a whole (much faster); the rest go file by file.
// Step "verify" catches anything a folder push silently skipped.
func planPushes(root, base string, files []gameFile) []pushJob {
	groups := map[string][]gameFile{}
	var tops []string
	for _, f := range files {
		top := ""
		if i := strings.IndexByte(f.Rel, '/'); i >= 0 {
			top = f.Rel[:i]
		}
		if _, ok := groups[top]; !ok {
			tops = append(tops, top)
		}
		groups[top] = append(groups[top], f)
	}
	sort.Strings(tops) // "" (loose files in the main folder) comes first
	var jobs []pushJob
	for _, top := range tops {
		g := groups[top]
		if top != "" && len(g) > folderPushThreshold {
			jobs = append(jobs, pushJob{
				label:  top,
				local:  filepath.Join(root, filepath.FromSlash(top)) + string(filepath.Separator) + ".",
				remote: base + "/" + top + "/",
				files:  g,
			})
			continue
		}
		for _, f := range g {
			jobs = append(jobs, singleFileJob(base, f))
		}
	}
	return jobs
}

func singleFileJob(base string, f gameFile) pushJob {
	label := path.Dir(f.Rel)
	if label == "." {
		label = "main folder"
	}
	return pushJob{label: label, local: f.Abs, remote: base + "/" + f.Rel, files: []gameFile{f}}
}

func totalSize(files []gameFile) int64 {
	var n int64
	for _, f := range files {
		n += f.Size
	}
	return n
}

// copyProgress feeds the UI's progress display. While a long folder push
// runs, a ticker keeps the display alive (console spinner, busy stripes).
type copyProgress struct {
	mu   sync.Mutex
	p    Progress
	stop chan struct{}
}

func newCopyProgress(verb string, files []gameFile) *copyProgress {
	c := &copyProgress{p: Progress{Label: verb, TotalFiles: len(files), TotalBytes: totalSize(files), Started: time.Now()}, stop: make(chan struct{})}
	ui.Progress(&c.p)
	go func() {
		t := time.NewTicker(250 * time.Millisecond)
		defer t.Stop()
		for {
			select {
			case <-c.stop:
				return
			case <-t.C:
				c.mu.Lock()
				c.p.Spin++
				p := c.p
				c.mu.Unlock()
				ui.Progress(&p)
			}
		}
	}()
	return c
}

// start marks one push as running.
func (c *copyProgress) start(j pushJob) {
	c.mu.Lock()
	c.p.Folder = j.label
	c.p.Busy = len(j.files) > 1
	p := c.p
	c.mu.Unlock()
	ui.Progress(&p)
}

func (c *copyProgress) add(files []gameFile) {
	c.mu.Lock()
	c.p.Files += len(files)
	c.p.Bytes += totalSize(files)
	c.p.Busy = false
	p := c.p
	c.mu.Unlock()
	ui.Progress(&p)
}

// For streamed copies: bytes and files as they are read, the current
// folder, a snapshot to roll back to when a batch is retried.
func (c *copyProgress) addBytes(n int64) {
	c.mu.Lock()
	c.p.Bytes += n
	c.p.Busy = false
	c.mu.Unlock()
}

func (c *copyProgress) addFiles(n int) {
	c.mu.Lock()
	c.p.Files += n
	c.mu.Unlock()
}

func (c *copyProgress) setFolder(label string) {
	c.mu.Lock()
	c.p.Folder = label
	c.mu.Unlock()
}

// setPhase changes the label ("Packing (part 2 of 5)", ...); busy shows the
// busy stripes and hides the folder while one long step runs.
func (c *copyProgress) setPhase(label string, busy bool) {
	c.mu.Lock()
	if label != "" {
		c.p.Label = label
	}
	c.p.Busy = busy
	if busy {
		c.p.Folder = ""
	}
	c.mu.Unlock()
}

func (c *copyProgress) snapshot() Progress {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.p
}

func (c *copyProgress) restore(p Progress) {
	c.mu.Lock()
	c.p.Files, c.p.Bytes = p.Files, p.Bytes
	c.mu.Unlock()
}

func (c *copyProgress) finish() {
	close(c.stop)
	c.mu.Lock()
	c.p.Folder, c.p.Busy = "", false
	p := c.p
	c.mu.Unlock()
	ui.Progress(&p)
	ui.Progress(nil)
}

// listRemote lists the files below a headset folder with their sizes.
func (a *app) listRemote(dir string) (map[string]int64, error) {
	out, err := a.adb.shell(10*time.Minute, remoteListCommand(dir))
	files, complete := parseRemoteList(out)
	if !complete {
		if err == nil {
			err = fmt.Errorf("the file list from the headset was incomplete")
		}
		return nil, err
	}
	return files, nil
}

func (a *app) headsetGone() bool {
	out, err := a.adb.run(20*time.Second, "get-state")
	return err != nil || strings.TrimSpace(out) != "device"
}

var errDisconnected = &userError{msg: "The connection to the headset was lost (USB cable unplugged, or Wi-Fi\n" +
	"dropped or the headset went to sleep). Check the connection and press Try\n" +
	"again; it continues where it stopped and only copies what is missing."}

// runPushes runs the jobs with a progress line.
func (a *app) runPushes(verb string, jobs []pushJob, files []gameFile) error {
	p := newCopyProgress(verb, files)
	for _, j := range jobs {
		p.start(j)
		out, err := a.adb.run(0, "push", j.local, j.remote)
		if err == errClosed {
			p.finish()
			logf("copy stopped during the push of %s (window closed)", j.local)
			return errClosed
		}
		if err != nil || strings.Contains(out, "adb: error") {
			logf("push of %s reported a problem", j.local)
			if a.headsetGone() {
				if !a.waitForHeadset() {
					p.finish()
					return errDisconnected
				}
				// Continue; the check afterwards copies again whatever this
				// push did not bring over.
			}
		}
		p.add(j.files)
	}
	p.finish()
	return nil
}

// freeSpaceKB parses "df -k" output and returns the available KB.
func parseDfAvailable(out string) (int64, bool) {
	lines := strings.Split(strings.TrimSpace(out), "\n")
	if len(lines) < 2 {
		return 0, false
	}
	// Skip the header; the filesystem name may sit on its own line, so look
	// for the first three numbers in a row: size, used, available.
	toks := strings.Fields(strings.Join(lines[1:], " "))
	for i := 0; i+2 < len(toks); i++ {
		var n [3]int64
		ok := true
		for k := 0; k < 3; k++ {
			v, err := strconv.ParseInt(toks[i+k], 10, 64)
			if err != nil {
				ok = false
				break
			}
			n[k] = v
		}
		if ok {
			return n[2], true
		}
	}
	return 0, false
}

// checkFreeSpace warns when the headset may be too full for need bytes.
func (a *app) checkFreeSpace(need int64) error {
	out, err := a.adb.shell(30*time.Second, "df -k "+shQuote(a.remoteBase()))
	if err != nil {
		logf("df failed; skipping the free space check")
		return nil
	}
	availKB, ok := parseDfAvailable(out)
	if !ok {
		logf("could not parse df output; skipping the free space check")
		return nil
	}
	free := availKB * 1024
	const margin = 200 << 20
	logf("headset free space %d bytes, need %d", free, need)
	if free >= need+margin {
		return nil
	}
	logf("WARNING: not enough free space on the headset")
	q := Question{
		Kind:  "buttons",
		Title: "The headset may not have enough free space",
		Text: fmt.Sprintf("Needed: about %s    Free on the headset: %s\n"+
			"Free up space on the headset (for example uninstall games you no longer play).",
			humanSize(need), humanSize(free)),
		Options: []Option{
			{ID: "stop", Label: "Stop", Key: "n", Style: "primary"},
			{ID: "go", Label: "Continue anyway", Key: "y"},
		},
		Default:       "stop",
		ConsolePrompt: "Continue anyway? (y/N)",
	}
	if ui.Ask(q).ID != "go" {
		return fail("Stopped so you can free up space on the headset. Run the launcher again\nafterwards; it only copies what is missing.")
	}
	return nil
}

// copyGame is step 7.
func (a *app) copyGame() error {
	say("Checking the game folder...")
	files, skipped, err := walkGame(a.gameDir)
	if err != nil {
		return fail("Could not read the game folder:\n  %v", err)
	}
	logf("%d game files, %d skipped", len(files), skipped)
	say("%d game files (%s).", len(files), humanSize(totalSize(files)))

	for attempt := 1; ; attempt++ {
		if err := a.copyMissing(files); err != nil {
			return err
		}
		bad, xwaBad, err := a.finalOwnershipCheck(files)
		if err == nil {
			break
		}
		if xwaBad || attempt == 2 {
			return err
		}
		// Some folders below XWA belong to the wrong owner: remove them and
		// copy them again, letting adb push make the folders.
		say("Some folders on the headset belong to the wrong owner; removing them and\ncopying them again.")
		a.mkdirOK = false
		var q []string
		for _, p := range bad {
			q = append(q, shQuote(a.remoteGame()+"/"+p))
		}
		if out, err := a.adb.shell(10*time.Minute, "rm -rf "+strings.Join(q, " ")); err != nil {
			if err == errClosed {
				return err
			}
			return fail("Could not remove the unusable folders:\n  %s", strings.TrimSpace(out))
		}
	}
	return a.writeManifest(files)
}

// copyMissing copies the files that are missing or have the wrong size, then
// verifies them.
func (a *app) copyMissing(files []gameFile) error {
	say("Checking what is already on the headset...")
	remote, err := a.listRemote(a.remoteGame())
	if err != nil && err != errClosed && a.headsetGone() && a.waitForHeadset() {
		remote, err = a.listRemote(a.remoteGame())
	}
	if err != nil {
		if err == errClosed {
			return err
		}
		return fail("Could not read the files on the headset (%v).\nCheck the %s and try again.", err, a.connectionWord())
	}
	todo := filesToCopy(files, remote)
	if len(todo) == 0 {
		say("All game files are already on the headset. Nothing to copy.")
		return nil
	}
	if len(todo) == len(files) {
		say("%s to copy (%s).", plural(len(todo), "file"), humanSize(totalSize(todo)))
	} else {
		say("%s to copy (new or changed, %s).", plural(len(todo), "file"), humanSize(totalSize(todo)))
	}
	if a.helper {
		// The zip of one batch sits on the headset while it is unpacked.
		if err := a.checkFreeSpace(totalSize(todo) + importBatchSize); err != nil {
			return err
		}
		if err := a.importFiles("Copying", itemsOf(todo), "XWA"); err != nil {
			return err
		}
		return a.verifyGame(files)
	}
	if err := a.checkFreeSpace(totalSize(todo)); err != nil {
		return err
	}
	// Create every folder first (pushing into missing folders is where files
	// went missing before), but only where folders made that way belong to
	// the app (see probeCopyOwnership); otherwise adb push makes them.
	if a.mkdirOK {
		for _, c := range mkdirCommands(a.remoteGame(), todo, 4000) {
			if out, err := a.adb.shell(2*time.Minute, c); err != nil {
				return fail("Could not create the folders on the headset:\n  %s", strings.TrimSpace(out))
			}
		}
	} else {
		logf("not creating folders with mkdir on this headset; adb push makes them")
	}
	if err := a.runPushes("Copying", planPushes(a.gameDir, a.remoteGame(), todo), todo); err != nil {
		return err
	}
	return a.verifyGame(files)
}

// verifyGame lists the headset again and re-copies missing or wrong-size
// files one by one, up to 3 times.
func (a *app) verifyGame(files []gameFile) error {
	say("Checking the copied files...")
	const rounds = 3
	waits := 0
	for round := 1; ; round++ {
		remote, err := a.listRemote(a.remoteGame())
		if err != nil {
			if err == errClosed {
				return err
			}
			if a.headsetGone() {
				if waits++; waits <= 5 && a.waitForHeadset() {
					round--
					continue
				}
				return errDisconnected
			}
			return fail("Could not read the files on the headset (%v).\nCheck the %s and try again.", err, a.connectionWord())
		}
		bad := filesToCopy(files, remote)
		if len(bad) == 0 {
			say("All %d game files are on the headset and checked.", len(files))
			return nil
		}
		logf("verify round %d: %d files missing or wrong size", round, len(bad))
		if round > rounds {
			var b strings.Builder
			b.WriteString("These files could not be copied to the headset:\n")
			for i, f := range bad {
				if i == 20 {
					fmt.Fprintf(&b, "  ... and %d more\n", len(bad)-20)
					break
				}
				fmt.Fprintf(&b, "  %s\n", f.Rel)
			}
			b.WriteString("\nPlease check the " + a.connectionWord() + " (for a cable: try another cable or USB\n" +
				"port) and that the headset has enough free space, then run the launcher\n" +
				"again. It will only copy what is missing.")
			return &userError{msg: b.String()}
		}
		say("%s did not arrive correctly. Copying again (try %d of %d)...", plural(len(bad), "file"), round, rounds)
		if a.helper {
			if err := a.importFiles("Copying again", itemsOf(bad), "XWA"); err != nil {
				return err
			}
			continue
		}
		var jobs []pushJob
		for _, f := range bad {
			jobs = append(jobs, singleFileJob(a.remoteGame(), f))
		}
		if err := a.runPushes("Copying again", jobs, bad); err != nil {
			return err
		}
	}
}

// manifestText is the install file list the app reads at start to name
// missing files: a header line, then "<size>\t<relative/path>" lines.
func manifestText(files []gameFile) string {
	var b strings.Builder
	b.WriteString(manifestHeader + "\n")
	for _, f := range files {
		fmt.Fprintf(&b, "%d\t%s\n", f.Size, f.Rel)
	}
	return b.String()
}

func (a *app) writeManifest(files []gameFile) error {
	text := manifestText(files)
	if a.helper {
		item := importItem{Rel: manifestName, Data: []byte(text), Size: int64(len(text))}
		if err := a.importFiles("", []importItem{item}, "XWA"); err != nil {
			return err
		}
		logf("install file list written (%d files)", len(files))
		return nil
	}
	tmp, err := os.CreateTemp("", "openxwa-install-files-*.txt")
	if err != nil {
		return fail("Could not write a temporary file: %v", err)
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if _, err := tmp.WriteString(text); err != nil {
		tmp.Close()
		return fail("Could not write a temporary file: %v", err)
	}
	tmp.Close()
	remote := a.remoteGame() + "/" + manifestName
	for try := 1; try <= 3; try++ {
		a.adb.run(2*time.Minute, "push", tmpPath, remote)
		out, _ := a.adb.shell(30*time.Second, "stat -c %s "+shQuote(remote))
		if n, err := strconv.ParseInt(firstLine(out), 10, 64); err == nil && n == int64(len(text)) {
			logf("install file list written (%d files)", len(files))
			return nil
		}
	}
	return fail("Could not write the install file list to the headset.\nCheck the %s and try again.", a.connectionWord())
}
