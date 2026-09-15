package cli

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cidx"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

// --- the synthetic tree ------------------------------------------------------
//
// SOURCE folders are "Album - Artist" (the artist is what follows the LAST
// " - "); the index and the device use "Artist - Album". Every entry is a rule
// from tools/build_index.py, and the files are real FLACs, so the reference
// tool can be run over the same tree with the real ffprobe and the two indexes
// compared byte for byte.

type fixtureFile struct {
	rel  string            // path under the source root
	tags map[string]string // Vorbis comments, as written
	secs int               // duration in whole seconds
}

// longTitle is 70 bytes with a 2-byte rune straddling the 47-byte cut of the
// 48-byte title field, so the truncation has to land on a rune boundary.
const longTitle = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" + "\u00e9" + "bbbbbbbbbbbbbbbbbbbbbb"

var fixtureFiles = []fixtureFile{
	// Unnumbered source files, tagged out of alphabetical order: the
	// enumeration is Alpha, Bravo, Charlie; the tags say Bravo is track 1.
	{"Album One - Artist A/Alpha.flac", map[string]string{"TITLE": "Alpha", "ALBUM": "Album One", "TRACKNUMBER": "2"}, 61},
	{"Album One - Artist A/Bravo.flac", map[string]string{"TITLE": "Bravo", "ALBUM": "Album One", "TRACKNUMBER": "1"}, 62},
	{"Album One - Artist A/Charlie.flac", map[string]string{"TITLE": "Charlie", "ALBUM": "Album One", "TRACKNUMBER": "3"}, 63},

	// Leading numbers that are not track numbers, and one that is.
	{"Numbers - Prince/7 rings.flac", map[string]string{"TITLE": "7 rings"}, 64},
	{"Numbers - Prince/1999.flac", map[string]string{"TITLE": "1999"}, 65},
	{"Numbers - Prince/2-05 Title.flac", map[string]string{"TITLE": "Title"}, 66},

	// "Disc N" folders, flattened, with a (wrong) disc tag the folder beats —
	// and the same title twice, which is the case the de-duplication suffix
	// exists for and never fires on, because NN already makes the names
	// distinct.
	{"Boxed - Band/Disc 1/Same Title.flac", map[string]string{"TITLE": "Same Title", "DISCNUMBER": "9"}, 67},
	{"Boxed - Band/Disc 2/Same Title.flac", map[string]string{"TITLE": "Same Title", "DISCNUMBER": "9"}, 68},

	// Curly apostrophes in the folder, the filename and the tags.
	{"Don\u2019t Stop - Curly Artist/Don\u2019t.flac", map[string]string{"TITLE": "Don\u2019t", "ALBUM": "Don\u2019t Stop"}, 69},

	// A FAT-unsafe album name: the folder carries the sanitized form and the
	// tag the real one, so the record displays the real one while the locator
	// stays FAT-safe.
	{"F_CK LOVE 3+_ OVER YOU - The Kid LAROI/01 - The Kid LAROI - Stay.flac",
		map[string]string{"TITLE": "Stay", "ALBUM": "F*CK LOVE 3+: OVER YOU"}, 70},

	// A mapped artist beats a comma-list tag genre; an unmapped artist keeps
	// the first comma-part.
	{"Mapped - Mapped Artist/x.flac", map[string]string{"TITLE": "x", "GENRE": "Metal, Death"}, 71},
	{"Genre - Comma Artist/y.flac", map[string]string{"TITLE": "y", "GENRE": "Hip-Hop, Rap"}, 72},

	// Control characters in a title, and a title longer than its field.
	{"Odd - Ctrl Artist/c.flac", map[string]string{"TITLE": "Bad\x01Ti\x1ftle"}, 73},
	{"Odd - Ctrl Artist/d.flac", map[string]string{"TITLE": longTitle}, 74},

	// An uppercase extension: glob("*.flac") does not match it, so the
	// reference tool leaves it out of the index and so do we.
	{"Case - Artist C/a.flac", map[string]string{"TITLE": "a"}, 75},
	{"Case - Artist C/B.FLAC", map[string]string{"TITLE": "B"}, 76},

	// A folder with no " - ": not an album folder, skipped entirely.
	{"NoSeparator/lonely.flac", map[string]string{"TITLE": "lonely"}, 77},
}

func writeFixtureTree(t *testing.T, root string) {
	t.Helper()
	for _, f := range fixtureFiles {
		p := filepath.Join(root, filepath.FromSlash(f.rel))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		info := flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16,
			TotalSamples: uint64(f.secs)*44100 + 12345}
		if err := os.WriteFile(p, flac.BuildFile(info, f.tags, nil), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func writeFixtureGenreMap(t *testing.T, path string) {
	t.Helper()
	doc := map[string]any{
		"_comment": []string{"keys are the FOLDER-derived artist, matched exactly"},
		"genres":   map[string]string{"Mapped Artist": "Pop"},
	}
	b, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

// TestIndexSyntheticRecords runs the real command over the tree and checks the
// decoded records field by field, in order.
func TestIndexSyntheticRecords(t *testing.T) {
	root := t.TempDir()
	src := filepath.Join(root, "src")
	out := filepath.Join(root, "nested", "CORELIB.IDX")
	writeFixtureTree(t, src)
	mapPath := filepath.Join(root, "genres.json")
	writeFixtureGenreMap(t, mapPath)

	stdout, stderr, err := runCore(t, "index", "--src", src, "--out", out,
		"--genre-map", mapPath, "--show-drift")
	if err != nil {
		t.Fatalf("core index: %v\n%s\n%s", err, stdout, stderr)
	}

	data, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	recs, err := cidx.Decode(data)
	if err != nil {
		t.Fatalf("the index we just wrote does not validate: %v", err)
	}

	want := []cidx.Record{
		// Album One: records in (disc, track) order, not enumeration order,
		// while the filenames keep the enumeration.
		{DurationS: 62, Track: 1, Disc: 1, Folder: "Artist A - Album One", File: "02. Bravo.flac", Title: "Bravo", Artist: "Artist A"},
		{DurationS: 61, Track: 2, Disc: 1, Folder: "Artist A - Album One", File: "01. Alpha.flac", Title: "Alpha", Artist: "Artist A"},
		{DurationS: 63, Track: 3, Disc: 1, Folder: "Artist A - Album One", File: "03. Charlie.flac", Title: "Charlie", Artist: "Artist A"},
		// The Disc folder beats the disc tag; the enumeration runs
		// continuously across discs; two identical titles keep distinct names.
		{DurationS: 67, Track: 1, Disc: 1, Folder: "Band - Boxed", File: "01. Same Title.flac", Title: "Same Title", Artist: "Band"},
		{DurationS: 68, Track: 2, Disc: 2, Folder: "Band - Boxed", File: "02. Same Title.flac", Title: "Same Title", Artist: "Band"},
		// B.FLAC is not in the index at all.
		{DurationS: 75, Track: 1, Disc: 1, Folder: "Artist C - Case", File: "01. a.flac", Title: "a", Artist: "Artist C"},
		// Curly quotes survive into the record; the display album is
		// straightened, the file name is not.
		{DurationS: 69, Track: 1, Disc: 1, Folder: "Curly Artist - Don't Stop", File: "01. Don\u2019t.flac", Title: "Don\u2019t", Artist: "Curly Artist"},
		// The tag album comes back with its FAT-unsafe characters.
		{DurationS: 70, Track: 1, Disc: 1, Folder: "The Kid LAROI - F*CK LOVE 3+: OVER YOU", File: "01. Stay.flac", Title: "Stay", Artist: "The Kid LAROI"},
		{DurationS: 72, Track: 1, Disc: 1, Folder: "Comma Artist - Genre", File: "01. y.flac", Title: "y", Artist: "Comma Artist", Genre: "Hip-Hop"},
		{DurationS: 71, Track: 1, Disc: 1, Folder: "Mapped Artist - Mapped", File: "01. x.flac", Title: "x", Artist: "Mapped Artist", Genre: "Pop"},
		// Enumeration order in Numbers is 1999, 2-05 Title, 7 rings.
		{DurationS: 65, Track: 1, Disc: 1, Folder: "Prince - Numbers", File: "01. 1999.flac", Title: "1999", Artist: "Prince"},
		{DurationS: 64, Track: 3, Disc: 1, Folder: "Prince - Numbers", File: "03. 7 rings.flac", Title: "7 rings", Artist: "Prince"},
		{DurationS: 66, Track: 5, Disc: 2, Folder: "Prince - Numbers", File: "02. 2-05 Title.flac", Title: "Title", Artist: "Prince"},
		// Control characters dropped; the 70-byte title truncated to 46 bytes
		// because the rune straddling the 47-byte cut goes whole.
		{DurationS: 73, Track: 1, Disc: 1, Folder: "Ctrl Artist - Odd", File: "01. c.flac", Title: "BadTitle", Artist: "Ctrl Artist"},
		{DurationS: 74, Track: 2, Disc: 1, Folder: "Ctrl Artist - Odd", File: "02. d.flac", Title: strings.Repeat("a", 46), Artist: "Ctrl Artist"},
	}
	// The locator hashes are derived, not typed out: they are over the
	// FAT-SAFE folder name (which is not always the displayed one) and the
	// device filename.
	deviceFolder := map[string]string{
		"The Kid LAROI - F*CK LOVE 3+: OVER YOU": "The Kid LAROI - F_CK LOVE 3+_ OVER YOU",
		"Curly Artist - Don't Stop":              "Curly Artist - Don\u2019t Stop",
	}
	for i := range want {
		locator := want[i].Folder
		if d, ok := deviceFolder[locator]; ok {
			locator = d
		}
		want[i].FolderHash = library.NameHash(locator)
		want[i].FileHash = library.NameHash(want[i].File)
	}

	if len(recs) != len(want) {
		t.Fatalf("got %d records, want %d:\n%s", len(recs), len(want), dumpRecords(recs))
	}
	for i := range want {
		if recs[i] != want[i] {
			t.Errorf("record %d:\n got %+v\nwant %+v", i, recs[i], want[i])
		}
	}

	// The summary lines, and the drift report that is the evidence behind
	// "the device and the reference client disagree on N tracks".
	for _, line := range []string{
		"  Artist A - Album One: 3\n",
		"  Artist C - Case: 1\n",
		fmt.Sprintf("\nCORELIB.IDX: 15 songs from 9 albums, %d bytes -> %s\n", len(data), out),
		"3 track(s) are numbered differently from their filename position\n",
		"  Artist A - Album One / 01. Alpha.flac: disc 1 track 2 (tag)\n",
		"  Prince - Numbers / 02. 2-05 Title.flac: disc 2 track 5 (filename)\n",
	} {
		if !strings.Contains(stdout, line) {
			t.Errorf("stdout is missing %q\n%s", line, stdout)
		}
	}
	if !strings.Contains(stderr, "B.FLAC") {
		t.Errorf("nothing warned about the uppercase extension:\n%s", stderr)
	}
}

func dumpRecords(recs []cidx.Record) string {
	var b strings.Builder
	for i, r := range recs {
		fmt.Fprintf(&b, "%2d %+v\n", i, r)
	}
	return b.String()
}

func TestIndexDryRunWritesNothing(t *testing.T) {
	root := t.TempDir()
	src := filepath.Join(root, "src")
	out := filepath.Join(root, "CORELIB.IDX")
	writeFixtureTree(t, src)

	stdout, _, err := runCore(t, "index", "--src", src, "--out", out, "--genre-map", "", "--dry-run", "-q")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(out); !os.IsNotExist(err) {
		t.Errorf("--dry-run wrote %s", out)
	}
	if !strings.Contains(stdout, "15 songs from 9 albums") || !strings.Contains(stdout, "dry run") {
		t.Errorf("dry-run summary = %q", stdout)
	}
	if strings.Contains(stdout, "  Artist A - Album One: 3") {
		t.Error("-q should suppress the per-album lines")
	}
}

func TestIndexRefusesWithoutSourceAndOut(t *testing.T) {
	t.Setenv("CORELIB_SRC", "")
	t.Setenv("CORELIB_OUT", "")
	if _, _, err := runCore(t, "index"); err == nil {
		t.Error("index with no --src/--out must refuse rather than guess")
	}
	if _, _, err := runCore(t, "index", "--src", filepath.Join(t.TempDir(), "nope"), "--out", filepath.Join(t.TempDir(), "x.idx"), "--genre-map", ""); err == nil {
		t.Error("a missing source tree must be an error")
	}
}

// TestIndexCapRefusesAnIndexTheDeviceWouldTruncate: past LIB_MAX_SONGS the
// device loads the first N and drops the rest with no error and no log, so the
// only place this can be caught is here.
func TestIndexCapRefusesAnIndexTheDeviceWouldTruncate(t *testing.T) {
	root := t.TempDir()
	src := filepath.Join(root, "src")
	writeFixtureTree(t, src)
	out := filepath.Join(root, "CORELIB.IDX")
	_, _, err := runCore(t, "index", "--src", src, "--out", out, "--genre-map", "", "--max-songs", "10", "-q")
	if err == nil {
		t.Fatal("15 records with a cap of 10 must fail")
	}
	if !strings.Contains(err.Error(), "LIB_MAX_SONGS") || !strings.Contains(err.Error(), "silently drop 5") {
		t.Errorf("error = %v", err)
	}
	if _, statErr := os.Stat(out); !os.IsNotExist(statErr) {
		t.Error("an index that exceeds the cap must not be written")
	}
}

// TestIndexReportsUnreadableTracks: a file whose metadata cannot be read still
// gets a record (duration 0, filename-derived title) — which is exactly what a
// healthy record looks like from the device's side, so the run must say so and
// exit non-zero.
func TestIndexReportsUnreadableTracks(t *testing.T) {
	root := t.TempDir()
	src := filepath.Join(root, "src", "Broken - Artist")
	if err := os.MkdirAll(src, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(src, "Not Audio.flac"), []byte("nope"), 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(root, "CORELIB.IDX")
	_, stderr, err := runCore(t, "index", "--src", filepath.Join(root, "src"), "--out", out, "--genre-map", "", "-q")
	if err == nil {
		t.Error("a run with unreadable tracks must exit non-zero")
	}
	if !strings.Contains(stderr, "Not Audio.flac") {
		t.Errorf("stderr does not name the file:\n%s", stderr)
	}
	// ...and the index is still written, because one bad file must not cost
	// the other 900 tracks.
	if _, err := os.Stat(out); err != nil {
		t.Errorf("the index was not written: %v", err)
	}
}

// --- parity with the reference implementation --------------------------------

// TestBuildIndexParitySynthetic requires the Go index to be byte-identical to
// tools/build_index.py over the same tree, with the real ffprobe reading the
// same files. Byte-identical is the bar: a mismatch is a bug to find, not a
// tolerance to add.
func TestBuildIndexParitySynthetic(t *testing.T) {
	script, skip := referenceTool()
	if skip != "" {
		t.Skip(skip)
	}
	root := t.TempDir()
	src := filepath.Join(root, "src")
	writeFixtureTree(t, src)
	mapPath := filepath.Join(root, "genres.json")
	writeFixtureGenreMap(t, mapPath)

	pyOut := filepath.Join(root, "python.idx")
	if out, err := runReference(script, src, pyOut, mapPath); err != nil {
		t.Fatalf("build_index.py failed: %v\n%s", err, out)
	}
	goOut := filepath.Join(root, "go.idx")
	if stdout, stderr, err := runCore(t, "index", "--src", src, "--out", goOut, "--genre-map", mapPath, "-q"); err != nil {
		t.Fatalf("core index: %v\n%s\n%s", err, stdout, stderr)
	}
	comparePairs(t, goOut, pyOut)
}

// TestParityMC is the real thing: the 900-odd track library this project is
// built around, both tools run over it, byte for byte. Set CORE_PARITY_SRC to
// the source tree (and CORE_PARITY_IDX to the oracle index, if there is one).
func TestParityMC(t *testing.T) {
	src := os.Getenv("CORE_PARITY_SRC")
	if src == "" {
		t.Skip("CORE_PARITY_SRC not set; the real library is not on this host")
	}
	script, skip := referenceTool()
	if skip != "" {
		t.Skip(skip)
	}
	repo := repoDir()
	mapPath := filepath.Join(repo, "tools", "artist_genres.json")

	root := t.TempDir()
	goOut := filepath.Join(root, "go.idx")
	stdout, stderr, err := runCore(t, "index", "--src", src, "--out", goOut, "--genre-map", mapPath, "-q", "--show-drift")
	if err != nil {
		t.Fatalf("core index: %v\n%s\n%s", err, stdout, stderr)
	}
	t.Logf("core index:\n%s", stdout)

	// A FRESH reference run, not the stale oracle file: the tree may have
	// changed since the oracle was built, and a difference there would look
	// exactly like a bug in this code.
	pyOut := filepath.Join(root, "python.idx")
	if out, err := runReference(script, src, pyOut, mapPath); err != nil {
		t.Fatalf("build_index.py failed on %s: %v\n%s", src, err, out)
	}
	comparePairs(t, goOut, pyOut)

	// The Sep-10 oracle is reported, not asserted: if it differs, the
	// interesting question is what changed in the tree, not whether this code
	// is right — the fresh run above already answered that.
	if oracle := os.Getenv("CORE_PARITY_IDX"); oracle != "" {
		want, err := os.ReadFile(oracle)
		if err != nil {
			t.Logf("oracle %s: %v", oracle, err)
			return
		}
		got, err := os.ReadFile(goOut)
		if err != nil {
			t.Fatal(err)
		}
		if bytes.Equal(got, want) {
			t.Logf("also byte-identical to the oracle %s (%d bytes)", oracle, len(want))
		} else {
			t.Logf("NOT identical to the oracle %s (%d vs %d bytes) — the tree has moved since it was built:\n%s",
				oracle, len(got), len(want), diffIndexes(got, want))
		}
	}
}

func comparePairs(t *testing.T, goPath, pyPath string) {
	t.Helper()
	got, err := os.ReadFile(goPath)
	if err != nil {
		t.Fatal(err)
	}
	want, err := os.ReadFile(pyPath)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("the Go index is not byte-identical to build_index.py:\n%s", diffIndexes(got, want))
	}
	t.Logf("byte-identical to build_index.py: %d bytes, %d records", len(got), (len(got)-16)/256)
}

// referenceTool locates tools/build_index.py and the programs it needs. The
// returned string is a skip reason when the oracle cannot run here.
func referenceTool() (script, skip string) {
	repo := repoDir()
	if repo == "" {
		return "", "firmware repo not found (set CORE_REPO); tools/build_index.py unavailable"
	}
	script = filepath.Join(repo, "tools", "build_index.py")
	if _, err := os.Stat(script); err != nil {
		return "", "tools/build_index.py not found"
	}
	if _, err := exec.LookPath("python3"); err != nil {
		return "", "python3 not on PATH"
	}
	if _, err := exec.LookPath("ffprobe"); err != nil {
		return "", "ffprobe not on PATH (ffmpeg provides it)"
	}
	return script, ""
}

func runReference(script, src, out, genreMap string) (string, error) {
	cmd := exec.Command("python3", script, "--src", src, "--out", out, "--genre-map", genreMap, "-q")
	b, err := cmd.CombinedOutput()
	return string(b), err
}

// repoDir locates the firmware repository: $CORE_REPO if set, else by walking
// up from this source file. "" when it is not there, which makes the parity
// tests skip rather than fail.
func repoDir() string {
	isRepo := func(dir string) bool {
		_, err := os.Stat(filepath.Join(dir, "tools", "build_index.py"))
		return err == nil
	}
	if p := os.Getenv("CORE_REPO"); p != "" {
		if isRepo(p) {
			return p
		}
		return ""
	}
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		return ""
	}
	dir := filepath.Dir(file)
	for {
		if isRepo(dir) {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return ""
		}
		dir = parent
	}
}

// diffIndexes reports the first divergences in terms of records and fields, so
// a parity failure names the rule that broke rather than a byte offset.
func diffIndexes(got, want []byte) string {
	var b strings.Builder
	if len(got) != len(want) {
		fmt.Fprintf(&b, "length: got %d, want %d\n", len(got), len(want))
	}
	if len(got) >= 16 && len(want) >= 16 && !bytes.Equal(got[:16], want[:16]) {
		fmt.Fprintf(&b, "header: got % x\n        want % x\n", got[:16], want[:16])
	}
	const hdr, rec = 16, 256
	n := len(got)
	if len(want) < n {
		n = len(want)
	}
	n = (n - hdr) / rec
	shown := 0
	for i := 0; i < n && shown < 5; i++ {
		g := got[hdr+i*rec : hdr+(i+1)*rec]
		w := want[hdr+i*rec : hdr+(i+1)*rec]
		if bytes.Equal(g, w) {
			continue
		}
		shown++
		fmt.Fprintf(&b, "record %d differs:\n  got  %s\n  want %s\n", i, describeRecord(g), describeRecord(w))
	}
	return b.String()
}

func describeRecord(r []byte) string {
	str := func(b []byte) string {
		if i := bytes.IndexByte(b, 0); i >= 0 {
			b = b[:i]
		}
		return string(b)
	}
	le32 := func(b []byte) uint32 {
		return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
	}
	le16 := func(b []byte) uint16 { return uint16(b[0]) | uint16(b[1])<<8 }
	return fmt.Sprintf("dur=%d trk=%d disc=%d folder=%q file=%q title=%q artist=%q genre=%q fh=%08X filh=%08X",
		le32(r[0:4]), le16(r[4:6]), le16(r[6:8]), str(r[8:72]), str(r[72:136]),
		str(r[136:184]), str(r[184:224]), str(r[224:248]), le32(r[248:252]), le32(r[252:256]))
}
