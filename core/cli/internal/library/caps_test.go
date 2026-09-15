package library

import (
	"os"
	"path/filepath"
	"regexp"
	"testing"
)

// TestCapsMatchTheFirmware: the three caps are C #defines duplicated into Go
// constants. Past a cap the device drops records silently — no error, no log,
// the songs simply are not in the library — so the host has to know the real
// numbers. This is the test that keeps the copy honest.
func TestCapsMatchTheFirmware(t *testing.T) {
	repo := repoRoot()
	if repo == "" {
		t.Skip("firmware repo not found (set CORE_REPO); cannot check the caps")
	}
	mainC := filepath.Join(repo, "core", "kernel", "main.c")
	src, err := os.ReadFile(mainC)
	if err != nil {
		t.Skipf("read %s: %v", mainC, err)
	}
	for _, c := range []struct {
		define string
		go_    int
	}{
		{"LIB_MAX_SONGS", MaxSongs},
		{"LIB_MAX_ALBUMS", MaxAlbums},
		{"LIB_MAX_GENRES", MaxGenres},
	} {
		re := regexp.MustCompile(`(?m)^#define\s+` + c.define + `\s+(\d+)`)
		m := re.FindSubmatch(src)
		if m == nil {
			t.Errorf("%s is not #defined in %s any more — the cap check is blind", c.define, mainC)
			continue
		}
		if got := atoi(string(m[1])); got != c.go_ {
			t.Errorf("%s is %d in the firmware, %d here", c.define, got, c.go_)
		}
	}
}
