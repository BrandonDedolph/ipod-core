// Package library turns a source music tree of "Album - Artist" folders into
// the album/track model the device index is written from.
//
// Every rule in this package is mirrored, rule for rule, from
// tools/build_index.py — the reference implementation and the parity oracle.
// Where the two could differ they are held together by tests: the golden hash
// vectors in core/tests/kernel/name_hash_vectors.h (shared with the firmware's
// C side), a synthetic tree run through both tools, and a byte-for-byte
// comparison on the real library. The hash is the ONLY thing binding an index
// record to the file on disk, so a divergence does not fail loudly: the track
// silently stops resolving on the device.
package library

import (
	"regexp"
	"strings"
	"unicode/utf8"
)

// fnvOffset / fnvPrime are the FNV-1a 32-bit parameters.
const (
	fnvOffset = 0x811c9dc5
	fnvPrime  = 0x01000193
)

// NormKey is the canonical form for name matching: smart quotes and dashes
// folded to ASCII, A-Z lowercased, nothing else touched. It must stay identical
// to norm_key() in tools/build_index.py and to the fold inside name_hash() in
// core/library/names.c.
//
// Invalid UTF-8 decodes to U+FFFD one byte at a time, which is what
// core/library/names.c's mn_utf8_next does with a malformed sequence.
func NormKey(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for _, ch := range s {
		switch ch {
		case 0x2018, 0x2019:
			ch = '\''
		case 0x201C, 0x201D:
			ch = '"'
		case 0x2013, 0x2014:
			ch = '-'
		}
		if ch >= 'A' && ch <= 'Z' {
			ch += 32
		}
		b.WriteRune(ch)
	}
	return b.String()
}

// NameHash is FNV-1a 32 over the UTF-8 bytes of NormKey(s) — the locator that
// binds an index record to its folder and file on disk.
func NameHash(s string) uint32 {
	h := uint32(fnvOffset)
	for _, c := range []byte(NormKey(s)) {
		h = (h ^ uint32(c)) * fnvPrime
	}
	return h
}

// fatBad is the set FAT32 forbids in a name; build_index.py's BAD regex.
const fatBad = `\/:*?"<>|`

// FatSafe replaces every character FAT32 forbids with "_" and then strips
// trailing spaces and dots (which FAT also will not keep). The device folder
// and file names are FatSafe on both sides, so the hashes match.
func FatSafe(s string) string {
	b := []byte(s)
	out := make([]byte, len(b))
	for i, c := range b {
		if c < utf8.RuneSelf && strings.IndexByte(fatBad, c) >= 0 {
			out[i] = '_'
			continue
		}
		out[i] = c
	}
	// Python: BAD.sub("_", s).rstrip(" .")
	end := len(out)
	for end > 0 && (out[end-1] == ' ' || out[end-1] == '.') {
		end--
	}
	return string(out[:end])
}

// Straighten folds curly apostrophes to the ASCII one. build_index.py uses it
// to compare a tag album against a folder album without caring which quote
// style either happens to use.
func Straighten(s string) string {
	return strings.NewReplacer("’", "'", "‘", "'").Replace(s)
}

// UTF8Field renders a name for a fixed-width index field: C0 control
// characters dropped, UTF-8, truncated to n-1 bytes on a rune boundary (the
// record's fields are NUL-terminated, so the last byte is never ours). The
// returned slice is NOT padded — cidx.Encode pads it into the record.
//
// Mirrors utf8_field() in build_index.py, including the detail that only
// codepoints below 0x20 are dropped: DEL and the C1 range are kept.
func UTF8Field(s string, n int) []byte {
	if n <= 1 {
		return nil
	}
	// Dropping bytes < 0x20 is the same as dropping characters < 0x20: every
	// byte of a multi-byte UTF-8 sequence is >= 0x80.
	kept := make([]byte, 0, len(s))
	for i := 0; i < len(s); i++ {
		if s[i] >= 0x20 {
			kept = append(kept, s[i])
		}
	}
	if len(kept) > n-1 {
		kept = kept[:n-1]
	}
	// Python re-decodes with "ignore", which drops anything that is not valid
	// UTF-8 — in practice the sequence the truncation split in half.
	out := make([]byte, 0, len(kept))
	for i := 0; i < len(kept); {
		r, size := utf8.DecodeRune(kept[i:])
		if r == utf8.RuneError && size <= 1 {
			i++
			continue
		}
		out = append(out, kept[i:i+size]...)
		i += size
	}
	return out
}

// SplitAlbumArtist splits a SOURCE folder name on its LAST " - ": the artist is
// what follows, the album what precedes. A folder with no " - " has no artist
// and is skipped by the scan (mirrors split_album_artist()).
func SplitAlbumArtist(folder string) (artist, album string) {
	i := strings.LastIndex(folder, " - ")
	if i < 0 {
		return "", folder
	}
	return pyStrip(folder[i+3:]), pyStrip(folder[:i])
}

// TrackTitle is the title read off a source file NAME (not its tags): the stem
// with an "Artist - Title" or "NN - Artist - Title" prefix removed.
func TrackTitle(fname string) string {
	stem, _ := splitExt(fname)
	parts := strings.Split(stem, " - ")
	switch {
	case len(parts) >= 3:
		return pyStrip(strings.Join(parts[2:], " - "))
	case len(parts) == 2:
		return pyStrip(parts[1])
	default:
		return pyStrip(stem)
	}
}

// pySpace is Python's \s for str patterns and the set str.strip() removes:
// Unicode whitespace plus the C0 separators 0x1c-0x1f, which Go's
// unicode.IsSpace does not consider space.
const pySpace = `\t\n\v\f\r \x{1c}-\x{1f}\x{85}\x{a0}\x{1680}\x{2000}-\x{200a}\x{2028}\x{2029}\x{202f}\x{205f}\x{3000}`

var pySpaceSet = regexp.MustCompile(`^[` + pySpace + `]$`)

func pyIsSpace(r rune) bool { return pySpaceSet.MatchString(string(r)) }

func pyStrip(s string) string { return strings.TrimFunc(s, pyIsSpace) }

// splitExt is os.path.splitext: the extension is the last "." in the name, and
// a name that is all leading dots has none. filepath.Ext disagrees (it calls
// ".flac" an extension with an empty stem), and the difference decides what a
// dotfile's TrackTitle is.
func splitExt(name string) (root, ext string) {
	i := strings.LastIndexByte(name, '.')
	if i < 0 {
		return name, ""
	}
	j := 0
	for j < len(name) && name[j] == '.' {
		j++
	}
	if i < j {
		return name, ""
	}
	return name[:i], name[i:]
}

// A track number at the start of a filename stem: one to three digits, then a
// REAL separator (".", "-" or "_", spaced or not), then something. Three digits
// caps it below any year ("1999.flac" is a song called 1999) and the separator
// rules out an artist whose name starts with digits ("2Pac - ..."). A bare
// space is NOT a separator: "7 rings", "99 Luftballons" and "21 Guns" are
// titles. "D-NN" is the flattened multi-disc convention ("2-05 Title").
//
// build_index.py writes the trailing condition as a lookahead, which RE2 has
// no equivalent for; consuming the character instead changes only where the
// match ends, and the match end is not used.
var (
	leadDiscTrack = regexp.MustCompile(`^[` + pySpace + `]*(\d{1,2})-(\d{1,3})[` + pySpace + `._-]`)
	leadTrackRe   = regexp.MustCompile(`^[` + pySpace + `]*(\d{1,3})[` + pySpace + `]*[._-][` + pySpace + `]*[^` + pySpace + `]`)
	leadIntRe     = regexp.MustCompile(`^[` + pySpace + `]*(\d+)`)
)

// LeadTrack reads (disc, track) off a filename stem, (0, 0) when there is none.
// A stem that simply starts with the album's artist ("50 Cent - In Da Club") is
// an "Artist - Title" name, not a numbered one, whatever the artist is called.
func LeadTrack(stem, artist string) (disc, track int) {
	if artist != "" && strings.HasPrefix(strings.ToLower(stem), strings.ToLower(artist)) {
		return 0, 0
	}
	if m := leadDiscTrack.FindStringSubmatch(stem); m != nil {
		return atoi(m[1]), atoi(m[2])
	}
	if m := leadTrackRe.FindStringSubmatch(stem); m != nil {
		return 0, atoi(m[1])
	}
	return 0, 0
}

// LeadInt is the leading integer of a TAG value: "7", "7/12", " 3" -> 7, 7, 3;
// else 0. For a filename use LeadTrack — this one would happily read a year.
func LeadInt(s string) int {
	m := leadIntRe.FindStringSubmatch(s)
	if m == nil {
		return 0
	}
	return atoi(m[1])
}

// TrackNumber is the ONE authority for (disc, track): tags first, the
// filename's leading number second, and 0 ("unnumbered") last — the caller
// substitutes the enumeration position for a 0 track so the gutter is never
// blank.
//
// Disc: a "Disc N" source folder is ground truth when there is one; otherwise
// the tag, otherwise 1.
func TrackNumber(stem string, tagTrack, tagDisc, folderDisc int, artist string) (disc, track int) {
	ldisc, ltrack := LeadTrack(stem, artist)
	track = tagTrack
	if track == 0 {
		track = ltrack
	}
	switch {
	case folderDisc != 0:
		disc = folderDisc
	case tagDisc != 0:
		disc = tagDisc
	case ldisc != 0:
		disc = ldisc
	default:
		disc = 1
	}
	return disc, track
}

// atoi parses a run of ASCII digits the regexps already validated. The values
// are filename fragments of at most three digits, so overflow is impossible;
// a longer run (LeadInt's \d+) saturates rather than wrapping.
func atoi(s string) int {
	n := 0
	for i := 0; i < len(s); i++ {
		if n > (1<<31-1-9)/10 {
			return 1<<31 - 1
		}
		n = n*10 + int(s[i]-'0')
	}
	return n
}
