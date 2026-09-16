package cli

import (
	"errors"
	"fmt"
	"io"
	"os"
	"runtime"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/spf13/cobra"
)

// Everything in this file is the shared front end of the three
// device-touching commands (info, backup, firmware read). They differ
// only in what they do with the bytes; finding the iPod, opening it,
// and explaining what to do when the OS says no is identical, and it is
// the part where a difference between commands would be a bug.

// selectDevice resolves the global --device flag to one iPod.
//
// Errors come back already decorated: a permission failure carries the
// elevated command line to run, and "no iPod found" carries the two
// things that are actually wrong 90 % of the time (not in disk mode,
// not plugged into a port that carries data).
func selectDevice(cmd *cobra.Command) (disk.IPod, error) {
	pod, err := disk.SelectIPod(deviceFlag(cmd))
	if err != nil {
		return disk.IPod{}, decorateDeviceError(err)
	}
	return pod, nil
}

// openDevice resolves --device and opens the disk read-only.
//
// Read-only is not a default that commands may override: nothing in S6
// writes, and the write path (S7) opens its own handle after a backup
// and a typed confirmation. A shared helper that could hand back a
// writable handle is a shared helper that will eventually hand one to a
// command that did not mean to ask.
func openDevice(cmd *cobra.Command) (disk.IPod, disk.Handle, error) {
	pod, err := selectDevice(cmd)
	if err != nil {
		return disk.IPod{}, nil, err
	}
	h, err := disk.Open(pod.Disk.Path, false)
	if err != nil {
		return disk.IPod{}, nil, decorateDeviceError(
			fmt.Errorf("open %s for reading: %w", pod.Disk.Path, err))
	}
	return pod, h, nil
}

// firmwarePartition wraps the firmware partition of an open device as
// something fwpart can read: offsets relative to the start of partition
// 0, bounded by its length, so nothing in fwpart can address a byte of
// the music partition even by arithmetic error.
func firmwarePartition(pod disk.IPod, h disk.Handle) fwpart.Partition {
	return fwpart.Partition{
		R:    io.NewSectionReader(h, pod.FWPartStart, pod.FWPartLen),
		Size: pod.FWPartLen,
	}
}

// decorateDeviceError turns the two sentinel failures into something
// the reader can act on.
func decorateDeviceError(err error) error {
	switch {
	case errors.Is(err, disk.ErrNoDevice):
		return fmt.Errorf("%w\n"+
			"  - put the iPod in disk mode: hold Select+Menu to reset, then Select+Play at the Apple logo\n"+
			"  - use a cable that carries data, and a port on the machine rather than a hub\n"+
			"  - `core info --all-disks` lists every disk the OS can see, iPod or not", err)
	case errors.Is(err, disk.ErrRawAccessDenied) || errors.Is(err, os.ErrPermission):
		return fmt.Errorf("%s\n%s", err, elevationAdvice())
	}
	return err
}

// elevationAdvice is the paragraph printed when the OS refused the raw
// open. It quotes the exact command, with this process's own arguments,
// because "run as administrator" is advice nobody can follow without
// retyping a path.
func elevationAdvice() string {
	exe, err := os.Executable()
	if err != nil || exe == "" {
		exe = "core"
	}
	args := os.Args[1:]
	line := disk.ElevationCommand(runtime.GOOS, exe, args)
	if runtime.GOOS == "windows" {
		return "Reading a raw disk on Windows needs an elevated process. Either open an\n" +
			"Administrator console and re-run the command, or paste this (the RunAs child\n" +
			"gets its own console, so the redirect has to happen inside it):\n\n  " +
			line + "\n"
	}
	return "Reading a raw disk needs root. Re-run:\n\n  " + line + "\n"
}

// describeDevice prints the device header shared by `info` and the
// preamble of `backup`.
func describeDevice(out io.Writer, pod disk.IPod) {
	d := pod.Disk
	fmt.Fprintf(out, "disk        %s\n", d.Path)
	fmt.Fprintf(out, "model       %s\n", strings.TrimSpace(d.Vendor+" "+d.Model))
	if d.Serial != "" {
		fmt.Fprintf(out, "serial      %s\n", d.Serial)
	}
	fmt.Fprintf(out, "size        %s\n", disk.HumanSize(d.SizeBytes))
	fmt.Fprintf(out, "sector      %d bytes (%s)\n", pod.SectorSize, sectorSourceText(pod))
	if len(d.Volumes) > 0 {
		vols := make([]string, len(d.Volumes))
		copy(vols, d.Volumes)
		for i, mp := range d.MountPoints {
			if i < len(vols) && mp != "" && mp != vols[i] {
				vols[i] += " on " + mp
			}
		}
		fmt.Fprintf(out, "volumes     %s\n", strings.Join(vols, ", "))
	}
	if pod.Tested {
		fmt.Fprintf(out, "hardware    %s — tested\n", pod.Model)
	} else {
		fmt.Fprintf(out, "hardware    %s — UNTESTED: %s\n", pod.Model, pod.UntestedReason)
	}
}

// sectorSourceText spells out how the sector size was settled. It reads
// like a pedantic detail and is not: every byte offset the flash path
// computes is this number times an LBA, and a mismatch between the
// driver and the bridge is the one way to be off by 96 KB while every
// other number on screen looks right.
func sectorSourceText(pod disk.IPod) string {
	// FindIPods always fills all four MBR slots, but this also runs on
	// a hand-built IPod in tests and in --dry-run fakes, and a panic in
	// the line that explains the sector size would be a poor way to
	// learn that.
	startLBA := uint32(0)
	if len(pod.Partitions) > 0 {
		startLBA = pod.Partitions[0].StartLBA
	}
	switch pod.SectorSizeSource {
	case disk.SectorSizeFromOS:
		return fmt.Sprintf("reported by the OS, confirmed: the Apple preamble is at sector %d",
			startLBA)
	case disk.SectorSizeProbed512:
		return fmt.Sprintf("PROBED: the OS said %d, but the preamble is only at sector %d × 512",
			pod.Disk.SectorSize, startLBA)
	case disk.SectorSizeProbed2048:
		return fmt.Sprintf("PROBED: the OS said %d, but the preamble is only at sector %d × 2048",
			pod.Disk.SectorSize, startLBA)
	default:
		return "source unknown"
	}
}

// describePartitions prints the MBR the way ipodpatcher does, plus the
// byte range each partition works out to at the confirmed sector size.
func describePartitions(out io.Writer, pod disk.IPod) {
	fmt.Fprintf(out, "\npartitions (sectors of %d bytes)\n", pod.SectorSize)
	fmt.Fprintf(out, "  %-2s %-20s %12s %12s %14s %14s  %s\n",
		"#", "type", "start", "end", "offset", "bytes", "")
	for _, p := range pod.Partitions {
		if !p.Used() {
			continue
		}
		note := ""
		switch p.Index {
		case 0:
			note = "firmware (never written except the OSOS body + its directory row)"
		case 1:
			note = "music"
		}
		fmt.Fprintf(out, "  %-2d %-20s %12d %12d %14d %14d  %s\n",
			p.Index, p.TypeName(), p.StartLBA, p.EndLBA(),
			p.ByteStart(pod.SectorSize), p.ByteLength(pod.SectorSize), note)
	}
}

// firmwareVersionLine answers "which build of our firmware is on this
// device?" from the only thing that can answer it: the OSOS body.
//
// The directory entry's `vers` field is Apple's (0xB012, and
// ipodpatcher leaves it alone), and the entry carries a length and a
// checksum but no version, so the image carries its own — a tagged,
// NUL-terminated `CORE-FW-VERSION:<tag>|<build id>` string somewhere in
// .rodata (core/docs/hw/08-boot-dock.md, "Version marker"). It is a
// scan of the body, never a read at an offset: the marker moves with
// every build.
//
// An image with no marker is every image before v0.1.3, which includes
// whatever is on the device right now. That is "unknown" and not an
// error, and the line says which release started carrying one so the
// answer reads as a fact about the image rather than a failure of the
// host.
func firmwareVersionLine(p fwpart.Partition, d *fwpart.Directory) string {
	_, osos, ok := d.OSOS()
	if !ok {
		return "firmware: unknown (this partition has no OSOS image)"
	}
	body, err := fwpart.ReadBody(p, osos)
	if err != nil {
		return "firmware: unknown (the OSOS body could not be read: " + err.Error() + ")"
	}
	// A device that has never been flashed has no marker AND no Core
	// image, and "unknown" there reads as a failure of this program
	// rather than as the fact it is: Apple's firmware is still on the
	// device. The row says so — see fwpart.Classify.
	if inst := fwpart.Classify(osos, body); inst.Kind == fwpart.Other {
		return "firmware: " + inst.Description + " — Core is not installed (core install)"
	}
	return "firmware: " + fwpart.VersionText(body)
}

// firmwareVersionString is the same answer without the label, for
// `info --json`.
func firmwareVersionString(p fwpart.Partition, d *fwpart.Directory) string {
	_, osos, ok := d.OSOS()
	if !ok {
		return "unknown"
	}
	body, err := fwpart.ReadBody(p, osos)
	if err != nil {
		return "unknown"
	}
	if v, _, ok := fwpart.FindVersion(body); ok {
		return v
	}
	return "unknown"
}
