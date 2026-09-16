package app

import (
	"context"
	"fmt"
	"image"
	"image/color"
	"io"
	"os"
	"runtime"
	"strings"
	"sync"
	"time"

	"gioui.org/io/clipboard"
	"gioui.org/io/key"
	"gioui.org/layout"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/version"
)

// Shorthands. Gio code is 90 % these two types and the full names turn
// every layout function signature into two lines.
type (
	// C is layout.Context.
	C = layout.Context
	// D is layout.Dimensions.
	D = layout.Dimensions
)

// MinWidth and MinHeight are the window's minimum size. Below this the
// four cards and the log pane stop being readable at the same time,
// which is the whole layout.
const (
	MinWidth  = 720
	MinHeight = 520
)

// Options configure a UI.
type Options struct {
	// Backend is the wiring. Required.
	Backend Backend
	// Config is the loaded settings; ConfigPath is where changes go
	// back. An empty ConfigPath means "do not persist", which is what
	// the snapshot renderer and the tests want.
	Config     Config
	ConfigPath string
	// Source overrides Config.Source (the --source flag).
	Source string
	// Invalidate asks the window for a frame. nil in a headless
	// render, where there is exactly one frame and nothing to ask.
	Invalidate func()
	// NoDetect turns the 2-second poll off (--no-detect). The window
	// then only knows what a job told it, which is what a test or a
	// screenshot wants and nothing else does.
	NoDetect bool
	// LogFile is --log: a file the startup path appends to. A
	// `-H windowsgui` process has no console, so a window that fails to
	// open fails in complete silence; this is the only way to see why
	// from a shell.
	LogFile string
}

// logf appends one line to Options.LogFile, if there is one.
func logf(o Options, format string, args ...any) {
	if o.LogFile == "" {
		return
	}
	f, err := os.OpenFile(o.LogFile, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, format+"\n", args...)
}

// dialogRequest is one modal question. It crosses a goroutine boundary:
// a job asks, the UI goroutine draws the dialog, and the answer goes
// back down reply. The job blocks in between, which is correct — a
// flash that proceeded while the confirmation was still on screen is
// the bug this whole mechanism exists to prevent.
type dialogRequest struct {
	title string
	// body is shown in a scrollable monospace block: the flasher's
	// plan, or the list of things a prune would delete.
	body string
	// prompt is the line above the input.
	prompt string
	// want is the exact text that must be typed. Empty means the
	// dialog is a plain Yes/Cancel.
	want string
	// okLabel is the confirming button's text.
	okLabel string
	// reply carries the typed answer, or "" for a cancel. Buffered, so
	// a job that has already given up cannot wedge the UI goroutine.
	reply chan string
}

// renameResult is what a finished rename job leaves for the next frame:
// the friendly name that was asked for, the label the volume actually
// took (read back), and the serial to file the name under.
type renameResult struct{ friendly, label, serial string }

// UI is the window's state: the model, the widgets, and the queues the
// job goroutines talk to it through.
type UI struct {
	th  *material.Theme
	pal Palette
	st  State
	run *Runner
	be  Backend

	cfg        Config
	cfgPath    string
	invalidate func()

	// Everything below the mutex is written by job goroutines and read
	// (and cleared) by the UI goroutine at the top of a frame. State
	// itself is never in here — that is the rule.
	mu        sync.Mutex
	inbox     []Event
	askQueue  *dialogRequest
	newDev    *Device
	newLib    *Library
	newRel    *Release
	newPath   string // a path chosen in the folder/file picker
	pathFor   JobKind
	newName   *renameResult // what a finished rename settled on
	newDetect *detectNews   // what the last poll found (detect.go)
	// The library manager's four results, plus the plan the grid is
	// drawn from.
	newReport  *librarian.Report
	newCands   map[librarian.AlbumKey][]artfetch.Candidate
	newFix     *librarian.Result
	newUndo    *organizer.UndoReport
	newPlan    *syncer.Plan
	planSynced bool

	dlg *dialogRequest // the dialog on screen; UI goroutine only

	// pending is the one-job-at-a-time queue: things to start on a
	// frame where nothing is running (see later()). UI goroutine only.
	pending []func()

	// det is the auto-detect poll's memory of the last set of devices
	// it saw. It has its own mutex; see detect.go.
	det detector

	cards   widget.List
	logList widget.List
	dlgBody widget.List
	// grid, libList and prevList are the three scrolling panes the tabs
	// add: the album grid, the Library tab's problem column and its
	// preview.
	grid     widget.List
	libList  widget.List
	prevList widget.List

	// thumbs is the album grid's cover cache; artThumbs is the same
	// machinery keyed by URL for the art candidates in the Library tab.
	thumbs    *thumbCache
	artThumbs *thumbCache

	sourceEd  widget.Editor
	flashEd   widget.Editor
	confirmEd widget.Editor
	nameEd    widget.Editor

	// editingName is the header's click-to-edit state: the name line
	// is a label until it is clicked, then a field with Save/Cancel.
	// It is UI-goroutine-only, like everything else outside the mutex.
	editingName bool

	browseBtn                                   widget.Clickable
	dryRunBtn, syncBtn, pruneBtn, cancelBtn     widget.Clickable
	checkBtn, updateBtn, flashBtn, flashOpenBtn widget.Clickable
	backupBtn, ejectBtn, copyBtn                widget.Clickable
	nameClick, nameSaveBtn, nameCancelBtn       widget.Clickable
	installBtn, installFileBtn                  widget.Clickable
	dlgOK, dlgCancel                            widget.Clickable

	// The top bar's one primary button, the tabs, and the footer's
	// "Change" for the music folder.
	primaryBtn   widget.Clickable
	tabClicks    [3]widget.Clickable
	changeBtn    widget.Clickable
	tileClicks   []*widget.Clickable
	catClicks    [5]widget.Clickable
	artTake      []*widget.Clickable
	artSkip      []*widget.Clickable
	fixAllBtn    widget.Clickable
	undoBtn      widget.Clickable
	rescanBtn    widget.Clickable
	artFindBtn   widget.Clickable
	namesTick    widget.Bool
	organizeTick widget.Bool
	discTick     widget.Bool

	pickerOK bool
}

// NewUI builds the window's state.
func NewUI(o Options) *UI {
	th, pal := newTheme()
	u := &UI{
		th:         th,
		pal:        pal,
		run:        NewRunner(),
		be:         o.Backend,
		cfg:        o.Config,
		cfgPath:    o.ConfigPath,
		invalidate: o.Invalidate,
		pickerOK:   PickerAvailable(),
	}
	if u.invalidate == nil {
		u.invalidate = func() {}
	}
	u.cards.Axis = layout.Vertical
	u.grid.Axis = layout.Vertical
	u.libList.Axis = layout.Vertical
	u.prevList.Axis = layout.Vertical
	u.dlgBody.Axis = layout.Vertical
	u.logList.Axis = layout.Vertical
	// The log is a transcript: the interesting end is the bottom, and a
	// pane that stays at the top while a sync scrolls past is a pane
	// nobody reads.
	u.logList.ScrollToEnd = true

	// The two picture caches. The album grid decodes the device's own
	// sidecars off the UI goroutine; the candidate row fetches 200 px
	// thumbnails through the Backend, so a test sees a fake and no
	// network.
	u.thumbs = newThumbCache(func(dir string) (image.Image, error) {
		if u.be == nil {
			return nil, ErrNoThumb
		}
		return u.be.Thumbnail(dir)
	}, u.invalidate)
	u.artThumbs = newThumbCache(func(url string) (image.Image, error) {
		if u.be == nil {
			return nil, ErrNoThumb
		}
		return u.be.FetchThumb(context.Background(), url)
	}, u.invalidate)

	// The two tick boxes the report's categories carry start ON: the
	// report only lists things that are wrong, and a Fix all that did
	// nothing because two boxes started empty is worse than one that
	// does what the list says. The multi-disc split starts OFF — it
	// changes the shape of an album rather than repairing it.
	u.st.FixNames, u.st.Organize = true, true
	u.namesTick.Value, u.organizeTick.Value = true, true

	u.sourceEd.SingleLine, u.sourceEd.Submit = true, true
	u.flashEd.SingleLine, u.flashEd.Submit = true, true
	u.confirmEd.SingleLine, u.confirmEd.Submit = true, true
	u.nameEd.SingleLine, u.nameEd.Submit = true, true

	src := o.Source
	if src == "" {
		src = o.Config.Source
	}
	u.st.Source = src
	u.sourceEd.SetText(src)
	u.st.CLIPath, _ = CLIPath()
	u.st.Logf("core-app %s", version.Version)
	if u.st.CLIPath != "" {
		u.st.Logf("core CLI: %s", u.st.CLIPath)
	} else {
		u.st.Logf("core CLI: not found beside this app — Flash and Update will print the command instead")
	}
	return u
}

// Runner exposes the job runner, for the window's pump goroutine.
func (u *UI) Runner() *Runner { return u.run }

// Post hands one event to the UI goroutine and asks for a frame. The
// window's pump calls it; so does anything else off the UI goroutine
// that has something to say.
func (u *UI) Post(e Event) {
	u.mu.Lock()
	u.inbox = append(u.inbox, e)
	u.mu.Unlock()
	u.invalidate()
}

// State returns a copy of the model, for the tests and the snapshot
// renderer.
func (u *UI) State() State { return u.st }

// SetState replaces the model wholesale. Only the canned states
// (Demo/Empty) and the tests use it.
//
// The two text fields are part of the model as far as a reader is
// concerned, but Gio keeps their contents inside the Editor widgets, so
// they have to be pushed across explicitly — otherwise a snapshot of
// the demo state renders with empty boxes and their hint text, which is
// exactly the screenshot that would make someone think the field does
// not work.
func (u *UI) SetState(st State) {
	u.st = st
	u.sourceEd.SetText(st.Source)
	u.flashEd.SetText(st.FlashFile)
	u.nameEd.SetText(st.Device.Name)
	u.namesTick.Value = st.FixNames
	u.organizeTick.Value = st.Organize
	u.discTick.Value = st.DiscFolders
}

// --- the per-frame work ------------------------------------------------

// drain folds everything the job goroutines left behind into the model.
// It is the first thing a frame does, and the only place State changes
// from outside a click handler.
func (u *UI) drain() {
	u.mu.Lock()
	events := u.inbox
	u.inbox = nil
	dev, lib, rel := u.newDev, u.newLib, u.newRel
	u.newDev, u.newLib, u.newRel = nil, nil, nil
	path, pathFor := u.newPath, u.pathFor
	u.newPath, u.pathFor = "", JobNone
	renamed := u.newName
	u.newName = nil
	detected := u.newDetect
	u.newDetect = nil
	report, cands := u.newReport, u.newCands
	u.newReport, u.newCands = nil, nil
	fixed, undone := u.newFix, u.newUndo
	u.newFix, u.newUndo = nil, nil
	plan, planSynced := u.newPlan, u.planSynced
	u.newPlan, u.planSynced = nil, false
	ask := u.askQueue
	u.askQueue = nil
	u.mu.Unlock()

	for _, e := range events {
		u.st.Apply(e)
		u.afterJob(e)
	}
	if dev != nil {
		u.st.Device = *dev
		// The label came off the volume; the friendly name is ours,
		// and this is the only place the two are joined.
		u.st.Device.Name = u.cfg.NameFor(dev.Serial)
		if !u.editingName {
			u.nameEd.SetText(u.st.Device.Name)
		}
	}
	if renamed != nil {
		u.st.Device.Name, u.st.Device.Label = renamed.friendly, renamed.label
		u.cfg.SetName(renamed.serial, renamed.friendly)
		u.saveConfig()
		u.nameEd.SetText(renamed.friendly)
	}
	if lib != nil {
		u.st.Library = *lib
	}
	if rel != nil {
		u.st.Release = *rel
	}
	switch {
	case path != "" && pathFor == JobSync:
		u.setSource(path)
	case path != "" && pathFor == JobFlash:
		u.st.FlashFile = path
		u.flashEd.SetText(path)
	case path != "" && pathFor == JobInstall:
		// "Install from a file…" is one gesture: the file dialog IS the
		// decision, and a second click on a button that appeared where
		// the first one was is a click nobody expects to need.
		u.st.FlashFile = path
		u.flashEd.SetText(path)
		u.startInstall(path)
	}
	if report != nil {
		u.st.Report, u.st.Scanned = report, time.Now()
		// A rescan of the same tree (Rescan, or ticking the multi-disc
		// row) keeps the lookup and the Take/Skip decisions for every
		// album that is still coverless: the keys are the album folders,
		// and asking the internet and the person again for an answer they
		// already gave is the kind of thing that makes a rescan feel like
		// a punishment. A key that is gone (a cover arrived, the album
		// moved) is dropped with its decision.
		u.st.ArtCands, u.st.ArtChoices = keepArt(report, u.st.ArtCands, u.st.ArtChoices)
		if u.st.Category.Count(report) == 0 {
			u.st.Category = firstCategory(report)
		}
	}
	if cands != nil {
		u.st.ArtCands = cands
		// "Accept all at 0.9 or better" is the rule the CLI's --yes
		// uses and the one the design page names; applying it here
		// means the ordinary case needs no clicks at all, and every
		// row still says what it is taking and offers Skip.
		for key, dec := range librarian.AcceptAbove(cands, librarian.DefaultMinScore) {
			if dec.Skip {
				// AcceptAbove marks everything below the line as a skip.
				// A weak match is not a skip, it is a question: leave it
				// undecided so the row asks it.
				continue
			}
			u.setChoice(key, dec)
		}
		u.st.Logf("cover art: %d album(s) matched, %d accepted at %.2f or better",
			len(cands), len(u.st.ArtChoices), librarian.DefaultMinScore)
	}
	if fixed != nil {
		u.st.Journal = fixed.Journal
		if fixed.Renamed+fixed.Moved+fixed.ArtWritten > 0 {
			u.st.LibraryChanged = true
		}
		// The report describes a tree that no longer exists: scan
		// again, then re-plan the sync the renames just made bigger.
		src := u.st.Source
		u.later(func() { u.startInspect(src) })
		u.later(func() { u.startSync(JobDryRun) })
	}
	if undone != nil {
		u.st.Journal = ""
		u.st.LibraryChanged = false
		src := u.st.Source
		u.later(func() { u.startInspect(src) })
		u.later(func() { u.startSync(JobDryRun) })
	}
	if plan != nil {
		u.st.Albums = AlbumsFromPlan(plan)
		if planSynced {
			markSynced(u.st.Albums)
		}
	}
	if detected != nil {
		u.applyDetect(detected)
	}
	if ask != nil {
		u.dlg = ask
		u.confirmEd.SetText("")
	}
	u.runPending()
}

// afterJob is the one place a finished job schedules the next read-only
// pass. Nothing here writes anything: a refresh that found a volume is
// what fills the grid and the Library tab on launch, with no click.
func (u *UI) afterJob(e Event) {
	if e.Kind != EventDone {
		return
	}
	switch e.Job {
	case JobRefresh:
		if u.st.Source == "" {
			return
		}
		if u.st.Report == nil {
			src := u.st.Source
			u.later(func() { u.startInspect(src) })
		}
		if len(u.st.Albums) == 0 && u.st.Device.Volume != "" {
			u.later(func() { u.startSync(JobDryRun) })
		}
	}
}

// runPending starts the next queued job, if the Runner is free. One per
// frame: the next one goes on the frame after that job's EventDone, which
// is also when the model is in a fit state to decide whether it is still
// wanted.
func (u *UI) runPending() {
	if len(u.pending) == 0 || u.st.Busy() || u.run.Busy() {
		return
	}
	f := u.pending[0]
	u.pending = u.pending[1:]
	f()
}

// firstCategory is the row the Library tab opens on: the first one with
// anything in it, so a report lands on something to look at.
func firstCategory(r *librarian.Report) Category {
	for _, c := range Categories {
		if c.Count(r) > 0 {
			return c
		}
	}
	return CatArt
}

// markSynced turns a plan that has just run into the grid it leaves
// behind: everything it was going to copy is on the iPod now.
func markSynced(albums []Album) {
	for i := range albums {
		albums[i].Copy = 0
		albums[i].Bytes = 0
		albums[i].State = TileOnDevice
	}
}

func (u *UI) saveConfig() {
	if u.cfgPath == "" {
		return
	}
	u.cfg.Source = u.st.Source
	if err := SaveConfig(u.cfgPath, u.cfg); err != nil {
		u.st.Logf("warning: could not save %s: %v", u.cfgPath, err)
	}
}

// Layout draws one frame. It is pure with respect to the outside world:
// everything it does is read the model, read widget events, and start
// jobs through the Runner.
func (u *UI) Layout(gtx C) D {
	u.drain()
	if u.dlg != nil {
		u.dialogEvents(gtx)
	} else {
		u.events(gtx)
	}

	paint.Fill(gtx.Ops, u.pal.Surface)
	return layout.Stack{}.Layout(gtx,
		layout.Stacked(func(gtx C) D {
			g := gtx
			if u.dlg != nil {
				// A modal that does not actually block the buttons
				// behind it is a modal that lets somebody start a sync
				// while a flash confirmation is on screen.
				g = gtx.Disabled()
			}
			g.Constraints.Min = gtx.Constraints.Max
			return u.page(g)
		}),
		layout.Expanded(func(gtx C) D {
			if u.dlg == nil {
				return D{}
			}
			return u.dialog(gtx)
		}),
	)
}

// events turns this frame's clicks into jobs.
func (u *UI) events(gtx C) {
	busy := u.st.Busy()

	// The tabs answer whether a job is running or not: looking at the
	// log while a sync runs is exactly what the Details tab is for.
	for i, t := range []Tab{TabAlbums, TabLibrary, TabDetails} {
		if u.tabClicks[i].Clicked(gtx) {
			u.st.Tab = t
		}
	}
	u.libraryEvents(gtx, busy)
	u.gridEvents(gtx, busy)

	if !busy {
		if u.primaryBtn.Clicked(gtx) {
			u.doPrimary()
		}
		if u.changeBtn.Clicked(gtx) {
			u.pick(JobSync)
		}
		if u.installBtn.Clicked(gtx) {
			u.startInstall("")
		}
		if u.installFileBtn.Clicked(gtx) {
			u.pick(JobInstall)
		}
		if u.browseBtn.Clicked(gtx) {
			u.pick(JobSync)
		}
		if u.flashOpenBtn.Clicked(gtx) {
			u.pick(JobFlash)
		}
		if u.dryRunBtn.Clicked(gtx) {
			u.startSync(JobDryRun)
		}
		if u.syncBtn.Clicked(gtx) {
			u.startSync(JobSync)
		}
		if u.pruneBtn.Clicked(gtx) {
			u.startSync(JobSyncPrune)
		}
		if u.checkBtn.Clicked(gtx) {
			u.startCheck()
		}
		if u.updateBtn.Clicked(gtx) {
			u.startUpdate()
		}
		if u.flashBtn.Clicked(gtx) {
			u.startFlash(strings.TrimSpace(u.flashEd.Text()))
		}
		if u.backupBtn.Clicked(gtx) {
			u.startBackup()
		}
		if u.ejectBtn.Clicked(gtx) {
			u.startEject()
		}
		if u.nameClick.Clicked(gtx) && u.st.Device.Found && !u.editingName {
			u.editingName = true
			u.nameEd.SetText(u.st.Device.Name)
			gtx.Execute(key.FocusCmd{Tag: &u.nameEd})
		}
	}
	u.nameEvents(gtx, busy)
	if u.st.CanCancel() && u.cancelBtn.Clicked(gtx) {
		u.st.Logf("cancel requested")
		u.run.Cancel()
	}
	if u.copyBtn.Clicked(gtx) {
		text := strings.Join(u.st.Log, "\n") + "\n"
		gtx.Execute(clipboard.WriteCmd{Type: "application/text", Data: io.NopCloser(strings.NewReader(text))})
		u.st.Logf("copied %d log lines to the clipboard", len(u.st.Log))
	}

	// The source field is saved as it is edited, not on some Save
	// button nobody would press.
	for {
		ev, ok := u.sourceEd.Update(gtx)
		if !ok {
			break
		}
		switch ev.(type) {
		case widget.ChangeEvent:
			if s := strings.TrimSpace(u.sourceEd.Text()); s != u.st.Source {
				u.st.Source = s
				u.saveConfig()
			}
		case widget.SubmitEvent:
			// Enter in the field is the gesture that means "this one":
			// the scan follows it, rather than following every
			// keystroke on the way to a path.
			u.setSource(strings.TrimSpace(u.sourceEd.Text()))
		}
	}
	for {
		ev, ok := u.flashEd.Update(gtx)
		if !ok {
			break
		}
		switch ev.(type) {
		case widget.ChangeEvent, widget.SubmitEvent:
			u.st.FlashFile = strings.TrimSpace(u.flashEd.Text())
		}
	}
}

// nameEvents is the click-to-edit field's half of the frame: Enter or
// Save applies, Cancel drops it, and a job running takes the edit away —
// a rename during a flash would be a rename of a dismounted volume.
//
// The editor is drained every frame, editing or not: a widget whose
// events nobody reads keeps them forever, and the Submit that arrived
// on the frame the job started would fire again on the frame it ended.
func (u *UI) nameEvents(gtx C, busy bool) {
	submitted := false
	for {
		ev, ok := u.nameEd.Update(gtx)
		if !ok {
			break
		}
		if _, isSubmit := ev.(widget.SubmitEvent); isSubmit {
			submitted = true
		}
	}
	if !u.editingName {
		return
	}
	if busy {
		u.editingName = false
		return
	}
	if u.nameCancelBtn.Clicked(gtx) {
		u.editingName = false
		u.nameEd.SetText(u.st.Device.Name)
		return
	}
	if u.nameSaveBtn.Clicked(gtx) || submitted {
		u.editingName = false
		u.startRename(strings.TrimSpace(u.nameEd.Text()))
	}
}

// startRename is the rename job. It lives here rather than in
// actions.go because the field that starts it does too, and because it
// is the one job whose result the UI has to write back into
// config.json.
func (u *UI) startRename(name string) {
	vol, serial := u.st.Device.Volume, u.st.Device.Serial
	if vol == "" {
		u.st.Logf("rename: no iPod volume yet — press Refresh")
		return
	}
	u.start(JobRename, func(ctx context.Context, emit func(Event)) error {
		label, err := u.be.Rename(ctx, vol, name)
		if err != nil {
			return err
		}
		u.mu.Lock()
		u.newName = &renameResult{friendly: name, label: label, serial: serial}
		u.mu.Unlock()
		if name == "" {
			emit(Event{Kind: EventLog, Text: vol + ": the name was cleared"})
		} else {
			emit(Event{Kind: EventLog, Text: fmt.Sprintf(
				"%s: named %q — Windows shows the volume as %s", vol, name, label)})
		}
		return nil
	})
}

func (u *UI) dialogEvents(gtx C) {
	for {
		_, ok := u.confirmEd.Update(gtx)
		if !ok {
			break
		}
	}
	if u.dlgCancel.Clicked(gtx) {
		u.answer("")
		return
	}
	if u.dlgOK.Clicked(gtx) {
		u.answer(strings.TrimSpace(u.confirmEd.Text()))
	}
}

func (u *UI) answer(text string) {
	if u.dlg == nil {
		return
	}
	if u.dlg.want == "" && text == "" {
		// A plain Yes/Cancel dialog: OK means yes, and the flasher's
		// Confirm contract is "return the typed device path", so the
		// literal answer is what the caller asked for.
		text = "yes"
	}
	select {
	case u.dlg.reply <- text:
	default:
	}
	u.dlg = nil
	u.confirmEd.SetText("")
}

// ask puts a modal question on screen and blocks the calling (job)
// goroutine until it is answered or the job is cancelled.
func (u *UI) ask(ctx context.Context, req *dialogRequest) (string, error) {
	req.reply = make(chan string, 1)
	if req.okLabel == "" {
		req.okLabel = "Confirm"
	}
	u.mu.Lock()
	u.askQueue = req
	u.mu.Unlock()
	u.invalidate()
	select {
	case s := <-req.reply:
		return s, nil
	case <-ctx.Done():
		return "", ctx.Err()
	}
}

// --- drawing -----------------------------------------------------------

func (u *UI) page(gtx C) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(u.topBar),
		layout.Rigid(u.progress),
		layout.Rigid(u.tabBar),
		layout.Flexed(1, func(gtx C) D {
			return layout.Inset{Left: unit.Dp(14), Right: unit.Dp(14),
				Top: unit.Dp(12), Bottom: unit.Dp(10)}.Layout(gtx, u.body)
		}),
		layout.Rigid(u.footer),
	)
}

// topBar is the mockup's one-line-each frame: who this iPod is, what the
// window is about to do, and the ONE primary button that does it.
//
// The button's label is the decision (State.Primary): Install Core on a stock
// iPod, Sync when the library and the device differ, Update when a newer
// release is known, Eject when there is nothing left. Eject is also offered
// beside Sync, because "sync, then unplug" is one session and the second half
// should not need another screen.
func (u *UI) topBar(gtx C) D {
	head, sub := u.topStatus()
	act := u.st.Primary()
	busy := u.st.Busy()
	return layout.Inset{Top: unit.Dp(10), Bottom: unit.Dp(10), Left: unit.Dp(14), Right: unit.Dp(14)}.
		Layout(gtx, func(gtx C) D {
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					gtx.Constraints.Max.X = gtx.Constraints.Max.X / 3
					return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
						layout.Rigid(u.line(16, u.pal.Ink, u.deviceHead())),
						layout.Rigid(u.line(11, u.pal.Muted, u.deviceSub())),
					)
				}),
				layout.Flexed(1, func(gtx C) D {
					return layout.Inset{Left: unit.Dp(16), Right: unit.Dp(16)}.Layout(gtx, func(gtx C) D {
						gtx.Constraints.Min.X = gtx.Constraints.Max.X
						return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
							layout.Rigid(u.line(13, u.pal.Ink, head)),
							layout.Rigid(u.line(11, u.pal.Muted, sub)),
						)
					})
				}),
				layout.Rigid(func(gtx C) D {
					if act == ActionNone {
						return D{}
					}
					return u.button(gtx, btn{&u.primaryBtn, act.Label(), true, !busy})
				}),
				layout.Rigid(func(gtx C) D {
					// The secondary Eject, only where the primary is
					// something else and there is a volume to let go of.
					if act == ActionEject || act == ActionNone || u.st.Device.Volume == "" {
						return D{}
					}
					return layout.Inset{Left: unit.Dp(6)}.Layout(gtx, func(gtx C) D {
						return u.button(gtx, btn{&u.ejectBtn, "Eject", false, !busy})
					})
				}),
			)
		})
}

// deviceHead is the iPod's name, or what the window is looking for.
func (u *UI) deviceHead() string {
	if !u.st.Device.Found {
		return "No iPod"
	}
	return u.st.Device.DisplayName()
}

// deviceSub is the firmware and the size, one line: the mockup's
// "Core v0.1.3 · up to date · 20.3 GB of 74.5 GB".
func (u *UI) deviceSub() string {
	d := u.st.Device
	if !d.Found {
		if u.st.Detecting {
			return "watching every two seconds"
		}
		return "not connected"
	}
	var parts []string
	switch {
	case d.Installed.Kind == fwpart.Other:
		parts = append(parts, "Apple firmware")
	case installedTag(d.Firmware) != "":
		parts = append(parts, "Core "+installedTag(d.Firmware))
	default:
		parts = append(parts, "Core")
	}
	switch {
	case u.st.UpdateAvailable():
		parts = append(parts, u.st.Release.Tag+" available")
	case u.st.Release.Checked && u.st.Release.Err == "":
		parts = append(parts, "up to date")
	}
	if sz := u.sizeLine(); sz != "" {
		parts = append(parts, sz)
	}
	return strings.Join(parts, "  ·  ")
}

// topStatus is the middle of the bar: the sentence that says what the one
// button is for. A running job owns it — a status line that still says "7
// albums to sync" while the sync runs is a line nobody reads twice.
func (u *UI) topStatus() (head, sub string) {
	if u.st.Busy() || (u.st.Job != nil && u.st.Job.Failed) {
		return u.st.StatusLine(), u.jobSub()
	}
	switch u.st.Phase() {
	case PhaseLooking:
		return "Looking for an iPod…", "hold Select + Menu, then Select + Play at the Apple logo"
	case PhaseNotInstalled:
		return "Core is not installed on this iPod",
			"Apple's firmware is backed up to a file first"
	}
	if n := u.st.PendingAlbums(); n > 0 {
		return fmt.Sprintf("%d album%s to sync", n, plural(n)),
			fmt.Sprintf("%d tracks · %s · from %s", u.st.PendingTracks(),
				disk.HumanSize(u.st.PendingBytes()), sourceName(u.st.Source))
	}
	if u.st.LibraryChanged {
		return "The library changed", "renamed files are copied again on the next sync"
	}
	if u.st.UpdateAvailable() {
		return "A newer Core is available", u.st.Release.Tag + " · " + u.st.Release.Asset
	}
	if len(u.st.Albums) > 0 {
		return "Everything is on the iPod", "eject before pulling the cable"
	}
	return "Ready", "choose a music folder to see your albums"
}

// jobSub is the second line while a job runs: its own progress text, which
// the status line above has already had the job name prefixed to.
func (u *UI) jobSub() string {
	if u.st.Job == nil {
		return ""
	}
	if u.st.Job.Pct > 0 && u.st.Job.Pct < 1 {
		return fmt.Sprintf("%.0f%%", u.st.Job.Pct*100)
	}
	return ""
}

// sourceName is the last element of the music folder, for a line that has
// room for a word and not for a path.
func sourceName(src string) string {
	src = strings.TrimRight(strings.TrimSpace(src), `\/`)
	if src == "" {
		return "your music folder"
	}
	if i := strings.LastIndexAny(src, `\/`); i >= 0 && i+1 < len(src) {
		return src[i+1:]
	}
	return src
}

// tabBar is the row under the top bar. It is only drawn on the main screen:
// the Looking and Install screens have one thing to say and tabs on them
// would offer a grid of an iPod that is not there.
func (u *UI) tabBar(gtx C) D {
	if u.st.Phase() != PhaseReady {
		return u.hairline(gtx)
	}
	tabs := []Tab{TabAlbums, TabLibrary, TabDetails}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			return layout.Inset{Left: unit.Dp(14), Right: unit.Dp(14), Bottom: unit.Dp(2)}.
				Layout(gtx, func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					children := make([]layout.FlexChild, 0, len(tabs)*2+1)
					for i, t := range tabs {
						tab := t
						idx := i
						if i > 0 {
							children = append(children, layout.Rigid(layout.Spacer{Width: unit.Dp(18)}.Layout))
						}
						children = append(children, layout.Rigid(func(gtx C) D {
							return u.tabItem(gtx, tab, idx)
						}))
					}
					children = append(children, layout.Flexed(1, func(gtx C) D {
						return layout.E.Layout(gtx, u.line(11, u.pal.Muted2, u.tabNote()))
					}))
					return layout.Flex{Alignment: layout.Middle}.Layout(gtx, children...)
				})
		}),
		layout.Rigid(u.hairline),
	)
}

// tabItem is one tab: the word, and a 2 px rule under the chosen one.
func (u *UI) tabItem(gtx C, t Tab, idx int) D {
	on := u.st.Tab == t
	col := u.pal.Muted
	if on {
		col = u.pal.Ink
	}
	label := t.String()
	if t == TabLibrary {
		if n := u.fixCount(); n > 0 {
			label += "  " + itoa(n)
		}
	}
	return u.tabClicks[idx].Layout(gtx, func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				return layout.Inset{Top: unit.Dp(6), Bottom: unit.Dp(4)}.Layout(gtx, func(gtx C) D {
					l := material.Label(u.th, unit.Sp(13), label)
					l.Color = col
					l.MaxLines = 1
					return l.Layout(gtx)
				})
			}),
			layout.Rigid(func(gtx C) D {
				h := gtx.Dp(2)
				c := u.pal.Surface
				if on {
					c = u.pal.Ink
				}
				sz := image.Pt(gtx.Constraints.Min.X, h)
				paint.FillShape(gtx.Ops, c, clip.Rect{Max: sz}.Op())
				return D{Size: sz}
			}),
		)
	})
}

// fixCount is the number on the Library tab: how many things the last scan
// found, tracks counted as tracks (see Category.Count).
func (u *UI) fixCount() int {
	n := 0
	for _, c := range Categories {
		n += c.Count(u.st.Report)
	}
	return n
}

// tabNote is the small right-hand line in the tab row: when the report was
// taken, or how many albums the grid holds.
func (u *UI) tabNote() string {
	switch u.st.Tab {
	case TabLibrary:
		if u.st.Report == nil {
			return "nothing scanned yet"
		}
		if u.st.Scanned.IsZero() {
			return "scanned"
		}
		return "scanned " + ago(time.Since(u.st.Scanned))
	case TabAlbums:
		if n := len(u.st.Albums); n > 0 {
			return fmt.Sprintf("%d albums · %d new · %d changed", n,
				countState(u.st.Albums, TileNew), countState(u.st.Albums, TileChanged))
		}
		return ""
	default:
		return u.st.CLIPath
	}
}

// countState counts the tiles in one state.
func countState(albums []Album, st TileState) int {
	n := 0
	for _, a := range albums {
		if a.State == st {
			n++
		}
	}
	return n
}

// ago is a duration as the footer says it.
func ago(d time.Duration) string {
	switch {
	case d < time.Minute:
		return "just now"
	case d < time.Hour:
		return fmt.Sprintf("%d min ago", int(d.Minutes()))
	default:
		return fmt.Sprintf("%d h ago", int(d.Hours()))
	}
}

// hairline is the 1 px rule the bars are separated by.
func (u *UI) hairline(gtx C) D {
	sz := image.Pt(gtx.Constraints.Max.X, gtx.Dp(1))
	paint.FillShape(gtx.Ops, u.pal.Border, clip.Rect{Max: sz}.Op())
	return D{Size: sz}
}

// footer is the bottom strip: what is on the iPod, and the music folder with
// the one link that changes it.
func (u *UI) footer(gtx C) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(u.hairline),
		layout.Rigid(func(gtx C) D {
			return layout.Inset{Top: unit.Dp(9), Bottom: unit.Dp(9), Left: unit.Dp(14), Right: unit.Dp(14)}.
				Layout(gtx, func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
						layout.Rigid(u.text(11, u.pal.MutedD, u.footerLine())),
						layout.Flexed(1, func(gtx C) D {
							return layout.E.Layout(gtx, u.line(11, u.pal.Muted, u.footerSource()))
						}),
						layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
						layout.Rigid(func(gtx C) D {
							if !u.pickerOK {
								return D{}
							}
							return u.changeBtn.Layout(gtx, func(gtx C) D {
								col := u.pal.Accent
								if u.st.Busy() {
									col = u.pal.SelSub
								}
								return u.text(11, col, "Change")(gtx)
							})
						}),
					)
				})
		}),
	)
}

// progress is the one place Accent is allowed to move. An indeterminate
// job draws the full track in the muted colour rather than a fake
// fraction: a bar that claims 40 % when nothing knows the total is a bar
// that teaches people to distrust it.
func (u *UI) progress(gtx C) D {
	pct := float32(0)
	col := u.pal.Accent
	switch {
	case u.st.Job == nil:
		col = u.pal.Trk
	case u.st.Job.Failed:
		pct, col = 1, u.pal.Muted2
	case u.st.Job.Pct < 0:
		pct, col = 1, u.pal.Muted2
	default:
		pct = u.st.Job.Pct
	}
	bar := material.ProgressBar(u.th, pct)
	bar.Color = col
	bar.TrackColor = u.pal.Trk
	bar.Height = unit.Dp(4)
	return bar.Layout(gtx)
}

// body is the phase switch: the whole middle of the window.
//
// The log pane below it does not move, because everything that happens
// in any phase says what it is doing there — a Looking screen that
// hides the reason the raw open was denied is a screen with one
// sentence and no way forward.
func (u *UI) body(gtx C) D {
	switch u.st.Phase() {
	case PhaseLooking:
		return u.lookingPane(gtx)
	case PhaseNotInstalled:
		return u.installPane(gtx)
	}
	switch u.st.Tab {
	case TabLibrary:
		return u.libraryPane(gtx)
	case TabDetails:
		return u.detailsPane(gtx)
	default:
		return u.albumsPane(gtx)
	}
}

// lookingPane is the screen with no iPod on it.
//
// It has no button at all. There is nothing for a button to do that the
// poll is not already doing twice a second, and a Refresh that a user
// presses because the screen implied it was needed teaches them the app
// does not notice things by itself.
func (u *UI) lookingPane(gtx C) D {
	d := u.st.Device
	return u.topPlate(gtx, func(gtx C) D {
		return layout.UniformInset(unit.Dp(18)).Layout(gtx, func(gtx C) D {
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			var rows []layout.FlexChild
			add := func(w layout.Widget) { rows = append(rows, layout.Rigid(w)) }

			add(u.text(18, u.pal.Ink, u.lookingHead()))
			rows = append(rows, layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout))
			add(u.wrap(12, u.pal.Muted, "Plug the iPod into a port that carries data, then put it in disk mode:"))
			rows = append(rows, layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout))
			add(u.line(13, u.pal.Ink, "1.  Hold  Select + Menu  until it resets"))
			add(u.line(13, u.pal.Ink, "2.  At the Apple logo, hold  Select + Play"))
			rows = append(rows, layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout))
			add(u.wrap(11, u.pal.Muted2, u.lookingNote()))
			if d.Err != "" {
				rows = append(rows, layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout))
				add(u.line(11, u.pal.Muted2, "last look:  "+d.Err))
			}
			if d.ElevationAdviceNeeded {
				add(u.wrap(11, u.pal.Accent, "Reading a raw disk needs Administrator: close this and start it with Run as administrator."))
			}
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
		})
	})
}

func (u *UI) lookingHead() string {
	if u.st.Device.Err == "the iPod was unplugged" {
		return "The iPod was unplugged"
	}
	return "Looking for an iPod…"
}

func (u *UI) lookingNote() string {
	if u.st.Detecting {
		return "This window checks every two seconds. It will find the iPod on its own — " +
			"there is nothing to press."
	}
	return "Auto-detect is off in this window (--no-detect)."
}

// installPane is the one screen a stock iPod gets: what was found, what
// will happen to it, and one button.
//
// The promise on it is the whole reason the screen exists rather than a
// dialog: Apple stopped distributing this firmware, the copy on the
// device is the only one, and the sentence saying where it is going is
// what makes pressing the button reasonable (library-manager-plan.md,
// decision 3).
func (u *UI) installPane(gtx C) D {
	d := u.st.Device
	busy := u.st.Busy()
	return u.topPlate(gtx, func(gtx C) D {
		return layout.UniformInset(unit.Dp(18)).Layout(gtx, func(gtx C) D {
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(u.line(18, u.pal.Ink, "Core is not installed on this iPod")),
				layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
				layout.Rigid(u.line(13, u.pal.Ink, "found      "+d.DisplayName()+"  ·  "+disk.HumanSize(d.Size))),
				layout.Rigid(u.line(12, u.pal.Muted, "           "+d.Path+"  ·  "+installedText(d))),
				layout.Rigid(layout.Spacer{Height: unit.Dp(12)}.Layout),
				layout.Rigid(u.wrap(12, u.pal.MutedD, u.appleBackupPromise())),
				layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
				layout.Rigid(u.wrap(11, u.pal.Muted, "Then the image is written and read back, and CORECFG.DAT, "+
					"CORELOG.BIN and Music\\ are created on the music volume — the firmware can "+
					"overwrite those files but cannot create them.")),
				layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
				layout.Rigid(u.wrap(11, u.pal.Muted2, "The write asks for the device path typed out first, and cannot be "+
					"stopped once it starts. If anything goes wrong: hold Select+Menu, then Select+Play at the "+
					"Apple logo. That is disk mode and it is in ROM.")),
				layout.Rigid(layout.Spacer{Height: unit.Dp(14)}.Layout),
				layout.Rigid(func(gtx C) D {
					return u.buttonRow(gtx,
						btn{&u.installBtn, "Install Core", true, !busy},
						btn{&u.installFileBtn, "Install from a file…", false, !busy && u.pickerOK},
					)
				}),
			)
		})
	})
}

// topPlate is a plate that hugs its content at the top of the space it
// is given, rather than stretching to fill it. A card with three
// sentences in it and 300 px of empty plate underneath reads as a
// screen that failed to load the rest.
func (u *UI) topPlate(gtx C, w layout.Widget) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			gtx.Constraints.Min.Y = 0
			return u.plate(gtx, w)
		}),
	)
}

// installedText is the classification, in the words fwpart chose, with
// a fallback for a device nobody managed to classify.
func installedText(d Device) string {
	if s := strings.TrimSpace(d.Installed.Description); s != "" {
		return s
	}
	return "firmware not recognised"
}

// appleBackupPromise names the file Apple's firmware is about to be
// copied into. It calls the clock, which a layout function should not
// do lightly — but the name carries today's date and a promise about a
// file with a different name in it is not a promise.
func (u *UI) appleBackupPromise() string {
	name := flasher.AppleBackupFileName(u.st.Device.Serial, time.Now())
	return "Apple's firmware on this iPod is copied to " + name +
		" before a single byte is written. Apple does not distribute it and it cannot be " +
		"taken from another iPod, so that file is the only copy there is — it is never deleted."
}

// --- the iPod's name and the firmware note ------------------------------

// Every line on a plate is one line: MaxLines 1 with a clean clip. A
// device path, a build id and a release note are all long enough to wrap
// at 340 px, and a pane that changes height because the string in it got
// longer is a pane that pushes its button row off the bottom of the
// window on the machine where it matters.

// nameRow is the iPod's name: a label until it is clicked, then a field
// with Save and Cancel.
//
// It shows DisplayName — the friendly name, else the volume label as FAT
// stores it, else "iPod Video 80 GB". "Apple iPod" (the SCSI model string
// every iPod ever made reports) is never a name; the hardware line below
// carries what this device actually is.
func (u *UI) nameRow(gtx C) D {
	d := u.st.Device
	if u.editingName {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
					layout.Flexed(1, func(gtx C) D {
						return u.field(gtx, &u.nameEd, "Brandon's iPod")
					}),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D {
						return u.button(gtx, btn{&u.nameSaveBtn, "Save", true, !u.st.Busy()})
					}),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D {
						return u.button(gtx, btn{&u.nameCancelBtn, "Cancel", false, true})
					}),
				)
			}),
			layout.Rigid(u.line(11, u.pal.Muted2, u.namePreview())),
		)
	}
	head := d.DisplayName()
	if d.Size > 0 {
		head += "  ·  " + disk.HumanSize(d.Size)
	}
	hint := "click to rename"
	if u.st.Busy() {
		hint = ""
	}
	return u.nameClick.Layout(gtx, func(gtx C) D {
		return layout.Flex{Alignment: layout.Baseline}.Layout(gtx,
			layout.Flexed(1, u.line(14, u.pal.Ink, head)),
			layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
			layout.Rigid(u.text(10, u.pal.Muted2, hint)),
		)
	})
}

// namePreview is the sentence under the field while it is being typed
// in. A FAT label is eleven upper-case ASCII bytes, so most names come
// out shorter and louder than they went in, and finding that out in
// Explorer afterwards is how a rename reads as a bug.
func (u *UI) namePreview() string {
	typed := strings.TrimSpace(u.nameEd.Text())
	label := disk.LegalLabel(typed)
	switch {
	case typed == "":
		return "An empty name clears it: the drive shows with no name at all."
	case label == "":
		return "Nothing in that name fits a FAT label (A-Z, 0-9, space, !#$%&'()-@^_`{}~)."
	case label == typed:
		return "Windows will show it as " + label + "."
	default:
		return "Windows will show it as " + label + " — a FAT label is 11 upper-case ASCII bytes."
	}
}

// cancelLabel says out loud that the two write jobs cannot be stopped
// between the body write and the directory write. A Cancel that would
// leave a partition with a new body and an old directory row is a
// Cancel that bricks the device, so it is not offered — the button says
// why instead of pretending.
func (u *UI) cancelLabel() string {
	if u.st.Job != nil && u.st.Job.Kind.Writes() && u.st.Busy() {
		return "Cannot cancel once writing"
	}
	return "Cancel"
}

// flashNote is the sentence that decides whether a user can flash at
// all. It names the CLI beside the app, or says it is missing and gives
// the command — an app that offered a Flash button it could not honour
// would be worse than no button.
func (u *UI) flashNote() string {
	name := "core"
	if runtime.GOOS == "windows" {
		name = "core.exe"
	}
	if u.st.CLIPath == "" {
		file := strings.TrimSpace(u.flashEd.Text())
		if file == "" {
			file = "<image>"
		}
		return name + " is not beside this app. Run:  " + name + " flash " + file
	}
	if u.st.Device.Elevated {
		return "This window has Administrator rights: the write runs here. The whole partition is backed up first and the write is read back."
	}
	if runtime.GOOS == "windows" {
		return "Runs " + name + " elevated (a UAC prompt appears). The whole partition is backed up first and the write is read back."
	}
	return "Needs root — the sudo line goes to the log. The whole partition is backed up first and the write is read back."
}

// --- the log pane -------------------------------------------------------

func (u *UI) logPane(gtx C) D {
	return u.plate(gtx, func(gtx C) D {
		return layout.UniformInset(unit.Dp(10)).Layout(gtx, func(gtx C) D {
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
						layout.Rigid(u.text(11, u.pal.Muted2, fmt.Sprintf("LOG · %d lines", len(u.st.Log)))),
						layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Max.X, 0)} }),
						layout.Rigid(func(gtx C) D {
							return u.button(gtx, btn{&u.copyBtn, "Copy", false, len(u.st.Log) > 0})
						}),
					)
				}),
				layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
				layout.Flexed(1, func(gtx C) D {
					list := material.List(u.th, &u.logList)
					list.Indicator.Color = u.pal.SelSub
					return list.Layout(gtx, len(u.st.Log), func(gtx C, i int) D {
						l := material.Label(u.th, unit.Sp(11), u.st.Log[i])
						l.Font = Mono
						l.Color = u.pal.SelTrk
						l.MaxLines = 1
						return l.Layout(gtx)
					})
				}),
			)
		})
	})
}

// --- the modal ----------------------------------------------------------

func (u *UI) dialog(gtx C) D {
	// The scrim. It is drawn over the whole window, dims what is behind
	// it, and is the visual half of the input block set up in Layout.
	paint.FillShape(gtx.Ops, color.NRGBA{R: 0x19, G: 0x14, B: 0x10, A: 0x99},
		clip.Rect{Max: gtx.Constraints.Min}.Op())

	return layout.Center.Layout(gtx, func(gtx C) D {
		if w := gtx.Constraints.Max.X - gtx.Dp(64); w < gtx.Constraints.Max.X {
			gtx.Constraints.Max.X = w
		}
		gtx.Constraints.Min.X = gtx.Constraints.Max.X
		if h := gtx.Constraints.Max.Y - gtx.Dp(48); h > 0 {
			gtx.Constraints.Max.Y = h
		}
		gtx.Constraints.Min.Y = 0
		return u.plate(gtx, func(gtx C) D {
			return layout.UniformInset(unit.Dp(14)).Layout(gtx, func(gtx C) D {
				return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
					layout.Rigid(u.text(16, u.pal.Ink, u.dlg.title)),
					layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
					layout.Rigid(func(gtx C) D {
						lines := strings.Split(u.dlg.body, "\n")
						// A Gio list fills its main axis, so a short
						// plan in a tall window would leave the two
						// buttons stranded at the bottom of an empty
						// box. Cap the height at what the text actually
						// needs and the dialog shrinks to its content;
						// a long plan still scrolls.
						if want := len(lines) * gtx.Sp(unit.Sp(16)); want < gtx.Constraints.Max.Y {
							gtx.Constraints.Max.Y = want
						}
						gtx.Constraints.Min.Y = 0
						list := material.List(u.th, &u.dlgBody)
						list.Indicator.Color = u.pal.SelSub
						return list.Layout(gtx, len(lines), func(gtx C, i int) D {
							l := material.Label(u.th, unit.Sp(11), lines[i])
							l.Font = Mono
							l.Color = u.pal.MutedD
							l.MaxLines = 1
							l.Truncator = " "
							return l.Layout(gtx)
						})
					}),
					layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
					layout.Rigid(u.text(13, u.pal.Ink, u.dlg.prompt)),
					layout.Rigid(func(gtx C) D {
						if u.dlg.want == "" {
							return D{}
						}
						return layout.Inset{Top: unit.Dp(6)}.Layout(gtx, func(gtx C) D {
							return u.field(gtx, &u.confirmEd, u.dlg.want)
						})
					}),
					layout.Rigid(layout.Spacer{Height: unit.Dp(12)}.Layout),
					layout.Rigid(func(gtx C) D {
						ok := u.dlg.want == "" || strings.TrimSpace(u.confirmEd.Text()) == u.dlg.want
						return layout.Flex{}.Layout(gtx,
							layout.Rigid(func(gtx C) D {
								return u.button(gtx, btn{&u.dlgCancel, "Cancel", false, true})
							}),
							layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Max.X, 0)} }),
							layout.Rigid(func(gtx C) D {
								return u.button(gtx, btn{&u.dlgOK, u.dlg.okLabel, true, ok})
							}),
						)
					}),
				)
			})
		})
	})
}

// --- small drawing helpers ---------------------------------------------

// line is one line of text: a value, held to one line so a card cannot
// change height because the string in it got longer. wrap is the same
// thing allowed to wrap, for the places that are a sentence.
//
// Truncator is a single space and not the default "…" on purpose.
// Gio v0.8 draws the truncator glyph at the START of the line rather
// than the end (checked here with a three-label headless render), so a
// line that overflows would read "…library 928 songs" — which looks
// like the beginning is missing when it is the end that is. Every
// string on a card is written to fit at the 720 px minimum; the
// truncation is the backstop for an unusually long serial or build id,
// and a clean clip is the honest way to show one.
func (u *UI) line(size unit.Sp, col color.NRGBA, txt string) layout.Widget {
	return func(gtx C) D {
		l := material.Label(u.th, size, txt)
		l.Color = col
		l.MaxLines = 1
		l.Truncator = " "
		return layout.Inset{Bottom: unit.Dp(2)}.Layout(gtx, l.Layout)
	}
}

func (u *UI) wrap(size unit.Sp, col color.NRGBA, txt string) layout.Widget {
	return func(gtx C) D {
		l := material.Label(u.th, size, txt)
		l.Color = col
		l.MaxLines = 3
		return layout.Inset{Bottom: unit.Dp(2)}.Layout(gtx, l.Layout)
	}
}

func (u *UI) text(size unit.Sp, col color.NRGBA, txt string) layout.Widget {
	return func(gtx C) D {
		l := material.Label(u.th, size, txt)
		l.Color = col
		return l.Layout(gtx)
	}
}

// plate is a card ground: the Plate fill with a hairline Border, which
// is the only elevation this design has. No shadows — the device has
// none and a drop shadow under a rectangle is the thing that makes a
// flat palette look like a mistake.
func (u *UI) plate(gtx C, w layout.Widget) D {
	return widget.Border{Color: u.pal.Border, CornerRadius: unit.Dp(8), Width: unit.Dp(1)}.Layout(gtx,
		func(gtx C) D {
			return layout.Stack{}.Layout(gtx,
				layout.Expanded(func(gtx C) D {
					r := gtx.Dp(8)
					defer clip.UniformRRect(image.Rectangle{Max: gtx.Constraints.Min}, r).Push(gtx.Ops).Pop()
					paint.ColorOp{Color: u.pal.Plate}.Add(gtx.Ops)
					paint.PaintOp{}.Add(gtx.Ops)
					return D{Size: gtx.Constraints.Min}
				}),
				layout.Stacked(func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					return w(gtx)
				}),
			)
		})
}

func (u *UI) card(gtx C, title string, body layout.Widget) D {
	return u.plate(gtx, func(gtx C) D {
		return layout.UniformInset(unit.Dp(10)).Layout(gtx, func(gtx C) D {
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					l := material.Label(u.th, unit.Sp(9), title)
					l.Color = u.pal.Muted2
					l.MaxLines = 1
					return l.Layout(gtx)
				}),
				layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
				layout.Rigid(body),
			)
		})
	})
}

// field is a bordered text input. material.Editor has no frame of its
// own, and an unframed input on a plate is indistinguishable from a
// label.
func (u *UI) field(gtx C, ed *widget.Editor, hint string) D {
	return widget.Border{Color: u.pal.Trk, CornerRadius: unit.Dp(5), Width: unit.Dp(1)}.Layout(gtx,
		func(gtx C) D {
			return layout.Stack{}.Layout(gtx,
				layout.Expanded(func(gtx C) D {
					r := gtx.Dp(5)
					defer clip.UniformRRect(image.Rectangle{Max: gtx.Constraints.Min}, r).Push(gtx.Ops).Pop()
					paint.ColorOp{Color: u.pal.Surface}.Add(gtx.Ops)
					paint.PaintOp{}.Add(gtx.Ops)
					return D{Size: gtx.Constraints.Min}
				}),
				layout.Stacked(func(gtx C) D {
					gtx.Constraints.Min.X = gtx.Constraints.Max.X
					return layout.Inset{Top: unit.Dp(6), Bottom: unit.Dp(6), Left: unit.Dp(7), Right: unit.Dp(7)}.
						Layout(gtx, func(gtx C) D {
							e := material.Editor(u.th, ed, hint)
							e.TextSize = unit.Sp(12)
							e.Color = u.pal.Ink
							e.HintColor = u.pal.SelSub
							e.SelectionColor = u.pal.Trk
							return e.Layout(gtx)
						})
				}),
			)
		})
}

// btn is one button's whole description.
type btn struct {
	click   *widget.Clickable
	label   string
	primary bool
	enabled bool
}

func (u *UI) buttonRow(gtx C, bs ...btn) D {
	children := make([]layout.FlexChild, 0, len(bs)*2)
	for i := range bs {
		b := bs[i]
		if i > 0 {
			children = append(children, layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout))
		}
		children = append(children, layout.Rigid(func(gtx C) D { return u.button(gtx, b) }))
	}
	return layout.Flex{}.Layout(gtx, children...)
}

// button draws one. A disabled button keeps its shape and loses its
// contrast rather than disappearing: a row whose buttons come and go
// while a job runs is a row that moves under the pointer.
func (u *UI) button(gtx C, b btn) D {
	s := material.Button(u.th, b.click, b.label)
	s.CornerRadius = unit.Dp(5)
	s.TextSize = unit.Sp(12)
	s.Inset = layout.Inset{Top: unit.Dp(6), Bottom: unit.Dp(6), Left: unit.Dp(10), Right: unit.Dp(10)}
	switch {
	case !b.enabled:
		s.Background, s.Color = u.pal.PillOff, u.pal.Surface
		gtx = gtx.Disabled()
	case b.primary:
		s.Background, s.Color = u.pal.Accent, u.pal.Surface
	default:
		s.Background, s.Color = u.pal.Trk, u.pal.Ink
	}
	return s.Layout(gtx)
}

// --- formatting ---------------------------------------------------------

func itoa(n int) string { return fmt.Sprintf("%d", n) }

func okWord(ok bool) string {
	if ok {
		return "OK"
	}
	return "BAD"
}

// comma groups a byte count the way the index's own size is quoted in
// the docs (237,584 B).
func comma(n int64) string {
	s := fmt.Sprintf("%d", n)
	if len(s) <= 3 {
		return s
	}
	var out []byte
	for i, c := range []byte(s) {
		if i > 0 && (len(s)-i)%3 == 0 {
			out = append(out, ',')
		}
		out = append(out, c)
	}
	return string(out)
}

// tail keeps the last n lines of a block, which is what a dialog has
// room for.
func tail(s string, n int) string {
	s = strings.TrimRight(strings.ReplaceAll(s, "\r\n", "\n"), "\n")
	lines := strings.Split(s, "\n")
	if len(lines) <= n {
		return s
	}
	return "…\n" + strings.Join(lines[len(lines)-n:], "\n")
}

// keepArt filters the candidate and decision maps down to the albums the
// new report still lists as coverless, returning nil maps when nothing
// survives so the tab reads exactly as it does before any lookup.
func keepArt(rep *librarian.Report, cands map[librarian.AlbumKey][]artfetch.Candidate,
	choices map[librarian.AlbumKey]librarian.Decision) (
	map[librarian.AlbumKey][]artfetch.Candidate, map[librarian.AlbumKey]librarian.Decision) {
	if rep == nil || (len(cands) == 0 && len(choices) == 0) {
		return nil, nil
	}
	live := map[librarian.AlbumKey]bool{}
	for _, a := range rep.MissingArt {
		live[a.Key] = true
	}
	var kc map[librarian.AlbumKey][]artfetch.Candidate
	for k, v := range cands {
		if live[k] {
			if kc == nil {
				kc = map[librarian.AlbumKey][]artfetch.Candidate{}
			}
			kc[k] = v
		}
	}
	var kd map[librarian.AlbumKey]librarian.Decision
	for k, v := range choices {
		if live[k] {
			if kd == nil {
				kd = map[librarian.AlbumKey]librarian.Decision{}
			}
			kd[k] = v
		}
	}
	return kc, kd
}
