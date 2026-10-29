package main

import (
	"archive/zip"
	"compress/flate"
	"errors"
	"fmt"
	"io"
	"os"
	"path"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// The OpenXWA app has a small helper (SetupProvider in the APK) that adb
// can call without starting the game, so the headset can stay on the desk:
//
//	content call --method mkdirs --arg 'XWA'   makes folders (and the obb
//	                                          folder) as the app
//	push part.zip <obb>/openxwa-import.zip     the app can read its obb folder
//	content call --method unzip --arg 'XWA'    the app unpacks it
//
// Everything in the files folder is written by the app, so it belongs to
// the app. Files and folders adb makes there belong to the "shell" user,
// and on Quest (Horizon OS 14) the app cannot use them. Fallback when the
// obb folder is not readable: the zip is streamed into the app
// (exec-in content write).

const (
	helperURI       = "content://org.openxwa.quest.setup"
	importZipName   = ".openxwa-import.zip"
	importBatchSize = 128 << 20 // bytes of game files per zip
	importBatchMax  = 2500      // files per zip
)

type helperResult map[string]string

var bundleKeyRe = regexp.MustCompile(`(?:\{|, )([A-Za-z_]+)=`)

// parseBundle reads the "Result: Bundle[{key=value, ...}]" line printed by
// "content call". ok is false when there is no Bundle (no helper, refused).
func parseBundle(out string) (r helperResult, ok bool) {
	i := strings.Index(out, "Result: Bundle[{")
	if i < 0 {
		return nil, false
	}
	body := out[i+len("Result: Bundle["):]
	j := strings.LastIndex(body, "}]")
	if j < 0 {
		return nil, false
	}
	body = body[:j] // "{key=value, ..."
	locs := bundleKeyRe.FindAllStringSubmatchIndex(body, -1)
	r = helperResult{}
	for k, l := range locs {
		end := len(body)
		if k+1 < len(locs) {
			end = locs[k+1][0]
		}
		r[body[l[2]:l[3]]] = body[l[1]:end]
	}
	return r, true
}

// helperCall calls the app's helper. Errors carry the helper's own message
// ("error" in the result) or the content tool's.
func (a *app) helperCall(timeout time.Duration, method, arg string) (helperResult, error) {
	out, err := a.adb.shell(timeout, "content call --uri "+helperURI+" --method "+method+" --arg "+shQuote(arg))
	if err == errClosed {
		return nil, err
	}
	r, ok := parseBundle(out)
	if !ok {
		msg := strings.TrimSpace(out)
		switch {
		case strings.Contains(msg, "Result: null"):
			msg = "the app on the headset refused the request"
		case msg == "" && err != nil:
			msg = err.Error()
		case msg == "":
			msg = "no answer from the app on the headset"
		}
		return nil, errors.New(firstLine(msg))
	}
	if e, ok := r["error"]; ok {
		return r, errors.New(e)
	}
	return r, nil
}

// helperMkdirs makes folders (relative to the app's files folder) as the app.
func (a *app) helperMkdirs(rels ...string) error {
	r, err := a.helperCall(2*time.Minute, "mkdirs", strings.Join(rels, ":"))
	if err != nil {
		return err
	}
	if r["made"] != strconv.Itoa(len(rels)) {
		return fmt.Errorf("made %s of %d folders", r["made"], len(rels))
	}
	return nil
}

// tryHelper checks once whether the installed app has the helper, by
// letting it make the XWA folder.
func (a *app) tryHelper() bool {
	if a.helperTried {
		return a.helper
	}
	a.helperTried = true
	if err := a.helperMkdirs("XWA"); err != nil {
		logf("app helper not available: %v", err)
		return false
	}
	logf("app helper available")
	a.helper = true
	return true
}

// removeShellLeftovers deletes what earlier launcher versions copied over
// USB below XWA and pilot-import: it belongs to shell, so the app can
// neither read nor replace it.
func (a *app) removeShellLeftovers() error {
	dirs := shQuote(a.remoteGame()) + " " + shQuote(a.remotePilots())
	find := "find " + dirs + " -mindepth 1 -user " + strconv.Itoa(shellUID) + " -prune"
	out, err := a.adb.shell(5*time.Minute, find+" -print 2>/dev/null | wc -l")
	if err == errClosed {
		return err
	}
	n, perr := strconv.Atoi(strings.TrimSpace(firstLine(out)))
	if perr != nil || n == 0 {
		return nil
	}
	say("Removing what an earlier launcher copied over USB (%d files and folders):\nthe game cannot read it. It is copied again.", n)
	if out, err := a.adb.shell(10*time.Minute, find+" -exec rm -rf {} + 2>&1"); err != nil {
		if err == errClosed {
			return err
		}
		return fail("Could not remove the unusable files on the headset:\n  %s", strings.TrimSpace(out))
	}
	return nil
}

// importItem is one file to put in the zip: a PC file, or data in memory.
type importItem struct {
	Rel  string
	Abs  string // "" when Data is used
	Data []byte
	Size int64
}

func itemsOf(files []gameFile) []importItem {
	out := make([]importItem, len(files))
	for i, f := range files {
		out[i] = importItem{Rel: f.Rel, Abs: f.Abs, Size: f.Size}
	}
	return out
}

// importBatches splits the items into zips of limited size and count.
func importBatches(items []importItem) [][]importItem {
	var out [][]importItem
	var cur []importItem
	var size int64
	for _, it := range items {
		if len(cur) > 0 && (size+it.Size > importBatchSize || len(cur) >= importBatchMax) {
			out = append(out, cur)
			cur, size = nil, 0
		}
		cur = append(cur, it)
		size += it.Size
	}
	if len(cur) > 0 {
		out = append(out, cur)
	}
	return out
}

// importFiles puts the items into dest (relative to the app's files folder,
// e.g. "XWA") through the app, in zips of up to importBatchSize that the
// app unpacks. verb labels the progress display ("" shows none).
func (a *app) importFiles(verb string, items []importItem, dest string) error {
	var files []gameFile
	for _, it := range items {
		files = append(files, gameFile{Rel: it.Rel, Size: it.Size})
	}
	var p *copyProgress
	if verb == "" { // small and quick: no progress display
		p = &copyProgress{stop: make(chan struct{})}
	} else {
		p = newCopyProgress(verb, files)
	}
	defer p.finish()
	batches := importBatches(items)
	for bi, b := range batches {
		part := ""
		if len(batches) > 1 {
			part = fmt.Sprintf(" (part %d of %d)", bi+1, len(batches))
		}
		reconnects := 0
		for try := 1; ; try++ {
			mark := p.snapshot()
			err := a.importBatch(p, b, dest, verb, part)
			if err == nil {
				break
			}
			if err == errClosed || appCtx.Err() != nil {
				return errClosed
			}
			p.restore(mark)
			if err == errImportModeChanged {
				try--
				continue
			}
			logf("import %d of %d failed (try %d): %v", bi+1, len(batches), try, err)
			if a.headsetGone() {
				if !a.waitForHeadset() {
					return errDisconnected
				}
				// A dropped connection does not use up a try (within reason):
				// the batch is sent again from the start.
				if reconnects++; reconnects <= 8 {
					try--
					continue
				}
			}
			if try == 4 {
				return fail("Could not copy the files to the headset:\n  %v\n\n"+
					"Check the %s (for a cable: try another cable or USB port) and that the\n"+
					"headset has enough free space, then try again. Only what is missing is\n"+
					"copied.", err, a.connectionWord())
			}
		}
	}
	return nil
}

// waitForHeadset waits for a dropped connection to come back: 90 s for USB;
// over Wi-Fi (or the SSH tunnel) 3 minutes, reconnecting every few seconds.
func (a *app) waitForHeadset() bool {
	network := isNetSerial(a.adb.Serial)
	limit := 90 * time.Second
	if network {
		limit = 3 * time.Minute
		say("The Wi-Fi connection to the headset dropped. Reconnecting... Keep the\n" +
			"headset awake (on Steam Frame keep \"Lepton Development\" running).")
	} else {
		say("The connection to the headset dropped. Waiting for it to come back...")
	}
	logf("waiting up to %s for %s", limit, a.adb.Serial)
	start := time.Now()
	lastTry := time.Time{}
	for time.Since(start) < limit {
		if network && time.Since(lastTry) >= 6*time.Second {
			lastTry = time.Now()
			a.reconnectNet()
		}
		out, err := a.adb.runQuiet(10*time.Second, "get-state")
		if err == nil && strings.TrimSpace(out) == "device" {
			logf("headset back after %.0f s", time.Since(start).Seconds())
			say("The headset is back. Continuing.")
			sleepCtx(2 * time.Second)
			return true
		}
		if !network && int(time.Since(start).Seconds())%20 < 2 {
			a.adb.runQuiet(15*time.Second, "reconnect", "offline")
		}
		if !sleepCtx(2 * time.Second) {
			return false
		}
	}
	logf("headset did not come back")
	return false
}

type countWriter struct {
	w io.Writer
	n int64
}

func (c *countWriter) Write(b []byte) (int, error) {
	n, err := c.w.Write(b)
	c.n += int64(n)
	return n, err
}

type progressReader struct {
	r io.Reader
	p *copyProgress
}

func (r *progressReader) Read(b []byte) (int, error) {
	n, err := r.r.Read(b)
	r.p.addBytes(int64(n))
	return n, err
}

// writeZip writes the items as a zip (fast deflate; ZipInputStream needs
// deflated entries when sizes follow the data).
func writeZip(w io.Writer, items []importItem, p *copyProgress) error {
	zw := zip.NewWriter(w)
	zw.RegisterCompressor(zip.Deflate, func(out io.Writer) (io.WriteCloser, error) {
		return flate.NewWriter(out, flate.BestSpeed)
	})
	for _, it := range items {
		if p != nil {
			label := path.Dir(it.Rel)
			if label == "." {
				label = "main folder"
			}
			p.setFolder(label)
		}
		zf, err := zw.CreateHeader(&zip.FileHeader{Name: it.Rel, Method: zip.Deflate, Modified: time.Now()})
		if err != nil {
			return err
		}
		var src io.Reader
		var f *os.File
		if it.Abs != "" {
			if f, err = os.Open(it.Abs); err != nil {
				return fmt.Errorf("cannot read %s: %v", it.Rel, err)
			}
			src = f
		} else {
			src = strings.NewReader(string(it.Data))
		}
		if p != nil {
			src = &progressReader{r: src, p: p}
		}
		n, err := io.Copy(zf, src)
		if f != nil {
			f.Close()
		}
		if err != nil {
			return err
		}
		if n != it.Size {
			return fmt.Errorf("%s changed while it was copied (%d bytes, expected %d)", it.Rel, n, it.Size)
		}
		if p != nil {
			p.addFiles(1)
		}
	}
	return zw.Close()
}

var errImportModeChanged = errors.New("import mode changed")

// remoteObb is the app's obb folder: the app can read what adb copies there
// (it is where stores and sideloading tools put game data), unlike
// anything adb copies into its files folder.
func (a *app) remoteObb() string { return a.storage + "/Android/obb/" + appPackage }

const obbZipName = "openxwa-import.zip"

// importBatch puts one zip on the headset and lets the app unpack it. The
// zip is packed into a temporary file and copied with adb push into the
// app's obb folder. If the app cannot read it there, the remaining zips are
// streamed into the app instead (importMode "stream").
func (a *app) importBatch(p *copyProgress, items []importItem, dest, verb, part string) error {
	if a.importMode == "stream" {
		p.setPhase(verb+part, false)
		return a.importBatchStream(p, items, dest, verb, part)
	}
	p.setPhase("Packing"+part, false)
	tmp, err := os.CreateTemp("", "openxwa-import-*.zip")
	if err != nil {
		return fmt.Errorf("cannot create a temporary file: %v", err)
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	werr := writeZip(tmp, items, p)
	cerr := tmp.Close()
	if werr != nil {
		return fmt.Errorf("packing failed: %v", werr)
	}
	if cerr != nil {
		return fmt.Errorf("packing failed: %v", cerr)
	}
	st, err := os.Stat(tmpPath)
	if err != nil {
		return err
	}
	remote := a.remoteObb() + "/" + obbZipName
	p.setPhase("Sending"+part, true)
	out, err := a.adb.run(0, "push", tmpPath, remote)
	if err == errClosed {
		return err
	}
	if err != nil || strings.Contains(out, "adb: error") {
		return fmt.Errorf("sending failed: %s", firstLine(strings.TrimSpace(out)))
	}
	if sz, err := a.adb.runQuiet(30*time.Second, "shell", "stat -c %s "+shQuote(remote)); err == nil {
		if n, perr := strconv.ParseInt(firstLine(strings.TrimSpace(sz)), 10, 64); perr == nil && n != st.Size() {
			return fmt.Errorf("only %s of %s arrived", humanSize(n), humanSize(st.Size()))
		}
	}
	p.setPhase("Unpacking on the headset"+part, true)
	r, err := a.helperCall(0, "unzip", dest)
	defer a.adb.runQuiet(30*time.Second, "shell", "rm -f "+shQuote(remote))
	if err != nil {
		if strings.Contains(err.Error(), obbZipName) && a.importMode == "" {
			// The app cannot read what adb put in its obb folder on this
			// headset: stream the zips into the app from now on.
			logf("the app cannot read the zip in its obb folder (%v); streaming from now on", err)
			a.importMode = "stream"
			return errImportModeChanged
		}
		return fmt.Errorf("unpacking on the headset failed: %v", err)
	}
	logf("unpacked: %v", r)
	if err := checkUnpacked(r, len(items)); err != nil {
		return err
	}
	a.importMode = "obb" // confirmed working
	return nil
}

func checkUnpacked(r helperResult, want int) error {
	if r["files"] != strconv.Itoa(want) || (r["skipped"] != "" && r["skipped"] != "0") {
		return fmt.Errorf("the headset unpacked %s of %d files (%s skipped)", r["files"], want, r["skipped"])
	}
	return nil
}

// importBatchStream streams one zip into the app (adb exec-in, content
// write) and lets it unpack it. adb's input stays open until the headset
// has every byte: closing it earlier drops data still on its way and
// upset the adb server on Windows.
func (a *app) importBatchStream(p *copyProgress, items []importItem, dest, verb, part string) error {
	a.adb.runQuiet(30*time.Second, "shell", "rm -f "+shQuote(a.remoteObb()+"/"+obbZipName))
	pr, pw := io.Pipe()
	type zipResult struct {
		n   int64
		err error
	}
	zipDone := make(chan zipResult, 1)
	adbDone := make(chan adbResult, 1)
	go func() {
		cw := &countWriter{w: pw}
		err := writeZip(cw, items, p)
		if err != nil {
			pw.CloseWithError(err)
		}
		zipDone <- zipResult{cw.n, err}
	}()
	go func() {
		out, err := a.adb.stream(0, pr, "exec-in", "content", "write", "--uri", helperURI+"/import")
		pr.CloseWithError(io.ErrClosedPipe) // unblock the zip writer
		adbDone <- adbResult{out, err}
	}()
	var zr zipResult
	select {
	case zr = <-zipDone:
	case ar := <-adbDone:
		<-zipDone
		return fmt.Errorf("sending stopped early: %v %s", ar.err, strings.TrimSpace(ar.out))
	}
	if zr.err != nil {
		<-adbDone
		return fmt.Errorf("sending failed: %v", zr.err)
	}
	p.setPhase("Sending"+part, true)
	werr := a.waitImportSize(zr.n, adbDone)
	pw.Close()
	select {
	case ar := <-adbDone:
		if msg := strings.TrimSpace(ar.out); msg != "" {
			logf("content write said: %s", msg)
		}
	case <-time.After(60 * time.Second):
		logf("adb exec-in did not end")
	}
	if werr != nil {
		return werr
	}
	p.setPhase("Unpacking on the headset"+part, true)
	r, err := a.helperCall(0, "unzip", dest)
	if err != nil {
		return fmt.Errorf("unpacking on the headset failed: %v", err)
	}
	logf("unpacked: %v", r)
	return checkUnpacked(r, len(items))
}

type adbResult struct {
	out string
	err error
}

// waitImportSize waits until the streamed zip on the headset has all n
// bytes. It stops when adb ends (the result is put back for the caller) or
// the size stops growing for 20 s.
func (a *app) waitImportSize(n int64, adbDone chan adbResult) error {
	remote := a.remoteBase() + "/" + importZipName
	var last int64 = -1
	lastChange := time.Now()
	adbEnded := false
	unreadable := 0
	for {
		out, err := a.adb.runQuiet(30*time.Second, "shell", "stat -c %s "+shQuote(remote))
		if err == errClosed {
			return err
		}
		size, perr := strconv.ParseInt(firstLine(strings.TrimSpace(out)), 10, 64)
		switch {
		case perr != nil:
			unreadable++
			if unreadable >= 20 {
				return fmt.Errorf("cannot read the size of the zip on the headset: %s", firstLine(strings.TrimSpace(out)))
			}
		case size == n:
			logf("zip on the headset: %d bytes", n)
			return nil
		case size != last:
			last, lastChange = size, time.Now()
		}
		if adbEnded {
			return fmt.Errorf("only %s of %s arrived", humanSize(max(last, 0)), humanSize(n))
		}
		if time.Since(lastChange) > 20*time.Second {
			return fmt.Errorf("only %s of %s arrived (stopped)", humanSize(max(last, 0)), humanSize(n))
		}
		select {
		case ar := <-adbDone:
			adbDone <- ar
			adbEnded = true // check the size once more
		case <-time.After(500 * time.Millisecond):
		case <-appCtx.Done():
			return errClosed
		}
	}
}
