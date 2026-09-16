// SPDX-License-Identifier: Apache-2.0

package doctor

import (
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
)

// Everything On-The-Go can be wrong about is INVISIBLE on the device: a
// missing COREOTG.DAT means the list is lost at every boot with nothing on
// screen to say so, a torn slot opens to an empty list, and a playlist of the
// user's own at a slot name is quietly never written to. So it is `core
// doctor`'s job to say it out loud, and this is the test of that.

func find(t *testing.T, checks []Check, name string) Check {
	t.Helper()
	for _, c := range checks {
		if c.Name == name {
			return c
		}
	}
	t.Fatalf("no %q check in %v", name, checks)
	return Check{}
}

func TestCheckOTG(t *testing.T) {
	dir := t.TempDir()

	t.Run("missing", func(t *testing.T) {
		c := find(t, CheckOTG(filepath.Join(dir, "nope.dat")), "on-the-go")
		if c.State != Warn || !strings.Contains(c.Text, "core sync") {
			t.Errorf("%+v", c)
		}
	})

	t.Run("valid and empty", func(t *testing.T) {
		root := t.TempDir()
		if _, err := devicefs.EnsureOTG(root); err != nil {
			t.Fatal(err)
		}
		c := find(t, CheckOTG(filepath.Join(root, devicefs.OTGName)), "on-the-go")
		if c.State != OK || !strings.Contains(c.Text, "seq 1") ||
			!strings.Contains(c.Text, "0 track(s)") {
			t.Errorf("%+v", c)
		}
	})

	t.Run("valid with a list in the newer slot", func(t *testing.T) {
		root := t.TempDir()
		blob := make([]byte, devicefs.OTGFileBytes)
		old, err := devicefs.EncodeOTGSlot(nil, 1, 0)
		if err != nil {
			t.Fatal(err)
		}
		newer, err := devicefs.EncodeOTGSlot([]devicefs.OTGEntry{
			{FolderHash: 1, FileHash: 2}, {FolderHash: 3, FileHash: 4},
		}, 2, 7)
		if err != nil {
			t.Fatal(err)
		}
		copy(blob, old[:])
		copy(blob[devicefs.OTGSlotBytes:], newer[:])
		path := filepath.Join(root, devicefs.OTGName)
		if err := os.WriteFile(path, blob, 0o644); err != nil {
			t.Fatal(err)
		}
		c := find(t, CheckOTG(path), "on-the-go")
		if c.State != OK || !strings.Contains(c.Text, "seq 2") ||
			!strings.Contains(c.Text, "2 track(s)") {
			t.Errorf("%+v — the NEWER slot is the one that counts", c)
		}
	})

	t.Run("too short", func(t *testing.T) {
		path := filepath.Join(t.TempDir(), devicefs.OTGName)
		if err := os.WriteFile(path, make([]byte, devicefs.OTGSlotBytes), 0o644); err != nil {
			t.Fatal(err)
		}
		if c := find(t, CheckOTG(path), "on-the-go"); c.State != Fail {
			t.Errorf("%+v — a file too short for two slots is refused by the device", c)
		}
	})

	t.Run("no valid slot", func(t *testing.T) {
		path := filepath.Join(t.TempDir(), devicefs.OTGName)
		if err := os.WriteFile(path, make([]byte, devicefs.OTGFileBytes), 0o644); err != nil {
			t.Fatal(err)
		}
		if c := find(t, CheckOTG(path), "on-the-go"); c.State != Fail {
			t.Errorf("%+v", c)
		}
	})
}

func TestCheckPlaylistsReportsEverySlotState(t *testing.T) {
	dir := t.TempDir()
	if _, err := devicefs.EnsureOTGSlots(dir); err != nil {
		t.Fatal(err)
	}
	pldir := filepath.Join(dir, devicefs.PlaylistDir)

	write := func(n int, b []byte) {
		t.Helper()
		if err := os.WriteFile(filepath.Join(pldir, devicefs.OTGSlotName(n)), b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write(2, savedSlot(t, 9, "/Music/A - B/01. One.flac", "/Music/A - B/02. Two.flac"))
	write(3, tornSlot(t))
	write(4, []byte("#EXTM3U\n/Music/Mine/x.flac\n"))
	if err := os.Remove(filepath.Join(pldir, devicefs.OTGSlotName(5))); err != nil {
		t.Fatal(err)
	}

	checks := CheckPlaylists(pldir)
	for _, tc := range []struct {
		slot  int
		state State
		want  string
	}{
		{1, OK, "empty"},
		{2, OK, "2 track(s)"},
		{3, Fail, "torn save"},
		{4, Warn, "NEVER writes to it"},
		{5, Warn, "missing"},
	} {
		c := find(t, checks, fmt.Sprintf("otg slot %d", tc.slot))
		if c.State != tc.state || !strings.Contains(c.Text, tc.want) {
			t.Errorf("slot %d: %+v, want %s containing %q", tc.slot, c, tc.state, tc.want)
		}
	}

	// An UNREADABLE slot is the same dead end and gets the same way out.
	t.Run("unreadable", func(t *testing.T) {
		dir := t.TempDir()
		if _, err := devicefs.EnsureOTGSlots(dir); err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(dir, devicefs.PlaylistDir, devicefs.OTGSlotName(1))
		if err := os.Remove(path); err != nil {
			t.Fatal(err)
		}
		if err := os.Mkdir(path, 0o755); err != nil {
			t.Fatal(err) // a directory at the path: os.ReadFile fails
		}
		c := find(t, CheckPlaylists(filepath.Join(dir, devicefs.PlaylistDir)),
			"otg slot 1")
		if c.State != Fail || !strings.Contains(c.Text, "core sync") {
			t.Errorf("%+v", c)
		}
	})

	// A slot the device will never write to has to say how to get it back:
	// a foreign file and an interrupted save look identical, and only the
	// person looking at the file can tell them apart. Nothing else the user
	// ever sees mentions this state.
	c4 := find(t, checks, "otg slot 4")
	for _, want := range []string{"delete", "core sync", "On-The-Go 4.m3u8"} {
		if !strings.Contains(c4.Text, want) {
			t.Errorf("the foreign-slot line does not say %q:\n%s", want, c4.Text)
		}
	}

	// The user's own playlists are still counted as playlists, and the five
	// device slots are NOT folded into that number — "5 playlists" would
	// otherwise mean five nobody made.
	if err := os.WriteFile(filepath.Join(pldir, "Gym.m3u8"), []byte("#EXTM3U\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	c := find(t, CheckPlaylists(pldir), "playlists")
	if !strings.Contains(c.Text, "Gym.m3u8") {
		t.Errorf("%+v", c)
	}
}

// savedSlot / tornSlot are what the FIRMWARE leaves behind; the host only
// reads these, so the bytes are built here from core/library/otg_slot.h's
// format rather than borrowed from the code under test.
func savedSlot(t *testing.T, gen int, entries ...string) []byte {
	t.Helper()
	var body strings.Builder
	for _, e := range entries {
		body.WriteString(e)
		body.WriteByte('\n')
	}
	return padSlot(t, "#EXTM3U\n"+
		fmt.Sprintf("#CORE-OTG v1 count=%05d crc=%08X gen=%05d\n",
			len(entries), crc32.ChecksumIEEE([]byte(body.String())), gen)+
		body.String()+
		fmt.Sprintf("#CORE-OTG-END gen=%05d\n", gen))
}

// tornSlot is a power cut before the LAST write: the OLD header (gen 4) over
// the NEW trailer (gen 5).
func tornSlot(t *testing.T) []byte {
	t.Helper()
	body := "/Music/A - B/01. One.flac\n"
	return padSlot(t, "#EXTM3U\n"+
		fmt.Sprintf("#CORE-OTG v1 count=%05d crc=%08X gen=%05d\n",
			1, crc32.ChecksumIEEE([]byte(body)), 4)+
		body+
		fmt.Sprintf("#CORE-OTG-END gen=%05d\n", 5))
}

func padSlot(t *testing.T, head string) []byte {
	t.Helper()
	b := make([]byte, devicefs.OTGSlotFileBytes)
	if copy(b, head) != len(head) {
		t.Fatalf("the fixture does not fit in %d bytes", devicefs.OTGSlotFileBytes)
	}
	for i := len(head); i < len(b); i++ {
		b[i] = '\n'
	}
	return b
}
