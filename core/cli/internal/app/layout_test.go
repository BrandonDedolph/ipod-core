package app

import (
	"strings"
	"testing"

	"image"

	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/unit"
)

// newTestContext is a layout context with no GPU behind it. Gio records
// drawing into an op.Ops list; nothing is rasterised until a frame is
// submitted, so every layout function in this package can be exercised
// on a machine with no display and no Vulkan headers.
func newTestContext(w, h int) layout.Context {
	return layout.Context{
		Ops:         new(op.Ops),
		Metric:      unit.Metric{PxPerDp: 1, PxPerSp: 1},
		Constraints: layout.Exact(image.Pt(w, h)),
	}
}

// TestLayoutAtTheMinimumSize is the regression guard for the thing that
// actually went wrong while this was being built: at 720x520 a single
// scrolling column put the Firmware and Eject cards below the fold, and
// the Firmware card's button row was clipped in half. The fix was two
// columns and shorter strings; this asserts the frame still reports the
// size it was given, in every state, at both sizes.
func TestLayoutAtEverySizeAndState(t *testing.T) {
	states := map[string]State{"demo": DemoState(), "empty": EmptyState()}
	sizes := [][2]int{{MinWidth, MinHeight}, {900, 600}, {1400, 900}}
	for name, st := range states {
		for _, sz := range sizes {
			ui := NewUI(Options{})
			ui.SetState(st)
			gtx := newTestContext(sz[0], sz[1])
			got := ui.Layout(gtx).Size
			if got.X != sz[0] || got.Y != sz[1] {
				t.Errorf("%s at %dx%d laid out %v", name, sz[0], sz[1], got)
			}
		}
	}
}

// The dialog is a modal over the page, so it must also report the full
// window size and must not panic on a body of a thousand lines.
func TestLayoutWithADialogOpen(t *testing.T) {
	ui := NewUI(Options{})
	ui.SetState(DemoState())
	ui.dlg = &dialogRequest{
		title:   "Write this image to the iPod?",
		body:    strings.Repeat("plan line\n", 1000),
		prompt:  `type the device path (\\.\PhysicalDrive2) to confirm:`,
		want:    `\\.\PhysicalDrive2`,
		okLabel: "Write it",
		reply:   make(chan string, 1),
	}
	got := ui.Layout(newTestContext(MinWidth, MinHeight)).Size
	if got.X != MinWidth || got.Y != MinHeight {
		t.Errorf("a frame with the dialog open laid out %v", got)
	}
}

// SetState has to push the model's two path strings into the Editor
// widgets, or a snapshot of the demo state renders with empty boxes.
func TestSetStateFillsTheEditors(t *testing.T) {
	ui := NewUI(Options{})
	st := DemoState()
	ui.SetState(st)
	if ui.sourceEd.Text() != st.Source {
		t.Errorf("the source field holds %q, want %q", ui.sourceEd.Text(), st.Source)
	}
	if ui.flashEd.Text() != st.FlashFile {
		t.Errorf("the image field holds %q, want %q", ui.flashEd.Text(), st.FlashFile)
	}
}

// The Cancel button says out loud that the two write jobs cannot be
// stopped between the body write and the directory write.
func TestCancelLabelForAWriteJob(t *testing.T) {
	ui := NewUI(Options{})
	ui.st.Job = &JobStatus{Kind: JobFlash}
	if got := ui.cancelLabel(); !strings.Contains(got, "Cannot cancel") {
		t.Errorf("cancelLabel() during a flash = %q", got)
	}
	ui.st.Job = &JobStatus{Kind: JobSync}
	if got := ui.cancelLabel(); got != "Cancel" {
		t.Errorf("cancelLabel() during a sync = %q", got)
	}
}

// The Firmware card must never offer a Flash it cannot honour: with no
// CLI beside the app it prints the command instead.
func TestFlashNoteWithoutTheCLI(t *testing.T) {
	ui := NewUI(Options{})
	ui.st.CLIPath = ""
	ui.flashEd.SetText("/img/core.ipod")
	note := ui.flashNote()
	if !strings.Contains(note, "not beside this app") || !strings.Contains(note, "flash /img/core.ipod") {
		t.Errorf("flashNote() = %q", note)
	}

	ui.st.CLIPath = "/opt/core"
	if note := ui.flashNote(); strings.Contains(note, "not beside this app") {
		t.Errorf("flashNote() still complains with a CLI present: %q", note)
	}
}
