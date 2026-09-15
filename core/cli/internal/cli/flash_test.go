package cli

import (
	"strings"
	"testing"
)

// internal/cli/flash.go is deliberately thin: the sequence and every
// assertion about it live in internal/flasher's tests, which drive the
// whole thing against an in-memory device. What is left to check here is
// the wiring — that the flags the safety story depends on exist, that
// the command refuses with no input, and that a run with no iPod
// attached fails with something the reader can act on.

func TestFlashHasItsSafetyFlags(t *testing.T) {
	cmd := findCmd(t, "flash")
	for _, flag := range []string{
		"dry-run", "yes", "backup-dir", "untested-hardware", "no-relaunch", "from-backup",
	} {
		if cmd.Flags().Lookup(flag) == nil {
			t.Errorf("core flash has no --%s", flag)
		}
	}
	if cmd.InheritedFlags().Lookup("device") == nil {
		t.Error("core flash cannot see the global --device flag")
	}
	// --elevated-log is how the elevated child's output gets back to the
	// parent; flash is the command that starts one.
	if cmd.InheritedFlags().Lookup("elevated-log") == nil {
		t.Error("core flash cannot see --elevated-log")
	}
}

func TestFlashRefusesWithNothingToWrite(t *testing.T) {
	_, _, err := runCore(t, "flash")
	if err == nil {
		t.Fatal("core flash with no arguments succeeded")
	}
	if !strings.Contains(err.Error(), "--from-backup") {
		t.Errorf("the refusal does not name the alternative:\n%v", err)
	}
}

func TestFlashRefusesBothInputs(t *testing.T) {
	_, _, err := runCore(t, "flash", "core.ipod", "--from-backup", "fwpart.bin")
	if err == nil || !strings.Contains(err.Error(), "not both") {
		t.Errorf("core flash with both inputs = %v, want a refusal", err)
	}
}

// TestFlashWithoutADeviceExplainsItself mirrors the info test: on a
// machine with no iPod, the failure has to say what to do next rather
// than bottom out in a Go error. The image is checked first, so this
// also proves the input parse happens before the device is touched.
func TestFlashWithoutADeviceExplainsItself(t *testing.T) {
	dir := t.TempDir()
	in := writeTemp(t, dir, "core.bin", plausibleImage(8192))
	_, _, err := runCore(t, "flash", in, "--dry-run")
	if err == nil {
		t.Skip("this machine has something that identifies as an iPod attached")
	}
	msg := err.Error()
	actionable := strings.Contains(msg, "disk mode") ||
		strings.Contains(msg, "sudo ") ||
		strings.Contains(msg, "RunAs") ||
		strings.Contains(msg, "Administrator")
	if !actionable {
		t.Errorf("flash failed without telling the user what to do:\n%s", msg)
	}
}

// TestFlashRefusesAnImplausibleImageBeforeLookingForADevice: the input
// parse is step 1 for a reason — a truncated objcopy output should not
// need an iPod attached to be rejected.
func TestFlashRefusesAnImplausibleImageBeforeLookingForADevice(t *testing.T) {
	dir := t.TempDir()
	in := writeTemp(t, dir, "core.bin", make([]byte, 64))
	_, _, err := runCore(t, "flash", in, "--dry-run")
	if err == nil {
		t.Fatal("a 64-byte core.bin was accepted")
	}
	if !strings.Contains(err.Error(), "below the") {
		t.Errorf("the refusal is not the plausibility rule:\n%v", err)
	}
}
