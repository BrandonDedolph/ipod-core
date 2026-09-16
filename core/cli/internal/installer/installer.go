// Package installer is "put Core on an iPod that is still running
// Apple's firmware", as a sequence with no command line in it.
//
// It was `internal/cli/install.go` first. It moved here when the app
// needed the same thing, because the alternative — a window that
// re-implements the order of "classify, refuse, back up, write, verify,
// create the device files" — is two sequences that will differ on the
// day one of them is wrong. The CLI is now a caller: it parses flags,
// downloads the release image, and prints an exit code.
//
// Nothing in this package knows about cobra, Gio, or a terminal. What
// it cannot do itself it takes as a Dep: finding and classifying the
// device, resolving an image when the caller did not name one, the
// flasher call, and the io.Writer everything is narrated to.
package installer

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/doctor"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
)

// ErrAlreadyCore is the refusal that makes an install a different thing
// from a flash: this device is already ours, and what the person wants
// is `core update`. Options.Force overrides it. It is the sentinel;
// AlreadyCoreError is what is actually returned, because the sentence a
// person reads has to name their device and what is on it.
var ErrAlreadyCore = errors.New("the device is already running Core")

// AlreadyCoreError is the refusal, with the two facts that make it
// actionable.
type AlreadyCoreError struct {
	Device    string
	Installed fwpart.Installed
}

func (e *AlreadyCoreError) Error() string {
	return fmt.Sprintf("%s is already running Core (%s) — this device is already "+
		"installed, use `core update` to move it to the latest release\n"+
		"  (`core install --force` writes anyway; `core flash <image>` writes a "+
		"specific file)", e.Device, e.Installed.Description)
}

// Is makes errors.Is(err, ErrAlreadyCore) work without the sentinel
// having to carry the device path.
func (e *AlreadyCoreError) Is(target error) bool { return target == ErrAlreadyCore }

// Options are the install's decisions. They are the flags of `core
// install`, minus the ones that only shape where the image comes from
// (--repo, --tag), which the caller resolves before calling.
type Options struct {
	// Image is the .ipod or .bin to write. Empty means "ask
	// Deps.ResolveImage" — the latest release, for both callers.
	Image string
	// Device is the --device selector (a path or a serial), passed
	// through to the flasher.
	Device string
	// Yes skips the typed confirmation. It does NOT skip the backup or
	// the read-back.
	Yes bool
	// DryRun prints the plan and stops: no backup, no write, and the
	// music volume is not touched either.
	DryRun bool
	// Force installs over a device that is already running Core.
	Force bool
	// BackupDir overrides <user config dir>/core/backups.
	BackupDir string
	// Untested allows a device outside the tested capacity window.
	Untested bool
	// NoRelaunch forbids starting an elevated child.
	NoRelaunch bool
}

// Device is what the classification step found: the iPod, and what
// firmware is on it right now.
type Device struct {
	Pod       disk.IPod
	Installed fwpart.Installed
}

// Deps are the calls that touch the outside world. Inspect is the only
// one with no default — there is no sequence at all without an answer
// to "what is on the device".
type Deps struct {
	// Inspect finds the iPod and classifies its OSOS image. Required.
	// It is called twice: once to decide what this write is, and once
	// afterwards to report what is on the device now, read back rather
	// than assumed.
	Inspect func(ctx context.Context) (Device, error)
	// ResolveImage produces the image path when Options.Image is empty
	// (the CLI downloads the latest release; the app hands the path in
	// and never needs this). It runs BEFORE the write and in the
	// unelevated parent, which is the rule that keeps the elevated
	// child off the network.
	ResolveImage func(ctx context.Context) (string, error)
	// Flash is flasher.Flash, replaced in tests.
	Flash func(ctx context.Context, o flasher.Options, d flasher.Deps) (*flasher.Result, error)
	// Volume resolves the mounted FAT volume of the iPod, waiting for
	// the OS to remount it after the write.
	Volume func(pod disk.IPod) string
	// Out is where the sequence narrates itself.
	Out io.Writer
	// ChildArgs is the command line an elevated child re-runs, built
	// from the RESOLVED image path — it is a function and not a slice
	// because when the caller did not name an image there is no path to
	// put in it until step 3 has run, and a child told to fetch the
	// release itself is a child doing network I/O as Administrator.
	ChildArgs func(image string) []string
	// The rest are handed straight to the flasher; see its Deps.
	GOOS       string
	Confirm    func(prompt string) (string, error)
	Executable func() (string, error)
	Relaunch   func(args []string) (exitCode int, log string, err error)
}

func (d *Deps) setDefaults() {
	if d.Flash == nil {
		d.Flash = flasher.Flash
	}
	if d.Volume == nil {
		d.Volume = WaitForVolume
	}
	if d.Out == nil {
		d.Out = io.Discard
	}
	if d.GOOS == "" {
		d.GOOS = runtime.GOOS
	}
}

// Result is what happened. It is returned even when the error is
// non-nil, so a caller can tell an elevated child's exit code from a
// failure in this process.
type Result struct {
	// Before is what was on the device when the sequence started;
	// After is what is on it now, re-read. After is the zero Installed
	// when the re-read failed or there was nothing to re-read.
	Before, After fwpart.Installed
	// Pod is the device, as Inspect found it.
	Pod disk.IPod
	// Image is the file that was written (or would have been).
	Image string
	// Flash is the flasher's own result: backup path, verification,
	// relaunch, abort.
	Flash *flasher.Result
	// Volume is the FAT volume the device files were created on, or ""
	// when it was not mounted (or not reached).
	Volume string
	// Created records whether each of the things the firmware cannot make
	// for itself had to be made. OTGSlotsCreated counts the On-The-Go slot
	// playlists (there are devicefs.OTGPlaylistSlots of them); an existing
	// one is never touched, whatever it holds.
	ConfigCreated, LogCreated, OTGCreated, MusicCreated bool
	OTGSlotsCreated                                     int
	// ClockStamped is the host time written into CORECFG.DAT for the
	// device's first boot to pick up; zero when no stamp was written.
	ClockStamped time.Time
}

// Installed reports what to believe about the device after this run:
// the re-read if there was one, else what was written in.
func (r *Result) Installed() fwpart.Installed {
	if r == nil {
		return fwpart.Installed{}
	}
	if r.After.Kind != "" {
		return r.After
	}
	return r.Before
}

// Install runs the whole sequence.
//
//  1. the iPod is found and its OSOS image is classified
//  2. a device already running Core is refused (Force overrides)
//  3. the image is resolved, if the caller did not name one
//  4. the WHOLE firmware partition is backed up, the image is written
//     with the entry point set to 0, and the write is read back — all
//     of that is internal/flasher, unchanged, the same call `core
//     flash` makes
//  5. on the FAT volume: CORECFG.DAT, CORELOG.BIN and Music\, which
//     the firmware can overwrite but cannot create
//  6. the device is read again and the before/after printed
func Install(ctx context.Context, o Options, d Deps) (*Result, error) {
	d.setDefaults()
	if d.Inspect == nil {
		return nil, errors.New("installer: Deps.Inspect is required")
	}
	out := d.Out

	// 1. What is on the device. Unlike an update, this failing IS
	// fatal: the whole command is a decision about what is installed,
	// and there is nothing to decide without reading it.
	dev, err := d.Inspect(ctx)
	if err != nil {
		return nil, err
	}
	res := &Result{Before: dev.Installed, Pod: dev.Pod}

	fmt.Fprintf(out, "device:    %s", dev.Pod.Disk.Path)
	if s := dev.Pod.Disk.Serial; s != "" {
		fmt.Fprintf(out, " (serial %s)", s)
	}
	fmt.Fprintln(out)
	fmt.Fprintf(out, "installed: %s\n", dev.Installed.Description)

	// 2. Already ours.
	if dev.Installed.IsCore() && !o.Force {
		return res, &AlreadyCoreError{Device: dev.Pod.Disk.Path, Installed: dev.Installed}
	}
	if dev.Installed.IsCore() {
		fmt.Fprintf(out, "--force: installing over an existing Core image.\n")
	}

	// 3. The image. The download happens HERE, in the unelevated
	// parent, so the elevated child never does network I/O.
	image := o.Image
	if image == "" {
		if d.ResolveImage == nil {
			return res, errors.New("installer: no image and no way to resolve one")
		}
		if image, err = d.ResolveImage(ctx); err != nil {
			return res, err
		}
	}
	res.Image = image

	// 4. The write, through the same flasher sequence as `core flash`.
	fmt.Fprintln(out)
	fres, ferr := d.Flash(ctx, flasher.Options{
		Image:      image,
		Device:     o.Device,
		DryRun:     o.DryRun,
		Yes:        o.Yes,
		BackupDir:  o.BackupDir,
		Untested:   o.Untested,
		NoRelaunch: o.NoRelaunch,
		Install:    true,
	}, flasher.Deps{
		GOOS:       d.GOOS,
		ChildArgs:  childArgs(d.ChildArgs, image),
		Out:        out,
		Confirm:    d.Confirm,
		Executable: d.Executable,
		Relaunch:   d.Relaunch,
	})
	res.Flash = fres
	if ferr != nil {
		return res, ferr
	}
	if fres == nil {
		return res, errors.New("the flash step returned nothing")
	}
	if fres.DryRun {
		fmt.Fprintf(out, "\n--dry-run: the music volume was not touched either. "+
			"It would get %s, %s and %s\\.\n",
			devicefs.ConfigName, devicefs.LogName, devicefs.MusicDir)
		return res, nil
	}
	if fres.Relaunched {
		// The elevated child ran this same sequence and did the volume
		// work itself; its output has already been printed.
		return res, nil
	}
	if fres.Aborted {
		return res, nil
	}

	// 5. The FAT side.
	if err := volumeFiles(out, d.Volume(dev.Pod), res); err != nil {
		return res, err
	}

	// 6. What is on the device now, read back off the device rather
	// than assumed from what was written.
	if after, aerr := d.Inspect(ctx); aerr == nil {
		res.After = after.Installed
		fmt.Fprintf(out, "\nfirmware:  %s (was: %s)\n",
			after.Installed.Description, dev.Installed.Description)
	}
	PrintRecovery(out, fres)
	return res, nil
}

// volumeFiles creates what the firmware cannot: CORECFG.DAT,
// CORELOG.BIN, COREOTG.DAT, the five On-The-Go slot playlists and Music\.
//
// The device's FAT driver can overwrite the data sectors of a file that
// already exists and nothing else — it cannot create, grow, move or
// delete (internal/devicefs). A freshly installed iPod with none of
// these boots with no settings and no log, cannot keep an On-The-Go list
// past a power cycle, has nowhere to save one, and finds no library root,
// so this step is part of the install and not an optional extra.
func volumeFiles(out io.Writer, volume string, res *Result) error {
	if volume == "" {
		fmt.Fprintf(out, "\nthe music volume is not mounted, so %s, %s, %s and %s\\ were not created.\n"+
			"  Unplug and replug the iPod (still in disk mode) and run `core doctor`, which\n"+
			"  says which of them are missing, or `core sync`, which creates them.\n",
			devicefs.ConfigName, devicefs.LogName, devicefs.OTGName, devicefs.MusicDir)
		return nil
	}
	res.Volume = volume

	fmt.Fprintf(out, "\nvolume:    %s\n", volume)
	created, err := devicefs.EnsureConfig(volume)
	if err != nil {
		return fmt.Errorf("creating %s on %s: %w", devicefs.ConfigName, volume, err)
	}
	res.ConfigCreated = created
	fmt.Fprintf(out, "  %-12s %s\n", devicefs.ConfigName, createdText(created))

	// The clock, into the file we have just guaranteed exists. A freshly
	// installed iPod has no time at all (the RTC reads its reset value until
	// something sets it), so the first boot after an install should not be the
	// one that shows "Not set". `core sync` and `core eject` stamp again, and
	// a failed stamp is never worth failing an install over — the device works
	// fine with a wrong clock.
	if stamped, err := devicefs.StampConfigTime(volume, time.Now()); err != nil {
		fmt.Fprintf(out, "  %-12s clock not stamped: %v\n", "", err)
	} else {
		res.ClockStamped = stamped.When
		fmt.Fprintf(out, "  %-12s clock stamped %s\n", "",
			stamped.When.Format("2006-01-02 15:04"))
	}

	created, err = devicefs.EnsureLog(volume, 0)
	if err != nil {
		return fmt.Errorf("creating %s on %s: %w", devicefs.LogName, volume, err)
	}
	res.LogCreated = created
	fmt.Fprintf(out, "  %-12s %s\n", devicefs.LogName, createdText(created))

	created, err = devicefs.EnsureOTG(volume)
	if err != nil {
		return fmt.Errorf("creating %s on %s: %w", devicefs.OTGName, volume, err)
	}
	res.OTGCreated = created
	fmt.Fprintf(out, "  %-12s %s\n", devicefs.OTGName, createdText(created))

	music := filepath.Join(volume, devicefs.MusicDir)
	_, statErr := os.Stat(music)
	if err := os.MkdirAll(music, 0o755); err != nil {
		return fmt.Errorf("creating %s: %w", music, err)
	}
	res.MusicCreated = statErr != nil
	fmt.Fprintf(out, "  %-12s %s (empty — `core sync` fills it)\n",
		devicefs.MusicDir+`\`, createdText(statErr != nil))

	// The five saved On-The-Go slots live under Music\Playlists\, so they
	// come after the folder exists. An existing one is never touched.
	slots, err := devicefs.EnsureOTGSlots(music)
	if err != nil {
		return fmt.Errorf("creating the On-The-Go slots on %s: %w", volume, err)
	}
	res.OTGSlotsCreated = len(slots)
	fmt.Fprintf(out, "  %-12s %d of %d created\n", "On-The-Go",
		len(slots), devicefs.OTGPlaylistSlots)
	return nil
}

// childArgs is Deps.ChildArgs applied, or nil — a nil slice makes the
// flasher fall back to os.Args[1:], which is exactly right for a CLI
// that was invoked with the arguments it wants repeated.
func childArgs(f func(string) []string, image string) []string {
	if f == nil {
		return nil
	}
	return f(image)
}

func createdText(created bool) string {
	if created {
		return "created"
	}
	return "already there and valid"
}

// WaitForVolume returns the mounted FAT volume, giving it a few seconds
// to come back.
//
// The flasher locks and dismounts the volume for the duration of the
// write (that is what makes the raw write safe), and Windows remounts
// it when the handle closes — but not instantly. Failing here because
// the OS was half a second behind would be a bad reason to leave a
// freshly installed iPod without its config file.
func WaitForVolume(pod disk.IPod) string {
	volume := doctor.VolumeOf(pod, true)
	if volume == "" {
		return ""
	}
	deadline := time.Now().Add(10 * time.Second)
	for {
		if _, err := os.Stat(volume); err == nil {
			return volume
		}
		if !time.Now().Before(deadline) {
			return ""
		}
		time.Sleep(250 * time.Millisecond)
	}
}

// PrintRecovery is the last thing an install prints: where the firmware
// that was on the device went, and how to put it back.
func PrintRecovery(out io.Writer, res *flasher.Result) {
	if res == nil || res.BackupPath == "" {
		return
	}
	fmt.Fprintf(out, "\nThe firmware that was on this device is saved at:\n  %s\n", res.BackupPath)
	if res.Installed.Kind == fwpart.Other {
		fmt.Fprintf(out, "  (%s — the only copy there is; keep the file)\n",
			res.Installed.Description)
	}
	fmt.Fprintf(out, "To put it back:\n  core flash --from-backup %s --yes\n", res.BackupPath)
	fmt.Fprintf(out, "If the iPod will not boot: hold Select+Menu to reset, then hold Select+Play\n"+
		"at the Apple logo for disk mode. That is the recovery floor and it is in ROM.\n")
}
