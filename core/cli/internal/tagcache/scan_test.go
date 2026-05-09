package tagcache

import (
	"os"
	"path/filepath"
	"sort"
	"testing"
)

// TestScanRecursive verifies the scanner descends into artist/album
// subdirectories — the layout every real music library uses — and
// returns the union of all .flac/.mp3 files. Empty files are fine:
// readTags fails silently and the basename-as-title fallback kicks in,
// which is enough to confirm the file was discovered.
func TestScanRecursive(t *testing.T) {
	root := t.TempDir()
	want := []string{
		filepath.Join(root, "ArtistA", "Album1", "01 Track.mp3"),
		filepath.Join(root, "ArtistA", "Album1", "02 Track.flac"),
		filepath.Join(root, "ArtistB", "Album1", "Disc 1", "deep.mp3"),
		filepath.Join(root, "loose.MP3"), // case-insensitive ext
	}
	for _, p := range want {
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatalf("mkdir: %v", err)
		}
		if err := os.WriteFile(p, nil, 0o644); err != nil {
			t.Fatalf("write %s: %v", p, err)
		}
	}
	// Drop a non-audio sibling to make sure it's filtered out.
	if err := os.WriteFile(filepath.Join(root, "ArtistA", "cover.jpg"), nil, 0o644); err != nil {
		t.Fatalf("write cover: %v", err)
	}

	songs, err := Scan(root)
	if err != nil {
		t.Fatalf("Scan: %v", err)
	}
	if len(songs) != len(want) {
		t.Fatalf("want %d songs, got %d: %v", len(want), len(songs), songs)
	}
	got := make([]string, len(songs))
	for i, s := range songs {
		got[i] = s.Path
	}
	sort.Strings(got)
	sort.Strings(want)
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("song[%d] path: want %q got %q", i, want[i], got[i])
		}
	}
}

// TestScanRejectsFile guards the precondition: passing a regular file
// instead of a directory must error rather than silently scanning the
// file's parent.
func TestScanRejectsFile(t *testing.T) {
	f := filepath.Join(t.TempDir(), "x.mp3")
	if err := os.WriteFile(f, nil, 0o644); err != nil {
		t.Fatalf("write: %v", err)
	}
	if _, err := Scan(f); err == nil {
		t.Fatal("Scan on a regular file: want error, got nil")
	}
}
