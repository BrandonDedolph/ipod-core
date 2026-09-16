package cli

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/spf13/cobra"
)

// newArtCmd builds "core art", the Go replacement for tools/coreart.py.
//
// Three shapes, matching the reference script's three modes:
//
//	core art <in.flac> <out.art> [--size N]   one file  -> one sidecar
//	core art --album <folder>                 folder.art + folder.thm in <folder>
//	core art --batch <root>                   the same for every subfolder
//
// The one difference from coreart.py: its --album took an explicit output path
// and wrote a single size, while its --thumb wrote the pair into the folder.
// Since the firmware always wants the pair, --album here is the script's
// --thumb, and the single-file form covers the one-off case.
func newArtCmd() *cobra.Command {
	var (
		album   string
		batch   string
		size    int
		artSize int
		thmSize int
		fetch   artFetchOptions
		doFetch bool
	)
	cmd := &cobra.Command{
		Use:   "art [in.flac out.art]",
		Short: "Bake embedded FLAC cover art into CoreArt RGB565 sidecars",
		Long: `Renders an album's embedded cover into the two sidecars the firmware
reads: folder.art (120x120) and folder.thm (28x28), both CoreArt
containers holding raw little-endian RGB565.

The device has no room to decode a JPEG while it decodes audio, so the
cover is pre-rendered here: decode, Lanczos3 stretch to the exact square
(no crop), round to 5/6/5, prepend the 12-byte "CART" header. Both sizes
come from the source picture independently — the thumbnail is not a
downscale of the 120 — and the 28 must stay exactly 28 so the device
copies it 1:1 instead of resampling it under the scroll wheel.

Forms:
  core art <in.flac> <out.art> [--size N]
      One FLAC's cover to one sidecar. Default size 120.

  core art --album <folder> [--art-size N] [--thumb-size N]
      Take the folder's first FLAC and write folder.art + folder.thm
      into that same folder.

  core art --batch <root> [--art-size N] [--thumb-size N]
      Do that for every immediate subfolder of <root>. Prints one line
      per album and a count; exits non-zero if any album failed. An
      album with no FLAC, or whose first FLAC has no embedded picture,
      is reported as skipped, not failed.

  core art --fetch <album folder>|--batch <root> [--write] [--yes]
           [--dry-run] [--min-score 0.9]
      Find cover art for the albums that have none. Each album whose
      art source carries no front cover is looked up on the iTunes
      Search API and then, if that misses, on MusicBrainz + the Cover
      Art Archive; the candidates are printed with a match score.

      Nothing is written without --write. With --write each album is
      asked about (y/N) unless --yes, which takes the top candidate
      only when it scored --min-score (0.9 by default) or better — the
      score where the artist and the album both match once edition
      decorations like "(Deluxe)" are stripped. An accepted cover is
      embedded in EVERY FLAC of the album, written beside them as
      cover.jpg, and baked into folder.art + folder.thm. --dry-run
      prints the decision and touches nothing.

      The images belong to their rights-holders: they are written to
      your own files and your own cache (<user cache dir>/core/art)
      and nowhere else.`,
		Args:         cobra.ArbitraryArgs,
		SilenceUsage: true,
		RunE: func(cmd *cobra.Command, args []string) error {
			switch {
			case batch != "" && album != "":
				return errors.New("--batch and --album are alternatives; pass one")
			case doFetch:
				dirs, err := artFetchDirs(args, album, batch)
				if err != nil {
					return err
				}
				return runArtFetch(cmd, dirs, fetch)
			case batch != "":
				if len(args) != 0 {
					return fmt.Errorf("--batch takes no positional arguments (got %d)", len(args))
				}
				return runArtBatch(cmd, batch, artSize, thmSize)
			case album != "":
				if len(args) != 0 {
					return fmt.Errorf("--album takes no positional arguments (got %d)", len(args))
				}
				return runArtAlbum(cmd, album, artSize, thmSize)
			default:
				if len(args) != 2 {
					return errors.New("need <in.flac> <out.art>, or --album <folder>, or --batch <root>")
				}
				return runArtOne(cmd, args[0], args[1], size)
			}
		},
	}
	cmd.Flags().StringVar(&album, "album", "", "Album folder: write folder.art + folder.thm from its first FLAC")
	cmd.Flags().StringVar(&batch, "batch", "", "Root folder: do --album for every immediate subfolder")
	cmd.Flags().IntVar(&size, "size", coreart.ArtSize, "Square size for the single-file form")
	cmd.Flags().IntVar(&artSize, "art-size", coreart.ArtSize, "Square size for folder.art")
	cmd.Flags().IntVar(&thmSize, "thumb-size", coreart.ThumbSize, "Square size for folder.thm (28 = the firmware's ARTCACHE_DIM; anything else is resampled on the device)")
	cmd.Flags().BoolVar(&doFetch, "fetch", false, "Look up cover art for albums that have none (iTunes, then MusicBrainz + Cover Art Archive)")
	cmd.Flags().BoolVar(&fetch.Write, "write", false, "--fetch: embed the accepted cover into every FLAC of the album and re-render the sidecars")
	cmd.Flags().BoolVar(&fetch.Yes, "yes", false, "--fetch --write: accept the top candidate without asking, when it scores --min-score or better")
	cmd.Flags().BoolVar(&fetch.DryRun, "dry-run", false, "--fetch: print the decision and write nothing")
	cmd.Flags().Float64Var(&fetch.MinScore, "min-score", 0.9, "--fetch --yes: the match score at which a candidate is accepted unasked")
	return cmd
}

// checkDim rejects a size the firmware could never load, before any work.
func checkDim(what string, n int) error {
	if n <= 0 || n > coreart.MaxDim {
		return fmt.Errorf("%s %d is outside 1..%d (the firmware's ARTCACHE_MAX_DIM)", what, n, coreart.MaxDim)
	}
	return nil
}

func runArtOne(cmd *cobra.Command, in, out string, size int) error {
	if err := checkDim("--size", size); err != nil {
		return err
	}
	m, err := flac.ReadFile(in)
	if err != nil {
		return err
	}
	pic := m.FrontCover()
	if pic == nil {
		return fmt.Errorf("%s: no embedded picture", in)
	}
	img, err := coreart.FromPicture(pic)
	if err != nil {
		return fmt.Errorf("%s: %w", in, err)
	}
	b := coreart.Render(img, size)
	if err := os.WriteFile(out, b, 0o644); err != nil {
		return err
	}
	fmt.Fprintf(cmd.OutOrStdout(), "%s: %dx%d (%d bytes, from %dx%d %s)\n",
		out, size, size, len(b), img.Bounds().Dx(), img.Bounds().Dy(), pic.MIME)
	return nil
}

// artAlbumOutcome is what one album did, so --batch can count without parsing
// its own output.
type artAlbumOutcome int

const (
	artWrote artAlbumOutcome = iota
	artSkipped
	artFailed
)

// runArtAlbum is the --album form; it prints one line and returns an error on
// a real failure (skips are not errors).
func runArtAlbum(cmd *cobra.Command, dir string, artSize, thmSize int) error {
	if err := checkDim("--art-size", artSize); err != nil {
		return err
	}
	if err := checkDim("--thumb-size", thmSize); err != nil {
		return err
	}
	outcome, err := artOneAlbum(cmd, dir, artSize, thmSize)
	if outcome == artFailed {
		return err
	}
	return nil
}

func artOneAlbum(cmd *cobra.Command, dir string, artSize, thmSize int) (artAlbumOutcome, error) {
	w := cmd.OutOrStdout()
	src, err := coreart.FirstFLAC(dir)
	if err != nil {
		fmt.Fprintf(w, "%s: FAIL (%v)\n", dir, err)
		return artFailed, err
	}
	if src == "" {
		fmt.Fprintf(w, "%s: skip (no FLAC)\n", dir)
		return artSkipped, nil
	}
	m, err := flac.ReadFile(src)
	if err != nil {
		fmt.Fprintf(w, "%s: FAIL (%v)\n", dir, err)
		return artFailed, err
	}
	res, err := writeSizedAlbum(dir, m, artSize, thmSize)
	if err != nil {
		fmt.Fprintf(w, "%s: FAIL (%v)\n", dir, err)
		return artFailed, err
	}
	if res.NoPicture {
		fmt.Fprintf(w, "%s: skip (no embedded art in %s)\n", dir, filepath.Base(src))
		return artSkipped, nil
	}
	fmt.Fprintf(w, "%s: ok (%s %dx%d -> folder.art %dx%d + folder.thm %dx%d)\n",
		dir, filepath.Base(src), res.SrcW, res.SrcH,
		res.ArtDim, res.ArtDim, res.ThumbDim, res.ThumbDim)
	return artWrote, nil
}

// writeSizedAlbum is coreart.WriteAlbum when the sizes are the firmware's, and
// the same work at other sizes when the user overrode them. The override path
// exists for experiments only; the device reads 120 and 28.
func writeSizedAlbum(dir string, m *flac.Meta, artSize, thmSize int) (coreart.Result, error) {
	if artSize == coreart.ArtSize && thmSize == coreart.ThumbSize {
		return coreart.WriteAlbum(dir, m)
	}
	res := coreart.Result{Dir: dir}
	pic := m.FrontCover()
	if pic == nil {
		res.NoPicture = true
		return res, nil
	}
	img, err := coreart.FromPicture(pic)
	if err != nil {
		return res, err
	}
	res.SrcW, res.SrcH = img.Bounds().Dx(), img.Bounds().Dy()
	for _, s := range []struct {
		name string
		dim  int
		dst  *string
		out  *int
	}{
		{coreart.ArtName, artSize, &res.ArtPath, &res.ArtDim},
		{coreart.ThumbName, thmSize, &res.ThumbPath, &res.ThumbDim},
	} {
		p := filepath.Join(dir, s.name)
		if err := os.WriteFile(p, coreart.Render(img, s.dim), 0o644); err != nil {
			return res, err
		}
		*s.dst, *s.out = p, s.dim
	}
	return res, nil
}

func runArtBatch(cmd *cobra.Command, root string, artSize, thmSize int) error {
	if err := checkDim("--art-size", artSize); err != nil {
		return err
	}
	if err := checkDim("--thumb-size", thmSize); err != nil {
		return err
	}
	ents, err := os.ReadDir(root)
	if err != nil {
		return err
	}
	var dirs []string
	for _, e := range ents {
		if e.IsDir() {
			dirs = append(dirs, filepath.Join(root, e.Name()))
		}
	}
	sort.Strings(dirs)

	var wrote, skipped, failed int
	for _, d := range dirs {
		switch out, _ := artOneAlbum(cmd, d, artSize, thmSize); out {
		case artWrote:
			wrote++
		case artSkipped:
			skipped++
		default:
			failed++
		}
	}
	fmt.Fprintf(cmd.OutOrStdout(), "%s: %d/%d folder(s) got art, %d skipped, %d failed\n",
		root, wrote, len(dirs), skipped, failed)
	if failed > 0 {
		return fmt.Errorf("%d of %d album(s) failed", failed, len(dirs))
	}
	return nil
}

// artFetchDirs works out which album folders --fetch was aimed at:
// --batch's immediate subfolders, or the one folder named by --album or
// by a single positional argument.
func artFetchDirs(args []string, album, batch string) ([]string, error) {
	switch {
	case batch != "":
		if len(args) != 0 {
			return nil, fmt.Errorf("--batch takes no positional arguments (got %d)", len(args))
		}
		ents, err := os.ReadDir(batch)
		if err != nil {
			return nil, err
		}
		var dirs []string
		for _, e := range ents {
			if e.IsDir() {
				dirs = append(dirs, filepath.Join(batch, e.Name()))
			}
		}
		sort.Strings(dirs)
		return dirs, nil
	case album != "":
		if len(args) != 0 {
			return nil, fmt.Errorf("--album takes no positional arguments (got %d)", len(args))
		}
		return []string{album}, nil
	case len(args) == 1:
		return []string{args[0]}, nil
	}
	return nil, errors.New("--fetch needs an album folder: pass one, or --album <folder>, or --batch <root>")
}
