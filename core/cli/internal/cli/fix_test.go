package cli

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
)

// The tests in this file are about the COMMAND — the flags, what lands on
// stdout, what is on disk afterwards. internal/librarian holds the rules.
// Nothing here touches the network: the art client is pointed at an httptest
// server that also serves the cover.

// fixFixture is a tree with one of each problem in it:
//
//	Canon - Artist A/  01 - Artist A - One.flac   canonical, no cover
//	Messy - Artist B/  Track 3.flac               tags say "03 - Artist B - Three"
//	Downloads/Wild - Artist C/whatever.flac       right names, wrong place
//	Messy - Artist B/  mystery.flac               no title tag: needs attention
//
// Three of those albums have no embedded cover; the fake service below knows
// two of them, so the third exercises the "no match" line.
func fixFixture(t *testing.T) string {
	t.Helper()
	src := t.TempDir()
	write := func(dir, name string, tags map[string]string) {
		t.Helper()
		full := filepath.Join(src, filepath.FromSlash(dir))
		if err := os.MkdirAll(full, 0o755); err != nil {
			t.Fatal(err)
		}
		raw := flac.BuildFile(
			flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
			tags, nil)
		if err := os.WriteFile(filepath.Join(full, name), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("Canon - Artist A", "01 - Artist A - One.flac", map[string]string{
		"album": "Canon", "artist": "Artist A", "title": "One", "tracknumber": "1"})
	write("Messy - Artist B", "Track 3.flac", map[string]string{
		"album": "Messy", "artist": "Artist B", "title": "Three", "tracknumber": "3"})
	write("Messy - Artist B", "mystery.flac", map[string]string{
		"album": "Messy", "artist": "Artist B", "tracknumber": "9"})
	write("Downloads/Wild - Artist C", "whatever.flac", map[string]string{
		"album": "Wild", "artist": "Artist C", "title": "Song", "tracknumber": "1"})
	return src
}

// useFakeLibrarianArt points `core fix`'s art client at an httptest server
// that answers for the fixture's two coverless albums.
func useFakeLibrarianArt(t *testing.T) *httptest.Server {
	t.Helper()
	cover := jpegSquare(t, 400)
	mux := http.NewServeMux()
	var srv *httptest.Server
	mux.HandleFunc("/search", func(w http.ResponseWriter, r *http.Request) {
		term := r.URL.Query().Get("term")
		for _, a := range []struct{ artist, album string }{
			{"Artist A", "Canon"}, {"Artist C", "Wild"},
		} {
			if strings.Contains(term, a.album) && strings.Contains(term, a.artist) {
				fmt.Fprintf(w, `{"resultCount":1,"results":[{"collectionId":7,`+
					`"artistName":%q,"collectionName":%q,`+
					`"artworkUrl100":"%s/img/100x100bb.jpg"}]}`, a.artist, a.album, srv.URL)
				return
			}
		}
		w.Write([]byte(`{"resultCount":0,"results":[]}`))
	})
	mux.HandleFunc("/img/", func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasSuffix(r.URL.Path, "/1200x1200bb.jpg") {
			http.NotFound(w, r)
			return
		}
		w.Write(cover)
	})
	srv = httptest.NewServer(mux)
	t.Cleanup(srv.Close)

	prev := newLibrarianArtClient
	newLibrarianArtClient = func() (librarian.ArtClient, error) {
		return &artfetch.Client{
			Providers:   []artfetch.Provider{&artfetch.ITunes{BaseURL: srv.URL}},
			UserAgent:   artfetch.UserAgent(),
			StopAtScore: 1.0,
		}, nil
	}
	t.Cleanup(func() { newLibrarianArtClient = prev })
	return srv
}

// TestFixIsAReportByDefault: the default prints the list and changes nothing,
// and the last line is the cost of acting on it.
func TestFixIsAReportByDefault(t *testing.T) {
	src := fixFixture(t)
	out, _, err := runCore(t, "fix", "--src", src)
	if err != nil {
		t.Fatalf("fix: %v", err)
	}
	for _, want := range []string{
		"Missing art (3)",
		"Canon - Artist A",
		"Misnamed (1)",
		"Track 3.flac",
		"03 - Artist B - Three.flac",
		"Unorganized (1)",
		"Needs attention (1)",
		"no title tag",
		"will be re-copied to the iPod on the next sync",
		"--yes",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("the report does not mention %q:\n%s", want, out)
		}
	}
	// A report asks nobody about art: no candidate line, no "Looking for".
	if strings.Contains(out, "Looking for") {
		t.Errorf("the default report searched for cover art:\n%s", out)
	}
	if _, err := os.Stat(filepath.Join(src, "Messy - Artist B", "Track 3.flac")); err != nil {
		t.Errorf("the report renamed a file: %v", err)
	}
}

// TestFixArtPrintsCandidates: --art is the flag that goes to the network, and
// it prints what it found with the score --yes would act on.
func TestFixArtPrintsCandidates(t *testing.T) {
	src := fixFixture(t)
	useFakeLibrarianArt(t)
	out, _, err := runCore(t, "fix", "--src", src, "--art")
	if err != nil {
		t.Fatalf("fix --art: %v", err)
	}
	for _, want := range []string{"Looking for 3 cover(s)", "1.00", "itunes", "[take]"} {
		if !strings.Contains(out, want) {
			t.Errorf("the candidate list does not mention %q:\n%s", want, out)
		}
	}
	// --art alone narrows the job: no rename table.
	if strings.Contains(out, "Misnamed (") {
		t.Errorf("--art printed the rename table too:\n%s", out)
	}
	if !hasNoCover(t, filepath.Join(src, "Canon - Artist A", "01 - Artist A - One.flac")) {
		t.Error("--art without --yes embedded a cover")
	}
}

// TestFixYesFixesEverything is the whole command: names, folders and covers in
// one journalled job.
func TestFixYesFixesEverything(t *testing.T) {
	src := fixFixture(t)
	journal := t.TempDir()
	useFakeLibrarianArt(t)

	out, _, err := runCore(t, "fix", "--src", src, "--yes", "--journal-dir", journal)
	if err != nil {
		t.Fatalf("fix --yes: %v", err)
	}
	if !strings.Contains(out, "fixed:") || !strings.Contains(out, "undo journal:") {
		t.Errorf("the summary is missing:\n%s", out)
	}
	for _, want := range []string{
		"Messy - Artist B/03 - Artist B - Three.flac",
		"Wild - Artist C/01 - Artist C - Song.flac",
		"Canon - Artist A/folder.art",
		"Canon - Artist A/cover.jpg",
		"Wild - Artist C/folder.thm",
	} {
		if _, err := os.Stat(filepath.Join(src, filepath.FromSlash(want))); err != nil {
			t.Errorf("%s: %v", want, err)
		}
	}
	mustValidSidecar(t, filepath.Join(src, "Canon - Artist A", coreart.ArtName), coreart.ArtSize)
	if hasNoCover(t, filepath.Join(src, "Canon - Artist A", "01 - Artist A - One.flac")) {
		t.Error("the accepted cover was not embedded")
	}

	// Exactly one journal for the whole job, and --undo puts the names back.
	js, err := os.ReadDir(journal)
	if err != nil || len(js) != 1 {
		t.Fatalf("journal dir holds %v (%v)", js, err)
	}
	undone, _, err := runCore(t, "fix", "--undo", filepath.Join(journal, js[0].Name()))
	if err != nil {
		t.Fatalf("fix --undo: %v", err)
	}
	if !strings.Contains(undone, "put 2 file(s) back") {
		t.Errorf("undo said: %s", undone)
	}
	if !strings.Contains(undone, "not removed by an undo") {
		t.Errorf("undo does not say the art stays:\n%s", undone)
	}
	if _, err := os.Stat(filepath.Join(src, "Messy - Artist B", "Track 3.flac")); err != nil {
		t.Errorf("the undo did not put the name back: %v", err)
	}
}

// TestFixDryRunWritesNothing: --dry-run --yes decides everything and touches
// no file.
func TestFixDryRunWritesNothing(t *testing.T) {
	src := fixFixture(t)
	journal := t.TempDir()
	useFakeLibrarianArt(t)

	before := listTree(t, src)
	out, _, err := runCore(t, "fix", "--src", src, "--yes", "--dry-run", "--journal-dir", journal)
	if err != nil {
		t.Fatalf("fix --dry-run: %v", err)
	}
	if !strings.Contains(out, "would fix:") {
		t.Errorf("a dry run did not say it was one:\n%s", out)
	}
	if after := listTree(t, src); strings.Join(after, "\n") != strings.Join(before, "\n") {
		t.Errorf("a dry run changed the tree:\n%v\n%v", before, after)
	}
	if js, _ := os.ReadDir(journal); len(js) != 0 {
		t.Errorf("a dry run wrote a journal: %v", js)
	}
}

// TestFixNamesOnly: one flag narrows the job to one part of it.
func TestFixNamesOnly(t *testing.T) {
	src := fixFixture(t)
	journal := t.TempDir()
	_, _, err := runCore(t, "fix", "--src", src, "--names", "--yes", "--journal-dir", journal)
	if err != nil {
		t.Fatalf("fix --names --yes: %v", err)
	}
	if _, err := os.Stat(filepath.Join(src, "Messy - Artist B", "03 - Artist B - Three.flac")); err != nil {
		t.Errorf("the rename did not happen: %v", err)
	}
	if _, err := os.Stat(filepath.Join(src, "Downloads", "Wild - Artist C", "whatever.flac")); err != nil {
		t.Errorf("--names moved a folder as well: %v", err)
	}
}

// TestFixJSONIsOneObject: the UI reads the same command the terminal does.
func TestFixJSONIsOneObject(t *testing.T) {
	src := fixFixture(t)
	useFakeLibrarianArt(t)
	out, _, err := runCore(t, "fix", "--src", src, "--art", "--json")
	if err != nil {
		t.Fatalf("fix --json: %v", err)
	}
	var got struct {
		Report struct {
			Root           string              `json:"root"`
			MissingArt     []map[string]any    `json:"missing_art"`
			Misnamed       []map[string]any    `json:"misnamed"`
			NeedsAttention []map[string]string `json:"needs_attention"`
			RecopyTracks   int                 `json:"recopy_tracks"`
		} `json:"report"`
		Candidates map[string][]struct {
			Score float64 `json:"Score"`
		} `json:"candidates"`
	}
	if err := json.Unmarshal([]byte(out), &got); err != nil {
		t.Fatalf("--json is not JSON: %v\n%s", err, out)
	}
	if len(got.Report.MissingArt) != 3 || len(got.Report.Misnamed) != 1 ||
		len(got.Report.NeedsAttention) != 1 {
		t.Errorf("the JSON report is not the printed one: %+v", got.Report)
	}
	if len(got.Candidates) != 2 {
		t.Errorf("candidates: %+v", got.Candidates)
	}
}

// TestFixNeedsASource — with no --src and no CORELIB_SRC, the error says so.
func TestFixNeedsASource(t *testing.T) {
	t.Setenv("CORELIB_SRC", "")
	_, _, err := runCore(t, "fix")
	if err == nil || !strings.Contains(err.Error(), "--src") {
		t.Fatalf("error = %v, want one that names --src", err)
	}
}

// --- helpers ----------------------------------------------------------------

func hasNoCover(t *testing.T, path string) bool {
	t.Helper()
	m, err := flac.ReadFile(path)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	return m.FrontCover() == nil
}

func mustValidSidecar(t *testing.T, path string, dim int) {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	if !coreart.Valid(b, dim) {
		t.Errorf("%s is not a valid %dx%d sidecar", path, dim, dim)
	}
}

func listTree(t *testing.T, root string) []string {
	t.Helper()
	var out []string
	err := filepath.Walk(root, func(p string, fi os.FileInfo, err error) error {
		if err != nil || fi.IsDir() {
			return err
		}
		rel, _ := filepath.Rel(root, p)
		out = append(out, fmt.Sprintf("%s %d", filepath.ToSlash(rel), fi.Size()))
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// TestFixDryRunAloneShowsWhatYesWouldDo: --dry-run without --yes is not the
// bare report; it is the whole decision pass with the "would fix" line, and it
// writes nothing — no journal, no rename.
func TestFixDryRunAloneShowsWhatYesWouldDo(t *testing.T) {
	src := fixFixture(t)
	journal := t.TempDir()
	before := listTree(t, src)
	out, _, err := runCore(t, "fix", "--src", src, "--dry-run", "--journal-dir", journal)
	if err != nil {
		t.Fatalf("fix --dry-run: %v", err)
	}
	if !strings.Contains(out, "would fix:") {
		t.Errorf("--dry-run alone did not run the decision pass:\n%s", out)
	}
	if entries, _ := os.ReadDir(journal); len(entries) != 0 {
		t.Errorf("--dry-run wrote a journal: %v", entries)
	}
	if after := listTree(t, src); strings.Join(after, "\n") != strings.Join(before, "\n") {
		t.Errorf("--dry-run changed the tree:\n%v\n%v", before, after)
	}
}

// discFolderFixture is a flat album whose tags carry two discs, already named
// and foldered the way the convention wants. The only thing to say about it is
// that it COULD be split.
func discFolderFixture(t *testing.T) string {
	t.Helper()
	src := t.TempDir()
	dir := filepath.Join(src, "Double - Band")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct{ name, title, track, disc string }{
		{"01 - Band - Alpha.flac", "Alpha", "1", "1"},
		{"02 - Band - Beta.flac", "Beta", "2", "1"},
		{"03 - Band - Gamma.flac", "Gamma", "1", "2"},
		{"04 - Band - Delta.flac", "Delta", "2", "2"},
	} {
		raw := flac.BuildFile(
			flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
			map[string]string{"album": "Double", "artist": "Band", "title": tc.title,
				"tracknumber": tc.track, "discnumber": tc.disc}, nil)
		if err := os.WriteFile(filepath.Join(dir, tc.name), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return src
}

// TestFixMultiDiscIsOffByDefault: the split is offered on its own line, is not
// counted as unorganized, and nothing is moved without --disc-folders.
func TestFixMultiDiscIsOffByDefault(t *testing.T) {
	src := discFolderFixture(t)
	out, _, err := runCore(t, "fix", "--src", src, "--names", "--organize", "--dry-run")
	if err != nil {
		t.Fatalf("fix: %v", err)
	}
	for _, want := range []string{
		"Unorganized (0)",
		"Misnamed (0)",
		"Multi-disc albums (4 files) — off by default, `--disc-folders` to split into Disc N folders",
		"would fix: 0 renamed, 0 re-foldered",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("the report does not say %q:\n%s", want, out)
		}
	}

	out, _, err = runCore(t, "fix", "--src", src, "--names", "--organize", "--disc-folders", "--dry-run")
	if err != nil {
		t.Fatalf("fix --disc-folders: %v", err)
	}
	for _, want := range []string{
		"Multi-disc albums (4)",
		"Disc 1/01 - Band - Alpha.flac",
		"Disc 2/01 - Band - Gamma.flac",
		"would fix: 0 renamed, 4 re-foldered",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("--disc-folders does not say %q:\n%s", want, out)
		}
	}
	// Still a dry run: the files are where they were.
	if _, err := os.Stat(filepath.Join(src, "Double - Band", "03 - Band - Gamma.flac")); err != nil {
		t.Errorf("the dry run moved a file: %v", err)
	}
}
