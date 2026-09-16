//go:build darwin

package eject

import (
	"fmt"
	"io"
	"os/exec"
	"strings"
)

// ejectVolume on macOS is diskutil, which flushes, unmounts every volume on
// the disk and powers it down in one step. It accepts a mount point, a volume
// node or a whole-disk node, so whatever the user typed goes straight through.
func ejectVolume(w io.Writer, target string) error {
	if p, err := exec.LookPath("sync"); err == nil {
		if err := exec.Command(p).Run(); err != nil {
			fmt.Fprintf(w, "warning: sync failed (%v)\n", err)
		}
	}
	diskutil, err := exec.LookPath("diskutil")
	if err != nil {
		fmt.Fprintf(w, "%s: flushed. diskutil was not found, so run this yourself:\n  diskutil eject %s\n", target, target)
		return fmt.Errorf("diskutil not found; the volume was flushed but not ejected")
	}
	out, err := exec.Command(diskutil, "eject", target).CombinedOutput()
	if err != nil {
		return fmt.Errorf("diskutil eject %s: %v: %s", target, err, strings.TrimSpace(string(out)))
	}
	fmt.Fprintf(w, "%s: %s", target, string(out))
	fmt.Fprintln(w, "Safe to unplug.")
	return nil
}
