package app

import (
	"bytes"
	"context"
	"fmt"
	"image"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/doctor"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/eject"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/ghrelease"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/installer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
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
	// Detect is the cheap half of Refresh: which iPods are attached,
	// one raw open each, no volume walk and no checksum. The 2-second
	// poll calls it; a change is what starts a Refresh.
	Detect(ctx context.Context) ([]disk.IPod, error)
	// Classify says what firmware is on one iPod. It is what decides
	// between the Install screen and the main screen.
	Classify(ctx context.Context, pod disk.IPod) (fwpart.Installed, error)
	// Install puts Core on an iPod that is not running it: the whole
	// internal/installer sequence, which is the same one `core install`
	// runs. confirm is the flasher's typed confirmation, exactly as for
	// Flash.
	Install(ctx context.Context, o installer.Options, emit func(Event),
		confirm func(prompt string) (string, error)) (*installer.Result, error)
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
	// Rename writes the iPod's name onto its volume: the FAT label
	// becomes disk.LegalLabel(name) (upper-case, ASCII, 11 bytes), an
	// empty name clears it, and the label is read back and returned —
	// what comes back is what Explorer and the device will show. The
	// friendly name itself is the UI's business: it lives in
	// config.json, not on the device.
	Rename(ctx context.Context, volume, name string) (label string, err error)
	// Eject flushes and ejects the volume. It emits what the OS
	// actually did: "flushed but not ejected, run this yourself" and
	// "dismounted and ejected" are different facts, and the user is
	// about to pull a cable on the strength of one of them.
	Eject(ctx context.Context, volume string, emit func(Event)) error

	// --- the library manager (the Library tab) ---------------------

	// Inspect walks the source tree and reports what is wrong with it:
	// missing covers, files whose names do not match their tags,
	// folders to organize, and the files nobody can name from their
	// tags. It writes nothing and makes no network request, which is
	// why the window runs it the moment a folder is chosen.
	//
	// discFolders is librarian's opt-in multi-disc split: with it off the
	// split is still reported and costs nothing, and only a report made
	// with it ON can have the split applied — which is why ticking that
	// row in the window rescans.
	Inspect(ctx context.Context, src string, discFolders bool, emit func(Event)) (
		*librarian.Report, error)
	// Candidates asks the art providers about the coverless albums in
	// a report. It is the only call in this half that reaches the
	// internet, so it is a button and not part of Inspect.
	Candidates(ctx context.Context, rep *librarian.Report, emit func(Event)) (
		map[librarian.AlbumKey][]artfetch.Candidate, error)
	// Fix applies the ticked parts of a report as ONE job with ONE undo
	// journal: covers first, then the moves, then the sidecars.
	Fix(ctx context.Context, rep *librarian.Report, ch librarian.Choices, emit func(Event)) (
		*librarian.Result, error)
	// UndoFix replays one journal backwards, refusing any file that has
	// changed since it was moved.
	UndoFix(ctx context.Context, journal string, emit func(Event)) (*organizer.UndoReport, error)

	// Thumbnail decodes one album folder's cover for the grid: the
	// device's own folder.art sidecar when there is one, the embedded
	// front cover otherwise. It is called off the UI goroutine by the
	// thumbnail cache, never from a layout.
	Thumbnail(dir string) (image.Image, error)
	// FetchThumb downloads one art candidate's ~200 px thumbnail. It is
	// on the Backend so the Library tab's tests draw the candidate row
	// with no network at all.
	FetchThumb(ctx context.Context, url string) (image.Image, error)
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
	// HTTP is the client FetchThumb uses; nil means a default one.
	HTTP *http.Client

	// art is the shared artfetch client, built once: its cache and its
	// one-request-a-second MusicBrainz limiter have to be shared across
	// every lookup the window makes.
	artOnce sync.Once
	art     *artfetch.Client
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
		if inst, err := b.Classify(ctx, p); err == nil {
			d.Installed = inst
		} else if d.Err == "" {
			// Not fatal and not silent: a device that cannot be
			// classified stays on the main screen (see State.Phase) and
			// the reason goes where the user can read it.
			d.Err = err.Error()
		}
		d.Volume = doctor.VolumeOf(p, true)
		if d.Volume != "" {
			// The iPod's name. A volume with no label, or a platform
			// with no way to ask, leaves it empty and the header falls
			// back — a name is not worth an error on a refresh.
			d.Label, _ = disk.VolumeLabel(d.Volume)
		}
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

// --- Detect ------------------------------------------------------------

// Detect is one disk.FindIPods: a raw read-only open of each physical
// drive, the MBR and the preamble. It is what the poll runs twice a
// second-ish, and it is deliberately not the doctor — a full Refresh
// every 2 s would re-read and re-checksum a 7.6 MB image forever.
func (b *RealBackend) Detect(ctx context.Context) ([]disk.IPod, error) {
	return disk.FindIPods()
}

// Classify opens one iPod read-only and says what is on its OSOS image.
func (b *RealBackend) Classify(ctx context.Context, pod disk.IPod) (fwpart.Installed, error) {
	h, err := disk.Open(pod.Disk.Path, false)
	if err != nil {
		return fwpart.Installed{}, err
	}
	defer h.Close()
	inst, _, err := fwpart.ClassifyPartition(doctor.FirmwarePartition(pod, h))
	if err != nil {
		return fwpart.Installed{}, fmt.Errorf("reading the firmware partition on %s: %w",
			pod.Disk.Path, err)
	}
	return inst, nil
}

// --- Install -----------------------------------------------------------

// Install runs internal/installer over the attached iPod: classify,
// refuse a device already running Core, back Apple's firmware up under
// a name a person can find, write through the same flasher sequence as
// Flash, then create CORECFG.DAT, CORELOG.BIN and Music\ on the volume.
func (b *RealBackend) Install(ctx context.Context, o installer.Options, emit func(Event),
	confirm func(prompt string) (string, error)) (*installer.Result, error) {
	dir := o.BackupDir
	if dir == "" {
		dir = b.BackupDir
	}
	if dir == "" {
		if d, err := flasher.DefaultBackupDir(); err == nil {
			dir = d
		}
	}
	o.BackupDir = dir
	deps := buildInstallDeps(b, dir, confirm, emitWriter(emit))
	if b.GOOS != "" {
		deps.GOOS = b.GOOS
	}
	return installer.Install(ctx, o, deps)
}

// buildInstallDeps is the app's install wiring, as a function of the
// backend and nothing else, so a test can assert what the elevated
// child would be told to do without a device or a UAC prompt.
//
// It mirrors buildFlashDeps exactly, and for the same reason: the
// elevated child is the `core` CLI beside the app, running a command
// line a person could read in the UAC prompt and re-run by hand. The
// one difference is the command — `core install <image>`, not `flash`,
// because the volume step is part of an install and the child is the
// process that reaches it.
func buildInstallDeps(b *RealBackend, backupDir string,
	confirm func(prompt string) (string, error), out io.Writer) installer.Deps {
	d := installer.Deps{
		Inspect: func(ctx context.Context) (installer.Device, error) {
			pod, err := disk.SelectIPod("")
			if err != nil {
				return installer.Device{}, err
			}
			inst, err := b.Classify(ctx, pod)
			if err != nil {
				return installer.Device{}, err
			}
			return installer.Device{Pod: pod, Installed: inst}, nil
		},
		ChildArgs: func(image string) []string {
			args := []string{"install", image}
			if backupDir != "" {
				args = append(args, "--backup-dir", backupDir)
			}
			return args
		},
		Confirm: confirm,
		Out:     out,
	}
	if b.CLI != "" {
		cli := b.CLI
		d.Executable = func() (string, error) { return cli, nil }
		d.Relaunch = func(args []string) (int, string, error) {
			return disk.RelaunchElevatedExe(cli, args)
		}
	} else {
		d.Executable = func() (string, error) { return "core", nil }
		d.Relaunch = func([]string) (int, string, error) {
			return 0, "", fmt.Errorf("the core CLI is not next to this app; " +
				"put core.exe beside core-app.exe, or run `core install` in an Administrator console")
		}
	}
	return d
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

// --- Rename -------------------------------------------------------------

// Rename writes the volume label and proves it by reading it back.
//
// Two things this deliberately does NOT do. It does not touch
// config.json: the friendly name is the UI's, and a backend that wrote
// settings would have two owners for one file. And it does not lock the
// volume first — SetVolumeLabelW needs the volume mounted, and the one
// thing that must never happen is a rename during a flash, which
// dismounts the drive letter. The Runner's one-job-at-a-time rule is
// what keeps those two apart.
func (b *RealBackend) Rename(ctx context.Context, volume, name string) (string, error) {
	if volume == "" {
		return "", fmt.Errorf("no iPod volume: plug the iPod in (disk mode: Select+Play) and press Refresh")
	}
	label := disk.LegalLabel(name)
	if strings.TrimSpace(name) != "" && label == "" {
		return "", fmt.Errorf("%q has nothing a FAT volume label can hold "+
			"(A-Z, 0-9, space and !#$%%&'()-@^_`{}~, 11 bytes)", name)
	}
	if err := disk.SetVolumeLabel(volume, label); err != nil {
		return "", err
	}
	got, err := disk.VolumeLabel(volume)
	if err != nil {
		// The write reported success and the read-back failed: say what
		// was written rather than claiming the rename did not happen.
		return label, nil
	}
	if got != label {
		return got, fmt.Errorf("the label read back as %q, not %q — the volume did not take it", got, label)
	}
	return got, nil
}

// Eject hands the volume back to the OS through internal/eject — the
// same code `core eject` runs.
func (b *RealBackend) Eject(ctx context.Context, volume string, emit func(Event)) error {
	return eject.Eject(emitWriter(emit), volume)
}

// --- the library manager ------------------------------------------------

// The four librarian calls, wired the way `core fix` wires them, plus the
// two picture readers the grid and the candidate row need.
//
// librarian's Event is app's Event with a string job name (it must not
// import this package — linking Gio into core.exe is the thing that split
// them), so the adapter here is a switch and two copies, exactly as the
// plan's L5 said it would be.

// libEvents adapts librarian's progress callback to the window's.
func libEvents(emit func(Event)) func(librarian.Event) {
	return func(e librarian.Event) {
		switch e.Kind {
		case librarian.EventProgress:
			emit(Event{Kind: EventProgress, Text: e.Text, Pct: e.Pct})
		default:
			emit(Event{Kind: EventLog, Text: e.Text})
		}
	}
}

// artClient is the shared art client. One per backend, so the cache and
// the MusicBrainz rate limiter are shared across every lookup the window
// makes — a limiter per call would be no limiter at all.
func (b *RealBackend) artClient() *artfetch.Client {
	b.artOnce.Do(func() {
		b.art = artfetch.New()
	})
	return b.art
}

// Inspect is librarian.Inspect with the window's progress.
func (b *RealBackend) Inspect(ctx context.Context, src string, discFolders bool,
	emit func(Event)) (*librarian.Report, error) {
	return librarian.Inspect(ctx, src, librarian.Options{
		Progress:    libEvents(emit),
		DiscFolders: discFolders,
	})
}

// Candidates asks the providers about every coverless album in the report.
func (b *RealBackend) Candidates(ctx context.Context, rep *librarian.Report, emit func(Event)) (
	map[librarian.AlbumKey][]artfetch.Candidate, error) {
	return librarian.Candidates(ctx, rep, b.artClient(), librarian.Options{Progress: libEvents(emit)})
}

// Fix applies the ticked parts of the report. The art client goes in
// because an accepted candidate is downloaded here, at apply time, rather
// than held in memory from the lookup.
func (b *RealBackend) Fix(ctx context.Context, rep *librarian.Report, ch librarian.Choices,
	emit func(Event)) (*librarian.Result, error) {
	return librarian.Fix(ctx, rep, ch, librarian.Options{
		Art:         b.artClient(),
		Progress:    libEvents(emit),
		DiscFolders: ch.DiscFolders,
	})
}

// UndoFix replays one journal backwards.
func (b *RealBackend) UndoFix(ctx context.Context, journal string, emit func(Event)) (
	*organizer.UndoReport, error) {
	return librarian.Undo(ctx, journal, librarian.Options{Progress: libEvents(emit)})
}

// Thumbnail is the grid's decode: the device's own sidecar, else the
// album's embedded front cover.
func (b *RealBackend) Thumbnail(dir string) (image.Image, error) { return AlbumThumbnail(dir) }

// FetchThumb downloads one candidate's thumbnail.
//
// It is a plain GET rather than artfetch.Client.Fetch because Fetch is the
// downloader for the picture that gets EMBEDDED: it enforces a minimum of
// several hundred pixels, and a 100 px confirmation thumbnail would fail
// that check by design. The User-Agent is artfetch's either way —
// MusicBrainz and the Cover Art Archive require it of every request.
func (b *RealBackend) FetchThumb(ctx context.Context, url string) (image.Image, error) {
	if strings.TrimSpace(url) == "" {
		return nil, ErrNoThumb
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", artfetch.UserAgent())
	req.Header.Set("Accept", "image/*")
	client := b.HTTP
	if client == nil {
		client = &http.Client{Timeout: artfetch.RequestTimeout}
	}
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("art thumbnail %s: %s", url, resp.Status)
	}
	// A confirmation thumbnail is 200 px of JPEG; anything an order of
	// magnitude bigger is not one, and this reader is the trust boundary
	// for bytes a search result pointed at.
	body, err := io.ReadAll(io.LimitReader(resp.Body, maxThumbBytes))
	if err != nil {
		return nil, err
	}
	img, _, err := image.Decode(bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	return Downscale(img, 200), nil
}

// maxThumbBytes is the cap on a candidate thumbnail download.
const maxThumbBytes = 2 << 20
