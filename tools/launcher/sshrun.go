package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
	"time"
)

// sshRunner runs single commands on the Steam Frame (SteamOS, user
// "steamos") with the system's OpenSSH client, one ssh process per command.
// The password is handed over through SSH_ASKPASS the way the tunnel does
// (sshtunnel.go); with an ssh too old for SSH_ASKPASS_REQUIRE the user types
// it at ssh's own prompt for every command (console mode).
type sshRunner struct {
	sshPath   string
	host      string // the IP address
	name      string // what the user typed (for messages)
	password  string // memory only
	mode      string // askpass or console
	acceptNew bool
	runs      int
}

// sshRunError is a failed ssh command: kind as classifySSH sorts it, or
// "exit" when the remote command itself failed.
type sshRunError struct {
	kind   string
	code   int
	stderr string
}

func (e *sshRunError) Error() string {
	if e.kind == "exit" {
		return fmt.Sprintf("the command on the Steam Frame failed (exit code %d): %s", e.code, lastLines(e.stderr, 3))
	}
	return fmt.Sprintf("ssh: %s: %s", e.kind, lastLines(e.stderr, 3))
}

func newSSHRunner(nameIn string) (*sshRunner, error) {
	host, _, err := splitHostPort(nameIn, 22)
	if err != nil {
		return nil, &wifiProblem{kind: "notfound", host: nameIn, msg: err.Error()}
	}
	sshPath := findSSH()
	if sshPath == "" {
		logf("ssh not found in: %s", strings.Join(sshCandidates(), "; "))
		return nil, &wifiProblem{kind: "nossh", host: host, msg: sshProblemText("nossh", host)}
	}
	ver := sshVersionString(sshPath)
	logf("ssh: %s (%s)", sshPath, ver)
	ip, rerr := resolveHost(host)
	if rerr != nil {
		return nil, &wifiProblem{kind: "notfound", host: host, msg: sshProblemText("notfound", host)}
	}
	maj, min, ok := parseSSHVersion(ver)
	r := &sshRunner{sshPath: sshPath, host: ip, name: host, mode: "askpass",
		acceptNew: !ok || maj > 7 || (maj == 7 && min >= 6)}
	// SSH_ASKPASS_REQUIRE=force needs OpenSSH 8.4; older ones ask on their
	// console (the launcher's, in --console mode) for every command.
	if ok && (maj < 8 || (maj == 8 && min < 4)) {
		r.mode = "console"
	}
	if _, console := ui.(*consoleUI); !console && r.mode == "console" {
		return nil, &wifiProblem{kind: "nowindow", host: host, msg: sshProblemText("nowindow", host)}
	}
	return r, nil
}

func (r *sshRunner) args(remote string) []string {
	strict := "accept-new"
	if !r.acceptNew {
		strict = "no"
	}
	return []string{
		"-T",
		"-o", "ServerAliveInterval=15",
		"-o", "ServerAliveCountMax=4",
		"-o", "ConnectTimeout=20",
		"-o", "StrictHostKeyChecking=" + strict,
		"-o", "UserKnownHostsFile=" + sshOptionPath(sshKnownHostsPath()),
		"-o", "PubkeyAuthentication=no",
		"-o", "GSSAPIAuthentication=no",
		"-o", "PreferredAuthentications=keyboard-interactive,password",
		"-o", "NumberOfPasswordPrompts=1",
		"-o", "LogLevel=ERROR",
		"-l", frameUser,
		"-p", "22",
		r.host,
		remote,
	}
}

// run executes `remote` (a sh command line) on the Frame. stdin (may be
// nil) is streamed to it, its output to stdout (may be nil: collected and
// returned). Returns the collected output.
func (r *sshRunner) run(timeout time.Duration, remote string, stdin io.Reader, stdout io.Writer) (string, error) {
	ctx, cancel := context.WithTimeout(appCtx, timeout)
	defer cancel()
	r.runs++
	cmd := exec.CommandContext(ctx, r.sshPath, r.args(remote)...)
	cmd.WaitDelay = 3 * time.Second
	env := os.Environ()
	var srv *askpassServer
	if r.mode == "askpass" {
		s, err := newAskpassServer(r.password)
		if err != nil {
			return "", fmt.Errorf("askpass: %v", err)
		}
		srv = s
		defer srv.close()
		exe, _ := os.Executable()
		env = append(env, srv.env()...)
		env = append(env, "SSH_ASKPASS="+exe, "SSH_ASKPASS_REQUIRE=force")
		if os.Getenv("DISPLAY") == "" {
			env = append(env, "DISPLAY=localhost:0")
		}
	} else {
		var clean []string
		for _, e := range env {
			if !strings.HasPrefix(e, "SSH_ASKPASS") {
				clean = append(clean, e)
			}
		}
		env = clean
	}
	cmd.Env = env
	sshProcAttr(cmd, r.mode)
	if stdin == nil {
		stdin = strings.NewReader("")
	}
	cmd.Stdin = stdin
	var out bytes.Buffer
	if stdout == nil {
		cmd.Stdout = &out
	} else {
		cmd.Stdout = stdout
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	short := remote
	if len(short) > 160 {
		short = short[:160] + "..."
	}
	logf("ssh %s: %s", r.host, strings.ReplaceAll(short, "\n", " "))
	started := time.Now()
	err := cmd.Run()
	errText := stderr.String()
	if err != nil {
		if appCtx.Err() != nil {
			return out.String(), errClosed
		}
		var ee *exec.ExitError
		code := -1
		if errors.As(err, &ee) {
			code = ee.ExitCode()
		}
		if code == 255 {
			// ssh itself failed: no connection, no login, host key.
			kind := classifySSH(errText)
			if kind == "other" {
				kind = "ssh"
			}
			logf("ssh failed after %.1f s (%s): %s", time.Since(started).Seconds(), kind, lastLines(errText, 3))
			if srv != nil && srv.count() == 0 && (kind == "sshauth" || kind == "ssh") {
				kind = "noaskpass"
			}
			return out.String(), &sshRunError{kind: kind, code: code, stderr: errText}
		}
		if ctx.Err() != nil {
			return out.String(), &sshRunError{kind: "timeout", code: code, stderr: errText}
		}
		logf("remote command failed after %.1f s (exit %d): %s", time.Since(started).Seconds(), code, lastLines(errText, 5))
		return out.String(), &sshRunError{kind: "exit", code: code, stderr: errText}
	}
	logf("ssh done after %.1f s", time.Since(started).Seconds())
	return out.String(), nil
}

// sh runs a command and returns its output; the error text includes the
// remote stderr.
func (r *sshRunner) sh(timeout time.Duration, remote string) (string, error) {
	return r.run(timeout, remote, nil, nil)
}

// sshErrorKind sorts an error from run for the problem cards.
func sshErrorKind(err error) string {
	var se *sshRunError
	if errors.As(err, &se) {
		return se.kind
	}
	return ""
}
