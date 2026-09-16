// Package flasher performs the one write this project makes to a block
// device: replacing the OSOS image on an iPod's firmware partition, or
// restoring a whole partition from a backup.
//
// It exists as its own package, rather than as the body of
// internal/cli/flash.go, for one reason: the sequence in Flash is the
// safety checklist in internal/fwpart/doc.go turned into code, and a
// sequence that can only be run by attaching an 80 GB iPod is a
// sequence that gets reviewed by reading it. Every OS call it makes —
// find the device, open it, lock it, elevate — is a field of Deps, so
// the tests drive the whole thing against disk.NewMemDevice and can
// inject a write that fails halfway.
//
// The order of operations is mandatory and is the order of the code
// below. From the plan (core/docs/design/companion-app-plan.md §S7) and
// the checklist:
//
//  1. parse the input (.ipod checksum verified / .bin plausibility)
//  2. find the device (exactly one, or --device)
//  3. the tested-hardware gate
//  4. open READ-ONLY, parse the directory, verify the current OSOS
//  5. plan the writes — this is where an oversize image is refused
//  6. print the plan; --dry-run stops here, having opened nothing
//     for writing and written no backup
//  7. typed confirmation (the device path, not "y"), unless --yes
//  8. elevation: relaunch on Windows, refuse with the sudo line
//     elsewhere
//  9. back the WHOLE partition up: temp file, fsync, rename, then
//     re-parse the file on disk and require it to validate
//  10. open for WRITE, lock, apply the writes (body first, directory
//     sector second), flush, unlock, close
//  11. re-open read-only and verify the read-back
//  12. print VERIFIED, or the backup path and the fallback line
//
// Steps 9 and 11 are the two that cannot be skipped by any flag: a
// write with no backup on disk and a write nobody read back are the two
// failures with no recovery that does not involve a stranger's disk.
package flasher

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
)

// Options is the command line, minus the plumbing.
type Options struct {
	// Image is a .ipod transport file or a raw core.bin. Exactly one
	// of Image and FromBackup must be set.
	Image string
	// FromBackup is a whole-partition dump (from `core backup` or
	// ipodpatcher -r) to restore. This is the recovery path.
	FromBackup string
	// Device is the global --device selector (path or serial).
	Device string
	// DryRun prints the plan and stops before the confirmation.
	DryRun bool
	// Yes skips the typed confirmation. It does NOT skip the backup,
	// the tested gate or the read-back.
	Yes bool
	// BackupDir overrides <user config dir>/core/backups.
	BackupDir string
	// Untested allows writing to hardware outside the tested window.
	Untested bool
	// NoRelaunch forbids starting an elevated copy of this binary. The
	// elevated child is always given it, so a child that somehow
	// decides it is not elevated refuses instead of spawning another
	// one.
	NoRelaunch bool
	// Install says this write is the first one onto a device that was
	// not running Core: `core install` rather than `core flash`. It
	// changes nothing about the write — the entry-point policy is a
	// property of the image and applies either way — only what the plan
	// calls itself, so the person reading it sees the command they
	// typed.
	Install bool
}

// Deps are the calls that touch the outside world. Every one of them
// has a default; tests replace the ones they care about.
type Deps struct {
	// SelectDevice resolves --device to one iPod (disk.SelectIPod).
	SelectDevice func(selector string) (disk.IPod, error)
	// Open opens a raw device (disk.Open).
	Open func(path string, write bool) (disk.Handle, error)
	// IsElevated reports Administrator/root (disk.IsElevated).
	IsElevated func() bool
	// Relaunch starts an elevated copy of this binary and returns its
	// exit code and everything it printed (disk.RelaunchElevated).
	Relaunch func(args []string) (exitCode int, log string, err error)
	// Executable is os.Executable, for the printed elevation line.
	Executable func() (string, error)
	// GOOS is runtime.GOOS, so the Windows wording can be tested from
	// Linux.
	GOOS string
	// ChildArgs is the argument list a relaunch re-runs — normally
	// os.Args[1:]. --yes and --no-relaunch are added; any existing
	// --elevated-log is dropped so the child gets a fresh one.
	ChildArgs []string
	// Confirm reads the typed confirmation. Returning an error aborts.
	Confirm func(prompt string) (string, error)
	// Out is where the plan and the result are printed. The elevated
	// child's Out is a MultiWriter onto its log file (see
	// internal/cli.openElevatedLog), which is how the parent gets to
	// print what the child did.
	Out io.Writer
	// Now dates the default backup file name.
	Now func() time.Time
	// ConfigDir is os.UserConfigDir.
	ConfigDir func() (string, error)
}

func (d *Deps) setDefaults() {
	if d.SelectDevice == nil {
		d.SelectDevice = disk.SelectIPod
	}
	if d.Open == nil {
		d.Open = disk.Open
	}
	if d.IsElevated == nil {
		d.IsElevated = disk.IsElevated
	}
	if d.Relaunch == nil {
		d.Relaunch = disk.RelaunchElevated
	}
	if d.Executable == nil {
		d.Executable = os.Executable
	}
	if d.GOOS == "" {
		d.GOOS = runtime.GOOS
	}
	if d.ChildArgs == nil && len(os.Args) > 1 {
		d.ChildArgs = os.Args[1:]
	}
	if d.Confirm == nil {
		d.Confirm = promptStdin
	}
	if d.Out == nil {
		d.Out = io.Discard
	}
	if d.Now == nil {
		d.Now = time.Now
	}
	if d.ConfigDir == nil {
		d.ConfigDir = os.UserConfigDir
	}
}

// Mode says which of the two writes this was.
type Mode string

const (
	// ModeOSOS replaces the OSOS body and its directory row.
	ModeOSOS Mode = "osos"
	// ModeRestore rewrites the whole partition from a backup.
	ModeRestore Mode = "restore"
)

// Result is what happened. It is filled in as far as the sequence got,
// so a caller can print the backup path even when Flash returned an
// error.
type Result struct {
	Mode Mode
	// Device is the disk path written (or that would have been).
	Device string
	// DryRun: nothing was opened for writing.
	DryRun bool
	// Aborted: the user declined the typed confirmation.
	Aborted bool
	// Relaunched: an elevated child did the work; ChildExit is its
	// exit code and ChildLog everything it printed.
	Relaunched bool
	ChildExit  int
	ChildLog   string
	// BackupPath is the whole-partition backup, once it is on disk and
	// fsynced. Empty means no backup was taken, which means (by
	// construction) that no byte was written.
	BackupPath string
	// Verified: the read-back matched.
	Verified bool
	// The OSOS entry before and after, for ModeOSOS.
	OldLength, NewLength     uint32
	OldChecksum, NewChecksum uint32
	// OldChecksumOK records whether the image already on the device
	// verified. False is reported, never fatal: a previous bad flash is
	// the thing someone is here to fix.
	OldChecksumOK bool
	// Installed is what was on the device BEFORE this write: Core, an
	// old Core, or something else (Apple's firmware, on every iPod that
	// has never been flashed). It decides the backup's name — see
	// AppleBackupFileName — and it is what `core install` reports as
	// "was:".
	Installed fwpart.Installed
	// BytesWritten is the total handed to WriteAt.
	BytesWritten int64
}

// AppleBackup is the backup path when what this write replaced was NOT
// Core — in practice, the only copy of that device's Apple firmware.
// Empty for a re-flash over one of our own images, and empty before the
// backup has been taken.
//
// The app's Install card and the CLI's last lines both print this one;
// it is a method rather than a second field so it cannot disagree with
// BackupPath.
func (r *Result) AppleBackup() string {
	if r == nil || r.Installed.Kind != fwpart.Other {
		return ""
	}
	return r.BackupPath
}

// Errors callers distinguish.
var (
	// ErrAborted: the typed confirmation did not match.
	ErrAborted = errors.New("flash: aborted at the confirmation prompt")
	// ErrNeedsElevation: this process cannot write and could not (or
	// was not allowed to) start one that can. The message carries the
	// exact command.
	ErrNeedsElevation = errors.New("flash: writing a raw disk needs an elevated process")
	// ErrBadInput: the file handed in is not something to flash.
	ErrBadInput = errors.New("flash: input rejected")
)

// Flash runs the whole sequence. ctx is checked between steps; there is
// no point aborting in the middle of a 368 KB WriteAt, and every point
// where stopping is safe is a point where ctx is looked at.
func Flash(ctx context.Context, o Options, d Deps) (*Result, error) {
	d.setDefaults()
	f := &flash{o: o, d: d, out: d.Out, res: &Result{DryRun: o.DryRun}}
	defer func() { _ = f.restore.Close() }()
	err := f.run(ctx)
	return f.res, err
}

type flash struct {
	o   Options
	d   Deps
	out io.Writer
	res *Result

	// the input, step 1
	img      *imageInput
	restore  *restoreInput
	pod      disk.IPod
	writes   []fwpart.Write
	newEntry firmware.DirectoryEntry

	// backupAt is the backup path decided ONCE, when the plan is
	// printed, and used verbatim by backup(). Computing it twice would
	// call Now() twice, and a second-boundary between the two would
	// print one file name and write another.
	backupAt string
	// planShown: the plan reached the user. The elevation advice uses
	// it — a command with --yes in it must never be handed to someone
	// who has not seen what it will do.
	planShown bool
}

func (f *flash) run(ctx context.Context) error {
	if err := f.parseInput(); err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := f.findDevice(); err != nil {
		return err
	}
	if err := f.gateHardware(); err != nil {
		return err
	}
	if err := f.inspectAndPlan(); err != nil {
		return err
	}
	if f.o.DryRun {
		fmt.Fprintf(f.out, "\n--dry-run: nothing was opened for writing and no backup was taken.\n")
		return nil
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := f.confirm(); err != nil {
		return err
	}
	relaunched, err := f.elevate()
	if err != nil || relaunched {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	if err := f.backup(); err != nil {
		return err
	}
	return f.writeAndVerify(ctx)
}

// --- step 1: the input ------------------------------------------------

// imageInput is a parsed OSOS image: the body bytes, and how we know
// they are an image.
type imageInput struct {
	path  string
	kind  string // ".ipod transport file" | "raw image"
	image []byte
	sum   uint32
}

// restoreInput is a whole-partition file that has already proved
// itself: preamble, directory, OSOS checksum.
type restoreInput struct {
	path   string
	file   *os.File
	size   int64
	osos   firmware.DirectoryEntry
	images int
}

func (f *flash) parseInput() error {
	haveImage, haveBackup := f.o.Image != "", f.o.FromBackup != ""
	switch {
	case haveImage && haveBackup:
		return fmt.Errorf("%w: pass either an image or --from-backup, not both", ErrBadInput)
	case !haveImage && !haveBackup:
		return fmt.Errorf("%w: nothing to write; pass <core.ipod | core.bin> or --from-backup <fwpart.bin>",
			ErrBadInput)
	case haveBackup:
		f.res.Mode = ModeRestore
		return f.parseBackup()
	default:
		f.res.Mode = ModeOSOS
		return f.parseImage()
	}
}

// maxInputFile bounds the read of an image file. The OSOS capacity on
// the device is 7.6 MB; 64 MiB is generous and still refuses to load a
// partition dump into memory by accident.
const maxInputFile = 64 << 20

func (f *flash) parseImage() error {
	path := f.o.Image
	st, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("%w: %v", ErrBadInput, err)
	}
	if st.IsDir() {
		return fmt.Errorf("%w: %s is a directory", ErrBadInput, path)
	}
	if st.Size() > maxInputFile {
		return fmt.Errorf("%w: %s is %d bytes; no firmware image is that large "+
			"(a whole-partition dump goes to --from-backup)", ErrBadInput, path, st.Size())
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("%w: read %s: %v", ErrBadInput, path, err)
	}

	// A partition dump handed in as an image is the mistake with the
	// worst outcome: its first bytes are Apple's preamble, which would
	// be flashed into the OSOS body as if it were code. It is also an
	// easy mistake, because both files sit in the same folder.
	if fwpart.CheckPreamble(data) == nil {
		return fmt.Errorf("%w: %s starts with the Apple firmware-partition preamble, so it is a "+
			"whole-partition dump, not a firmware image. To restore it, run:\n  core flash --from-backup %s",
			ErrBadInput, path, path)
	}

	in := &imageInput{path: path}
	if isIPodFile(path, data) {
		name, image, err := firmware.ReadIPodFile(bytes.NewReader(data))
		if err != nil {
			// Fatal. `firmware unpack --ignore-checksum` exists for
			// looking at a damaged image; nothing may flash one.
			return fmt.Errorf("%w: %s does not verify as a .ipod file: %v", ErrBadInput, path, err)
		}
		// The header names a model. Only the Video's tag may go onto a
		// Video: a nano image verifies just as well as a .ipod file and
		// boots to nothing at all on this SoC.
		if name != firmware.ModelNameIPodVideo {
			return fmt.Errorf("%w: %s is a .ipod file for model %q, not %q (iPod Video); "+
				"refusing to flash another model's image", ErrBadInput, path,
				string(name[:]), string(firmware.ModelNameIPodVideo[:]))
		}
		in.kind = fmt.Sprintf(".ipod transport file, model %q, header checksum OK", string(name[:]))
		in.image = image
	} else {
		in.kind = "raw image (no .ipod header)"
		in.image = data
	}
	if err := firmware.ValidateImage(path, "flash", in.image, f.out); err != nil {
		return fmt.Errorf("%w: %v", ErrBadInput, err)
	}
	in.sum = fwpart.ImageChecksum(in.image)
	f.img = in
	f.res.NewLength = uint32(len(in.image))
	f.res.NewChecksum = in.sum
	return nil
}

// isIPodFile decides whether to read the file through the .ipod
// transport codec. The extension is authoritative when it is there; a
// file without one is recognized by the 4-byte model name at offset 4,
// which is how `firmware inspect` classifies it too.
func isIPodFile(path string, data []byte) bool {
	if strings.EqualFold(filepath.Ext(path), ".ipod") {
		return true
	}
	if len(data) < firmware.IPodFileHeaderSize {
		return false
	}
	var name firmware.ModelName
	copy(name[:], data[4:8])
	_, ok := firmware.ModelNumForName(name)
	return ok
}

func (f *flash) parseBackup() error {
	path := f.o.FromBackup
	file, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("%w: %v", ErrBadInput, err)
	}
	ok := false
	defer func() {
		if !ok {
			file.Close()
		}
	}()
	st, err := file.Stat()
	if err != nil {
		return fmt.Errorf("%w: stat %s: %v", ErrBadInput, path, err)
	}
	in := &restoreInput{path: path, file: file, size: st.Size()}

	head := make([]byte, fwpart.PreambleWindow)
	if _, err := file.ReadAt(head, 0); err != nil {
		return fmt.Errorf("%w: %s is too short to be a firmware partition: %v",
			ErrBadInput, path, err)
	}
	if err := fwpart.CheckPreamble(head); err != nil {
		return fmt.Errorf("%w: %s is not a firmware-partition image: %w", ErrBadInput, path, err)
	}
	p := fwpart.Partition{R: file, Size: in.size}
	dir, err := fwpart.Parse(p)
	if err != nil {
		return fmt.Errorf("%w: %s: %w", ErrBadInput, path, err)
	}
	_, osos, found := dir.OSOS()
	if !found {
		return fmt.Errorf("%w: %s has no OSOS image; restoring it would leave the device "+
			"with no operating system", ErrBadInput, path)
	}
	// Unlike the device's own OSOS, a BAD checksum here IS fatal. The
	// device's may be bad because that is what we are fixing; a backup
	// whose OS image does not verify is a backup that would not boot,
	// and writing it over a partition that might still be recoverable
	// makes things worse.
	if err := fwpart.VerifyEntry(p, osos); err != nil {
		return fmt.Errorf("%w: the OSOS image in %s does not verify: %v\n"+
			"Refusing to restore a backup that would not boot.", ErrBadInput, path, err)
	}
	in.osos = osos
	in.images = len(dir.Entries)
	f.restore = in
	f.res.NewLength = osos.Length
	f.res.NewChecksum = osos.Checksum
	ok = true
	return nil
}

// --- step 2 and 3: the device, and the hardware gate ------------------

func (f *flash) findDevice() error {
	pod, err := f.d.SelectDevice(f.o.Device)
	if err != nil {
		if isDenied(err) {
			return fmt.Errorf("%v\n\n%s", err, f.elevationAdvice())
		}
		return err
	}
	f.pod = pod
	f.res.Device = pod.Disk.Path
	return nil
}

func (f *flash) gateHardware() error {
	if f.pod.Tested || f.o.Untested {
		return nil
	}
	return &disk.UntestedHardwareError{Pod: f.pod}
}

// --- steps 4 and 5: inspect, plan, print ------------------------------

func (f *flash) inspectAndPlan() error {
	h, err := f.d.Open(f.pod.Disk.Path, false)
	if err != nil {
		if isDenied(err) {
			return fmt.Errorf("open %s for reading: %v\n\n%s",
				f.pod.Disk.Path, err, f.elevationAdvice())
		}
		return fmt.Errorf("open %s for reading: %w", f.pod.Disk.Path, err)
	}
	defer h.Close()

	p := f.partition(h)
	dir, err := fwpart.Parse(p)
	if err != nil {
		return fmt.Errorf("reading the firmware partition on %s: %w", f.pod.Disk.Path, err)
	}
	idx, osos, found := dir.OSOS()
	if !found {
		return fmt.Errorf("%s: %w", f.pod.Disk.Path, fwpart.ErrNoOSOS)
	}
	f.res.OldLength, f.res.OldChecksum = osos.Length, osos.Checksum
	// A BAD checksum on what is already there is reported, never fatal.
	verifyErr := fwpart.VerifyEntry(p, osos)
	f.res.OldChecksumOK = verifyErr == nil

	// What is on the device decides one thing here: the name of the
	// backup. A device running Apple's firmware has exactly one copy of
	// it in the world — the one this run is about to overwrite — and a
	// file called fwpart-131475456-2026-09-15T01-02-03Z.bin is not a
	// file anyone finds a year later.
	f.res.Installed = fwpart.ClassifyEntry(p, osos)

	backupPath, err := f.backupPath()
	if err != nil {
		return err
	}
	f.backupAt = backupPath

	if f.res.Mode == ModeRestore {
		if f.restore.size != f.pod.FWPartLen {
			return fmt.Errorf("%w: %s is %d bytes and this device's firmware partition is %d; "+
				"a whole-partition restore has to be the same size, and a size mismatch means the "+
				"backup came from a different model", ErrBadInput,
				f.restore.path, f.restore.size, f.pod.FWPartLen)
		}
	} else {
		// fwpart.CoreImage, always, and not a field of Options: every
		// image this command writes is a Core image, whose entry point
		// is its first byte on every device (library-manager-plan.md,
		// decision 1). An option here would be an option whose only
		// other value produces a device that does not boot — and whose
		// zero value, fwpart.KeepEntry, is exactly that one.
		writes, entry, err := fwpart.PlanWrite(dir, idx, f.img.image, f.pod.SectorSize, fwpart.CoreImage)
		if err != nil {
			// Oversize lands here, before anything is opened for
			// writing and before a backup is taken.
			return err
		}
		f.writes, f.newEntry = writes, entry
	}

	f.printPlan(dir, idx, osos, verifyErr, backupPath)
	f.planShown = true
	return nil
}

func (f *flash) partition(h disk.Handle) fwpart.Partition {
	return fwpart.Partition{
		R:    io.NewSectionReader(h, f.pod.FWPartStart, f.pod.FWPartLen),
		Size: f.pod.FWPartLen,
	}
}

func (f *flash) printPlan(dir *fwpart.Directory, idx int, osos firmware.DirectoryEntry,
	verifyErr error, backupPath string) {
	out := f.out
	pod := f.pod

	name := "core flash"
	if f.o.Install {
		name = "core install"
	}
	fmt.Fprintf(out, "%s — plan\n\n", name)
	if f.res.Mode == ModeRestore {
		fmt.Fprintf(out, "  restore      %s\n", f.restore.path)
		fmt.Fprintf(out, "               %d bytes, preamble OK, %d images, OSOS %d bytes checksum %#08x OK\n",
			f.restore.size, f.restore.images, f.restore.osos.Length, f.restore.osos.Checksum)
	} else {
		fmt.Fprintf(out, "  image        %s\n", f.img.path)
		fmt.Fprintf(out, "               %s\n", f.img.kind)
		fmt.Fprintf(out, "               %d bytes, plain sum %#08x, body write %d bytes zero-padded to %#x\n",
			len(f.img.image), f.img.sum, padTo(len(f.img.image), fwpart.BodyAlign), fwpart.BodyAlign)
	}

	fmt.Fprintf(out, "\n  device       %s\n", pod.Disk.Path)
	fmt.Fprintf(out, "  model        %s\n", strings.TrimSpace(pod.Disk.Vendor+" "+pod.Disk.Model))
	if pod.Disk.Serial != "" {
		fmt.Fprintf(out, "  serial       %s\n", pod.Disk.Serial)
	}
	fmt.Fprintf(out, "  hardware     %s", pod.Model)
	if pod.Tested {
		fmt.Fprintf(out, " — tested\n")
	} else {
		fmt.Fprintf(out, " — UNTESTED (--untested-hardware given): %s\n", pod.UntestedReason)
	}
	fmt.Fprintf(out, "  sector       %d bytes (%s)\n", pod.SectorSize, pod.SectorSizeSource)
	fmt.Fprintf(out, "  partition    %d bytes at disk offset %#x\n", pod.FWPartLen, pod.FWPartStart)

	fmt.Fprintf(out, "\n  OSOS entry   row %d at partition %#x, devOffset %#x, body %#x\n",
		idx, dir.EntryOffset(idx), osos.DevOffset, fwpart.BodyOffset(osos))
	fmt.Fprintf(out, "  capacity     %d bytes\n", dir.Capacity(idx))
	if f.res.Mode == ModeOSOS {
		fmt.Fprintf(out, "  length       %d → %d\n", f.res.OldLength, f.res.NewLength)
		fmt.Fprintf(out, "  checksum     %#08x → %#08x\n", f.res.OldChecksum, f.res.NewChecksum)
		// The field that makes a stock iPod bootable. It is printed
		// only when it changes, because on a device already running
		// Core it is 0 → 0 and a line saying so is noise.
		if osos.EntryOffset != f.newEntry.EntryOffset {
			fmt.Fprintf(out, "  entryOffset  %#x → %#x  (this image starts at its first byte)\n",
				osos.EntryOffset, f.newEntry.EntryOffset)
		}
	} else {
		fmt.Fprintf(out, "  length       %d → %d (from the backup)\n", f.res.OldLength, f.res.NewLength)
		fmt.Fprintf(out, "  checksum     %#08x → %#08x (from the backup)\n", f.res.OldChecksum, f.res.NewChecksum)
	}
	fmt.Fprintf(out, "  installed    %s\n", f.res.Installed.Description)
	if verifyErr != nil {
		fmt.Fprintf(out, "  current      the image already on the device does NOT verify: %v\n", verifyErr)
		fmt.Fprintf(out, "               (reported, not fatal — a previous bad flash is what this may be fixing)\n")
	} else {
		fmt.Fprintf(out, "  current      the image already on the device verifies\n")
	}

	fmt.Fprintf(out, "\n  writes\n")
	if f.res.Mode == ModeRestore {
		fmt.Fprintf(out, "    the whole partition: %d bytes at partition 0x0 (disk %#x), in %d-byte-aligned chunks\n",
			f.restore.size, pod.FWPartStart, pod.SectorSize)
		fmt.Fprintf(out, "    this rewrites the preamble, the directory and EVERY image, including Apple's\n")
	} else {
		labels := []string{"body", "directory sector"}
		for i, w := range f.writes {
			label := ""
			if i < len(labels) {
				label = "  " + labels[i]
			}
			fmt.Fprintf(out, "    %d. %8d bytes at partition %#x (disk %#x)%s\n",
				i+1, len(w.Data), w.Off, pod.FWPartStart+w.Off, label)
		}
		fmt.Fprintf(out, "    the preamble, the partition table and every other directory row are not written\n")
	}
	fmt.Fprintf(out, "\n  backup       %s\n", backupPath)
}

func padTo(n, align int) int { return (n + align - 1) &^ (align - 1) }

// --- step 7: the typed confirmation -----------------------------------

func (f *flash) confirm() error {
	target := f.pod.Disk.Path
	if f.o.Yes {
		fmt.Fprintf(f.out, "\n--yes: skipping the typed confirmation for %s.\n", target)
		return nil
	}
	got, err := f.d.Confirm(ConfirmPrompt(target))
	if err != nil {
		return fmt.Errorf("%w: reading the confirmation: %v", ErrAborted, err)
	}
	if strings.TrimSpace(got) != target {
		f.res.Aborted = true
		return fmt.Errorf("%w: typed %q, expected %q", ErrAborted, strings.TrimSpace(got), target)
	}
	return nil
}

// confirmPromptPrefix and confirmPromptSuffix frame the device path in
// the prompt handed to Deps.Confirm. They are shared with ConfirmTarget
// so a GUI that has to know what to demand can read it back from the
// prompt instead of guessing at the wording — the first core-app
// looked for a parenthesised path that was never there, and every
// flash through the window aborted at the confirmation.
const (
	confirmPromptPrefix = "\nThis will write to "
	confirmPromptSuffix = ". Type the device path exactly to continue: "
)

// ConfirmPrompt is the text Deps.Confirm receives: the device path
// framed by a sentence. Confirm must return exactly that path.
func ConfirmPrompt(target string) string {
	return confirmPromptPrefix + target + confirmPromptSuffix
}

// ConfirmTarget pulls the device path back out of a ConfirmPrompt. ok is
// false for any other text.
func ConfirmTarget(prompt string) (target string, ok bool) {
	if !strings.HasPrefix(prompt, confirmPromptPrefix) || !strings.HasSuffix(prompt, confirmPromptSuffix) {
		return "", false
	}
	t := prompt[len(confirmPromptPrefix) : len(prompt)-len(confirmPromptSuffix)]
	if t == "" {
		return "", false
	}
	return t, true
}

// promptStdin is the default Confirm: one line from the terminal.
func promptStdin(prompt string) (string, error) {
	fmt.Fprint(os.Stdout, prompt)
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil && line == "" {
		return "", err
	}
	return line, nil
}

// --- step 8: elevation ------------------------------------------------

// elevate decides whether this process may write, and on Windows starts
// the process that can.
//
// Returns relaunched=true when an elevated child ran the whole command;
// the caller then stops, because the child did the work.
func (f *flash) elevate() (relaunched bool, err error) {
	need := disk.DecideElevation(f.d.GOOS, f.d.IsElevated(), false, true)
	if need == disk.NeedNone {
		return false, nil
	}
	if need == disk.NeedSudo || f.o.NoRelaunch {
		// The loop guard. An elevated child is always launched with
		// --no-relaunch, so a child that for any reason believes it is
		// not elevated refuses here instead of spawning a third
		// process, and a fourth, until the user kills the tree.
		return false, fmt.Errorf("%w\n\n%s", ErrNeedsElevation, f.elevationAdvice())
	}

	args := childArgs(f.d.ChildArgs)
	fmt.Fprintf(f.out, "\nnot elevated: starting an elevated copy of this command.\n"+
		"  args: %s\n", strings.Join(args, " "))
	code, log, err := f.d.Relaunch(args)
	f.res.Relaunched, f.res.ChildExit, f.res.ChildLog = true, code, log
	if err != nil {
		return true, fmt.Errorf("%w: %v", ErrNeedsElevation, err)
	}
	if log != "" {
		fmt.Fprintf(f.out, "\n--- elevated process output ---\n%s", log)
		if !strings.HasSuffix(log, "\n") {
			fmt.Fprintln(f.out)
		}
		fmt.Fprintf(f.out, "--- end of elevated process output (exit %d) ---\n", code)
	} else {
		fmt.Fprintf(f.out, "\nthe elevated process exited %d and wrote nothing to its log\n", code)
	}
	if code != 0 {
		return true, fmt.Errorf("the elevated process failed (exit %d); its output is above", code)
	}
	return true, nil
}

// childArgs builds the argument list for the elevated child: the user's
// own arguments, minus anything that would make the child prompt or
// relaunch, plus --yes and --no-relaunch. RelaunchElevated appends
// --elevated-log itself.
func childArgs(args []string) []string {
	out := make([]string, 0, len(args)+2)
	skipNext := false
	for _, a := range args {
		if skipNext {
			skipNext = false
			continue
		}
		switch {
		case a == disk.ElevatedLogFlag:
			skipNext = true
		case strings.HasPrefix(a, disk.ElevatedLogFlag+"="):
		case a == "--yes" || a == "-y" || a == "--no-relaunch":
		default:
			out = append(out, a)
		}
	}
	return append(out, "--yes", "--no-relaunch")
}

// elevationAdvice is the exact command to run instead.
func (f *flash) elevationAdvice() string {
	exe, err := f.d.Executable()
	if err != nil || exe == "" {
		exe = "core"
	}
	args := childArgs(f.d.ChildArgs)
	// The command a human runs by hand does not want --no-relaunch in
	// it; that flag exists for the machine-started child.
	args = args[:len(args)-1]
	line := disk.ElevationCommand(f.d.GOOS, exe, args)
	if f.d.GOOS == "windows" {
		msg := "Writing a raw disk on Windows needs an elevated process. Open an Administrator\n" +
			"console and re-run the command, or paste this (a RunAs child gets its own\n" +
			"console, so the redirect has to happen inside it):\n\n  " + line + "\n"
		if !f.planShown {
			// The line above carries --yes, because a RunAs child
			// cannot take a typed confirmation through a redirect.
			// Nobody has seen a plan yet in this case (the device could
			// not even be opened for reading), so the first thing to
			// paste is the read-only version.
			dry := disk.ElevationCommand(f.d.GOOS, exe, dryRunArgs(args))
			msg = "Writing a raw disk on Windows needs an elevated process, and this one could\n" +
				"not even read the device, so no plan has been shown. First, to see the plan\n" +
				"without writing anything (the output lands in core-out.txt next to the exe):\n\n  " +
				dry + "\n\nthen, once the plan reads right, the same command with --yes instead of --dry-run:\n\n  " +
				line + "\n"
		}
		return msg
	}
	return "Writing a raw disk needs root. Re-run:\n\n  " + line + "\n"
}

// dryRunArgs turns the child argument list into its read-only form:
// --yes out, --dry-run in.
func dryRunArgs(args []string) []string {
	out := make([]string, 0, len(args)+1)
	for _, a := range args {
		if a == "--yes" || a == "-y" || a == "--dry-run" {
			continue
		}
		out = append(out, a)
	}
	return append(out, "--dry-run")
}

// --- step 9: the backup -----------------------------------------------

// backupPath decides where the whole-partition backup goes, once.
//
// The NAME depends on what is being overwritten. Anything that is not
// Core — Apple's firmware on a device that has never been flashed, or
// somebody else's — gets the apple-<serial>-<date> name, because that
// file is the only copy of that device's original firmware in
// existence and it has to be findable by a person a year later. A
// re-flash over our own image gets the ordinary fwpart-<size>-<time>
// name.
func (f *flash) backupPath() (string, error) {
	name := BackupFileName(f.pod.FWPartLen, f.d.Now())
	apple := f.res.Installed.Kind == fwpart.Other
	if apple {
		name = AppleBackupFileName(f.pod.Disk.Serial, f.d.Now())
	}
	dir := f.o.BackupDir
	if dir == "" {
		cfg, err := f.d.ConfigDir()
		if err != nil {
			return "", fmt.Errorf("locating the user config directory for the backup "+
				"(pass --backup-dir to choose one): %w", err)
		}
		dir = filepath.Join(cfg, "core", "backups")
	}
	path := filepath.Join(dir, name)
	if apple {
		// Only the apple- name needs this. The ordinary name carries a
		// timestamp to the second, so a collision there means the clock
		// stood still, and writeFileAtomic's refusal is the right
		// answer — see TestBackupIsNeverOverwritten.
		path = unusedPath(path)
	}
	return path, nil
}

// unusedPath returns path, or path with a -2, -3 … before the extension
// if something is already there.
//
// writeFileAtomic refuses to overwrite a backup, which is right, and
// the apple- name carries only a date — so a second install on the same
// day (restore Apple, install again) would otherwise abort the flash at
// the backup step. Renaming the new file is the only answer that
// neither loses the old backup nor blocks the write.
func unusedPath(path string) string {
	if _, err := os.Stat(path); err != nil {
		return path
	}
	ext := filepath.Ext(path)
	stem := path[:len(path)-len(ext)]
	for n := 2; n < 1000; n++ {
		candidate := fmt.Sprintf("%s-%d%s", stem, n, ext)
		if _, err := os.Stat(candidate); err != nil {
			return candidate
		}
	}
	return path
}

// backup copies the WHOLE firmware partition to a file, fsyncs it,
// renames it into place, and then re-opens the file and requires it to
// parse. Nothing downstream runs unless all of that succeeded.
//
// Re-parsing the file rather than the bytes we just streamed is the
// point: it proves the preamble, the directory and the OSOS checksum
// survived onto the disk, after the fsync, rather than proving
// something about a buffer.
func (f *flash) backup() error {
	path := f.backupAt
	if path == "" {
		var err error
		if path, err = f.backupPath(); err != nil {
			return err
		}
	}
	if dir := filepath.Dir(path); dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return fmt.Errorf("create the backup directory %s: %w", dir, err)
		}
	}
	h, err := f.d.Open(f.pod.Disk.Path, false)
	if err != nil {
		return fmt.Errorf("re-open %s to back it up: %w", f.pod.Disk.Path, err)
	}
	defer h.Close()

	fmt.Fprintf(f.out, "\nbacking up the whole firmware partition (%d bytes) …\n", f.pod.FWPartLen)
	src := io.NewSectionReader(h, f.pod.FWPartStart, f.pod.FWPartLen)
	if err := writeFileAtomic(path, func(w io.Writer) error {
		n, err := io.Copy(w, src)
		if err != nil {
			return fmt.Errorf("read %s: %w", f.pod.Disk.Path, err)
		}
		if n != f.pod.FWPartLen {
			return fmt.Errorf("read %d of %d bytes from %s", n, f.pod.FWPartLen, f.pod.Disk.Path)
		}
		return nil
	}); err != nil {
		return fmt.Errorf("the backup failed, so nothing was written to the device: %w", err)
	}
	if err := verifyBackupFile(path, f.pod.FWPartLen); err != nil {
		return fmt.Errorf("the backup on disk does not validate, so nothing was written to "+
			"the device: %w", err)
	}
	f.res.BackupPath = path
	fmt.Fprintf(f.out, "backup       %s (fsynced, re-parsed, OSOS present)\n", path)
	return nil
}

// verifyBackupFile re-reads a just-written backup and requires it to be
// a firmware partition with an OSOS entry.
//
// The OSOS checksum is checked and reported but not required: the whole
// reason to keep a backup of a device whose image is broken is to be
// able to put the broken image back if the replacement is worse.
func verifyBackupFile(path string, wantSize int64) error {
	file, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("re-open the backup: %w", err)
	}
	defer file.Close()
	st, err := file.Stat()
	if err != nil {
		return fmt.Errorf("stat the backup: %w", err)
	}
	if st.Size() != wantSize {
		return fmt.Errorf("the backup is %d bytes, the partition is %d", st.Size(), wantSize)
	}
	head := make([]byte, fwpart.PreambleWindow)
	if _, err := file.ReadAt(head, 0); err != nil {
		return fmt.Errorf("read the backup: %w", err)
	}
	if err := fwpart.CheckPreamble(head); err != nil {
		return err
	}
	p := fwpart.Partition{R: file, Size: st.Size()}
	dir, err := fwpart.Parse(p)
	if err != nil {
		return err
	}
	if _, _, ok := dir.OSOS(); !ok {
		return fwpart.ErrNoOSOS
	}
	return nil
}

// --- steps 10 and 11: write, then prove it ----------------------------

func (f *flash) writeAndVerify(ctx context.Context) error {
	writeErr := f.doWrite(ctx)
	verifyErr := f.doVerify()
	if writeErr == nil && verifyErr == nil {
		f.res.Verified = true
		fmt.Fprintf(f.out, "\nVERIFIED — %d bytes written to %s and read back byte for byte.\n",
			f.res.BytesWritten, f.pod.Disk.Path)
		fmt.Fprintf(f.out, "backup       %s\n", f.res.BackupPath)
		if line := f.keptFirmwareLine(); line != "" {
			fmt.Fprintf(f.out, "%s\n", line)
		}
		return nil
	}
	return f.failure(writeErr, verifyErr)
}

func (f *flash) doWrite(ctx context.Context) error {
	h, err := f.d.Open(f.pod.Disk.Path, true)
	if err != nil {
		if isDenied(err) {
			return fmt.Errorf("open %s for writing: %v\n\n%s",
				f.pod.Disk.Path, err, f.elevationAdvice())
		}
		return fmt.Errorf("open %s for writing: %w", f.pod.Disk.Path, err)
	}
	// Close last, after the unlock, and report a Close failure: on
	// Windows a deferred write can surface there and nowhere else.
	var closed bool
	defer func() {
		if !closed {
			h.Close()
		}
	}()

	if err := h.Lock(); err != nil {
		return fmt.Errorf("lock %s (close anything using the iPod's disk and retry): %w",
			f.pod.Disk.Path, err)
	}
	unlocked := false
	defer func() {
		if !unlocked {
			_ = h.Unlock()
		}
	}()

	fmt.Fprintf(f.out, "writing …\n")
	var writeErr error
	if f.res.Mode == ModeRestore {
		writeErr = f.writeWholePartition(ctx, h)
	} else {
		writeErr = f.writePlannedWrites(h)
	}

	// Flush, unlock and close happen whatever the write did: bytes
	// already in the OS's cache have to reach the disk before the
	// read-back, and a device left locked is a device the user has to
	// replug.
	if err := h.Flush(); err != nil && writeErr == nil {
		writeErr = fmt.Errorf("flush %s: %w", f.pod.Disk.Path, err)
	}
	if err := h.Unlock(); err != nil && writeErr == nil {
		writeErr = fmt.Errorf("unlock %s: %w", f.pod.Disk.Path, err)
	}
	unlocked = true
	closed = true
	if err := h.Close(); err != nil && writeErr == nil {
		writeErr = fmt.Errorf("close %s: %w", f.pod.Disk.Path, err)
	}
	return writeErr
}

// writePlannedWrites performs fwpart's write set in order: body first,
// directory sector second. The order is the recovery story — an
// interrupted flash leaves the OLD entry pointing at a half-written
// body, which the boot ROM refuses, rather than a NEW entry pointing at
// bytes that were never written.
func (f *flash) writePlannedWrites(h disk.Handle) error {
	for i, w := range f.writes {
		off := f.pod.FWPartStart + w.Off
		n, err := h.WriteAt(w.Data, off)
		f.res.BytesWritten += int64(n)
		if err != nil {
			return fmt.Errorf("write %d of %d (%d bytes at disk %#x): %w",
				i+1, len(f.writes), len(w.Data), off, err)
		}
		if n != len(w.Data) {
			return fmt.Errorf("write %d of %d: %d of %d bytes at disk %#x",
				i+1, len(f.writes), n, len(w.Data), off)
		}
		fmt.Fprintf(f.out, "  wrote %d bytes at partition %#x\n", n, w.Off)
	}
	return nil
}

// restoreChunk is the unit a whole-partition restore is written in. It
// is a multiple of every sector size in play, so the aligned handle
// never has to read-modify-write.
const restoreChunk = 4 << 20

func (f *flash) writeWholePartition(ctx context.Context, h disk.Handle) error {
	// ctx is honoured only BEFORE the first chunk. The first 4 MiB
	// carries the preamble and the directory; from the moment it lands,
	// the partition describes bodies that are not there yet, and the
	// only state that boots is the fully restored one. Stopping halfway
	// on a Ctrl-C would leave exactly what the backup exists to undo.
	if err := ctx.Err(); err != nil {
		return err
	}
	buf := make([]byte, restoreChunk)
	var off int64
	for off < f.restore.size {
		n := int64(len(buf))
		if rem := f.restore.size - off; rem < n {
			n = rem
		}
		chunk := buf[:n]
		if _, err := f.restore.file.ReadAt(chunk, off); err != nil {
			return fmt.Errorf("read %s at %d: %w", f.restore.path, off, err)
		}
		wrote, err := h.WriteAt(chunk, f.pod.FWPartStart+off)
		f.res.BytesWritten += int64(wrote)
		if err != nil {
			return fmt.Errorf("write %d bytes at disk %#x: %w",
				len(chunk), f.pod.FWPartStart+off, err)
		}
		if int64(wrote) != n {
			return fmt.Errorf("write at disk %#x: %d of %d bytes",
				f.pod.FWPartStart+off, wrote, n)
		}
		off += n
	}
	fmt.Fprintf(f.out, "  wrote %d bytes over the whole partition\n", off)
	return nil
}

// doVerify re-opens the device READ-ONLY and compares. A fresh handle,
// not the one that wrote, so nothing can be served back out of a cache
// the writer owns.
func (f *flash) doVerify() error {
	h, err := f.d.Open(f.pod.Disk.Path, false)
	if err != nil {
		return fmt.Errorf("re-open %s to verify the write: %w", f.pod.Disk.Path, err)
	}
	defer h.Close()
	p := f.partition(h)

	if f.res.Mode == ModeRestore {
		return f.verifyWholePartition(p)
	}
	if err := fwpart.VerifyWritten(p, f.newEntry, f.img.image); err != nil {
		return err
	}
	fmt.Fprintf(f.out, "read-back    OSOS body %d bytes and the directory row match\n",
		f.newEntry.Length)
	return nil
}

func (f *flash) verifyWholePartition(p fwpart.Partition) error {
	want := make([]byte, restoreChunk)
	got := make([]byte, restoreChunk)
	var off int64
	for off < f.restore.size {
		n := int64(len(want))
		if rem := f.restore.size - off; rem < n {
			n = rem
		}
		if _, err := f.restore.file.ReadAt(want[:n], off); err != nil {
			return fmt.Errorf("re-read %s at %d: %w", f.restore.path, off, err)
		}
		if _, err := p.R.ReadAt(got[:n], off); err != nil {
			return fmt.Errorf("%w: re-read the partition at %#x: %v", fwpart.ErrVerify, off, err)
		}
		if !bytes.Equal(want[:n], got[:n]) {
			for i := int64(0); i < n; i++ {
				if want[i] != got[i] {
					return fmt.Errorf("%w: the partition differs from %s at byte %d",
						fwpart.ErrVerify, f.restore.path, off+i)
				}
			}
		}
		off += n
	}
	fmt.Fprintf(f.out, "read-back    all %d partition bytes match %s\n", off, f.restore.path)
	return nil
}

// --- the failure report -----------------------------------------------

// failure is the message printed when anything after the backup went
// wrong. It has one job: tell someone holding a device that may not
// boot exactly what to run.
func (f *flash) failure(writeErr, verifyErr error) error {
	var b strings.Builder
	if writeErr != nil {
		fmt.Fprintf(&b, "the write to %s failed: %v\n", f.pod.Disk.Path, writeErr)
	}
	if verifyErr != nil {
		fmt.Fprintf(&b, "the read-back of %s did NOT verify: %v\n", f.pod.Disk.Path, verifyErr)
	}
	b.WriteString("\n")
	b.WriteString(f.Fallback())
	return errors.New(strings.TrimRight(b.String(), "\n"))
}

// Fallback is the recovery paragraph: the backup, the command that puts
// it back, and ipodpatcher. Exported because `core flash` prints it
// from the CLI layer too.
func (f *flash) Fallback() string {
	var b strings.Builder
	b.WriteString("RECOVERY\n")
	if f.res.BackupPath != "" {
		fmt.Fprintf(&b, "  The partition as it was before this write is saved at:\n    %s\n",
			f.res.BackupPath)
		if f.res.Installed.Kind == fwpart.Other {
			fmt.Fprintf(&b, "    (%s — this is the only copy of it there is; keep the file)\n",
				f.res.Installed.Description)
		}
		fmt.Fprintf(&b, "  Put it back with:\n    core flash --from-backup %s --yes\n",
			quoteIfNeeded(f.res.BackupPath))
	} else {
		b.WriteString("  No backup was taken, which means nothing was written to the device.\n")
	}
	n := ipodpatcherDisk(f.pod.Disk.Path)
	// ipodpatcher's verb depends on what the file is: -wf takes a .ipod
	// (header + image), -wfb a bare image, -w a whole partition dump.
	// Handing a .bin to -wf would flash 8 bytes of garbage in front of
	// the code.
	switch {
	case f.o.FromBackup != "":
		fmt.Fprintf(&b, "  Or write the whole partition back with ipodpatcher:\n    ipodpatcher %d -w %s\n",
			n, quoteIfNeeded(f.o.FromBackup))
	case f.img != nil && strings.HasPrefix(f.img.kind, ".ipod"):
		fmt.Fprintf(&b, "  Or flash the image with ipodpatcher:\n    ipodpatcher %d -wf %s\n",
			n, quoteIfNeeded(f.o.Image))
	default:
		fmt.Fprintf(&b, "  Or flash the image with ipodpatcher:\n    ipodpatcher %d -wfb %s\n",
			n, quoteIfNeeded(f.o.Image))
	}
	fmt.Fprintf(&b, "  If the iPod will not boot: hold Select+Menu to reset, then hold Select+Play\n"+
		"  at the Apple logo for disk mode. That is the recovery floor and it is in ROM.\n")
	return b.String()
}

// ipodpatcherDisk turns a device path into the number ipodpatcher wants
// as its first argument: the trailing digits of \\.\PhysicalDriveN, or
// of /dev/diskN. There is no such number for /dev/sdb, so 1 — the
// device number on the machine this project uses — is the fallback, and
// the line is advice to check, not a script.
func ipodpatcherDisk(path string) int {
	digits := ""
	for i := len(path) - 1; i >= 0; i-- {
		if path[i] < '0' || path[i] > '9' {
			break
		}
		digits = string(path[i]) + digits
	}
	if digits == "" {
		return 1
	}
	n := 0
	for _, c := range digits {
		n = n*10 + int(c-'0')
	}
	return n
}

func quoteIfNeeded(s string) string {
	if strings.ContainsAny(s, " \t") {
		return `"` + s + `"`
	}
	return s
}

// --- shared helpers ---------------------------------------------------

func isDenied(err error) bool {
	return errors.Is(err, disk.ErrRawAccessDenied) || errors.Is(err, os.ErrPermission)
}

// BackupFileName is the name of one whole-partition backup.
//
// The size is in it because the firmware-partition size is the one
// number that says which iPod a loose backup came from, and a restore
// to the wrong model is the mistake with no undo — `--from-backup`
// refuses a size mismatch for the same reason. The timestamp is RFC3339
// with the colons replaced, because a colon is legal in a path on
// exactly one of the three operating systems this runs on.
func BackupFileName(sizeBytes int64, t time.Time) string {
	stamp := strings.ReplaceAll(t.Format(time.RFC3339), ":", "-")
	return fmt.Sprintf("fwpart-%d-%s.bin", sizeBytes, stamp)
}

// AppleBackupFileName is the name of the backup taken when the device
// was NOT running Core — in practice, the one and only copy of that
// iPod's Apple firmware.
//
// It is a different name from BackupFileName on purpose, and the
// difference is not cosmetic: Apple has not distributed iPod Video
// firmware for years, so this file cannot be re-downloaded, cannot be
// borrowed from another device (the preamble is per-device) and must
// never be pruned. A cleanup that ages out backups — there is none
// today, and when there is one — keeps every apple-* file forever.
//
// The date, not the time: one install per device per day is the whole
// population, and a name a person can retype is worth more than a
// unique one. unusedPath adds a -2 in the rare case.
func AppleBackupFileName(serial string, t time.Time) string {
	return fmt.Sprintf("apple-%s-%s.bin", backupSerial(serial), t.Format("2006-01-02"))
}

// backupSerial makes a disk serial safe to put in a file name: the OS
// hands these back from a SCSI inquiry and they can carry spaces and
// punctuation. An empty serial becomes "unknown" rather than an empty
// field, so the name never reads as apple--2026-09-15.bin.
func backupSerial(serial string) string {
	var b strings.Builder
	for _, r := range serial {
		switch {
		case r >= 'a' && r <= 'z', r >= 'A' && r <= 'Z', r >= '0' && r <= '9', r == '-', r == '_':
			b.WriteRune(r)
		}
	}
	if b.Len() == 0 {
		return "unknown"
	}
	return b.String()
}

// keptFirmwareLine is the sentence printed after a successful write
// that replaced something other than Core: where the firmware that was
// there went, and the command that puts it back. It is the whole
// reason the backup has a findable name.
func (f *flash) keptFirmwareLine() string {
	if f.res.Installed.Kind != fwpart.Other || f.res.BackupPath == "" {
		return ""
	}
	what := f.res.Installed.Description
	if strings.HasPrefix(what, "Apple firmware") {
		what = "Apple firmware"
	}
	return fmt.Sprintf("%s kept at %s; `core flash --from-backup %s` puts it back",
		what, f.res.BackupPath, quoteIfNeeded(f.res.BackupPath))
}

// DefaultBackupDir is <user config dir>/core/backups. The plan names
// UserConfigDir specifically: backups are small, are the thing you want
// after a reinstall, and a cache directory is the one place an OS feels
// free to delete.
func DefaultBackupDir() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", fmt.Errorf("locating the user config directory for the default backup path "+
			"(pass --backup-dir to choose one): %w", err)
	}
	return filepath.Join(dir, "core", "backups"), nil
}

// writeFileAtomic writes path via a temp file in the same directory,
// fsyncs it, and renames it into place. An existing path is never
// overwritten: a backup you can clobber is not a backup, and the second
// run is exactly when you have just broken the device the first one
// came from.
func writeFileAtomic(path string, write func(io.Writer) error) error {
	if _, err := os.Stat(path); err == nil {
		return fmt.Errorf("%s already exists; refusing to overwrite a backup", path)
	}
	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, "."+filepath.Base(path)+".tmp-*")
	if err != nil {
		return fmt.Errorf("create a temp file in %s: %w", dir, err)
	}
	name := tmp.Name()
	fail := func(err error) error {
		_ = tmp.Close()
		_ = os.Remove(name)
		return err
	}
	if err := write(tmp); err != nil {
		return fail(err)
	}
	if err := tmp.Sync(); err != nil {
		return fail(fmt.Errorf("fsync %s: %w", name, err))
	}
	// Checked, not deferred: on many filesystems Close is where a
	// deferred write finally reports ENOSPC or EIO, and an unchecked
	// Close turns that into a backup that is not there.
	if err := tmp.Close(); err != nil {
		return fail(fmt.Errorf("close %s: %w", name, err))
	}
	if err := os.Chmod(name, 0o644); err != nil {
		return fail(fmt.Errorf("chmod %s: %w", name, err))
	}
	if err := os.Rename(name, path); err != nil {
		return fail(fmt.Errorf("rename %s → %s: %w", name, path, err))
	}
	if d, err := os.Open(dir); err == nil {
		_ = d.Sync()
		_ = d.Close()
	}
	return nil
}

// Close releases the backup file a --from-backup run holds open.
func (r *restoreInput) Close() error {
	if r == nil || r.file == nil {
		return nil
	}
	return r.file.Close()
}
