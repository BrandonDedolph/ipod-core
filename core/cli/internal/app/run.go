//go:build !linux || novulkan

package app

import (
	"fmt"
	"os"

	gio "gioui.org/app"
	"gioui.org/op"
	"gioui.org/unit"
)

// Main opens the window and runs until it is closed. It never returns
// on a healthy run: Gio's event loop has to own the main goroutine, so
// the window loop lives on another one and exits the process itself.
//
// The import alias matters — this package is called app and so is
// Gio's. `gio` is the toolkit; `app` is us.
func Main(o Options) error {
	go func() {
		err := runWindow(o)
		if err != nil {
			logf(o, "core-app: %v", err)
			fmt.Fprintln(os.Stderr, "core-app:", err)
			os.Exit(1)
		}
		os.Exit(0)
	}()
	gio.Main()
	return nil
}

func runWindow(o Options) error {
	w := new(gio.Window)
	w.Option(
		gio.Title("Core"),
		gio.Size(unit.Dp(980), unit.Dp(700)),
		gio.MinSize(unit.Dp(MinWidth), unit.Dp(MinHeight)),
	)
	o.Invalidate = w.Invalidate
	u := NewUI(o)
	logf(o, "window opened")

	// The pump. Job goroutines write to the Runner's channel; this
	// moves each event onto the UI goroutine's queue and asks for a
	// frame. Doing it here rather than draining the channel inside
	// Layout is what makes a job that finishes while the window is idle
	// still repaint: an idle Gio window produces no frames at all.
	go func() {
		for e := range u.Runner().Events() {
			u.Post(e)
		}
	}()

	var ops op.Ops
	for {
		switch e := w.Event().(type) {
		case gio.DestroyEvent:
			return e.Err
		case gio.FrameEvent:
			gtx := gio.NewContext(&ops, e)
			u.Layout(gtx)
			e.Frame(gtx.Ops)
		}
	}
}
