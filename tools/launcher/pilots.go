package main

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// pilotName groups "Luke0.plt" and "Luke1.plt" as the pilot "Luke".
func pilotName(file string) string {
	base := strings.TrimSuffix(file, filepath.Ext(file))
	if len(base) > 1 && base[len(base)-1] >= '0' && base[len(base)-1] <= '9' {
		base = base[:len(base)-1]
	}
	return base
}

// pilotFiles lists the *.plt files at the top level of the game folder.
func pilotFiles(dir string) []gameFile {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []gameFile
	for _, e := range entries {
		if !e.Type().IsRegular() || !strings.EqualFold(filepath.Ext(e.Name()), ".plt") {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		out = append(out, gameFile{Rel: e.Name(), Abs: filepath.Join(dir, e.Name()), Size: info.Size()})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Rel < out[j].Rel })
	return out
}

func pilotNames(files []gameFile) []string {
	seen := map[string]bool{}
	var names []string
	for _, f := range files {
		n := pilotName(f.Rel)
		if !seen[strings.ToLower(n)] {
			seen[strings.ToLower(n)] = true
			names = append(names, n)
		}
	}
	return names
}

// copyPilots is step 6: optionally bring the PC pilots over. The app
// imports *.plt from pilot-import when it starts.
func (a *app) copyPilots() error {
	files := pilotFiles(a.gameDir)
	if len(files) == 0 {
		say("No pilot files in the game folder. Nothing to do.")
		return nil
	}
	logf("pilots: %v", pilotNames(files))
	ans := ui.Ask(Question{
		Kind:  "buttons",
		Title: "Copy your pilots?",
		Text: "Pilots found in your game folder: " + strings.Join(pilotNames(files), ", ") + ".\n" +
			"The VR game keeps its own pilots. Copying brings your PC pilots over; they\n" +
			"are imported the next time the game starts on the headset.",
		Options: []Option{
			{ID: "yes", Label: "Copy pilots", Key: "y"},
			{ID: "no", Label: "Don't copy", Key: "n", Style: "primary"},
		},
		Default:       "no",
		ConsolePrompt: "Copy these pilots to the headset? (y/N)",
	})
	if ans.ID != "yes" {
		say("Pilots are not copied.")
		return nil
	}
	if a.helper {
		if err := a.importFiles("Copying pilots", itemsOf(files), "pilot-import"); err != nil {
			return err
		}
		return a.pilotsDone()
	}
	// pilot-import is made by mkdir only where such folders belong to the app
	// (see probeCopyOwnership); otherwise adb push makes it.
	if a.mkdirOK {
		if out, err := a.adb.shell(30*time.Second, "mkdir -p "+shQuote(a.remotePilots())); err != nil {
			return fail("Could not create the pilot folder on the headset:\n  %s", strings.TrimSpace(out))
		}
	}
	for try := 1; try <= 3; try++ {
		var jobs []pushJob
		remote, err := a.listRemote(a.remotePilots())
		if err != nil {
			remote = map[string]int64{}
		}
		bad := filesToCopy(files, remote)
		if len(bad) == 0 {
			return a.pilotsDone()
		}
		for _, f := range bad {
			j := singleFileJob(a.remotePilots(), f)
			j.label = ""
			jobs = append(jobs, j)
		}
		if err := a.runPushes("Copying pilots", jobs, bad); err != nil {
			return err
		}
	}
	remote, _ := a.listRemote(a.remotePilots())
	if bad := filesToCopy(files, remote); len(bad) > 0 {
		var names []string
		for _, f := range bad {
			names = append(names, f.Rel)
		}
		say("WARNING: these pilot files could not be copied: %s", strings.Join(names, ", "))
		say("The game files are copied anyway; run the launcher again later to retry.")
		return nil
	}
	return a.pilotsDone()
}

// pilotsDone checks that the app can read pilot-import, then reports.
func (a *app) pilotsDone() error {
	m := a.remoteOwners(a.remotePilots())
	if o, ok := m[a.remotePilots()]; ok && a.foreign(o) {
		logf("pilot-import belongs to %v; removing it", o)
		a.adb.shell(60*time.Second, "rm -rf "+shQuote(a.remotePilots()))
		say("WARNING: the pilots could not be copied in a way the game can read on this\n" +
			"headset, so they were removed again. The game files are copied anyway.")
		return nil
	}
	say("Pilots copied. The game brings them in the next time it starts.")
	return nil
}
