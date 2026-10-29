package main

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

// The web UI: a tiny HTTP server on 127.0.0.1 (random port) serves one
// embedded page, opened as an app window (Edge/Chrome --app, or the default
// browser). Every request must carry the random token. The page polls
// /api/state and posts answers to /api/answer. It also holds /api/alive
// open, so a closed window is noticed even when the page's timers are
// throttled in the background.

//go:embed web/index.html
var indexHTML []byte

const (
	windowGoneAfter = 15 * time.Second
	maxLogLines     = 3000
)

type stepView struct {
	Title string `json:"title"`
	State string `json:"state"` // pending, current, done, error
}

type progressView struct {
	Label  string  `json:"label"`
	Text   string  `json:"text"`
	Folder string  `json:"folder,omitempty"`
	Frac   float64 `json:"frac"`
	Busy   bool    `json:"busy"`
}

type questionView struct {
	ID int `json:"id"`
	Question
}

type webUI struct {
	mu        sync.Mutex
	token     string
	addr      string
	url       string
	srv       *http.Server
	canBrowse bool

	steps    []stepView
	current  int
	headline string
	recent   []string // status lines of the current step
	log      []string
	logBase  int // index of log[0] in the whole log
	prog     *progressView
	q        *questionView
	qCh      chan Answer
	offer    *questionView
	offerCh  chan Answer
	seq      int
	finished bool
	code     int
	seenDone bool

	started   time.Time
	connected bool
	lastSeen  time.Time
	alive     int
	hadAlive  bool // an /api/alive request was open at some time
	warned    bool
	closed    chan struct{}
	closeOnce sync.Once
}

func randomToken() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

// startWebUI starts the server; the window is opened by the caller.
func startWebUI() (*webUI, error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	w := &webUI{
		token:     randomToken(),
		addr:      ln.Addr().String(),
		started:   time.Now(),
		closed:    make(chan struct{}),
		canBrowse: folderPickerAvailable(),
	}
	w.url = "http://" + w.addr + "/?token=" + w.token
	mux := http.NewServeMux()
	mux.HandleFunc("/", w.handleIndex)
	mux.HandleFunc("/api/state", w.handleState)
	mux.HandleFunc("/api/answer", w.handleAnswer)
	mux.HandleFunc("/api/alive", w.handleAlive)
	mux.HandleFunc("/api/browse", w.handleBrowse)
	mux.HandleFunc("/api/open", w.handleOpen)
	w.srv = &http.Server{Handler: w.guard(mux), ReadHeaderTimeout: 10 * time.Second}
	go w.srv.Serve(ln)
	go w.watchdog()
	logf("web UI listening on %s", w.addr)
	return w, nil
}

// guard rejects requests without the token or for another host name (DNS
// rebinding), and records that the page is still there.
func (w *webUI) guard(next http.Handler) http.Handler {
	return http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		host := r.Host
		if host != w.addr && host != strings.Replace(w.addr, "127.0.0.1", "localhost", 1) {
			http.Error(rw, "forbidden", http.StatusForbidden)
			return
		}
		tok := r.Header.Get("X-Token")
		if tok == "" {
			tok = r.URL.Query().Get("token")
		}
		if subtle.ConstantTimeCompare([]byte(tok), []byte(w.token)) != 1 {
			logf("web: rejected %s %s (no or wrong token)", r.Method, r.URL.Path)
			http.Error(rw, "forbidden", http.StatusForbidden)
			return
		}
		w.mu.Lock()
		if !w.connected {
			logf("web: the launcher window connected")
		}
		w.connected = true
		w.lastSeen = time.Now()
		w.mu.Unlock()
		rw.Header().Set("Cache-Control", "no-store")
		rw.Header().Set("X-Content-Type-Options", "nosniff")
		next.ServeHTTP(rw, r)
	})
}

func (w *webUI) handleIndex(rw http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(rw, r)
		return
	}
	rw.Header().Set("Content-Type", "text/html; charset=utf-8")
	rw.Header().Set("Content-Security-Policy", "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; img-src data:")
	rw.Write(indexHTML)
}

func (w *webUI) handleState(rw http.ResponseWriter, r *http.Request) {
	from := 0
	fmt.Sscanf(r.URL.Query().Get("log"), "%d", &from)
	w.mu.Lock()
	if from < w.logBase {
		from = w.logBase
	}
	var lines []string
	if from-w.logBase < len(w.log) {
		lines = append(lines, w.log[from-w.logBase:]...)
	}
	st := map[string]any{
		"version":   version,
		"steps":     w.steps,
		"current":   w.current,
		"headline":  w.headline,
		"recent":    w.recent,
		"log":       lines,
		"logNext":   w.logBase + len(w.log),
		"progress":  w.prog,
		"question":  w.q,
		"offer":     w.offer,
		"finished":  w.finished,
		"code":      w.code,
		"canBrowse": w.canBrowse,
	}
	if w.finished {
		w.seenDone = true
	}
	w.mu.Unlock()
	rw.Header().Set("Content-Type", "application/json")
	json.NewEncoder(rw).Encode(st)
}

func (w *webUI) handleAnswer(rw http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(rw, "POST only", http.StatusMethodNotAllowed)
		return
	}
	var in struct {
		ID     int    `json:"id"`
		Option string `json:"option"`
		Text   string `json:"text"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(rw, r.Body, 64<<10)).Decode(&in); err != nil {
		http.Error(rw, "bad request", http.StatusBadRequest)
		return
	}
	w.mu.Lock()
	var ch chan Answer
	var q *questionView
	switch {
	case w.q != nil && w.q.ID == in.ID:
		q, ch = w.q, w.qCh
		w.q, w.qCh = nil, nil
	case w.offer != nil && w.offer.ID == in.ID:
		q, ch = w.offer, w.offerCh
		w.offer, w.offerCh = nil, nil
	}
	w.mu.Unlock()
	if ch == nil {
		http.Error(rw, "this question is no longer open", http.StatusConflict)
		return
	}
	valid := false
	for _, o := range q.Options {
		if o.ID == in.Option {
			valid = true
		}
	}
	if !valid {
		in.Option = q.Default
	}
	ch <- Answer{ID: in.Option, Text: in.Text}
	rw.WriteHeader(http.StatusNoContent)
}

// handleAlive keeps a request open while the window exists.
func (w *webUI) handleAlive(rw http.ResponseWriter, r *http.Request) {
	w.mu.Lock()
	w.alive++
	w.hadAlive = true
	w.mu.Unlock()
	defer func() {
		w.mu.Lock()
		w.alive--
		w.lastSeen = time.Now()
		w.mu.Unlock()
	}()
	rw.Header().Set("Content-Type", "text/plain")
	rw.WriteHeader(http.StatusOK)
	fl, _ := rw.(http.Flusher)
	if fl != nil {
		fl.Flush()
	}
	t := time.NewTicker(10 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case <-w.closed:
			return
		case <-t.C:
			if _, err := rw.Write([]byte(" ")); err != nil {
				return
			}
			if fl != nil {
				fl.Flush()
			}
		}
	}
}

func (w *webUI) handleBrowse(rw http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost || !w.canBrowse {
		http.Error(rw, "not available", http.StatusNotFound)
		return
	}
	var in struct {
		Initial string `json:"initial"`
	}
	json.NewDecoder(http.MaxBytesReader(rw, r.Body, 64<<10)).Decode(&in)
	path, ok, err := pickFolder(in.Initial)
	if err != nil {
		logf("folder picker failed: %v", err)
	}
	rw.Header().Set("Content-Type", "application/json")
	json.NewEncoder(rw).Encode(map[string]any{"path": path, "ok": ok && err == nil})
}

// handleOpen opens the link of the open question in the normal browser.
// Only that link can be opened; the page cannot name arbitrary targets.
func (w *webUI) handleOpen(rw http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(rw, "POST only", http.StatusMethodNotAllowed)
		return
	}
	w.mu.Lock()
	link := ""
	if w.q != nil {
		link = w.q.Link
	}
	if link == "" && w.offer != nil {
		link = w.offer.Link
	}
	w.mu.Unlock()
	if !strings.HasPrefix(link, "https://") {
		http.Error(rw, "nothing to open", http.StatusNotFound)
		return
	}
	logf("web: opening %s", link)
	if err := openURL(link); err != nil {
		logf("could not open %s: %v", link, err)
	}
	rw.WriteHeader(http.StatusNoContent)
}

// watchdog notices a closed window (no requests for 15 s and no open
// /api/alive request) and stops the launcher.
func (w *webUI) watchdog() {
	t := time.NewTicker(time.Second)
	defer t.Stop()
	for {
		select {
		case <-w.closed:
			return
		case <-t.C:
		}
		w.mu.Lock()
		connected, alive, last, started, warned := w.connected, w.alive, w.lastSeen, w.started, w.warned
		// The page re-opens /api/alive within a second, so once it has been
		// open, a few seconds without it mean the window is gone.
		gone := windowGoneAfter
		if w.hadAlive {
			gone = 5 * time.Second
		}
		if !connected && !warned && time.Since(started) > 25*time.Second {
			w.warned = true
		}
		w.mu.Unlock()
		switch {
		case connected && alive == 0 && time.Since(last) > gone:
			w.markClosed("the launcher window was closed")
		case !connected && !warned && time.Since(started) > 25*time.Second:
			logf("web: the window has not connected yet; showing the address")
			go showOpenProblem(w.url)
		case !connected && time.Since(started) > 15*time.Minute:
			w.markClosed("the launcher window never opened")
		}
	}
}

func (w *webUI) markClosed(why string) {
	w.closeOnce.Do(func() {
		logf("web: %s; stopping (any copy in progress is stopped; the next run continues where it stopped)", why)
		close(w.closed)
		appCancel()
	})
}

// Closed is closed when the window is gone.
func (w *webUI) Closed() <-chan struct{} { return w.closed }

func (w *webUI) addLog(msg string) {
	for _, l := range strings.Split(msg, "\n") {
		w.log = append(w.log, l)
	}
	if over := len(w.log) - maxLogLines; over > 0 {
		w.log = append([]string(nil), w.log[over:]...)
		w.logBase += over
	}
}

func (w *webUI) SetSteps(titles []string) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.steps = nil
	for _, t := range titles {
		w.steps = append(w.steps, stepView{Title: t, State: "pending"})
	}
	w.current = 0
}

func (w *webUI) SetStep(n int, state string) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if n < 1 || n > len(w.steps) {
		return
	}
	w.steps[n-1].State = state
	if state == "current" {
		w.current = n
		w.headline = w.steps[n-1].Title
		w.recent = nil
		w.prog = nil
		w.addLog(fmt.Sprintf("— Step %d of %d: %s —", n, len(w.steps), w.steps[n-1].Title))
	}
}

// SetHeadline shows a title outside the numbered steps (update check).
func (w *webUI) SetHeadline(h string) {
	w.mu.Lock()
	w.headline = h
	w.recent = nil
	w.mu.Unlock()
}

func (w *webUI) Say(msg string) {
	msg = strings.TrimRight(msg, "\n")
	if strings.TrimSpace(msg) == "" {
		return
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	w.addLog(msg)
	w.recent = append(w.recent, reflow(msg))
	if len(w.recent) > 5 {
		w.recent = w.recent[len(w.recent)-5:]
	}
}

func (w *webUI) Progress(p *Progress) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if p == nil {
		if w.prog != nil {
			w.addLog(w.prog.Label + ": " + w.prog.Text)
			logf("PROGRESS: %s: %s", w.prog.Label, w.prog.Text)
		}
		w.prog = nil
		return
	}
	w.prog = &progressView{Label: p.Label, Text: p.Summary(), Folder: p.Folder, Frac: p.Fraction(), Busy: p.Busy}
}

// reflow joins the hard line breaks of the console texts so the browser can
// wrap them: a line break stays only before an empty or indented line, or
// after a line ending in ":".
func reflow(s string) string {
	lines := strings.Split(s, "\n")
	var b strings.Builder
	for i, l := range lines {
		if i > 0 {
			prev := lines[i-1]
			if strings.TrimSpace(prev) == "" || strings.TrimSpace(l) == "" ||
				strings.HasPrefix(l, " ") || strings.HasPrefix(l, "\t") || strings.HasSuffix(prev, ":") {
				b.WriteString("\n")
			} else {
				b.WriteString(" ")
			}
		}
		b.WriteString(l)
	}
	return b.String()
}

func (w *webUI) Ask(q Question) Answer {
	if q.Kind == "" {
		q.Kind = "buttons"
	}
	q.Text, q.Problem = reflow(q.Text), reflow(q.Problem)
	ch := make(chan Answer, 1)
	w.mu.Lock()
	w.seq++
	w.q = &questionView{ID: w.seq, Question: q}
	w.qCh = ch
	w.mu.Unlock()
	logf("ASK: %s | %s", q.Title, q.Text)
	select {
	case a := <-ch:
		if q.Secret {
			logf("ANSWER: %s (secret text not logged)", a.ID)
		} else {
			logf("ANSWER: %s %q", a.ID, a.Text)
		}
		return a
	case <-w.closed:
		return Answer{ID: q.Default, Text: q.Value, Closed: true}
	}
}

func (w *webUI) Offer(q Question) <-chan Answer {
	if q.Kind == "" {
		q.Kind = "buttons"
	}
	q.Text, q.Problem = reflow(q.Text), reflow(q.Problem)
	ch := make(chan Answer, 1)
	w.mu.Lock()
	w.seq++
	w.offer = &questionView{ID: w.seq, Question: q}
	w.offerCh = ch
	w.mu.Unlock()
	logf("OFFER: %s | %s", q.Title, q.Text)
	return ch
}

func (w *webUI) ClearOffer() {
	w.mu.Lock()
	w.offer, w.offerCh = nil, nil
	w.mu.Unlock()
}

func (w *webUI) OpenPath(p string) {
	logf("opening %s", p)
	if err := openPath(p); err != nil {
		logf("could not open %s: %v", p, err)
		say("Could not open %s", p)
	}
}

// Finish shows the end state, waits until the page has seen it (at most a
// few seconds), then stops the server.
func (w *webUI) Finish(code int, pause bool) {
	w.mu.Lock()
	w.finished = true
	w.code = code
	w.q, w.offer, w.prog = nil, nil, nil
	w.mu.Unlock()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		w.mu.Lock()
		seen := w.seenDone
		w.mu.Unlock()
		if seen {
			time.Sleep(500 * time.Millisecond) // let the page finish its last poll
			break
		}
		select {
		case <-w.closed:
			deadline = time.Now()
		case <-time.After(100 * time.Millisecond):
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	w.srv.Shutdown(ctx)
	w.closeOnce.Do(func() { close(w.closed) })
}

// openWindow opens the page in an app window, or prints the address.
func (w *webUI) openWindow(exeDir string, noWindow bool) {
	if noWindow || os.Getenv("OPENXWA_NO_WINDOW") != "" {
		fmt.Println(launcherName + " is running at: " + w.url)
		logf("not opening a window (--no-window): %s", w.url)
		return
	}
	if err := openAppWindow(w.url, exeDir); err != nil {
		logf("could not open the launcher window: %v", err)
		go showOpenProblem(w.url)
	}
}
