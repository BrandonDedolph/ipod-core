package disk

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// ErrNeedsSudo is what RelaunchElevated returns everywhere except
// Windows: there is no way to raise the privileges of a running
// process, and unlike UAC there is a perfectly good command the user
// can run instead. The concrete error carries that command.
var ErrNeedsSudo = errors.New("disk: raw device access needs root")

// NeedsSudoError carries the exact line to re-run.
type NeedsSudoError struct {
	// Command is the full `sudo /path/to/core …` line, quoted so it
	// can be pasted.
	Command string
}

func (e *NeedsSudoError) Error() string {
	return "raw device access needs root; re-run:\n  " + e.Command
}

// Is makes errors.Is(err, ErrNeedsSudo) work.
func (e *NeedsSudoError) Is(target error) bool { return target == ErrNeedsSudo }

// Need is what a caller must do to get raw access.
type Need int

const (
	// NeedNone: go ahead, the access we have is enough.
	NeedNone Need = iota
	// NeedRelaunch: Windows, not elevated. UAC cannot elevate this
	// console, so the binary has to start a copy of itself.
	NeedRelaunch
	// NeedSudo: Linux/macOS, not root. sudo re-runs in place.
	NeedSudo
)

func (n Need) String() string {
	switch n {
	case NeedRelaunch:
		return "relaunch-elevated"
	case NeedSudo:
		return "sudo"
	default:
		return "none"
	}
}

// DecideElevation is the whole elevation policy, as a function of four
// facts, so it can be read and tested as a table instead of being
// spread across three OS files.
//
//	elevated     — already Administrator / root
//	openRefused  — a raw open was attempted and came back denied
//	wantWrite    — the command is going to write
//
// The asymmetry worth noticing: a read that SUCCEEDED needs nothing,
// even unelevated. That is not hypothetical — on Linux a user in the
// `disk` group reads /dev/sdX fine, and telling them to sudo when they
// did not have to is how a tool teaches people to sudo reflexively. A
// write, though, asks for elevation whether or not the read worked: on
// Windows a non-elevated handle can sometimes read a physical drive
// while every write to it fails, and finding that out halfway through a
// flash is the failure this exists to prevent.
func DecideElevation(goos string, elevated, openRefused, wantWrite bool) Need {
	if elevated {
		return NeedNone
	}
	if !openRefused && !wantWrite {
		return NeedNone
	}
	if goos == "windows" {
		return NeedRelaunch
	}
	return NeedSudo
}

// ElevationCommand is the line to print when DecideElevation says the
// user has to do something. It is a pure function of the OS so the
// tests can check the Windows wording from Linux.
func ElevationCommand(goos, exe string, args []string) string {
	if goos == "windows" {
		// A RunAs child gets its own console, so anything it prints is
		// gone when the window closes — the redirect has to happen
		// INSIDE the elevated process, which is why this wraps cmd /c
		// rather than calling core.exe directly.
		// The log path is absolute, next to the executable: a RunAs child
		// starts in C:\Windows\System32, and a relative core-out.txt would
		// land there (or fail to open), which is the one place nobody looks.
		inner := quoteWindows(exe) + " " + strings.Join(quoteAllWindows(args), " ")
		log := quoteWindows(windowsDir(exe) + `\core-out.txt`)
		return fmt.Sprintf(`powershell.exe -NoProfile -Command "Start-Process cmd.exe `+
			`-ArgumentList '/c','%s > %s 2>&1' -Verb RunAs -Wait"`, inner, log)
	}
	return "sudo " + quoteUnix(exe) + " " + strings.Join(quoteAllUnix(args), " ")
}

// SudoCommand is the exact `sudo …` line for this process and these
// args, which is what a NeedsSudoError carries.
func SudoCommand(args []string) string { return SudoCommandExe("", args) }

// SudoCommandExe is SudoCommand for a named binary. An empty exe means
// "this process", which is what the CLI wants; core-app passes the path
// of the `core` CLI beside it, because that is the binary that would
// actually do the write.
func SudoCommandExe(exe string, args []string) string {
	if exe == "" {
		self, err := os.Executable()
		if err == nil {
			exe = self
		}
	}
	if exe == "" {
		exe = "core"
	}
	return ElevationCommand(runtime.GOOS, exe, args)
}

// quoteUnix single-quotes anything that is not plainly safe, so the
// printed line can be pasted verbatim.
func quoteUnix(s string) string {
	if s != "" && strings.IndexFunc(s, func(r rune) bool {
		return !(r == '/' || r == '.' || r == '-' || r == '_' || r == ':' ||
			(r >= '0' && r <= '9') || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z'))
	}) < 0 {
		return s
	}
	return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'"
}

func quoteAllUnix(args []string) []string {
	out := make([]string, len(args))
	for i, a := range args {
		out[i] = quoteUnix(a)
	}
	return out
}

// quoteWindows double-quotes an argument containing spaces. It is not
// syscall.EscapeArg (which only exists on Windows and which we want to
// be able to test from here); the cases it has to handle are paths with
// spaces, which is what C:\Users\… produces.
func quoteWindows(s string) string {
	if s != "" && !strings.ContainsAny(s, " \t\"") {
		return s
	}
	return `"` + strings.ReplaceAll(s, `"`, `\"`) + `"`
}

// windowsDir is filepath.Dir for a Windows path, computed by hand so the
// line can be built (and tested) on a host whose separator is '/'.
func windowsDir(p string) string {
	i := strings.LastIndexAny(p, `\/`)
	if i <= 0 {
		return "."
	}
	return p[:i]
}

func quoteAllWindows(args []string) []string {
	out := make([]string, len(args))
	for i, a := range args {
		out[i] = quoteWindows(a)
	}
	return out
}

// ElevatedLogFlag is the flag RelaunchElevated appends so the child,
// which owns a console the user will never see, writes its output
// somewhere the parent can read it back.
const ElevatedLogFlag = "--elevated-log"

// elevatedLogPath invents a path for one relaunch.
func elevatedLogPath() string {
	return filepath.Join(os.TempDir(), fmt.Sprintf("core-elevated-%d.log", os.Getpid()))
}

// isAccessDenied recognises the OS's "you are not allowed to open
// this". On Windows ERROR_ACCESS_DENIED already maps to
// fs.ErrPermission through syscall.Errno.Is, so one test covers all
// three platforms.
func isAccessDenied(err error) bool {
	return errors.Is(err, fs.ErrPermission) || errors.Is(err, ErrRawAccessDenied)
}
