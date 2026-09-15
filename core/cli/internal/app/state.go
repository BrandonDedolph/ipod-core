// Package app is core-app, the desktop application: a Gio window over
// exactly the same internal packages the `core` CLI drives.
//
// The split inside this package is the one that makes a GUI testable.
// State is a plain struct; Event is a plain struct; applying an Event
// to a State is a pure function; Runner is the only thing that starts
// goroutines; Backend is an interface with one real implementation and
// one fake. ui.go turns a State into widgets and never does I/O, and
// run.go owns the window. Everything anybody could get wrong about job
// serialisation, event ordering or cancellation is therefore reachable
// from `go test` with no display attached.
//
// One rule holds the whole thing together: State is mutated only by the
// UI goroutine. Jobs run on their own goroutines and say what happened
// by sending Events; the window's pump appends them to a queue and asks
// for a frame; Layout drains the queue into State at the top of the
// frame. No lock protects State itself, because nothing else ever
// touches it.
package app

import (
	"fmt"
	"strings"
	"time"
)

// LogLines is how much of the log the window keeps. It is a ring: a
// long sync prints one line per album and a big library is a thousand
// of them, and a GUI that grows without bound while a user leaves it
// open overnight is a GUI that eventually stops repainting.
const LogLines = 2000

// JobKind names one unit of work. There is at most one running at a
// time — see Runner — so this doubles as "what is the app doing".
type JobKind int

// The jobs.
const (
	JobNone JobKind = iota
	JobRefresh
	JobDryRun
	JobSync
	JobSyncPrune
	JobCheck
	JobUpdate
	JobFlash
	JobBackup
	JobEject
)

// String is the label shown in the status line and written into the
// log, so it reads as a sentence fragment rather than as an identifier.
func (k JobKind) String() string {
	switch k {
	case JobRefresh:
		return "refresh"
	case JobDryRun:
		return "dry run"
	case JobSync:
		return "sync"
	case JobSyncPrune:
		return "sync + prune"
	case JobCheck:
		return "check for updates"
	case JobUpdate:
		return "update firmware"
	case JobFlash:
		return "flash"
	case JobBackup:
		return "backup"
	case JobEject:
		return "eject"
	default:
		return "idle"
	}
}

// Writes reports whether this job writes to the device's raw firmware
// partition. The two that do are the two the UI guards with a typed
// confirmation, and the two whose Cancel button is a lie (see
// Runner.Cancel).
func (k JobKind) Writes() bool { return k == JobFlash || k == JobUpdate }

// EventKind is what an Event says.
type EventKind int

// The kinds of Event.
const (
	// EventLog is one line for the log pane.
	EventLog EventKind = iota
	// EventProgress updates the running job's status line and bar.
	// Pct < 0 means indeterminate.
	EventProgress
	// EventDone ends a job successfully. Runner sends exactly one per
	// job that returns nil.
	EventDone
	// EventError ends a job with a failure. Runner sends exactly one
	// per job that returns an error.
	EventError
)

// String makes an Event readable in a test failure.
func (k EventKind) String() string {
	switch k {
	case EventLog:
		return "log"
	case EventProgress:
		return "progress"
	case EventDone:
		return "done"
	case EventError:
		return "error"
	default:
		return "?"
	}
}

// Event is one thing a job has to say.
type Event struct {
	Kind EventKind
	Job  JobKind
	Text string
	// Pct is 0..1 for EventProgress, or negative for "still working,
	// no idea how far".
	Pct float32
}

// Device is what the Device card shows.
type Device struct {
	Found                 bool
	Path, Model, Serial   string
	Size                  int64
	SectorSize            int
	Tested                bool
	Volume                string
	Firmware              string
	OSOSOK                bool
	OSOSNote              string
	Err                   string
	Elevated              bool
	ElevationAdviceNeeded bool
}

// Library is the volume half of the Device card.
type Library struct {
	Present               bool
	Songs, Albums, Genres int
	IndexBytes            int64
	ConfigValid, LogValid bool
	Note                  string
}

// Release is what `Check` filled in.
type Release struct {
	Checked bool
	Tag     string
	Notes   string
	Asset   string
	Err     string
}

// JobStatus is the running (or last-finished) job.
type JobStatus struct {
	Kind JobKind
	Text string
	// Pct is 0..1, or negative for indeterminate.
	Pct          float32
	Done, Failed bool
}

// Running reports whether this job is still going.
func (j *JobStatus) Running() bool { return j != nil && !j.Done && !j.Failed }

// State is the whole model. Everything on screen is a function of it.
type State struct {
	Device  Device
	Library Library
	Release Release
	// Source is the music folder; FlashFile is the image path typed
	// into the Firmware card.
	Source    string
	FlashFile string
	// CLIPath is the `core` binary beside the app, or "" when there is
	// none — the Firmware card says so rather than pretending a flash
	// would work.
	CLIPath string
	Job     *JobStatus
	Log     []string
}

// Busy reports whether a job is running. Every button except Cancel is
// disabled while it is true.
func (s *State) Busy() bool { return s.Job.Running() }

// CanCancel reports whether the Cancel button does anything. It is
// false for the two jobs that write the device: the flasher does not
// stop between the body write and the directory write, and the
// elevated child on Windows cannot be reached at all, so a Cancel that
// only cancelled the parent's context would report "cancelled" over a
// write that finished. The button is greyed and says why instead.
func (s *State) CanCancel() bool { return s.Busy() && !s.Job.Kind.Writes() }

// Logf appends one line to the ring.
func (s *State) Logf(format string, args ...any) {
	s.appendLines(fmt.Sprintf(format, args...))
}

// appendLines splits on newlines so a multi-line block from a
// subprocess or from the flasher arrives as separate lines, which is
// what a log pane with a scrollbar wants.
func (s *State) appendLines(text string) {
	text = strings.ReplaceAll(text, "\r\n", "\n")
	text = strings.TrimRight(text, "\n")
	if text == "" {
		// A deliberate blank line is still a line: the flasher prints
		// them to separate the plan from the result.
		s.push("")
		return
	}
	for _, line := range strings.Split(text, "\n") {
		s.push(strings.TrimRight(line, "\r"))
	}
}

func (s *State) push(line string) {
	s.Log = append(s.Log, line)
	if len(s.Log) > LogLines {
		// Copy rather than reslice: reslicing keeps the whole backing
		// array alive forever, which is the leak this ring exists to
		// avoid.
		keep := make([]string, LogLines)
		copy(keep, s.Log[len(s.Log)-LogLines:])
		s.Log = keep
	}
}

// Apply folds one Event into the State. It is the only place a State
// changes in response to a job, and it is pure: no I/O, no goroutines,
// no clock beyond the timestamps the caller already put in the text.
func (s *State) Apply(e Event) {
	switch e.Kind {
	case EventLog:
		s.appendLines(e.Text)
	case EventProgress:
		if s.Job == nil || s.Job.Kind != e.Job {
			s.Job = &JobStatus{Kind: e.Job}
		}
		if e.Text != "" {
			s.Job.Text = e.Text
		}
		s.Job.Pct = e.Pct
	case EventDone:
		if s.Job == nil || s.Job.Kind != e.Job {
			s.Job = &JobStatus{Kind: e.Job}
		}
		s.Job.Done, s.Job.Failed = true, false
		s.Job.Pct = 1
		if e.Text != "" {
			s.Job.Text = e.Text
			s.appendLines(e.Text)
		}
	case EventError:
		if s.Job == nil || s.Job.Kind != e.Job {
			s.Job = &JobStatus{Kind: e.Job}
		}
		s.Job.Done, s.Job.Failed = false, true
		s.Job.Text = e.Text
		s.appendLines("error: " + e.Text)
	}
}

// StatusLine is the sentence in the header: what is happening, or what
// happened last.
func (s *State) StatusLine() string {
	if s.Job == nil {
		return "ready"
	}
	switch {
	case s.Job.Failed:
		return s.Job.Kind.String() + " failed — " + firstLine(s.Job.Text)
	case s.Job.Done:
		if s.Job.Text != "" {
			return s.Job.Kind.String() + ": " + firstLine(s.Job.Text)
		}
		return s.Job.Kind.String() + " finished"
	default:
		if s.Job.Text != "" {
			return s.Job.Kind.String() + ": " + firstLine(s.Job.Text)
		}
		return s.Job.Kind.String() + " …"
	}
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}

// stamp is the "14:02:11" prefix the log lines carry. A sync that
// stalls on one album looks exactly like a sync that finished without
// saying so, unless the lines are timed.
func stamp(t time.Time) string { return t.Format("15:04:05") }
