package app

import (
	"context"
	"fmt"
	"runtime"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/installer"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// This file is what the buttons do: one function per job, each handed
// to the Runner, each written so that everything it needs from the
// model is read on the UI goroutine BEFORE the closure is built. A job
// goroutine that reached back into u.st would be the one data race in
// the program, and it would be invisible until a sync collided with a
// repaint.
//
// Results come back the other way: a job writes u.newDev / u.newLib /
// u.newRel under the mutex and the next frame's drain() folds them in.

// --- the jobs ----------------------------------------------------------

func (u *UI) start(kind JobKind, fn func(ctx context.Context, emit func(Event)) error) {
	if err := u.run.Start(kind, fn); err != nil {
		u.st.Logf("%s: %v", kind, err)
		return
	}
	u.st.Job = &JobStatus{Kind: kind, Pct: -1}
	u.st.Logf("--- %s ---", kind)
}

func (u *UI) startRefresh() {
	u.start(JobRefresh, func(ctx context.Context, emit func(Event)) error {
		emit(Event{Kind: EventProgress, Text: "looking for an iPod", Pct: -1})
		dev, lib, err := u.be.Refresh(ctx)
		if err != nil {
			return err
		}
		u.mu.Lock()
		u.newDev, u.newLib = &dev, &lib
		u.mu.Unlock()
		if dev.Found {
			emit(Event{Kind: EventLog, Text: fmt.Sprintf("device: %s — %s, %s",
				dev.Path, dev.Model, disk.HumanSize(dev.Size))})
		} else {
			emit(Event{Kind: EventLog, Text: "no iPod found: " + dev.Err})
		}
		return nil
	})
}

// startSync runs one of the three Music buttons.
//
// Sync + prune is two dialogs and two passes on purpose: the first
// dialog says what prune means, then a DRY RUN produces the actual
// orphan list, and the second dialog shows that list and asks for a
// typed word. Nothing is deleted on the strength of a number nobody has
// seen.
func (u *UI) startSync(kind JobKind) {
	src := strings.TrimSpace(u.sourceEd.Text())
	dst := u.st.Device.Volume
	u.start(kind, func(ctx context.Context, emit func(Event)) error {
		if src == "" {
			return fmt.Errorf("no music folder: type one in the Music card, or press Browse")
		}
		if dst == "" {
			return fmt.Errorf("no iPod volume: plug the iPod in (disk mode: Select+Play) and press Refresh")
		}
		o := syncer.Options{Src: src, Dst: dst, GenreMap: library.GenreMapEmbedded}
		switch kind {
		case JobDryRun:
			o.DryRun = true
			plan, err := u.be.Sync(ctx, o, emit)
			u.publishPlan(plan, false)
			return err
		case JobSync:
			plan, err := u.be.Sync(ctx, o, emit)
			u.publishPlan(plan, err == nil)
			return err
		}

		// JobSyncPrune.
		answer, err := u.ask(ctx, &dialogRequest{
			title:   "Sync + prune deletes music",
			body:    "Prune removes everything under Music/ on the iPod that this source folder does\nnot produce — tracks, albums and their art sidecars.\n\nsource  " + src + "\niPod    " + dst + "\n\nA dry run comes next and will list exactly what would go.",
			prompt:  "Continue to the list?",
			okLabel: "Show me the list",
		})
		if err != nil {
			return err
		}
		if answer == "" {
			return fmt.Errorf("cancelled before the dry run")
		}

		dry := o
		dry.DryRun, dry.Prune = true, true
		plan, err := u.be.Sync(ctx, dry, emit)
		if err != nil {
			return err
		}
		if len(plan.Prune) == 0 {
			emit(Event{Kind: EventLog, Text: "nothing to prune; running a plain sync instead"})
			_, err := u.be.Sync(ctx, o, emit)
			return err
		}
		answer, err = u.ask(ctx, &dialogRequest{
			title: fmt.Sprintf("Delete %d item(s) from the iPod?", len(plan.Prune)),
			body: fmt.Sprintf("%s would be deleted from %s:\n\n%s",
				disk.HumanSize(plan.PruneBytes), dst, strings.Join(plan.Prune, "\n")),
			prompt:  "Type  prune  to confirm:",
			want:    "prune",
			okLabel: "Delete and sync",
		})
		if err != nil {
			return err
		}
		if answer != "prune" {
			return fmt.Errorf("aborted: the confirmation did not match")
		}
		o.Prune, o.Yes = true, true
		plan, err = u.be.Sync(ctx, o, emit)
		u.publishPlan(plan, err == nil)
		return err
	})
}

func (u *UI) startCheck() {
	u.start(JobCheck, func(ctx context.Context, emit func(Event)) error {
		emit(Event{Kind: EventProgress, Text: "asking GitHub", Pct: -1})
		rel, err := u.be.Release(ctx)
		u.mu.Lock()
		u.newRel = &rel
		u.mu.Unlock()
		if err != nil {
			return err
		}
		emit(Event{Kind: EventLog, Text: "latest release: " + rel.Tag + " (" + rel.Asset + ")"})
		if rel.Notes != "" {
			emit(Event{Kind: EventLog, Text: "--- release notes ---\n" + rel.Notes + "\n--- end ---"})
		}
		return nil
	})
}

func (u *UI) startUpdate() {
	rel := u.st.Release
	installed := installedTag(u.st.Device.Firmware)
	cli := u.st.CLIPath
	u.start(JobUpdate, func(ctx context.Context, emit func(Event)) error {
		if !rel.Checked || rel.Tag == "" {
			return fmt.Errorf("press Check first, so there is a release to install")
		}
		if installed != "" && installed == rel.Tag {
			// Not a refusal — `core update --tag` re-flashes on purpose
			// too — but a fact the user should read before the dialog.
			emit(Event{Kind: EventLog, Text: "the iPod already runs " + installed +
				"; continuing will write the same version again"})
		}
		file, err := u.be.Download(ctx, rel, emit)
		if err != nil {
			return err
		}
		return u.flashFile(ctx, file, cli, emit)
	})
}

// installedTag is the "v0.1.2" out of fwpart.VersionText's
// "v0.1.2 (build …)", or "" when the text does not start with a tag.
func installedTag(firmware string) string {
	tag, _, _ := strings.Cut(strings.TrimSpace(firmware), " ")
	if strings.HasPrefix(tag, "v") {
		return tag
	}
	return ""
}

func (u *UI) startFlash(file string) {
	cli := u.st.CLIPath
	u.start(JobFlash, func(ctx context.Context, emit func(Event)) error {
		if file == "" {
			return fmt.Errorf("no image: type a path to a .ipod or .bin file, or press Flash file…")
		}
		return u.flashFile(ctx, file, cli, emit)
	})
}

// flashFile is the shared tail of Flash and Update.
//
// The confirmation dialog shows the flasher's OWN plan text — captured
// from Deps.Out as it is printed, which is everything written before
// Confirm is called — and requires the device path typed exactly, the
// same gate the CLI has. A GUI "Are you sure? [Yes]" over a raw disk
// write would be a downgrade from the terminal.
//
// cliPath is read from the model on the UI goroutine by the caller and
// passed in: this function runs on the job goroutine, where u.st is
// off limits.
func (u *UI) flashFile(ctx context.Context, file, cliPath string, emit func(Event)) error {
	plan, tee := teePlan(emit)
	confirm := u.confirmWrite(ctx, plan, cliPath,
		"Write this image to the iPod?", "Write it")
	return u.be.Flash(ctx, file, confirm, tee)
}

// teePlan splits a job's log events two ways: on to the window, and
// into a buffer the confirmation dialog shows as the plan. Everything
// the flasher printed before it asked is what the person is agreeing
// to, and it is the flasher's own words rather than a paraphrase.
func teePlan(emit func(Event)) (*strings.Builder, func(Event)) {
	plan := &strings.Builder{}
	return plan, func(e Event) {
		if e.Kind == EventLog {
			plan.WriteString(e.Text)
		}
		emit(e)
	}
}

// confirmWrite is the modal that guards every raw write this app makes:
// the flasher's own plan, and the device path typed exactly. Flash,
// Update and Install share it, because a second dialog with a slightly
// different gate is a second gate to get wrong.
//
// cliPath is read from the model on the UI goroutine by the caller and
// passed in: this runs on the job goroutine, where u.st is off limits.
func (u *UI) confirmWrite(ctx context.Context, plan *strings.Builder,
	cliPath, title, okLabel string) func(prompt string) (string, error) {
	return func(prompt string) (string, error) {
		note := ""
		if runtime.GOOS == "windows" && cliPath != "" {
			note = "\n\nWindows will show a UAC prompt for:\n  " + cliPath
		}
		want, ok := devicePathFromPrompt(prompt)
		if !ok {
			// The flasher's wording changed under us. Refusing here is
			// the only honest move: a dialog that cannot say what to
			// type would either wave the write through or fail it
			// with a message about a path nobody was shown.
			return "", fmt.Errorf("the flasher's confirmation prompt is not in the form core-app knows: %q", prompt)
		}
		return u.ask(ctx, &dialogRequest{
			title:   title,
			body:    tail(plan.String(), 40) + note,
			prompt:  strings.TrimSpace(prompt),
			want:    want,
			okLabel: okLabel,
		})
	}
}

// startInstall is the Install screen's one button: put Core on an iPod
// that is still running Apple's firmware.
//
// file is an image the user picked, or "" for the latest release —
// which is downloaded and checksum-verified here, in this process,
// before anything is written, so the elevated child (when there is one)
// never makes a network request.
//
// It ends with a Refresh rather than with what it wrote: the phase the
// window moves to is decided by reading the device back, so a write
// that verified but somehow left something else installed cannot show
// the main screen.
func (u *UI) startInstall(file string) {
	cli := u.st.CLIPath
	rel := u.st.Release
	u.start(JobInstall, func(ctx context.Context, emit func(Event)) error {
		image := file
		if image == "" {
			if !rel.Checked || rel.Tag == "" {
				got, err := u.be.Release(ctx)
				u.mu.Lock()
				u.newRel = &got
				u.mu.Unlock()
				if err != nil {
					return err
				}
				rel = got
			}
			var err error
			if image, err = u.be.Download(ctx, rel, emit); err != nil {
				return err
			}
		}

		plan, tee := teePlan(emit)
		confirm := u.confirmWrite(ctx, plan, cli,
			"Install Core on this iPod?", "Install it")
		res, err := u.be.Install(ctx, installer.Options{Image: image}, tee, confirm)
		if err != nil {
			return err
		}
		if res != nil && res.Flash != nil && res.Flash.Aborted {
			return fmt.Errorf("aborted: the confirmation did not match")
		}
		if res != nil && res.Flash != nil {
			if apple := res.Flash.AppleBackup(); apple != "" {
				emit(Event{Kind: EventLog, Text: "Apple's firmware is kept at " + apple +
					" — `core flash --from-backup " + apple + "` puts it back"})
			}
		}

		dev, lib, rerr := u.be.Refresh(ctx)
		if rerr != nil {
			return rerr
		}
		u.mu.Lock()
		u.newDev, u.newLib = &dev, &lib
		u.mu.Unlock()
		emit(Event{Kind: EventLog, Text: "firmware now: " + dev.Installed.Description})
		return nil
	})
}

// devicePathFromPrompt pulls the exact string the flasher wants typed
// out of its own prompt, so the dialog and the flasher agree about what
// counts as confirmation without this file re-deriving the device path.
// The prompt's shape is owned by internal/flasher (ConfirmPrompt); this
// is its inverse.
func devicePathFromPrompt(prompt string) (string, bool) {
	return flasher.ConfirmTarget(prompt)
}

func (u *UI) startBackup() {
	u.start(JobBackup, func(ctx context.Context, emit func(Event)) error {
		path, err := u.be.Backup(ctx, emit)
		if err != nil {
			return err
		}
		emit(Event{Kind: EventLog, Text: "backup written: " + path})
		return nil
	})
}

func (u *UI) startEject() {
	vol := u.st.Device.Volume
	u.start(JobEject, func(ctx context.Context, emit func(Event)) error {
		if vol == "" {
			return fmt.Errorf("no volume to eject; press Refresh with the iPod plugged in")
		}
		return u.be.Eject(ctx, vol, emit)
	})
}

// pick opens the OS folder/file dialog on its own goroutine — the
// dialog is a separate process and can sit on screen for a minute, and
// a UI goroutine waiting on it is a frozen window.
func (u *UI) pick(for_ JobKind) {
	go func() {
		var (
			path string
			err  error
		)
		if for_ == JobSync {
			path, err = PickFolder(context.Background())
		} else {
			path, err = PickFile(context.Background())
		}
		if err != nil {
			u.Post(Event{Kind: EventLog, Text: "picker: " + err.Error()})
			return
		}
		if path == "" {
			return
		}
		u.mu.Lock()
		u.newPath, u.pathFor = path, for_
		u.mu.Unlock()
		u.invalidate()
	}()
}

// --- the library manager -----------------------------------------------

// The Library tab's four jobs. Each one reads what it needs from the
// model on the UI goroutine, hands the rest to the Runner, and writes
// its result back under the mutex for the next frame's drain() — the
// same shape every other job in this file has.

// doPrimary runs whatever the top bar's one button currently says.
//
// The decision is State.Primary and it is made in exactly one place, so
// the label and the action cannot disagree: a button that says Sync and
// ejects is the single worst bug a one-button window can have.
func (u *UI) doPrimary() {
	switch u.st.Primary() {
	case ActionInstall:
		u.startInstall("")
	case ActionSync:
		u.startSync(JobSync)
	case ActionUpdate:
		u.startUpdate()
	case ActionEject:
		u.startEject()
	}
}

// setSource is the one place the music folder changes: the Browse
// button, the footer's Change, and Enter in either copy of the field.
//
// A new folder invalidates everything that was said about the old one —
// the report, the candidates, the decisions and the undo journal all
// describe a tree nobody is looking at any more — and then the two
// read-only passes run: the scan that fills the Library tab and the dry
// run that fills the grid.
func (u *UI) setSource(path string) {
	path = strings.TrimSpace(path)
	if path == "" {
		return
	}
	changed := path != u.st.Source
	u.st.Source = path
	u.sourceEd.SetText(path)
	u.saveConfig()
	if changed {
		u.st.Report, u.st.ArtCands, u.st.ArtChoices = nil, nil, nil
		u.st.Journal, u.st.Albums = "", nil
		u.st.LibraryChanged = false
	}
	u.startInspect(path)
	u.later(func() { u.startSync(JobDryRun) })
}

// startInspect is librarian.Inspect: one walk of the source tree, no
// network and no writes, which is why it runs on its own the moment a
// folder is chosen rather than waiting for a button.
func (u *UI) startInspect(src string) {
	src = strings.TrimSpace(src)
	if src == "" {
		u.st.Logf("scan: no music folder yet — press Browse")
		return
	}
	discs := u.st.DiscFolders
	u.start(JobInspect, func(ctx context.Context, emit func(Event)) error {
		emit(Event{Kind: EventProgress, Text: "reading the tags in " + src, Pct: -1})
		rep, err := u.be.Inspect(ctx, src, discs, emit)
		if err != nil {
			return err
		}
		u.mu.Lock()
		u.newReport = rep
		u.mu.Unlock()
		return nil
	})
}

// startCandidates is the one thing on this screen that goes to the
// internet, which is why it is a button and not part of the scan
// (plan §5 decision 2: Inspect never touches the network).
func (u *UI) startCandidates() {
	rep := u.st.Report
	if rep == nil || len(rep.MissingArt) == 0 {
		return
	}
	u.start(JobArt, func(ctx context.Context, emit func(Event)) error {
		cands, err := u.be.Candidates(ctx, rep, emit)
		if len(cands) > 0 {
			u.mu.Lock()
			u.newCands = cands
			u.mu.Unlock()
		}
		return err
	})
}

// startFix is Fix all: one job, one journal, one undo.
//
// The confirmation is not "are you sure" — it is the count of what will
// change and what that will cost the next sync, because a rename IS a
// re-copy and a person who has not been told that will read the sync
// that follows as a bug.
func (u *UI) startFix() {
	rep := u.st.Report
	if rep == nil {
		return
	}
	ch := u.choices()
	summary := u.fixSummary()
	src := u.st.Source
	u.start(JobFix, func(ctx context.Context, emit func(Event)) error {
		answer, err := u.ask(ctx, &dialogRequest{
			title:   "Fix the library?",
			body:    "in " + src + "\n\n" + summary,
			prompt:  "Names and folders can be undone until the next sync; a cover written into a file stays.",
			okLabel: "Fix all",
		})
		if err != nil {
			return err
		}
		if answer == "" {
			return fmt.Errorf("cancelled before anything was changed")
		}
		res, err := u.be.Fix(ctx, rep, ch, emit)
		if res != nil {
			u.mu.Lock()
			u.newFix = res
			u.mu.Unlock()
			emit(Event{Kind: EventLog, Text: fmt.Sprintf(
				"fixed: %d renamed, %d moved, %d cover(s) written, %d skipped",
				res.Renamed, res.Moved, res.ArtWritten, res.Skipped)})
			for _, f := range res.Failures {
				emit(Event{Kind: EventLog, Text: "could not finish " + string(f.Album) + ": " + f.Reason})
			}
			if res.Journal != "" {
				emit(Event{Kind: EventLog, Text: "undo journal: " + res.Journal})
			}
		}
		return err
	})
}

// startUndo replays the last journal backwards. It is enabled only while
// there is one: organizer refuses an op whose target changed since the
// move, so an undo of a journal the user has already synced past is not
// a promise this button should imply.
func (u *UI) startUndo(journal string) {
	if journal == "" {
		return
	}
	u.start(JobUndoFix, func(ctx context.Context, emit func(Event)) error {
		rep, err := u.be.UndoFix(ctx, journal, emit)
		if rep != nil {
			u.mu.Lock()
			u.newUndo = rep
			u.mu.Unlock()
			emit(Event{Kind: EventLog, Text: fmt.Sprintf("undone: %d file(s) put back, %d left alone",
				rep.Undone, len(rep.Skipped))})
			for _, sk := range rep.Skipped {
				emit(Event{Kind: EventLog, Text: "left alone: " + sk.Op.To + " — " + sk.Reason})
			}
			emit(Event{Kind: EventLog, Text: "embedded covers are not undone; " +
				"`core art --remove` takes a picture back out"})
		}
		return err
	})
}

// gridEvents is the Albums tab's half of the frame: a tile click shows
// what the plan says about that album.
func (u *UI) gridEvents(gtx C, busy bool) {
	for i := range u.tileClicks {
		if i >= len(u.st.Albums) {
			break
		}
		if u.tileClicks[i].Clicked(gtx) {
			u.showAlbum(u.st.Albums[i])
		}
	}
}

// showAlbum puts one album's facts on screen, in the modal the write
// confirmations already use. It is a read: there is no job behind it and
// the reply channel is buffered, so the OK button simply closes it.
func (u *UI) showAlbum(a Album) {
	state := "every track is on the iPod"
	switch a.State {
	case TileNew:
		state = "none of it is on the iPod yet"
	case TileChanged:
		state = fmt.Sprintf("%d of %d tracks are about to be copied", a.Copy, a.Tracks)
	}
	body := strings.Join([]string{
		"folder on the iPod   Music/" + a.Device,
		"folder on this PC    " + a.Dir,
		fmt.Sprintf("tracks               %d", a.Tracks),
		fmt.Sprintf("to copy              %d  (%s)", a.Copy, disk.HumanSize(a.Bytes)),
	}, "\n")
	u.dlg = &dialogRequest{
		title:   a.Title,
		body:    body,
		prompt:  state,
		okLabel: "Close",
		reply:   make(chan string, 1),
	}
}

// later queues one thing to start on a frame when nothing is running.
//
// The Runner takes one job at a time on purpose, so "scan the library,
// then plan the sync" cannot be two Start calls in a row. This is the
// queue that makes a sequence out of them without the second one
// racing the first: drain() pops it when the model says idle.
func (u *UI) later(f func()) { u.pending = append(u.pending, f) }

// publishPlan hands a finished plan to the UI goroutine.
//
// synced says the plan was executed rather than only planned, which is
// the difference between "these albums are about to be copied" and
// "these albums have just been copied" — the same plan, two grids.
func (u *UI) publishPlan(plan *syncer.Plan, synced bool) {
	if plan == nil {
		return
	}
	u.mu.Lock()
	u.newPlan, u.planSynced = plan, synced
	u.mu.Unlock()
}
