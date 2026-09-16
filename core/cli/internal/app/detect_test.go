package app

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
)

// The launch phases, through the fake: no iPod → Looking; an iPod
// running Apple's firmware → NotInstalled; one running Core → Ready;
// unplug → Looking again. Nothing here waits two seconds for anything:
// the poll's clock is injected and one tick is one send.

const testPath = `\\.\PhysicalDrive9`

func applePod() disk.IPod {
	return disk.IPod{
		Disk:  disk.Disk{Path: testPath, Serial: "TESTSERIAL", SizeBytes: 80_026_361_856},
		Model: "iPod Video 5.5G 80 GB", Tested: true,
	}
}

func appleFirmware() fwpart.Installed {
	return fwpart.Installed{Kind: fwpart.Other, Description: "Apple firmware (7.6 MB, entry 0x736000)"}
}

func coreFirmware() fwpart.Installed {
	return fwpart.Installed{Kind: fwpart.Core, Version: "v0.1.3", Description: "Core v0.1.3 (build v0.1.3)"}
}

// poll runs one detection and folds its news into the model, which is
// what a tick plus the next frame do in the window.
func poll(u *UI) {
	u.detectOnce(context.Background())
	u.drain()
	settle(u)
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func (f *fakeBackend) detects() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.detectCalls
}

// The phase is a function of the model, so it is worth one table: the
// three screens and the one case that must not be NotInstalled — a
// device nobody has classified yet.
func TestPhaseFollowsTheDevice(t *testing.T) {
	cases := []struct {
		name string
		dev  Device
		want Phase
	}{
		{"nothing plugged in", Device{}, PhaseLooking},
		{"apple firmware", Device{Found: true, Installed: appleFirmware()}, PhaseNotInstalled},
		{"core", Device{Found: true, Installed: coreFirmware()}, PhaseReady},
		{"an old core is installed, not missing",
			Device{Found: true, Installed: fwpart.Installed{Kind: fwpart.CoreOld}}, PhaseReady},
		{"unclassified is not an install prompt", Device{Found: true}, PhaseReady},
	}
	for _, c := range cases {
		st := State{Device: c.dev}
		if got := st.Phase(); got != c.want {
			t.Errorf("%s: phase %v, want %v", c.name, got, c.want)
		}
	}
}

func TestDetectReachesReadyWithNoClick(t *testing.T) {
	f := &fakeBackend{
		pods:      [][]disk.IPod{{applePod()}},
		installed: map[string]fwpart.Installed{testPath: coreFirmware()},
		lib:       Library{Present: true, Songs: 928, Albums: 101},
	}
	u := NewUI(Options{Backend: f})
	if u.st.Phase() != PhaseLooking {
		t.Fatalf("a window that has looked at nothing is on %v", u.st.Phase())
	}
	poll(u)
	if u.st.Phase() != PhaseReady {
		t.Fatalf("after finding a Core iPod the phase is %v", u.st.Phase())
	}
	if !u.st.Device.Found || u.st.Device.Path != testPath {
		t.Errorf("the device was not filled in: %+v", u.st.Device)
	}
	if u.st.Library.Songs != 928 {
		t.Errorf("the library was not read: %+v", u.st.Library)
	}
}

func TestDetectOnAStockIPodStopsAtNotInstalled(t *testing.T) {
	f := &fakeBackend{
		pods:      [][]disk.IPod{{applePod()}},
		installed: map[string]fwpart.Installed{testPath: appleFirmware()},
	}
	u := NewUI(Options{Backend: f})
	poll(u)
	if u.st.Phase() != PhaseNotInstalled {
		t.Fatalf("an iPod running Apple's firmware put the window on %v", u.st.Phase())
	}
	if u.st.Device.Installed.Kind != fwpart.Other {
		t.Errorf("the classification did not reach the model: %+v", u.st.Device.Installed)
	}
}

// Unplugging must take the window back to Looking AND take the library
// with it: 928 songs shown for a drive that is not there is a number
// somebody will believe.
func TestUnplugReturnsToLookingAndClearsTheLibrary(t *testing.T) {
	f := &fakeBackend{
		pods:      [][]disk.IPod{{applePod()}, nil},
		installed: map[string]fwpart.Installed{testPath: coreFirmware()},
		lib:       Library{Present: true, Songs: 928},
	}
	u := NewUI(Options{Backend: f})
	poll(u)
	if u.st.Phase() != PhaseReady {
		t.Fatalf("the first poll left the window on %v", u.st.Phase())
	}
	poll(u)
	if u.st.Phase() != PhaseLooking {
		t.Fatalf("after the unplug the window is on %v", u.st.Phase())
	}
	if u.st.Library.Present || u.st.Library.Songs != 0 {
		t.Errorf("the library survived the unplug: %+v", u.st.Library)
	}
	if !strings.Contains(strings.Join(u.st.Log, "\n"), "unplugged") {
		t.Errorf("the log does not say the iPod went away:\n%s", strings.Join(u.st.Log, "\n"))
	}
	// And it keeps looking: a third poll with nothing attached is
	// silent rather than an error.
	poll(u)
	if u.st.Phase() != PhaseLooking {
		t.Errorf("the third poll moved the window to %v", u.st.Phase())
	}
}

// The same set of devices twice is one Refresh. Without this the log
// would be a list of the same iPod every two seconds forever.
func TestDetectIsSilentWhenNothingChanged(t *testing.T) {
	f := &fakeBackend{
		pods:      [][]disk.IPod{{applePod()}},
		installed: map[string]fwpart.Installed{testPath: coreFirmware()},
	}
	u := NewUI(Options{Backend: f})
	poll(u)
	lines := len(u.st.Log)
	for i := 0; i < 3; i++ {
		if u.detectOnce(context.Background()) {
			t.Fatalf("poll %d published news about an unchanged device", i+2)
		}
	}
	u.drain()
	if len(u.st.Log) != lines {
		t.Errorf("the log grew by %d lines with nothing happening", len(u.st.Log)-lines)
	}
}

// The poll must not open the raw device while a job has it. The one
// that matters is a flash, which dismounts the volume; a detector
// opening the same disk in the middle of that is a race nobody needs.
func TestPollIsPausedWhileAJobRuns(t *testing.T) {
	f := &fakeBackend{
		pods:      [][]disk.IPod{{applePod()}},
		installed: map[string]fwpart.Installed{testPath: coreFirmware()},
		block:     make(chan struct{}),
	}
	u := newTestUI(f)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	u.startSync(JobSync)
	waitFor(t, "the sync to start", func() bool { return u.run.Busy() })

	ticks := make(chan time.Time)
	u.StartDetect(ctx, ticks)
	for i := 0; i < 3; i++ {
		ticks <- time.Now()
	}
	if n := f.detects(); n != 0 {
		t.Fatalf("the poll ran %d times during a job", n)
	}
	if !u.st.Detecting {
		t.Error("StartDetect did not tell the model the poll is on")
	}

	close(f.block)
	settle(u)
	ticks <- time.Now()
	waitFor(t, "the poll to resume", func() bool { return f.detects() > 0 })
}

// The immediate poll at launch: an iPod that was already plugged in
// must be found now, not in two seconds.
func TestStartDetectLooksOnceImmediately(t *testing.T) {
	f := &fakeBackend{
		pods:      [][]disk.IPod{{applePod()}},
		installed: map[string]fwpart.Installed{testPath: coreFirmware()},
	}
	u := newTestUI(f)
	u.st.Device = Device{}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	u.StartDetect(ctx, make(chan time.Time))
	waitFor(t, "the first poll", func() bool { return f.detects() > 0 })
	waitFor(t, "the news to be published", func() bool {
		u.mu.Lock()
		defer u.mu.Unlock()
		return u.newDetect != nil
	})
	u.drain()
	settle(u)
	if u.st.Phase() != PhaseReady {
		t.Errorf("the launch poll left the window on %v", u.st.Phase())
	}
}

// detectKey is what decides "same device as last time". Two iPods
// swapped between ports, or one that re-enumerated onto another path,
// are both changes.
func TestDetectKeyDistinguishesPathAndSerial(t *testing.T) {
	a := disk.IPod{Disk: disk.Disk{Path: `\\.\PhysicalDrive1`, Serial: "AAA"}}
	b := disk.IPod{Disk: disk.Disk{Path: `\\.\PhysicalDrive2`, Serial: "BBB"}}
	same := detectKey([]disk.IPod{a}, nil)
	if same != detectKey([]disk.IPod{a}, nil) {
		t.Error("the same device produced two keys")
	}
	for name, other := range map[string][]disk.IPod{
		"another path":   {{Disk: disk.Disk{Path: `\\.\PhysicalDrive3`, Serial: "AAA"}}},
		"another serial": {{Disk: disk.Disk{Path: `\\.\PhysicalDrive1`, Serial: "CCC"}}},
		"two devices":    {a, b},
		"none":           nil,
	} {
		if detectKey(other, nil) == same {
			t.Errorf("%s has the same key as one device", name)
		}
	}
}
