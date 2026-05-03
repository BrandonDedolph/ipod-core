package tagcache

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/dhowden/tag"
)

// SongInfo holds the per-file metadata captured during a scan, before
// dedup + serialization. The strings are owner copies; ArtBytes is a
// borrowed slice from the source library, valid until the next call.
type SongInfo struct {
	Path     string
	Title    string
	Artist   string
	Album    string
	Genre    string
	Composer string
	ArtBytes []byte
}

// Scan walks `dir` non-recursively for .flac/.mp3 files (case-insensitive)
// and parses tags via dhowden/tag. Files we can't open or parse are
// silently skipped — the user gets a row with the basename-as-title
// instead of an outright failure, matching the firmware's runtime
// behavior in tagcache.c.
//
// Returned songs are sorted by title (case-insensitive) so that the
// global song-index order matches what the firmware would produce.
func Scan(dir string) ([]SongInfo, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, fmt.Errorf("tagcache: read dir %s: %w", dir, err)
	}
	songs := make([]SongInfo, 0, len(entries))
	for _, ent := range entries {
		if ent.IsDir() {
			continue
		}
		name := ent.Name()
		ext := strings.ToLower(filepath.Ext(name))
		if ext != ".flac" && ext != ".mp3" {
			continue
		}
		full := filepath.Join(dir, name)
		s := SongInfo{Path: full, Title: strings.TrimSuffix(name, filepath.Ext(name))}
		readTags(&s, full)
		songs = append(songs, s)
	}
	sort.SliceStable(songs, func(i, j int) bool {
		return strings.ToLower(songs[i].Title) < strings.ToLower(songs[j].Title)
	})
	return songs, nil
}

// readTags opens `path` and populates s with whatever tag data is
// present. On any error (open failed, format unrecognized) the
// already-populated default fields (Path, basename Title) stay as-is.
func readTags(s *SongInfo, path string) {
	f, err := os.Open(path)
	if err != nil {
		return
	}
	defer f.Close()
	m, err := tag.ReadFrom(f)
	if err != nil {
		return
	}
	if t := m.Title(); t != "" {
		s.Title = t
	}
	s.Artist = m.Artist()
	s.Album = m.Album()
	s.Genre = m.Genre()
	s.Composer = m.Composer()
	if pic := m.Picture(); pic != nil && len(pic.Data) > 0 {
		// dhowden/tag returns the raw bytes verbatim; the firmware
		// expects JPEG today, so we stash whatever's there and let
		// the C-side stb_image fail gracefully on non-JPEG. Filtering
		// to image/jpeg here would be stricter but breaks the user's
		// PNG cover art when stb_image gains PNG support.
		s.ArtBytes = pic.Data
	}
}
