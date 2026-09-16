package cli

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
	"github.com/spf13/cobra"
)

// newOrganizeCmd builds "core organize": the source tree's folder and file
// names, made to say what the tags say.
//
// It is a preview by default and a journalled apply on request, because the
// filename is the device's locator: every renamed track re-hashes to a name
// the iPod does not have yet, and the next sync copies it again. The preview's
// last line is that cost, in tracks.
func newOrganizeCmd() *cobra.Command {
	var (
		src          string
		root         string
		apply        bool
		dryRun       bool
		undo         string
		listJournals bool
		journalDir   string
		asJSON       bool
		discFolders  bool
	)

	cmd := &cobra.Command{
		Use:   "organize",
		Short: "Rename and re-folder a music tree from its tags (preview, apply, undo)",
		Long: `Puts a source tree into the one layout the device locator is derived from:

    <src>/Album - Artist/[Disc N/]NN - Artist - Title.flac

Everything in those names comes from the tags — the album, the artist
(decided once per album from albumartist, else artist), the title and the
track number. Nothing is guessed: a file whose tags do not carry all four
is listed under "needs attention" and is not touched, and a name that is
already taken by a different file is never overwritten.

Why this layout and not another: internal/library reads the device
filename as "NN. Title.flac" where NN is the file's position in the
byte-sorted folder listing, and the index record is bound to that name by
its hash. Zero-padded track numbers sort into track order, and
"NN - Artist - " is exactly the prefix the title is read back out of.

The cost of a rename is a re-copy: the iPod finds tracks by the hash of
their filename, so a renamed track is a new file to it and the old one
becomes an orphan the next sync prunes. The preview says how many.

Without --apply nothing is written. --apply writes an undo journal to
` + "`<user config dir>/core/journal/`" + ` and fsyncs it BEFORE the first rename;
--undo <journal> puts every file back, refusing any that changed since.`,
		Args: cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			if src == "" {
				src = root
			}
			if src == "" {
				src = os.Getenv("CORELIB_SRC")
			}
			o := organizer.Options{Root: src, JournalDir: journalDir, DiscFolders: discFolders}

			switch {
			case listJournals:
				return runListJournals(cmd, o)
			case undo != "":
				return runOrganizeUndo(cmd, undo, o)
			}
			if apply && dryRun {
				return errors.New("--apply and --dry-run are alternatives; pass one")
			}
			if src == "" {
				return errors.New("--src is required (or set CORELIB_SRC)")
			}
			return runOrganize(cmd, o, apply, asJSON)
		},
	}

	cmd.Flags().StringVar(&src, "src", "", "Source music tree to organize")
	cmd.Flags().StringVar(&root, "root", "", "Alias for --src")
	cmd.Flags().BoolVar(&apply, "apply", false, "Actually move the files (writes an undo journal first)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Print the plan and change nothing (the default)")
	cmd.Flags().StringVar(&undo, "undo", "", "Replay a journal backwards, putting every file back")
	cmd.Flags().BoolVar(&listJournals, "list-journals", false, "List the undo journals, newest first")
	cmd.Flags().StringVar(&journalDir, "journal-dir", "", "Where journals live (default <user config dir>/core/journal)")
	cmd.Flags().BoolVar(&asJSON, "json", false, "Print the plan as JSON")
	cmd.Flags().BoolVar(&discFolders, "disc-folders", false,
		"Split a flat album whose tags carry several discs into Disc N subfolders (off by default)")
	return cmd
}

func runOrganize(cmd *cobra.Command, o organizer.Options, apply, asJSON bool) error {
	out, errw := cmd.OutOrStdout(), cmd.ErrOrStderr()
	plan, err := organizer.Scan(o)
	if err != nil {
		return err
	}
	if asJSON {
		enc := json.NewEncoder(out)
		enc.SetIndent("", "  ")
		if !apply {
			return enc.Encode(plan)
		}
	} else {
		printOrganizePlan(out, plan)
	}
	for _, w := range plan.Warnings {
		fmt.Fprintf(errw, "warning: %s\n", w)
	}
	if !apply {
		if !asJSON && !plan.Empty() {
			fmt.Fprintf(out, "\nNothing was changed. Run it again with --apply to do it.\n")
		}
		return nil
	}
	if plan.Empty() {
		return nil
	}
	j, err := organizer.Apply(cmd.Context(), plan, o)
	if j != nil && j.Path != "" {
		fmt.Fprintf(out, "\nundo journal: %s\n", j.Path)
	}
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "moved %d file(s); %s\n", len(j.Ops), recopyLine(plan))
	if asJSON {
		enc := json.NewEncoder(out)
		enc.SetIndent("", "  ")
		return enc.Encode(plan)
	}
	return nil
}

// printOrganizePlan is the preview: one block per source folder, the moves as
// "before → after", then the files a person has to decide about, then the cost.
func printOrganizePlan(w io.Writer, p *organizer.Plan) {
	byDir := map[string][]organizer.Move{}
	for _, m := range p.Moves {
		d := relTo(p.Root, filepath.Dir(m.From))
		byDir[d] = append(byDir[d], m)
	}
	dirs := make([]string, 0, len(byDir))
	for d := range byDir {
		dirs = append(dirs, d)
	}
	sort.Strings(dirs)

	for _, d := range dirs {
		fmt.Fprintf(w, "%s\n", d)
		moves := byDir[d]
		width := 0
		for _, m := range moves {
			if n := len(filepath.Base(m.From)); n > width {
				width = n
			}
		}
		if width > 52 {
			width = 52
		}
		for _, m := range moves {
			to := relTo(p.Root, m.To)
			if filepath.Dir(m.From) == filepath.Dir(m.To) {
				to = filepath.Base(m.To)
			}
			fmt.Fprintf(w, "  %-*s  ->  %s  (%s)\n", width, filepath.Base(m.From), to, m.Reason)
		}
	}
	if len(p.DiscSplit) > 0 {
		fmt.Fprintf(w, "\nMulti-disc albums (%d files) — off by default, `--disc-folders` to split into Disc N folders\n",
			len(p.DiscSplit))
	}
	if len(p.Attention) > 0 {
		fmt.Fprintf(w, "\nNeeds attention (%d) — nothing below was touched:\n", len(p.Attention))
		for _, a := range p.Attention {
			fmt.Fprintf(w, "  %s — %s\n", relTo(p.Root, a.Path), a.Reason)
		}
	}
	fmt.Fprintln(w)
	if p.Empty() {
		fmt.Fprintf(w, "%s: every file is already named from its tags. Nothing to do.\n", p.Root)
		return
	}
	fmt.Fprintf(w, "%d file(s) in %d folder(s), %s\n",
		len(p.Moves), p.AlbumsTouched, humanBytes(p.Bytes()))
	fmt.Fprintf(w, "%s\n", recopyLine(p))
}

// recopyLine is the sentence decision 7 is about: a rename is a re-copy, and a
// 2 GB re-copy nobody expected reads as a bug.
func recopyLine(p *organizer.Plan) string {
	s := fmt.Sprintf("%d track(s) will be re-copied to the iPod on the next sync",
		p.TracksRecopied)
	if p.TracksNew > 0 {
		s += fmt.Sprintf(", and %d that the library cannot see today will be copied for the first time",
			p.TracksNew)
	}
	return s
}

func runOrganizeUndo(cmd *cobra.Command, journal string, o organizer.Options) error {
	out := cmd.OutOrStdout()
	rep, err := organizer.Undo(cmd.Context(), journal, o)
	if rep != nil {
		fmt.Fprintf(out, "%s: put %d file(s) back\n", journal, rep.Undone)
		for _, s := range rep.Skipped {
			fmt.Fprintf(out, "  skipped %s: %s\n", s.Op.To, s.Reason)
		}
	}
	return err
}

func runListJournals(cmd *cobra.Command, o organizer.Options) error {
	dir := o.JournalDir
	if dir == "" {
		d, err := organizer.DefaultJournalDir()
		if err != nil {
			return err
		}
		dir = d
	}
	js, err := organizer.ListJournals(dir)
	if err != nil {
		return err
	}
	out := cmd.OutOrStdout()
	if len(js) == 0 {
		fmt.Fprintf(out, "%s: no journals\n", dir)
		return nil
	}
	for _, j := range js {
		fmt.Fprintf(out, "%s  %s  %d/%d move(s)  %s\n",
			filepath.Base(j.Path), j.When.Local().Format("2006-01-02 15:04"),
			j.Completed(), len(j.Ops), j.Root)
	}
	fmt.Fprintf(out, "\nundo one with: core organize --undo %s\n", js[0].Path)
	return nil
}

// relTo is a path as the user typed it if it can be, absolute if it cannot.
func relTo(root, p string) string {
	if r, err := filepath.Rel(root, p); err == nil && !strings.HasPrefix(r, "..") {
		return filepath.ToSlash(r)
	}
	return p
}
