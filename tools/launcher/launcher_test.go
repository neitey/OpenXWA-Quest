package main

import (
	"archive/zip"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"time"
)

func TestCompareVersions(t *testing.T) {
	cases := []struct {
		a, b string
		want int
	}{
		{"1.0.0", "1.0.0", 0},
		{"v1.0.1", "1.0.0", 1},
		{"1.0.0", "1.0.0-rc3", 1},
		{"1.0.0-rc3", "1.0.0-rc2", 1},
		{"1.0.0-rc10", "1.0.0-rc9", 1},
		{"1.0.0-rc3", "1.0.0", -1},
		{"1.1", "1.0.9", 1},
		{"2.0.0-rc1", "1.9.9", 1},
		{"dev", "0.0.1", -1},
		{"dev", "0.0.0", 0},
		{"1.0.0", "dev", 1},
		{"1.0.0+build5", "1.0.0", 0},
	}
	for _, c := range cases {
		if got := compareVersions(c.a, c.b); got != c.want {
			t.Errorf("compareVersions(%q, %q) = %d, want %d", c.a, c.b, got, c.want)
		}
	}
}

func TestParseVDFPaths(t *testing.T) {
	newFmt := `"libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"apps" { "228980" "1" }
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"   // comment
	}
}`
	got := parseVDFPaths(newFmt)
	want := []string{`C:\Program Files (x86)\Steam`, `D:\SteamLibrary`}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("new format: got %q want %q", got, want)
	}
	oldFmt := `"LibraryFolders"
{
	"TimeNextStatsReport"		"1561832478"
	"ContentStatsID"		"-158337411836"
	"1"		"E:\\Games\\Steam"
}`
	got = parseVDFPaths(oldFmt)
	want = []string{`E:\Games\Steam`}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("old format: got %q want %q", got, want)
	}
}

func TestSkipFileName(t *testing.T) {
	skip := []string{"XWINGALLIANCE.EXE", "ddraw.dll", "Luke0.plt", "game.LNK", "goggame-1234.info", "unins000.dat",
		"steam_api.dll", "steam_api64.dll", "desktop.ini", "Thumbs.db", ".DS_Store", "x.tmp", "icon.ico", "site.url"}
	keep := []string{"RESDATA.TXT", "SPACECRAFT0.LST", "N010101.WAV", "readme.txt", "exefile.dat", "plt.txt"}
	for _, n := range skip {
		if !skipFileName(n) {
			t.Errorf("%q should be skipped", n)
		}
	}
	for _, n := range keep {
		if skipFileName(n) {
			t.Errorf("%q should be kept", n)
		}
	}
}

func TestParseRemoteList(t *testing.T) {
	out := "12|./RESDATA.TXT\n" +
		"0|./MISSIONS/empty file.txt\n" +
		"345|./My Mods/it's here|odd.txt\n" +
		"stat: './x': Permission denied\n" +
		"garbage line\n" +
		listEndMarker + "\n"
	files, ok := parseRemoteList(out)
	if !ok {
		t.Fatal("marker not seen")
	}
	want := map[string]int64{"resdata.txt": 12, "missions/empty file.txt": 0, "my mods/it's here|odd.txt": 345}
	if !reflect.DeepEqual(files, want) {
		t.Errorf("got %v", files)
	}
	if _, ok := parseRemoteList("12|./a\n"); ok {
		t.Error("listing without marker must be incomplete")
	}
	local := []gameFile{{Rel: "RESDATA.TXT", Size: 12}, {Rel: "Missions/Empty File.txt", Size: 1}, {Rel: "NEW.TXT", Size: 1}}
	todo := filesToCopy(local, files)
	if len(todo) != 2 || todo[0].Rel != "Missions/Empty File.txt" || todo[1].Rel != "NEW.TXT" {
		t.Errorf("filesToCopy = %v", todo)
	}
}

func TestShQuoteAndMkdir(t *testing.T) {
	if got := shQuote("it's a b"); got != `'it'\''s a b'` {
		t.Errorf("shQuote = %s", got)
	}
	files := []gameFile{
		{Rel: "A/B/C/f1"}, {Rel: "A/B/f2"}, {Rel: "A B/f3"}, {Rel: "root.txt"}, {Rel: "Q/it's/f"},
	}
	cmds := mkdirCommands("/base", files, 4000)
	want := []string{`mkdir -p '/base/A B' '/base/A/B/C' '/base/Q/it'\''s'`}
	if !reflect.DeepEqual(cmds, want) {
		t.Errorf("mkdir = %q", cmds)
	}
	// Splitting keeps every command under the limit.
	var many []gameFile
	for i := 0; i < 500; i++ {
		many = append(many, gameFile{Rel: strings.Repeat("d", 20) + "/" + string(rune('a'+i%26)) + strings.Repeat("x", i) + "/f"})
	}
	cmds = mkdirCommands("/sdcard/Android/data/org.openxwa.quest/files/XWA", many, 4000)
	count := 0
	for _, c := range cmds {
		if len(c) > 4000 && strings.Count(c, "'/sdcard") > 1 {
			t.Errorf("command too long: %d", len(c))
		}
		count += strings.Count(c, "'/sdcard")
	}
	if count != 500 {
		t.Errorf("got %d dirs, want 500", count)
	}
}

func TestPlanPushes(t *testing.T) {
	var files []gameFile
	for i := 0; i < 250; i++ {
		files = append(files, gameFile{Rel: "BIG/sub/f" + string(rune('a'+i%26)) + strings.Repeat("1", i/26), Size: 1})
	}
	files = append(files, gameFile{Rel: "SMALL/x.txt", Abs: "/g/SMALL/x.txt"}, gameFile{Rel: "RESDATA.TXT", Abs: "/g/RESDATA.TXT"})
	jobs := planPushes("/g", "/r", files)
	if len(jobs) != 3 {
		t.Fatalf("got %d jobs", len(jobs))
	}
	if jobs[0].remote != "/r/RESDATA.TXT" || jobs[1].local != filepath.Join("/g", "BIG")+string(filepath.Separator)+"." || jobs[1].remote != "/r/BIG/" || len(jobs[1].files) != 250 || jobs[2].remote != "/r/SMALL/x.txt" {
		t.Errorf("jobs: %+v %+v %+v", jobs[0], jobs[1].remote, jobs[2])
	}
}

func TestParseDf(t *testing.T) {
	toybox := "Filesystem     1K-blocks    Used Available Use% Mounted on\n/dev/fuse      110000000 9000000 101000000   9% /storage/emulated\n"
	if n, ok := parseDfAvailable(toybox); !ok || n != 101000000 {
		t.Errorf("toybox: %d %v", n, ok)
	}
	wrapped := "Filesystem 1K-blocks Used Available Use% Mounted on\n/dev/very/long/name\n 1000 400 600 40% /x\n"
	if n, ok := parseDfAvailable(wrapped); !ok || n != 600 {
		t.Errorf("wrapped: %d %v", n, ok)
	}
	if _, ok := parseDfAvailable("df: /x: No such file"); ok {
		t.Error("error output must not parse")
	}
}

func TestParseSHA256Sums(t *testing.T) {
	h := strings.Repeat("ab", 32)
	text := h + "  OpenXWA-Quest-1.0.1.zip\n" + strings.Repeat("CD", 32) + " *OpenXWA-quest-1.0.1.apk\r\nbad line\n"
	sums := parseSHA256Sums(strings.ReplaceAll(text, "\r", ""))
	if sums["OpenXWA-Quest-1.0.1.zip"] != h || sums["OpenXWA-quest-1.0.1.apk"] != strings.Repeat("cd", 32) || len(sums) != 2 {
		t.Errorf("sums = %v", sums)
	}
}

func TestDevicesAndNames(t *testing.T) {
	out := "List of devices attached\n" +
		"1WMHH000000000\tdevice usb:1-1 product:eureka model:Quest_3 device:eureka transport_id:3\n" +
		"PA7L10MGF\tunauthorized usb:1-2 transport_id:4\n" +
		"0123\tno permissions (user in plugdev group); see [http://developer.android.com/tools/device.html]\n\n"
	devs := parseDevices(out)
	if len(devs) != 3 || devs[0].model != "Quest 3" || devs[1].state != "unauthorized" || devs[2].state != "no permissions" {
		t.Errorf("devices = %+v", devs)
	}
	if waitState(devs) != "unauthorized" || waitState(nil) != "none" || waitState([]adbDevice{{state: "weird"}}) != "weird" ||
		waitState([]adbDevice{{state: "offline"}, {state: "host"}}) != "offline" {
		t.Error("waitState")
	}
	for _, st := range []string{"none", "unauthorized", "offline", "authorizing", "connecting", "no permissions",
		"recovery", "sideload", "bootloader", "host", "weird"} {
		if waitMessage(st) == "" {
			t.Errorf("no message for %s", st)
		}
	}
	if !strings.Contains(waitMessage("unauthorized"), "Allow") || !strings.Contains(waitMessage("none"), "USB-C") ||
		!strings.Contains(waitMessage("unauthorized"), "Steam Frame") || !strings.Contains(waitMessage("weird"), "weird") {
		t.Error("wait messages")
	}
	if !strings.Contains(shellProblem("error: closed"), "closed") || !strings.Contains(shellProblem("adb: more than one device/emulator"), "Unplug") {
		t.Error("shellProblem")
	}
	names := map[[2]string]string{
		{"Oculus", "Quest 3"}:  "Meta Quest 3",
		{"Meta", "Quest 3S"}:   "Meta Quest 3S",
		{"Oculus", "Quest"}:    "Meta Quest",
		{"Pico", "PICO 4"}:     "PICO 4",
		{"Valve", "Frame"}:     "Valve Frame",
		{"", ""}:               "headset",
		{"Oculus", "Oculus X"}: "Oculus X",
	}
	for in, want := range names {
		if got := deviceDisplayName(in[0], in[1]); got != want {
			t.Errorf("deviceDisplayName(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestInstalledVersionAndPilots(t *testing.T) {
	dump := "Activity Resolver Table:\n  versionName=9.9 (other)\nPackages:\n  Package [org.openxwa.quest] (c0ffee):\n    versionCode=7 minSdk=29\n    versionName=1.0.0-rc3\n"
	if v, ok := installedVersion(dump); !ok || v != "1.0.0-rc3" {
		t.Errorf("installedVersion = %q %v", v, ok)
	}
	if _, ok := installedVersion("Unable to find package: org.openxwa.quest"); ok {
		t.Error("not installed expected")
	}
	if apkVersionFromName("OpenXWA-quest-1.0.0-rc3.apk") != "1.0.0-rc3" || apkVersionFromName("other.apk") != "" {
		t.Error("apkVersionFromName")
	}
	names := pilotNames([]gameFile{{Rel: "Luke0.plt"}, {Rel: "Luke1.plt"}, {Rel: "Wedge0.plt"}, {Rel: "X.plt"}})
	if !reflect.DeepEqual(names, []string{"Luke", "Wedge", "X"}) {
		t.Errorf("pilotNames = %q", names)
	}
}

func TestGameDetection(t *testing.T) {
	dir := t.TempDir()
	game := filepath.Join(dir, "Star Wars X-Wing Alliance", "game")
	for _, f := range []string{"resdata.txt", "FlightModels/Spacecraft0.lst", "MISSIONS/MISSION.LST", "MOVIES/PROLOGUE.SNM", "MOVIES/battle1.snm", "WAVE/FRONTEND/B1M1/N010101.WAV"} {
		p := filepath.Join(game, filepath.FromSlash(f))
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte("x"), 0o644)
	}
	root, _ := findGameRoot(filepath.Join(dir, "Star Wars X-Wing Alliance"), 2)
	if root != game {
		t.Errorf("findGameRoot = %q", root)
	}
	cd := filepath.Join(dir, "cd")
	os.MkdirAll(filepath.Join(cd, "ALLIANCE"), 0o755)
	os.WriteFile(filepath.Join(cd, "ALLIANCE", "RESDATA.TXT"), nil, 0o644)
	if r, u := findGameRoot(cd, 2); r != "" || u != cd {
		t.Errorf("unmerged: %q %q", r, u)
	}
	// Steam library with libraryfolders.vdf pointing to a second library.
	steam := filepath.Join(dir, "steam")
	lib := filepath.Join(dir, "lib2")
	os.MkdirAll(filepath.Join(steam, "steamapps"), 0o755)
	os.MkdirAll(filepath.Join(lib, "steamapps", "common"), 0o755)
	os.Rename(filepath.Join(dir, "Star Wars X-Wing Alliance"), filepath.Join(lib, "steamapps", "common", "Star Wars X-Wing Alliance"))
	vdf := `"libraryfolders" { "1" { "path" "` + strings.ReplaceAll(lib, `\`, `\\`) + `" } }`
	os.WriteFile(filepath.Join(steam, "steamapps", "libraryfolders.vdf"), []byte(vdf), 0o644)
	dirs := steamGameDirs([]string{steam})
	if len(dirs) != 1 || !strings.HasSuffix(dirs[0], "Star Wars X-Wing Alliance") {
		t.Errorf("steamGameDirs = %q", dirs)
	}
	if got := cleanUserPath(`  "C:\Games\XWA"  `); got != `C:\Games\XWA` {
		t.Errorf("cleanUserPath = %q", got)
	}
}

func TestSafeZipPath(t *testing.T) {
	dest := t.TempDir()
	for _, bad := range []string{"../x", "a/../../x", "/etc/passwd", `C:\x`, `..\x`} {
		if _, err := safeZipPath(dest, bad); err == nil {
			t.Errorf("%q must be rejected", bad)
		}
	}
	if p, err := safeZipPath(dest, "platform-tools/adb"); err != nil || p != filepath.Join(dest, "platform-tools", "adb") {
		t.Errorf("good path: %q %v", p, err)
	}
	// A zip with a bad entry is rejected before anything is written.
	zp := filepath.Join(t.TempDir(), "bad.zip")
	f, _ := os.Create(zp)
	w := zip.NewWriter(f)
	ww, _ := w.Create("ok.txt")
	ww.Write([]byte("ok"))
	w.Create("../evil.txt")
	w.Close()
	f.Close()
	if _, err := extractZip(zp, dest); err == nil {
		t.Error("zip slip not rejected")
	}
	if _, err := os.Stat(filepath.Join(dest, "ok.txt")); err == nil {
		t.Error("files written before rejecting")
	}
}

func TestSettings(t *testing.T) {
	p := filepath.Join(t.TempDir(), "launcher.ini")
	s := loadSettings(p)
	if !s.getBool("check_updates", false) || s.getBool("include_test_versions", true) || s.get("skip_version", "x") != "" {
		t.Error("defaults")
	}
	if err := s.set("skip_version", "1.0.1"); err != nil {
		t.Fatal(err)
	}
	s2 := loadSettings(p)
	if s2.get("skip_version", "") != "1.0.1" {
		t.Error("skip_version not saved")
	}
	data, _ := os.ReadFile(p)
	if !strings.Contains(string(data), "# include_test_versions") {
		t.Error("comments lost")
	}
}

func TestFetchNewestRelease(t *testing.T) {
	releases := []ghRelease{
		{TagName: "v1.0.0", PublishedAt: "2026-10-01T10:00:00Z"},
		{TagName: "v1.0.2-rc1", Prerelease: true},
		{TagName: "v9.0.0", Draft: true},
		{TagName: "v1.0.1"},
	}
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("User-Agent") == "" || r.Header.Get("Accept") != "application/vnd.github+json" {
			w.WriteHeader(400)
			return
		}
		switch r.URL.Path {
		case releasesPath + "/latest":
			json.NewEncoder(w).Encode(releases[3])
		case releasesPath:
			json.NewEncoder(w).Encode(releases)
		default:
			w.WriteHeader(404)
		}
	}))
	defer srv.Close()
	r, err := fetchNewestRelease(srv.URL, false)
	if err != nil || r.version() != "1.0.1" {
		t.Errorf("stable: %v %v", r, err)
	}
	r, err = fetchNewestRelease(srv.URL, true)
	if err != nil || r.version() != "1.0.2-rc1" {
		t.Errorf("test versions: %v %v", r, err)
	}
	if _, err := fetchNewestRelease("http://127.0.0.1:1", false); err == nil {
		t.Error("offline must fail")
	}
	if d := (&ghRelease{PublishedAt: "2026-10-01T10:00:00Z"}).releaseDate(); d != "1 Oct 2026" {
		t.Errorf("date %q", d)
	}
}

func TestOwnership(t *testing.T) {
	out := "10225|u0_a225|/sdcard/x/XWA\n2000|shell|/sdcard/x/XWA/FLIGHTMODELS\nstat: missing\n"
	m := parseOwners(out, []string{"/sdcard/x/XWA", "/sdcard/x/XWA/FLIGHTMODELS"})
	if m["/sdcard/x/XWA"].uid != 10225 || m["/sdcard/x/XWA/FLIGHTMODELS"].name != "shell" {
		t.Errorf("parseOwners = %v", m)
	}
	// Names printed differently: matched by order when every path got a line.
	m = parseOwners("10225|u0_a225|/storage/emulated/0/a\n", []string{"/sdcard/a"})
	if m["/sdcard/a"].uid != 10225 {
		t.Errorf("by order: %v", m)
	}
	cases := []struct {
		o      owner
		app    int
		foreig bool
	}{
		{owner{"shell", 2000}, 10225, true},
		{owner{"shell", -1}, 0, true},
		{owner{"u0_a225", 10225}, 10225, false},
		{owner{"u0_a99", 10099}, 10225, true},
		{owner{"u0_a225", 10225}, 0, false},
		{owner{"root", 0}, 0, true},
		{owner{"u0_a225", -1}, 10225, false},
	}
	for _, c := range cases {
		if got := foreignOwner(c.o, c.app); got != c.foreig {
			t.Errorf("foreignOwner(%v, %d) = %v", c.o, c.app, got)
		}
	}
	// Steam Frame: what the app makes shows as root, so root is the app's.
	lep := &app{uid: 10225, appOwner: &owner{"root", 0}}
	if lep.foreign(owner{"root", 0}) || !lep.foreign(owner{"shell", 2000}) || lep.foreign(owner{"u0_a225", 10225}) {
		t.Error("foreign with a learnt root owner")
	}
	if !(&app{uid: 10225}).foreign(owner{"root", 0}) {
		t.Error("root without a learnt owner")
	}
	dump := "Packages:\n  Package [org.openxwa.quest] (4d2b1c):\n    userId=10225\n    versionName=1.0\n"
	if parseAppUID(dump) != 10225 || parseAppUID("nothing") != 0 {
		t.Error("parseAppUID")
	}
}

func TestLeptonPorts(t *testing.T) {
	ss := "State  Recv-Q Send-Q Local Address:Port Peer Address:Port\n" +
		"LISTEN 0      128          0.0.0.0:22         0.0.0.0:*\n" +
		"LISTEN 0      4096       127.0.0.1:5557       0.0.0.0:*\n" +
		"LISTEN 0      4096            [::]:5555          [::]:*\n" +
		"ESTAB  0      0          127.0.0.1:5560     127.0.0.1:40000\n"
	if got := parseLeptonPorts(ss); !reflect.DeepEqual(got, []int{5555, 5557}) {
		t.Errorf("ss: %v", got)
	}
	netstat := "Proto Recv-Q Send-Q Local Address  Foreign Address State\ntcp 0 0 0.0.0.0:5556 0.0.0.0:* LISTEN\n"
	if got := parseLeptonPorts(netstat); !reflect.DeepEqual(got, []int{5556}) {
		t.Errorf("netstat: %v", got)
	}
}

func TestReflow(t *testing.T) {
	in := "OpenXWA could not be updated: the copy\nwas signed differently.\n\nWARNING: uninstalling\ndeletes files.\nThese files:\n  a.txt\n  b.txt"
	want := "OpenXWA could not be updated: the copy was signed differently.\n\nWARNING: uninstalling deletes files. These files:\n  a.txt\n  b.txt"
	if got := reflow(in); got != want {
		t.Errorf("reflow:\n%q\nwant\n%q", got, want)
	}
}

func TestParseBundle(t *testing.T) {
	r, ok := parseBundle("Result: Bundle[{made=1, asked=1, base=/storage/emulated/0/Android/data/org.openxwa.quest/files/}]\n")
	if !ok || r["made"] != "1" || r["asked"] != "1" || r["base"] != "/storage/emulated/0/Android/data/org.openxwa.quest/files/" {
		t.Errorf("parseBundle: %v %v", r, ok)
	}
	r, ok = parseBundle("Result: Bundle[{error=java.io.FileNotFoundException: /x/y (Permission denied), base=/b/}]")
	if !ok || r["error"] != "java.io.FileNotFoundException: /x/y (Permission denied)" || r["base"] != "/b/" {
		t.Errorf("parseBundle error: %v", r)
	}
	if _, ok := parseBundle("Result: null"); ok {
		t.Error("null result parsed")
	}
	if _, ok := parseBundle("Error while accessing provider:org.openxwa.quest.setup\njava.lang.IllegalArgumentException: Unknown authority"); ok {
		t.Error("error parsed")
	}
}

func TestImportBatches(t *testing.T) {
	var items []importItem
	for i := 0; i < 6000; i++ {
		items = append(items, importItem{Rel: fmt.Sprint(i), Size: 10})
	}
	items = append(items, importItem{Rel: "big1", Size: importBatchSize - 5}, importItem{Rel: "big2", Size: importBatchSize + 5})
	b := importBatches(items)
	n := 0
	for _, x := range b {
		n += len(x)
		if len(x) > importBatchMax {
			t.Errorf("batch of %d files", len(x))
		}
	}
	if n != len(items) || len(b) != 5 {
		t.Errorf("%d items in %d batches", n, len(b))
	}
}

// TestMain lets the test binary act as ssh's askpass program (see
// TestSSHTunnelAskpass), like the launcher exe does.
func TestMain(m *testing.M) {
	if runAskpass() {
		return
	}
	os.Exit(m.Run())
}

func TestSplitHostPort(t *testing.T) {
	cases := []struct {
		in   string
		host string
		port int
		ok   bool
	}{
		{"frame", "frame", 5555, true},
		{" frame ", "frame", 5555, true},
		{"frame:5556", "frame", 5556, true},
		{"192.168.1.23", "192.168.1.23", 5555, true},
		{"192.168.1.23:5599", "192.168.1.23", 5599, true},
		{"[fe80::1]:5555", "fe80::1", 5555, true},
		{"fe80::1", "fe80::1", 5555, true},
		{"frame:", "", 0, false},
		{"frame:99999", "", 0, false},
		{"frame:abc", "", 0, false},
		{"", "", 0, false},
		{"my frame", "", 0, false},
		{"steamos@frame", "", 0, false},
		{"http://frame", "", 0, false},
	}
	for _, c := range cases {
		h, p, err := splitHostPort(c.in, adbTCPPort)
		if (err == nil) != c.ok || h != c.host || p != c.port {
			t.Errorf("splitHostPort(%q) = %q, %d, %v", c.in, h, p, err)
		}
	}
	for s, want := range map[string]bool{
		"192.168.1.5:5555": true, "localhost:5555": true, "127.0.0.1:41234": true, "[fe80::1]:5555": true,
		"1WMHH8123": false, "emulator-5554": false, "": false, "2G0YC1ZF8B07WD": false,
	} {
		if isNetSerial(s) != want {
			t.Errorf("isNetSerial(%q) = %v", s, !want)
		}
	}
	if got := pickAddr([]string{"fe80::1", "192.168.1.9"}); got != "192.168.1.9" {
		t.Errorf("pickAddr = %s", got)
	}
	if got := pickAddr([]string{"fe80::1", "2001:db8::5"}); got != "2001:db8::5" {
		t.Errorf("pickAddr v6 = %s", got)
	}
}

func TestDialErrorKinds(t *testing.T) {
	cases := map[string]string{
		"failed to connect to '192.168.1.5:5555': Connection refused":                                                                     "refused",
		"cannot connect to 192.168.1.5:5555: No connection could be made because the target machine actively refused it. (10061)":         "refused",
		"cannot resolve host 'frame' and port 5555: No such host is known. (11001)":                                                       "notfound",
		"failed to resolve host: 'frame': Name or service not known":                                                                      "notfound",
		"failed to connect to '192.168.1.5:5555': Connection timed out":                                                                   "timeout",
		"cannot connect to 10.0.0.9:5555: A connection attempt failed because the connected party did not properly respond after (10060)": "timeout",
		"dial tcp 10.1.1.1:5555: i/o timeout":                                                                                             "timeout",
		"dial tcp 10.1.1.1:5555: connect: no route to host":                                                                               "unreachable",
		"failed to connect to '10.0.0.2:5555': Network is unreachable":                                                                    "unreachable",
		"failed to authenticate to 192.168.1.5:5555":                                                                                      "auth",
		"something else entirely": "other",
	}
	for in, want := range cases {
		if got := dialErrorKind(in); got != want {
			t.Errorf("dialErrorKind(%q) = %s, want %s", in, got, want)
		}
	}
	for in, want := range map[string]bool{
		"connected to 192.168.1.5:5555":                               true,
		"already connected to 127.0.0.1:41000":                        true,
		"failed to connect to '192.168.1.5:5555': Connection refused": false,
		"cannot connect to frame:5555: connected to nothing":          false,
	} {
		if adbConnectOK(in) != want {
			t.Errorf("adbConnectOK(%q) = %v", in, !want)
		}
	}
}

func TestParseWlanIP(t *testing.T) {
	addr := "34: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP group default qlen 3000\n" +
		"    inet 192.168.178.44/24 brd 192.168.178.255 scope global wlan0\n" +
		"       valid_lft forever preferred_lft forever\n"
	if got := parseWlanIP(addr); got != "192.168.178.44" {
		t.Errorf("addr: %q", got)
	}
	route := "192.168.178.0/24 dev wlan0 proto kernel scope link src 192.168.178.44\n"
	if got := parseWlanIP("\n" + route); got != "192.168.178.44" {
		t.Errorf("route: %q", got)
	}
	both := "1: lo: <LOOPBACK,UP>\n    inet 127.0.0.1/8 scope host lo\n" +
		"5: rmnet0: <UP>\n    inet 10.20.30.40/30 scope global rmnet0\n" +
		"10.0.0.0/8 dev rmnet0 src 10.20.30.40\n" + route
	if got := parseWlanIP(both); got != "192.168.178.44" {
		t.Errorf("both: %q", got)
	}
	if got := parseWlanIP("1: lo: <LOOPBACK,UP>\n    inet 127.0.0.1/8 scope host lo\n"); got != "" {
		t.Errorf("loopback only: %q", got)
	}
	if got := parseWlanIP("7: eth0: <UP>\n    inet 10.1.2.3/24 scope global eth0\n"); got != "10.1.2.3" {
		t.Errorf("fallback: %q", got)
	}
}

func TestSSHArgsAndErrors(t *testing.T) {
	args := sshArgs("192.168.1.5", 41234, 5555, `C:\Users\John Doe\AppData\Local\OpenXWA\known_hosts`, true)
	j := strings.Join(args, "\x00")
	for _, want := range []string{
		"-L\x00127.0.0.1:41234:127.0.0.1:5555", "-l\x00steamos", "StrictHostKeyChecking=accept-new",
		"ExitOnForwardFailure=yes", "NumberOfPasswordPrompts=1", "PubkeyAuthentication=no",
	} {
		if !strings.Contains(j, want) {
			t.Errorf("sshArgs lacks %q: %q", want, args)
		}
	}
	if runtimeIsWindows() {
		if !strings.Contains(j, `UserKnownHostsFile="C:/Users/John Doe/AppData/Local/OpenXWA/known_hosts"`) {
			t.Errorf("known_hosts path not quoted: %q", args)
		}
	}
	if args[len(args)-2] != "192.168.1.5" || args[len(args)-1] != sshRemoteCommand {
		t.Errorf("host and command must come last: %q", args[len(args)-2:])
	}
	if a := strings.Join(sshArgs("frame", 1, 5556, "/tmp/k h", false), " "); !strings.Contains(a, "StrictHostKeyChecking=no") ||
		!strings.Contains(a, `UserKnownHostsFile="/tmp/k h"`) || !strings.Contains(a, "127.0.0.1:1:127.0.0.1:5556") {
		t.Errorf("old ssh args: %s", a)
	}

	cases := map[string]string{
		"ssh: Could not resolve hostname frame: No such host is known.":                                   "notfound",
		"steamos@192.168.1.5: Permission denied (publickey,password).":                                    "sshauth",
		"ssh: connect to host 192.168.1.5 port 22: Connection refused":                                    "sshrefused",
		"ssh: connect to host 192.168.1.5 port 22: Connection timed out":                                  "timeout",
		"ssh: connect to host 10.0.0.5 port 22: No route to host":                                         "unreachable",
		"@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@\nHost key verification failed.":         "hostkey",
		"bind [127.0.0.1]:41234: Address already in use\nchannel_setup_fwd_listener_tcpip: cannot listen": "localport",
		"channel 2: open failed: connect failed: Connection refused":                                      "lepton",
		"kex_exchange_identification: read: Connection reset":                                             "other",
	}
	for in, want := range cases {
		if got := classifySSH(in); got != want {
			t.Errorf("classifySSH(%q) = %s, want %s", in, got, want)
		}
	}
	for in, want := range map[string][2]int{
		"OpenSSH_for_Windows_8.1p1, LibreSSL 3.0.2": {8, 1},
		"OpenSSH_for_Windows_7.7p1, LibreSSL 2.6.5": {7, 7},
		"OpenSSH_9.6p1 Ubuntu-3ubuntu13, OpenSSL":   {9, 6},
	} {
		ma, mi, ok := parseSSHVersion(in)
		if !ok || ma != want[0] || mi != want[1] {
			t.Errorf("parseSSHVersion(%q) = %d.%d %v", in, ma, mi, ok)
		}
	}
	if _, _, ok := parseSSHVersion("ssh: unknown"); ok {
		t.Error("parseSSHVersion accepted garbage")
	}
	for in, want := range map[string]bool{
		"steamos@192.168.1.5's password: ":                                      true,
		"(steamos@frame) Password: ":                                            true,
		"Enter passphrase for key 'id_ed25519': ":                               false,
		"Are you sure you want to continue connecting (yes/no/[fingerprint])? ": false,
	} {
		if askpassWantsPassword(in) != want {
			t.Errorf("askpassWantsPassword(%q) = %v", in, !want)
		}
	}
	kh := filepath.Join(t.TempDir(), "known_hosts")
	os.WriteFile(kh, []byte("192.168.1.5 ssh-ed25519 AAAA\n[192.168.1.6]:22 ssh-ed25519 BBBB\nother ssh-ed25519 CCCC\n"), 0o600)
	forgetHostKey(kh, "192.168.1.5")
	forgetHostKey(kh, "192.168.1.6")
	if data, _ := os.ReadFile(kh); string(data) != "other ssh-ed25519 CCCC\n" {
		t.Errorf("forgetHostKey left %q", data)
	}
}

func TestRouteQuestionAndSpeed(t *testing.T) {
	q := routeQuestion("")
	if q.Default != routeUSB || q.Options[0].Key != "" || q.Options[0].Style != "primary" {
		t.Errorf("default route: %+v", q)
	}
	q = routeQuestion(routeWiFi)
	if q.Default != routeWiFi || q.Options[2].Key != "" || q.Options[0].Key != "u" {
		t.Errorf("wifi route: %+v", q)
	}
	p := Progress{Bytes: 100 << 20, TotalBytes: 400 << 20, Started: time.Now().Add(-10 * time.Second)}
	if got := p.Speed(p.Started.Add(10 * time.Second)); got != "10 MB/s, about 30 s left" {
		t.Errorf("speed: %q", got)
	}
	if got := p.Speed(p.Started.Add(time.Second)); got != "" {
		t.Errorf("speed too early: %q", got)
	}
	if got := (&Progress{Bytes: 5}).Speed(time.Now()); got != "" {
		t.Errorf("speed without start: %q", got)
	}
	for d, want := range map[time.Duration]string{
		40 * time.Second: "40 s", 12 * time.Minute: "12 min", 65 * time.Minute: "1 h 5 min",
	} {
		if got := humanDuration(d); got != want {
			t.Errorf("humanDuration(%s) = %q", d, got)
		}
	}
}

func TestScanAndProbe(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Skip(err)
	}
	defer ln.Close()
	open := ln.Addr().(*net.TCPAddr).Port
	closed, _ := freeLocalPort()
	if got := probeTCP("127.0.0.1", open, time.Second); got != "open" {
		t.Errorf("probe open port: %s", got)
	}
	if got := probeTCP("127.0.0.1", closed, time.Second); got != "refused" {
		t.Errorf("probe closed port: %s", got)
	}
	if got := scanPorts("127.0.0.1", []int{closed, open}, time.Second); !reflect.DeepEqual(got, []int{open}) {
		t.Errorf("scanPorts: %v", got)
	}
}

func runtimeIsWindows() bool { return filepath.Separator == '\\' }

// fakeTool writes a shell script standing in for adb or ssh.
func fakeTool(t *testing.T, name, body string) string {
	t.Helper()
	if runtimeIsWindows() {
		t.Skip("shell script tools")
	}
	p := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(p, []byte("#!/bin/sh\n"+body), 0o755); err != nil {
		t.Fatal(err)
	}
	return p
}

// TestConnectWiFi runs the Wi-Fi connection against a fake adb and real
// local ports: a refused port gives a "refused" problem, a listening port
// with an adb that answers gives a network serial.
func TestConnectWiFi(t *testing.T) {
	adb := fakeTool(t, "adb", `case "$*" in
  "connect "*) echo "connected to $2" ;;
  *"getprop ro.build.version.sdk"*) echo 32 ;;
  *) echo ok ;;
esac
`)
	a := &app{adb: &ADB{Path: adb}}
	old := lookupHost
	defer func() { lookupHost = old }()
	lookupHost = func(h string) ([]string, error) {
		if h == "frame" {
			return []string{"127.0.0.1"}, nil
		}
		return nil, fmt.Errorf("no such host")
	}
	err := a.connectWiFi("nosuchheadset")
	var wp *wifiProblem
	if !errors.As(err, &wp) || wp.kind != "notfound" {
		t.Fatalf("unknown host: %v", err)
	}
	closed, _ := freeLocalPort()
	err = a.connectWiFi("frame:" + strconv.Itoa(closed))
	if !errors.As(err, &wp) || wp.kind != "refused" || !strings.Contains(wp.msg, "Lepton") {
		t.Fatalf("closed port: %v", err)
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Skip(err)
	}
	defer ln.Close()
	port := ln.Addr().(*net.TCPAddr).Port
	if err := a.connectWiFi("frame:" + strconv.Itoa(port)); err != nil {
		t.Fatalf("open port: %v", err)
	}
	want := "127.0.0.1:" + strconv.Itoa(port)
	if a.adb.Serial != want || a.lepton != want || a.netHost != "frame" {
		t.Errorf("after connect: serial %q lepton %q host %q", a.adb.Serial, a.lepton, a.netHost)
	}
	if a.connectionWord() != "Wi-Fi connection" {
		t.Errorf("connectionWord: %s", a.connectionWord())
	}

	// An adb that keeps answering "unauthorized"-free garbage is not Android.
	a2 := &app{adb: &ADB{Path: fakeTool(t, "adb", `case "$*" in
  "connect "*) echo "connected to $2" ;;
  *) echo "/system/bin/sh: getprop: not found" ;;
esac
`)}}
	err = a2.netConnect(want, 30*time.Second)
	if !errors.As(err, &wp) || wp.kind != "noadb" {
		t.Errorf("not android: %v", err)
	}
}

// TestSSHTunnelAskpass runs the tunnel against a fake ssh that asks for the
// password through SSH_ASKPASS (this test binary, see TestMain), like
// OpenSSH does, and checks that the password never reaches the log.
func TestSSHTunnelAskpass(t *testing.T) {
	ssh := fakeTool(t, "ssh", `pw=$("$SSH_ASKPASS" "steamos@127.0.0.1's password: ")
if [ "$pw" != "s3cret pw" ]; then echo "steamos@127.0.0.1: Permission denied (password)." >&2; exit 255; fi
echo 'Authenticated to 127.0.0.1 ([127.0.0.1]:22) using "password".' >&2
echo "LISTEN 0 4096 127.0.0.1:5555 0.0.0.0:*"
echo `+sshReadyMarker+`
cat >/dev/null
echo "ssh: closed" >&2
`)
	t.Setenv("XDG_CACHE_HOME", t.TempDir())
	t.Setenv("HOME", t.TempDir())
	dir := t.TempDir()
	openLog(dir)
	defer closeLog()

	tun := &sshTunnel{sshPath: ssh, host: "127.0.0.1", remotePort: 5555, password: "s3cret pw", mode: "askpass", acceptNew: true}
	if err := tun.start(); err != nil {
		t.Fatalf("start: %v", err)
	}
	if !tun.running() || tun.localPort == 0 {
		t.Fatal("tunnel not running")
	}
	if got := parseLeptonPorts(tun.output()); !reflect.DeepEqual(got, []int{5555}) {
		t.Errorf("ports seen over ssh: %v", got)
	}
	tun.stop()
	if tun.running() {
		t.Error("tunnel still running after stop")
	}

	bad := &sshTunnel{sshPath: ssh, host: "127.0.0.1", remotePort: 5555, password: "wrong", mode: "askpass", acceptNew: true}
	err := bad.start()
	var wp *wifiProblem
	if !errors.As(err, &wp) || wp.kind != "sshauth" {
		t.Errorf("wrong password: %v", err)
	}
	if bad.password != "" {
		t.Error("a refused password is kept")
	}

	// An ssh that never asks (askpass not used) is reported as such, so the
	// launcher can fall back to a console window (Windows) or explain.
	silent := fakeTool(t, "ssh", `echo "steamos@127.0.0.1: Permission denied (password)." >&2; exit 255`)
	s2 := &sshTunnel{sshPath: silent, host: "127.0.0.1", remotePort: 5555, password: "x", mode: "askpass", acceptNew: true}
	err = s2.startOnce()
	if !errors.As(err, &wp) || wp.kind != "noaskpass" {
		t.Errorf("no askpass: %v", err)
	}

	closeLog()
	data, _ := os.ReadFile(filepath.Join(dir, logFileName))
	if len(data) == 0 {
		t.Fatal("empty log")
	}
	if strings.Contains(string(data), "s3cret") || strings.Contains(string(data), "wrong\n") {
		t.Errorf("the password reached the log:\n%s", data)
	}
	if !strings.Contains(string(data), "askpass request 1") || !strings.Contains(string(data), "exit code") {
		t.Errorf("log lacks the askpass request or exit code:\n%s", data)
	}
}
