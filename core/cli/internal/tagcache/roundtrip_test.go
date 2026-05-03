package tagcache

import (
	"bytes"
	"encoding/binary"
	"reflect"
	"testing"
)

// TestRoundTrip exercises Build → Write → Read on a hand-crafted song
// list covering: tagged + untagged tracks, two artists, two albums,
// two genres, one composer with multiple tracks, and one shared cover
// art (so the dedup path is exercised).
func TestRoundTrip(t *testing.T) {
	jpegA := []byte{0xff, 0xd8, 0xff, 0xe0, 1, 2, 3} // pretend-JPEG
	jpegB := []byte{0xff, 0xd8, 0xff, 0xe1, 9, 9}

	in := []SongInfo{
		{Path: "/m/track1.flac", Title: "Apple",   Artist: "Aphex Twin", Album: "Drukqs",  Genre: "IDM",      Composer: "Aphex Twin", ArtBytes: jpegA},
		{Path: "/m/track2.flac", Title: "Banana",  Artist: "Aphex Twin", Album: "Drukqs",  Genre: "IDM",      Composer: "Aphex Twin", ArtBytes: jpegA},
		{Path: "/m/track3.flac", Title: "Cherry",  Artist: "Brian Eno",  Album: "Ambient", Genre: "Ambient",  Composer: "",            ArtBytes: jpegB},
		{Path: "/m/track4.flac", Title: "Durian",  Artist: "",            Album: "",        Genre: "",         Composer: "",            ArtBytes: nil},
	}
	// Sort like Scan would.
	want := Build(append([]SongInfo(nil), in...))

	var buf bytes.Buffer
	if err := want.Write(&buf); err != nil {
		t.Fatalf("write: %v", err)
	}
	got, err := Read(buf.Bytes())
	if err != nil {
		t.Fatalf("read: %v", err)
	}

	checkStrings := func(label string, w, g []string) {
		t.Helper()
		if !reflect.DeepEqual(w, g) {
			t.Errorf("%s mismatch:\n want %v\n  got %v", label, w, g)
		}
	}
	checkStrings("uniq_artists",   want.UniqArtists,   got.UniqArtists)
	checkStrings("uniq_albums",    want.UniqAlbums,    got.UniqAlbums)
	checkStrings("uniq_genres",    want.UniqGenres,    got.UniqGenres)
	checkStrings("uniq_composers", want.UniqComposers, got.UniqComposers)

	if !reflect.DeepEqual(want.SongArtistIdx,   got.SongArtistIdx)   { t.Errorf("song_artist_idx mismatch") }
	if !reflect.DeepEqual(want.SongAlbumIdx,    got.SongAlbumIdx)    { t.Errorf("song_album_idx mismatch") }
	if !reflect.DeepEqual(want.SongGenreIdx,    got.SongGenreIdx)    { t.Errorf("song_genre_idx mismatch") }
	if !reflect.DeepEqual(want.SongComposerIdx, got.SongComposerIdx) { t.Errorf("song_composer_idx mismatch") }

	if !reflect.DeepEqual(want.ArtistGroups,   got.ArtistGroups)   { t.Errorf("artist_groups mismatch:\n want %v\n  got %v", want.ArtistGroups, got.ArtistGroups) }
	if !reflect.DeepEqual(want.AlbumGroups,    got.AlbumGroups)    { t.Errorf("album_groups mismatch") }
	if !reflect.DeepEqual(want.GenreGroups,    got.GenreGroups)    { t.Errorf("genre_groups mismatch") }
	if !reflect.DeepEqual(want.ComposerGroups, got.ComposerGroups) { t.Errorf("composer_groups mismatch") }

	for i := range want.Songs {
		if want.Songs[i].Title != got.Songs[i].Title { t.Errorf("song[%d] title: want %q got %q", i, want.Songs[i].Title, got.Songs[i].Title) }
		if want.Songs[i].Path  != got.Songs[i].Path  { t.Errorf("song[%d] path: want %q got %q", i, want.Songs[i].Path,  got.Songs[i].Path) }
		if !bytes.Equal(want.Songs[i].ArtBytes, got.Songs[i].ArtBytes) {
			t.Errorf("song[%d] art bytes mismatch", i)
		}
	}

	// Spot-check dedup outcomes.
	if len(got.UniqArtists) != 2   { t.Errorf("expected 2 uniq artists, got %d (%v)", len(got.UniqArtists), got.UniqArtists) }
	if len(got.UniqComposers) != 1 { t.Errorf("expected 1 uniq composer, got %d (%v)", len(got.UniqComposers), got.UniqComposers) }
	// Aphex group should hold the two Aphex tracks (post-sort by title — Apple, Banana = global indices 0, 1).
	aphex := -1
	for i, a := range got.UniqArtists {
		if a == "Aphex Twin" { aphex = i }
	}
	if aphex < 0 || !reflect.DeepEqual(got.ArtistGroups[aphex], []uint32{0, 1}) {
		t.Errorf("aphex group: want [0 1] got %v (artists=%v)", got.ArtistGroups[aphex], got.UniqArtists)
	}

	// Art dedup: jpegA was used twice; the file should hold it only once,
	// so total art bytes len = len(jpegA) + len(jpegB).
	wantArtLen := len(jpegA) + len(jpegB)
	// Inspect via re-parse of the header.
	var hdr Header
	if err := binary.Read(bytes.NewReader(buf.Bytes()[:HeaderSize]), LE, &hdr); err != nil {
		t.Fatalf("re-read header: %v", err)
	}
	if int(hdr.ArtLen) != wantArtLen {
		t.Errorf("art blob len: want %d (deduped) got %d", wantArtLen, hdr.ArtLen)
	}
}
