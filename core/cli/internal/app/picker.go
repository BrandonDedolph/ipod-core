package app

import (
	"context"
	"errors"
	"fmt"
	"os/exec"
	"runtime"
	"strings"
)

// Gio draws pixels; it does not open the operating system's file
// dialog, and there is no portable Go one that is not a cgo toolkit in
// its own right. So the app shells out to the picker every desktop
// already has, reads the chosen path off stdout, and — when there is
// none — disables the Browse button and says why, rather than opening
// a home-made file list that would be worse than the text field beside
// it.
//
// The commands are built by pure functions of GOOS and a PATH lookup so
// the wording can be tested from here for all three platforms.

// ErrNoPicker means this system has no folder/file dialog we know how
// to drive. The text field still works.
var ErrNoPicker = errors.New("app: no folder picker on this system (type the path instead)")

// The PowerShell one-liners. They are single-quoted inside and never
// interpolate anything the user typed: the only input is the dialog's
// own output, which comes back on stdout.
const (
	// RootFolder MyComputer starts the tree at This PC (drives, then
	// Users\…), not at the profile's Desktop/Music/Downloads list.
	winFolderPS = `Add-Type -AssemblyName System.Windows.Forms; ` +
		`$d = New-Object System.Windows.Forms.FolderBrowserDialog; ` +
		`$d.Description = 'Choose the music folder'; ` +
		`$d.RootFolder = [System.Environment+SpecialFolder]::MyComputer; ` +
		`$d.ShowNewFolderButton = $false; ` +
		`if ($d.ShowDialog() -eq 'OK') { $d.SelectedPath }`
	winFilePS = `Add-Type -AssemblyName System.Windows.Forms; ` +
		`$d = New-Object System.Windows.Forms.OpenFileDialog; ` +
		`$d.Filter = 'Firmware image (*.ipod;*.bin)|*.ipod;*.bin|All files (*.*)|*.*'; ` +
		`if ($d.ShowDialog() -eq 'OK') { $d.FileName }`
)

// lookPath is exec.LookPath, replaced in the tests so the Linux
// branches can be checked on a machine with neither zenity nor kdialog
// installed (and on one with both).
type lookPath func(string) (string, error)

// pickerCommand returns the program and arguments that ask the user for
// a path. dir selects the folder variant.
func pickerCommand(goos string, dir bool, look lookPath) (string, []string, error) {
	switch goos {
	case "windows":
		script := winFilePS
		if dir {
			script = winFolderPS
		}
		// -STA is load-bearing: Windows Forms dialogs need a single-
		// threaded apartment, and PowerShell's default (MTA) draws the
		// FolderBrowserDialog tree WITHOUT expand arrows — the user sees
		// Desktop/Music/Downloads and cannot open any of them. Seen on
		// the first real pass through the window, 2026-09-16.
		return "powershell.exe", []string{"-NoProfile", "-NonInteractive", "-STA", "-Command", script}, nil
	case "darwin":
		// osascript's `choose folder` returns an alias; `POSIX path of`
		// turns it into something Go can open. A cancelled dialog exits
		// non-zero, which is how Pick reports "the user said no".
		script := `POSIX path of (choose file with prompt "Choose a firmware image")`
		if dir {
			script = `POSIX path of (choose folder with prompt "Choose the music folder")`
		}
		return "osascript", []string{"-e", script}, nil
	default:
		if p, err := look("zenity"); err == nil {
			args := []string{"--file-selection", "--title=Choose a firmware image"}
			if dir {
				args = []string{"--file-selection", "--directory", "--title=Choose the music folder"}
			}
			return p, args, nil
		}
		if p, err := look("kdialog"); err == nil {
			if dir {
				return p, []string{"--getexistingdirectory", "."}, nil
			}
			return p, []string{"--getopenfilename", "."}, nil
		}
		return "", nil, ErrNoPicker
	}
}

// PickerAvailable reports whether Browse… can be offered at all.
func PickerAvailable() bool {
	_, _, err := pickerCommand(runtime.GOOS, true, exec.LookPath)
	return err == nil
}

// PickFolder opens the OS folder dialog and returns what was chosen.
// An empty string with a nil error means the user cancelled.
func PickFolder(ctx context.Context) (string, error) { return pick(ctx, true) }

// PickFile opens the OS file dialog.
func PickFile(ctx context.Context) (string, error) { return pick(ctx, false) }

func pick(ctx context.Context, dir bool) (string, error) {
	name, args, err := pickerCommand(runtime.GOOS, dir, exec.LookPath)
	if err != nil {
		return "", err
	}
	out, err := exec.CommandContext(ctx, name, args...).Output()
	if err != nil {
		// Every one of these dialogs exits non-zero when the user
		// cancels, and a cancel is not a failure to report.
		var ee *exec.ExitError
		if errors.As(err, &ee) {
			return "", nil
		}
		return "", fmt.Errorf("app: running the folder picker (%s): %w", name, err)
	}
	return strings.TrimSpace(strings.TrimRight(string(out), "\r\n")), nil
}
