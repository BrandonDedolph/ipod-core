package cli

import (
	"strings"
	"testing"

	"github.com/spf13/cobra"
)

func findCmd(t *testing.T, path ...string) *cobra.Command {
	t.Helper()
	cmd, _, err := Root().Find(path)
	if err != nil {
		t.Fatalf("Find(%v): %v", path, err)
	}
	if cmd.Name() != path[len(path)-1] {
		t.Fatalf("Find(%v) resolved to %q", path, cmd.Name())
	}
	return cmd
}

// TestRootRegistersOnlyRealCommands pins the rule that the command tree
// advertises nothing it cannot do. There are no stubs left at all now:
// `info` was the last one, and S6 filled it in along with `backup` and
// `firmware read`; S8 added `update` and `doctor`. A command listed in --help reads, to anyone who did
// not write it, as a command that works; the ones still to come are
// tracked in core/cli/README.md.
func TestRootRegistersOnlyRealCommands(t *testing.T) {
	want := map[string]bool{
		"build": true, "info": true, "backup": true,
		"firmware": true, "index": true, "art": true,
		"sync": true, "eject": true, "flash": true,
		"update": true, "doctor": true,
	}
	for _, c := range Root().Commands() {
		if c.Name() == "help" || c.Name() == "completion" {
			continue
		}
		if !want[c.Name()] {
			t.Errorf("root registers %q, which is not an implemented command", c.Name())
		}
		delete(want, c.Name())
	}
	for name := range want {
		t.Errorf("root no longer registers %q", name)
	}
}

// TestDeviceFlagExists: internal/disk defines ErrMultipleDevices
// ("select one with --device") and this is the way to select.
func TestDeviceFlagExists(t *testing.T) {
	root := Root()
	f := root.PersistentFlags().Lookup("device")
	if f == nil {
		t.Fatal("root has no persistent --device flag")
	}
	if f.DefValue != "" {
		t.Errorf("--device default = %q, want empty", f.DefValue)
	}
	// Persistent root flags must be inherited by every subcommand.
	for _, name := range []string{"build", "info", "backup", "firmware", "index", "art",
		"sync", "eject", "flash", "update", "doctor"} {
		c := findCmd(t, name)
		if c.InheritedFlags().Lookup("device") == nil {
			t.Errorf("%s cannot see the global --device flag", name)
		}
	}
}

func TestDeviceFlagIsReadable(t *testing.T) {
	root := Root()
	root.SetArgs([]string{"info", "--device", "/dev/sdz"})
	var got string
	info, _, err := root.Find([]string{"info"})
	if err != nil {
		t.Fatalf("find info: %v", err)
	}
	info.RunE = func(cmd *cobra.Command, args []string) error {
		got = deviceFlag(cmd)
		return nil
	}
	root.SetOut(&strings.Builder{})
	root.SetErr(&strings.Builder{})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute: %v", err)
	}
	if got != "/dev/sdz" {
		t.Errorf("deviceFlag = %q, want /dev/sdz", got)
	}
}

func TestDeviceFlagDefaultsEmpty(t *testing.T) {
	if got := deviceFlag(Root()); got != "" {
		t.Errorf("deviceFlag on a bare root = %q, want empty", got)
	}
}

// TestInfoWithoutADeviceExplainsItself: with nothing plugged in, info
// must fail with the thing to try next, not with a bare "no iPod found"
// and not with a stack of Go errors. The three lines it prints are the
// three causes: not in disk mode, a charge-only cable, or (on Windows)
// not elevated.
//
// This runs on the developer's own machine, which has real disks on it.
// The assertion is therefore about the SHAPE of the failure, not about
// a particular message: an ordinary user account cannot open
// /dev/sd* or \\.\PhysicalDriveN, so either "no iPod" or "access
// denied" is a correct answer here and both must be actionable.
func TestInfoWithoutADeviceExplainsItself(t *testing.T) {
	_, _, err := runCore(t, "info")
	if err == nil {
		t.Skip("this machine has something that identifies as an iPod attached")
	}
	msg := err.Error()
	actionable := strings.Contains(msg, "disk mode") ||
		strings.Contains(msg, "sudo ") ||
		strings.Contains(msg, "RunAs") ||
		strings.Contains(msg, "Administrator")
	if !actionable {
		t.Errorf("info failed without telling the user what to do:\n%s", msg)
	}
}

// TestLoadBearingCommandsExist guards the commands that actually work
// today, including the exact invocation core/Makefile's "ipod" target
// depends on.
func TestLoadBearingCommandsExist(t *testing.T) {
	for _, path := range [][]string{
		{"firmware", "pack"},
		{"firmware", "unpack"},
		{"build"},
	} {
		cmd := findCmd(t, path...)
		if cmd.RunE == nil && cmd.Run == nil && !cmd.HasSubCommands() {
			t.Errorf("%v has no implementation", path)
		}
	}
	// core/Makefile: go run ./cmd/core firmware pack <bin> --out <ipod> --force
	pack := findCmd(t, "firmware", "pack")
	for _, flag := range []string{"out", "force"} {
		if pack.Flags().Lookup(flag) == nil {
			t.Errorf("firmware pack lost --%s, which core/Makefile depends on", flag)
		}
	}
}
