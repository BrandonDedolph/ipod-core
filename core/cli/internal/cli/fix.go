package cli

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
	"github.com/spf13/cobra"
)

// newLibrarianArtClient builds the client the art half of `fix` uses. It is a
// variable so the tests can hand back one pointed at an httptest server; no
// test in this package makes a real network call.
var newLibrarianArtClient = func() (librarian.ArtClient, error) { return artfetch.New(), nil }

// newFixCmd builds "core fix": the one list of everything wrong with a
// library, and the one command that fixes it.
//
// It is a report by default. Nothing is written without --yes, and the art
// half does not even ask the internet unless --art (or --yes) says so, because
// an inspect is something a UI runs on every refresh and a search is something
// a rate-limited public service answers.
func newFixCmd() *cobra.Command {
	var (
		src         string
		names       bool
		organize    bool
		art         bool
		yes         bool
		dryRun      bool
		asJSON      bool
		discFolders bool
		minScore    float64
		journalDir  string
		undo        string
	)

	cmd := &cobra.Command{
		Use:   "fix",
		Short: "List — and fix — what is wrong with a music library",
		Long: `Reads a source tree once and says what is wrong with it:

  missing art     albums whose first track carries no cover picture
  misnamed        files in the right folder under the wrong name
  unorganized     files that belong in another folder
  needs attention files nobody can name from their tags — never guessed

and what fixing it costs: the iPod finds a track by the hash of its
filename, so every renamed track is copied to the device again on the
next sync and the old one is pruned. The last line says how many.

Without --yes this writes nothing. --yes fixes everything listed: the
renames and the folders as one journalled job (undo it with --undo),
and the covers, which are fetched, embedded in every track of the
album, baked into folder.art + folder.thm and dropped beside the
tracks as cover.jpg. A cover is only accepted unasked when it scored
--min-score or better (artist and album match once "(Deluxe)" and the
like are stripped); anything weaker is listed and skipped.

--names, --organize and --art each narrow the job to that one part;
with none of them, all three are meant.

Undo puts names and folders back. It does not remove embedded art —
the old file is not kept, so there is nothing to put back.`,
		Args: cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			if undo != "" {
				return runFixUndo(cmd, undo, journalDir)
			}
			if src == "" {
				src = os.Getenv("CORELIB_SRC")
			}
			if src == "" {
				return errors.New("--src is required (or set CORELIB_SRC)")
			}
			all := !names && !organize && !art
			return runFix(cmd, fixOptions{
				Src:      src,
				Names:    names || all,
				Organize: organize || all,
				Art:      art || all,
				AskArt:   art || (yes && all),
				// --dry-run alone runs the whole decision pass and prints
				// what --yes WOULD do; nothing is written either way.
				DiscFolders: discFolders,
				Apply:       yes || dryRun,
				DryRun:      dryRun,
				JSON:        asJSON,
				MinScore:    minScore,
				JournalDir:  journalDir,
			})
		},
	}

	cmd.Flags().StringVar(&src, "src", "", "Source music tree to inspect")
	cmd.Flags().BoolVar(&names, "names", false, "Only the filenames")
	cmd.Flags().BoolVar(&organize, "organize", false, "Only the folders")
	cmd.Flags().BoolVar(&art, "art", false, "Only the cover art (searches for it, and prints what it found)")
	cmd.Flags().BoolVar(&yes, "yes", false, "Actually fix it (writes an undo journal first)")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Decide everything the way --yes would, print it, and write nothing")
	cmd.Flags().BoolVar(&asJSON, "json", false, "Print the report (and the result) as JSON")
	cmd.Flags().BoolVar(&discFolders, "disc-folders", false,
		"Also split a flat album whose tags carry several discs into Disc N subfolders (off by default)")
	cmd.Flags().Float64Var(&minScore, "min-score", librarian.DefaultMinScore,
		"The match score --yes accepts a cover at")
	cmd.Flags().StringVar(&journalDir, "journal-dir", "",
		"Where undo journals live (default <user config dir>/core/journal)")
	cmd.Flags().StringVar(&undo, "undo", "", "Replay a journal backwards, putting every name back")
	return cmd
}

// fixOptions is the command's flags, resolved.
type fixOptions struct {
	Src                   string
	Names, Organize, Art  bool
	AskArt, Apply, DryRun bool
	// DiscFolders is the opt-in multi-disc split: it puts the moves in the
	// plan AND ticks them, which is one flag with one meaning.
	DiscFolders bool
	JSON        bool
	MinScore    float64
	JournalDir  string
}

// fixJSON is what --json prints: one object with whatever the run produced, so
// the UI can read the same command the terminal does.
type fixJSON struct {
	Report     *librarian.Report                           `json:"report"`
	Candidates map[librarian.AlbumKey][]artfetch.Candidate `json:"candidates,omitempty"`
	Result     *librarian.Result                           `json:"result,omitempty"`
}

func runFix(cmd *cobra.Command, o fixOptions) error {
	out := cmd.OutOrStdout()
	lo := librarian.Options{
		JournalDir:  o.JournalDir,
		DryRun:      o.DryRun,
		DiscFolders: o.DiscFolders,
	}
	if !o.JSON {
		// Only the fix job's log lines. The progress events are for a
		// window with a bar in it, the inspect's summary is the table
		// below, and the art search's lines are printed as a table too —
		// saying either of those twice is noise, not information.
		lo.Progress = func(e librarian.Event) {
			if e.Kind == librarian.EventLog && e.Job == librarian.JobFix {
				fmt.Fprintf(out, "  %s\n", e.Text)
			}
		}
	}

	rep, err := librarian.Inspect(cmd.Context(), o.Src, lo)
	if err != nil {
		return err
	}
	res := fixJSON{Report: rep}
	if !o.JSON {
		printFixReport(out, rep, o)
	}
	for _, w := range rep.Warnings {
		fmt.Fprintf(cmd.ErrOrStderr(), "warning: %s\n", w)
	}

	ch := librarian.Choices{FixNames: o.Names, Organize: o.Organize, DiscFolders: o.DiscFolders}
	if o.Art && o.AskArt && len(rep.MissingArt) > 0 {
		client, err := newLibrarianArtClient()
		if err != nil {
			return err
		}
		lo.Art = client
		if !o.JSON {
			fmt.Fprintf(out, "\nLooking for %d cover(s):\n", len(rep.MissingArt))
		}
		cands, err := librarian.Candidates(cmd.Context(), rep, client, lo)
		if err != nil {
			return err
		}
		res.Candidates = cands
		if !o.JSON {
			printCandidates(out, rep, cands, o.MinScore)
		}
		ch.Art = librarian.AcceptAbove(cands, o.MinScore)
	}

	if !o.Apply {
		if !o.JSON && !rep.Empty() {
			fmt.Fprintf(out, "\nNothing was changed. Run it again with --yes to fix it.\n")
		}
		if o.JSON {
			return encodeJSON(out, res)
		}
		return nil
	}

	result, err := librarian.Fix(cmd.Context(), rep, ch, lo)
	res.Result = result
	if o.JSON {
		if encErr := encodeJSON(out, res); encErr != nil && err == nil {
			err = encErr
		}
		return err
	}
	if result != nil {
		printFixResult(out, result)
	}
	return err
}

// printFixReport is the "things to fix" list: the counts, the preview per
// category, and the sentence that stops a 2 GB re-copy from reading as a bug.
func printFixReport(w io.Writer, r *librarian.Report, o fixOptions) {
	fmt.Fprintf(w, "%s\n", r.Root)
	fmt.Fprintf(w, "%d album(s), %d track(s)\n\n", r.Albums, r.Tracks)

	if o.Art {
		fmt.Fprintf(w, "Missing art (%d)\n", len(r.MissingArt))
		for _, a := range r.MissingArt {
			fmt.Fprintf(w, "  %-52s %d track(s)\n", truncate(string(a.Key), 52), a.Tracks)
		}
		if len(r.MissingArt) > 0 {
			fmt.Fprintln(w)
		}
	}
	if o.Names {
		printMoveTable(w, r.Root, "Misnamed", r.Misnamed)
	}
	if o.Organize {
		printMoveTable(w, r.Root, "Unorganized", r.Unorganized)
		printDiscSplit(w, r, o)
	}

	fmt.Fprintf(w, "Needs attention (%d) — nothing here is ever guessed\n", len(r.NeedsAttention))
	for _, a := range r.NeedsAttention {
		fmt.Fprintf(w, "  %s — %s\n", relTo(r.Root, a.Path), a.Reason)
	}
	fmt.Fprintln(w)

	if r.Empty() {
		fmt.Fprintf(w, "Nothing to fix.\n")
		return
	}
	fmt.Fprintf(w, "%s\n", fixRecopyLine(r))
}

// fixRecopyLine is decision 7: a rename is a re-copy, because the filename IS
// the locator.
func fixRecopyLine(r *librarian.Report) string {
	s := fmt.Sprintf("%d track(s) will be re-copied to the iPod on the next sync (%s)",
		r.RecopyTracks, humanBytes(r.RecopyBytes))
	if r.NewTracks > 0 {
		s += fmt.Sprintf(",\nand %d that the library cannot see today will be copied for the first time (%s)",
			r.NewTracks, humanBytes(r.NewBytes))
	}
	return s
}

// printDiscSplit is the multi-disc line. The split is off by default because
// the device reads disc and track off the TAGS — a flat multi-disc album
// already plays in the right order — so splitting it renames every file in it
// for nothing visible. Saying so is the point of the line.
func printDiscSplit(w io.Writer, r *librarian.Report, o fixOptions) {
	if len(r.DiscSplit) == 0 {
		return
	}
	if o.DiscFolders {
		printMoveTable(w, r.Root, "Multi-disc albums", r.DiscSplit)
		return
	}
	fmt.Fprintf(w, "Multi-disc albums (%d files) — off by default, `--disc-folders` to split into Disc N folders\n\n",
		len(r.DiscSplit))
}

func printMoveTable(w io.Writer, root, title string, moves []organizer.Move) {
	fmt.Fprintf(w, "%s (%d)\n", title, len(moves))
	byDir := map[string][]organizer.Move{}
	for _, m := range moves {
		byDir[relTo(root, filepath.Dir(m.From))] = append(byDir[relTo(root, filepath.Dir(m.From))], m)
	}
	dirs := make([]string, 0, len(byDir))
	for d := range byDir {
		dirs = append(dirs, d)
	}
	sort.Strings(dirs)
	for _, d := range dirs {
		fmt.Fprintf(w, "  %s\n", d)
		width := 0
		for _, m := range byDir[d] {
			if n := len(filepath.Base(m.From)); n > width {
				width = n
			}
		}
		if width > 44 {
			width = 44
		}
		for _, m := range byDir[d] {
			to := relTo(root, m.To)
			if filepath.Dir(m.From) == filepath.Dir(m.To) {
				to = filepath.Base(m.To)
			}
			fmt.Fprintf(w, "    %-*s  ->  %s\n", width, truncate(filepath.Base(m.From), 44), to)
		}
	}
	if len(moves) > 0 {
		fmt.Fprintln(w)
	}
}

func printCandidates(w io.Writer, r *librarian.Report, cands map[librarian.AlbumKey][]artfetch.Candidate, min float64) {
	for _, a := range r.MissingArt {
		list := cands[a.Key]
		if len(list) == 0 {
			fmt.Fprintf(w, "  %-40s no match\n", truncate(string(a.Key), 40))
			continue
		}
		top := list[0]
		mark := "skip"
		if top.Score+1e-9 >= min {
			mark = "take"
		}
		fmt.Fprintf(w, "  %-40s %s  [%s]\n", truncate(string(a.Key), 40), top, mark)
	}
}

func printFixResult(w io.Writer, res *librarian.Result) {
	did := "fixed"
	if res.DryRun {
		did = "would fix"
	}
	fmt.Fprintf(w, "\n%s: %d renamed, %d re-foldered, %d album(s) of art embedded, %d skipped\n",
		did, res.Renamed, res.Moved, res.ArtWritten, res.Skipped)
	for _, f := range res.Failures {
		fmt.Fprintf(w, "  failed: %s — %s\n", f.Album, f.Reason)
	}
	for _, m := range res.Deferred {
		fmt.Fprintf(w, "  left for later: %s\n", filepath.Base(m.From))
	}
	if res.Journal != "" {
		fmt.Fprintf(w, "undo journal: %s\n", res.Journal)
		fmt.Fprintf(w, "put the names back with: core fix --undo %s\n", res.Journal)
	}
}

func runFixUndo(cmd *cobra.Command, journal, journalDir string) error {
	out := cmd.OutOrStdout()
	rep, err := librarian.Undo(cmd.Context(), journal, librarian.Options{JournalDir: journalDir})
	if rep != nil {
		fmt.Fprintf(out, "%s: put %d file(s) back\n", journal, rep.Undone)
		for _, s := range rep.Skipped {
			fmt.Fprintf(out, "  skipped %s: %s\n", s.Op.To, s.Reason)
		}
		if rep.Undone > 0 {
			fmt.Fprintf(out, "Embedded cover art is not removed by an undo: the original file is "+
				"not kept, so there is nothing to put back.\n")
		}
	}
	return err
}

func encodeJSON(w io.Writer, v any) error {
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	return enc.Encode(v)
}
