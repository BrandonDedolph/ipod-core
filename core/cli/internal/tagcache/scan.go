package tagcache

import (
	"fmt"
	"io/fs"
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

// Scan walks `dir` recursively for .flac/.mp3 files (case-insensitive)
// and parses tags via dhowden/tag. Files we can't open or parse are
// silently skipped — the user gets a row with the basename-as-title
// instead of an outright failure, matching the firmware's runtime
// behavior in tagcache.c. Real-world libraries are nearly always nested
// by artist/album, so recursion is the default; there is no flat mode.
//
// Subtrees that error mid-walk (permission denied, vanished while we
// were iterating) are skipped rather than failing the whole scan: the
// user gets the music we *could* read, and the worst case is fewer
// songs than expected — never a half-built tagcache. Symlinks to
// directories are not followed (filepath.WalkDir's default), which
// also avoids cycle hazards on libraries built out of symlinks.
//
// Returned songs are sorted by title (case-insensitive) so that the
// global song-index order matches what the firmware would produce.
func Scan(dir string) ([]SongInfo, error) {
	if fi, err := os.Stat(dir); err != nil {
		return nil, fmt.Errorf("tagcache: stat %s: %w", dir, err)
	} else if !fi.IsDir() {
		return nil, fmt.Errorf("tagcache: %s is not a directory", dir)
	}
	var songs []SongInfo
	walkErr := filepath.WalkDir(dir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			if d != nil && d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() {
			return nil
		}
		ext := strings.ToLower(filepath.Ext(d.Name()))
		if ext != ".flac" && ext != ".mp3" {
			return nil
		}
		s := SongInfo{Path: path, Title: strings.TrimSuffix(d.Name(), filepath.Ext(d.Name()))}
		readTags(&s, path)
		songs = append(songs, s)
		return nil
	})
	if walkErr != nil {
		return nil, fmt.Errorf("tagcache: walk %s: %w", dir, walkErr)
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
