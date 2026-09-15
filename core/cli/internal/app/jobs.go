package app

import (
	"context"
	"errors"
	"fmt"
	"runtime/debug"
	"sync"
)

// ErrBusy is Start's answer when a job is already running.
//
// One job at a time is not a simplification, it is the safety property.
// Two of these jobs open the same raw device; one of them writes it.
// A queue would be worse than a refusal: a user who clicked Flash and
// then Sync would get the flash, then a sync onto a volume the flash
// had just dismounted, with nothing on screen having asked. So the
// buttons go grey while something runs, and a click that slipped
// through anyway is refused here rather than run late.
var ErrBusy = errors.New("app: another job is already running")

// Runner runs one job at a time and reports what it does over a
// channel.
//
// The channel is buffered and the sender never drops: a log line lost
// because the UI was mid-frame is a log line missing from the record of
// a flash. If the buffer fills, the job blocks until the window drains
// it, which is the correct back-pressure — the job is slower, the log
// is complete.
type Runner struct {
	mu      sync.Mutex
	events  chan Event
	running bool
	kind    JobKind
	cancel  context.CancelFunc
	// done is closed when the current job's goroutine has sent its
	// final event. Tests wait on it; the window never does.
	done chan struct{}
}

// EventBuffer is the channel depth. A sync of a thousand albums emits
// a few thousand events over minutes, so this only has to cover the gap
// between two frames.
const EventBuffer = 256

// NewRunner returns an idle runner.
func NewRunner() *Runner {
	return &Runner{events: make(chan Event, EventBuffer)}
}

// Events is the stream. It is never closed: the window outlives every
// job, and a closed channel would turn "the last job finished" into "the
// app is shutting down" for the pump goroutine.
func (r *Runner) Events() <-chan Event { return r.events }

// Busy reports whether a job is running.
func (r *Runner) Busy() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.running
}

// Kind is the running job, or JobNone.
func (r *Runner) Kind() JobKind {
	r.mu.Lock()
	defer r.mu.Unlock()
	if !r.running {
		return JobNone
	}
	return r.kind
}

// Start runs fn on its own goroutine.
//
// fn gets a context that Cancel cancels and an emit function that tags
// every Event with this job's kind. Exactly one terminal event is sent
// per job: EventDone when fn returns nil, EventError otherwise — and
// that is sent by Start, not by fn, so a job that forgets cannot leave
// the buttons grey forever.
func (r *Runner) Start(kind JobKind, fn func(ctx context.Context, emit func(Event)) error) error {
	r.mu.Lock()
	if r.running {
		r.mu.Unlock()
		return ErrBusy
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	r.running, r.kind, r.cancel, r.done = true, kind, cancel, done
	r.mu.Unlock()

	emit := func(e Event) {
		e.Job = kind
		r.events <- e
	}

	go func() {
		defer close(done)
		defer cancel()
		err := runRecovering(ctx, emit, fn)

		// Mark idle BEFORE the terminal event goes out. The window
		// enables its buttons when it sees EventDone, and a button
		// enabled a moment before Busy() went false is a button whose
		// first click is refused with ErrBusy for no reason a user
		// could understand.
		r.mu.Lock()
		r.running, r.kind, r.cancel = false, JobNone, nil
		r.mu.Unlock()

		if err != nil {
			text := err.Error()
			if errors.Is(err, context.Canceled) {
				text = "cancelled"
			}
			r.events <- Event{Kind: EventError, Job: kind, Text: text}
			return
		}
		r.events <- Event{Kind: EventDone, Job: kind}
	}()
	return nil
}

// runRecovering turns a panic in a job into its error. A GUI process
// that dies mid-sync leaves a half-copied library and a window that
// simply vanished; a job that ends with "panic: …" in the log leaves
// the same files and an explanation.
func runRecovering(ctx context.Context, emit func(Event), fn func(context.Context, func(Event)) error) (err error) {
	defer func() {
		if p := recover(); p != nil {
			err = fmt.Errorf("panic: %v\n%s", p, debug.Stack())
		}
	}()
	return fn(ctx, emit)
}

// Cancel asks the running job to stop.
//
// It is a request, not a guarantee, and for the two jobs that write the
// device it is deliberately almost nothing: internal/flasher does not
// check the context between the body write and the directory write,
// because a partition with a new body and an old directory row is the
// one state nothing on the device can boot out of. The UI says so on
// the button rather than offering a stop that would not stop.
func (r *Runner) Cancel() {
	r.mu.Lock()
	cancel := r.cancel
	r.mu.Unlock()
	if cancel != nil {
		cancel()
	}
}

// Wait blocks until the running job has sent its terminal event. It
// exists for the tests and for a clean shutdown; the window never calls
// it, because blocking the UI goroutine on a job is how an app stops
// repainting.
func (r *Runner) Wait() {
	r.mu.Lock()
	done := r.done
	r.mu.Unlock()
	if done != nil {
		<-done
	}
}

// Drain moves every event currently queued into st. This is what the
// window calls at the top of a frame.
func (r *Runner) Drain(st *State) int {
	n := 0
	for {
		select {
		case e := <-r.events:
			st.Apply(e)
			n++
		default:
			return n
		}
	}
}
