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

	"gioui.org/io/clipboard"
	"gioui.org/layout"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
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
	mu       sync.Mutex
	inbox    []Event
	askQueue *dialogRequest
	newDev   *Device
	newLib   *Library
	newRel   *Release
	newPath  string // a path chosen in the folder/file picker
	pathFor  JobKind

	dlg *dialogRequest // the dialog on screen; UI goroutine only

	cards   widget.List
	logList widget.List
	dlgBody widget.List

	sourceEd  widget.Editor
	flashEd   widget.Editor
	confirmEd widget.Editor

	refreshBtn, browseBtn                       widget.Clickable
	dryRunBtn, syncBtn, pruneBtn, cancelBtn     widget.Clickable
	checkBtn, updateBtn, flashBtn, flashOpenBtn widget.Clickable
	backupBtn, ejectBtn, copyBtn                widget.Clickable
	dlgOK, dlgCancel                            widget.Clickable

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
	u.dlgBody.Axis = layout.Vertical
	u.logList.Axis = layout.Vertical
	// The log is a transcript: the interesting end is the bottom, and a
	// pane that stays at the top while a sync scrolls past is a pane
	// nobody reads.
	u.logList.ScrollToEnd = true

	u.sourceEd.SingleLine, u.sourceEd.Submit = true, true
	u.flashEd.SingleLine, u.flashEd.Submit = true, true
	u.confirmEd.SingleLine, u.confirmEd.Submit = true, true

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
	ask := u.askQueue
	u.askQueue = nil
	u.mu.Unlock()

	for _, e := range events {
		u.st.Apply(e)
	}
	if dev != nil {
		u.st.Device = *dev
	}
	if lib != nil {
		u.st.Library = *lib
	}
	if rel != nil {
		u.st.Release = *rel
	}
	switch {
	case path != "" && pathFor == JobSync:
		u.st.Source = path
		u.sourceEd.SetText(path)
		u.saveConfig()
	case path != "" && pathFor == JobFlash:
		u.st.FlashFile = path
		u.flashEd.SetText(path)
	}
	if ask != nil {
		u.dlg = ask
		u.confirmEd.SetText("")
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

	if !busy {
		if u.refreshBtn.Clicked(gtx) {
			u.startRefresh()
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
	}
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
		case widget.ChangeEvent, widget.SubmitEvent:
			if s := strings.TrimSpace(u.sourceEd.Text()); s != u.st.Source {
				u.st.Source = s
				u.saveConfig()
			}
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
		layout.Rigid(u.header),
		layout.Flexed(1, func(gtx C) D {
			return layout.Inset{Left: unit.Dp(12), Right: unit.Dp(12)}.Layout(gtx, u.cardStack)
		}),
		layout.Rigid(func(gtx C) D { return layout.Spacer{Height: unit.Dp(8)}.Layout(gtx) }),
		layout.Flexed(0.34, func(gtx C) D {
			return layout.Inset{Left: unit.Dp(12), Right: unit.Dp(12), Bottom: unit.Dp(10)}.Layout(gtx, u.logPane)
		}),
	)
}

func (u *UI) header(gtx C) D {
	return layout.Inset{Top: unit.Dp(10), Bottom: unit.Dp(8), Left: unit.Dp(12), Right: unit.Dp(12)}.Layout(gtx,
		func(gtx C) D {
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					return layout.Flex{Alignment: layout.Baseline}.Layout(gtx,
						layout.Rigid(u.text(18, u.pal.Ink, "core")),
						layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
						layout.Rigid(u.text(11, u.pal.Muted2, version.Version)),
						layout.Flexed(1, func(gtx C) D {
							return layout.E.Layout(gtx, u.line(11, u.pal.Muted, u.st.StatusLine()))
						}),
					)
				}),
				layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
				layout.Rigid(u.progress),
			)
		})
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

// cardStack lays the four cards out as two columns.
//
// Two columns and not one scrolling stack because of the minimum window
// size: at 720x520 a vertical stack shows two cards and hides the other
// two below the fold, and "the firmware section exists, scroll down" is
// not a thing a first-time user finds. The whole grid sits inside a
// one-item list, so a window narrower or shorter than the content
// scrolls as a unit rather than clipping.
func (u *UI) cardStack(gtx C) D {
	list := material.List(u.th, &u.cards)
	list.Indicator.Color = u.pal.SelSub
	list.Indicator.HoverColor = u.pal.Muted2
	return list.Layout(gtx, 1, func(gtx C, _ int) D {
		return layout.Flex{Alignment: layout.Start}.Layout(gtx,
			layout.Flexed(1, func(gtx C) D {
				return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
					layout.Rigid(u.deviceCard),
					layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
					layout.Rigid(u.firmwareCard),
				)
			}),
			layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
			layout.Flexed(1, func(gtx C) D {
				return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
					layout.Rigid(u.musicCard),
					layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
					layout.Rigid(u.ejectCard),
				)
			}),
		)
	})
}

// --- card 1: the device -------------------------------------------------

// Every line in every card is one line: MaxLines 1 with an ellipsis.
// A device path, a build id and a release note are all long enough to
// wrap at 340 px, and a card that changes height when the string it
// shows gets longer is a card that pushes the button row off the bottom
// of the window on the machine where it matters.

func (u *UI) deviceCard(gtx C) D {
	d, lib := u.st.Device, u.st.Library
	return u.card(gtx, "IPOD", func(gtx C) D {
		var rows []layout.FlexChild
		add := func(w layout.Widget) { rows = append(rows, layout.Rigid(w)) }

		if d.Found {
			head := d.Model
			if d.Size > 0 {
				head += "  ·  " + disk.HumanSize(d.Size)
			}
			add(u.line(14, u.pal.Ink, head))

			where := d.Path
			if d.Volume != "" {
				where += "  ·  " + d.Volume
			}
			if d.Serial != "" {
				where += "  ·  " + d.Serial
			}
			add(u.line(11, u.pal.Muted, where))

			// Three facts, one line: the hardware gate, the unit every
			// byte offset in the flash path is multiplied by, and
			// whether the installed image still sums to its directory
			// row. The sentence behind the OSOS verdict is long and
			// goes to the log, where there is room for it.
			gate := "tested hardware"
			gateCol := u.pal.MutedD
			if !d.Tested {
				gate, gateCol = "UNTESTED hardware", u.pal.Accent
			}
			add(u.line(11, gateCol, gate+"  ·  sector "+itoa(d.SectorSize)+" B  ·  OSOS "+okWord(d.OSOSOK)))

			fw := d.Firmware
			if fw == "" {
				fw = "unknown (images before v0.1.3 carry no marker)"
			}
			add(u.line(12, u.pal.Ink, "firmware  "+fw))
		} else {
			add(u.line(14, u.pal.Ink, "No iPod found"))
			add(u.wrap(11, u.pal.Muted, "Put it in disk mode — hold Select+Menu to reset, then Select+Play at the Apple logo — and plug it into a port that carries data."))
			if d.Err != "" {
				add(u.line(11, u.pal.Muted2, d.Err))
			}
			if d.ElevationAdviceNeeded {
				add(u.wrap(11, u.pal.Accent, "Reading a raw disk needs Administrator: close this and start it with Run as administrator."))
			}
		}

		line := "library  not read yet"
		switch {
		case lib.Present:
			line = fmt.Sprintf("library  %d songs · %d albums · index %s B",
				lib.Songs, lib.Albums, comma(lib.IndexBytes))
		case lib.Note != "":
			line = "library  " + lib.Note
		}
		add(u.line(11, u.pal.MutedD, line))
		if lib.Present {
			add(u.line(11, u.pal.Muted, "CORECFG.DAT "+okWord(lib.ConfigValid)+
				"  ·  CORELOG.BIN "+okWord(lib.LogValid)+"  ·  "+itoa(lib.Genres)+" genres"))
		}

		rows = append(rows,
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				return u.buttonRow(gtx,
					btn{&u.refreshBtn, "Refresh", true, !u.st.Busy()},
					btn{&u.cancelBtn, u.cancelLabel(), false, u.st.CanCancel()},
				)
			}),
		)
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
	})
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

// --- card 2: music ------------------------------------------------------

func (u *UI) musicCard(gtx C) D {
	busy := u.st.Busy()
	return u.card(gtx, "MUSIC", func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(u.line(11, u.pal.Muted, `Source folders named "Album - Artist", each holding .flac`)),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(func(gtx C) D {
				return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
					layout.Flexed(1, func(gtx C) D {
						return u.field(gtx, &u.sourceEd, `C:\Users\you\Music`)
					}),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D {
						return u.button(gtx, btn{&u.browseBtn, "Browse…", false, !busy && u.pickerOK})
					}),
				)
			}),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(u.line(11, u.pal.Muted2, u.destLine())),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				ready := !busy && u.st.Device.Volume != ""
				return u.buttonRow(gtx,
					btn{&u.dryRunBtn, "Dry run", false, ready},
					btn{&u.syncBtn, "Sync", true, ready},
					btn{&u.pruneBtn, "Sync + prune", false, ready},
				)
			}),
		)
	})
}

func (u *UI) destLine() string {
	if !u.pickerOK {
		return "No folder picker here (zenity/kdialog) — type the path"
	}
	if u.st.Device.Volume == "" {
		return "Destination: no iPod volume yet — press Refresh"
	}
	return "Destination: " + u.st.Device.Volume + "  Music/ · CORELIB.IDX · CORECFG.DAT"
}

// --- card 3: firmware ---------------------------------------------------

func (u *UI) firmwareCard(gtx C) D {
	busy := u.st.Busy()
	installed := u.st.Device.Firmware
	if installed == "" {
		installed = "unknown"
	}
	latest := "—  press Check"
	if u.st.Release.Checked {
		latest = u.st.Release.Tag
		if u.st.Release.Err != "" {
			latest = "could not check: " + firstLine(u.st.Release.Err)
		}
	}
	return u.card(gtx, "FIRMWARE", func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(u.line(12, u.pal.Ink, "installed  "+installed)),
			layout.Rigid(u.line(12, u.pal.MutedD, "latest     "+latest)),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(func(gtx C) D {
				return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
					layout.Flexed(1, func(gtx C) D {
						return u.field(gtx, &u.flashEd, "path to core.ipod or core.bin")
					}),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D {
						return u.button(gtx, btn{&u.flashOpenBtn, "Flash file…", false, !busy && u.pickerOK})
					}),
				)
			}),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(u.wrap(11, u.pal.Muted2, u.flashNote())),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				return u.buttonRow(gtx,
					btn{&u.checkBtn, "Check", false, !busy},
					btn{&u.updateBtn, "Update", true, !busy && u.st.Release.Checked && u.st.Release.Tag != ""},
					btn{&u.flashBtn, "Flash", false, !busy && strings.TrimSpace(u.flashEd.Text()) != ""},
					btn{&u.backupBtn, "Backup", false, !busy && u.st.Device.Found},
				)
			}),
		)
	})
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

// --- card 4: eject ------------------------------------------------------

func (u *UI) ejectCard(gtx C) D {
	vol := u.st.Device.Volume
	note := "Flush the caches and dismount before pulling the cable."
	if vol != "" {
		note = "Flush and eject " + vol + " before pulling the cable."
	}
	return u.card(gtx, "EJECT", func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(u.wrap(11, u.pal.Muted, note)),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				return u.buttonRow(gtx, btn{&u.ejectBtn, "Eject", true, !u.st.Busy() && vol != ""})
			}),
		)
	})
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
