//go:build !windows

package disk

import "os"

// IsElevated reports whether this process can open a raw block device
// by right. On Unix that is "am I root": the alternative test — trying
// an open and seeing what happens — is what the callers do anyway, and
// this one has to answer before any device is chosen.
//
// Group membership (`disk` on Debian, `operator` on macOS) can also
// grant the access without root, which is why DecideElevation asks
// whether an open was actually refused rather than trusting this alone.
func IsElevated() bool { return os.Geteuid() == 0 }

// RelaunchElevated does not exist off Windows, and that is the better
// situation: sudo re-runs THIS command with THESE arguments in THIS
// terminal, so there is no child console to tail and no log file to
// marshal output through. The error carries the exact line.
func RelaunchElevated(args []string) (exitCode int, log string, err error) {
	return RelaunchElevatedExe("", args)
}

// RelaunchElevatedExe is RelaunchElevated with the child named
// explicitly. Off Windows there is still no child to start — the answer
// is the same sudo line — but the line now names the binary the caller
// meant. core-app calls this with the path of the `core` CLI beside it,
// so a Linux or macOS user is told to run `sudo /path/to/core flash …`
// and not `sudo /path/to/core-app …`, which would do nothing at all.
func RelaunchElevatedExe(exe string, args []string) (exitCode int, log string, err error) {
	return 0, "", &NeedsSudoError{Command: SudoCommandExe(exe, args)}
}
