package library

import (
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"testing"
)

// repoRoot locates the firmware repository: $CORE_REPO if set, else by walking
// up from this source file. Returns "" when it is not there, which is the
// signal for a parity test to skip rather than fail — the Go module can be
// built and tested on its own.
func repoRoot() string {
	isRepo := func(dir string) bool {
		_, err := os.Stat(filepath.Join(dir, "core", "tests", "kernel", "name_hash_vectors.h"))
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

// vecRe is the regex core/tests/scripts/check_name_hash_parity.py parses the
// golden table with, transliterated to RE2. Keeping it identical matters: if
// this one quietly matched fewer vectors, the test would pass by testing less.
var vecRe = regexp.MustCompile(`(?s)NAME_HASH_(VEC|XFAIL)\(\s*"([^"]*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*(0[xX][0-9A-Fa-f]+)u?`)

type hashVector struct {
	kind  string
	label string
	name  string
	want  uint32
}

func parseVectors(t *testing.T, path string) []hashVector {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	// Ignore the two #ifndef guards in the header preamble, as the Python
	// parser does.
	body := strings.SplitN(string(raw), "#endif /* CORE_TESTS", 2)[0]
	var out []hashVector
	for _, m := range vecRe.FindAllStringSubmatch(body, -1) {
		want, err := strconv.ParseUint(m[4][2:], 16, 32)
		if err != nil {
			t.Fatalf("vector %q: bad hash %q: %v", m[2], m[4], err)
		}
		out = append(out, hashVector{kind: m[1], label: m[2], name: unescapeCLiteral(m[3]), want: uint32(want)})
	}
	return out
}

// unescapeCLiteral turns the table's C string literal back into the raw UTF-8
// bytes it stands for. The literal is printable ASCII verbatim with everything
// else as a three-digit octal escape (octal, not hex, because a C hex escape is
// greedy and "\xc3\x89douard" would swallow the 'd').
func unescapeCLiteral(s string) string {
	var b []byte
	for i := 0; i < len(s); {
		c := s[i]
		if c != '\\' {
			b = append(b, c)
			i++
			continue
		}
		i++
		if i >= len(s) {
			break
		}
		switch e := s[i]; {
		case e >= '0' && e <= '7':
			v := 0
			n := 0
			for n < 3 && i < len(s) && s[i] >= '0' && s[i] <= '7' {
				v = v*8 + int(s[i]-'0')
				i++
				n++
			}
			b = append(b, byte(v))
		default:
			i++
			switch e {
			case 'n':
				b = append(b, '\n')
			case 't':
				b = append(b, '\t')
			case 'r':
				b = append(b, '\r')
			case 'a':
				b = append(b, 7)
			case 'b':
				b = append(b, 8)
			case 'f':
				b = append(b, 12)
			case 'v':
				b = append(b, 11)
			default: // \\ \" \' and anything else: the character itself
				b = append(b, e)
			}
		}
	}
	return string(b)
}

// TestNameHashGoldenVectors holds this implementation to the SAME golden table
// the firmware's C name_hash() and tools/build_index.py are held to. The hash
// is the only thing binding an index record to the file on disk: if two
// implementations disagree for some name, the affected track is not reported
// missing, it silently never resolves. That failure is invisible in every other
// test, which is why the table exists.
//
// NAME_HASH_XFAIL marks a vector the C side was once known to get wrong; like
// the Python parity script, this side is held to the CORRECT value either way.
func TestNameHashGoldenVectors(t *testing.T) {
	repo := repoRoot()
	if repo == "" {
		t.Skip("firmware repo not found (set CORE_REPO); golden vectors unavailable")
	}
	vecs := parseVectors(t, filepath.Join(repo, "core", "tests", "kernel", "name_hash_vectors.h"))
	if len(vecs) < 10 {
		t.Fatalf("only parsed %d vectors — the table or the regex is broken", len(vecs))
	}
	for _, v := range vecs {
		if got := NameHash(v.name); got != v.want {
			t.Errorf("NameHash(%q) = %08X, golden %08X  [%s %s]", v.name, got, v.want, v.kind, v.label)
		}
	}
	t.Logf("%d golden vectors matched", len(vecs))
}

// TestNameHashFolds states the fold's purpose directly, so the vectors are not
// the only thing saying it: the pairs the fold exists for must collide, and
// distinct names must not.
func TestNameHashFolds(t *testing.T) {
	pairs := []struct{ what, a, b string }{
		{"case", "01. INTENTIONS.FLAC", "01. intentions.flac"},
		{"apostrophe", "Don’t Stop", "Don't Stop"},
		{"quotes", "“Hello”", `"Hello"`},
		{"en dash", "A – B", "A - B"},
		{"em dash", "A — B", "A - B"},
	}
	for _, p := range pairs {
		if NameHash(p.a) != NameHash(p.b) {
			t.Errorf("%s fold does not collide: %q vs %q", p.what, p.a, p.b)
		}
	}
	if NameHash("01. Intentions.flac") == NameHash("02. Intentions.flac") {
		t.Error("distinct names collide")
	}
	if got, want := NameHash(""), uint32(0x811C9DC5); got != want {
		t.Errorf("NameHash(\"\") = %08X, want the FNV-1a offset basis %08X", got, want)
	}
}

func TestNormKeyFoldsOnlyWhatItPromises(t *testing.T) {
	// The fold is ASCII-only: an accented capital is NOT lowercased, on
	// either side. Pinned so nobody "improves" one side into a Unicode-aware
	// fold without the other.
	for _, c := range []struct{ in, want string }{
		{"Édouard", "Édouard"},
		{"Björk - Vespertine", "björk - vespertine"},
		{"A — B", "a - b"},
		{"“Hi”", `"hi"`},
	} {
		if got := NormKey(c.in); got != c.want {
			t.Errorf("NormKey(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

func TestLeadTrack(t *testing.T) {
	// The table from core/tests/scripts/check_build_index.py, which is where
	// each of these was decided.
	cases := []struct {
		stem, artist string
		disc, track  int
	}{
		{"01. Title", "", 0, 1},
		{"01 - Title", "", 0, 1},
		{"01_Title", "", 0, 1},
		{"01-Title", "", 0, 1},
		{"7 rings", "", 0, 0},
		{"99 Luftballons", "", 0, 0},
		{"21 Guns", "", 0, 0},
		{"12 Title", "", 0, 0},
		{"07. 7 rings", "", 0, 7},
		{"07 - 7 rings", "", 0, 7},
		{"7.", "", 0, 0},
		{"1-01 Title", "", 1, 1},
		{"2-05 Title", "", 2, 5},
		{"1999", "", 0, 0},
		{"1999 - Title", "", 0, 0},
		{"2Pac - Title", "", 0, 0},
		{"7", "", 0, 0},
		{"50 Cent - In Da Club", "50 Cent", 0, 0},
		// No separator after the digits, so it is not a track number under a
		// different artist either.
		{"50 Cent - In Da Club", "Eminem", 0, 0},
		{"50 - In Da Club", "Eminem", 0, 50},
		{"50 - In Da Club", "50", 0, 0},
	}
	for _, c := range cases {
		d, tr := LeadTrack(c.stem, c.artist)
		if d != c.disc || tr != c.track {
			t.Errorf("LeadTrack(%q, %q) = (%d, %d), want (%d, %d)", c.stem, c.artist, d, tr, c.disc, c.track)
		}
	}
}

func TestTrackNumber(t *testing.T) {
	if d, tr := TrackNumber("03. Foo", 7, 0, 0, ""); d != 1 || tr != 7 {
		t.Errorf("the tag must beat the filename: got (%d, %d)", d, tr)
	}
	if d, tr := TrackNumber("x", 0, 9, 2, ""); d != 2 || tr != 0 {
		t.Errorf("a Disc folder must beat the disc tag: got (%d, %d)", d, tr)
	}
	if d, tr := TrackNumber("x", 4, 2, 0, ""); d != 2 || tr != 4 {
		t.Errorf("a flat album must read the disc tag: got (%d, %d)", d, tr)
	}
	if d, tr := TrackNumber("2-05 Title", 0, 0, 0, ""); d != 2 || tr != 5 {
		t.Errorf("the flattened multi-disc filename: got (%d, %d)", d, tr)
	}
}

func TestLeadInt(t *testing.T) {
	for _, c := range []struct {
		in   string
		want int
	}{{"7", 7}, {"7/12", 7}, {" 3", 3}, {"", 0}, {"none", 0}, {"1999", 1999}} {
		if got := LeadInt(c.in); got != c.want {
			t.Errorf("LeadInt(%q) = %d, want %d", c.in, got, c.want)
		}
	}
}

func TestTrackTitle(t *testing.T) {
	for _, c := range []struct{ in, want string }{
		{"01. Intentions.flac", "01. Intentions"},
		{"01 - Artist - Title.flac", "Title"},
		{"Artist - Title.flac", "Title"},
		{"01 - Artist - A - B.flac", "A - B"},
		{"Title.flac", "Title"},
		{"Title", "Title"},
		{".hidden", ".hidden"},
	} {
		if got := TrackTitle(c.in); got != c.want {
			t.Errorf("TrackTitle(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

func TestFatSafeAndStraighten(t *testing.T) {
	for _, c := range []struct{ in, want string }{
		{`F*CK LOVE 3+: OVER YOU`, "F_CK LOVE 3+_ OVER YOU"},
		{`a/b\c:d*e?f"g<h>i|j`, "a_b_c_d_e_f_g_h_i_j"},
		{"trailing. ", "trailing"},
		{"dots...", "dots"},
		{"Björk", "Björk"},
	} {
		if got := FatSafe(c.in); got != c.want {
			t.Errorf("FatSafe(%q) = %q, want %q", c.in, got, c.want)
		}
	}
	if got := Straighten("Don’t ‘Stop’"); got != "Don't 'Stop'" {
		t.Errorf("Straighten = %q", got)
	}
}

func TestUTF8FieldTruncatesOnARuneBoundary(t *testing.T) {
	// 24 is the genre field: 23 usable bytes.
	long := strings.Repeat("a", 40)
	if got := string(UTF8Field(long, 24)); got != strings.Repeat("a", 23) {
		t.Errorf("UTF8Field truncated to %d bytes, want 23", len(got))
	}
	// A 3-byte rune straddling the cut must be dropped whole, not split.
	s := strings.Repeat("a", 22) + "é" // 22 + 2 bytes = 24 > 23
	got := string(UTF8Field(s, 24))
	if got != strings.Repeat("a", 22) {
		t.Errorf("UTF8Field(%q, 24) = %q; a split rune must be dropped whole", s, got)
	}
	// C0 controls are dropped; DEL and C1 are not (build_index.py keeps
	// anything >= 0x20).
	if got := string(UTF8Field("a\x01b\x1fc\x7f", 48)); got != "abc\x7f" {
		t.Errorf("UTF8Field control handling = %q", got)
	}
	if got := UTF8Field("abc", 1); len(got) != 0 {
		t.Errorf("a 1-byte field can hold nothing but the NUL, got %q", got)
	}
}

func TestSplitAlbumArtistSplitsOnTheLastSeparator(t *testing.T) {
	for _, c := range []struct{ in, artist, album string }{
		{"Changes - Justin Bieber", "Justin Bieber", "Changes"},
		{"A - B - C", "C", "A - B"},
		{"Album", "", "Album"},
		{"Album - ", "", "Album"},
	} {
		a, al := SplitAlbumArtist(c.in)
		if a != c.artist || al != c.album {
			t.Errorf("SplitAlbumArtist(%q) = (%q, %q), want (%q, %q)", c.in, a, al, c.artist, c.album)
		}
	}
}

// TestGoldenVectorsCoverTheAstralPlane is a guard on the table itself: the
// 4-byte UTF-8 vectors were once XFAIL because names.c folded an astral
// codepoint into a 3-byte frame, so any track with an emoji in its name went
// into the index under a hash the device could never match.
func TestGoldenVectorsCoverTheAstralPlane(t *testing.T) {
	repo := repoRoot()
	if repo == "" {
		t.Skip("firmware repo not found (set CORE_REPO)")
	}
	vecs := parseVectors(t, filepath.Join(repo, "core", "tests", "kernel", "name_hash_vectors.h"))
	var astral int
	for _, v := range vecs {
		for _, r := range v.name {
			if r > 0xFFFF {
				astral++
				break
			}
		}
	}
	if astral == 0 {
		t.Error("no astral-plane vector in the golden table")
	}
}
