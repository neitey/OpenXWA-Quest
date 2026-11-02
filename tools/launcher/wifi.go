package main

import (
	"errors"
	"fmt"
	"net"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Wi-Fi connections. The connection step (chooseRoute) comes first for every
// headset, before the launcher waits for USB:
//
//	usb      wait for a USB device (the Steam Frame then uses its Lepton flow)
//	usbwifi  Quest/Pico: wait for USB, then "adb tcpip 5555" and continue over
//	         Wi-Fi so the cable can be unplugged (until the headset sleeps or
//	         restarts)
//	wifi     "adb connect <host>:5555": the Steam Frame's Android layer
//	         (Lepton Development), or a Quest/Pico whose Wi-Fi debugging is
//	         already on. If nothing answers there, a Steam Frame can be reached
//	         through an SSH tunnel (sshtunnel.go).
//
// Every network serial ("host:port") is reconnected by reconnectNet when the
// connection drops during a copy.

const (
	routeUSB     = "usb"
	routeUSBWiFi = "usbwifi"
	routeWiFi    = "wifi"
	routeFrame   = "frame" // Steam Frame: into its Steam Library over SSH (steamlib.go)
	adbTCPPort   = 5555
)

// errBackToUSB: the user left the Wi-Fi flow to use the USB cable.
var errBackToUSB = errors.New("use the USB cable")

// launcherSettings loads launcher.ini next to the exe.
func (a *app) launcherSettings() *settings {
	return loadSettings(filepath.Join(a.exeDir, settingsFileName))
}

func (a *app) saveSetting(key, value string) {
	if err := a.launcherSettings().set(key, value); err != nil {
		logf("could not save %s in %s: %v", key, settingsFileName, err)
	}
}

// routeQuestion is the connection step's card.
func routeQuestion(last string) Question {
	if last != routeUSBWiFi && last != routeWiFi && last != routeFrame {
		last = routeUSB
	}
	opts := []Option{
		{ID: routeFrame, Label: "Steam Frame (into its Steam Library)", Key: "f"},
		{ID: routeUSB, Label: "USB cable", Key: "u"},
		{ID: routeUSBWiFi, Label: "USB, then switch to Wi-Fi", Key: "s"},
		{ID: routeWiFi, Label: "Wi-Fi", Key: "w"},
	}
	for i := range opts {
		if opts[i].ID == last {
			opts[i].Key, opts[i].Style = "", "primary" // Enter in the console
		}
	}
	return Question{
		Kind:  "buttons",
		Title: "How should the launcher reach the headset?",
		Text: "Steam Frame: choose \"Steam Frame\". The game goes into the headset's Steam\n" +
			"Library over Wi-Fi (SSH), nothing is deleted when it closes, and \"Lepton\n" +
			"Development\" is not needed. (Wi-Fi and USB reach Lepton Development instead.)\n\n" +
			"Meta Quest and Pico: choose USB cable. To unplug the cable while the files\n" +
			"are copied, choose \"USB, then switch to Wi-Fi\": Wi-Fi works only after a\n" +
			"USB connection, and lasts until the headset sleeps or restarts.\n\n" +
			"Developer Mode must be on for every headset.",
		Options: opts,
		Default: last,
	}
}

// chooseRoute asks how to connect and remembers the answer.
func (a *app) chooseRoute() (string, error) {
	last := a.launcherSettings().get("connection", routeUSB)
	ans := ui.Ask(routeQuestion(last))
	if ans.Closed && appCtx.Err() != nil {
		return "", errClosed
	}
	route := ans.ID
	if route != routeUSBWiFi && route != routeWiFi && route != routeFrame {
		route = routeUSB
	}
	logf("connection route chosen: %s (last time: %s)", route, last)
	if route != last {
		a.saveSetting("connection", route)
	}
	return route, nil
}

// wifiHostQuestion asks for the headset's name or IP address.
func wifiHostQuestion(value, problem string) Question {
	return Question{
		Kind:  "text",
		Title: "Connect over Wi-Fi",
		Text: "Enter the headset's name or IP address. The PC and the headset must be on\n" +
			"the same network.\n\n" +
			"Steam Frame: \"frame\" usually works; the IP address is in Quick Settings or\n" +
			"in Steam Settings > Internet. Start \"Lepton Development\" (Steam Library) on\n" +
			"the headset first and keep it running. Once, set its Launch Options\n" +
			"(Steam Library > Lepton Development > Properties) to:\n" +
			"    LEPTON_NO_CLEANUP=1 %command%\n" +
			"Without this, closing Lepton Development deletes the app and the copied\n" +
			"game files. (If it is running already, close it first so the setting applies.)\n\n" +
			"Meta Quest / Pico: Wi-Fi works only after a USB connection. Use \"USB, then\n" +
			"switch to Wi-Fi\" instead, unless Wi-Fi debugging is already on.",
		Problem:     problem,
		Value:       value,
		Placeholder: "frame or 192.168.1.23",
		Options: []Option{
			{ID: "connect", Label: "Connect", Style: "primary"},
			{ID: "usb", Label: "Use the USB cable instead"},
		},
		Default:       "connect",
		ConsolePrompt: "Headset name or IP address (Enter = " + value + ", u = USB cable instead):",
	}
}

// askWiFiHost returns the host the user entered, or errBackToUSB.
func (a *app) askWiFiHost(problem string) (string, error) {
	value := a.launcherSettings().get("wifi_host", "")
	if value == "" {
		value = "frame"
	}
	ans := ui.Ask(wifiHostQuestion(value, problem))
	if ans.Closed && appCtx.Err() != nil {
		return "", errClosed
	}
	host := strings.TrimSpace(ans.Text)
	if ans.ID == "usb" || strings.EqualFold(host, "u") || strings.EqualFold(host, "usb") {
		logf("Wi-Fi: the user chose the USB cable instead")
		return "", errBackToUSB
	}
	if host == "" {
		host = value
	}
	logf("Wi-Fi host entered: %q", host)
	return host, nil
}

// wifiFlow connects over Wi-Fi, with a card for every problem. It returns
// nil when a.adb.Serial is the headset's network serial, errBackToUSB when
// the user wants the cable, or errClosed.
func (a *app) wifiFlow() error {
	problem := ""
	host := ""
	for {
		if host == "" {
			h, err := a.askWiFiHost(problem)
			if err != nil {
				return err
			}
			host = h
		}
		err := a.connectWiFi(host)
		if err == nil {
			a.saveSetting("wifi_host", host)
			return nil
		}
		if err == errClosed {
			return err
		}
		logf("Wi-Fi connection to %s failed: %v", host, err)
		var wp *wifiProblem
		if !errors.As(err, &wp) {
			wp = &wifiProblem{kind: "other", host: host, msg: err.Error()}
		}
		switch a.wifiProblemCard(wp) {
		case "retry":
		case "host":
			problem, host = wp.msg, ""
		case "ssh":
			err := a.connectSSH(host)
			if err == nil {
				a.saveSetting("wifi_host", host)
				return nil
			}
			if err == errClosed {
				return err
			}
			if err == errBackToUSB {
				continue
			}
			logf("SSH connection to %s failed: %v", host, err)
			if errors.As(err, &wp) && wp.kind == "notfound" {
				problem, host = wp.msg, ""
				continue
			}
			if a.sshProblemCard(err) == "usb" {
				return errBackToUSB
			}
		case "usb":
			return errBackToUSB
		default:
			return errClosed
		}
	}
}

// wifiProblem is a failed Wi-Fi or SSH connection with a kind for the
// message: notfound, refused, timeout, unreachable, auth, noadb, other and
// for SSH also sshauth, sshrefused, hostkey, nossh, lepton.
type wifiProblem struct {
	kind string
	host string
	msg  string
	ssh  bool // port 22 answers: probably a Steam Frame
}

func (e *wifiProblem) Error() string { return e.msg }

// wifiProblemText explains a failed direct Wi-Fi connection.
func wifiProblemText(kind, host string, port int, sshOpen bool) string {
	hp := net.JoinHostPort(host, strconv.Itoa(port))
	switch kind {
	case "notfound":
		return fmt.Sprintf("The name %q was not found on the network. Enter the headset's IP\n"+
			"address instead (Steam Frame: Quick Settings or Steam Settings > Internet;\n"+
			"Quest: Settings > Wi-Fi > your network). Also check that no other device\n"+
			"on the network has the same name.", host)
	case "refused":
		s := fmt.Sprintf("The headset at %s answered, but nothing accepts adb connections on\n"+
			"port %d.\n\n", host, port)
		if sshOpen {
			s += "This looks like a Steam Frame (its SSH service answers). Make sure \"Lepton\n" +
				"Development\" is running on the headset, then try again. If it is running,\n" +
				"its adb port may only be reachable on the headset itself: choose\n" +
				"\"Connect through SSH\"."
		} else {
			s += "Steam Frame: start \"Lepton Development\" (Steam Library) and keep it running,\n" +
				"or choose \"Connect through SSH\".\n" +
				"Meta Quest / Pico: Wi-Fi debugging is off (it ends when the headset sleeps or\n" +
				"restarts). Use the USB cable once: choose \"USB, then switch to Wi-Fi\"."
		}
		return s
	case "timeout", "unreachable":
		return fmt.Sprintf("No answer from %s.\n\n"+
			"Check that the headset is awake and that the PC and the headset are on the\n"+
			"same network. Some routers keep devices apart (\"client isolation\" or \"AP\n"+
			"isolation\", often on guest networks); then use the IP address on a normal\n"+
			"network, or the USB cable.", hp)
	case "auth":
		return "The headset did not allow this computer. Put it on and choose Allow in the\n" +
			"\"Allow USB debugging?\" prompt (on Steam Frame it appears in the Lepton\n" +
			"window), tick \"Always allow from this computer\", then try again."
	case "noadb":
		return fmt.Sprintf("Something answers at %s, but it does not work like an Android\n"+
			"headset (adb). Check the address, and on Steam Frame that \"Lepton\n"+
			"Development\" is running.", hp)
	}
	return fmt.Sprintf("Could not connect to %s.", hp)
}

// wifiProblemCard shows a failed Wi-Fi connection and returns retry, host,
// ssh or usb.
func (a *app) wifiProblemCard(wp *wifiProblem) string {
	opts := []Option{{ID: "retry", Label: "Try again", Key: "r", Style: "primary"}}
	if wp.kind != "notfound" {
		ssh := Option{ID: "ssh", Label: "Connect through SSH (Steam Frame)", Key: "s"}
		if wp.ssh && wp.kind == "refused" {
			ssh.Style, opts[0].Style = "primary", ""
			opts = append([]Option{ssh}, opts...)
		} else {
			opts = append(opts, ssh)
		}
	}
	opts = append(opts,
		Option{ID: "host", Label: "Change the address", Key: "a"},
		Option{ID: "usb", Label: "Use the USB cable", Key: "u"})
	ans := ui.Ask(Question{Kind: "buttons", Error: true, Title: "The Wi-Fi connection did not work",
		Text: wp.msg, Options: opts, Default: opts[0].ID})
	if ans.Closed && appCtx.Err() != nil {
		return "close"
	}
	logf("Wi-Fi problem card (%s): %s", wp.kind, ans.ID)
	return ans.ID
}

// sshProblemCard shows a failed SSH connection; returns retry or usb.
func (a *app) sshProblemCard(err error) string {
	ans := ui.Ask(Question{Kind: "buttons", Error: true, Title: "The SSH connection did not work",
		Text: err.Error(),
		Options: []Option{
			{ID: "retry", Label: "Back", Key: "", Style: "primary"},
			{ID: "usb", Label: "Use the USB cable", Key: "u"},
		}, Default: "retry"})
	if ans.Closed && appCtx.Err() != nil {
		return "usb"
	}
	return ans.ID
}

// splitHostPort accepts "frame", "frame:5556", "192.168.1.5",
// "192.168.1.5:5555", "[fe80::1]:5555" and "fe80::1".
func splitHostPort(s string, defPort int) (host string, port int, err error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return "", 0, errors.New("empty address")
	}
	if strings.ContainsAny(s, " \t/\\@") {
		return "", 0, fmt.Errorf("%q is not a host name or IP address", s)
	}
	if h, p, e := net.SplitHostPort(s); e == nil {
		n, perr := strconv.Atoi(p)
		if perr != nil || n < 1 || n > 65535 || h == "" {
			return "", 0, fmt.Errorf("%q has no valid port", s)
		}
		return h, n, nil
	}
	if strings.Count(s, ":") > 1 { // a bare IPv6 address
		s = strings.Trim(s, "[]")
		if net.ParseIP(s) == nil {
			return "", 0, fmt.Errorf("%q is not a valid address", s)
		}
		return s, defPort, nil
	}
	if strings.Contains(s, ":") {
		return "", 0, fmt.Errorf("%q has no valid port", s)
	}
	return s, defPort, nil
}

// isNetSerial reports adb serials of TCP connections ("host:port"), not
// USB serials or emulators.
func isNetSerial(serial string) bool {
	if serial == "" || strings.HasPrefix(serial, "emulator-") {
		return false
	}
	_, _, err := net.SplitHostPort(serial)
	return err == nil
}

// lookupHost resolves a name the way adb would, preferring IPv4 (link-local
// IPv6 addresses from mDNS often do not work for adb).
var lookupHost = net.LookupHost

func resolveHost(host string) (string, error) {
	if ip := net.ParseIP(host); ip != nil {
		return host, nil
	}
	addrs, err := lookupHost(host)
	if err != nil || len(addrs) == 0 {
		logf("could not resolve %q: %v", host, err)
		return "", fmt.Errorf("not found")
	}
	logf("%s resolves to %v", host, addrs)
	return pickAddr(addrs), nil
}

func pickAddr(addrs []string) string {
	for _, a := range addrs {
		if ip := net.ParseIP(a); ip != nil && ip.To4() != nil {
			return a
		}
	}
	for _, a := range addrs {
		if ip := net.ParseIP(a); ip != nil && !ip.IsLinkLocalUnicast() {
			return a
		}
	}
	return addrs[0]
}

// probeTCP dials host:port and returns open, refused, timeout or
// unreachable.
func probeTCP(host string, port int, timeout time.Duration) string {
	c, err := net.DialTimeout("tcp", net.JoinHostPort(host, strconv.Itoa(port)), timeout)
	if err == nil {
		c.Close()
		return "open"
	}
	return dialErrorKind(err.Error())
}

// dialErrorKind sorts a dial or adb connect error text (Windows and Linux
// wordings).
func dialErrorKind(msg string) string {
	l := strings.ToLower(msg)
	switch {
	case strings.Contains(l, "refused"), strings.Contains(l, "10061"):
		return "refused"
	case strings.Contains(l, "no such host"), strings.Contains(l, "cannot resolve"),
		strings.Contains(l, "failed to resolve"), strings.Contains(l, "11001"),
		strings.Contains(l, "name or service not known"), strings.Contains(l, "nodename nor servname"):
		return "notfound"
	case strings.Contains(l, "unreachable"), strings.Contains(l, "no route"),
		strings.Contains(l, "10051"), strings.Contains(l, "10065"):
		return "unreachable"
	case strings.Contains(l, "timed out"), strings.Contains(l, "timeout"),
		strings.Contains(l, "10060"), strings.Contains(l, "did not properly respond"):
		return "timeout"
	case strings.Contains(l, "authenticate"), strings.Contains(l, "unauthorized"):
		return "auth"
	}
	return "other"
}

// adbConnectOK reports whether "adb connect" says it is connected.
func adbConnectOK(out string) bool {
	l := strings.ToLower(out)
	return strings.Contains(l, "connected to") && !strings.Contains(l, "failed") &&
		!strings.Contains(l, "cannot") && !strings.Contains(l, "unable")
}

// scanPorts returns which of the ports accept TCP connections, in order;
// all are tried at once with a short timeout.
func scanPorts(host string, ports []int, timeout time.Duration) []int {
	var mu sync.Mutex
	var wg sync.WaitGroup
	var open []int
	for _, p := range ports {
		wg.Add(1)
		go func(p int) {
			defer wg.Done()
			if probeTCP(host, p, timeout) == "open" {
				mu.Lock()
				open = append(open, p)
				mu.Unlock()
			}
		}(p)
	}
	wg.Wait()
	sort.Ints(open)
	return open
}

func portRange(first, last int) []int {
	var out []int
	for p := first; p <= last; p++ {
		out = append(out, p)
	}
	return out
}

// connectWiFi connects to an adb port of host directly. On success
// a.adb.Serial is the network serial.
func (a *app) connectWiFi(hostIn string) error {
	host, port, err := splitHostPort(hostIn, adbTCPPort)
	if err != nil {
		return &wifiProblem{kind: "notfound", host: hostIn, msg: fmt.Sprintf("%v. Enter the headset's name or IP address.", err)}
	}
	say("Connecting to %s over Wi-Fi...", hostIn)
	ip, err := resolveHost(host)
	if err != nil {
		return &wifiProblem{kind: "notfound", host: host, msg: wifiProblemText("notfound", host, port, false)}
	}
	sshOpen := probeTCP(ip, 22, 3*time.Second) == "open"
	res := probeTCP(ip, port, 5*time.Second)
	logf("Wi-Fi probe %s (%s) port %d: %s; ssh port 22 open: %v", host, ip, port, res, sshOpen)
	ports := []int{}
	if res == "open" {
		ports = append(ports, port)
	}
	if res != "open" && port == adbTCPPort && (res == "refused" || sshOpen) {
		// Other Lepton instances listen on 5556-5599: cheap to look at all
		// at once.
		extra := scanPorts(ip, portRange(leptonPortFirst+1, leptonPortLast), 1500*time.Millisecond)
		logf("open ports in %d-%d on %s: %v", leptonPortFirst+1, leptonPortLast, ip, extra)
		ports = append(ports, extra...)
	}
	if len(ports) == 0 {
		kind := res
		if kind == "other" {
			kind = "timeout"
		}
		return &wifiProblem{kind: kind, host: host, ssh: sshOpen, msg: wifiProblemText(kind, host, port, sshOpen)}
	}
	var last error
	for _, p := range ports {
		target := net.JoinHostPort(ip, strconv.Itoa(p))
		err := a.netConnect(target, 20*time.Second)
		if err == nil {
			a.netHost = host
			frame := sshOpen || strings.EqualFold(host, "frame")
			logf("connected over Wi-Fi: %s (%s), Steam Frame: %v", hostIn, target, frame)
			if frame {
				a.lepton = target
			}
			return nil
		}
		if err == errClosed {
			return err
		}
		last = err
	}
	var wp *wifiProblem
	if errors.As(last, &wp) {
		wp.host, wp.ssh = host, sshOpen
		if wp.kind == "noadb" || wp.kind == "refused" {
			wp.msg = wifiProblemText(wp.kind, host, ports[0], sshOpen)
		}
		return wp
	}
	return &wifiProblem{kind: "other", host: host, ssh: sshOpen, msg: last.Error()}
}

// netConnect runs "adb connect target" until target answers as an Android
// device. An unauthorized connection waits (90 s) for the user to allow it
// on the headset. On success a.adb.Serial is target.
func (a *app) netConnect(target string, wait time.Duration) error {
	c := *a.adb
	c.Serial = ""
	deadline := time.Now().Add(wait)
	shownAuth := false
	lastOut := ""
	for attempt := 1; ; attempt++ {
		out, err := c.run(20*time.Second, "connect", target)
		if err == errClosed {
			return err
		}
		lastOut = out
		connected := adbConnectOK(out)
		logf("adb connect %s (attempt %d): connected=%v", target, attempt, connected)
		if connected {
			l := *a.adb
			l.Serial = target
			sdk, _ := l.shell(20*time.Second, "getprop ro.build.version.sdk")
			if _, perr := strconv.Atoi(firstLine(sdk)); perr == nil {
				a.adb.Serial = target
				logf("connected to %s (Android SDK %s)", target, firstLine(sdk))
				return nil
			}
			ls := strings.ToLower(sdk)
			if strings.Contains(ls, "unauthorized") || strings.Contains(ls, "authoriz") {
				if !shownAuth {
					say("Put on the headset and choose Allow in the \"Allow USB debugging?\" prompt\n" +
						"(on Steam Frame it appears in the Lepton window). Tick \"Always allow from\n" +
						"this computer\".")
					shownAuth = true
					deadline = time.Now().Add(90 * time.Second)
				}
			} else if !strings.Contains(ls, "error") && attempt >= 3 {
				// A shell answered, but not Android's (adb errors such as
				// "offline" or "closed" right after allowing are waited out).
				c.run(10*time.Second, "disconnect", target)
				return &wifiProblem{kind: "noadb", msg: "no Android answer at " + target}
			}
		}
		if time.Now().After(deadline) {
			c.run(10*time.Second, "disconnect", target)
			if shownAuth {
				return &wifiProblem{kind: "auth", msg: wifiProblemText("auth", target, 0, false)}
			}
			kind := dialErrorKind(lastOut)
			logf("adb connect %s gave up: %s (%s)", target, strings.TrimSpace(lastOut), kind)
			switch kind {
			case "refused", "timeout", "unreachable", "notfound":
			default:
				kind = "noadb"
			}
			return &wifiProblem{kind: kind, msg: fmt.Sprintf("The headset did not answer at %s (%s).", target, connectReason(lastOut))}
		}
		if !sleepCtx(2 * time.Second) {
			return errClosed
		}
	}
}

var (
	inetRe     = regexp.MustCompile(`\binet\s+(\d+\.\d+\.\d+\.\d+)(?:/\d+)?`)
	routeSrcRe = regexp.MustCompile(`\bdev\s+(\S+).*\bsrc\s+(\d+\.\d+\.\d+\.\d+)`)
)

// parseWlanIP finds the headset's Wi-Fi IPv4 address in the output of
// "ip -f inet addr show wlan0" and/or "ip route" (a wlan line wins; any
// other non-loopback address is the fallback).
func parseWlanIP(out string) string {
	var fallback string
	iface := ""
	for _, line := range strings.Split(out, "\n") {
		t := strings.TrimSpace(line)
		// "3: wlan0: <BROADCAST,...>" starts an interface block.
		if f := strings.Fields(t); len(f) >= 2 && strings.HasSuffix(f[0], ":") && strings.HasSuffix(f[1], ":") {
			if _, err := strconv.Atoi(strings.TrimSuffix(f[0], ":")); err == nil {
				iface = strings.TrimSuffix(f[1], ":")
				if i := strings.IndexByte(iface, '@'); i >= 0 {
					iface = iface[:i]
				}
			}
		}
		if m := routeSrcRe.FindStringSubmatch(t); m != nil {
			if strings.HasPrefix(m[1], "wlan") && !strings.HasPrefix(m[2], "127.") {
				return m[2]
			}
			if fallback == "" && m[1] != "lo" && !strings.HasPrefix(m[2], "127.") {
				fallback = m[2]
			}
			continue
		}
		if m := inetRe.FindStringSubmatch(t); m != nil && !strings.HasPrefix(m[1], "127.") {
			if strings.HasPrefix(iface, "wlan") || strings.HasSuffix(t, "wlan0") {
				return m[1]
			}
			if fallback == "" && iface != "lo" {
				fallback = m[1]
			}
		}
	}
	return fallback
}

// switchToWiFi moves a USB-connected Quest/Pico to Wi-Fi (adb tcpip) so the
// cable can be unplugged. On failure the USB connection stays in use.
func (a *app) switchToWiFi() error {
	usb := a.adb.Serial
	out, err := a.adb.shell(20*time.Second, "ip -f inet addr show wlan0 2>/dev/null; ip route 2>/dev/null")
	if err == errClosed {
		return err
	}
	ip := parseWlanIP(out)
	logf("headset Wi-Fi address: %q", ip)
	if ip == "" {
		return fail("The headset's Wi-Fi address was not found. Is the headset connected to\n" +
			"Wi-Fi? The launcher stays on the USB cable.")
	}
	say("Switching to Wi-Fi (%s)...", ip)
	if out, err := a.adb.run(30*time.Second, "tcpip", strconv.Itoa(adbTCPPort)); err != nil {
		if err == errClosed {
			return err
		}
		return fail("The headset did not switch to Wi-Fi (%s). The launcher stays on the USB\ncable.", firstLine(out))
	}
	if !sleepCtx(3 * time.Second) {
		return errClosed
	}
	target := net.JoinHostPort(ip, strconv.Itoa(adbTCPPort))
	if err := a.netConnect(target, 30*time.Second); err != nil {
		if err == errClosed {
			return err
		}
		a.adb.Serial = usb
		// adbd restarted for tcpip; give the USB connection a moment.
		a.waitUSBBack(usb)
		return fail("Wi-Fi did not work (%v).\nThe PC and the headset must be on the same network. The launcher stays on\nthe USB cable.", err)
	}
	a.netHost = ip
	a.saveSetting("wifi_host", ip)
	say("Connected over Wi-Fi (%s). You can unplug the USB cable now.\n"+
		"Wi-Fi debugging lasts until the headset sleeps or restarts: keep it awake\n"+
		"while the files are copied (for example keep it on your head, or cover the\n"+
		"sensor between the lenses).", ip)
	return nil
}

// waitUSBBack waits up to 15 s for serial to answer again.
func (a *app) waitUSBBack(serial string) {
	c := *a.adb
	c.Serial = serial
	for i := 0; i < 8; i++ {
		out, err := c.runQuiet(10*time.Second, "get-state")
		if err == nil && strings.TrimSpace(out) == "device" {
			return
		}
		if !sleepCtx(2 * time.Second) {
			return
		}
	}
}

// reconnectNet re-establishes a network adb connection (Wi-Fi, the SSH
// tunnel or the Steam Frame's USB port forward) after it dropped.
func (a *app) reconnectNet() {
	if a.forwardPort > 0 && a.frameUSB != "" {
		c := *a.adb
		c.Serial = a.frameUSB
		p := "tcp:" + strconv.Itoa(a.forwardPort)
		c.run(20*time.Second, "forward", p, p)
	}
	if a.tunnel != nil && !a.tunnel.running() {
		logf("the SSH tunnel has ended; starting it again")
		if err := a.tunnel.restart(); err != nil {
			logf("SSH tunnel restart failed: %v", err)
		}
	}
	serial := a.adb.Serial // after a tunnel restart: its new port
	if !isNetSerial(serial) {
		return
	}
	c := *a.adb
	c.Serial = ""
	// An "offline" entry answers "already connected": drop it first.
	st := *a.adb
	if out, err := st.runQuiet(10*time.Second, "get-state"); err != nil || strings.TrimSpace(out) != "device" {
		c.run(10*time.Second, "disconnect", serial)
	}
	out, _ := c.run(20*time.Second, "connect", serial)
	logf("reconnect %s: connected=%v", serial, adbConnectOK(out))
}

// connectionWord is "USB cable" or "Wi-Fi connection" for messages.
func (a *app) connectionWord() string {
	if a.adb != nil && isNetSerial(a.adb.Serial) && a.forwardPort == 0 {
		return "Wi-Fi connection"
	}
	return "USB cable"
}
