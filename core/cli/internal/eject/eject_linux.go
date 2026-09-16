//go:build linux

package eject

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// ejectVolume on Linux: flush, then hand the volume to udisks, which is what
// the desktop's own "safely remove" button uses. Doing it by hand (umount +
// sending a START STOP UNIT to the disk) needs root; udisks is the thing that
// is allowed to do it for a logged-in user.
//
// When udisksctl is not installed the command does NOT half-do the job: it
// prints the two lines to run and returns an error, because "ejected" is a
// claim the user is about to act on by pulling a cable.
func ejectVolume(w io.Writer, target string) error {
	dev, err := resolveDevice(target)
	if err != nil {
		return err
	}
	disk := parentDisk(dev)

	flushDisks(w)

	udisks, lookErr := exec.LookPath("udisksctl")
	if lookErr != nil {
		fmt.Fprintf(w, "%s: flushed. udisksctl is not installed, so run these yourself:\n", dev)
		fmt.Fprintf(w, "  udisksctl unmount -b %s\n", dev)
		fmt.Fprintf(w, "  udisksctl power-off -b %s\n", disk)
		return fmt.Errorf("udisksctl not found; the volume was flushed but not ejected")
	}

	if out, err := exec.Command(udisks, "unmount", "-b", dev).CombinedOutput(); err != nil {
		msg := strings.TrimSpace(string(out))
		// "not mounted" is a fine place to be: carry on to the power-off.
		if !strings.Contains(strings.ToLower(msg), "not mounted") {
			return fmt.Errorf("udisksctl unmount -b %s: %v: %s", dev, err, msg)
		}
	}
	fmt.Fprintf(w, "%s: unmounted.\n", dev)

	if out, err := exec.Command(udisks, "power-off", "-b", disk).CombinedOutput(); err != nil {
		// The volume is unmounted, which is the part that protects the data.
		fmt.Fprintf(w, "%s: could not power off (%v: %s) — the volume is unmounted, so it is safe to unplug.\n",
			disk, err, strings.TrimSpace(string(out)))
		return nil
	}
	fmt.Fprintf(w, "%s: powered off. Safe to unplug.\n", disk)
	return nil
}

// flushDisks empties the kernel's page cache to the medium. The sync(1) binary
// rather than the syscall, because syscall.Sync has a different signature on
// Linux and macOS and this is not worth a build tag.
func flushDisks(w io.Writer) {
	if p, err := exec.LookPath("sync"); err == nil {
		if err := exec.Command(p).Run(); err != nil {
			fmt.Fprintf(w, "warning: sync failed (%v)\n", err)
		}
	}
}

// resolveDevice accepts a device node or a mount point and returns the device
// node.
func resolveDevice(target string) (string, error) {
	if strings.HasPrefix(target, "/dev/") {
		return target, nil
	}
	st, err := os.Stat(target)
	if err != nil {
		return "", fmt.Errorf("%s: %w (give a device node like /dev/sdb1 or a mount point)", target, err)
	}
	if !st.IsDir() {
		return "", fmt.Errorf("%s is neither a device node nor a mount point", target)
	}
	abs, err := filepath.Abs(target)
	if err != nil {
		return "", err
	}
	if dev := deviceForMount(filepath.Clean(abs)); dev != "" {
		return dev, nil
	}
	return "", fmt.Errorf("%s is not a mount point; pass the device node (/dev/sdb1)", target)
}

// deviceForMount reads /proc/self/mountinfo and returns the source device of
// the mount at point.
func deviceForMount(point string) string {
	f, err := os.Open("/proc/self/mountinfo")
	if err != nil {
		return ""
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for sc.Scan() {
		fields := strings.Fields(sc.Text())
		if len(fields) < 10 {
			continue
		}
		sep := -1
		for i := 6; i < len(fields); i++ {
			if fields[i] == "-" {
				sep = i
				break
			}
		}
		if sep < 0 || sep+2 >= len(fields) {
			continue
		}
		if unescapeMountPath(fields[4]) == point {
			return unescapeMountPath(fields[sep+2])
		}
	}
	return ""
}

func unescapeMountPath(s string) string {
	if !strings.Contains(s, `\`) {
		return s
	}
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+3 < len(s) &&
			s[i+1] >= '0' && s[i+1] <= '7' &&
			s[i+2] >= '0' && s[i+2] <= '7' &&
			s[i+3] >= '0' && s[i+3] <= '7' {
			b.WriteByte((s[i+1]-'0')<<6 | (s[i+2]-'0')<<3 | (s[i+3] - '0'))
			i += 3
			continue
		}
		b.WriteByte(s[i])
	}
	return b.String()
}

// parentDisk turns a partition node into its whole-disk node: /dev/sdb2 ->
// /dev/sdb, /dev/nvme0n1p1 -> /dev/nvme0n1, /dev/mmcblk0p1 -> /dev/mmcblk0.
// power-off applies to the disk, not the partition.
func parentDisk(dev string) string {
	base := filepath.Base(dev)
	end := len(base)
	for end > 0 && base[end-1] >= '0' && base[end-1] <= '9' {
		end--
	}
	if end == len(base) || end == 0 {
		return dev // already a whole disk
	}
	if base[end-1] == 'p' && end > 1 {
		// nvme0n1p1 / mmcblk0p1: the 'p' belongs to the partition suffix.
		end--
	}
	parent := filepath.Join(filepath.Dir(dev), base[:end])
	// "/dev/nvme0n1" is a whole disk whose name ends in a digit; stripping
	// it would invent "/dev/nvme0n". If the name we derived is not a node
	// that exists, the one we were given already was the disk.
	if _, err := os.Stat(parent); err != nil {
		return dev
	}
	return parent
}
