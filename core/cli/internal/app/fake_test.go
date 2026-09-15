package app

import (
	"context"
	"errors"
	"sync"
	"time"

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

	syncCalls   []syncer.Options
	flashCalls  []string
	flashPrompt string
	flashAnswer string
	backupCalls int
	ejectCalls  []string
	downloaded  string
}

func (f *fakeBackend) Refresh(ctx context.Context) (Device, Library, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.dev, f.lib, f.refreshErr
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
