package app

import (
	"context"
	"fmt"
	"runtime"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
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
			_, err := u.be.Sync(ctx, o, emit)
			return err
		case JobSync:
			_, err := u.be.Sync(ctx, o, emit)
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
		_, err = u.be.Sync(ctx, o, emit)
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
	var plan strings.Builder
	tee := func(e Event) {
		if e.Kind == EventLog {
			plan.WriteString(e.Text)
		}
		emit(e)
	}
	confirm := func(prompt string) (string, error) {
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
			title:   "Write this image to the iPod?",
			body:    tail(plan.String(), 40) + note,
			prompt:  strings.TrimSpace(prompt),
			want:    want,
			okLabel: "Write it",
		})
	}
	return u.be.Flash(ctx, file, confirm, tee)
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
