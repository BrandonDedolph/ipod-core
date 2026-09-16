package organizer

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cidx"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

// writeTrack puts a real (tiny, silent) FLAC at path. The fixtures are real
// files because the organizer reads their tags through the same reader the
// index does: a fake would only prove the fake agrees with itself.
func writeTrack(t *testing.T, path string, tags map[string]string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	raw := flac.BuildFile(
		flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
		tags, nil)
	if err := os.WriteFile(path, raw, 0o644); err != nil {
		t.Fatal(err)
	}
}

func tagsFor(album, artist, title, track string) map[string]string {
	return map[string]string{"album": album, "artist": artist, "title": title, "tracknumber": track}
}

// moveMap is the plan as "before -> after", both relative to the root, which
// is how the preview reads and the only shape worth asserting on.
func moveMap(t *testing.T, p *Plan) map[string]string {
	t.Helper()
	out := map[string]string{}
	for _, m := range p.Moves {
		out[rel(p.Root, m.From)] = rel(p.Root, m.To)
	}
	return out
}

func attentionOn(p *Plan, root, relPath string) string {
	for _, a := range p.Attention {
		if rel(root, a.Path) == relPath {
			return a.Reason
		}
	}
	return ""
}

func scan(t *testing.T, root string) *Plan {
	t.Helper()
	p, err := Scan(Options{Root: root})
	if err != nil {
		t.Fatalf("Scan: %v", err)
	}
	return p
}

func scanWith(t *testing.T, o Options) *Plan {
	t.Helper()
	p, err := Scan(o)
	if err != nil {
		t.Fatalf("Scan: %v", err)
	}
	return p
}

func splitMap(p *Plan) map[string]string {
	out := map[string]string{}
	for _, m := range p.DiscSplit {
		out[rel(p.Root, m.From)] = rel(p.Root, m.To)
	}
	return out
}

// TestAlreadyCanonicalTreeHasNothingToDo is the test the real library has to
// pass: the MC tree is already in this convention, so a plan against it must
// be empty. An organizer that "fixes" a correct tree would re-copy the whole
// library to the iPod for nothing.
func TestAlreadyCanonicalTreeHasNothingToDo(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "I'm The Problem - Morgan Wallen")
	writeTrack(t, filepath.Join(dir, "01 - Morgan Wallen - I'm The Problem.flac"),
		tagsFor("I'm The Problem", "Morgan Wallen", "I'm The Problem", "1"))
	writeTrack(t, filepath.Join(dir, "02 - Morgan Wallen - I Got Better.flac"),
		tagsFor("I'm The Problem", "Morgan Wallen", "I Got Better", "2"))
	writeTrack(t, filepath.Join(dir, "03 - Morgan Wallen - Superman.flac"),
		tagsFor("I'm The Problem", "Morgan Wallen", "Superman", "3"))

	p := scan(t, root)
	if !p.Empty() {
		t.Errorf("plan on an already-canonical album is not empty: %v", moveMap(t, p))
	}
	if len(p.Attention) != 0 {
		t.Errorf("attention on a clean album: %v", p.Attention)
	}
	if p.TracksRecopied != 0 || p.TracksNew != 0 {
		t.Errorf("recopy cost %d/%d, want 0/0", p.TracksRecopied, p.TracksNew)
	}
}

// TestJunkNamesBecomeTheConvention: the names carry nothing, the tags carry
// everything, and the result is the convention.
func TestJunkNamesBecomeTheConvention(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "morgan wallen stuff")
	writeTrack(t, filepath.Join(dir, "Track 3.flac"),
		tagsFor("If I Know Me", "Morgan Wallen", "Whiskey Glasses", "3"))
	writeTrack(t, filepath.Join(dir, "morgan wallen - tn.flac"),
		tagsFor("If I Know Me", "Morgan Wallen", "Talkin' Tennessee", "1"))
	writeTrack(t, filepath.Join(dir, "02.flac"),
		tagsFor("If I Know Me", "Morgan Wallen", "Up Down", "2"))

	p := scan(t, root)
	want := map[string]string{
		"morgan wallen stuff/Track 3.flac":            "If I Know Me - Morgan Wallen/03 - Morgan Wallen - Whiskey Glasses.flac",
		"morgan wallen stuff/morgan wallen - tn.flac": "If I Know Me - Morgan Wallen/01 - Morgan Wallen - Talkin' Tennessee.flac",
		"morgan wallen stuff/02.flac":                 "If I Know Me - Morgan Wallen/02 - Morgan Wallen - Up Down.flac",
	}
	if got := moveMap(t, p); !sameMap(got, want) {
		t.Errorf("moves =\n%s\nwant\n%s", dump(got), dump(want))
	}
	if p.AlbumsTouched != 1 {
		t.Errorf("AlbumsTouched = %d, want 1", p.AlbumsTouched)
	}
}

// TestNestedAlbumComesUpToTheRoot is the "Downloads\RUNNING WILD\" case: the
// folder is two levels down, so library.ScanTree cannot see it at all and the
// tracks are not on the iPod. They count as new copies, not re-copies.
func TestNestedAlbumComesUpToTheRoot(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Downloads", "RUNNING WILD")
	writeTrack(t, filepath.Join(dir, "1.flac"), tagsFor("RUNNING WILD", "Cameron Dallas", "Why Haven't I Met You", "1"))
	writeTrack(t, filepath.Join(dir, "2.flac"), tagsFor("RUNNING WILD", "Cameron Dallas", "Hollywood", "2"))

	p := scan(t, root)
	want := map[string]string{
		"Downloads/RUNNING WILD/1.flac": "RUNNING WILD - Cameron Dallas/01 - Cameron Dallas - Why Haven't I Met You.flac",
		"Downloads/RUNNING WILD/2.flac": "RUNNING WILD - Cameron Dallas/02 - Cameron Dallas - Hollywood.flac",
	}
	if got := moveMap(t, p); !sameMap(got, want) {
		t.Errorf("moves =\n%s\nwant\n%s", dump(got), dump(want))
	}
	if p.TracksNew != 2 || p.TracksRecopied != 0 {
		t.Errorf("cost = %d new / %d re-copied, want 2/0", p.TracksNew, p.TracksRecopied)
	}
}

// TestMultiDiscGetsDiscFolders: a flat folder whose tags say two discs comes
// out as Disc 1/ and Disc 2/ — but only when it is ASKED for, because the
// device takes disc and track from the tags and a flat album already sorts.
func TestMultiDiscGetsDiscFolders(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "dump")
	for _, tc := range []struct{ name, title, track, disc string }{
		{"a.flac", "One", "1", "1"},
		{"b.flac", "Two", "2", "1"},
		{"c.flac", "Three", "1", "2"},
	} {
		tg := tagsFor("Big Album", "Band", tc.title, tc.track)
		tg["discnumber"] = tc.disc
		writeTrack(t, filepath.Join(dir, tc.name), tg)
	}
	p := scanWith(t, Options{Root: root, DiscFolders: true})
	want := map[string]string{
		"dump/a.flac": "Big Album - Band/Disc 1/01 - Band - One.flac",
		"dump/b.flac": "Big Album - Band/Disc 1/02 - Band - Two.flac",
		"dump/c.flac": "Big Album - Band/Disc 2/01 - Band - Three.flac",
	}
	if got := moveMap(t, p); !sameMap(got, want) {
		t.Errorf("moves =\n%s\nwant\n%s", dump(got), dump(want))
	}
	for _, m := range p.Moves {
		if m.Reason != "disc" {
			t.Errorf("%s: reason %q, want disc", rel(p.Root, m.From), m.Reason)
		}
	}
	if len(p.DiscSplit) != 0 {
		t.Errorf("DiscSplit = %v, want empty when the split is on", p.DiscSplit)
	}

	// Off (the default): the album is still lifted out of "dump" into its
	// "Album - Artist" folder, flat, and the split is only OFFERED.
	p = scan(t, root)
	wantFlat := map[string]string{
		"dump/a.flac": "Big Album - Band/1-01 - Band - One.flac",
		"dump/b.flac": "Big Album - Band/1-02 - Band - Two.flac",
		"dump/c.flac": "Big Album - Band/2-01 - Band - Three.flac",
	}
	if got := moveMap(t, p); !sameMap(got, wantFlat) {
		t.Errorf("flat moves =\n%s\nwant\n%s", dump(got), dump(wantFlat))
	}
	if got := splitMap(p); !sameMap(got, want) {
		t.Errorf("DiscSplit =\n%s\nwant\n%s", dump(got), dump(want))
	}
}

// TestFlatMultiDiscStaysFlatAndStill: the real library's case. "SWAG II" is
// already in the right folder with the right names and its tags carry two
// discs; by default there is NOTHING to do, and the split is offered as its
// own category.
func TestFlatMultiDiscStaysFlatAndStill(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Big Album - Band")
	for _, tc := range []struct{ name, title, track, disc string }{
		{"01 - Band - One.flac", "One", "1", "1"},
		{"02 - Band - Two.flac", "Two", "2", "1"},
		{"03 - Band - Three.flac", "Three", "1", "2"},
	} {
		tg := tagsFor("Big Album", "Band", tc.title, tc.track)
		tg["discnumber"] = tc.disc
		writeTrack(t, filepath.Join(dir, tc.name), tg)
	}
	p := scan(t, root)
	if len(p.Moves) != 0 {
		t.Errorf("moves = %s, want none", dump(moveMap(t, p)))
	}
	if len(p.DiscSplit) != 3 {
		t.Errorf("DiscSplit = %d move(s), want 3", len(p.DiscSplit))
	}
	for _, m := range p.DiscSplit {
		if m.Reason != "disc" {
			t.Errorf("%s: reason %q, want disc", rel(p.Root, m.From), m.Reason)
		}
	}
	if p.TracksRecopied != 0 {
		t.Errorf("TracksRecopied = %d, want 0 — the split is not in the plan", p.TracksRecopied)
	}
	// The number a flat file already carries is kept: renumbering it would
	// move every device name in the folder.
	want := map[string]string{
		"Big Album - Band/01 - Band - One.flac":   "Big Album - Band/Disc 1/01 - Band - One.flac",
		"Big Album - Band/02 - Band - Two.flac":   "Big Album - Band/Disc 1/02 - Band - Two.flac",
		"Big Album - Band/03 - Band - Three.flac": "Big Album - Band/Disc 2/01 - Band - Three.flac",
	}
	if got := splitMap(p); !sameMap(got, want) {
		t.Errorf("DiscSplit =\n%s\nwant\n%s", dump(got), dump(want))
	}
}

// TestDiscFoldersLeavesAnAlreadySplitAlbumAlone: the option is about SPLITTING
// a flat album, never about flattening one that is already in Disc N folders.
func TestDiscFoldersLeavesAnAlreadySplitAlbumAlone(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Big Album - Band")
	for _, tc := range []struct{ sub, name, title, track string }{
		{"Disc 1", "01 - Band - One.flac", "One", "1"},
		{"Disc 2", "01 - Band - Three.flac", "Three", "1"},
	} {
		writeTrack(t, filepath.Join(dir, tc.sub, tc.name),
			tagsFor("Big Album", "Band", tc.title, tc.track))
	}
	p := scan(t, root)
	if len(p.Moves) != 0 || len(p.DiscSplit) != 0 {
		t.Errorf("moves = %s, DiscSplit = %v; want both empty", dump(moveMap(t, p)), p.DiscSplit)
	}
}

// TestSeparatorsInNames pins the two halves of the " - " rule: a TITLE keeps
// its separator, because TrackTitle re-joins everything after the second one;
// an ARTIST cannot, because its separator would eat the start of the title.
func TestSeparatorsInNames(t *testing.T) {
	root := t.TempDir()
	writeTrack(t, filepath.Join(root, "x", "1.flac"),
		tagsFor("Live", "Emerson - Lake", "Ghost - Live", "1"))

	p := scan(t, root)
	to := p.Moves[0].To
	if got, want := rel(p.Root, to), "Live - Emerson – Lake/01 - Emerson – Lake - Ghost - Live.flac"; got != want {
		t.Fatalf("target = %q, want %q", got, want)
	}
	// The whole point: library reads the title straight back out.
	if got := library.TrackTitle(filepath.Base(to)); got != "Ghost - Live" {
		t.Errorf("TrackTitle(%q) = %q, want the tag title back", filepath.Base(to), got)
	}
	artist, album := library.SplitAlbumArtist(filepath.Base(filepath.Dir(to)))
	if artist != "Emerson – Lake" || album != "Live" {
		t.Errorf("SplitAlbumArtist = (%q, %q), want (Emerson – Lake, Live)", artist, album)
	}
}

// TestEnDashIsNotWorthARename is the same rule as the curly apostrophe, for
// the other half of the set NormKey folds. The real library's "our little
// angel - EP - ROLE MODEL" was listed as unorganized for one reason: the tag
// album spells the EP with U+2013. Nine files, a whole album re-copied, for a
// dash the device's locator hash cannot even see.
//
// The folder SEPARATOR is still an ASCII " - " on both sides — the fold is a
// comparison, not a rewrite, and the album that is kept is the one on disk.
func TestEnDashIsNotWorthARename(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "our little angel - EP - ROLE MODEL")
	writeTrack(t, filepath.Join(dir, "01 - ROLE MODEL - alive.flac"),
		tagsFor("our little angel \u2013 EP", "ROLE MODEL", "alive", "1"))
	writeTrack(t, filepath.Join(dir, "02 - ROLE MODEL - blind \u2014 reprise.flac"),
		tagsFor("our little angel \u2013 EP", "ROLE MODEL", "blind \u2014 reprise", "2"))
	if err := os.WriteFile(filepath.Join(dir, "cover.jpg"), []byte("jpeg"), 0o644); err != nil {
		t.Fatal(err)
	}
	p := scan(t, root)
	if !p.Empty() {
		t.Errorf("an en dash moved the album: %v", moveMap(t, p))
	}
	if p.TracksRecopied != 0 {
		t.Errorf("TracksRecopied = %d, want 0", p.TracksRecopied)
	}

	// A file whose name differs by CASE as well is still a rename: the device
	// shows the folder's own spelling, so that one is worth the re-copy.
	writeTrack(t, filepath.Join(dir, "03 - role model - GOING OUT.flac"),
		tagsFor("our little angel \u2013 EP", "ROLE MODEL", "going out", "3"))
	p = scan(t, root)
	want := map[string]string{
		"our little angel - EP - ROLE MODEL/03 - role model - GOING OUT.flac": "our little angel - EP - ROLE MODEL/03 - ROLE MODEL - going out.flac",
	}
	if got := moveMap(t, p); !sameMap(got, want) {
		t.Errorf("moves =\n%s\nwant\n%s", dump(got), dump(want))
	}
}

// TestCurlyQuoteIsNotWorthARename is the real library's case: the MC tree's
// folders and files spell "I'm The Problem" with an ASCII apostrophe and the
// tags spell it with U+2019. NormKey folds the two together, so the device's
// locator hash cannot tell them apart and the index resolves either — but the
// FAT names would all change, and that is a 790 MB re-copy for a typographic
// difference nobody asked about. So it is not a rename.
func TestCurlyQuoteIsNotWorthARename(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "I'm The Problem - Morgan Wallen")
	writeTrack(t, filepath.Join(dir, "01 - Morgan Wallen - I'm The Problem.flac"),
		tagsFor("I’m The Problem", "Morgan Wallen", "I’m The Problem", "1"))
	if err := os.WriteFile(filepath.Join(dir, "cover.jpg"), []byte("jpeg"), 0o644); err != nil {
		t.Fatal(err)
	}
	p := scan(t, root)
	if !p.Empty() {
		t.Errorf("a curly apostrophe moved the album: %v", moveMap(t, p))
	}
	// A difference the hash CAN see is still a rename.
	writeTrack(t, filepath.Join(dir, "02 - morgan wallen - i got better.flac"),
		tagsFor("I’m The Problem", "Morgan Wallen", "I Got Better", "2"))
	p = scan(t, root)
	if got, want := len(p.Moves), 1; got != want {
		t.Fatalf("moves = %v, want the lower-case one", moveMap(t, p))
	}
	if got, want := rel(p.Root, p.Moves[0].To),
		"I'm The Problem - Morgan Wallen/02 - Morgan Wallen - I Got Better.flac"; got != want {
		t.Errorf("target = %q, want %q (the folder keeps its own spelling)", got, want)
	}
}

// TestMissingTagsAreQuestionsNotGuesses. Decision 5: the tags are the truth
// this app is about to trust, so a file that does not carry one is a question
// for a human and is not touched.
func TestMissingTagsAreQuestionsNotGuesses(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "mixed")
	writeTrack(t, filepath.Join(dir, "ok.flac"), tagsFor("Album", "Artist", "Fine", "1"))
	writeTrack(t, filepath.Join(dir, "notitle.flac"), map[string]string{
		"album": "Album", "artist": "Artist", "tracknumber": "2"})
	writeTrack(t, filepath.Join(dir, "notrack.flac"), map[string]string{
		"album": "Album", "artist": "Artist", "title": "Numberless"})
	writeTrack(t, filepath.Join(dir, "noalbum.flac"), map[string]string{
		"artist": "Artist", "title": "Homeless", "tracknumber": "4"})
	writeTrack(t, filepath.Join(dir, "noartist.flac"), map[string]string{
		"album": "Album", "title": "Anonymous", "tracknumber": "5"})
	if err := os.WriteFile(filepath.Join(dir, "broken.flac"), []byte("not a flac at all"), 0o644); err != nil {
		t.Fatal(err)
	}

	p := scan(t, root)
	for _, tc := range []struct{ file, want string }{
		{"mixed/notitle.flac", "no title tag"},
		{"mixed/notrack.flac", "no usable track number"},
		{"mixed/noalbum.flac", "no album tag"},
		{"mixed/noartist.flac", "no artist tag"},
		{"mixed/broken.flac", "cannot be read"},
	} {
		got := attentionOn(p, p.Root, tc.file)
		if !strings.Contains(got, tc.want) {
			t.Errorf("%s: attention %q, want it to mention %q", tc.file, got, tc.want)
		}
		for _, m := range p.Moves {
			if rel(p.Root, m.From) == tc.file {
				t.Errorf("%s needs attention but the plan moves it", tc.file)
			}
		}
	}
	if len(p.Moves) != 1 {
		t.Errorf("moves = %v, want only the one good file", moveMap(t, p))
	}
}

// TestTwoFilesClaimingOneTrackNumber: neither moves, both are reported. Two
// files called "05 - …" would still sort into a stable order, but the second
// one takes the position the sixth track's number claims, and the gutter on
// the device stops matching the filename.
func TestTwoFilesClaimingOneTrackNumber(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	writeTrack(t, filepath.Join(dir, "a.flac"), tagsFor("Album", "Artist", "First Five", "5"))
	writeTrack(t, filepath.Join(dir, "b.flac"), tagsFor("Album", "Artist", "Other Five", "5"))
	writeTrack(t, filepath.Join(dir, "c.flac"), tagsFor("Album", "Artist", "Six", "6"))

	p := scan(t, root)
	if len(p.Moves) != 1 || rel(p.Root, p.Moves[0].From) != "Album - Artist/c.flac" {
		t.Errorf("moves = %v, want only the unambiguous track", moveMap(t, p))
	}
	for _, f := range []string{"Album - Artist/a.flac", "Album - Artist/b.flac"} {
		if got := attentionOn(p, p.Root, f); !strings.Contains(got, "track 5 is claimed") {
			t.Errorf("%s: attention %q", f, got)
		}
	}
}

// TestNeverOverwrites: a target that already holds a different file is a
// question, not a rename. This is the reviewer-checklist line.
func TestNeverOverwrites(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	writeTrack(t, filepath.Join(dir, "junk.flac"), tagsFor("Album", "Artist", "Song", "1"))
	// Something else is already sitting on the canonical name, and it has no
	// tags, so it is not going anywhere either.
	writeTrack(t, filepath.Join(dir, "01 - Artist - Song.flac"), map[string]string{})

	p := scan(t, root)
	for _, m := range p.Moves {
		if rel(p.Root, m.From) == "Album - Artist/junk.flac" {
			t.Errorf("junk.flac would overwrite an existing file: %v", m)
		}
	}
	if got := attentionOn(p, p.Root, "Album - Artist/junk.flac"); !strings.Contains(got, "already there") {
		t.Errorf("attention on junk.flac = %q", got)
	}
}

// TestTargetFreedByAnotherMove: a name that is taken by a file which is
// itself moving away is not a collision — it is an ordering problem, and the
// plan is ordered so no rename ever lands on an occupied name.
func TestTargetFreedByAnotherMove(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	writeTrack(t, filepath.Join(dir, "junk.flac"), tagsFor("Album", "Artist", "Song", "1"))
	writeTrack(t, filepath.Join(dir, "01 - Artist - Song.flac"), tagsFor("Album", "Artist", "Something Else", "9"))

	p := scan(t, root)
	if len(p.Moves) != 2 {
		t.Fatalf("moves = %v, want two", moveMap(t, p))
	}
	if filepath.Base(p.Moves[0].From) != "01 - Artist - Song.flac" {
		t.Errorf("the occupied name is renamed second: %v", moveMap(t, p))
	}
	if _, err := Apply(context.Background(), p, Options{Root: root, JournalDir: t.TempDir()}); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	for _, name := range []string{"01 - Artist - Song.flac", "09 - Artist - Something Else.flac"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Errorf("%s: %v", name, err)
		}
	}
}

// TestTwoFilesOntoOneName: same album, same track number is covered above;
// this is the other way in — two different track numbers whose FAT-safe names
// collide. Neither moves.
func TestTwoFilesOntoOneName(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	// "?" and "*" are both FAT-illegal and both become "_".
	writeTrack(t, filepath.Join(dir, "a.flac"), tagsFor("Album", "Artist", "Why?", "1"))
	writeTrack(t, filepath.Join(dir, "b.flac"), tagsFor("Album", "Artist", "Why*", "1"))

	p := scan(t, root)
	if len(p.Moves) != 0 {
		t.Errorf("moves = %v, want none", moveMap(t, p))
	}
	if got := attentionOn(p, p.Root, "Album - Artist/a.flac"); got == "" {
		t.Error("a.flac got no attention line")
	}
}

// TestAFileThatStaysBlocksItsName: the file sitting on the canonical name
// needs attention, so it is not going anywhere — and that makes the name
// unavailable. This is the fixpoint in resolveCollisions.
func TestAFileThatStaysBlocksItsName(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	writeTrack(t, filepath.Join(dir, "01 - Artist - Song.flac"), map[string]string{
		"album": "Album", "artist": "Artist", "title": "Song"}) // no track number
	writeTrack(t, filepath.Join(dir, "junk.flac"), tagsFor("Album", "Artist", "Song", "1"))

	p := scan(t, root)
	if len(p.Moves) != 0 {
		t.Errorf("moves = %v, want none", moveMap(t, p))
	}
	if got := attentionOn(p, p.Root, "Album - Artist/junk.flac"); !strings.Contains(got, "already there") {
		t.Errorf("attention on junk.flac = %q", got)
	}
}

// TestTwoFilesSwappingNames is the cycle: each wants the name the other has.
// The plan breaks it with one hop through a temporary name, and the apply and
// the undo both come out right.
func TestTwoFilesSwappingNames(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	writeTrack(t, filepath.Join(dir, "01 - Artist - Alpha.flac"), tagsFor("Album", "Artist", "Beta", "2"))
	writeTrack(t, filepath.Join(dir, "02 - Artist - Beta.flac"), tagsFor("Album", "Artist", "Alpha", "1"))
	before := map[string]string{}
	for _, n := range []string{"01 - Artist - Alpha.flac", "02 - Artist - Beta.flac"} {
		b, err := os.ReadFile(filepath.Join(dir, n))
		if err != nil {
			t.Fatal(err)
		}
		before[n] = string(b)
	}

	p := scan(t, root)
	if len(p.Moves) != 3 {
		t.Fatalf("moves = %v, want three (one of them the temporary hop)", moveMap(t, p))
	}
	j, err := Apply(context.Background(), p, Options{Root: root, JournalDir: t.TempDir()})
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	for n, want := range before {
		other := "01 - Artist - Alpha.flac"
		if n == other {
			other = "02 - Artist - Beta.flac"
		}
		b, err := os.ReadFile(filepath.Join(dir, other))
		if err != nil {
			t.Fatalf("%s: %v", other, err)
		}
		if string(b) != want {
			t.Errorf("%s did not end up holding what %s held", other, n)
		}
	}
	if _, err := Undo(context.Background(), j.Path, Options{Root: root}); err != nil {
		t.Fatalf("Undo: %v", err)
	}
	for n, want := range before {
		b, err := os.ReadFile(filepath.Join(dir, n))
		if err != nil || string(b) != want {
			t.Errorf("undo did not put %s back (%v)", n, err)
		}
	}
}

// TestCaseOnlyRename runs with the Windows rule on: the two names are the same
// file, so the plan still has to emit the rename and the apply has to go
// through a temporary name.
func TestCaseOnlyRename(t *testing.T) {
	defer withCaseFolding(t, true)()
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	writeTrack(t, filepath.Join(dir, "01 - Artist - song.flac"), tagsFor("Album", "Artist", "Song", "1"))

	p := scan(t, root)
	if len(p.Moves) != 1 {
		t.Fatalf("moves = %v, want one", moveMap(t, p))
	}
	if got, want := rel(p.Root, p.Moves[0].To), "Album - Artist/01 - Artist - Song.flac"; got != want {
		t.Fatalf("target = %q, want %q", got, want)
	}
	if _, err := Apply(context.Background(), p, Options{Root: root, JournalDir: t.TempDir()}); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "01 - Artist - Song.flac")); err != nil {
		t.Fatalf("after apply: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "01 - Artist - Song.flac.core-tmp")); err == nil {
		t.Error("the two-step rename left its temporary name behind")
	}
}

// TestSameFileUnderTwoSpellings is the decision the case-only rename rests on:
// with case folding on, two spellings of one path are one file; two different
// paths never are, however alike their contents.
func TestSameFileUnderTwoSpellings(t *testing.T) {
	defer withCaseFolding(t, true)()
	root := t.TempDir()
	writeTrack(t, filepath.Join(root, "a.flac"), tagsFor("A", "B", "C", "1"))
	writeTrack(t, filepath.Join(root, "b.flac"), tagsFor("A", "B", "C", "1"))
	fa, err := os.Lstat(filepath.Join(root, "a.flac"))
	if err != nil {
		t.Fatal(err)
	}
	fb, err := os.Lstat(filepath.Join(root, "b.flac"))
	if err != nil {
		t.Fatal(err)
	}
	if !sameTarget(filepath.Join(root, "a.flac"), filepath.Join(root, "A.FLAC"), fa, fa) {
		t.Error("two spellings of one path are not recognised as the same file")
	}
	if sameTarget(filepath.Join(root, "a.flac"), filepath.Join(root, "b.flac"), fa, fb) {
		t.Error("two different files were taken for one")
	}
}

// TestSidecarsAndStrangersStayPut: the album pictures follow their tracks;
// everything else — a .FLAC the index cannot see, a text file — is left
// exactly where it is.
func TestSidecarsAndStrangersStayPut(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "wrong name")
	writeTrack(t, filepath.Join(dir, "1.flac"), tagsFor("Album", "Artist", "Song", "1"))
	for _, name := range []string{"folder.art", "folder.thm", "cover.jpg", "Album.m3u", "notes.txt", "Bonus.FLAC"} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(name), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	p := scan(t, root)
	want := map[string]string{
		"wrong name/1.flac":     "Album - Artist/01 - Artist - Song.flac",
		"wrong name/folder.art": "Album - Artist/folder.art",
		"wrong name/folder.thm": "Album - Artist/folder.thm",
		"wrong name/cover.jpg":  "Album - Artist/cover.jpg",
		"wrong name/Album.m3u":  "Album - Artist/Album.m3u",
	}
	if got := moveMap(t, p); !sameMap(got, want) {
		t.Errorf("moves =\n%s\nwant\n%s", dump(got), dump(want))
	}
	if len(p.Warnings) != 1 || !strings.Contains(p.Warnings[0], "Bonus.FLAC") {
		t.Errorf("warnings = %v, want one about the upper-case extension", p.Warnings)
	}
	if _, err := Apply(context.Background(), p, Options{Root: root, JournalDir: t.TempDir()}); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	for _, name := range []string{"notes.txt", "Bonus.FLAC"} {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Errorf("%s was moved or removed: %v", name, err)
		}
	}
	// The folder still has files in it, so it stays.
	if _, err := os.Stat(dir); err != nil {
		t.Errorf("a folder that still has files in it was removed: %v", err)
	}
}

// TestHalfOrganizedFolderKeepsItsArt: a track left behind (it needs attention)
// means the folder is still an album, so its pictures stay with it.
func TestHalfOrganizedFolderKeepsItsArt(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "wrong name")
	writeTrack(t, filepath.Join(dir, "1.flac"), tagsFor("Album", "Artist", "Song", "1"))
	writeTrack(t, filepath.Join(dir, "2.flac"), map[string]string{
		"album": "Album", "artist": "Artist", "tracknumber": "2"}) // no title
	if err := os.WriteFile(filepath.Join(dir, "folder.art"), []byte("art"), 0o644); err != nil {
		t.Fatal(err)
	}
	p := scan(t, root)
	for _, m := range p.Moves {
		if strings.HasSuffix(m.From, "folder.art") {
			t.Errorf("the art left a folder that still holds a track: %v", m)
		}
	}
}

// TestSplitArtistVoteIsAQuestion: library gives every track in a folder ONE
// artist, so an album whose tags cannot agree on one is not guessed at.
func TestSplitArtistVoteIsAQuestion(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "dump")
	writeTrack(t, filepath.Join(dir, "a.flac"), tagsFor("Split", "Alice", "One", "1"))
	writeTrack(t, filepath.Join(dir, "b.flac"), tagsFor("Split", "Bob", "Two", "2"))
	p := scan(t, root)
	if len(p.Moves) != 0 {
		t.Errorf("moves = %v, want none", moveMap(t, p))
	}
	if got := attentionOn(p, p.Root, "dump/a.flac"); !strings.Contains(got, "split") {
		t.Errorf("attention = %q", got)
	}
}

// TestAlbumArtistWins: a compilation's tracks each have their own artist, and
// albumartist is what the folder (and so the device) is named after.
func TestAlbumArtistWins(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "dump")
	for i, a := range []string{"Alice", "Bob"} {
		tg := tagsFor("Comp", a, fmt.Sprintf("Song %d", i+1), fmt.Sprintf("%d", i+1))
		tg["albumartist"] = "Various Artists"
		writeTrack(t, filepath.Join(dir, fmt.Sprintf("%d.flac", i)), tg)
	}
	p := scan(t, root)
	want := map[string]string{
		"dump/0.flac": "Comp - Various Artists/01 - Various Artists - Song 1.flac",
		"dump/1.flac": "Comp - Various Artists/02 - Various Artists - Song 2.flac",
	}
	if got := moveMap(t, p); !sameMap(got, want) {
		t.Errorf("moves =\n%s\nwant\n%s", dump(got), dump(want))
	}
}

// TestRecopyCostCountsTheNeighbours: a file that does not move can still change
// its device name, because the name carries its POSITION in the folder. Here
// one track is renamed and the two after it shift up.
func TestRecopyCostCountsTheNeighbours(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "Album - Artist")
	writeTrack(t, filepath.Join(dir, "zz first.flac"), tagsFor("Album", "Artist", "First", "1"))
	writeTrack(t, filepath.Join(dir, "02 - Artist - Second.flac"), tagsFor("Album", "Artist", "Second", "2"))
	writeTrack(t, filepath.Join(dir, "03 - Artist - Third.flac"), tagsFor("Album", "Artist", "Third", "3"))

	p := scan(t, root)
	if len(p.Moves) != 1 {
		t.Fatalf("moves = %v, want one", moveMap(t, p))
	}
	// Before: "zz first" sorts last, so it is 03 and the others are 01, 02.
	// After: it is 01 and the others are 02, 03 — all three re-copy.
	if p.TracksRecopied != 3 {
		t.Errorf("TracksRecopied = %d, want 3 (the rename plus the two it shifts)", p.TracksRecopied)
	}
}

// TestOrganizeThenScanIsTheLocatorContract is the load-bearing test of the
// whole slice: after organizing, library.ScanTree must name exactly the files
// that exist, take every track number from the tags, and encode into an index
// that decodes back to the same titles and numbers.
func TestOrganizeThenScanIsTheLocatorContract(t *testing.T) {
	root := t.TempDir()
	// A junk-named album, a nested one, and a two-disc one.
	junk := filepath.Join(root, "wallen dump")
	titles := []string{"Whiskey Glasses", "Up Down", "Talkin' Tennessee", "Ghost - Live"}
	for i, title := range titles {
		writeTrack(t, filepath.Join(junk, fmt.Sprintf("x%d.flac", len(titles)-i)),
			tagsFor("If I Know Me", "Morgan Wallen", title, fmt.Sprintf("%d", i+1)))
	}
	nested := filepath.Join(root, "Downloads", "dl")
	writeTrack(t, filepath.Join(nested, "only.flac"), tagsFor("DOA", "ericdoa", "sad4whatever", "1"))
	flat := filepath.Join(root, "two disc")
	for i, tc := range []struct{ title, track, disc string }{
		{"Alpha", "1", "1"}, {"Beta", "2", "1"}, {"Gamma", "1", "2"}, {"Delta", "2", "2"},
	} {
		tg := tagsFor("Double", "Band", tc.title, tc.track)
		tg["discnumber"] = tc.disc
		writeTrack(t, filepath.Join(flat, fmt.Sprintf("%d.flac", i)), tg)
	}

	p := scan(t, root)
	if _, err := Apply(context.Background(), p, Options{Root: root, JournalDir: t.TempDir()}); err != nil {
		t.Fatalf("Apply: %v", err)
	}

	sc, err := library.ScanTree(root, library.Options{})
	if err != nil {
		t.Fatalf("ScanTree: %v", err)
	}
	if len(sc.Albums) != 3 {
		t.Fatalf("albums = %d, want 3: %v", len(sc.Albums), sc.Skipped)
	}
	total := 0
	for i := range sc.Albums {
		a := &sc.Albums[i]
		multiDisc := strings.Contains(a.Tracks[0].SrcPath, string(filepath.Separator)+"Disc ")
		for _, tr := range a.Tracks {
			// A flat multi-disc album counts too: its position keeps
			// counting across the discs, so pos is not the track number
			// there either.
			if tr.Disc != a.Tracks[0].Disc {
				multiDisc = true
			}
		}
		for _, tr := range a.Tracks {
			total++
			m, err := flac.ReadFile(tr.SrcPath)
			if err != nil {
				t.Fatal(err)
			}
			tagTitle := m.Tag("title")
			tagTrack := library.LeadInt(m.Tag("tracknumber"))
			if want := fmt.Sprintf("%02d. %s.flac", tr.Pos, library.FatSafe(tagTitle)); tr.DeviceName != want {
				t.Errorf("%s: DeviceName = %q, want %q", tr.SrcPath, tr.DeviceName, want)
			}
			if tr.NumberFrom != "tag" {
				t.Errorf("%s: NumberFrom = %q, want tag", tr.SrcPath, tr.NumberFrom)
			}
			if tr.Track != tagTrack {
				t.Errorf("%s: Track = %d, want the tag's %d", tr.SrcPath, tr.Track, tagTrack)
			}
			if tr.Title != tagTitle {
				t.Errorf("%s: Title = %q, want %q", tr.SrcPath, tr.Title, tagTitle)
			}
			// Within one disc the enumeration position IS the track number:
			// zero-padded NN in byte order is track order. Across discs the
			// position keeps counting, which is what the device wants.
			if !multiDisc && tr.Pos != tr.Track {
				t.Errorf("%s: pos %d != track %d", tr.SrcPath, tr.Pos, tr.Track)
			}
		}
	}
	if len(sc.Drift) != 0 && !hasMultiDisc(sc) {
		t.Errorf("drift after organizing a single-disc tree: %v", sc.Drift)
	}

	// And the index the device reads decodes back to the same thing.
	recs := cidx.RecordsFromScan(sc)
	if len(recs) != total {
		t.Fatalf("records = %d, tracks = %d", len(recs), total)
	}
	back, err := cidx.Decode(cidx.Encode(recs))
	if err != nil {
		t.Fatalf("Decode: %v", err)
	}
	for i, r := range back {
		if r.Title != recs[i].Title || r.Track != recs[i].Track || r.File != recs[i].File {
			t.Errorf("record %d round-tripped to %+v, want %+v", i, r, recs[i])
		}
		if r.FileHash != library.NameHash(r.File) {
			t.Errorf("record %d: the locator hash does not name the file", i)
		}
	}
}

func hasMultiDisc(s *library.Scan) bool {
	for i := range s.Albums {
		for _, tr := range s.Albums[i].Tracks {
			if tr.Disc > 1 {
				return true
			}
		}
	}
	return false
}

// withCaseFolding flips the Windows/macOS path rule on for one test and gives
// back the restore.
func withCaseFolding(t *testing.T, on bool) func() {
	t.Helper()
	old := caseFoldPaths
	caseFoldPaths = on
	return func() { caseFoldPaths = old }
}

func sameMap(a, b map[string]string) bool {
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

func dump(m map[string]string) string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var b strings.Builder
	for _, k := range keys {
		fmt.Fprintf(&b, "  %s -> %s\n", k, m[k])
	}
	return b.String()
}
