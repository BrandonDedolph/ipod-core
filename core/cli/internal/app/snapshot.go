//go:build !linux || novulkan

package app

import (
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"

	"gioui.org/gpu/headless"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/unit"
)

// Snapshot renders one frame of the given State at w×h into a PNG and
// returns.
//
// This is the review channel. The orchestrator building this app cannot
// see the user's desktop, and neither can CI; a window that has never
// been looked at is a window whose text is clipped in a way nobody
// notices until it ships. `core-app --screenshot out.png --state demo`
// is the same call, so the picture a human reviews and the picture the
// test asserts on come out of the same code path.
//
// It renders through gioui.org/gpu/headless, which on this WSL box goes
// to Mesa's software EGL (a DRI3 warning on stderr is normal). Where
// there is no EGL at all, headless.NewWindow fails and the caller
// (TestSnapshot) skips rather than fails: a machine with no GPU stack
// is not a broken layout.
func Snapshot(st State, w, h int, out string) error {
	return snapshotWith(st, w, h, out, nil)
}

// snapshotWith is Snapshot with a hook that runs after the State is set
// and before the frame is laid out, for the test that needs a modal on
// screen.
func snapshotWith(st State, w, h int, out string, tweak func(*UI)) error {
	hw, err := headless.NewWindow(w, h)
	if err != nil {
		return fmt.Errorf("%w: %v", ErrNoHeadless, err)
	}
	defer hw.Release()

	ui := NewUI(Options{})
	ui.SetState(st)
	if tweak != nil {
		tweak(ui)
	}

	var ops op.Ops
	sz := image.Pt(w, h)
	gtx := layout.Context{
		Ops:         &ops,
		Metric:      unit.Metric{PxPerDp: 1, PxPerSp: 1},
		Constraints: layout.Exact(sz),
	}
	ui.Layout(gtx)
	if err := hw.Frame(&ops); err != nil {
		return fmt.Errorf("rendering the frame: %w", err)
	}
	img := image.NewRGBA(image.Rectangle{Max: sz})
	if err := hw.Screenshot(img); err != nil {
		return fmt.Errorf("reading the frame back: %w", err)
	}
	if dir := filepath.Dir(out); dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}
	f, err := os.Create(out)
	if err != nil {
		return err
	}
	if err := png.Encode(f, img); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}
