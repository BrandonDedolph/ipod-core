package disk

import (
	"errors"
	"io/fs"
	"runtime"
	"strings"
	"testing"
)

// TestDecideElevation is the policy, as a table.
//
// The two rows worth arguing about are the unelevated reads: a read
// that WORKED asks for nothing (a Linux user in the `disk` group, a
// Windows metadata-only enumeration), and a read that was REFUSED asks
// for the same thing a write does. Getting the first one wrong teaches
// people to sudo by reflex; getting the second one wrong prints "no
// iPod found" at somebody whose iPod is plugged in.
func TestDecideElevation(t *testing.T) {
	cases := []struct {
		goos                             string
		elevated, openRefused, wantWrite bool
		want                             Need
	}{
		// Already privileged: nothing to do, whatever the command is.
		{"windows", true, false, false, NeedNone},
		{"windows", true, true, true, NeedNone},
		{"linux", true, true, true, NeedNone},
		{"darwin", true, false, true, NeedNone},

		// Unelevated reads that succeeded.
		{"windows", false, false, false, NeedNone},
		{"linux", false, false, false, NeedNone},
		{"darwin", false, false, false, NeedNone},

		// Unelevated reads that were refused.
		{"windows", false, true, false, NeedRelaunch},
		{"linux", false, true, false, NeedSudo},
		{"darwin", false, true, false, NeedSudo},

		// Writes always ask, refused open or not: a non-elevated
		// Windows handle can read a physical drive and still fail every
		// write to it, and finding that out mid-flash is the failure
		// this table exists to prevent.
		{"windows", false, false, true, NeedRelaunch},
		{"windows", false, true, true, NeedRelaunch},
		{"linux", false, false, true, NeedSudo},
		{"darwin", false, true, true, NeedSudo},
	}
	for _, c := range cases {
		got := DecideElevation(c.goos, c.elevated, c.openRefused, c.wantWrite)
		if got != c.want {
			t.Errorf("DecideElevation(%s, elevated=%v, refused=%v, write=%v) = %v, want %v",
				c.goos, c.elevated, c.openRefused, c.wantWrite, got, c.want)
		}
	}
}

func TestNeedString(t *testing.T) {
	for need, want := range map[Need]string{
		NeedNone:     "none",
		NeedRelaunch: "relaunch-elevated",
		NeedSudo:     "sudo",
	} {
		if got := need.String(); got != want {
			t.Errorf("Need(%d).String() = %q, want %q", need, got, want)
		}
	}
}

// TestElevationCommand pins the exact lines printed to the user. The
// Windows one is checked from Linux on purpose: it is the line a person
// pastes into PowerShell, and the reason it wraps cmd /c is that a
// RunAs child owns a console that closes with it, so the redirect has
// to happen inside the elevated process.
func TestElevationCommand(t *testing.T) {
	win := ElevationCommand("windows", `C:\Users\me\core.exe`, []string{"info"})
	for _, want := range []string{"Start-Process cmd.exe", "-Verb RunAs", "-Wait", `core.exe`, "info", "2>&1", `> C:\Users\me\core-out.txt`} {
		if !strings.Contains(win, want) {
			t.Errorf("the Windows elevation line is missing %q:\n%s", want, win)
		}
	}
	winSpace := ElevationCommand("windows", `C:\Program Files\core.exe`, []string{"backup", "--out", `C:\a b\x.bin`})
	if !strings.Contains(winSpace, `"C:\Program Files\core.exe"`) {
		t.Errorf("a path with a space was not quoted:\n%s", winSpace)
	}
	if !strings.Contains(winSpace, `"C:\a b\x.bin"`) {
		t.Errorf("an argument with a space was not quoted:\n%s", winSpace)
	}

	nix := ElevationCommand("linux", "/usr/local/bin/core", []string{"backup", "--device", "/dev/sdb"})
	if nix != "sudo /usr/local/bin/core backup --device /dev/sdb" {
		t.Errorf("the sudo line is %q", nix)
	}
	quoted := ElevationCommand("darwin", "/opt/my core/core", []string{"info"})
	if !strings.Contains(quoted, "'/opt/my core/core'") {
		t.Errorf("a path with a space was not quoted for the shell:\n%s", quoted)
	}
}

// TestRelaunchElevatedOffWindows: there is no UAC to invoke, so the
// call must hand back the exact sudo line rather than pretending to
// elevate.
func TestRelaunchElevatedOffWindows(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("this is the non-Windows behaviour")
	}
	code, log, err := RelaunchElevated([]string{"flash", "core.ipod"})
	if code != 0 || log != "" {
		t.Errorf("RelaunchElevated returned (%d, %q), want (0, \"\")", code, log)
	}
	if !errors.Is(err, ErrNeedsSudo) {
		t.Fatalf("err = %v, want ErrNeedsSudo", err)
	}
	var se *NeedsSudoError
	if !errors.As(err, &se) {
		t.Fatalf("err is not a *NeedsSudoError: %T", err)
	}
	if !strings.HasPrefix(se.Command, "sudo ") {
		t.Errorf("the carried command is %q, want it to start with sudo", se.Command)
	}
	for _, want := range []string{"flash", "core.ipod"} {
		if !strings.Contains(se.Command, want) {
			t.Errorf("the sudo line lost the argument %q: %s", want, se.Command)
		}
	}
}

func TestIsAccessDenied(t *testing.T) {
	if !isAccessDenied(fs.ErrPermission) {
		t.Error("fs.ErrPermission is not recognised as access denied")
	}
	if !isAccessDenied(ErrRawAccessDenied) {
		t.Error("ErrRawAccessDenied is not recognised as access denied")
	}
	if isAccessDenied(fs.ErrNotExist) {
		t.Error("a missing device was read as access denied")
	}
	if isAccessDenied(nil) {
		t.Error("nil was read as access denied")
	}
}

// TestElevatedLogFlagSpelling: RelaunchElevated appends this flag to
// the child's argv and the root command has to accept it. A typo here
// is a child that exits immediately with "unknown flag" and a parent
// that reports an empty log.
func TestElevatedLogFlagSpelling(t *testing.T) {
	if ElevatedLogFlag != "--elevated-log" {
		t.Errorf("ElevatedLogFlag = %q; internal/cli registers --elevated-log", ElevatedLogFlag)
	}
}
