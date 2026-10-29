//go:build windows

package main

import (
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

const createNoWindow = 0x08000000

var (
	kernel32         = syscall.NewLazyDLL("kernel32.dll")
	procAllocConsole = kernel32.NewProc("AllocConsole")
	procMessageBoxW  = syscall.NewLazyDLL("user32.dll").NewProc("MessageBoxW")
)

// isHiddenSystemFile reports Windows files marked both hidden and system
// (desktop.ini, Thumbs.db and the like).
func isHiddenSystemFile(info fs.FileInfo) bool {
	d, ok := info.Sys().(*syscall.Win32FileAttributeData)
	if !ok {
		return false
	}
	const both = syscall.FILE_ATTRIBUTE_HIDDEN | syscall.FILE_ATTRIBUTE_SYSTEM
	return d.FileAttributes&both == both
}

// noConsoleWindow stops console programs (adb, powershell, cmd) started by
// this GUI program from flashing a console window.
func noConsoleWindow(cmd *exec.Cmd) {
	if cmd.SysProcAttr == nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{}
	}
	cmd.SysProcAttr.CreationFlags |= createNoWindow
}

// sshProcAttr sets how ssh is started: "askpass" without any console (so it
// cannot ask on one and uses SSH_ASKPASS), "window" in a new console window
// where the user types the password, "console" in the launcher's console.
func sshProcAttr(cmd *exec.Cmd, mode string) {
	const detachedProcess, createNewConsole = 0x00000008, 0x00000010
	if cmd.SysProcAttr == nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{}
	}
	switch mode {
	case "askpass":
		cmd.SysProcAttr.CreationFlags |= detachedProcess
	case "window":
		cmd.SysProcAttr.CreationFlags |= createNewConsole
	}
}

// startLauncher starts the new launcher (a GUI program) from its own folder.
func startLauncher(exe string) error {
	cmd := exec.Command(exe)
	cmd.Dir = filepath.Dir(exe)
	if err := cmd.Start(); err != nil {
		return err
	}
	return cmd.Process.Release()
}

// setupConsole gives the GUI build a console window for --console.
func setupConsole() {
	r, _, _ := procAllocConsole.Call()
	if r == 0 {
		return // already has a console
	}
	open := func(name string, access uint32) *os.File {
		p, _ := syscall.UTF16PtrFromString(name)
		h, err := syscall.CreateFile(p, access, syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE, nil, syscall.OPEN_EXISTING, 0, 0)
		if err != nil {
			return nil
		}
		return os.NewFile(uintptr(h), name)
	}
	if f := open("CONOUT$", syscall.GENERIC_READ|syscall.GENERIC_WRITE); f != nil {
		os.Stdout, os.Stderr = f, f
	}
	if f := open("CONIN$", syscall.GENERIC_READ|syscall.GENERIC_WRITE); f != nil {
		os.Stdin = f
	}
}

// showOpenProblem tells the user the address when no window could be opened.
func showOpenProblem(url string) {
	text, _ := syscall.UTF16PtrFromString("The " + launcherName + " window did not open.\r\n\r\n" +
		"Please open this address in your web browser:\r\n" + url)
	title, _ := syscall.UTF16PtrFromString(launcherName)
	const mbOK, mbIconInfo, mbSetForeground = 0x0, 0x40, 0x10000
	procMessageBoxW.Call(0, uintptr(unsafe.Pointer(text)), uintptr(unsafe.Pointer(title)), mbOK|mbIconInfo|mbSetForeground)
}

// appPathsExe reads "App Paths\<exe>" from the registry (where installers
// register their programs).
func appPathsExe(exe string) []string {
	var out []string
	for _, root := range []syscall.Handle{syscall.HKEY_CURRENT_USER, syscall.HKEY_LOCAL_MACHINE} {
		if h, ok := regOpen(root, `SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\`+exe); ok {
			if s, ok := regString(h, ""); ok && s != "" {
				out = append(out, strings.Trim(s, `"`))
			}
			syscall.RegCloseKey(h)
		}
	}
	return out
}

// appBrowsers lists Edge first, then Chrome.
func appBrowsers() []string {
	var out []string
	add := func(rel string) {
		for _, env := range []string{"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"} {
			if d := os.Getenv(env); d != "" {
				out = append(out, filepath.Join(d, rel))
			}
		}
	}
	add(`Microsoft\Edge\Application\msedge.exe`)
	out = append(out, appPathsExe("msedge.exe")...)
	add(`Google\Chrome\Application\chrome.exe`)
	out = append(out, appPathsExe("chrome.exe")...)
	return out
}

func nativePickerAvailable() bool { return true }

// The folder picker runs in PowerShell (WinForms FolderBrowserDialog) with no
// console window. A hidden top-most owner keeps the dialog in front. The
// start folder is passed in an environment variable to avoid quoting.
const pickerScript = `Add-Type -AssemblyName System.Windows.Forms; ` +
	`$f = New-Object System.Windows.Forms.Form -Property @{TopMost = $true; ShowInTaskbar = $false}; ` +
	`$d = New-Object System.Windows.Forms.FolderBrowserDialog; ` +
	`$d.Description = 'Choose your X-Wing Alliance folder (the one with RESDATA.TXT)'; ` +
	`$d.ShowNewFolderButton = $false; ` +
	`$p = $env:OPENXWA_PICKER_START; if ($p -and (Test-Path -LiteralPath $p)) { $d.SelectedPath = $p }; ` +
	`if ($d.ShowDialog($f) -eq [System.Windows.Forms.DialogResult]::OK) { ` +
	`[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; [Console]::Out.Write($d.SelectedPath) }; ` +
	`$f.Dispose()`

func pickFolderNative(initial string) (string, bool, error) {
	ps := "powershell.exe"
	if root := os.Getenv("SystemRoot"); root != "" {
		p := filepath.Join(root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")
		if isFile(p) {
			ps = p
		}
	}
	cmd := exec.Command(ps, "-NoProfile", "-STA", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", pickerScript)
	cmd.Env = append(os.Environ(), "OPENXWA_PICKER_START="+initial)
	noConsoleWindow(cmd)
	out, err := cmd.Output()
	if err != nil {
		return "", false, err
	}
	p := strings.TrimSpace(string(out))
	return p, p != "", nil
}
