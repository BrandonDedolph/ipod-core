package app

import (
	"context"
	"strings"
	"sync"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
)

// Auto-detect: the window finds the iPod by itself, and notices when it
// goes away.
//
// It is a poll and not a device notification because there is no way to
// receive one here. WM_DEVICECHANGE arrives at a window procedure, and
// Gio owns the only one this process has; libudev is Linux and this app
// is for Windows. Two raw opens a second is free next to what the user
// is about to ask for, and the same loop catches the unplug — which a
// one-shot "find it at launch" never would (library-manager-plan.md,
// decision 4).
//
// Two rules hold it together. The poll does NOT run while a job runs:
// the flasher dismounts the volume and opens the raw device, and a
// detector opening the same device in the middle of that is a race
// nobody needs. And the poll never touches State — it publishes one
// small struct under the mutex and the next frame's drain() decides what
// to do with it on the UI goroutine, like every other job result.

// DetectInterval is how often the poll looks.
const DetectInterval = 2 * time.Second

// detectNews is one poll's verdict, handed to the UI goroutine.
type detectNews struct {
	// found reports whether there is at least one iPod attached now.
	found bool
	// err is the one-line reason there is none, when there is one to
	// give (a raw open denied for want of Administrator, say).
	err string
}

// detector remembers what the last poll saw, so a poll that finds the
// same iPod as the last one is silent. Without it every tick would
// start a Refresh and the log would be a list of the same device twice
// a second.
type detector struct {
	mu   sync.Mutex
	last string
	seen bool
}

// change records key and reports whether it differs from the last one.
func (d *detector) change(key string) bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.seen && d.last == key {
		return false
	}
	d.last, d.seen = key, true
	return true
}

// forget drops the memory, so the next poll publishes whatever it finds.
// The UI goroutine calls it when it could not act on the news — a job
// started between the poll and the frame — because the alternative is a
// window that has decided an iPod is attached and never says so.
func (d *detector) forget() {
	d.mu.Lock()
	d.last, d.seen = "", false
	d.mu.Unlock()
}

// StartDetect starts the poll and returns immediately. It must be called
// from the UI goroutine (it sets the flag the Looking screen reads);
// everything after that runs on its own goroutine until ctx is done.
//
// tick is the clock. nil means a real DetectInterval ticker; a test
// passes its own channel so one poll is one send and the test does not
// wait two seconds to find out what the window does.
func (u *UI) StartDetect(ctx context.Context, tick <-chan time.Time) {
	u.st.Detecting = true
	if tick == nil {
		t := time.NewTicker(DetectInterval)
		tick = t.C
		go func() {
			<-ctx.Done()
			t.Stop()
		}()
	}
	go func() {
		// Once immediately: an iPod that was already plugged in when the
		// app opened should be found now, not in two seconds.
		u.detectOnce(ctx)
		for {
			select {
			case <-ctx.Done():
				return
			case <-tick:
				u.detectOnce(ctx)
			}
		}
	}()
}

// detectOnce is one poll. It reports whether it published anything, for
// the tests; the loop ignores the answer.
func (u *UI) detectOnce(ctx context.Context) bool {
	if u.run.Busy() {
		// Paused, and deliberately without forgetting the last key: the
		// job that is running is the one changing the device, and it
		// ends with a Refresh of its own.
		return false
	}
	pods, err := u.be.Detect(ctx)
	if ctx.Err() != nil {
		return false
	}
	if !u.det.change(detectKey(pods, err)) {
		return false
	}
	news := &detectNews{found: len(pods) > 0}
	if !news.found && err != nil {
		news.err = firstLine(err.Error())
	}
	u.mu.Lock()
	u.newDetect = news
	u.mu.Unlock()
	u.invalidate()
	return true
}

// detectKey is the identity of a found set: path and serial per device,
// in the order FindIPods returned them, or the error when there is
// nothing. Path alone would miss two iPods swapped between ports;
// serial alone would miss a device that moved from one path to another
// while keeping the same one (a re-enumeration), which is exactly the
// moment a Refresh is needed.
func detectKey(pods []disk.IPod, err error) string {
	if len(pods) == 0 {
		if err != nil {
			return "err:" + err.Error()
		}
		return "none"
	}
	var b strings.Builder
	for _, p := range pods {
		b.WriteString(p.Disk.Path)
		b.WriteByte('|')
		b.WriteString(p.Disk.Serial)
		b.WriteByte(';')
	}
	return b.String()
}

// applyDetect folds one poll's news into the model. UI goroutine only.
func (u *UI) applyDetect(n *detectNews) {
	if !n.found {
		had := u.st.Device.Found
		reason := n.err
		if had {
			reason = "the iPod was unplugged"
		}
		// Everything about the device goes, library included: a card
		// showing 928 songs on a drive that is not there is a card that
		// will be believed.
		u.st.Device = Device{Err: reason, Elevated: u.st.Device.Elevated}
		u.st.Library = Library{}
		if reason != "" {
			u.st.Logf("%s", reason)
		}
		return
	}
	if u.st.Busy() {
		// A job started between the poll and this frame. Forget, so the
		// next tick tells us again.
		u.det.forget()
		return
	}
	u.startRefresh()
}
