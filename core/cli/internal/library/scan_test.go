package library

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// fakeMeta is a metadata reader driven by a table, so the scan rules can be
// tested without writing a real FLAC for every case. The end-to-end fixtures
// (real FLACs, real ffprobe, byte parity with the reference tool) live in
// internal/cli/index_test.go.
func fakeMeta(tags map[string]map[string]string, secs map[string]int) func(string) (*flac.Meta, error) {
	return func(path string) (*flac.Meta, error) {
		base := filepath.Base(path)
		t, ok := tags[base]
		if !ok {
			return nil, fmt.Errorf("no tags for %s", base)
		}
		return &flac.Meta{
			Info: flac.StreamInfo{SampleRate: 44100, TotalSamples: uint64(secs[base])*44100 + 4},
			Tags: t,
		}, nil
	}
}

func writeTree(t *testing.T, root string, files []string) {
	t.Helper()
	for _, rel := range files {
		p := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("fLaC"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

// TestScanTreeEnumerationAndOrder pins the two rules that used to be conflated:
// the device FILENAME is the position in the enumeration (the locator contract
// with the files already on the disk), and the TRACK NUMBER comes from the tags
// — and the records are written in (disc, track) order, not enumeration order.
func TestScanTreeEnumerationAndOrder(t *testing.T) {
	src := t.TempDir()
	writeTree(t, src, []string{
		"Album One - Artist A/Alpha.flac",
		"Album One - Artist A/Bravo.flac",
		"Album One - Artist A/Charlie.flac",
		"Boxed - Band/Disc 1/x.flac",
		"Boxed - Band/Disc 10/z.flac",
		"Boxed - Band/Disc 2/y.flac",
		"NoSeparator/lonely.flac",
		"Empty - Artist/cover.jpg",
	})
	meta := fakeMeta(map[string]map[string]string{
		"Alpha.flac":   {"title": "Alpha", "tracknumber": "2"},
		"Bravo.flac":   {"title": "Bravo", "tracknumber": "1"},
		"Charlie.flac": {"title": "Charlie", "tracknumber": "3"},
		"x.flac":       {"title": "x", "discnumber": "9"},
		"y.flac":       {"title": "y", "discnumber": "9"},
		"z.flac":       {"title": "z", "discnumber": "9"},
	}, map[string]int{"Alpha.flac": 61, "Bravo.flac": 62, "Charlie.flac": 63})

	scan, err := ScanTree(src, Options{Meta: meta})
	if err != nil {
		t.Fatal(err)
	}
	if len(scan.Albums) != 2 {
		t.Fatalf("got %d albums, want 2 (NoSeparator and the album with no FLACs are skipped)", len(scan.Albums))
	}
	if len(scan.Skipped) != 2 {
		t.Errorf("Skipped = %q, want the two folders that produce nothing", scan.Skipped)
	}

	one := scan.Albums[0]
	if one.DeviceFolder != "Artist A - Album One" {
		t.Fatalf("device folder = %q", one.DeviceFolder)
	}
	var names, titles []string
	for _, tr := range one.Tracks {
		names = append(names, tr.DeviceName)
		titles = append(titles, tr.Title)
	}
	// Filenames keep the lexicographic enumeration; the ORDER is by track.
	if got := strings.Join(names, ","); got != "02. Bravo.flac,01. Alpha.flac,03. Charlie.flac" {
		t.Errorf("record order/names = %s", got)
	}
	if got := strings.Join(titles, ","); got != "Bravo,Alpha,Charlie" {
		t.Errorf("titles = %s", got)
	}
	if one.Tracks[0].DurationS != 62 {
		t.Errorf("duration = %d, want 62", one.Tracks[0].DurationS)
	}

	// "Disc N" folders sort as strings, which is what glob + sorted() give:
	// Disc 1, Disc 10, Disc 2. The enumeration position follows that order,
	// and the folder's number beats the (wrong) disc tag.
	boxed := scan.Albums[1]
	var discs []string
	for _, tr := range boxed.Tracks {
		discs = append(discs, fmt.Sprintf("%s=d%dt%d", tr.DeviceName, tr.Disc, tr.Track))
	}
	if got := strings.Join(discs, " "); got != "01. x.flac=d1t1 03. y.flac=d2t3 02. z.flac=d10t2" {
		t.Errorf("disc folders: %s", got)
	}
}

// TestScanTreeDriftIsReported: a track whose number is not its enumeration
// position is the mechanism behind "the device and the reference client
// disagree on N tracks", so the scan has to be able to name them.
func TestScanTreeDriftIsReported(t *testing.T) {
	src := t.TempDir()
	writeTree(t, src, []string{
		"Numbers - Prince/1999.flac",
		"Numbers - Prince/2-05 Title.flac",
		"Numbers - Prince/7 rings.flac",
	})
	meta := fakeMeta(map[string]map[string]string{
		"1999.flac":       {"title": "1999"},
		"2-05 Title.flac": {"title": "Title"},
		"7 rings.flac":    {"title": "7 rings"},
	}, nil)
	scan, err := ScanTree(src, Options{Meta: meta})
	if err != nil {
		t.Fatal(err)
	}
	var got []string
	for _, d := range scan.Drift {
		got = append(got, fmt.Sprintf("%s %d/%d %s", d.DeviceName, d.Disc, d.Track, d.NumberFrom))
	}
	// Only the flattened multi-disc name drifts: "1999" and "7 rings" take
	// their positions (1 and 3), which IS their position, so they do not.
	want := []string{"02. 2-05 Title.flac 2/5 filename"}
	if strings.Join(got, "|") != strings.Join(want, "|") {
		t.Errorf("drift = %q, want %q", got, want)
	}
	// "1999" is a title, not track 1999; "7 rings" is not track 7.
	byName := map[string]Track{}
	for _, tr := range scan.Albums[0].Tracks {
		byName[tr.DeviceName] = tr
	}
	if tr := byName["01. 1999.flac"]; tr.Track != 1 || tr.NumberFrom != "position" {
		t.Errorf("1999 = track %d (%s), want 1 (position)", tr.Track, tr.NumberFrom)
	}
	if tr := byName["03. 7 rings.flac"]; tr.Track != 3 || tr.NumberFrom != "position" {
		t.Errorf("7 rings = track %d (%s), want 3 (position)", tr.Track, tr.NumberFrom)
	}
}

// TestScanTreeAlbumDisplayName: the folder on disk is FAT-safe, the record
// shows the real album name. The FIRST track decides, and only when the tag
// album differs from the folder's by sanitization alone.
func TestScanTreeAlbumDisplayName(t *testing.T) {
	src := t.TempDir()
	writeTree(t, src, []string{
		"F_CK LOVE 3+_ OVER YOU - The Kid LAROI/a.flac",
		"Don’t Stop - Curly/b.flac",
		"Whatever - Other/c.flac",
	})
	meta := fakeMeta(map[string]map[string]string{
		"a.flac": {"title": "Stay", "album": "F*CK LOVE 3+: OVER YOU"},
		"b.flac": {"title": "Don’t", "album": "Don’t Stop"},
		"c.flac": {"title": "c", "album": "A Completely Different Album"},
	}, nil)
	scan, err := ScanTree(src, Options{Meta: meta})
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]string{
		"The Kid LAROI - F_CK LOVE 3+_ OVER YOU": "The Kid LAROI - F*CK LOVE 3+: OVER YOU",
		"Curly - Don’t Stop":                     "Curly - Don't Stop",
		"Other - Whatever":                       "Other - Whatever",
	}
	for i := range scan.Albums {
		a := &scan.Albums[i]
		if w, ok := want[a.DeviceFolder]; !ok {
			t.Errorf("unexpected album %q", a.DeviceFolder)
		} else if a.DisplayFolder != w {
			t.Errorf("%q displays as %q, want %q", a.DeviceFolder, a.DisplayFolder, w)
		}
	}
}

// TestScanTreeGenreRules: a mapped artist beats the tag; an unmapped artist
// keeps only the first comma-part; a single-valued tag is kept as is.
func TestScanTreeGenreRules(t *testing.T) {
	src := t.TempDir()
	writeTree(t, src, []string{
		"A - Mapped Artist/x.flac",
		"B - Comma Artist/y.flac",
		"C - Plain Artist/z.flac",
	})
	meta := fakeMeta(map[string]map[string]string{
		"x.flac": {"title": "x", "genre": "Metal, Death"},
		"y.flac": {"title": "y", "genre": "Hip-Hop, Rap"},
		"z.flac": {"title": "z", "genre": "Rock"},
	}, nil)
	scan, err := ScanTree(src, Options{Meta: meta, GenreMap: map[string]string{"Mapped Artist": "Pop"}})
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]string{"Mapped Artist": "Pop", "Comma Artist": "Hip-Hop", "Plain Artist": "Rock"}
	for i := range scan.Albums {
		a := &scan.Albums[i]
		if got := a.Tracks[0].Genre; got != want[a.FolderArtist] {
			t.Errorf("%s: genre %q, want %q", a.FolderArtist, got, want[a.FolderArtist])
		}
	}
	if n := scan.GenreCount(); n != 3 {
		t.Errorf("GenreCount = %d, want 3", n)
	}
}

// TestScanTreeUppercaseExtension: glob("*.flac") is case-sensitive, so the
// reference tool leaves an uppercase ".FLAC" out of the index. We do the same
// — and say so, because a track that is silently not in the library is the
// symptom nobody can explain later.
func TestScanTreeUppercaseExtension(t *testing.T) {
	src := t.TempDir()
	writeTree(t, src, []string{"Case - Artist/a.flac", "Case - Artist/B.FLAC", "Case - Artist/.hidden.flac"})
	meta := fakeMeta(map[string]map[string]string{"a.flac": {"title": "a"}}, nil)
	scan, err := ScanTree(src, Options{Meta: meta})
	if err != nil {
		t.Fatal(err)
	}
	if n := len(scan.Albums[0].Tracks); n != 1 {
		t.Fatalf("%d tracks, want 1 (B.FLAC and the dotfile are not matched by glob)", n)
	}
	if len(scan.Warnings) != 1 || !strings.Contains(scan.Warnings[0], "B.FLAC") {
		t.Errorf("Warnings = %q, want one naming B.FLAC", scan.Warnings)
	}
}

// TestScanTreeRecordsAMetadataFailure: an unreadable file must not abort the
// build, but it must be reported — its record is indistinguishable from a
// healthy one on the device (duration 0, filename-derived title), so this list
// is the only warning there is.
func TestScanTreeRecordsAMetadataFailure(t *testing.T) {
	src := t.TempDir()
	writeTree(t, src, []string{"Broken - Artist/Not Audio.flac"})
	scan, err := ScanTree(src, Options{})
	if err != nil {
		t.Fatal(err)
	}
	if len(scan.Failures) != 1 {
		t.Fatalf("Failures = %+v, want one", scan.Failures)
	}
	tr := scan.Albums[0].Tracks[0]
	if tr.DurationS != 0 || tr.Title != "Not Audio" || tr.DeviceName != "01. Not Audio.flac" {
		t.Errorf("failed track = %+v; want the filename-derived fallback", tr)
	}
}

// TestScanTreeDuplicateTitlesKeepDistinctNames: two files with the same title
// in one album (only possible across Disc folders, since a directory cannot
// hold two identical names) still get distinct device names, because NN is the
// enumeration position. The reference tool's de-duplication suffix therefore
// never fires — and if it ever did, its loop would not terminate, which is why
// this side bounds it.
func TestScanTreeDuplicateTitlesKeepDistinctNames(t *testing.T) {
	src := t.TempDir()
	writeTree(t, src, []string{
		"Boxed - Band/Disc 1/Same Title.flac",
		"Boxed - Band/Disc 2/Same Title.flac",
	})
	meta := fakeMeta(map[string]map[string]string{"Same Title.flac": {"title": "Same Title"}}, nil)
	scan, err := ScanTree(src, Options{Meta: meta})
	if err != nil {
		t.Fatal(err)
	}
	got := []string{scan.Albums[0].Tracks[0].DeviceName, scan.Albums[0].Tracks[1].DeviceName}
	if got[0] != "01. Same Title.flac" || got[1] != "02. Same Title.flac" {
		t.Errorf("device names = %q", got)
	}
	if NameHash(got[0]) == NameHash(got[1]) {
		t.Error("two tracks share a locator hash")
	}
}

// TestScanTreeMissingSource names the failure the way the reference tool does.
func TestScanTreeMissingSource(t *testing.T) {
	_, err := ScanTree(filepath.Join(t.TempDir(), "nope"), Options{})
	if err == nil || !strings.Contains(err.Error(), "source tree not found") {
		t.Errorf("err = %v, want a 'source tree not found' message", err)
	}
}
