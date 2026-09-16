// SPDX-License-Identifier: Apache-2.0

package syncer

import (
	"fmt"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

// PlaylistExts are the two extensions the firmware lists
// (core/library/playlist.c:98-146).
var PlaylistExts = []string{".m3u8", ".m3u"}

// planPlaylists reads the source playlists and rewrites every line as the
// device path of the track it names.
//
// A playlist on the host points at the source tree; the device has a
// different tree (Artist - Album, "NN. Title.flac" or ".mp3"), so a line is only
// meaningful after it has been mapped through the scan. A line that maps to
// nothing — a track outside the source tree, a file the scan skipped, a typo —
// is dropped with a warning rather than written: the firmware would silently
// count it as missing, and a playlist that is quietly two tracks short is
// exactly the kind of thing nobody notices until they are on a train.
func planPlaylists(o Options, scan *library.Scan, musicDir string) ([]PlaylistOp, []string, error) {
	dir := o.Playlists
	explicit := dir != ""
	if dir == "" {
		dir = filepath.Join(o.Src, devicefs.PlaylistDir)
	}
	ents, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) && !explicit {
			return nil, nil, nil // no Playlists folder is the normal case
		}
		return nil, nil, fmt.Errorf("playlists %s: %w", dir, err)
	}

	idx := newTrackIndex(scan)

	var names []string
	for _, e := range ents {
		if e.IsDir() {
			continue
		}
		if hasPlaylistExt(e.Name()) {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)

	var out []PlaylistOp
	var warns []string
	used := map[string]string{}
	for _, name := range names {
		src := filepath.Join(dir, name)
		raw, err := os.ReadFile(src)
		if err != nil {
			return nil, nil, fmt.Errorf("playlist %s: %w", src, err)
		}
		op := PlaylistOp{Name: devicePlaylistName(name), Src: src}
		if prev, dup := used[strings.ToLower(op.Name)]; dup {
			warns = append(warns, fmt.Sprintf("playlist %s: would overwrite %s on the device (both become %s); skipped", src, prev, op.Name))
			continue
		}
		used[strings.ToLower(op.Name)] = src
		op.Dst = filepath.Join(musicDir, devicefs.PlaylistDir, op.Name)

		for _, line := range splitLines(raw) {
			line = strings.Trim(line, " \t")
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			entry, ok := idx.lookup(line, dir, o.Src)
			switch {
			case !ok:
				op.Dropped = append(op.Dropped, line)
				warns = append(warns, fmt.Sprintf("playlist %s: %q is not a track in the source tree; dropped", name, line))
			case len(entry) > devicefs.PathMax:
				op.Dropped = append(op.Dropped, line)
				warns = append(warns, fmt.Sprintf("playlist %s: %q resolves to %d bytes, over the firmware's M3U_PATH_MAX (%d); dropped",
					name, entry, len(entry), devicefs.PathMax))
			default:
				op.Entries = append(op.Entries, entry)
			}
		}
		if old, err := os.ReadFile(op.Dst); err == nil && string(old) == playlistBytes(op.Entries) {
			op.Unchanged = true
		}
		out = append(out, op)
	}
	return out, warns, nil
}

// playlistBytes is exactly what devicefs.WriteM3U8 puts on the disk for these
// entries — they are already canonical (absolute, forward slashes, no padding),
// so this is a prediction, not a second implementation. It exists so an
// unchanged playlist is not rewritten on every sync.
func playlistBytes(entries []string) string {
	var b strings.Builder
	b.WriteString("#EXTM3U\n")
	for _, e := range entries {
		b.WriteString(e)
		b.WriteByte('\n')
	}
	return b.String()
}

func hasPlaylistExt(name string) bool {
	ext := strings.ToLower(filepath.Ext(name))
	for _, e := range PlaylistExts {
		if ext == e {
			return true
		}
	}
	return false
}

// devicePlaylistName is the name the playlist gets on the device: the source
// stem, made FAT-safe, always with the .m3u8 extension (the firmware accepts
// .m3u too, but the file is UTF-8 and .m3u8 is what that means).
func devicePlaylistName(name string) string {
	stem := strings.TrimSuffix(name, filepath.Ext(name))
	stem = library.FatSafe(stem)
	if stem == "" {
		stem = "Playlist"
	}
	return stem + ".m3u8"
}

// splitLines accepts LF, CRLF and lone CR, like the firmware's reader, and
// skips a UTF-8 BOM.
func splitLines(b []byte) []string {
	s := strings.TrimPrefix(string(b), "\ufeff")
	s = strings.ReplaceAll(s, "\r\n", "\n")
	s = strings.ReplaceAll(s, "\r", "\n")
	return strings.Split(s, "\n")
}

// trackIndex maps a source path, however a playlist happens to spell it, to
// the track's device path.
type trackIndex struct {
	// byPath is the absolute, lower-cased source path of every track the
	// scan INDEXED. A file the scan skipped is deliberately absent: it will
	// not be on the device, so a playlist must not claim it is.
	byPath map[string]string
	// byTail is "<album folder>/<file name>", lower-cased — the shape that
	// survives a playlist written on another machine, or against a moved
	// copy of the same tree. Ambiguous tails are removed rather than
	// guessed.
	byTail map[string]string
	ambig  map[string]bool
}

func newTrackIndex(scan *library.Scan) *trackIndex {
	idx := &trackIndex{
		byPath: map[string]string{},
		byTail: map[string]string{},
		ambig:  map[string]bool{},
	}
	for i := range scan.Albums {
		a := &scan.Albums[i]
		for _, t := range a.Tracks {
			dev := devicefs.PlaylistEntry(a.DeviceFolder, t.DeviceName)
			if abs, err := filepath.Abs(t.SrcPath); err == nil {
				idx.byPath[normPath(abs)] = dev
			}
			tail := normPath(filepath.Join(filepath.Base(filepath.Dir(t.SrcPath)), filepath.Base(t.SrcPath)))
			if prev, seen := idx.byTail[tail]; seen && prev != dev {
				idx.ambig[tail] = true
			} else {
				idx.byTail[tail] = dev
			}
		}
	}
	return idx
}

// lookup resolves one playlist line. The accepted spellings are the ones a
// real playlist carries: absolute (POSIX or with a drive letter), relative to
// the playlist file's own folder, or relative to the source root; '/' and '\'
// both separate.
func (idx *trackIndex) lookup(line, playlistDir, src string) (string, bool) {
	s := strings.ReplaceAll(line, `\`, "/")
	if s == "" {
		return "", false
	}

	var cands []string
	if isAbsLine(s) {
		cands = append(cands, s)
	} else {
		cands = append(cands,
			filepath.Join(playlistDir, filepath.FromSlash(s)),
			filepath.Join(src, filepath.FromSlash(s)),
		)
	}
	for _, c := range cands {
		p := c
		if abs, err := filepath.Abs(c); err == nil {
			p = abs
		}
		if dev, ok := idx.byPath[normPath(p)]; ok {
			return dev, true
		}
	}

	// Last resort: the album folder and file name. A Windows-written line
	// ("C:\Users\me\Music\MC\Album - Artist\01 Song.flac") never matches by
	// path on this host, but its last two segments do.
	clean := path.Clean(strings.TrimPrefix(s, "/"))
	segs := strings.Split(clean, "/")
	if len(segs) >= 2 {
		tail := normPath(strings.Join(segs[len(segs)-2:], string(filepath.Separator)))
		if idx.ambig[tail] {
			return "", false
		}
		if dev, ok := idx.byTail[tail]; ok {
			return dev, true
		}
	}
	return "", false
}

// isAbsLine reports whether the line names the root of something: a leading
// separator, or a drive letter at index 1 (what a Windows player writes).
func isAbsLine(s string) bool {
	if strings.HasPrefix(s, "/") {
		return true
	}
	return len(s) >= 3 && s[1] == ':' && s[2] == '/'
}

// normPath is the comparison form of a path: cleaned and lower-cased. Case is
// folded because a playlist written by a Windows player spells the tree in
// whatever case the user typed, and on FAT that is the same file.
func normPath(p string) string {
	return strings.ToLower(filepath.Clean(p))
}
