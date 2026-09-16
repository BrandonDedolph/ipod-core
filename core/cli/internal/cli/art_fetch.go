package cli

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
	"github.com/spf13/cobra"
)

// artFetchOptions are the --fetch flags.
type artFetchOptions struct {
	// Write embeds the accepted cover. Without it --fetch is a report:
	// it says what it found and changes nothing.
	Write bool
	// Yes accepts the top candidate without asking — but only when it
	// scored MinScore or better. A weaker match is skipped, never
	// guessed (artfetch, "Confirm first").
	Yes bool
	// DryRun prints what it would do and touches no file, even with
	// --write.
	DryRun bool
	// MinScore is the bar --yes accepts at.
	MinScore float64
}

// newArtFetchClient builds the client the --fetch path uses. It is a
// variable so the tests can hand back one pointed at an httptest
// server; no test in this package makes a real network call.
var newArtFetchClient = func() (*artfetch.Client, error) { return artfetch.New(), nil }

// pictureWriter embeds a front cover into one FLAC, replacing any
// picture already there.
type pictureWriter func(path string, data []byte, mime string) error

// writeFLACPicture is the writing step: flac.WritePicture, which
// replaces any front cover already in the file, leaves the audio frames
// byte-identical and does not preserve the mtime (the sidecar rule
// depends on the change being visible). It is a variable so a test can
// count the calls without writing 25 MB of FLAC.
var writeFLACPicture pictureWriter = func(path string, data []byte, mime string) error {
	return flac.WritePicture(path, flac.Picture{
		Type: flac.PictureTypeFrontCover,
		MIME: mime,
		Data: data,
	})
}

// runArtFetch is `core art --fetch`: for every album whose art source
// carries no front cover, ask the providers, print the candidates with
// their scores, and — with --write — embed the accepted one into every
// FLAC of the album, drop a cover.jpg beside them and re-render the
// sidecars.
//
// Every album is attempted; one that fails does not stop the rest, and
// the exit status reports whether any did.
func runArtFetch(cmd *cobra.Command, dirs []string, o artFetchOptions) error {
	w := cmd.OutOrStdout()
	client, err := newArtFetchClient()
	if err != nil {
		return err
	}
	client.OnWarn = func(provider string, err error) {
		fmt.Fprintf(w, "  ! %s: %v\n", provider, err)
	}

	var have, found, wrote, none, skipped, failed int
	for _, dir := range dirs {
		st, err := os.Stat(dir)
		if err != nil || !st.IsDir() {
			fmt.Fprintf(w, "%s: FAIL (not a folder)\n", dir)
			failed++
			continue
		}
		res, err := fetchOneAlbum(cmd.Context(), cmd, client, dir, o)
		switch {
		case err != nil:
			fmt.Fprintf(w, "%s: FAIL (%v)\n", dir, err)
			failed++
		case res == artFetchSkipped:
			skipped++
		case res == artFetchHasArt:
			have++
		case res == artFetchNoMatch:
			none++
		case res == artFetchWrote:
			found++
			wrote++
		case res == artFetchFound:
			found++
		}
	}
	fmt.Fprintf(w, "%d album(s): %d already had art, %d matched, %d embedded, %d with no match, %d skipped, %d failed\n",
		len(dirs), have, found, wrote, none, skipped, failed)
	if failed > 0 {
		return fmt.Errorf("%d of %d album(s) failed", failed, len(dirs))
	}
	return nil
}

// artFetchResult is what one album did.
type artFetchResult int

const (
	artFetchHasArt artFetchResult = iota
	artFetchSkipped
	artFetchNoMatch
	artFetchFound // a candidate was printed; nothing was written
	artFetchWrote
)

func fetchOneAlbum(ctx context.Context, cmd *cobra.Command, client *artfetch.Client,
	dir string, o artFetchOptions) (artFetchResult, error) {
	w := cmd.OutOrStdout()
	if ctx == nil {
		ctx = context.Background()
	}
	files, err := albumFLACs(dir)
	if err != nil {
		return artFetchSkipped, err
	}
	if len(files) == 0 {
		fmt.Fprintf(w, "%s: skip (no FLAC)\n", dir)
		return artFetchSkipped, nil
	}
	m, err := flac.ReadFile(files[0])
	if err != nil {
		return artFetchSkipped, err
	}
	if m.FrontCover() != nil {
		fmt.Fprintf(w, "%s: has art\n", dir)
		return artFetchHasArt, nil
	}

	q := albumQuery(dir, m)
	fmt.Fprintf(w, "%s: no cover — searching for %s\n", dir, queryText(q))

	cands, err := client.Find(ctx, q)
	if err != nil {
		if errors.Is(err, artfetch.ErrNoMatch) {
			fmt.Fprintf(w, "  no match\n")
			return artFetchNoMatch, nil
		}
		return artFetchSkipped, err
	}
	for i, c := range cands {
		if i >= 5 {
			break
		}
		fmt.Fprintf(w, "  %s\n", c)
	}
	top := cands[0]

	if !o.Write {
		fmt.Fprintf(w, "  → pass --write to embed the top candidate\n")
		return artFetchFound, nil
	}
	accepted, why := acceptCandidate(cmd, top, o)
	if !accepted {
		fmt.Fprintf(w, "  → %s\n", why)
		return artFetchFound, nil
	}
	if o.DryRun {
		fmt.Fprintf(w, "  → would embed %.2f %s into %d file(s) and re-render the sidecars (dry run)\n",
			top.Score, top.Provider, len(files))
		return artFetchFound, nil
	}

	data, mime, err := client.Fetch(ctx, top)
	if err != nil {
		return artFetchSkipped, err
	}
	n, err := embedCover(dir, files, data, mime)
	if err != nil {
		return artFetchSkipped, err
	}
	fmt.Fprintf(w, "  → embedded %.2f %s (%s, %d bytes) into %d file(s), %s, folder.art + folder.thm\n",
		top.Score, top.Provider, mime, len(data), n, coverName(mime))
	return artFetchWrote, nil
}

// acceptCandidate applies the confirm-first rule: --yes takes the top
// candidate when it is at or above --min-score and refuses it below;
// without --yes the user is asked, once per album.
func acceptCandidate(cmd *cobra.Command, top artfetch.Candidate, o artFetchOptions) (bool, string) {
	if o.Yes {
		if top.Score+1e-9 >= o.MinScore {
			return true, ""
		}
		return false, fmt.Sprintf("%.2f is below --min-score %.2f; skipped (accept it by hand without --yes)",
			top.Score, o.MinScore)
	}
	fmt.Fprintf(cmd.OutOrStdout(), "  embed this cover? [y/N] ")
	line, err := bufio.NewReader(cmd.InOrStdin()).ReadString('\n')
	if err != nil && err != io.EOF {
		return false, fmt.Sprintf("could not read the answer: %v", err)
	}
	switch strings.ToLower(strings.TrimSpace(line)) {
	case "y", "yes":
		return true, ""
	}
	return false, "skipped"
}

// embedCover writes the picture into every FLAC of the album, drops
// cover.jpg (or cover.png) beside them and re-renders the two sidecars.
//
// Every file, not just the one the index reads: the device takes its
// art from the album's first track, but a user who copies one file
// somewhere else expects its cover to come along.
func embedCover(dir string, files []string, data []byte, mime string) (int, error) {
	// The art source — the album's first file, the one "does this album
	// need art?" is decided by — is written LAST. A failure part way
	// through then leaves that file coverless, so the next run picks
	// the album up again instead of reporting "has art" over an album
	// where only some files got one.
	order := append(append([]string{}, files[1:]...), files[0])
	for i, f := range order {
		if err := writeFLACPicture(f, data, mime); err != nil {
			return i, fmt.Errorf("%s: %w (%d of %d file(s) were written; run --fetch --write again)",
				filepath.Base(f), err, i, len(files))
		}
	}
	if err := os.WriteFile(filepath.Join(dir, coverName(mime)), data, 0o644); err != nil {
		return len(files), err
	}
	// The sidecars come from the picture that was just embedded, so
	// the album is ready to sync without a second command.
	meta := &flac.Meta{Pictures: []flac.Picture{
		{Type: flac.PictureTypeFrontCover, MIME: mime, Data: data},
	}}
	if _, err := coreart.WriteAlbum(dir, meta); err != nil {
		return len(files), err
	}
	return len(files), nil
}

func coverName(mime string) string {
	if strings.EqualFold(mime, "image/png") {
		return "cover.png"
	}
	return "cover.jpg"
}

// albumQuery decides what to search for. The tags are asked first —
// they are the truth the library manager trusts everywhere else — and
// the folder name is the fallback, split on its LAST " - " the way
// library.SplitAlbumArtist does, because the source convention is
// "Album - Artist".
func albumQuery(dir string, m *flac.Meta) artfetch.Query {
	album := strings.TrimSpace(m.Tag("album"))
	artist := strings.TrimSpace(m.Tag("albumartist", "album artist", "artist"))
	if album == "" || artist == "" {
		fArtist, fAlbum := library.SplitAlbumArtist(filepath.Base(dir))
		if album == "" {
			album = fAlbum
		}
		if artist == "" {
			artist = fArtist
		}
	}
	return artfetch.Query{Artist: artist, Album: album}
}

func queryText(q artfetch.Query) string {
	if q.Artist == "" {
		return fmt.Sprintf("%q", q.Album)
	}
	return fmt.Sprintf("%q by %q", q.Album, q.Artist)
}

// albumFLACs lists an album's FLACs: the folder's own, then each
// "Disc N" subfolder's, each group sorted the way library.ScanTree
// enumerates them. A multi-disc album keeps its art in the folder that
// holds folder.art, so the first file of disc 1 is the art source.
func albumFLACs(dir string) ([]string, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var files, discs []string
	for _, e := range ents {
		n := e.Name()
		if e.IsDir() {
			if strings.HasPrefix(strings.ToLower(n), "disc") {
				discs = append(discs, filepath.Join(dir, n))
			}
			continue
		}
		if strings.HasSuffix(n, ".flac") || strings.HasSuffix(n, ".FLAC") {
			files = append(files, filepath.Join(dir, n))
		}
	}
	sort.Strings(files)
	sort.Strings(discs)
	for _, d := range discs {
		sub, err := os.ReadDir(d)
		if err != nil {
			return nil, err
		}
		var in []string
		for _, e := range sub {
			n := e.Name()
			if !e.IsDir() && (strings.HasSuffix(n, ".flac") || strings.HasSuffix(n, ".FLAC")) {
				in = append(in, filepath.Join(d, n))
			}
		}
		sort.Strings(in)
		files = append(files, in...)
	}
	return files, nil
}
