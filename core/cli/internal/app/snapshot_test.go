package app

import (
	"errors"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

// snapshotDir is where the PNGs land: $CORE_SNAPSHOT_DIR, or a stable
// place under the system temp directory. Stable and not t.TempDir(),
// because the point of these files is that a human (or an agent with an
// image reader) looks at them after the test run.
func snapshotDir(t *testing.T) string {
	t.Helper()
	if d := os.Getenv("CORE_SNAPSHOT_DIR"); d != "" {
		return d
	}
	return filepath.Join(os.TempDir(), "core-app-snapshots")
}

// TestSnapshot renders the main screen at both sizes that matter: the
// window's minimum, where the four cards and the log pane are closest
// to colliding, and a comfortable default.
//
// What it asserts is deliberately shallow — that a frame renders at all
// and that the PNG is the size asked for. A pixel-golden would fail on
// every font or Gio update and teach everyone to regenerate it without
// looking. The real review is a human opening the two files, which is
// why they are written somewhere that survives the run.
func TestSnapshot(t *testing.T) {
	dir := snapshotDir(t)
	sizes := []struct {
		name string
		w, h int
	}{
		{"main-900x600", 900, 600},
		{"main-720x520", MinWidth, MinHeight},
	}
	for _, s := range sizes {
		out := filepath.Join(dir, s.name+".png")
		err := Snapshot(DemoState(), s.w, s.h, out)
		if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
			t.Skipf("no headless GPU surface on this machine: %v", err)
		}
		if err != nil {
			t.Fatalf("Snapshot(%s): %v", s.name, err)
		}
		checkPNG(t, out, s.w, s.h)
		t.Logf("wrote %s", out)
	}
}

// TestSnapshotEmptyState renders the first-run screen: no device, no
// source, every button that needs one disabled. It is the state a user
// sees first and the one easiest to leave untested.
func TestSnapshotEmptyState(t *testing.T) {
	out := filepath.Join(snapshotDir(t), "empty-720x520.png")
	err := Snapshot(EmptyState(), MinWidth, MinHeight, out)
	if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
		t.Skipf("no headless GPU surface on this machine: %v", err)
	}
	if err != nil {
		t.Fatalf("Snapshot: %v", err)
	}
	checkPNG(t, out, MinWidth, MinHeight)
	t.Logf("wrote %s", out)
}

// TestSnapshotFlashDialog renders the modal that guards the one write
// this project makes. It is the screen with the most ways to go wrong —
// a long plan, a scrollable body, a typed confirmation and two buttons
// in a box that must not overflow the window it is centred in.
func TestSnapshotFlashDialog(t *testing.T) {
	out := filepath.Join(snapshotDir(t), "flash-dialog-900x600.png")
	err := snapshotDialog(DemoState(), 900, 600, out)
	if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
		t.Skipf("no headless GPU surface on this machine: %v", err)
	}
	if err != nil {
		t.Fatalf("snapshotDialog: %v", err)
	}
	checkPNG(t, out, 900, 600)
	t.Logf("wrote %s", out)
}

func checkPNG(t *testing.T, path string, w, h int) {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatalf("the snapshot was not written: %v", err)
	}
	defer f.Close()
	cfg, err := png.DecodeConfig(f)
	if err != nil {
		t.Fatalf("%s is not a PNG: %v", path, err)
	}
	if cfg.Width != w || cfg.Height != h {
		t.Errorf("%s is %dx%d, want %dx%d", path, cfg.Width, cfg.Height, w, h)
	}
}

// TestSnapshotLayoutIsPure checks the property the snapshot renderer
// depends on: laying the same State out twice produces the same
// dimensions. A Layout that changed size between frames would make the
// window jitter and the screenshot unrepeatable.
func TestSnapshotLayoutIsPure(t *testing.T) {
	first := layoutSize(t, DemoState(), MinWidth, MinHeight)
	second := layoutSize(t, DemoState(), MinWidth, MinHeight)
	if first != second {
		t.Errorf("two layouts of the same state gave %v and %v", first, second)
	}
}

// layoutSize runs Layout with no GPU at all — op recording is pure Go —
// so this half of the check runs on every machine, tags or not.
func layoutSize(t *testing.T, st State, w, h int) image.Point {
	t.Helper()
	ui := NewUI(Options{})
	ui.SetState(st)
	return ui.Layout(newTestContext(w, h)).Size
}
