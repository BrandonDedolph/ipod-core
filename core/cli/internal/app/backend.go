package app

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/doctor"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/eject"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/ghrelease"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// Backend is everything the window cannot do by drawing: the disk, the
// network and the filesystem. One implementation talks to the real
// machine; the tests use a fake, which is why every button's job can be
// exercised with no iPod, no GitHub and no display.
//
// Every method takes a context (Cancel) and the long ones take an emit
// function (the log pane and the progress bar). None of them touch
// State: they describe what happened and the UI goroutine folds it in.
type Backend interface {
	// Refresh identifies the attached iPod and inspects its library
	// volume. Read-only, top to bottom.
	Refresh(ctx context.Context) (Device, Library, error)
	// Sync plans and executes one sync. o.DryRun makes it a plan and
	// nothing else.
	Sync(ctx context.Context, o syncer.Options, emit func(Event)) (*syncer.Plan, error)
	// Release asks GitHub what the latest firmware release is.
	Release(ctx context.Context) (Release, error)
	// Download fetches (or finds cached) the release's .ipod and
	// returns the local path, checksum already verified.
	Download(ctx context.Context, rel Release, emit func(Event)) (string, error)
	// Flash writes a firmware image. confirm is called with the
	// flasher's own prompt and must return the typed answer.
	Flash(ctx context.Context, file string, confirm func(prompt string) (string, error), emit func(Event)) error
	// Backup copies the whole firmware partition to a file and returns
	// its path.
	Backup(ctx context.Context, emit func(Event)) (string, error)
	// Eject flushes and ejects the volume. It emits what the OS
	// actually did: "flushed but not ejected, run this yourself" and
	// "dismounted and ejected" are different facts, and the user is
	// about to pull a cable on the strength of one of them.
	Eject(ctx context.Context, volume string, emit func(Event)) error
}

// RealBackend is the production wiring. Its fields are the two paths
// the app needs to know about the machine it is on and are filled by
// NewRealBackend.
type RealBackend struct {
	// CLI is the `core` executable beside the app, or "" when there is
	// none. It is the process that does an elevated write on Windows.
	CLI string
	// BackupDir is where whole-partition backups go; "" means the
	// flasher's default, which is where the CLI puts them.
	BackupDir string
	// Repo is the GitHub repository releases come from.
	Repo string
	// GOOS is runtime.GOOS, overridable in a test.
	GOOS string
}

// NewRealBackend wires the production backend.
func NewRealBackend(backupDir string) *RealBackend {
	cli, _ := CLIPath()
	return &RealBackend{CLI: cli, BackupDir: backupDir, Repo: ghrelease.DefaultRepo, GOOS: runtime.GOOS}
}

// CLIPath finds the `core` CLI that ships beside the app.
//
// core-app never writes a raw disk itself. On Windows a `-H windowsgui`
// process has no console, so an elevated copy of IT could neither be
// told what to do on a command line a user can read nor print anything
// back; the app therefore runs `core.exe flash … --elevated-log <file>`
// under UAC and tails the log. That means one write path for the CLI
// and the app both, and a UAC prompt that names the binary the user was
// told does the writing.
//
// Beside the executable first, then PATH. Returning ok=false is not an
// error here: the Firmware card says the CLI is missing and prints the
// command to run instead.
func CLIPath() (string, bool) {
	name := "core"
	if runtime.GOOS == "windows" {
		name = "core.exe"
	}
	if exe, err := os.Executable(); err == nil {
		if dir := filepath.Dir(exe); dir != "" {
			cand := filepath.Join(dir, name)
			if st, err := os.Stat(cand); err == nil && !st.IsDir() {
				return cand, true
			}
		}
	}
	if p, err := exec.LookPath(name); err == nil {
		return p, true
	}
	return "", false
}

// --- Refresh -----------------------------------------------------------

// Refresh runs the same read-only checks `core doctor` runs, because
// the Device card answers the same questions and must not answer them
// differently.
func (b *RealBackend) Refresh(ctx context.Context) (Device, Library, error) {
	dev := doctor.CheckDevice(doctor.DeviceDeps{Missing: doctor.Fail})

	d := Device{
		Found:    dev.Found,
		Firmware: dev.Firmware,
		OSOSOK:   dev.OSOSOK,
		OSOSNote: dev.OSOSNote,
		Err:      dev.Err,
		Elevated: disk.IsElevated(),
	}
	if dev.Found {
		p := dev.Pod
		d.Path = p.Disk.Path
		d.Model = strings.TrimSpace(p.Model)
		d.Serial = p.Disk.Serial
		d.Size = p.Disk.SizeBytes
		d.SectorSize = p.SectorSize
		d.Tested = p.Tested
		d.Volume = doctor.VolumeOf(p, true)
	}
	// "Reading a raw disk needs Administrator" is worth saying on the
	// card only when that is actually what went wrong.
	d.ElevationAdviceNeeded = !d.Elevated && strings.Contains(strings.ToLower(d.Err), "denied")

	vol := doctor.CheckVolume(d.Volume)
	lib := Library{
		Present:     vol.Present,
		Songs:       vol.Songs,
		Albums:      vol.Albums,
		Genres:      vol.Genres,
		IndexBytes:  vol.IndexBytes,
		ConfigValid: vol.ConfigValid,
		LogValid:    vol.LogValid,
		Note:        vol.Note,
	}
	return d, lib, nil
}

// --- Sync --------------------------------------------------------------

// Sync scans, plans and (unless o.DryRun) executes. The plan comes back
// so the caller can count orphans for the prune confirmation without
// scanning twice.
func (b *RealBackend) Sync(ctx context.Context, o syncer.Options, emit func(Event)) (*syncer.Plan, error) {
	if err := syncer.CheckPaths(o); err != nil {
		return nil, err
	}
	emit(Event{Kind: EventProgress, Text: "scanning " + o.Src, Pct: -1})
	scan, err := syncer.ScanSource(o)
	if err != nil {
		return nil, err
	}
	emit(Event{Kind: EventLog, Text: fmt.Sprintf("%s  scanned %d albums, %d tracks",
		stamp(time.Now()), len(scan.Albums), scan.SongCount())})

	plan, err := syncer.MakePlan(o, scan)
	if err != nil {
		return nil, err
	}
	emit(Event{Kind: EventLog, Text: fmt.Sprintf(
		"%s  plan: %d albums, %d to copy, %d already there, %d to rename, %d orphan(s)",
		stamp(time.Now()), plan.Albums, len(plan.Copy), len(plan.Skip), len(plan.Rename), len(plan.Prune))})
	for _, w := range plan.Warnings {
		emit(Event{Kind: EventLog, Text: "warning: " + w})
	}
	if o.DryRun {
		emit(Event{Kind: EventProgress, Text: "dry run — nothing was written", Pct: 1})
		return plan, nil
	}

	total := plan.Albums
	o.Progress = func(e syncer.Event) {
		switch e.Kind {
		case syncer.EventPhase:
			emit(Event{Kind: EventProgress, Text: e.Phase, Pct: -1})
			emit(Event{Kind: EventLog, Text: stamp(time.Now()) + "  " + e.Phase})
		case syncer.EventAlbum:
			pct := float32(-1)
			if total > 0 {
				pct = float32(e.Index) / float32(total)
			}
			line := fmt.Sprintf("%s  [%d/%d] %s — %d copied, %d skipped, %d renamed, art %s",
				stamp(time.Now()), e.Index, e.Total, e.Album, e.Copied, e.Skipped, e.Renamed, e.Art)
			emit(Event{Kind: EventProgress, Text: e.Album, Pct: pct})
			emit(Event{Kind: EventLog, Text: line})
		case syncer.EventWarning:
			emit(Event{Kind: EventLog, Text: "warning: " + e.Message})
		case syncer.EventDone:
			emit(Event{Kind: EventProgress, Text: e.Phase, Pct: 1})
		}
	}
	rep, err := syncer.Execute(ctx, plan, o)
	if rep != nil {
		emit(Event{Kind: EventLog, Text: fmt.Sprintf(
			"%s  %d copied (%s), %d skipped, %d pruned, art %d written / %d kept, index %d bytes",
			stamp(time.Now()), rep.Copied, disk.HumanSize(rep.CopiedBytes), rep.Skipped,
			rep.Pruned, rep.ArtWritten, rep.ArtSkipped, rep.IndexBytes)})
	}
	return plan, err
}

// --- Firmware ----------------------------------------------------------

// Release asks GitHub for the latest release.
func (b *RealBackend) Release(ctx context.Context) (Release, error) {
	repo := b.Repo
	if repo == "" {
		repo = ghrelease.DefaultRepo
	}
	rel, err := ghrelease.Latest(ctx, repo)
	if err != nil {
		return Release{Checked: true, Err: err.Error()}, err
	}
	out := Release{Checked: true, Tag: rel.Tag, Notes: rel.Notes}
	if a, ok := ghrelease.FirmwareAsset(rel); ok {
		out.Asset = a.Name
	}
	return out, nil
}

// Download fetches the release's firmware image into the cache. The
// .ipod transport checksum is verified by internal/ghrelease before the
// file is renamed into place, so a path returned here is a path that
// verified.
func (b *RealBackend) Download(ctx context.Context, rel Release, emit func(Event)) (string, error) {
	repo := b.Repo
	if repo == "" {
		repo = ghrelease.DefaultRepo
	}
	full, err := ghrelease.ByTag(ctx, repo, rel.Tag)
	if err != nil {
		return "", err
	}
	asset, ok := ghrelease.FirmwareAsset(full)
	if !ok {
		return "", fmt.Errorf("%w: %s carries no core.ipod", ghrelease.ErrNoAsset, rel.Tag)
	}
	dst, err := ghrelease.CachePath(full.Tag, asset.Name)
	if err != nil {
		return "", err
	}
	if ghrelease.Cached(asset, dst) {
		emit(Event{Kind: EventLog, Text: "cached: " + dst + " (already downloaded and verified)"})
		return dst, nil
	}
	emit(Event{Kind: EventProgress, Text: fmt.Sprintf("downloading %s (%d bytes)", asset.Name, asset.Size), Pct: -1})
	if err := ghrelease.Download(ctx, asset, dst); err != nil {
		return "", err
	}
	emit(Event{Kind: EventLog, Text: "verified: " + dst + " (.ipod transport checksum OK)"})
	return dst, nil
}

// Flash writes an image through internal/flasher — the same call `core
// flash` makes, with the same mandatory sequence: backup first, typed
// confirmation, read-back verify.
func (b *RealBackend) Flash(ctx context.Context, file string,
	confirm func(prompt string) (string, error), emit func(Event)) error {
	dir := b.BackupDir
	if dir == "" {
		if d, err := flasher.DefaultBackupDir(); err == nil {
			dir = d
		}
	}
	o, deps := buildFlashDeps(file, b.CLI, dir, confirm, emitWriter(emit))
	if b.GOOS != "" {
		deps.GOOS = b.GOOS
	}
	_, err := flasher.Flash(ctx, o, deps)
	return err
}

// buildFlashDeps is the whole of the app's flash wiring, as a pure
// function, so a test can assert what the elevated child would be told
// to do without a device, a UAC prompt or a window.
//
// Three fields carry the decision made in the plan's S10: Executable
// and Relaunch both name the `core` CLI beside the app rather than the
// app itself, and ChildArgs spells the child's command line as a
// `flash`, because `core-app --something` is not a command line anybody
// could read in a UAC prompt or re-run by hand.
func buildFlashDeps(file, cliPath, backupDir string,
	confirm func(prompt string) (string, error), out io.Writer) (flasher.Options, flasher.Deps) {
	o := flasher.Options{Image: file, BackupDir: backupDir}
	d := flasher.Deps{
		ChildArgs: []string{"flash", file, "--backup-dir", backupDir},
		Confirm:   confirm,
		Out:       out,
	}
	if cliPath != "" {
		d.Executable = func() (string, error) { return cliPath, nil }
		d.Relaunch = func(args []string) (int, string, error) {
			return disk.RelaunchElevatedExe(cliPath, args)
		}
	} else {
		// No CLI beside the app. Refusing to relaunch here rather than
		// letting the flasher relaunch core-app itself is the point:
		// the GUI child would open a second window with no arguments it
		// understands and write nothing, which reads to a user exactly
		// like a flash that worked.
		d.Executable = func() (string, error) { return "core", nil }
		d.Relaunch = func([]string) (int, string, error) {
			return 0, "", fmt.Errorf("the core CLI is not next to this app; " +
				"put core.exe beside core-app.exe, or run the command above in an Administrator console")
		}
	}
	return o, d
}

// emitWriter adapts the flasher's io.Writer to the event channel. The
// flasher prints whole paragraphs; State.appendLines splits them.
func emitWriter(emit func(Event)) io.Writer {
	return writerFunc(func(p []byte) (int, error) {
		emit(Event{Kind: EventLog, Text: string(p)})
		return len(p), nil
	})
}

type writerFunc func([]byte) (int, error)

func (f writerFunc) Write(p []byte) (int, error) { return f(p) }

// Backup copies the whole firmware partition to a file and then parses
// the file back, because a backup nobody has read back is a file and
// not a backup.
func (b *RealBackend) Backup(ctx context.Context, emit func(Event)) (string, error) {
	pod, err := disk.SelectIPod("")
	if err != nil {
		return "", err
	}
	h, err := disk.Open(pod.Disk.Path, false)
	if err != nil {
		return "", err
	}
	defer h.Close()

	dir := b.BackupDir
	if dir == "" {
		if dir, err = flasher.DefaultBackupDir(); err != nil {
			return "", err
		}
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	path := filepath.Join(dir, flasher.BackupFileName(pod.FWPartLen, time.Now()))

	emit(Event{Kind: EventProgress, Text: fmt.Sprintf("reading partition 0: %d bytes", pod.FWPartLen), Pct: -1})
	tmp := path + ".part"
	f, err := os.Create(tmp)
	if err != nil {
		return "", err
	}
	src := io.NewSectionReader(h, pod.FWPartStart, pod.FWPartLen)
	n, cerr := io.Copy(f, src)
	if cerr == nil {
		cerr = f.Sync()
	}
	if err := f.Close(); cerr == nil {
		cerr = err
	}
	if cerr != nil {
		_ = os.Remove(tmp)
		return "", cerr
	}
	if n != pod.FWPartLen {
		_ = os.Remove(tmp)
		return "", fmt.Errorf("read %d of %d bytes from %s", n, pod.FWPartLen, pod.Disk.Path)
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return "", err
	}

	if err := verifyBackup(path); err != nil {
		emit(Event{Kind: EventLog, Text: "warning: " + err.Error()})
	} else {
		emit(Event{Kind: EventLog, Text: "verified: preamble, directory and OSOS checksum all read back OK"})
	}
	return path, nil
}

func verifyBackup(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return err
	}
	p := fwpart.Partition{R: f, Size: st.Size()}
	d, err := fwpart.Parse(p)
	if err != nil {
		return fmt.Errorf("the backup does not parse as a firmware partition: %w", err)
	}
	_, osos, ok := d.OSOS()
	if !ok {
		return fmt.Errorf("the backup has no OSOS image — do NOT restore from this file")
	}
	if err := fwpart.VerifyEntry(p, osos); err != nil {
		return fmt.Errorf("the backup's OSOS checksum does not verify: %w", err)
	}
	return nil
}

// Eject hands the volume back to the OS through internal/eject — the
// same code `core eject` runs.
func (b *RealBackend) Eject(ctx context.Context, volume string, emit func(Event)) error {
	return eject.Eject(emitWriter(emit), volume)
}
