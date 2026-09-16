package artfetch

import "testing"

// The 0.9 line is the one that matters: `core art --fetch --yes` and the
// app's "accept all" act on it, so every row at or above it is a cover
// this program is prepared to write into somebody's music without
// asking, and every row below it is one it will not.
func TestScore(t *testing.T) {
	cases := []struct {
		name    string
		qArtist string
		qAlbum  string
		cArtist string
		cAlbum  string
		rank    float64
		want    float64
	}{
		{"exact", "Morgan Wallen", "I'm The Problem", "Morgan Wallen", "I'm The Problem", 0, 1.0},
		{"curly apostrophe is the same album", "Morgan Wallen", "I'm The Problem",
			"Morgan Wallen", "I’m The Problem", 0, 1.0},
		{"case and punctuation", "morgan wallen", "one thing at a time",
			"Morgan Wallen", "One Thing at a Time", 0, 1.0},
		{"leading The is not a difference", "The Beatles", "Revolver", "Beatles", "Revolver", 0, 1.0},
		{"single suffix", "Morgan Wallen", "I'm The Problem",
			"Morgan Wallen", "I'm The Problem - Single", 0, 0.9},
		{"deluxe edition", "ericdoa", "COA", "ericdoa", "COA (Deluxe Edition)", 0, 0.9},
		{"remastered", "Radiohead", "OK Computer", "Radiohead", "OK Computer [Remastered]", 0, 0.9},
		{"containment", "Cameron Dallas", "Electric", "Cameron Dallas", "Electric Feelings Tour", 0, 0.7},
		{"a longer query containing the release", "Cameron Dallas", "Electric Feelings Tour",
			"Cameron Dallas", "Electric", 0, 0.7},
		{"the weaker field wins: right album, wrong artist",
			"Morgan Wallen", "Dangerous", "Some Other Guy", "Dangerous", 1, 0.45},
		{"the weaker field wins: right artist, wrong album",
			"Morgan Wallen", "Dangerous", "Morgan Wallen", "Sand In My Boots", 0.8, 0.36},
		{"nothing matches", "A Band", "A Record", "Another Band", "Another Record", 0, 0.0},
		{"an empty artist can never reach --yes",
			"", "One Thing At A Time", "Morgan Wallen", "One Thing at a Time", 1, 0.7},
		{"an empty album is no match at all", "Morgan Wallen", "", "Morgan Wallen", "Dangerous", 1, 0.45},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got := Score(Query{Artist: c.qArtist, Album: c.qAlbum},
				Candidate{Artist: c.cArtist, Album: c.cAlbum, Rank: c.rank})
			if diff := got - c.want; diff > 1e-9 || diff < -1e-9 {
				t.Errorf("Score(%q/%q vs %q/%q) = %.3f, want %.3f",
					c.qArtist, c.qAlbum, c.cArtist, c.cAlbum, got, c.want)
			}
		})
	}
}

func TestNormKeyAndStripDecorations(t *testing.T) {
	cases := []struct{ in, norm, stripped string }{
		{"I’m The Problem", "i m the problem", "i m the problem"},
		{"The Beatles", "beatles", "beatles"},
		{"COA (Deluxe Edition)", "coa deluxe edition", "coa"},
		{"OK Computer [Remastered]", "ok computer remastered", "ok computer"},
		{"Cover Me Up - Single", "cover me up single", "cover me up"},
		{"  ", "", ""},
	}
	for _, c := range cases {
		if got := NormKey(c.in); got != c.norm {
			t.Errorf("NormKey(%q) = %q, want %q", c.in, got, c.norm)
		}
		if got := StripDecorations(NormKey(c.in)); got != c.stripped {
			t.Errorf("StripDecorations(NormKey(%q)) = %q, want %q", c.in, got, c.stripped)
		}
	}
}
