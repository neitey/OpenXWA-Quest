package main

import (
	"os"
	"strings"
)

const settingsFileName = "launcher.ini"

const defaultSettings = `# ` + launcherName + ` settings
#
# check_updates: 1 = look for a new launcher version at start, 0 = never
check_updates=1
# include_test_versions: 1 = also offer test versions (release candidates)
include_test_versions=0
# skip_version: a version you chose to skip (filled in by the launcher)
skip_version=
# connection: how the launcher reaches the headset: usb, usbwifi (USB, then
# switch to Wi-Fi) or wifi (remembered from the last run)
connection=
# wifi_host: the headset's name or IP address for Wi-Fi (remembered)
wifi_host=
`

// settings is a tiny key=value file. Lines are kept so comments survive
// when a value is changed.
type settings struct {
	path  string
	lines []string
}

func loadSettings(path string) *settings {
	s := &settings{path: path}
	data, err := os.ReadFile(path)
	if err != nil {
		data = []byte(defaultSettings)
		if werr := os.WriteFile(path, data, 0o644); werr != nil {
			logf("could not create %s: %v", path, werr)
		}
	}
	text := strings.ReplaceAll(string(data), "\r\n", "\n")
	s.lines = strings.Split(strings.TrimRight(text, "\n"), "\n")
	return s
}

func (s *settings) get(key, def string) string {
	for _, l := range s.lines {
		t := strings.TrimSpace(l)
		if t == "" || t[0] == '#' || t[0] == ';' {
			continue
		}
		k, v, ok := strings.Cut(t, "=")
		if ok && strings.EqualFold(strings.TrimSpace(k), key) {
			return strings.TrimSpace(v)
		}
	}
	return def
}

func (s *settings) getBool(key string, def bool) bool {
	d := "0"
	if def {
		d = "1"
	}
	switch strings.ToLower(s.get(key, d)) {
	case "1", "true", "yes", "on":
		return true
	}
	return false
}

// set changes or adds key and saves the file.
func (s *settings) set(key, value string) error {
	done := false
	for i, l := range s.lines {
		t := strings.TrimSpace(l)
		if t == "" || t[0] == '#' || t[0] == ';' {
			continue
		}
		k, _, ok := strings.Cut(t, "=")
		if ok && strings.EqualFold(strings.TrimSpace(k), key) {
			s.lines[i] = key + "=" + value
			done = true
			break
		}
	}
	if !done {
		s.lines = append(s.lines, key+"="+value)
	}
	return os.WriteFile(s.path, []byte(strings.Join(s.lines, "\n")+"\n"), 0o644)
}
