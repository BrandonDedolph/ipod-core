package app

import (
	"strings"
	"testing"
)

func TestApplyLogSplitsBlocks(t *testing.T) {
	var st State
	// The flasher prints paragraphs through one Write; the pane wants
	// lines.
	st.Apply(Event{Kind: EventLog, Text: "plan:\r\n  device  X\n  image   Y\n"})
	want := []string{"plan:", "  device  X", "  image   Y"}
	if len(st.Log) != len(want) {
		t.Fatalf("got %d lines, want %d: %q", len(st.Log), len(want), st.Log)
	}
	for i := range want {
		if st.Log[i] != want[i] {
			t.Errorf("line %d = %q, want %q", i, st.Log[i], want[i])
		}
	}
}

func TestApplyProgressThenDone(t *testing.T) {
	var st State
	st.Apply(Event{Kind: EventProgress, Job: JobSync, Text: "copying", Pct: 0.25})
	if !st.Busy() {
		t.Fatal("a progress event did not mark the state busy")
	}
	if st.Job.Kind != JobSync || st.Job.Text != "copying" || st.Job.Pct != 0.25 {
		t.Fatalf("job status = %+v", st.Job)
	}
	if got, want := st.StatusLine(), "sync: copying"; got != want {
		t.Errorf("StatusLine() = %q, want %q", got, want)
	}

	st.Apply(Event{Kind: EventDone, Job: JobSync, Text: "928 tracks"})
	if st.Busy() {
		t.Error("the state is still busy after EventDone")
	}
	if st.Job.Pct != 1 {
		t.Errorf("a finished job is at %v, want 1", st.Job.Pct)
	}
	if got := st.StatusLine(); got != "sync: 928 tracks" {
		t.Errorf("StatusLine() = %q", got)
	}
	if last := st.Log[len(st.Log)-1]; last != "928 tracks" {
		t.Errorf("the done text was not logged: %q", st.Log)
	}
}

func TestApplyErrorMarksFailedAndLogs(t *testing.T) {
	var st State
	st.Apply(Event{Kind: EventProgress, Job: JobFlash, Pct: -1})
	st.Apply(Event{Kind: EventError, Job: JobFlash, Text: "the device went away\nsecond line"})
	if st.Busy() {
		t.Error("a failed job left the state busy")
	}
	if !st.Job.Failed {
		t.Error("Failed is not set")
	}
	if got, want := st.StatusLine(), "flash failed — the device went away"; got != want {
		t.Errorf("StatusLine() = %q, want %q", got, want)
	}
	if !strings.HasPrefix(st.Log[0], "error: the device went away") {
		t.Errorf("the error was not logged as an error: %q", st.Log)
	}
	if len(st.Log) != 2 {
		t.Errorf("a two-line error produced %d log lines: %q", len(st.Log), st.Log)
	}
}

// A progress event for a different job replaces the status rather than
// updating the old one, so a stale "sync" label cannot sit over a flash.
func TestApplyProgressForANewJobReplacesTheStatus(t *testing.T) {
	var st State
	st.Apply(Event{Kind: EventProgress, Job: JobSync, Text: "copying", Pct: 0.5})
	st.Apply(Event{Kind: EventProgress, Job: JobFlash, Text: "writing", Pct: 0.1})
	if st.Job.Kind != JobFlash || st.Job.Text != "writing" {
		t.Fatalf("job status = %+v", st.Job)
	}
}

func TestLogIsARing(t *testing.T) {
	var st State
	for i := 0; i < LogLines+250; i++ {
		st.Logf("line %d", i)
	}
	if len(st.Log) != LogLines {
		t.Fatalf("the log holds %d lines, want %d", len(st.Log), LogLines)
	}
	if st.Log[0] != "line 250" {
		t.Errorf("the ring dropped the wrong end: first line is %q", st.Log[0])
	}
	if last := st.Log[LogLines-1]; last != "line 2249" {
		t.Errorf("the ring lost the newest line: last is %q", last)
	}
}

func TestStatusLineWhenIdle(t *testing.T) {
	var st State
	if got := st.StatusLine(); got != "ready" {
		t.Errorf("StatusLine() with no job = %q, want \"ready\"", got)
	}
}

func TestJobKindWrites(t *testing.T) {
	for _, k := range []JobKind{JobFlash, JobUpdate} {
		if !k.Writes() {
			t.Errorf("%v does not report that it writes", k)
		}
	}
	for _, k := range []JobKind{JobRefresh, JobDryRun, JobSync, JobSyncPrune, JobCheck, JobBackup, JobEject} {
		if k.Writes() {
			t.Errorf("%v claims to write the firmware partition", k)
		}
	}
}

func TestComma(t *testing.T) {
	for _, c := range []struct {
		in   int64
		want string
	}{{0, "0"}, {999, "999"}, {1000, "1,000"}, {237584, "237,584"}, {7618560, "7,618,560"}} {
		if got := comma(c.in); got != c.want {
			t.Errorf("comma(%d) = %q, want %q", c.in, got, c.want)
		}
	}
}

func TestCanCancelIsFalseForTheWriteJobs(t *testing.T) {
	var s State
	if s.CanCancel() {
		t.Error("idle: CanCancel should be false")
	}
	s.Job = &JobStatus{Kind: JobSync, Pct: -1}
	if !s.CanCancel() {
		t.Error("sync: CanCancel should be true")
	}
	for _, k := range []JobKind{JobFlash, JobUpdate} {
		s.Job = &JobStatus{Kind: k, Pct: -1}
		if s.CanCancel() {
			t.Errorf("%s: CanCancel should be false", k)
		}
	}
}
