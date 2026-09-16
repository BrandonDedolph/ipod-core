package app

import (
	"errors"
	"fmt"
	"path/filepath"
	"reflect"
	"runtime"
	"testing"

	"gioui.org/layout"
	"gioui.org/widget"
)

// The frame budget.
//
// This file is the guard for the bug that made the Library tab unusable:
// widget.List's zero value scrolls HORIZONTALLY, so the three lists this
// tab added (the grid, the problem column and the preview) laid their
// children out along an unbounded X axis — layout.Inf, which is 1e6 px.
// Nothing complained: the ops list stayed small and every dimension
// assertion still passed, because the panes themselves reported the size
// they were given. It only became visible one layer down, where Gio turns
// a widget.Border's rounded rect into a stroked path: a border a million
// pixels wide flattens into tens of millions of segments, and the frame
// asked the allocator for gigabytes. The kernel killed the test binary at
// 40 GB, and core-app.exe reached 30 GB on Windows.
//
// So the assertion cannot be "the layout is the right size" — that was
// true the whole time. It has to be about what the frame COSTS. A frame
// of this window is a few hundred rectangles and some text; 256 MiB is
// two orders of magnitude of headroom over what it really takes and
// still three orders of magnitude below the failure.
const (
	maxFrameAlloc  = 256 << 20 // a whole headless frame, GPU included
	maxLayoutAlloc = 64 << 20  // recording the ops, which is pure Go
)

// everyState is every canned state the app can be screenshotted in. The
// screenshot flag takes these names, so this list and `--state` cover the
// same ground.
func everyState() []struct {
	name string
	st   State
} {
	return []struct {
		name string
		st   State
	}{
		{"demo", DemoState()},
		{"library", LibraryState()},
		{"details", detailsState()},
		{"empty", EmptyState()},
		{"looking", LookingState()},
		{"notinstalled", NotInstalledState()},
	}
}

// frameSizes are the two windows every snapshot is taken at: the
// comfortable default and the enforced minimum.
var frameSizes = [][2]int{{900, 600}, {MinWidth, MinHeight}}

// allocated runs f and reports how many bytes it asked the allocator for.
// TotalAlloc only ever goes up, so the difference is the work f did and
// not whatever the GC happened to reclaim underneath it.
func allocated(f func()) uint64 {
	var before, after runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&before)
	f()
	runtime.ReadMemStats(&after)
	return after.TotalAlloc - before.TotalAlloc
}

// TestLayoutAllocationIsBounded is the half that runs on every machine,
// GPU or not: recording a frame's ops is pure Go.
//
// It would not by itself have caught the Library tab (the unbounded axis
// made a small ops list describing an enormous shape), but it is the
// cheap half of the same question, and it does catch the other way this
// goes wrong: a pane that appends widgets per frame.
func TestLayoutAllocationIsBounded(t *testing.T) {
	for _, c := range everyState() {
		for _, sz := range frameSizes {
			ui := NewUI(Options{})
			ui.SetState(c.st)
			// One warm-up frame: the first one fills the shaper's
			// caches, which is a cost paid once and not per frame.
			ui.Layout(newTestContext(sz[0], sz[1]))
			n := allocated(func() {
				ui.Layout(newTestContext(sz[0], sz[1]))
			})
			t.Logf("%s at %dx%d: %s per layout", c.name, sz[0], sz[1], bytesHuman(n))
			if n > maxLayoutAlloc {
				t.Errorf("laying %s out at %dx%d allocated %s, over the %s budget",
					c.name, sz[0], sz[1], bytesHuman(n), bytesHuman(maxLayoutAlloc))
			}
		}
	}
}

// TestFrameAllocationIsBounded is the whole frame: layout, then Gio's
// GPU pass, which is where the Library tab's million-pixel border was
// turned into a path and where the memory actually went.
//
// It renders through the same Snapshot the screenshot flag uses, so a
// state that cannot be rendered under the budget fails here rather than
// on somebody's desktop.
func TestFrameAllocationIsBounded(t *testing.T) {
	dir := t.TempDir()
	for _, c := range everyState() {
		for _, sz := range frameSizes {
			out := filepath.Join(dir, fmt.Sprintf("%s-%dx%d.png", c.name, sz[0], sz[1]))
			var err error
			n := allocated(func() {
				err = Snapshot(c.st, sz[0], sz[1], out)
			})
			if errors.Is(err, ErrNoHeadless) || errors.Is(err, ErrNoGPU) {
				t.Skipf("no headless GPU surface on this machine: %v", err)
			}
			if err != nil {
				t.Fatalf("Snapshot(%s at %dx%d): %v", c.name, sz[0], sz[1], err)
			}
			t.Logf("%s at %dx%d: %s per frame", c.name, sz[0], sz[1], bytesHuman(n))
			if n > maxFrameAlloc {
				t.Errorf("rendering %s at %dx%d allocated %s, over the %s budget",
					c.name, sz[0], sz[1], bytesHuman(n), bytesHuman(maxFrameAlloc))
			}
		}
	}
}

// TestEveryListScrollsVertically is the direct guard on the cause.
//
// widget.List's zero value is a HORIZONTAL list, and a horizontal list
// hands its children layout.Inf on the X axis. Every list in this window
// is a column, so every one of them has to say so — and the way this bug
// arrived was three new fields added to the UI struct and two of the
// three Axis lines forgotten in NewUI. Reflection asks the question of
// every field there will ever be, including the ones not written yet.
func TestEveryListScrollsVertically(t *testing.T) {
	u := NewUI(Options{})
	v := reflect.ValueOf(u).Elem()
	want := reflect.TypeOf(widget.List{})
	found := 0
	for i := 0; i < v.NumField(); i++ {
		if v.Field(i).Type() != want {
			continue
		}
		found++
		name := v.Type().Field(i).Name
		axis := layout.Axis(v.Field(i).FieldByName("Axis").Uint())
		if axis != layout.Vertical {
			t.Errorf("UI.%s is a %v list: widget.List's zero value scrolls "+
				"horizontally, which gives its children an unbounded width "+
				"(layout.Inf) — set its Axis in NewUI", name, axis)
		}
	}
	if found == 0 {
		t.Fatal("no widget.List fields found on UI — this guard has stopped guarding anything")
	}
	t.Logf("%d scrolling panes checked", found)
}

func bytesHuman(n uint64) string {
	switch {
	case n >= 1<<30:
		return fmt.Sprintf("%.1f GiB", float64(n)/(1<<30))
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MiB", float64(n)/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.1f KiB", float64(n)/(1<<10))
	}
	return fmt.Sprintf("%d B", n)
}

// TestLayoutScalesToAThousandAlbums is the grid at the size of a big
// library: laying it out must cost about what laying out the demo costs,
// because the list only lays out the rows that are on screen. A grid that
// laid out every tile would be a thousand tiles per frame — and, worse, a
// thousand thumbnail loads per frame.
func TestLayoutScalesToAThousandAlbums(t *testing.T) {
	st := DemoState()
	st.Albums = st.Albums[:0]
	for i := 0; i < 1000; i++ {
		st.Albums = append(st.Albums, Album{
			Dir:    fmt.Sprintf("C:\\Music\\Album %04d - Artist", i),
			Device: fmt.Sprintf("Artist - Album %04d", i),
			Title:  fmt.Sprintf("Artist - Album %04d", i),
			Tracks: 12, Copy: i % 3, State: TileState(i % 3),
		})
	}
	for _, sz := range frameSizes {
		ui := NewUI(Options{})
		ui.SetState(st)
		ui.Layout(newTestContext(sz[0], sz[1]))
		n := allocated(func() { ui.Layout(newTestContext(sz[0], sz[1])) })
		t.Logf("1000 albums at %dx%d: %s per layout, %d thumbnail loads started",
			sz[0], sz[1], bytesHuman(n), ui.thumbs.Loads())
		if n > maxLayoutAlloc {
			t.Errorf("1000 albums at %dx%d allocated %s per layout, over the %s budget",
				sz[0], sz[1], bytesHuman(n), bytesHuman(maxLayoutAlloc))
		}
		if loads := ui.thumbs.Loads(); loads > 200 {
			t.Errorf("1000 albums at %dx%d started %d thumbnail loads in one frame; only the visible rows should", sz[0], sz[1], loads)
		}
	}
}
