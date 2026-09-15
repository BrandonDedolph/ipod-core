package app

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// collect reads events off the runner until the terminal one arrives,
// and returns them in the order they were sent. Order is the property
// under test: a log line that arrives after the Done event is a log
// line the window appends to the NEXT job.
func collect(t *testing.T, r *Runner) []Event {
	t.Helper()
	var got []Event
	deadline := time.After(3 * time.Second)
	for {
		select {
		case e := <-r.Events():
			got = append(got, e)
			if e.Kind == EventDone || e.Kind == EventError {
				return got
			}
		case <-deadline:
			t.Fatalf("the job never ended; got %v", got)
		}
	}
}

func TestRunnerRunsOneJobAtATime(t *testing.T) {
	r := NewRunner()
	release := make(chan struct{})
	started := make(chan struct{})

	if err := r.Start(JobSync, func(ctx context.Context, emit func(Event)) error {
		close(started)
		<-release
		return nil
	}); err != nil {
		t.Fatalf("the first Start was refused: %v", err)
	}
	<-started

	if !r.Busy() {
		t.Error("Busy() is false while a job is running")
	}
	if got := r.Kind(); got != JobSync {
		t.Errorf("Kind() = %v, want JobSync", got)
	}
	// The second job is refused, not queued: two of these open the same
	// raw device and one of them writes it.
	if err := r.Start(JobFlash, func(context.Context, func(Event)) error { return nil }); !errors.Is(err, ErrBusy) {
		t.Fatalf("the second Start returned %v, want ErrBusy", err)
	}

	close(release)
	collect(t, r)
	if r.Busy() {
		t.Error("Busy() is still true after the job ended")
	}
	if got := r.Kind(); got != JobNone {
		t.Errorf("Kind() after the job = %v, want JobNone", got)
	}
	// And now a new job is accepted.
	if err := r.Start(JobFlash, func(context.Context, func(Event)) error { return nil }); err != nil {
		t.Fatalf("a job after the first finished was refused: %v", err)
	}
	collect(t, r)
}

func TestRunnerEventsArriveInOrderAndEndWithDone(t *testing.T) {
	r := NewRunner()
	if err := r.Start(JobSync, func(ctx context.Context, emit func(Event)) error {
		emit(Event{Kind: EventLog, Text: "one"})
		emit(Event{Kind: EventProgress, Text: "half", Pct: 0.5})
		emit(Event{Kind: EventLog, Text: "two"})
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	got := collect(t, r)

	want := []struct {
		kind EventKind
		text string
	}{
		{EventLog, "one"},
		{EventProgress, "half"},
		{EventLog, "two"},
		{EventDone, ""},
	}
	if len(got) != len(want) {
		t.Fatalf("got %d events, want %d: %v", len(got), len(want), got)
	}
	for i, w := range want {
		if got[i].Kind != w.kind || got[i].Text != w.text {
			t.Errorf("event %d = %v %q, want %v %q", i, got[i].Kind, got[i].Text, w.kind, w.text)
		}
		if got[i].Job != JobSync {
			t.Errorf("event %d is tagged %v, want JobSync", i, got[i].Job)
		}
	}
}

func TestRunnerFailingJobEndsWithError(t *testing.T) {
	r := NewRunner()
	if err := r.Start(JobFlash, func(ctx context.Context, emit func(Event)) error {
		emit(Event{Kind: EventLog, Text: "about to fail"})
		return errors.New("the device went away")
	}); err != nil {
		t.Fatal(err)
	}
	got := collect(t, r)
	last := got[len(got)-1]
	if last.Kind != EventError {
		t.Fatalf("the last event is %v, want EventError", last.Kind)
	}
	if last.Text != "the device went away" {
		t.Errorf("the error text is %q", last.Text)
	}
	if last.Job != JobFlash {
		t.Errorf("the error is tagged %v, want JobFlash", last.Job)
	}
}

// A job that returns because its context was cancelled reports
// "cancelled" and not "context canceled": the log pane is read by
// someone who pressed a button, not by a Go programmer.
func TestRunnerCancelPropagates(t *testing.T) {
	r := NewRunner()
	started := make(chan struct{})
	if err := r.Start(JobSync, func(ctx context.Context, emit func(Event)) error {
		close(started)
		<-ctx.Done()
		return ctx.Err()
	}); err != nil {
		t.Fatal(err)
	}
	<-started
	r.Cancel()

	got := collect(t, r)
	last := got[len(got)-1]
	if last.Kind != EventError || last.Text != "cancelled" {
		t.Fatalf("a cancelled job ended with %v %q, want EventError \"cancelled\"", last.Kind, last.Text)
	}
}

// Cancel on an idle runner is a no-op, not a panic: the button is
// disabled but a click can still land on the frame the job ended in.
func TestRunnerCancelWhenIdle(t *testing.T) {
	NewRunner().Cancel()
}

func TestRunnerDrainAppliesToState(t *testing.T) {
	r := NewRunner()
	if err := r.Start(JobBackup, func(ctx context.Context, emit func(Event)) error {
		emit(Event{Kind: EventLog, Text: "a"})
		emit(Event{Kind: EventLog, Text: "b"})
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	r.Wait()
	var st State
	if n := r.Drain(&st); n != 3 {
		t.Fatalf("Drain moved %d events, want 3", n)
	}
	if len(st.Log) != 2 {
		t.Fatalf("the log has %d lines, want 2: %v", len(st.Log), st.Log)
	}
	if st.Job == nil || !st.Job.Done {
		t.Fatalf("the job status is %+v, want a finished JobBackup", st.Job)
	}
	// A second Drain on an empty channel returns immediately.
	if n := r.Drain(&st); n != 0 {
		t.Errorf("a second Drain moved %d events, want 0", n)
	}
}

func TestRunnerRecoversAPanicIntoAnError(t *testing.T) {
	r := NewRunner()
	if err := r.Start(JobSync, func(ctx context.Context, emit func(Event)) error {
		panic("boom")
	}); err != nil {
		t.Fatal(err)
	}
	r.Wait()
	var last Event
	for {
		select {
		case e := <-r.Events():
			last = e
			continue
		default:
		}
		break
	}
	if last.Kind != EventError || !strings.Contains(last.Text, "panic: boom") {
		t.Errorf("terminal event after a panic: %+v", last)
	}
	if r.Busy() {
		t.Error("runner still busy after a panicking job")
	}
}
