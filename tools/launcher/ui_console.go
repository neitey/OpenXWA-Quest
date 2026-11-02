package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"time"
)

// consoleUI is the plain text backend (--console, and for testing). At the
// end of input every question gets its default answer.
type consoleUI struct {
	r     *bufio.Reader
	eof   bool
	steps []string

	mu       sync.Mutex
	last     time.Time
	lastLen  int
	active   bool
	lastProg *Progress
}

func newConsoleUI(in io.Reader) *consoleUI { return &consoleUI{r: bufio.NewReader(in)} }

func (c *consoleUI) out(s string) { fmt.Fprint(os.Stdout, s) }

// endLine finishes an in-place progress line before normal output.
func (c *consoleUI) endLine() {
	if c.active && c.lastLen > 0 {
		c.out("\n")
		c.lastLen = 0
	}
}

func (c *consoleUI) SetSteps(titles []string) { c.steps = titles }

func (c *consoleUI) SetStep(n int, state string) {
	if state != "current" || n < 1 || n > len(c.steps) {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	c.endLine()
	msg := fmt.Sprintf("Step %d of %d: %s", n, len(c.steps), c.steps[n-1])
	logf("SAY: %s", msg)
	c.out("\n" + msg + "\n")
}

func (c *consoleUI) Say(msg string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.endLine()
	c.out(msg + "\n")
}

const progressWidth = 78

// Progress redraws one console line with \r, at most every 250 ms.
func (c *consoleUI) Progress(p *Progress) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if p == nil {
		if c.active && c.lastProg != nil {
			c.draw(c.lastProg, true, true)
			c.out("\n")
			logf("PROGRESS: %s: %s", c.lastProg.Label, c.lastProg.Summary())
		}
		c.active, c.lastProg, c.lastLen = false, nil, 0
		return
	}
	cp := *p
	c.lastProg = &cp
	c.active = true
	c.draw(&cp, false, false)
}

func (c *consoleUI) draw(p *Progress, force, final bool) {
	if !force && time.Since(c.last) < 250*time.Millisecond {
		return
	}
	c.last = time.Now()
	var s string
	if p.TotalFiles > 0 {
		s = fmt.Sprintf("%s: %d of %d files (%s of %s)", p.Label, p.Files, p.TotalFiles, humanSize(p.Bytes), humanSize(p.TotalBytes))
		if sp := p.Speed(time.Now()); sp != "" {
			s += ", " + sp
		}
	} else {
		s = p.Label + ": " + p.Summary()
	}
	if !final && p.Folder != "" {
		spinner := `|/-\`[p.Spin%4 : p.Spin%4+1]
		room := progressWidth - len(s) - 4
		f := p.Folder
		if room < 4 {
			f = ""
		} else if len(f) > room {
			f = "..." + f[len(f)-room+3:]
		}
		s += "  " + f + " " + spinner
	}
	if len(s) > progressWidth {
		s = s[:progressWidth]
	}
	pad := ""
	if c.lastLen > len(s) {
		pad = strings.Repeat(" ", c.lastLen-len(s))
	}
	c.out("\r" + s + pad)
	c.lastLen = len(s)
}

// readLine returns one line without the line ending; ok is false at end of input.
func (c *consoleUI) readLine() (string, bool) { return c.readLineSecret(false) }

// readLineSecret is readLine; a secret line is not written to the log.
func (c *consoleUI) readLineSecret(secret bool) (string, bool) {
	if c.eof {
		return "", false
	}
	s, err := c.r.ReadString('\n')
	if err != nil {
		c.eof = true
		if s == "" {
			logf("INPUT: <end of input>")
			return "", false
		}
	}
	s = strings.TrimRight(s, "\r\n")
	if secret {
		logf("INPUT: (secret, not logged)")
	} else {
		logf("INPUT: %q", s)
	}
	return s, true
}

func (c *consoleUI) prompt(text string) (string, bool) { return c.promptSecret(text, false) }

func (c *consoleUI) promptSecret(text string, secret bool) (string, bool) {
	c.mu.Lock()
	c.endLine()
	c.out(text + " ")
	c.mu.Unlock()
	logf("ASK: %s", text)
	line, ok := c.readLineSecret(secret)
	if !ok {
		c.out("\n")
	}
	return strings.TrimSpace(line), ok
}

func (c *consoleUI) Ask(q Question) Answer {
	// Titles are for menu-style questions; a yes/no prompt carries its own text.
	if q.Title != "" && (q.ConsolePrompt == "" || q.Error) {
		say("")
		say("%s", q.Title)
	}
	if q.Text != "" && (q.ConsolePrompt == "" || !strings.Contains(q.ConsolePrompt, q.Text)) {
		say("%s", q.Text)
	}
	if q.Notes != "" {
		lines := strings.Split(strings.TrimSpace(strings.ReplaceAll(q.Notes, "\r\n", "\n")), "\n")
		for i, l := range lines {
			if i == 15 {
				say("  ...")
				break
			}
			say("  %s", l)
		}
	}
	if q.Link != "" {
		say("  %s", q.Link)
	}
	for _, l := range q.List {
		say("  %s", l)
	}
	if q.Problem != "" {
		say("%s", q.Problem)
	}

	if q.Kind == "text" {
		if q.Value != "" {
			say("  %s", q.Value)
		}
		submit := "ok"
		if len(q.Options) > 0 {
			submit = q.Options[0].ID
		}
		p := q.ConsolePrompt
		if p == "" {
			p = "Folder:"
		}
		for {
			line, ok := c.promptSecret(p, q.Secret)
			if !ok {
				return Answer{ID: q.Default, Text: q.Value, Closed: true}
			}
			if line != "" {
				return Answer{ID: submit, Text: line}
			}
			if q.Value != "" {
				return Answer{ID: submit, Text: q.Value}
			}
		}
	}

	p := q.ConsolePrompt
	if p == "" {
		say("")
		for _, o := range q.Options {
			key := "[" + o.Key + "]"
			if o.Key == "" {
				key = "[Enter]"
			}
			say("  %-8s %s", key, o.Label)
		}
		p = "Your choice:"
	}
	for {
		line, ok := c.prompt(p)
		if !ok {
			return Answer{ID: q.Default, Closed: true}
		}
		if line == "" {
			for _, o := range q.Options {
				if o.Key == "" {
					return Answer{ID: o.ID}
				}
			}
			if q.Default != "" {
				return Answer{ID: q.Default}
			}
			continue
		}
		for _, o := range q.Options {
			if o.Key != "" && strings.EqualFold(line, o.Key) {
				return Answer{ID: o.ID}
			}
		}
		// Common yes/no words.
		if l := strings.ToLower(line); l == "yes" || l == "ja" || l == "no" || l == "nein" {
			for _, o := range q.Options {
				if (o.Key == "y" && (l == "yes" || l == "ja")) || (o.Key == "n" && (l == "no" || l == "nein")) {
					return Answer{ID: o.ID}
				}
			}
		}
		if q.AllowText {
			return Answer{ID: "text", Text: line}
		}
		var keys []string
		for _, o := range q.Options {
			if o.Key != "" {
				keys = append(keys, o.Key)
			}
		}
		say("Please type %s (or press Enter).", strings.Join(keys, ", "))
	}
}

func (c *consoleUI) Offer(Question) <-chan Answer { return nil }
func (c *consoleUI) ClearOffer()                  {}

func (c *consoleUI) OpenPath(p string) { say("  %s", p) }

func (c *consoleUI) Finish(code int, pause bool) {
	if !pause {
		return
	}
	c.prompt("\nPress Enter to close.")
}
