// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// parseM3U is a tiny stand-in for core/fs/m3u.c: enough of the firmware's
// line rules to prove that what WriteM3U8 puts on the disk is what the device
// reads back. It deliberately mirrors the C rather than the writer —
// splitting on CR *or* LF, skipping '#' lines and blanks, trimming spaces and
// tabs, accepting both separators, and resolving each line against `base` the
// way path_build() does.
//
// Returns the canonical volume-root-relative paths, plus the lines it refused.
func parseM3U(data []byte, base string) (paths, refused []string) {
	s := strings.TrimPrefix(string(data), "\xef\xbb\xbf") // BOM at offset 0 only
	for _, raw := range strings.FieldsFunc(s, func(r rune) bool {
		return r == '\n' || r == '\r'
	}) {
		line := strings.Trim(raw, " \t")
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		rel := strings.ReplaceAll(line, "\\", "/")
		bad := false
		for i := 0; i < len(rel); i++ {
			if rel[i] < 0x20 || (rel[i] == ':' && i != 1) {
				bad = true
				break
			}
		}
		if bad {
			refused = append(refused, line)
			continue
		}
		absolute := false
		if len(rel) >= 2 && rel[1] == ':' {
			rel, absolute = rel[2:], true
		}
		if strings.HasPrefix(rel, "/") {
			absolute = true
		}
		var out []string
		if !absolute && base != "" {
			out = strings.Split(base, "/")
		}
		escaped := false
		for _, seg := range strings.Split(rel, "/") {
			switch seg {
			case "", ".":
			case "..":
				if len(out) == 0 {
					escaped = true
				} else {
					out = out[:len(out)-1]
				}
			default:
				out = append(out, seg)
			}
		}
		if escaped || len(out) == 0 || len(strings.Join(out, "/")) > PathMax {
			refused = append(refused, line)
			continue
		}
		paths = append(paths, strings.Join(out, "/"))
	}
	return paths, refused
}

// firmwareBase is what core/kernel/main.c:2899-2902 hands the parser for a
// playlist in Music/Playlists — the fact the plan got wrong.
const firmwareBase = MusicDir + "/" + PlaylistDir

func TestWriteM3U8RoundTrip(t *testing.T) {
	dir := t.TempDir()
	dest := filepath.Join(dir, MusicDir, PlaylistDir, "Road Trip.m3u8")

	entries := []string{
		PlaylistEntry("Boards of Canada - Music Has the Right to Children", "01. Wildlife Analysis.flac"),
		PlaylistEntry("Émilie Simon - Végétal", "07. Dame de lotus.flac"),
		"/Music/Artist - Album/02. Other.flac",
		"../Artist - Album/03. Third.flac", // relative-with-a-pop also works
	}
	if err := WriteM3U8(dest, entries); err != nil {
		t.Fatal(err)
	}

	data, err := os.ReadFile(dest)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(string(data), "#EXTM3U\n") {
		t.Errorf("file does not start with the #EXTM3U header: %q", first(string(data), 32))
	}
	if strings.Contains(string(data), "\r") {
		t.Error("CR in the output; the writer must emit LF only")
	}
	if strings.Contains(string(data), "\\") {
		t.Error("backslash in the output; the writer must emit forward slashes")
	}
	if got, want := strings.Count(string(data), "\n"), len(entries)+1; got != want {
		t.Errorf("%d lines, want %d (header + entries)", got, want)
	}

	paths, refused := parseM3U(data, firmwareBase)
	if len(refused) != 0 {
		t.Errorf("the firmware's rules would refuse %q", refused)
	}
	want := []string{
		"Music/Boards of Canada - Music Has the Right to Children/01. Wildlife Analysis.flac",
		"Music/Émilie Simon - Végétal/07. Dame de lotus.flac",
		"Music/Artist - Album/02. Other.flac",
		"Music/Artist - Album/03. Third.flac",
	}
	if len(paths) != len(want) {
		t.Fatalf("parsed %d paths, want %d: %q", len(paths), len(want), paths)
	}
	for i := range want {
		if paths[i] != want[i] {
			t.Errorf("path %d = %q, want %q", i, paths[i], want[i])
		}
	}
}

// The plan says "paths relative to Music/". They are not: the base is the
// playlist's own directory, Music/Playlists. This is the test that pins it,
// so a future refactor cannot quietly reintroduce the bug.
func TestBarePathIsRelativeToPlaylistsNotMusic(t *testing.T) {
	paths, _ := parseM3U([]byte("#EXTM3U\nArtist - Album/01. Song.flac\n"), firmwareBase)
	if len(paths) != 1 {
		t.Fatalf("parsed %q", paths)
	}
	if paths[0] != "Music/Playlists/Artist - Album/01. Song.flac" {
		t.Fatalf("a bare relative line resolved to %q", paths[0])
	}
	if paths[0] == "Music/Artist - Album/01. Song.flac" {
		t.Fatal("a bare relative line is NOT relative to Music/")
	}
}

func TestWriteM3U8AcceptsTheFirmwaresOwnFixtureShapes(t *testing.T) {
	// Every entry shape core/tests/library/playlist_test.c:153-162 feeds the
	// real parser, minus the ones it expects to fail.
	entries := []string{
		"../Artist - Album/01 Song.flac",
		"/Music/Artist - Album/02 Other.flac",
		`\Music\Artist - Album\Missing.flac`,
		"/Root.flac",
	}
	dest := filepath.Join(t.TempDir(), "Favourites.m3u8")
	if err := WriteM3U8(dest, entries); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(dest)
	if err != nil {
		t.Fatal(err)
	}
	paths, refused := parseM3U(data, firmwareBase)
	if len(refused) != 0 {
		t.Fatalf("refused %q", refused)
	}
	want := []string{
		"Music/Artist - Album/01 Song.flac",
		"Music/Artist - Album/02 Other.flac",
		"Music/Artist - Album/Missing.flac",
		"Root.flac",
	}
	for i := range want {
		if paths[i] != want[i] {
			t.Errorf("path %d = %q, want %q", i, paths[i], want[i])
		}
	}
}

func TestWriteM3U8RejectsEntriesTheDeviceWouldDrop(t *testing.T) {
	long := "/Music/" + strings.Repeat("x", PathMax) + "/t.flac"
	cases := map[string]string{
		"empty":            "",
		"blank":            "   \t ",
		"newline":          "/Music/A - B/one.flac\n/Music/A - B/two.flac",
		"carriage return":  "/Music/A - B/one.flac\r",
		"url":              "http://example.com/x.flac",
		"comment":          "#EXTINF:1,nope",
		"escapes the root": "../../../outside.flac", // Music/Playlists has only two levels to pop
		"names the root":   "/",
		"over PathMax":     long,
	}
	for name, e := range cases {
		t.Run(name, func(t *testing.T) {
			dest := filepath.Join(t.TempDir(), "x.m3u8")
			if err := WriteM3U8(dest, []string{e}); err == nil {
				t.Errorf("wrote %q, which the firmware would drop", e)
			}
		})
	}
}

func TestPlaylistEntry(t *testing.T) {
	got := PlaylistEntry("Artist - Album", "01. Title.flac")
	if want := "/Music/Artist - Album/01. Title.flac"; got != want {
		t.Errorf("PlaylistEntry = %q, want %q", got, want)
	}
}

func first(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}
