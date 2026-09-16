package app

import (
	"context"
	"errors"
	"image"
	"sync"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/installer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// fakeBackend is the Backend every test in this package runs against.
//
// It exists so the answer to "does the Sync + prune button really do a
// dry run first, and really refuse to delete until the second dialog is
// answered with the exact word" is a test and not a paragraph. Nothing
// in here touches a disk, a network or a window.
type fakeBackend struct {
	mu sync.Mutex

	dev Device
	lib Library
	rel Release

	// plans is handed out one per Sync call, so a prune test can make
	// the dry run report orphans.
	plans []*syncer.Plan

	// block, when set, makes Sync wait for it (or for the context) so
	// the busy/cancel tests have something to race against.
	block chan struct{}

	refreshErr, syncErr, releaseErr, downloadErr, flashErr, backupErr, ejectErr error

	// The detect script: one []disk.IPod per Detect call, the last one
	// repeating forever. Empty means "nothing is scripted" and Refresh
	// answers with dev/lib as it always did.
	pods        [][]disk.IPod
	detectCalls int
	detectErr   error
	current     []disk.IPod
	// installed is what Classify says about each device path. The fake
	// installer rewrites it, which is how a test sees the phase move
	// from NotInstalled to Ready for the reason a real one would.
	installed   map[string]fwpart.Installed
	classifyErr error

	// volumeDir stands in for the mounted FAT volume during an install.
	volumeDir    string
	installCalls []installer.Options
	installFlash flasher.Options
	installAns   string
	installErr   error

	syncCalls   []syncer.Options
	flashCalls  []string
	flashPrompt string
	flashAnswer string
	backupCalls int
	ejectCalls  []string
	renameCalls []renameCall
	renameErr   error
	downloaded  string

	// The library manager's half: what Inspect answers, what the
	// lookup found, and a record of every Fix and Undo so a test can
	// prove the button ran once and with the choices on screen.
	report       *librarian.Report
	inspectCalls []string
	inspectDiscs []bool
	inspectErr   error
	cands        map[librarian.AlbumKey][]artfetch.Candidate
	candCalls    int
	candErr      error
	fixCalls     []librarian.Choices
	fixResult    *librarian.Result
	fixErr       error
	undoCalls    []string
	undoReport   *organizer.UndoReport
	undoErr      error

	// thumbs answers Thumbnail per album folder; thumbCalls and
	// fetchCalls are what the cache actually asked for.
	thumbs     map[string]image.Image
	thumbCalls []string
	thumbErr   error
	fetchCalls []string
}

func (f *fakeBackend) Inspect(ctx context.Context, src string, discFolders bool,
	emit func(Event)) (*librarian.Report, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.inspectCalls = append(f.inspectCalls, src)
	f.inspectDiscs = append(f.inspectDiscs, discFolders)
	if f.inspectErr != nil {
		return nil, f.inspectErr
	}
	emit(Event{Kind: EventLog, Text: "scanned " + src})
	return f.report, nil
}

func (f *fakeBackend) Candidates(ctx context.Context, rep *librarian.Report, emit func(Event)) (
	map[librarian.AlbumKey][]artfetch.Candidate, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.candCalls++
	return f.cands, f.candErr
}

func (f *fakeBackend) Fix(ctx context.Context, rep *librarian.Report, ch librarian.Choices,
	emit func(Event)) (*librarian.Result, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.fixCalls = append(f.fixCalls, ch)
	if f.fixErr != nil {
		return nil, f.fixErr
	}
	emit(Event{Kind: EventLog, Text: "fixing"})
	if f.fixResult == nil {
		return &librarian.Result{Journal: "/journal/one.json", Renamed: 1}, nil
	}
	return f.fixResult, nil
}

func (f *fakeBackend) UndoFix(ctx context.Context, journal string, emit func(Event)) (
	*organizer.UndoReport, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.undoCalls = append(f.undoCalls, journal)
	if f.undoErr != nil {
		return nil, f.undoErr
	}
	if f.undoReport == nil {
		return &organizer.UndoReport{Journal: journal, Undone: 1}, nil
	}
	return f.undoReport, nil
}

func (f *fakeBackend) Thumbnail(dir string) (image.Image, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.thumbCalls = append(f.thumbCalls, dir)
	if f.thumbErr != nil {
		return nil, f.thumbErr
	}
	if img, ok := f.thumbs[dir]; ok {
		return img, nil
	}
	return nil, ErrNoThumb
}

func (f *fakeBackend) FetchThumb(ctx context.Context, url string) (image.Image, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.fetchCalls = append(f.fetchCalls, url)
	return image.NewNRGBA(image.Rect(0, 0, 8, 8)), nil
}

func (f *fakeBackend) Refresh(ctx context.Context) (Device, Library, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.pods == nil {
		return f.dev, f.lib, f.refreshErr
	}
	// Scripted: the doctor's answer follows whatever the last Detect
	// returned, the way the real one follows the real device.
	if len(f.current) == 0 {
		return Device{Err: "disk: no iPod found"}, Library{}, f.refreshErr
	}
	p := f.current[0]
	return Device{
		Found:     true,
		Path:      p.Disk.Path,
		Serial:    p.Disk.Serial,
		Model:     p.Model,
		Size:      p.Disk.SizeBytes,
		Tested:    p.Tested,
		Volume:    `D:\`,
		Installed: f.installed[p.Disk.Path],
	}, f.lib, f.refreshErr
}

// Detect hands out the next scripted set. The last one repeats, so a
// test that scripts "iPod, then nothing" gets an unplug that stays
// unplugged however many times the poll runs.
func (f *fakeBackend) Detect(ctx context.Context) ([]disk.IPod, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.detectCalls++
	if len(f.pods) == 0 {
		f.current = nil
		return nil, f.detectErr
	}
	i := f.detectCalls - 1
	if i >= len(f.pods) {
		i = len(f.pods) - 1
	}
	f.current = f.pods[i]
	return f.current, f.detectErr
}

func (f *fakeBackend) Classify(ctx context.Context, pod disk.IPod) (fwpart.Installed, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.installed[pod.Disk.Path], f.classifyErr
}

// Install runs the REAL internal/installer over a stub device and a
// stub flasher.
//
// A fake that just returned a Result would prove the button starts a
// job and nothing else; this way the app's test also proves that the
// sequence the button runs asks for the typed confirmation, is told
// this write is an install, and creates the device files on the volume.
func (f *fakeBackend) Install(ctx context.Context, o installer.Options, emit func(Event),
	confirm func(prompt string) (string, error)) (*installer.Result, error) {
	f.mu.Lock()
	f.installCalls = append(f.installCalls, o)
	pod := disk.IPod{Disk: disk.Disk{Path: f.dev.Path, Serial: f.dev.Serial}, Tested: true}
	ierr := f.installErr
	f.mu.Unlock()
	if ierr != nil {
		return nil, ierr
	}

	return installer.Install(ctx, o, installer.Deps{
		Inspect: func(context.Context) (installer.Device, error) {
			f.mu.Lock()
			defer f.mu.Unlock()
			return installer.Device{Pod: pod, Installed: f.installed[pod.Disk.Path]}, nil
		},
		Flash: func(_ context.Context, fo flasher.Options, fd flasher.Deps) (*flasher.Result, error) {
			f.mu.Lock()
			if f.installed == nil {
				f.installed = map[string]fwpart.Installed{}
			}
			f.installFlash = fo
			before := f.installed[pod.Disk.Path]
			f.mu.Unlock()
			answer, err := fd.Confirm(flasher.ConfirmPrompt(pod.Disk.Path))
			if err != nil {
				return nil, err
			}
			f.mu.Lock()
			f.installAns = answer
			f.mu.Unlock()
			if answer != pod.Disk.Path {
				return &flasher.Result{Aborted: true, Device: pod.Disk.Path}, nil
			}
			f.mu.Lock()
			// The write happened: the device is one of ours now.
			f.installed[pod.Disk.Path] = fwpart.Installed{
				Kind: fwpart.Core, Version: "v0.1.3", BuildID: "v0.1.3",
				Description: "Core v0.1.3 (build v0.1.3)",
			}
			f.dev.Installed = f.installed[pod.Disk.Path]
			f.mu.Unlock()
			return &flasher.Result{
				Verified: true, Device: pod.Disk.Path, Installed: before,
				BackupPath: `C:\Users\you\AppData\Roaming\core\backups\apple-` +
					pod.Disk.Serial + `-2026-09-15.bin`,
			}, nil
		},
		Volume:    func(disk.IPod) string { return f.volumeDir },
		Out:       emitWriter(emit),
		Confirm:   confirm,
		ChildArgs: func(image string) []string { return []string{"install", image} },
	})
}

func (f *fakeBackend) Sync(ctx context.Context, o syncer.Options, emit func(Event)) (*syncer.Plan, error) {
	f.mu.Lock()
	f.syncCalls = append(f.syncCalls, o)
	n := len(f.syncCalls)
	block, err := f.block, f.syncErr
	var plan *syncer.Plan
	if n-1 < len(f.plans) {
		plan = f.plans[n-1]
	} else {
		plan = &syncer.Plan{Albums: 1}
	}
	f.mu.Unlock()

	emit(Event{Kind: EventLog, Text: "sync call " + itoa(n)})
	emit(Event{Kind: EventProgress, Text: "working", Pct: 0.5})
	if block != nil {
		select {
		case <-block:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}
	return plan, err
}

func (f *fakeBackend) Release(ctx context.Context) (Release, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.rel, f.releaseErr
}

func (f *fakeBackend) Download(ctx context.Context, rel Release, emit func(Event)) (string, error) {
	emit(Event{Kind: EventLog, Text: "downloading " + rel.Tag})
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.downloaded, f.downloadErr
}

func (f *fakeBackend) Flash(ctx context.Context, file string,
	confirm func(prompt string) (string, error), emit func(Event)) error {
	emit(Event{Kind: EventLog, Text: "plan:\n  device  " + f.dev.Path + "\n  image   " + file})
	f.mu.Lock()
	prompt := f.flashPrompt
	f.flashCalls = append(f.flashCalls, file)
	f.mu.Unlock()

	answer, err := confirm(prompt)
	if err != nil {
		return err
	}
	f.mu.Lock()
	f.flashAnswer = answer
	ferr := f.flashErr
	f.mu.Unlock()
	if ferr != nil {
		return ferr
	}
	if answer != f.dev.Path {
		return errors.New("aborted at the confirmation prompt")
	}
	emit(Event{Kind: EventLog, Text: "VERIFIED"})
	return nil
}

func (f *fakeBackend) Backup(ctx context.Context, emit func(Event)) (string, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.backupCalls++
	return "/tmp/fwpart-1-x.bin", f.backupErr
}

// renameCall is one Rename, recorded so a test can prove the Save
// button calls the backend exactly once and with what the user typed.
type renameCall struct{ volume, name string }

// Rename does what the real one does to the string — the label is
// LegalLabel of the name — and nothing at all to a disk.
func (f *fakeBackend) Rename(ctx context.Context, volume, name string) (string, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.renameCalls = append(f.renameCalls, renameCall{volume: volume, name: name})
	if f.renameErr != nil {
		return "", f.renameErr
	}
	return disk.LegalLabel(name), nil
}

func (f *fakeBackend) Eject(ctx context.Context, volume string, emit func(Event)) error {
	emit(Event{Kind: EventLog, Text: volume + ": dismounted and ejected"})
	f.mu.Lock()
	defer f.mu.Unlock()
	f.ejectCalls = append(f.ejectCalls, volume)
	return f.ejectErr
}

// --- test plumbing -----------------------------------------------------

// newTestUI builds a UI with no window, no config file and the fake
// backend, plus a device already "found" so the buttons' preconditions
// are met.
func newTestUI(f *fakeBackend) *UI {
	if f.installed == nil {
		f.installed = map[string]fwpart.Installed{}
	}
	u := NewUI(Options{Backend: f})
	u.st.Device = Device{Found: true, Path: `\\.\PhysicalDrive2`, Volume: `D:\`, Tested: true}
	u.st.Source = "/src"
	u.sourceEd.SetText("/src")
	u.st.CLIPath = "/opt/core"
	return u
}

// settle waits for the running job and folds everything it said into
// the model — the window does this over several frames; a test does it
// in one call.
func settle(u *UI) {
	u.run.Wait()
	deadline := time.Now().Add(2 * time.Second)
	for {
		u.drain()
		u.run.Drain(&u.st)
		if !u.st.Busy() || time.Now().After(deadline) {
			return
		}
		time.Sleep(time.Millisecond)
	}
}

// answerDialogs replies to each dialog the job raises, in order. It
// stands in for the human clicking the modal's buttons.
func answerDialogs(u *UI, answers ...string) chan int {
	done := make(chan int, 1)
	go func() {
		n := 0
		deadline := time.Now().Add(2 * time.Second)
		for n < len(answers) && time.Now().Before(deadline) {
			u.mu.Lock()
			req := u.askQueue
			if req != nil {
				u.askQueue = nil
			}
			u.mu.Unlock()
			if req == nil {
				time.Sleep(time.Millisecond)
				continue
			}
			req.reply <- answers[n]
			n++
		}
		done <- n
	}()
	return done
}
