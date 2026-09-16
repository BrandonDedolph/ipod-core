package artfetch

import (
	"regexp"
	"strings"
	"unicode"
)

// Score says how much a candidate looks like the album that was asked
// for. It is the whole safety mechanism of this package: `--yes` and
// the app's "accept all" act on it and nothing else.
//
// The ladder (plan §1 decision 9), applied per field and then taken at
// its weakest:
//
//	1.00  artist and album are equal once normalised
//	0.90  equal once the edition decorations are also stripped —
//	      "(Deluxe Edition)", "[Remastered]", "- Single", "feat. X"
//	0.70  one contains the other (the query is a prefix of a longer
//	      release title, or the folder name carried extra words)
//	<0.5  nothing matched by name: the provider's own rank, scaled to
//	      0.45 at best, so a name mismatch can never reach --yes
//
// Taking the weaker of the two fields is deliberate. An exact album
// title on the wrong artist is exactly the failure that puts a stranger's
// cover on someone's record.
func Score(q Query, c Candidate) float64 {
	artist := level(q.Artist, c.Artist)
	album := level(q.Album, c.Album)
	// An album with no artist in the query (a loose folder) is judged
	// on its title alone rather than failed outright, but it can never
	// be better than a containment match — there is no second field to
	// confirm it with.
	if strings.TrimSpace(q.Artist) == "" {
		if album > matchContains {
			album = matchContains
		}
		artist = album
	}
	weakest := artist
	if album < weakest {
		weakest = album
	}
	switch weakest {
	case matchExact:
		return 1.0
	case matchStripped:
		return 0.9
	case matchContains:
		return 0.7
	}
	rank := c.Rank
	if rank < 0 {
		rank = 0
	}
	if rank > 1 {
		rank = 1
	}
	return 0.45 * rank
}

// Match levels, weakest first.
const (
	matchNone = iota
	matchContains
	matchStripped
	matchExact
)

func level(want, got string) int {
	w, g := NormKey(want), NormKey(got)
	if w == "" || g == "" {
		return matchNone
	}
	if w == g {
		return matchExact
	}
	ws, gs := StripDecorations(w), StripDecorations(g)
	if ws != "" && ws == gs {
		return matchStripped
	}
	// Containment only counts when the shorter side is long enough to
	// mean something: "a" is inside everything.
	short, long := ws, gs
	if len(short) > len(long) {
		short, long = long, short
	}
	if len(short) >= 4 && strings.Contains(long, short) {
		return matchContains
	}
	return matchNone
}

// NormKey folds one name to the form two services can be compared in:
// lower case, typographic punctuation straightened, everything that is
// not a letter or a digit turned into a single space, a leading "the "
// dropped.
//
// This is artfetch's own normaliser, not library.NormKey: that one is
// the index's hash key and its exact behaviour is a device contract.
// Loosening this one must never be able to change a filename.
func NormKey(s string) string {
	s = strings.Map(straighten, s)
	s = strings.ToLower(s)
	var b strings.Builder
	space := true // leading spaces are dropped
	for _, r := range s {
		switch {
		case unicode.IsLetter(r) || unicode.IsDigit(r):
			b.WriteRune(r)
			space = false
		case !space:
			b.WriteByte(' ')
			space = true
		}
	}
	out := strings.TrimSpace(b.String())
	out = strings.TrimPrefix(out, "the ")
	return out
}

// straighten maps the typographic characters a music service and a tag
// editor disagree about onto their ASCII shapes, so "I’m The Problem"
// and "I'm The Problem" are the same album.
func straighten(r rune) rune {
	switch r {
	case '‘', '’', 'ʼ', '‛':
		return '\''
	case '“', '”':
		return '"'
	case '‐', '‑', '‒', '–', '—', '―':
		return '-'
	case '…':
		return '.'
	case ' ', ' ', ' ':
		return ' '
	}
	return r
}

// decorations are the parts of a release title that describe the
// edition rather than the record. They are stripped for the 0.9 tier
// only — an exact match is still an exact match.
var decorations = regexp.MustCompile(`(?i)\b(` + strings.Join([]string{
	"deluxe", "deluxe edition", "super deluxe", "expanded", "expanded edition",
	"remaster", "remastered", "remastered version", "anniversary edition",
	"special edition", "collectors edition", "collector s edition",
	"limited edition", "standard edition", "explicit", "clean",
	"bonus track version", "bonus tracks", "single", "ep", "original motion picture soundtrack",
	"feat", "featuring", "vol", "version", "edition", "soundtrack",
}, "|") + `)\b`)

// StripDecorations removes the edition words from an already-normalised
// name and re-collapses the spaces. It is exported because the tests
// table-drive it and because the librarian prints what it compared.
func StripDecorations(norm string) string {
	out := decorations.ReplaceAllString(norm, " ")
	return strings.Join(strings.Fields(out), " ")
}
