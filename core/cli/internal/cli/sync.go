package cli

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"path/filepath"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
	"github.com/spf13/cobra"
)

// newSyncCmd builds "core sync": the whole device state in one command.
//
// It replaces the old daily flow (Copy-Item the tree, build_index.py,
// coreart.py, make_config.py, Write-VolumeCache) with one pass that decides
// everything first and writes the index last.
func newSyncCmd() *cobra.Command {
	var o syncer.Options
	var asJSON, listSkips bool

	cmd := &cobra.Command{
		Use:   "sync",
		Short: "Put a source music tree on the iPod in the layout the firmware reads",
		Long: `Copies a source tree of "Album - Artist" folders onto the device as
Music/<Artist - Album>/NN. Title.flac, bakes the two art sidecars per
album, rewrites the playlists to device paths, creates CORECFG.DAT and
CORELOG.BIN if they are missing, and writes Music/CORELIB.IDX last.

--dst is the VOLUME ROOT of the iPod (D:\ on Windows), not its Music
folder.

What it will not do without being asked: delete anything. The plan always
lists what is on the device and not in the source; removing it takes
--prune --yes.

The index goes last on purpose. The device binds each record to a file by
hashing the folder and file names out of the index, so an index written
before a copy that then failed describes a library that is not there —
and nothing on the device would say so. Every other write happens first;
a failed copy leaves the index the device already had.

A track already on the device is skipped when its size matches and its
timestamp is within two seconds (FAT keeps time to two seconds). --verify
compares content hashes instead, which is slow and certain.`,
		Args:         cobra.NoArgs,
		SilenceUsage: true,
		RunE: func(cmd *cobra.Command, args []string) error {
			stdout, stderr := cmd.OutOrStdout(), cmd.ErrOrStderr()
			if !cmd.Flags().Changed("genre-map") {
				o.GenreMap = defaultGenreMap()
			}

			scan, err := syncer.ScanSource(o)
			if err != nil {
				return err
			}
			plan, err := syncer.MakePlan(o, scan)
			if err != nil {
				return err
			}

			if o.DryRun {
				if asJSON {
					return writeJSON(stdout, plan)
				}
				printPlan(stdout, plan, listSkips)
				printWarnings(stderr, plan.Warnings)
				return nil
			}

			if !asJSON {
				o.Progress = func(e syncer.Event) { printEvent(stdout, e) }
			}
			rep, err := syncer.Execute(cmd.Context(), plan, o)
			if err != nil {
				if errors.Is(err, syncer.ErrPruneNeedsYes) {
					printPruneList(stderr, plan)
					return err
				}
				// Report what did get done before the failure: the device is
				// in a known state and the user needs to know which one.
				if rep != nil {
					printReport(stdout, rep)
				}
				return err
			}
			if asJSON {
				return writeJSON(stdout, rep)
			}
			printReport(stdout, rep)
			// Orphans are worth saying even when the user did not ask to
			// prune: they are the difference between what is on the device
			// and what the index now describes.
			if !o.Prune && len(plan.Prune) > 0 {
				fmt.Fprintf(stdout, "%d item(s) under %s/ are not in the source tree; run again with --prune to list them\n",
					len(plan.Prune), devicefs.MusicDir)
			}
			printWarnings(stderr, rep.Warnings)
			return nil
		},
	}

	f := cmd.Flags()
	f.StringVar(&o.Src, "src", "", "Source music tree of \"Album - Artist\" folders")
	f.StringVar(&o.Dst, "dst", "", "The iPod's volume root (D:\\, /media/IPOD)")
	f.BoolVar(&o.DryRun, "dry-run", false, "Print the whole plan with byte totals and write nothing")
	f.BoolVar(&o.Verify, "verify", false, "Compare content hashes instead of size and timestamp")
	f.BoolVar(&o.Prune, "prune", false, "Remove everything under Music/ that is not in the source (needs --yes)")
	f.BoolVar(&o.Yes, "yes", false, "Confirm the deletions --prune listed")
	f.BoolVar(&o.NoArt, "no-art", false, "Do not touch the art sidecars (existing ones are left alone)")
	f.BoolVar(&o.ArtRefresh, "art-refresh", false, "Re-render folder.art and folder.thm even when the ones on the device are valid")
	f.StringVar(&o.GenreMap, "genre-map", "", "JSON artist -> genre map; pass '' for tag genres only (default: the built-in map, or tools/artist_genres.json when run inside the repo)")
	f.StringVar(&o.Playlists, "playlists", "", "Folder of .m3u8/.m3u playlists (default: <src>/Playlists)")
	f.BoolVar(&asJSON, "json", false, "Print the plan (with --dry-run) or the report as JSON")
	f.BoolVar(&listSkips, "list-skips", false, "With --dry-run, also list every file that would be skipped")

	return cmd
}

func writeJSON(w io.Writer, v any) error {
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	return enc.Encode(v)
}

// printEvent is the live line-per-album output.
func printEvent(w io.Writer, e syncer.Event) {
	switch e.Kind {
	case syncer.EventAlbum:
		art := ""
		switch e.Art {
		case "ok":
			art = " / art ok"
		case "skip":
			art = " / art kept"
		case "none":
			art = " / no cover"
		case "fail":
			art = " / art FAILED"
		}
		fmt.Fprintf(w, "  [%d/%d] %s: copied %d / skipped %d%s%s\n",
			e.Index, e.Total, e.Album, e.Copied, e.Skipped,
			plural(e.Renamed, " / renamed %d"), art)
	case syncer.EventPhase:
		if e.Phase != "copy" {
			fmt.Fprintf(w, "%s...\n", e.Phase)
		}
	}
}

func plural(n int, format string) string {
	if n == 0 {
		return ""
	}
	return fmt.Sprintf(format, n)
}

func printPlan(w io.Writer, p *syncer.Plan, listSkips bool) {
	fmt.Fprintf(w, "source: %s\ndevice: %s\n\n", p.Src, p.Dst)
	fmt.Fprintf(w, "%d album(s), %d track(s)\n", p.Albums, p.Tracks)

	for _, op := range p.Rename {
		fmt.Fprintf(w, "  rename %s (%s)\n", op.Device, op.Reason)
	}
	for _, op := range p.Copy {
		fmt.Fprintf(w, "  copy   %s  %s (%s)\n", op.Device, humanBytes(op.Size), op.Reason)
	}
	if listSkips {
		for _, op := range p.Skip {
			fmt.Fprintf(w, "  skip   %s (%s)\n", op.Device, op.Reason)
		}
	}
	var artWrite int
	for _, a := range p.Art {
		if a.Write {
			artWrite++
			fmt.Fprintf(w, "  art    %s (%s)\n", a.Album, a.Reason)
		}
	}
	for _, pl := range p.Playlists {
		state := "write"
		if pl.Unchanged {
			state = "unchanged"
		}
		fmt.Fprintf(w, "  m3u8   %s: %d entr%s, %d dropped (%s)\n",
			pl.Name, len(pl.Entries), ies(len(pl.Entries)), len(pl.Dropped), state)
	}
	if len(p.Prune) > 0 {
		printPruneList(w, p)
	}

	fmt.Fprintf(w, "\n  copy    %4d file(s)  %s\n", len(p.Copy), humanBytes(p.CopyBytes))
	fmt.Fprintf(w, "  skip    %4d file(s)  %s\n", len(p.Skip), humanBytes(p.SkipBytes))
	if len(p.Rename) > 0 {
		fmt.Fprintf(w, "  rename  %4d file(s)\n", len(p.Rename))
	}
	fmt.Fprintf(w, "  art     %4d to render, %d already valid\n", artWrite, len(p.Art)-artWrite)
	fmt.Fprintf(w, "  %s: %s\n", devicefs.ConfigName, created(p.Config))
	if p.Clock {
		fmt.Fprintf(w, "  %s: clock will be stamped\n", devicefs.ConfigName)
	}
	fmt.Fprintf(w, "  %s: %s\n", devicefs.LogName, created(p.Log))
	state := "write"
	if p.Index.Unchanged {
		state = "already identical"
	}
	fmt.Fprintf(w, "  %s/%s: %d bytes, %d record(s), %s\n",
		devicefs.MusicDir, devicefs.IndexName, p.Index.Size, p.Index.Records, state)
	fmt.Fprintln(w, "\ndry run — nothing was written")
}

func printPruneList(w io.Writer, p *syncer.Plan) {
	fmt.Fprintf(w, "\n%d item(s) under %s/ are not in the source tree (%s):\n",
		len(p.Prune), devicefs.MusicDir, humanBytes(p.PruneBytes))
	for _, victim := range p.Prune {
		rel, err := filepath.Rel(p.Dst, victim)
		if err != nil {
			rel = victim
		}
		fmt.Fprintf(w, "  %s\n", filepath.ToSlash(rel))
	}
	fmt.Fprintln(w, "  (pass --prune --yes to remove them; nothing was deleted)")
}

func printReport(w io.Writer, r *syncer.Report) {
	fmt.Fprintf(w, "\n%d album(s): copied %d (%s), skipped %d (%s)",
		r.Albums, r.Copied, humanBytes(r.CopiedBytes), r.Skipped, humanBytes(r.SkippedBytes))
	if r.Renamed > 0 {
		fmt.Fprintf(w, ", renamed %d", r.Renamed)
	}
	fmt.Fprintln(w)
	fmt.Fprintf(w, "art: %d rendered, %d kept, %d album(s) with no cover\n",
		r.ArtWritten, r.ArtSkipped, r.ArtMissing)
	if r.PlaylistsWritten+r.PlaylistsUnchanged > 0 {
		fmt.Fprintf(w, "playlists: %d written, %d unchanged, %d entr%s, %d line(s) dropped\n",
			r.PlaylistsWritten, r.PlaylistsUnchanged, r.PlaylistEntries, ies(r.PlaylistEntries), r.PlaylistDropped)
	}
	if r.Pruned > 0 {
		fmt.Fprintf(w, "pruned: %d item(s), %s freed\n", r.Pruned, humanBytes(r.PrunedBytes))
	}
	if r.ConfigCreated {
		fmt.Fprintf(w, "%s: created\n", devicefs.ConfigName)
	}
	if r.LogCreated {
		fmt.Fprintf(w, "%s: created\n", devicefs.LogName)
	}
	if !r.ClockStamped.IsZero() {
		// The device takes this at its next boot — it cannot be told the time
		// while it is on the cable (internal/devicefs/clock.go).
		_, off := r.ClockStamped.Zone()
		fmt.Fprintf(w, "clock: stamped %s UTC (%s)\n",
			r.ClockStamped.UTC().Format("2006-01-02 15:04"), offsetText(off/60))
	}
	idx := "unchanged"
	if r.IndexWritten {
		idx = "written"
	}
	fmt.Fprintf(w, "%s/%s: %s, %d bytes, %d record(s)\n",
		devicefs.MusicDir, devicefs.IndexName, idx, r.IndexBytes, r.IndexRecords)
	fmt.Fprintf(w, "done in %s\n", r.Elapsed.Round(1e6))
}

// offsetText renders a minutes offset the way a user reads a time zone.
func offsetText(min int) string {
	sign := "+"
	if min < 0 {
		sign, min = "-", -min
	}
	return fmt.Sprintf("UTC%s%02d:%02d", sign, min/60, min%60)
}

func printWarnings(w io.Writer, warns []string) {
	for _, s := range warns {
		fmt.Fprintf(w, "warning: %s\n", s)
	}
}

func created(b bool) string {
	if b {
		return "create"
	}
	return "present and valid, left alone"
}

func ies(n int) string {
	if n == 1 {
		return "y"
	}
	return "ies"
}

// humanBytes prints a size the way a user reads one. Powers of 1000, because
// that is what the disk's label says.
func humanBytes(n int64) string {
	const unit = 1000
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := int64(unit), 0
	for v := n / unit; v >= unit && exp < 4; v /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %cB", float64(n)/float64(div), "kMGTP"[exp])
}
