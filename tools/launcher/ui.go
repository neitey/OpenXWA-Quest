package main

import (
	"fmt"
	"strings"
	"time"
)

// The flow talks to the user only through the UI interface. There are two
// backends: the console (ui_console.go, --console) and the web window
// (ui_web.go, the default).

// Option is one button of a question.
type Option struct {
	ID    string `json:"id"`
	Label string `json:"label"`
	Key   string `json:"-"`               // console key; "" means Enter
	Style string `json:"style,omitempty"` // "primary", "danger" or ""
}

// Question is a card with text and buttons, optionally a text field.
type Question struct {
	Kind        string   `json:"kind"` // "buttons" or "text"
	Title       string   `json:"title,omitempty"`
	Text        string   `json:"text,omitempty"`
	Notes       string   `json:"notes,omitempty"` // release notes (scrollable box)
	Link        string   `json:"link,omitempty"`  // shown as a clickable link
	List        []string `json:"list,omitempty"`  // e.g. file names
	Problem     string   `json:"problem,omitempty"`
	Options     []Option `json:"options"`
	Default     string   `json:"default,omitempty"` // option chosen by Enter / end of input
	Value       string   `json:"value,omitempty"`   // text field content
	Placeholder string   `json:"placeholder,omitempty"`
	Browse      bool     `json:"browse,omitempty"` // offer a folder picker next to the field
	Error       bool     `json:"error,omitempty"`  // red card
	Secret      bool     `json:"secret,omitempty"` // password field; the answer is never logged

	// Console only.
	AllowText     bool   `json:"-"` // free text is accepted as the answer "text"
	ConsolePrompt string `json:"-"`
}

// Answer is the user's reply. Closed is set when the window was closed or
// the console input ended; ID is then the question's default.
type Answer struct {
	ID     string
	Text   string
	Closed bool
}

// Progress is the state of a long task.
type Progress struct {
	Label      string // "Copying", "Downloading", ...
	Files      int
	TotalFiles int
	Bytes      int64
	TotalBytes int64
	Folder     string
	Busy       bool      // a long single operation is running (counts move later)
	Spin       int       // console spinner phase
	Started    time.Time // set for copies: shows speed and time left
}

// Fraction returns 0..1 (bytes if known, else files), or -1 if unknown.
func (p *Progress) Fraction() float64 {
	switch {
	case p.TotalBytes > 0:
		return float64(p.Bytes) / float64(p.TotalBytes)
	case p.TotalFiles > 0:
		return float64(p.Files) / float64(p.TotalFiles)
	}
	return -1
}

// Summary is "412 of 1530 files - 380 MB of 1.02 GB" (or bytes only),
// followed by the speed and time left for copies.
func (p *Progress) Summary() string {
	s := humanSize(p.Bytes)
	if p.TotalFiles > 0 {
		s = fmt.Sprintf("%d of %d files - %s of %s", p.Files, p.TotalFiles, humanSize(p.Bytes), humanSize(p.TotalBytes))
	} else if p.TotalBytes > 0 {
		s = fmt.Sprintf("%s of %s (%d%%)", humanSize(p.Bytes), humanSize(p.TotalBytes), p.Bytes*100/p.TotalBytes)
	}
	if sp := p.Speed(time.Now()); sp != "" {
		s += " - " + sp
	}
	return s
}

// Speed is "12.3 MB/s, about 4 min left" (average since Started), or "" while
// not known yet.
func (p *Progress) Speed(now time.Time) string {
	if p.Started.IsZero() || p.Bytes <= 0 {
		return ""
	}
	el := now.Sub(p.Started)
	if el < 3*time.Second {
		return ""
	}
	rate := float64(p.Bytes) / el.Seconds()
	s := humanSize(int64(rate)) + "/s"
	if left := p.TotalBytes - p.Bytes; left > 0 && rate > 0 {
		s += ", about " + humanDuration(time.Duration(float64(left)/rate*float64(time.Second))) + " left"
	}
	return s
}

// humanDuration is "40 s", "12 min" or "1 h 5 min".
func humanDuration(d time.Duration) string {
	switch {
	case d < time.Minute:
		return fmt.Sprintf("%d s", int(d.Seconds()+0.5))
	case d < time.Hour:
		return fmt.Sprintf("%d min", int(d.Minutes()+0.5))
	default:
		m := int(d.Minutes()+0.5) % 60
		return fmt.Sprintf("%d h %d min", int(d.Hours()), m)
	}
}

// UI is implemented by consoleUI and webUI.
type UI interface {
	SetSteps(titles []string)
	SetStep(n int, state string) // n is 1-based; state "current", "done" or "error"
	Say(msg string)
	Progress(p *Progress) // nil ends the progress display
	Ask(q Question) Answer
	// Offer shows a question without waiting; the answer arrives on the
	// channel. Returns nil if the backend cannot do that (console).
	Offer(q Question) <-chan Answer
	ClearOffer()
	OpenPath(p string) // open a folder or file for the user (console: print it)
	Finish(code int, pause bool)
}

// ui is the active backend.
var ui UI

// say shows a line to the user and writes it to the log.
func say(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	if strings.TrimSpace(msg) != "" {
		logf("SAY: %s", msg)
	}
	if ui != nil {
		ui.Say(msg)
	}
}

// askYesNo asks a question with Yes/No buttons.
func askYesNo(title, text string, def bool) bool {
	d, prompt := "no", "(y/N)"
	if def {
		d, prompt = "yes", "(Y/n)"
	}
	q := Question{
		Kind:  "buttons",
		Title: title,
		Text:  text,
		Options: []Option{
			{ID: "yes", Label: "Yes", Key: "y"},
			{ID: "no", Label: "No", Key: "n"},
		},
		Default:       d,
		ConsolePrompt: strings.TrimSpace(text + " " + prompt),
	}
	if def {
		q.Options[0].Style = "primary"
	} else {
		q.Options[1].Style = "primary"
	}
	return ui.Ask(q).ID == "yes"
}

// sleepCtx waits d, returning false early if the launcher is stopping.
func sleepCtx(d time.Duration) bool {
	select {
	case <-time.After(d):
		return true
	case <-appCtx.Done():
		return false
	}
}

// humanSize formats a byte count the way Windows Explorer users expect.
func humanSize(b int64) string {
	const (
		kb = 1024
		mb = 1024 * kb
		gb = 1024 * mb
	)
	switch {
	case b < kb:
		return fmt.Sprintf("%d bytes", b)
	case b < mb:
		return fmt.Sprintf("%d KB", b/kb)
	case b < 10*mb:
		return fmt.Sprintf("%.1f MB", float64(b)/mb)
	case b < gb:
		return fmt.Sprintf("%d MB", b/mb)
	default:
		return fmt.Sprintf("%.2f GB", float64(b)/gb)
	}
}

// plural returns "1 file" or "3 files".
func plural(n int, word string) string {
	if n == 1 {
		return "1 " + word
	}
	return fmt.Sprintf("%d %ss", n, word)
}
