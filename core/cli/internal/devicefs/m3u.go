// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// PathMax is the longest canonical, volume-root-relative path the firmware's
// M3U parser will hand back (M3U_PATH_MAX, core/fs/m3u.h:94). A line that
// resolves to more than this is counted as skipped_long and the track
// silently vanishes from the playlist, so WriteM3U8 refuses to write one.
const PathMax = 191

// PlaylistEntry is the device path of one track as a playlist line:
//
//	/Music/<deviceFolder>/<deviceName>
//
// Leading slash deliberately. See WriteM3U8 for why a bare relative path is
// NOT "relative to Music/".
func PlaylistEntry(deviceFolder, deviceName string) string {
	return "/" + MusicDir + "/" + deviceFolder + "/" + deviceName
}

// WriteM3U8 writes an extended M3U8 playlist: "#EXTM3U\n" followed by one
// entry per line, UTF-8, LF terminators, forward slashes, no BOM. The parent
// directory is created if it does not exist. The file is flushed to the
// medium before the call returns.
//
// WHAT THE FIRMWARE ACCEPTS — read off the resolver rather than assumed:
//
// Where the playlist lives. core/library/playlist.c:98-146 lists
// `<library root>/Playlists/*.m3u8|*.m3u`, and on a device with a Music
// folder the library root is Music (core/kernel/main.c:2204-2213), so the
// file is at `<volume root>/Music/Playlists/<name>.m3u8`.
//
// What a line is resolved against. THE PLAN'S "relative to Music/" IS WRONG.
// core/kernel/main.c:2899-2902 passes base_dir = "Music/Playlists" (it is
// PLAYLIST_DIR alone only when there is no Music folder at all), and
// core/fs/m3u.c:225-238 resolves a relative line against that base — the
// directory the playlist file itself lives in. A bare "Artist - Album/01.
// Song.flac" therefore names Music/Playlists/Artist - Album/01. Song.flac,
// which does not exist. The two forms that do work, both exercised by the
// firmware's own fixture at core/tests/library/playlist_test.c:153-162:
//
//   - absolute from the VOLUME root: "/Music/Artist - Album/01. Song.flac"
//   - relative with a pop: "../Artist - Album/01. Song.flac"
//
// PlaylistEntry writes the first. It is the form that does not depend on
// where the playlist file sits, and the resolve walk starts at the volume
// root either way (core/library/playlist.c:213 passes fs->root_clus).
//
// Line rules (core/fs/m3u.c):
//   - A leading '/' or '\' means the volume root; so does a drive letter at
//     index 1 ("D:\Music\...", what a Windows player writes) — m3u.c:207-230.
//   - '/' and '\' are both separators, mixed freely — m3u.c:75-78, 246-258.
//   - "." segments are dropped, ".." pops; a ".." that would escape the
//     volume root REJECTS the line rather than clamping — m3u.c:255-268.
//   - Lines starting with '#' are directives: #EXTM3U is noted, #EXTINF:
//     parsed, every other one ignored, and none of them clears a pending
//     #EXTINF — m3u.c:409-426.
//   - CR and LF both end a line, so LF, CRLF and lone-CR files all work; the
//     empty line CRLF leaves behind is ignored — m3u.c:606-612, 402-408.
//   - A UTF-8 BOM at offset 0 is skipped — m3u.c:482, 586-604.
//   - Leading/trailing spaces and tabs are trimmed — m3u.c:392-399.
//   - Any byte below 0x20 rejects the line, and ':' anywhere but index 1
//     rejects it (that is what makes "http://..." not a path) — m3u.c:206-214.
//   - The resolved path is capped at M3U_PATH_MAX = 191 bytes, the logical
//     line at M3U_LINE_MAX = 512, the file at M3U_FILE_MAX = 1 MiB, and the
//     rows at PLAYLIST_TRACKS_MAX — m3u.h:94-97, playlist.h:56-57.
//
// Only files whose extension classify_ext() knows become rows
// (core/library/playlist.c:240-244); a folder or a .txt is counted as
// unplayable and dropped.
//
// An entry that the firmware could not read back as written — an empty line,
// a control byte, a colon anywhere but a drive letter, a '#' in column 1, or
// a canonical form over PathMax bytes — is an error here rather than a track
// that quietly disappears on the device.
func WriteM3U8(dest string, entries []string) error {
	var b strings.Builder
	b.WriteString("#EXTM3U\n")
	for i, e := range entries {
		line, err := m3uLine(e)
		if err != nil {
			return fmt.Errorf("playlist %s: entry %d (%q): %w", dest, i+1, e, err)
		}
		b.WriteString(line)
		b.WriteByte('\n')
	}
	if dir := filepath.Dir(dest); dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0o777); err != nil {
			return err
		}
	}
	return writeFileThrough(dest, []byte(b.String()))
}

// m3uLine normalises one entry to the shape WriteM3U8 puts on the disk and
// rejects anything the firmware's parser would drop.
func m3uLine(e string) (string, error) {
	line := strings.Trim(strings.ReplaceAll(e, "\\", "/"), " \t")
	if line == "" {
		return "", fmt.Errorf("empty")
	}
	for i := 0; i < len(line); i++ {
		if line[i] < 0x20 || line[i] == 0x7F {
			return "", fmt.Errorf("control byte 0x%02X at offset %d", line[i], i)
		}
		if line[i] == ':' && i != 1 {
			return "", fmt.Errorf("':' at offset %d — only a drive letter may carry one", i)
		}
	}
	if line[0] == '#' {
		return "", fmt.Errorf("starts with '#' — the firmware would read it as a directive")
	}
	canon, ok := canonicalM3UPath(line)
	if !ok {
		return "", fmt.Errorf("resolves outside the volume root")
	}
	if len(canon) > PathMax {
		return "", fmt.Errorf("canonical path %q is %d bytes, over M3U_PATH_MAX (%d)",
			canon, len(canon), PathMax)
	}
	return line, nil
}

// canonicalM3UPath is path_build() (core/fs/m3u.c:190-291) for the one case
// WriteM3U8 needs: how long the path will be once the firmware has resolved
// it. Entries written by this package are already absolute and clean, so the
// answer is the entry minus its leading separators; a relative entry is
// measured against the Music/Playlists base it will actually be resolved
// against. ok is false for a path that would escape the volume root, or that
// names the root itself — both of which the firmware rejects.
func canonicalM3UPath(line string) (string, bool) {
	rel := line
	absolute := false
	if len(rel) >= 2 && rel[1] == ':' {
		rel, absolute = rel[2:], true
	}
	if strings.HasPrefix(rel, "/") {
		absolute = true
	}
	var out []string
	if !absolute {
		out = append(out, MusicDir, PlaylistDir)
	}
	for _, seg := range strings.Split(rel, "/") {
		switch seg {
		case "", ".":
		case "..":
			if len(out) == 0 {
				return "", false
			}
			out = out[:len(out)-1]
		default:
			out = append(out, seg)
		}
	}
	if len(out) == 0 {
		return "", false
	}
	return strings.Join(out, "/"), true
}
