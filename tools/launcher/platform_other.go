//go:build !windows

package main

import (
	"errors"
	"fmt"
	"io/fs"
	"os/exec"
	"syscall"
)

func isHiddenSystemFile(fs.FileInfo) bool { return false }

func windowsSteamRoots() []string       { return nil }
func windowsRegistryGameDirs() []string { return nil }

func noConsoleWindow(*exec.Cmd) {}

// sshProcAttr: in askpass mode ssh runs in a new session without a
// terminal, so it uses SSH_ASKPASS; otherwise it asks on the terminal.
func sshProcAttr(cmd *exec.Cmd, mode string) {
	if mode == "askpass" {
		if cmd.SysProcAttr == nil {
			cmd.SysProcAttr = &syscall.SysProcAttr{}
		}
		cmd.SysProcAttr.Setsid = true
	}
}
func setupConsole() {}

// startLauncher is only supported on Windows (the release contains a Windows exe).
func startLauncher(string) error { return errors.New("not supported on this system") }

func showOpenProblem(url string) {
	fmt.Println("Open this address in your web browser: " + url)
}

// appBrowsers: on macOS and Linux the default browser is used.
func appBrowsers() []string { return nil }

func nativePickerAvailable() bool { return false }

func pickFolderNative(string) (string, bool, error) {
	return "", false, errors.New("no folder picker on this system")
}
