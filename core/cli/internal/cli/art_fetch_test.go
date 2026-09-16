package cli

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"net/http"
	"net/http/httptest"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// Nothing in this file touches the network: both providers are pointed
// at one httptest server, and the iTunes fixture's artwork URLs are
// rewritten to that server before it is served, so even a mistake in
// the URL-rewriting code cannot reach Apple.

// artStem is the mzstatic path in testdata/itunes_search.json for the
// album the fixture is about. The fake server stands in for it.
const artStem = "https://is1-ssl.mzstatic.com/image/thumb/Music221/v4/fa/60/fe/" +
	"fa60fe4a-4329-a0bb-eb7c-3b6094a4d831/25UMGIM46049.rgb.jpg"

// fakeArtServer answers both providers and serves the cover.
type fakeArtServer struct {
	*httptest.Server
	cover    []byte
	searches int32
	images   int32
}

func newFakeArtServer(t *testing.T) *fakeArtServer {
	t.Helper()
	s := &fakeArtServer{cover: jpegSquare(t, 400)}
	mux := http.NewServeMux()

	// iTunes.
	mux.HandleFunc("/search", func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&s.searches, 1)
		term := r.URL.Query().Get("term")
		switch {
		case strings.Contains(term, "Morgan Wallen"):
			fixture, err := os.ReadFile(filepath.Join("..", "artfetch", "testdata", "itunes_search.json"))
			if err != nil {
				t.Error(err)
				return
			}
			w.Write(bytes.ReplaceAll(fixture, []byte(artStem), []byte(s.URL+"/img")))
		case strings.Contains(term, "Cameron Dallas"):
			fmt.Fprintf(w, `{"resultCount":1,"results":[{"collectionId":1,`+
				`"artistName":"Cameron Dallas","collectionName":"Electric Feelings Tour",`+
				`"artworkUrl100":"%s/img/100x100bb.jpg"}]}`, s.URL)
		default:
			w.Write([]byte(`{"resultCount":0,"results":[]}`))
		}
	})
	// MusicBrainz: catalogued, but nobody has a cover.
	mux.HandleFunc("/ws/2/release/", func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&s.searches, 1)
		w.Write([]byte(`{"count":0,"offset":0,"releases":[]}`))
	})
	// The artwork host.
	mux.HandleFunc("/img/", func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&s.images, 1)
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

// jpegSquare is a real JPEG of the given square size — past artfetch's
// 300 px floor, so the validator accepts it.
func jpegSquare(t *testing.T, size int) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, size, size))
	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			img.Set(x, y, color.RGBA{uint8(x), uint8(y), 0x60, 255})
		}
	}
	var b bytes.Buffer
	if err := jpeg.Encode(&b, img, &jpeg.Options{Quality: 80}); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

// useFakeArt points the --fetch path at the fake server for one test.
func useFakeArt(t *testing.T, s *fakeArtServer) {
	t.Helper()
	cacheDir := t.TempDir()
	prev := newArtFetchClient
	newArtFetchClient = func() (*artfetch.Client, error) {
		return &artfetch.Client{
			Providers: []artfetch.Provider{
				&artfetch.ITunes{BaseURL: s.URL},
				&artfetch.MusicBrainz{BaseURL: s.URL, CAABaseURL: s.URL, Interval: time.Millisecond},
			},
			Cache:       &artfetch.Cache{Dir: cacheDir},
			UserAgent:   artfetch.UserAgent(),
			StopAtScore: 1.0,
		}, nil
	}
	t.Cleanup(func() { newArtFetchClient = prev })
}

// coverlessAlbum writes an album of n FLACs with tags and no pictures.
func coverlessAlbum(t *testing.T, root, folder, artist, album string, n int) string {
	t.Helper()
	dir := filepath.Join(root, folder)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for i := 1; i <= n; i++ {
		raw := flac.BuildFile(
			flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
			map[string]string{
				"title": fmt.Sprintf("Track %d", i), "artist": artist,
				"album": album, "tracknumber": fmt.Sprint(i),
			}, nil)
		name := fmt.Sprintf("%02d - %s - Track %d.flac", i, artist, i)
		if err := os.WriteFile(filepath.Join(dir, name), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func runArtFetchCmd(t *testing.T, stdin string, args ...string) (string, error) {
	t.Helper()
	cmd := newArtCmd()
	var out bytes.Buffer
	cmd.SetOut(&out)
	cmd.SetErr(&out)
	cmd.SetIn(strings.NewReader(stdin))
	cmd.SetArgs(args)
	err := cmd.Execute()
	return out.String(), err
}

// mustHaveCover fails unless every FLAC of the album carries the cover.
func mustHaveCover(t *testing.T, dir string, want []byte) {
	t.Helper()
	files, err := albumFLACs(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(files) == 0 {
		t.Fatalf("%s has no FLACs", dir)
	}
	for _, f := range files {
		m, err := flac.ReadFile(f)
		if err != nil {
			t.Fatalf("%s: %v", f, err)
		}
		pic := m.FrontCover()
		if pic == nil {
			t.Errorf("%s has no front cover", f)
			continue
		}
		if !bytes.Equal(pic.Data, want) {
			t.Errorf("%s carries %d bytes, want the %d fetched", f, len(pic.Data), len(want))
		}
		if pic.MIME != "image/jpeg" {
			t.Errorf("%s: MIME = %q", f, pic.MIME)
		}
	}
}

func TestArtFetchReportsWithoutWriting(t *testing.T) {
	root := t.TempDir()
	srv := newFakeArtServer(t)
	useFakeArt(t, srv)

	wallen := coverlessAlbum(t, root, "I'm The Problem - Morgan Wallen", "Morgan Wallen", "I'm The Problem", 2)
	writeAlbumDir(t, root, "Already Covered - Artist", coverJPEG(t, 200))
	coverlessAlbum(t, root, "Unreleased Tape - Nobody At All", "Nobody At All", "Unreleased Tape", 1)

	out, err := runArtFetchCmd(t, "", "--fetch", "--batch", root)
	if err != nil {
		t.Fatalf("art --fetch: %v\n%s", err, out)
	}
	for _, want := range []string{
		`no cover — searching for "I'm The Problem" by "Morgan Wallen"`,
		"1.00  itunes       Morgan Wallen — I’m The Problem",
		"0.90  itunes       Morgan Wallen — I'm The Problem - Single",
		"pass --write to embed",
		"has art",
		"no match",
		"3 album(s): 1 already had art, 1 matched, 0 embedded, 1 with no match",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("output is missing %q:\n%s", want, out)
		}
	}
	// A report writes nothing at all.
	for _, name := range []string{"cover.jpg", coreart.ArtName, coreart.ThumbName} {
		if _, err := os.Stat(filepath.Join(wallen, name)); !os.IsNotExist(err) {
			t.Errorf("a report without --write created %s", name)
		}
	}
	if atomic.LoadInt32(&srv.images) != 0 {
		t.Error("a report downloaded an image")
	}
	// The exact match stopped the walk before MusicBrainz was asked;
	// the album with no match consulted both.
	if got := atomic.LoadInt32(&srv.searches); got != 3 {
		t.Errorf("%d searches, want 3 (itunes for the match, itunes+musicbrainz for the miss)", got)
	}
}

func TestArtFetchDryRunWritesNothing(t *testing.T) {
	root := t.TempDir()
	srv := newFakeArtServer(t)
	useFakeArt(t, srv)
	dir := coverlessAlbum(t, root, "I'm The Problem - Morgan Wallen", "Morgan Wallen", "I'm The Problem", 2)

	out, err := runArtFetchCmd(t, "", "--fetch", "--batch", root, "--write", "--yes", "--dry-run")
	if err != nil {
		t.Fatalf("art --fetch --dry-run: %v\n%s", err, out)
	}
	if !strings.Contains(out, "would embed 1.00 itunes into 2 file(s)") {
		t.Errorf("output does not say what it would do:\n%s", out)
	}
	if _, err := os.Stat(filepath.Join(dir, "cover.jpg")); !os.IsNotExist(err) {
		t.Error("--dry-run wrote cover.jpg")
	}
	if atomic.LoadInt32(&srv.images) != 0 {
		t.Error("--dry-run downloaded the image")
	}
	m, err := flac.ReadFile(filepath.Join(dir, "01 - Morgan Wallen - Track 1.flac"))
	if err != nil {
		t.Fatal(err)
	}
	if m.FrontCover() != nil {
		t.Error("--dry-run embedded a cover")
	}
}

// TestArtFetchWriteEmbedsAndBakesSidecars is the slice's "done when":
// the fixture-driven fetch embeds art that coreart then turns into
// sidecars the firmware accepts.
func TestArtFetchWriteEmbedsAndBakesSidecars(t *testing.T) {
	root := t.TempDir()
	srv := newFakeArtServer(t)
	useFakeArt(t, srv)
	dir := coverlessAlbum(t, root, "I'm The Problem - Morgan Wallen", "Morgan Wallen", "I'm The Problem", 3)

	out, err := runArtFetchCmd(t, "", "--fetch", "--batch", root, "--write", "--yes")
	if err != nil {
		t.Fatalf("art --fetch --write: %v\n%s", err, out)
	}
	if !strings.Contains(out, "embedded 1.00 itunes (image/jpeg") ||
		!strings.Contains(out, "into 3 file(s), cover.jpg, folder.art + folder.thm") {
		t.Errorf("output does not report the write:\n%s", out)
	}
	mustHaveCover(t, dir, srv.cover)

	cover, err := os.ReadFile(filepath.Join(dir, "cover.jpg"))
	if err != nil || !bytes.Equal(cover, srv.cover) {
		t.Errorf("cover.jpg: %v (%d bytes)", err, len(cover))
	}
	mustValid(t, filepath.Join(dir, coreart.ArtName), coreart.ArtSize)
	mustValid(t, filepath.Join(dir, coreart.ThumbName), coreart.ThumbSize)

	// Everything the device needs now comes off the album itself, so a
	// second run has nothing to do and asks nobody.
	before := atomic.LoadInt32(&srv.searches)
	out, err = runArtFetchCmd(t, "", "--fetch", "--batch", root, "--write", "--yes")
	if err != nil {
		t.Fatalf("second run: %v\n%s", err, out)
	}
	if !strings.Contains(out, "has art") {
		t.Errorf("the album with art was searched for again:\n%s", out)
	}
	if got := atomic.LoadInt32(&srv.searches); got != before {
		t.Errorf("the second run made %d search(es)", got-before)
	}
}

// TestArtFetchYesRefusesAWeakMatch: --yes is not "do whatever you
// found". Below --min-score the candidate is printed and skipped.
func TestArtFetchYesRefusesAWeakMatch(t *testing.T) {
	root := t.TempDir()
	srv := newFakeArtServer(t)
	useFakeArt(t, srv)
	dir := coverlessAlbum(t, root, "Electric - Cameron Dallas", "Cameron Dallas", "Electric", 1)

	out, err := runArtFetchCmd(t, "", "--fetch", "--album", dir, "--write", "--yes")
	if err != nil {
		t.Fatalf("art --fetch: %v\n%s", err, out)
	}
	if !strings.Contains(out, "0.70  itunes") ||
		!strings.Contains(out, "0.70 is below --min-score 0.90; skipped") {
		t.Errorf("a 0.70 match was not refused:\n%s", out)
	}
	if _, err := os.Stat(filepath.Join(dir, "cover.jpg")); !os.IsNotExist(err) {
		t.Error("a below-threshold candidate was written")
	}
	// The same candidate is accepted when the user lowers the bar on
	// purpose.
	out, err = runArtFetchCmd(t, "", "--fetch", "--album", dir, "--write", "--yes", "--min-score", "0.7")
	if err != nil {
		t.Fatalf("art --fetch --min-score 0.7: %v\n%s", err, out)
	}
	if !strings.Contains(out, "embedded 0.70 itunes") {
		t.Errorf("--min-score 0.7 did not accept the 0.70 candidate:\n%s", out)
	}
	mustHaveCover(t, dir, srv.cover)
}

// TestArtFetchAsksWhenNotYes covers the confirm-first path: with
// --write but no --yes the user answers per album.
func TestArtFetchAsksWhenNotYes(t *testing.T) {
	root := t.TempDir()
	srv := newFakeArtServer(t)
	useFakeArt(t, srv)
	dir := coverlessAlbum(t, root, "I'm The Problem - Morgan Wallen", "Morgan Wallen", "I'm The Problem", 1)

	out, err := runArtFetchCmd(t, "n\n", "--fetch", "--album", dir, "--write")
	if err != nil {
		t.Fatalf("art --fetch: %v\n%s", err, out)
	}
	if !strings.Contains(out, "embed this cover? [y/N]") || !strings.Contains(out, "→ skipped") {
		t.Errorf("the answer 'n' was not honoured:\n%s", out)
	}
	if _, err := os.Stat(filepath.Join(dir, "cover.jpg")); !os.IsNotExist(err) {
		t.Error("'n' wrote the cover anyway")
	}

	out, err = runArtFetchCmd(t, "y\n", "--fetch", "--album", dir, "--write")
	if err != nil {
		t.Fatalf("art --fetch: %v\n%s", err, out)
	}
	if !strings.Contains(out, "embedded 1.00 itunes") {
		t.Errorf("the answer 'y' did not embed:\n%s", out)
	}
	mustHaveCover(t, dir, srv.cover)
}

func TestArtFetchNeedsATarget(t *testing.T) {
	if _, err := runArtFetchCmd(t, "", "--fetch"); err == nil {
		t.Error("--fetch with no folder was accepted")
	}
}

// TestAlbumQueryFallsBackToTheFolderName: a file with no album tag is
// still searchable, because the source convention puts the album and
// the artist in the folder name.
func TestAlbumQueryFallsBackToTheFolderName(t *testing.T) {
	m := &flac.Meta{Tags: map[string]string{}}
	q := albumQuery(filepath.Join("X", "One Thing At A Time - Morgan Wallen"), m)
	if q.Album != "One Thing At A Time" || q.Artist != "Morgan Wallen" {
		t.Errorf("albumQuery = %+v", q)
	}
	// Tags win when they are there.
	m = &flac.Meta{Tags: map[string]string{"album": "Dangerous", "albumartist": "M. Wallen"}}
	q = albumQuery(filepath.Join("X", "One Thing At A Time - Morgan Wallen"), m)
	if q.Album != "Dangerous" || q.Artist != "M. Wallen" {
		t.Errorf("albumQuery = %+v", q)
	}
}

func TestAlbumFLACsIncludesDiscFolders(t *testing.T) {
	root := t.TempDir()
	coverlessAlbum(t, root, filepath.Join("Album - A", "Disc 2"), "A", "Album", 1)
	coverlessAlbum(t, root, filepath.Join("Album - A", "Disc 1"), "A", "Album", 2)
	files, err := albumFLACs(filepath.Join(root, "Album - A"))
	if err != nil {
		t.Fatal(err)
	}
	if len(files) != 3 {
		t.Fatalf("got %d files, want 3: %v", len(files), files)
	}
	if !strings.Contains(files[0], "Disc 1") || !strings.Contains(files[2], "Disc 2") {
		t.Errorf("disc order is wrong: %v", files)
	}
}
