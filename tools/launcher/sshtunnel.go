package main

import (
	"bufio"
	"bytes"
	"crypto/subtle"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Steam Frame through SSH: when Lepton's adb port is not reachable over the
// network (it may listen on the headset's loopback only), the launcher runs
// the system's OpenSSH client (Windows 10/11 include it):
//
//	ssh -T -L 127.0.0.1:<free port>:127.0.0.1:5555 steamos@<frame> "<cmd>"
//
// and then "adb connect 127.0.0.1:<free port>". The remote command lists the
// listening ports (to find Lepton's), prints a marker, and waits on its
// input, so the tunnel ends when the launcher closes the pipe (or dies).
//
// The Developer Mode password: in the window UI it is asked in the launcher
// window and handed to ssh through SSH_ASKPASS: ssh runs this launcher's exe
// again (askpass mode, see runAskpass), which fetches the password once from
// a one-time loopback listener with a random token. It is kept in memory for
// this run only (to restart a dropped tunnel) and never logged or saved.
// ssh without a console (DETACHED_PROCESS on Windows, a new session
// elsewhere) cannot ask on a terminal; SSH_ASKPASS_REQUIRE=force (OpenSSH
// 8.4+) and DISPLAY (older versions) make it use askpass. If askpass is not
// used after all (old or unusual ssh builds), ssh is started again in its own
// console window where the user types the password at ssh's own prompt. In
// --console mode ssh always asks in the launcher's console.

const (
	frameUser        = "steamos"
	sshReadyMarker   = "LAUNCHER_SSH_READY"
	askpassAddrEnv   = "LAUNCHER_ASKPASS_ADDR"
	askpassTokenEnv  = "LAUNCHER_ASKPASS_TOKEN"
	sshAskpassWait   = 30 * time.Second // askpass mode: no askpass request by then -> window
	sshTypeWait      = 3 * time.Minute  // console/window mode: time to type the password
	sshAfterAuthWait = 30 * time.Second
)

// sshRemoteCommand lists listening TCP ports, prints the marker and keeps the
// session open until its input ends.
const sshRemoteCommand = "ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null; echo " + sshReadyMarker + "; cat >/dev/null"

// runAskpass is the askpass mode: ssh started this exe with the prompt as
// argument. It prints the password from the launcher and exits. Returns
// false when the exe was not started as askpass.
func runAskpass() bool {
	addr, token := os.Getenv(askpassAddrEnv), os.Getenv(askpassTokenEnv)
	if addr == "" || token == "" {
		return false
	}
	prompt := strings.Join(os.Args[1:], " ")
	c, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		os.Exit(1)
	}
	c.SetDeadline(time.Now().Add(30 * time.Second))
	fmt.Fprintf(c, "%s\n%s\n", token, strings.ReplaceAll(prompt, "\n", " "))
	reply, err := bufio.NewReader(c).ReadString('\n')
	c.Close()
	if err != nil || !strings.HasPrefix(reply, "OK ") {
		os.Exit(1)
	}
	os.Stdout.WriteString(strings.TrimSuffix(reply[3:], "\n") + "\n")
	os.Exit(0)
	return true
}

// askpassServer answers the askpass helper with the password.
type askpassServer struct {
	ln       net.Listener
	token    string
	password string
	mu       sync.Mutex
	requests int
	prompts  []string
}

func newAskpassServer(password string) (*askpassServer, error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	s := &askpassServer{ln: ln, token: randomToken(), password: password}
	go s.serve()
	return s, nil
}

func (s *askpassServer) serve() {
	for {
		c, err := s.ln.Accept()
		if err != nil {
			return
		}
		go s.handle(c)
	}
}

func (s *askpassServer) handle(c net.Conn) {
	defer c.Close()
	c.SetDeadline(time.Now().Add(10 * time.Second))
	r := bufio.NewReader(c)
	tok, err := r.ReadString('\n')
	if err != nil || subtle.ConstantTimeCompare([]byte(strings.TrimSpace(tok)), []byte(s.token)) != 1 {
		logf("askpass: rejected a request without the right token")
		return
	}
	prompt, _ := r.ReadString('\n')
	prompt = strings.TrimSpace(prompt)
	s.mu.Lock()
	s.requests++
	s.prompts = append(s.prompts, prompt)
	n := s.requests
	s.mu.Unlock()
	if askpassWantsPassword(prompt) {
		logf("askpass request %d: %q -> answering with the password (not logged)", n, prompt)
		fmt.Fprintf(c, "OK %s\n", s.password)
		return
	}
	logf("askpass request %d: %q -> not a password prompt; answering \"no\"", n, prompt)
	fmt.Fprintf(c, "OK no\n")
}

// askpassWantsPassword tells password prompts from others (host key
// questions, key passphrases).
func askpassWantsPassword(prompt string) bool {
	l := strings.ToLower(prompt)
	return strings.Contains(l, "password") && !strings.Contains(l, "passphrase") && !strings.Contains(l, "yes/no")
}

func (s *askpassServer) count() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.requests
}

func (s *askpassServer) close() { s.ln.Close() }

func (s *askpassServer) env() []string {
	return []string{askpassAddrEnv + "=" + s.ln.Addr().String(), askpassTokenEnv + "=" + s.token}
}

// sshTunnel is one running ssh process with a local port forward.
type sshTunnel struct {
	a          *app
	sshPath    string
	sshVersion string
	host       string // name or IP given to ssh
	localPort  int
	remotePort int
	password   string // memory only; "" in console/window mode
	mode       string // askpass, console or window
	acceptNew  bool   // OpenSSH 7.6+: StrictHostKeyChecking=accept-new

	mu       sync.Mutex
	cmd      *exec.Cmd
	stdin    io.WriteCloser
	done     chan struct{}
	exitCode int
	stderr   bytes.Buffer
	stdout   bytes.Buffer
	ready    chan struct{}
	askpass  *askpassServer
	starts   int
}

// sshCandidates lists where ssh may be, in order.
func sshCandidates() []string {
	var c []string
	if runtime.GOOS == "windows" {
		if root := os.Getenv("SystemRoot"); root != "" {
			c = append(c,
				filepath.Join(root, "System32", "OpenSSH", "ssh.exe"),
				filepath.Join(root, "Sysnative", "OpenSSH", "ssh.exe"))
		}
	}
	if p, err := exec.LookPath("ssh"); err == nil {
		c = append(c, p)
	}
	if runtime.GOOS == "windows" {
		for _, env := range []string{"ProgramFiles", "ProgramFiles(x86)"} {
			if d := os.Getenv(env); d != "" {
				c = append(c, filepath.Join(d, "Git", "usr", "bin", "ssh.exe"))
			}
		}
	} else {
		c = append(c, "/usr/bin/ssh")
	}
	return c
}

func findSSH() string {
	for _, p := range sshCandidates() {
		if isFile(p) {
			return p
		}
	}
	return ""
}

var sshVersionRe = regexp.MustCompile(`OpenSSH_(?:for_Windows_)?(\d+)\.(\d+)`)

// parseSSHVersion reads "OpenSSH_for_Windows_8.1p1, LibreSSL 3.0.2" as 8, 1.
func parseSSHVersion(s string) (major, minor int, ok bool) {
	m := sshVersionRe.FindStringSubmatch(s)
	if m == nil {
		return 0, 0, false
	}
	major, _ = strconv.Atoi(m[1])
	minor, _ = strconv.Atoi(m[2])
	return major, minor, true
}

// sshKnownHostsPath is the launcher's own known_hosts file (the user's
// ~/.ssh is not touched). Forward slashes and quotes keep Windows paths
// with spaces working in ssh's option parser.
func sshKnownHostsPath() string {
	dir, err := os.UserCacheDir()
	if err != nil {
		dir = os.TempDir()
	}
	dir = filepath.Join(dir, appName)
	os.MkdirAll(dir, 0o755)
	return filepath.Join(dir, "known_hosts")
}

func sshOptionPath(p string) string {
	p = filepath.ToSlash(p)
	if strings.ContainsAny(p, " \t") {
		p = `"` + p + `"`
	}
	return p
}

// sshArgs builds ssh's arguments. acceptNew is false for OpenSSH before 7.6
// (no "accept-new").
func sshArgs(host string, localPort, remotePort int, knownHosts string, acceptNew bool) []string {
	strict := "accept-new"
	if !acceptNew {
		strict = "no"
	}
	return []string{
		"-T",
		"-o", "ExitOnForwardFailure=yes",
		"-o", "ServerAliveInterval=15",
		"-o", "ServerAliveCountMax=4",
		"-o", "ConnectTimeout=15",
		"-o", "StrictHostKeyChecking=" + strict,
		"-o", "UserKnownHostsFile=" + sshOptionPath(knownHosts),
		"-o", "PubkeyAuthentication=no",
		"-o", "GSSAPIAuthentication=no",
		"-o", "PreferredAuthentications=keyboard-interactive,password",
		"-o", "NumberOfPasswordPrompts=1",
		"-o", "LogLevel=VERBOSE",
		"-L", fmt.Sprintf("127.0.0.1:%d:127.0.0.1:%d", localPort, remotePort),
		"-l", frameUser,
		"-p", "22",
		host,
		sshRemoteCommand,
	}
}

// classifySSH sorts ssh's error output.
func classifySSH(stderr string) string {
	l := strings.ToLower(stderr)
	switch {
	case strings.Contains(l, "could not resolve hostname"):
		return "notfound"
	case strings.Contains(l, "remote host identification has changed"), strings.Contains(l, "host key verification failed"):
		return "hostkey"
	case strings.Contains(l, "permission denied"), strings.Contains(l, "too many authentication failures"),
		strings.Contains(l, "authentication failed"):
		return "sshauth"
	case strings.Contains(l, "address already in use"), strings.Contains(l, "cannot listen to port"),
		strings.Contains(l, "could not request local forwarding"):
		return "localport"
	case strings.Contains(l, "port 22: connection refused"):
		return "sshrefused"
	case strings.Contains(l, "open failed: connect failed"), strings.Contains(l, "open failed: administratively prohibited"):
		return "lepton"
	case strings.Contains(l, "connection refused"):
		return "sshrefused"
	case strings.Contains(l, "no route to host"), strings.Contains(l, "network is unreachable"):
		return "unreachable"
	case strings.Contains(l, "timed out"):
		return "timeout"
	}
	return "other"
}

// sshProblemText explains a failed SSH connection.
func sshProblemText(kind, host string) string {
	switch kind {
	case "notfound":
		return wifiProblemText("notfound", host, 22, false)
	case "sshauth":
		return "The Steam Frame did not accept the password. Enter the password you set\n" +
			"when you turned on Developer Mode (user \"steamos\")."
	case "sshrefused":
		return fmt.Sprintf("%s does not accept SSH connections. Check that Developer Mode is on\n"+
			"for the Steam Frame and that the address is the headset's.", host)
	case "timeout", "unreachable":
		return wifiProblemText("timeout", host, 22, false)
	case "hostkey":
		return fmt.Sprintf("The SSH key of %s changed since the last connection. That happens when\n"+
			"the headset was reset (or the address now belongs to another device).\n"+
			"Choose \"Forget the old key\" only if you are sure it is your Steam Frame.", host)
	case "nossh":
		if runtime.GOOS != "windows" {
			return "The ssh program (OpenSSH client) was not found. Install it, then try again."
		}
		return "Windows' SSH client (ssh.exe) was not found. Install it: Settings > System >\n" +
			"Optional features > Add a feature > \"OpenSSH Client\". Then try again."
	case "lepton":
		return "The SSH connection works, but the Steam Frame's Android layer does not\n" +
			"answer. Start \"Lepton Development\" (Steam Library) on the headset, keep it\n" +
			"running, and try again."
	case "nowindow":
		return "ssh could not ask for the password here. Run the launcher with --console\n" +
			"and try again."
	}
	return "The SSH connection to " + host + " did not work. Details are in the launcher log."
}

// connectSSH asks for the Developer Mode password, starts the tunnel and
// connects adb through it.
func (a *app) connectSSH(hostIn string) error {
	host, _, err := splitHostPort(hostIn, 22)
	if err != nil {
		return &wifiProblem{kind: "notfound", host: hostIn, msg: err.Error()}
	}
	sshPath := findSSH()
	if sshPath == "" {
		logf("ssh not found in: %s", strings.Join(sshCandidates(), "; "))
		return &wifiProblem{kind: "nossh", host: host, msg: sshProblemText("nossh", host)}
	}
	ver := sshVersionString(sshPath)
	logf("ssh: %s (%s)", sshPath, ver)
	ip, rerr := resolveHost(host)
	if rerr != nil {
		return &wifiProblem{kind: "notfound", host: host, msg: sshProblemText("notfound", host)}
	}

	if a.tunnel != nil {
		a.tunnel.stop()
		a.tunnel = nil
	}
	_, console := ui.(*consoleUI)
	mode := "askpass"
	if console {
		mode = "console"
	}
	maj, min, ok := parseSSHVersion(ver)
	t := &sshTunnel{a: a, sshPath: sshPath, sshVersion: ver, host: ip, remotePort: leptonPortFirst,
		mode: mode, acceptNew: !ok || maj > 7 || (maj == 7 && min >= 6)}

	problem := ""
	for {
		if t.mode == "askpass" && t.password == "" {
			pw, err := askSSHPassword(problem)
			if err != nil {
				return err
			}
			t.password = pw
		}
		err := t.start()
		if err == errClosed {
			return err
		}
		var wp *wifiProblem
		if errors.As(err, &wp) {
			switch {
			case wp.kind == "hostkey" && a.askForgetHostKey(host):
				forgetHostKey(sshKnownHostsPath(), ip)
				continue
			case wp.kind == "localport" && t.starts < 4:
				continue
			case wp.kind == "sshauth" && t.mode == "askpass":
				problem = wp.msg // ask again
				continue
			}
		}
		if err != nil {
			t.stop()
			return err
		}
		break
	}

	// Lepton's ports as the headset sees them.
	ports := parseLeptonPorts(t.output())
	logf("Lepton ports listening on the Steam Frame (seen over SSH): %v", ports)
	if len(ports) > 0 && !containsInt(ports, t.remotePort) {
		t.stop()
		t.remotePort = ports[0]
		logf("restarting the SSH tunnel for Lepton port %d", t.remotePort)
		if err := t.start(); err != nil {
			t.stop()
			return err
		}
	}
	// No Lepton port in the list (or no list) is not taken as final: the
	// headset may hide it from ss; adb through the tunnel decides.
	target := net.JoinHostPort("127.0.0.1", strconv.Itoa(t.localPort))
	if err := a.netConnect(target, 20*time.Second); err != nil {
		if err == errClosed {
			t.stop()
			return err
		}
		logf("adb through the SSH tunnel failed: %v; ssh said:\n%s", err, t.errText())
		var wp *wifiProblem
		kind := "lepton"
		if errors.As(err, &wp) && wp.kind == "auth" {
			kind = "auth"
		}
		t.stop()
		msg := sshProblemText(kind, host)
		if kind == "auth" {
			msg = wifiProblemText("auth", host, 0, true)
		}
		return &wifiProblem{kind: kind, host: host, msg: msg}
	}
	a.tunnel = t
	a.lepton = target
	a.netHost = host
	say("Connected to the Steam Frame through SSH (tunnel on port %d).", t.localPort)
	return nil
}

// askSSHPassword asks for the Developer Mode password in the launcher
// window. The answer is never logged (Question.Secret).
func askSSHPassword(problem string) (string, error) {
	ans := ui.Ask(Question{
		Kind:  "text",
		Title: "Connect through SSH",
		Text: "The launcher connects to the Steam Frame over SSH (user \"steamos\") and\n" +
			"reaches its Android layer through that connection. \"Lepton Development\"\n" +
			"must be running on the headset.\n\n" +
			"Enter the Developer Mode password (the one you set when you turned on\n" +
			"Developer Mode). It is used for this connection only and is not saved.",
		Problem:     problem,
		Secret:      true,
		Placeholder: "Developer Mode password",
		Options: []Option{
			{ID: "connect", Label: "Connect", Style: "primary"},
			{ID: "back", Label: "Back"},
		},
		Default: "back",
	})
	if ans.Closed && appCtx.Err() != nil {
		return "", errClosed
	}
	if ans.ID != "connect" {
		return "", errBackToUSB
	}
	if ans.Text == "" {
		return askSSHPassword("Please enter the password.")
	}
	return ans.Text, nil
}

func containsInt(list []int, v int) bool {
	for _, x := range list {
		if x == v {
			return true
		}
	}
	return false
}

func sshVersionString(sshPath string) string {
	cmd := exec.Command(sshPath, "-V")
	noConsoleWindow(cmd)
	out, err := cmd.CombinedOutput()
	if err != nil && len(out) == 0 {
		return "unknown (" + err.Error() + ")"
	}
	return strings.TrimSpace(string(out))
}

func (a *app) askForgetHostKey(host string) bool {
	ans := ui.Ask(Question{Kind: "buttons", Error: true, Title: "The headset's SSH key changed",
		Text: sshProblemText("hostkey", host),
		Options: []Option{
			{ID: "forget", Label: "Forget the old key", Key: "f"},
			{ID: "cancel", Label: "Cancel", Key: "", Style: "primary"},
		}, Default: "cancel"})
	return ans.ID == "forget"
}

// forgetHostKey removes the lines for host from the launcher's own
// known_hosts file.
func forgetHostKey(path, host string) {
	data, err := os.ReadFile(path)
	if err != nil {
		return
	}
	var keep []string
	for _, l := range strings.Split(string(data), "\n") {
		f := strings.Fields(l)
		if len(f) > 0 && knownHostsLineMatches(f[0], host) {
			continue
		}
		keep = append(keep, l)
	}
	os.WriteFile(path, []byte(strings.Join(keep, "\n")), 0o600)
	logf("forgot the SSH key of %s in %s", host, path)
}

func knownHostsLineMatches(field, host string) bool {
	for _, h := range strings.Split(field, ",") {
		if h == host || h == "["+host+"]:22" {
			return true
		}
	}
	return false
}

// freeLocalPort asks the system for an unused local port.
func freeLocalPort() (int, error) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return 0, err
	}
	defer ln.Close()
	return ln.Addr().(*net.TCPAddr).Port, nil
}

func (t *sshTunnel) output() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.stdout.String()
}

func (t *sshTunnel) errText() string {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.stderr.String()
}

func (t *sshTunnel) running() bool {
	t.mu.Lock()
	done := t.done
	t.mu.Unlock()
	if done == nil {
		return false
	}
	select {
	case <-done:
		return false
	default:
		return true
	}
}

// lineWriter collects output, logs each line and watches for the marker.
type lineWriter struct {
	t      *sshTunnel
	buf    *bytes.Buffer
	name   string
	part   []byte
	marker func()
}

func (w *lineWriter) Write(b []byte) (int, error) {
	w.t.mu.Lock()
	w.buf.Write(b)
	w.t.mu.Unlock()
	w.part = append(w.part, b...)
	for {
		i := bytes.IndexByte(w.part, '\n')
		if i < 0 {
			break
		}
		line := strings.TrimRight(string(w.part[:i]), "\r")
		w.part = w.part[i+1:]
		logf("ssh %s: %s", w.name, line)
		if w.marker != nil && strings.TrimSpace(line) == sshReadyMarker {
			w.marker()
		}
	}
	return len(b), nil
}

// start runs ssh and waits until the remote command printed the marker
// (logged in, port forward set up).
func (t *sshTunnel) start() error {
	for {
		err := t.startOnce()
		var wp *wifiProblem
		if t.mode == "askpass" && errors.As(err, &wp) && wp.kind == "noaskpass" {
			if runtime.GOOS != "windows" {
				return &wifiProblem{kind: "nowindow", host: t.host, msg: sshProblemText("nowindow", t.host)}
			}
			logf("ssh did not use askpass; asking for the password in a console window instead")
			t.mode, t.password = "window", ""
			continue
		}
		return err
	}
}

func (t *sshTunnel) startOnce() error {
	port, err := freeLocalPort()
	if err != nil {
		return fmt.Errorf("no free local port: %v", err)
	}
	t.localPort = port
	t.starts++
	args := sshArgs(t.host, t.localPort, t.remotePort, sshKnownHostsPath(), t.acceptNew)
	cmd := exec.Command(t.sshPath, args...)
	env := os.Environ()
	switch t.mode {
	case "askpass":
		srv, err := newAskpassServer(t.password)
		if err != nil {
			return fmt.Errorf("askpass: %v", err)
		}
		t.askpass = srv
		exe, _ := os.Executable()
		env = append(env, srv.env()...)
		env = append(env, "SSH_ASKPASS="+exe, "SSH_ASKPASS_REQUIRE=force")
		if os.Getenv("DISPLAY") == "" {
			env = append(env, "DISPLAY=localhost:0")
		}
	default:
		var clean []string
		for _, e := range env {
			if !strings.HasPrefix(e, "SSH_ASKPASS") {
				clean = append(clean, e)
			}
		}
		env = clean
	}
	cmd.Env = env
	sshProcAttr(cmd, t.mode)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return err
	}
	ready := make(chan struct{})
	var once sync.Once
	t.mu.Lock()
	t.stdout.Reset()
	t.stderr.Reset()
	t.mu.Unlock()
	cmd.Stdout = &lineWriter{t: t, buf: &t.stdout, name: "out", marker: func() { once.Do(func() { close(ready) }) }}
	cmd.Stderr = &lineWriter{t: t, buf: &t.stderr, name: "err"}
	cmd.WaitDelay = 3 * time.Second
	logf("starting ssh (%s mode, try %d): %s %s", t.mode, t.starts, t.sshPath, strings.Join(quoteForLog(args), " "))
	if err := cmd.Start(); err != nil {
		t.closeAskpass()
		return fmt.Errorf("could not start ssh: %v", err)
	}
	done := make(chan struct{})
	t.mu.Lock()
	t.cmd, t.stdin, t.done, t.ready = cmd, stdin, done, ready
	t.mu.Unlock()
	started := time.Now()
	go func() {
		err := cmd.Wait()
		code := 0
		if cmd.ProcessState != nil {
			code = cmd.ProcessState.ExitCode()
		}
		t.mu.Lock()
		t.exitCode = code
		t.mu.Unlock()
		logf("ssh (port %d) ended after %.0f s: exit code %d (%v)", t.localPort, time.Since(started).Seconds(), code, err)
		close(done)
	}()

	switch t.mode {
	case "window":
		say("A black window opened for SSH. Type the Developer Mode password there\n" +
			"(nothing is shown while typing) and press Enter. Then leave that window\n" +
			"open (you can minimise it) while the launcher works.")
	case "console":
		say("Type the Steam Frame's Developer Mode password at ssh's prompt below\n" +
			"(nothing is shown while typing) and press Enter.")
	default:
		say("Connecting to the Steam Frame over SSH...")
	}
	wait := sshTypeWait
	if t.mode == "askpass" {
		wait = sshAskpassWait
	}
	deadline := time.Now().Add(wait)
	authed := false
	for {
		select {
		case <-ready:
			t.closeAskpass()
			logf("SSH tunnel ready: 127.0.0.1:%d -> %s 127.0.0.1:%d", t.localPort, t.host, t.remotePort)
			return nil
		case <-done:
			n := t.askpassCount()
			t.closeAskpass()
			stderr := t.errText()
			kind := classifySSH(stderr)
			logf("ssh failed (%s, askpass requests %d)", kind, n)
			if t.mode == "askpass" && n == 0 && (kind == "sshauth" || kind == "other") {
				return &wifiProblem{kind: "noaskpass", host: t.host}
			}
			if t.mode == "askpass" && kind == "sshauth" {
				t.password = "" // ask again next time
			}
			if kind == "other" {
				return &wifiProblem{kind: kind, host: t.host, msg: sshProblemText(kind, t.host) +
					"\n\nssh said: " + lastLines(stderr, 3)}
			}
			return &wifiProblem{kind: kind, host: t.host, msg: sshProblemText(kind, t.host)}
		case <-appCtx.Done():
			t.stop()
			return errClosed
		case <-time.After(500 * time.Millisecond):
		}
		if !authed && strings.Contains(t.errText(), "Authenticated to") {
			authed = true
			deadline = time.Now().Add(sshAfterAuthWait)
		}
		if t.mode == "askpass" && !authed && t.askpassCount() > 0 && wait == sshAskpassWait {
			wait = sshAfterAuthWait
			deadline = time.Now().Add(wait)
		}
		if time.Now().After(deadline) {
			n := t.askpassCount()
			logf("ssh: no tunnel after %s (askpass requests %d, authenticated %v)", wait, n, authed)
			t.stop()
			if t.mode == "askpass" && n == 0 && !authed {
				return &wifiProblem{kind: "noaskpass", host: t.host}
			}
			if t.mode != "askpass" && !authed {
				return &wifiProblem{kind: "other", host: t.host, msg: "No password was entered in time."}
			}
			return &wifiProblem{kind: "other", host: t.host, msg: sshProblemText("other", t.host)}
		}
	}
}

func (t *sshTunnel) askpassCount() int {
	if t.askpass == nil {
		return 0
	}
	return t.askpass.count()
}

func (t *sshTunnel) closeAskpass() {
	if t.askpass != nil {
		t.askpass.close()
		t.askpass = nil
	}
}

// restart starts a dropped tunnel again on the same local port's
// replacement; a.adb.Serial is updated to the new port.
func (t *sshTunnel) restart() error {
	if t.mode == "askpass" && t.password == "" {
		return errors.New("no password for the restart")
	}
	old := t.localPort
	if err := t.start(); err != nil {
		return err
	}
	if t.localPort != old && t.a != nil {
		oldSerial := net.JoinHostPort("127.0.0.1", strconv.Itoa(old))
		newSerial := net.JoinHostPort("127.0.0.1", strconv.Itoa(t.localPort))
		c := *t.a.adb
		c.Serial = ""
		c.run(10*time.Second, "disconnect", oldSerial)
		if t.a.adb.Serial == oldSerial {
			t.a.adb.Serial = newSerial
		}
		if t.a.lepton == oldSerial {
			t.a.lepton = newSerial
		}
		logf("SSH tunnel restarted: adb now uses %s", newSerial)
	}
	return nil
}

// stop ends ssh: closing its input ends the remote command; it is killed if
// it does not exit within 3 s.
func (t *sshTunnel) stop() {
	t.mu.Lock()
	cmd, stdin, done := t.cmd, t.stdin, t.done
	t.mu.Unlock()
	t.closeAskpass()
	if cmd == nil || done == nil {
		return
	}
	select {
	case <-done:
		return
	default:
	}
	if stdin != nil {
		stdin.Close()
	}
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		logf("ssh did not end by itself; stopping it")
		cmd.Process.Kill()
		select {
		case <-done:
		case <-time.After(3 * time.Second):
		}
	}
}

func lastLines(s string, n int) string {
	lines := strings.Split(strings.TrimSpace(s), "\n")
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return strings.Join(lines, "\n")
}
