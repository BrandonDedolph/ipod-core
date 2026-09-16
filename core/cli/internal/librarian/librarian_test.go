package librarian

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// Nothing in this file touches the network: the one provider is pointed at an
// httptest server that also serves the covers.

// --- fixtures ---------------------------------------------------------------

// smallPNG is a picture for a file that already HAS art. It never has to be
// decoded (Inspect only asks whether a front cover is there), but it is a real
// PNG anyway, so a test that starts decoding it does not fail for the wrong
// reason.
func smallPNG(t *testing.T) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, 8, 8))
	for y := 0; y < 8; y++ {
		for x := 0; x < 8; x++ {
			img.Set(x, y, color.RGBA{R: uint8(x * 31), G: uint8(y * 31), B: 0x40, A: 0xFF})
		}
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// jpegSquare is a real JPEG past artfetch's 300 px floor, so the validator
// accepts it as a cover.
func jpegSquare(t *testing.T, size int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			img.Set(x, y, color.RGBA{R: uint8(x), G: uint8(y), B: 0x60, A: 255})
		}
	}
	var b bytes.Buffer
	if err := jpeg.Encode(&b, img, &jpeg.Options{Quality: 80}); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func writeTrack(t *testing.T, dir, name string, tags map[string]string, cover []byte) string {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	var pics []flac.Picture
	if cover != nil {
		pics = []flac.Picture{{Type: flac.PictureTypeFrontCover, MIME: "image/png", Data: cover}}
	}
	raw := flac.BuildFile(
		flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
		tags, pics)
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

// fixture is one tree with all three problems in it plus a file nobody can
// name:
//
//	Canon - Artist A/      already canonical, NO cover      -> missing art
//	Messy - Artist B/      right folder, junk filename      -> misnamed
//	Downloads/Wild - .../  right names, wrong place, NO cover -> unorganized + art
//	Broken - Artist D/     a file with no title tag         -> needs attention
func fixture(t *testing.T) string {
	t.Helper()
	src := t.TempDir()
	cover := smallPNG(t)

	canon := filepath.Join(src, "Canon - Artist A")
	writeTrack(t, canon, "01 - Artist A - One.flac", map[string]string{
		"album": "Canon", "artist": "Artist A", "title": "One", "tracknumber": "1"}, nil)
	writeTrack(t, canon, "02 - Artist A - Two.flac", map[string]string{
		"album": "Canon", "artist": "Artist A", "title": "Two", "tracknumber": "2"}, nil)

	messy := filepath.Join(src, "Messy - Artist B")
	writeTrack(t, messy, "Track 3.flac", map[string]string{
		"album": "Messy", "artist": "Artist B", "title": "Three", "tracknumber": "3"}, cover)

	wild := filepath.Join(src, "Downloads", "Wild - Artist C")
	writeTrack(t, wild, "whatever.flac", map[string]string{
		"album": "Wild", "artist": "Artist C", "title": "Song", "tracknumber": "1"}, nil)

	broken := filepath.Join(src, "Broken - Artist D")
	writeTrack(t, broken, "mystery.flac", map[string]string{
		"album": "Broken", "artist": "Artist D", "tracknumber": "1"}, cover)

	return src
}

// --- the fake art service ----------------------------------------------------

// fakeArt is one httptest server standing in for the iTunes Search API and for
// the artwork host. The album named in badAlbum is found but its picture
// always 500s, which is how the "one album fails, the others are still done"
// test gets a failure out of the real artfetch.Client.
type fakeArt struct {
	*httptest.Server
	badAlbum string
	cover    []byte
	searches int
	fetches  int
}

func newFakeArt(t *testing.T, badAlbum string) *fakeArt {
	t.Helper()
	s := &fakeArt{cover: jpegSquare(t, 400), badAlbum: badAlbum}
	mux := http.NewServeMux()
	mux.HandleFunc("/search", func(w http.ResponseWriter, r *http.Request) {
		s.searches++
		term := r.URL.Query().Get("term")
		// The fixture's albums, keyed by the words the query carries.
		for _, a := range []struct{ artist, album string }{
			{"Artist A", "Canon"},
			{"Artist C", "Wild"},
		} {
			if strings.Contains(term, a.album) && strings.Contains(term, a.artist) {
				fmt.Fprintf(w, `{"resultCount":1,"results":[{"collectionId":7,`+
					`"artistName":%q,"collectionName":%q,`+
					`"artworkUrl100":"%s/img/%s/100x100bb.jpg"}]}`,
					a.artist, a.album, s.URL, a.album)
				return
			}
		}
		w.Write([]byte(`{"resultCount":0,"results":[]}`))
	})
	mux.HandleFunc("/img/", func(w http.ResponseWriter, r *http.Request) {
		s.fetches++
		if s.badAlbum != "" && strings.Contains(r.URL.Path, "/"+s.badAlbum+"/") {
			http.Error(w, "the artwork host is having a day", http.StatusInternalServerError)
			return
		}
		if !strings.HasSuffix(r.URL.Path, "/1200x1200bb.jpg") {
			http.NotFound(w, r) // only the rewritten URL exists
			return
		}
		w.Write(s.cover)
	})
	s.Server = httptest.NewServer(mux)
	t.Cleanup(s.Close)
	return s
}

// client is the real artfetch.Client with one provider, pointed at the fake.
// No cache: a cache directory shared between tests would turn "did it ask?"
// into a coin toss.
func (s *fakeArt) client() *artfetch.Client {
	return &artfetch.Client{
		Providers:   []artfetch.Provider{&artfetch.ITunes{BaseURL: s.URL}},
		UserAgent:   artfetch.UserAgent(),
		StopAtScore: 1.0,
	}
}

// --- helpers ----------------------------------------------------------------

func opts(t *testing.T) Options {
	t.Helper()
	return Options{JournalDir: t.TempDir()}
}

// tree is every file under root with its content hash — the comparison a
// "wrote nothing" test needs.
func tree(t *testing.T, root string) map[string]string {
	t.Helper()
	out := map[string]string{}
	err := filepath.Walk(root, func(p string, fi os.FileInfo, err error) error {
		if err != nil || fi.IsDir() {
			return err
		}
		b, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		sum := sha256.Sum256(b)
		rel, _ := filepath.Rel(root, p)
		out[filepath.ToSlash(rel)] = hex.EncodeToString(sum[:8])
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func hasCover(t *testing.T, path string) bool {
	t.Helper()
	m, err := flac.ReadFile(path)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	return m.FrontCover() != nil
}

func journalsIn(t *testing.T, dir string) []string {
	t.Helper()
	ents, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	var out []string
	for _, e := range ents {
		if strings.HasSuffix(e.Name(), ".json") {
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	sort.Strings(out)
	return out
}

func keys(r *Report) []string {
	var out []string
	for _, a := range r.MissingArt {
		out = append(out, string(a.Key))
	}
	sort.Strings(out)
	return out
}

// --- tests ------------------------------------------------------------------

// TestInspectFindsEveryProblemOnce is the report the Library tab draws: three
// kinds of problem, the files nobody can name, and the cost of acting on it.
func TestInspectFindsEveryProblemOnce(t *testing.T) {
	src := fixture(t)
	before := tree(t, src)

	rep, err := Inspect(context.Background(), src, opts(t))
	if err != nil {
		t.Fatalf("Inspect: %v", err)
	}

	if got, want := keys(rep), []string{"Canon - Artist A", "Downloads/Wild - Artist C"}; !equal(got, want) {
		t.Errorf("missing art = %q, want %q", got, want)
	}
	for _, a := range rep.MissingArt {
		switch a.Key {
		case "Canon - Artist A":
			if a.Tracks != 2 || filepath.Base(a.FirstFLAC) != "01 - Artist A - One.flac" {
				t.Errorf("%s: %d track(s), art source %s", a.Key, a.Tracks, a.FirstFLAC)
			}
			if a.Artist != "Artist A" || a.Album != "Canon" {
				t.Errorf("%s: query is %q — %q", a.Key, a.Artist, a.Album)
			}
		case "Downloads/Wild - Artist C":
			if a.Tracks != 1 || filepath.Base(a.FirstFLAC) != "whatever.flac" {
				t.Errorf("%s: %d track(s), art source %s", a.Key, a.Tracks, a.FirstFLAC)
			}
		}
	}

	if len(rep.Misnamed) != 1 || filepath.Base(rep.Misnamed[0].From) != "Track 3.flac" ||
		filepath.Base(rep.Misnamed[0].To) != "03 - Artist B - Three.flac" {
		t.Errorf("misnamed = %+v", rep.Misnamed)
	}
	if len(rep.Unorganized) != 1 || filepath.Base(rep.Unorganized[0].From) != "whatever.flac" {
		t.Errorf("unorganized = %+v", rep.Unorganized)
	}
	if want := filepath.Join(src, "Wild - Artist C", "01 - Artist C - Song.flac"); rep.Unorganized[0].To != want {
		t.Errorf("unorganized target = %s, want %s", rep.Unorganized[0].To, want)
	}
	if len(rep.NeedsAttention) != 1 || !strings.Contains(rep.NeedsAttention[0].Reason, "title") {
		t.Errorf("needs attention = %+v", rep.NeedsAttention)
	}
	if rep.Albums != 4 || rep.Tracks != 5 {
		t.Errorf("tree = %d album(s) / %d track(s), want 4/5", rep.Albums, rep.Tracks)
	}

	// The cost line: the renamed track is a re-copy, and the one the library
	// cannot see today is a first copy.
	if rep.RecopyTracks != 1 || rep.NewTracks != 1 {
		t.Errorf("cost = %d re-copied / %d new, want 1/1", rep.RecopyTracks, rep.NewTracks)
	}
	// The two halves of the cost are counted apart: the renamed track is
	// already on the iPod (a re-copy), the nested one never was.
	if rep.RecopyBytes <= 0 || rep.NewBytes <= 0 {
		t.Errorf("cost in bytes = %d re-copied / %d new, want both above zero",
			rep.RecopyBytes, rep.NewBytes)
	}
	if rep.RecopyBytes != rep.Misnamed[0].Bytes || rep.NewBytes != rep.Unorganized[0].Bytes {
		t.Errorf("the bytes are on the wrong side: %d/%d against moves of %d/%d",
			rep.RecopyBytes, rep.NewBytes, rep.Misnamed[0].Bytes, rep.Unorganized[0].Bytes)
	}

	// Inspect writes nothing.
	if after := tree(t, src); !sameTree(before, after) {
		t.Errorf("Inspect changed the tree:\n%v\n%v", before, after)
	}
	// And a report can be shown after a JSON round trip, even though it can
	// no longer be applied (Moves is not serialised).
	if _, err := json.Marshal(rep); err != nil {
		t.Fatalf("the report does not serialise: %v", err)
	}
}

// TestInspectOnACleanTreeIsEmpty: the report has to be able to say "nothing".
func TestInspectOnACleanTreeIsEmpty(t *testing.T) {
	src := t.TempDir()
	writeTrack(t, filepath.Join(src, "Canon - Artist A"), "01 - Artist A - One.flac",
		map[string]string{"album": "Canon", "artist": "Artist A", "title": "One", "tracknumber": "1"},
		smallPNG(t))
	rep, err := Inspect(context.Background(), src, opts(t))
	if err != nil {
		t.Fatalf("Inspect: %v", err)
	}
	if !rep.Empty() {
		t.Errorf("a tidy tree reported %+v", rep)
	}
}

// TestCandidatesAsksOnlyAboutTheCoverlessAlbums — and scores an exact match at
// 1.00, which is what --yes acts on.
func TestCandidatesAsksOnlyAboutTheCoverlessAlbums(t *testing.T) {
	src := fixture(t)
	srv := newFakeArt(t, "")
	rep, err := Inspect(context.Background(), src, opts(t))
	if err != nil {
		t.Fatal(err)
	}
	cands, err := Candidates(context.Background(), rep, srv.client(), opts(t))
	if err != nil {
		t.Fatalf("Candidates: %v", err)
	}
	if len(cands) != 2 {
		t.Fatalf("got candidates for %d album(s), want 2: %v", len(cands), cands)
	}
	if srv.searches != 2 {
		t.Errorf("%d search(es) for 2 coverless albums out of 4", srv.searches)
	}
	for key, list := range cands {
		if len(list) == 0 || list[0].Score < 1.0 {
			t.Errorf("%s: %v", key, list)
		}
	}
	// The accept-all rule turns those into decisions.
	dec := AcceptAbove(cands, DefaultMinScore)
	for key, d := range dec {
		if d.Skip {
			t.Errorf("%s was skipped at score %.2f", key, d.Candidate.Score)
		}
	}
}

// TestFixDoesTheWholeThing is the slice's "done when": inspect → fix → a tree
// that is organized, covered, sidecar'd and undoable.
func TestFixDoesTheWholeThing(t *testing.T) {
	src := fixture(t)
	srv := newFakeArt(t, "")
	o := opts(t)
	o.Art = srv.client()

	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	cands, err := Candidates(context.Background(), rep, o.Art, o)
	if err != nil {
		t.Fatal(err)
	}
	res, err := Fix(context.Background(), rep, Choices{
		FixNames: true, Organize: true, Art: AcceptAbove(cands, DefaultMinScore),
	}, o)
	if err != nil {
		t.Fatalf("Fix: %v", err)
	}

	if res.Renamed != 1 || res.Moved != 1 || res.ArtWritten != 2 {
		t.Errorf("result = %d renamed / %d moved / %d album(s) of art, want 1/1/2", res.Renamed, res.Moved, res.ArtWritten)
	}
	if len(res.Failures) != 0 {
		t.Errorf("failures: %+v", res.Failures)
	}
	if js := journalsIn(t, o.JournalDir); len(js) != 1 {
		t.Errorf("the job wrote %d journal(s), want exactly 1: %q", len(js), js)
	} else if js[0] != res.Journal {
		t.Errorf("Result.Journal = %s, the file is %s", res.Journal, js[0])
	}

	// The tree is organized.
	for _, want := range []string{
		"Canon - Artist A/01 - Artist A - One.flac",
		"Canon - Artist A/02 - Artist A - Two.flac",
		"Messy - Artist B/03 - Artist B - Three.flac",
		"Wild - Artist C/01 - Artist C - Song.flac",
	} {
		if _, err := os.Stat(filepath.Join(src, filepath.FromSlash(want))); err != nil {
			t.Errorf("%s is not there: %v", want, err)
		}
	}
	if _, err := os.Stat(filepath.Join(src, "Downloads", "Wild - Artist C")); err == nil {
		t.Errorf("the emptied source folder is still there")
	}

	// Every track of both albums carries the cover, not just the art source.
	for _, want := range []string{
		"Canon - Artist A/01 - Artist A - One.flac",
		"Canon - Artist A/02 - Artist A - Two.flac",
		"Wild - Artist C/01 - Artist C - Song.flac",
	} {
		if !hasCover(t, filepath.Join(src, filepath.FromSlash(want))) {
			t.Errorf("%s has no embedded cover", want)
		}
	}

	// The sidecars are valid at the two dimensions the firmware reads, and
	// they are in the folder the album ENDED UP in.
	for _, dir := range []string{"Canon - Artist A", "Wild - Artist C"} {
		d := filepath.Join(src, dir)
		mustSidecar(t, filepath.Join(d, coreart.ArtName), coreart.ArtSize)
		mustSidecar(t, filepath.Join(d, coreart.ThumbName), coreart.ThumbSize)
		if _, err := os.Stat(filepath.Join(d, "cover.jpg")); err != nil {
			t.Errorf("%s: no cover.jpg: %v", dir, err)
		}
	}

	// The load-bearing claim: the organized tree scans to the locator the
	// index is bound to.
	scan, err := library.ScanTree(src, library.Options{})
	if err != nil {
		t.Fatalf("ScanTree: %v", err)
	}
	for _, a := range scan.Albums {
		for _, tr := range a.Tracks {
			if tr.NumberFrom != "tag" {
				t.Errorf("%s: numbered from %s", tr.DeviceName, tr.NumberFrom)
			}
			want := fmt.Sprintf("%02d. %s.flac", tr.Pos, library.FatSafe(tr.Title))
			if tr.DeviceName != want {
				t.Errorf("device name %q, want %q", tr.DeviceName, want)
			}
		}
	}
}

// TestFixCarriesOnWhenOneAlbumsArtFails: the renames are already journaled and
// the other album's cover is already fetched; a 500 from an artwork host is
// not a reason to abandon either.
func TestFixCarriesOnWhenOneAlbumsArtFails(t *testing.T) {
	src := fixture(t)
	srv := newFakeArt(t, "Canon") // the Canon cover always 500s
	o := opts(t)
	o.Art = srv.client()

	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	cands, err := Candidates(context.Background(), rep, o.Art, o)
	if err != nil {
		t.Fatal(err)
	}
	res, err := Fix(context.Background(), rep, Choices{
		FixNames: true, Organize: true, Art: AcceptAbove(cands, DefaultMinScore),
	}, o)
	if err != nil {
		t.Fatalf("Fix returned an error for one album's art: %v", err)
	}

	if res.ArtWritten != 1 {
		t.Errorf("ArtWritten = %d, want 1 (the album that worked)", res.ArtWritten)
	}
	if len(res.Failures) != 1 || res.Failures[0].Album != "Canon - Artist A" {
		t.Fatalf("failures = %+v, want exactly the Canon album", res.Failures)
	}
	if !strings.Contains(res.Failures[0].Reason, "500") {
		t.Errorf("the failure does not say what went wrong: %q", res.Failures[0].Reason)
	}
	// The other album is done...
	if !hasCover(t, filepath.Join(src, "Wild - Artist C", "01 - Artist C - Song.flac")) {
		t.Error("the album whose fetch worked has no cover")
	}
	// ...the failed one is untouched, so the next inspect picks it up again...
	if hasCover(t, filepath.Join(src, "Canon - Artist A", "01 - Artist A - One.flac")) {
		t.Error("the album whose fetch failed has a cover anyway")
	}
	// ...and the renames happened.
	if res.Renamed != 1 || res.Moved != 1 || res.Journal == "" {
		t.Errorf("the moves did not survive the art failure: %+v", res)
	}
	if _, err := os.Stat(filepath.Join(src, "Messy - Artist B", "03 - Artist B - Three.flac")); err != nil {
		t.Errorf("the rename did not happen: %v", err)
	}
}

// TestFixDryRunWritesNothing — not a rename, not a byte of cover, and not a
// journal.
func TestFixDryRunWritesNothing(t *testing.T) {
	src := fixture(t)
	srv := newFakeArt(t, "")
	o := opts(t)
	o.Art = srv.client()
	o.DryRun = true

	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	cands, err := Candidates(context.Background(), rep, o.Art, o)
	if err != nil {
		t.Fatal(err)
	}
	before := tree(t, src)
	res, err := Fix(context.Background(), rep, Choices{
		FixNames: true, Organize: true, Art: AcceptAbove(cands, DefaultMinScore),
	}, o)
	if err != nil {
		t.Fatalf("dry run: %v", err)
	}
	if after := tree(t, src); !sameTree(before, after) {
		t.Errorf("a dry run changed the tree:\n%v\n%v", before, after)
	}
	if js := journalsIn(t, o.JournalDir); len(js) != 0 {
		t.Errorf("a dry run wrote a journal: %q", js)
	}
	if res.Journal != "" || !res.DryRun {
		t.Errorf("result = %+v, want DryRun with no journal", res)
	}
	// It still says what it would have done.
	if res.Renamed != 1 || res.Moved != 1 || res.ArtWritten != 2 {
		t.Errorf("dry-run counts = %d/%d/%d, want 1/1/2", res.Renamed, res.Moved, res.ArtWritten)
	}
	if srv.fetches != 0 {
		t.Errorf("a dry run downloaded %d image(s)", srv.fetches)
	}
}

// TestUndoPutsTheNamesBackAndLeavesTheArt is the decision in plan §5, made
// visible: the journal is names and folders, and the cover stays.
//
// It is also the test that pins the ORDER inside Fix. The journal records each
// file's size and mtime as it moves it, and Undo refuses a file that changed
// since; if the cover went in after the rename, every one of these files would
// come back "it changed after the move; left alone".
func TestUndoPutsTheNamesBackAndLeavesTheArt(t *testing.T) {
	src := fixture(t)
	srv := newFakeArt(t, "")
	o := opts(t)
	o.Art = srv.client()

	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	cands, err := Candidates(context.Background(), rep, o.Art, o)
	if err != nil {
		t.Fatal(err)
	}
	res, err := Fix(context.Background(), rep, Choices{
		FixNames: true, Organize: true, Art: AcceptAbove(cands, DefaultMinScore),
	}, o)
	if err != nil {
		t.Fatal(err)
	}

	rep2, err := Undo(context.Background(), res.Journal, o)
	if err != nil {
		t.Fatalf("Undo: %v", err)
	}
	if len(rep2.Skipped) != 0 {
		t.Errorf("Undo refused %d op(s): %+v", len(rep2.Skipped), rep2.Skipped)
	}
	for _, want := range []string{
		"Messy - Artist B/Track 3.flac",
		"Downloads/Wild - Artist C/whatever.flac",
	} {
		if _, err := os.Stat(filepath.Join(src, filepath.FromSlash(want))); err != nil {
			t.Errorf("%s did not come back: %v", want, err)
		}
	}
	// The folder the organize created does NOT come out again, because the
	// sidecars are in it and they are not the journal's to delete. That is
	// the same decision as the line below, seen from the other side.
	for _, name := range []string{coreart.ArtName, "cover.jpg"} {
		if _, err := os.Stat(filepath.Join(src, "Wild - Artist C", name)); err != nil {
			t.Errorf("the undo took %s with it: %v", name, err)
		}
	}
	if _, err := os.Stat(filepath.Join(src, "Wild - Artist C", "01 - Artist C - Song.flac")); err == nil {
		t.Error("the track did not come back out of the folder the organize made")
	}
	// The art is NOT undone, in the file and beside it.
	if !hasCover(t, filepath.Join(src, "Downloads", "Wild - Artist C", "whatever.flac")) {
		t.Error("the cover was removed by an undo; it is not the journal's to remove")
	}
	if _, err := os.Stat(filepath.Join(src, "Canon - Artist A", "cover.jpg")); err != nil {
		t.Errorf("cover.jpg went away with the undo: %v", err)
	}
}

// TestFixNamesOnlyLeavesTheFoldersAlone: the two tick boxes are independent,
// and unticking one must not drag the other along.
func TestFixNamesOnlyLeavesTheFoldersAlone(t *testing.T) {
	src := fixture(t)
	o := opts(t)
	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	res, err := Fix(context.Background(), rep, Choices{FixNames: true}, o)
	if err != nil {
		t.Fatalf("Fix: %v", err)
	}
	if res.Renamed != 1 || res.Moved != 0 {
		t.Errorf("result = %d renamed / %d moved, want 1/0", res.Renamed, res.Moved)
	}
	if _, err := os.Stat(filepath.Join(src, "Messy - Artist B", "03 - Artist B - Three.flac")); err != nil {
		t.Errorf("the rename did not happen: %v", err)
	}
	if _, err := os.Stat(filepath.Join(src, "Downloads", "Wild - Artist C", "whatever.flac")); err != nil {
		t.Errorf("the unticked album moved anyway: %v", err)
	}
	if res.ArtWritten != 0 || res.Skipped != 2 {
		t.Errorf("no art was accepted, yet ArtWritten=%d Skipped=%d", res.ArtWritten, res.Skipped)
	}
}

// TestFixNeedsAClientForAcceptedArt: a decision that cannot be carried out is
// an error before anything is written, not a silent skip.
func TestFixNeedsAClientForAcceptedArt(t *testing.T) {
	src := fixture(t)
	o := opts(t)
	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	before := tree(t, src)
	_, err = Fix(context.Background(), rep, Choices{
		Art: map[AlbumKey]Decision{"Canon - Artist A": Accept(artfetch.Candidate{FullURL: "x"})},
	}, o)
	if err == nil {
		t.Fatal("Fix accepted art with no client")
	}
	if after := tree(t, src); !sameTree(before, after) {
		t.Error("the failed Fix changed the tree")
	}
}

// TestSelectMovesDefersWhatItCannotDo: a ticked rename whose target is still
// held by a file whose own move was NOT ticked would stop organizer.Apply at
// the first op. It is deferred instead, and said out loud.
func TestSelectMovesDefersWhatItCannotDo(t *testing.T) {
	root := filepath.FromSlash("/music")
	p := func(s string) string { return filepath.Join(root, filepath.FromSlash(s)) }
	r := &Report{Root: root, Moves: []organizer.Move{
		// B has to leave its folder before A can take its name.
		{From: p("X - Y/b.flac"), To: p("Z - Y/01 - Y - B.flac"), Reason: "move"},
		{From: p("X - Y/a.flac"), To: p("X - Y/b.flac"), Reason: "rename"},
	}}
	sel, deferred, err := selectMoves(r, Choices{FixNames: true})
	if err != nil {
		t.Fatal(err)
	}
	if len(sel) != 0 {
		t.Errorf("selected %+v, want nothing: the name it wants is still taken", sel)
	}
	if len(deferred) != 1 || deferred[0].From != p("X - Y/a.flac") {
		t.Errorf("deferred = %+v", deferred)
	}
	// With both ticked, both run, in the plan's order.
	sel, deferred, err = selectMoves(r, Choices{FixNames: true, Organize: true})
	if err != nil {
		t.Fatal(err)
	}
	if len(sel) != 2 || len(deferred) != 0 || sel[0].Reason != "move" {
		t.Errorf("sel = %+v deferred = %+v", sel, deferred)
	}
}

// TestSelectMovesKeepsACycleWhole: the hop through "<target>.core-tmp" and the
// move it makes room for are one thing, and half of it is a file left under a
// temporary name.
func TestSelectMovesKeepsACycleWhole(t *testing.T) {
	root := filepath.FromSlash("/music")
	p := func(s string) string { return filepath.Join(root, filepath.FromSlash(s)) }
	r := &Report{Root: root, Moves: []organizer.Move{
		{From: p("A - Y/1.flac"), To: p("A - Y/2.flac.core-tmp"), Reason: "cycle"},
		{From: p("A - Y/2.flac"), To: p("A - Y/1.flac"), Reason: "rename"},
		{From: p("A - Y/2.flac.core-tmp"), To: p("A - Y/2.flac"), Reason: "rename"},
	}}
	sel, _, err := selectMoves(r, Choices{FixNames: true})
	if err != nil {
		t.Fatal(err)
	}
	if len(sel) != 3 {
		t.Errorf("a cycle came apart: %+v", sel)
	}
	sel, _, err = selectMoves(r, Choices{Organize: true})
	if err != nil {
		t.Fatal(err)
	}
	if len(sel) != 0 {
		t.Errorf("a rename cycle was taken for a re-folder: %+v", sel)
	}
}

// TestFixRefusesADecodedReport: Misnamed and Unorganized are views of a plan
// whose ORDER is what keeps a rename from landing on a file that has not moved
// out of the way yet. A report that came back from JSON has lost it, and
// half-applying it is worse than saying so.
func TestFixRefusesADecodedReport(t *testing.T) {
	src := fixture(t)
	o := opts(t)
	rep, err := Inspect(context.Background(), src, o)
	if err != nil {
		t.Fatal(err)
	}
	b, err := json.Marshal(rep)
	if err != nil {
		t.Fatal(err)
	}
	var decoded Report
	if err := json.Unmarshal(b, &decoded); err != nil {
		t.Fatal(err)
	}
	if _, err := Fix(context.Background(), &decoded, Choices{FixNames: true}, o); err == nil {
		t.Fatal("Fix applied a report with no apply order")
	}
}

// --- small helpers ----------------------------------------------------------

func mustSidecar(t *testing.T, path string, dim int) {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	if !coreart.Valid(b, dim) {
		t.Errorf("%s is not a valid %dx%d CoreArt sidecar (%d bytes)", path, dim, dim, len(b))
	}
}

func equal(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func sameTree(a, b map[string]string) bool {
	if len(a) != len(b) {
		return false
	}
	for k, v := range a {
		if b[k] != v {
			return false
		}
	}
	return true
}

// --- the multi-disc category ------------------------------------------------

// discFixture is one flat album whose tags carry two discs, already in the
// right folder under the right names. Nothing about it is wrong; the only
// thing to say is that it COULD be split.
func discFixture(t *testing.T) string {
	t.Helper()
	src := t.TempDir()
	cover := smallPNG(t)
	dir := filepath.Join(src, "Double - Band")
	for _, tc := range []struct{ name, title, track, disc string }{
		{"01 - Band - Alpha.flac", "Alpha", "1", "1"},
		{"02 - Band - Beta.flac", "Beta", "2", "1"},
		{"03 - Band - Gamma.flac", "Gamma", "1", "2"},
		{"04 - Band - Delta.flac", "Delta", "2", "2"},
	} {
		writeTrack(t, dir, tc.name, map[string]string{
			"album": "Double", "artist": "Band", "title": tc.title,
			"tracknumber": tc.track, "discnumber": tc.disc}, cover)
	}
	return src
}

// TestDiscSplitIsItsOwnCategory: the split is reported, is NOT counted as
// unorganized or misnamed, and costs no re-copy until it is asked for.
func TestDiscSplitIsItsOwnCategory(t *testing.T) {
	src := discFixture(t)
	rep, err := Inspect(context.Background(), src, Options{})
	if err != nil {
		t.Fatal(err)
	}
	if len(rep.Misnamed) != 0 || len(rep.Unorganized) != 0 {
		t.Errorf("misnamed = %v, unorganized = %v; want neither", rep.Misnamed, rep.Unorganized)
	}
	if len(rep.DiscSplit) != 4 {
		t.Fatalf("DiscSplit = %d, want 4", len(rep.DiscSplit))
	}
	if len(rep.Moves) != 0 {
		t.Errorf("Moves = %v, want none — the split is not in the plan by default", rep.Moves)
	}
	if rep.RecopyTracks != 0 || rep.RecopyBytes != 0 {
		t.Errorf("cost = %d track(s) / %d byte(s), want 0/0", rep.RecopyTracks, rep.RecopyBytes)
	}
	// Ticking it on a report that was inspected without it is an error, not
	// a tick that silently does nothing.
	if _, err := Fix(context.Background(), rep, Choices{DiscFolders: true},
		Options{DryRun: true}); err == nil {
		t.Error("Fix accepted the multi-disc tick on a report that has no disc moves to apply")
	}
}

// TestDiscSplitAppliesWhenAsked: inspected WITH the option, the same four
// files are in the plan, carry Reason "disc", and are applied only by the
// DiscFolders tick.
func TestDiscSplitAppliesWhenAsked(t *testing.T) {
	src := discFixture(t)
	rep, err := Inspect(context.Background(), src, Options{DiscFolders: true})
	if err != nil {
		t.Fatal(err)
	}
	if len(rep.DiscSplit) != 4 || len(rep.Unorganized) != 0 || len(rep.Misnamed) != 0 {
		t.Fatalf("categories = %d disc / %d unorganized / %d misnamed, want 4/0/0",
			len(rep.DiscSplit), len(rep.Unorganized), len(rep.Misnamed))
	}
	for _, m := range rep.DiscSplit {
		if m.Reason != "disc" {
			t.Errorf("%s: reason %q, want disc", m.From, m.Reason)
		}
	}
	// Names and folders ticked, discs not: nothing moves.
	sel, _, err := selectMoves(rep, Choices{FixNames: true, Organize: true})
	if err != nil {
		t.Fatal(err)
	}
	if len(sel) != 0 {
		t.Errorf("selected %d move(s) without the disc tick, want 0", len(sel))
	}
	sel, _, err = selectMoves(rep, Choices{DiscFolders: true})
	if err != nil {
		t.Fatal(err)
	}
	if len(sel) != 4 {
		t.Errorf("selected %d move(s) with the disc tick, want 4", len(sel))
	}
	res, err := Fix(context.Background(), rep, Choices{DiscFolders: true}, Options{DryRun: true})
	if err != nil {
		t.Fatal(err)
	}
	if res.Moved != 4 {
		t.Errorf("Moved = %d, want 4", res.Moved)
	}
}
