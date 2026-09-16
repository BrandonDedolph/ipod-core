package cli

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cidx"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
	"github.com/spf13/cobra"
)

func newIndexCmd() *cobra.Command {
	var (
		src       string
		out       string
		genreMap  string
		showDrift bool
		dryRun    bool
		quiet     bool
		maxSongs  int
	)

	cmd := &cobra.Command{
		Use:   "index",
		Short: "Build CORELIB.IDX from a source music tree",
		Long: `Reads a source tree of "Album - Artist" folders and writes the device
library index, CORELIB.IDX. The iPod's FAT volume is read-only to the
firmware, so the index cannot be built on the device: this file is what
lets it load the whole library in one read instead of probing every
track's tags at boot.

The output is byte-identical to tools/build_index.py, which stays as the
reference implementation and the parity oracle. One exception, and only
one: an MP3 with no Xing or VBRI header states no length, so both tools
ESTIMATE it from the first frame's bitrate. That is exact for a constant
bitrate file and only that; for a variable-bitrate file with no header
(lame -t and little else produces one) the two estimates differ and so
do the two indexes. The device reads the duration off the record either
way, so the effect is a wrong time on one track, not a track that will
not play.

Two different things live in each record. The FILENAME is a locator: the
NN in "NN. Title.flac" (or ".mp3" — nothing transcodes) is the file's
position in this tool's enumeration
(Disc folders in order, files sorted within each), and the record's hash
is computed over exactly that name — change it and tracks stop resolving
on a device that already holds the files. The TRACK NUMBER is metadata:
it comes from the tags first, a real "NN." or "D-NN" filename prefix
second, and the enumeration position last, and it is what the device
shows in the gutter and sorts the tracklist by. --show-drift lists every
track where the two disagree.`,
		Args: cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			if src == "" {
				src = os.Getenv("CORELIB_SRC")
			}
			if out == "" {
				out = os.Getenv("CORELIB_OUT")
			}
			if src == "" || (out == "" && !dryRun) {
				return errors.New("--src and --out are required (or set CORELIB_SRC / CORELIB_OUT)")
			}

			stdout, stderr := cmd.OutOrStdout(), cmd.ErrOrStderr()

			mapPath := genreMap
			if !cmd.Flags().Changed("genre-map") {
				mapPath = defaultGenreMap()
				if mapPath == "" {
					fmt.Fprintln(stderr, "genre map: none found next to the source or in the repo; using tag genres only")
				} else {
					fmt.Fprintf(stderr, "genre map: %s\n", mapPath)
				}
			}
			genres, err := library.LoadGenreMap(mapPath)
			if err != nil {
				return err
			}

			scan, err := library.ScanTree(src, library.Options{GenreMap: genres})
			if err != nil {
				return err
			}
			for _, w := range scan.Warnings {
				fmt.Fprintf(stderr, "warning: %s\n", w)
			}
			if !quiet {
				for i := range scan.Albums {
					a := &scan.Albums[i]
					fmt.Fprintf(stdout, "  %s: %d\n", a.DeviceFolder, len(a.Tracks))
				}
			}

			recs := cidx.RecordsFromScan(scan)
			if err := checkCaps(stderr, scan, len(recs), maxSongs); err != nil {
				return err
			}

			data := cidx.Encode(recs)
			if !dryRun {
				if err := writeAtomically(out, data); err != nil {
					return err
				}
			}

			where := out
			if dryRun {
				where = "(dry run — nothing written)"
			}
			fmt.Fprintf(stdout, "\nCORELIB.IDX: %d songs from %d albums, %d bytes -> %s\n",
				len(recs), len(scan.Albums), len(data), where)

			if n := len(scan.Drift); n > 0 {
				tail := " (--show-drift lists them)"
				if showDrift {
					tail = ""
				}
				fmt.Fprintf(stdout, "%d track(s) are numbered differently from their filename position%s\n", n, tail)
				if showDrift {
					for _, t := range scan.Drift {
						fmt.Fprintf(stdout, "  %s / %s: disc %d track %d (%s)\n",
							t.DeviceFolder, t.DeviceName, t.Disc, t.Track, t.NumberFrom)
					}
				}
			}

			// The failures are reported instead of being shipped: their
			// records carry duration 0 and a filename-derived title, which is
			// exactly what a healthy index looks like from the device's side,
			// so this list is the only warning there is.
			if n := len(scan.Failures); n > 0 {
				fmt.Fprintf(stderr, "\n%d track(s) could not be read. Their records carry duration 0 "+
					"and a filename-derived title, which is exactly what a healthy index looks like "+
					"from the device's side — so this list is the only warning you get:\n", n)
				for _, f := range scan.Failures {
					fmt.Fprintf(stderr, "  %s: %s\n", f.Path, f.Reason)
				}
				return fmt.Errorf("%d track(s) could not be read", n)
			}
			return nil
		},
	}

	cmd.Flags().StringVar(&src, "src", "", "Source music tree of \"Album - Artist\" folders (default: $CORELIB_SRC)")
	cmd.Flags().StringVar(&out, "out", "", "Path to write CORELIB.IDX to (default: $CORELIB_OUT)")
	cmd.Flags().StringVar(&genreMap, "genre-map", "", "JSON artist -> genre map; pass '' to use only the genres in the files' own tags (default: the built-in map, or tools/artist_genres.json when run inside the repo)")
	cmd.Flags().BoolVar(&showDrift, "show-drift", false, "List every track whose filename position differs from its track number, with where the number came from")
	cmd.Flags().BoolVar(&dryRun, "dry-run", false, "Scan and report, but write nothing")
	cmd.Flags().BoolVarP(&quiet, "quiet", "q", false, "Don't print a line per album")
	cmd.Flags().IntVar(&maxSongs, "max-songs", library.MaxSongs, "Override the firmware's LIB_MAX_SONGS cap check")

	return cmd
}

// checkCaps refuses an index the device would silently truncate.
// library_load_index() stops at LIB_MAX_SONGS with no error and no log, so
// everything past it simply is not in the library — a message here rather than
// a mystery on the device. The album and genre caps are warnings for the same
// reason: past them the device drops, and says nothing.
func checkCaps(stderr io.Writer, scan *library.Scan, n, maxSongs int) error {
	if maxSongs > 0 && n > maxSongs {
		return fmt.Errorf(`%d records exceeds the firmware's LIB_MAX_SONGS (%d).
The device would load the first %d and silently drop %d tracks — no error, they just would not appear in the library.
Raise LIB_MAX_SONGS in core/kernel/main.c — it costs roughly 180 bytes of .bss per song (lib_song_t plus the two index arrays), and .bss is budgeted, so check core/tests/scripts/check_size.sh — or reduce the source tree.`,
			n, maxSongs, maxSongs, n-maxSongs)
	}
	if maxSongs > 0 && n > maxSongs*9/10 {
		fmt.Fprintf(stderr, "warning: %d records is within 10%% of the firmware's LIB_MAX_SONGS (%d)\n", n, maxSongs)
	}
	if a := len(scan.Albums); a > library.MaxAlbums {
		fmt.Fprintf(stderr, "warning: %d albums exceeds the firmware's LIB_MAX_ALBUMS (%d); the device drops the rest silently\n", a, library.MaxAlbums)
	}
	if g := scan.GenreCount(); g > library.MaxGenres {
		fmt.Fprintf(stderr, "warning: %d distinct genres exceeds the firmware's LIB_MAX_GENRES (%d); the device drops the rest silently\n", g, library.MaxGenres)
	}
	return nil
}

func writeAtomically(path string, data []byte) error {
	dir := filepath.Dir(path)
	if dir != "" {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}
	tmp, err := os.CreateTemp(dir, ".corelib-*.idx")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name) // no-op once the rename succeeded
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Chmod(name, 0o644); err != nil {
		return err
	}
	return os.Rename(name, path)
}

// defaultGenreMap locates tools/artist_genres.json by walking up from the
// working directory, the way build_index.py finds it next to itself, so a
// developer editing the map sees the edit at once. Outside the repo it is
// the copy embedded in the binary — never "no map": the first core.exe sync
// on the device wrote 919 empty genres because this used to return "".
func defaultGenreMap() string {
	dir, err := os.Getwd()
	if err != nil {
		return library.GenreMapEmbedded
	}
	for {
		p := filepath.Join(dir, "tools", "artist_genres.json")
		if _, err := os.Stat(p); err == nil {
			return p
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return library.GenreMapEmbedded
		}
		dir = parent
	}
}
