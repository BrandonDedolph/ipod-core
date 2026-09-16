// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"encoding/binary"
	"hash/crc32"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The three pairs tools/make_otg.py --emit puts in slot 0, and the sequence
// and gen it stamps them with. Fixed on both sides on purpose: the point of
// the fixture is that the host encoder and the device decoder agree.
var fixtureEntries = []OTGEntry{
	{0x11111111, 0x22222222},
	{0xAABBCCDD, 0x01020304},
	{0xFFFFFFFF, 0x00000001},
}

const (
	fixtureSeq = 1
	fixtureGen = 3
)

// TestEncodeOTGSlotMatchesMakeOTG re-runs the reference implementation and
// compares fresh. The C decoder is held to the same bytes by
// core/tests/kernel/otg_store_test.c, so all three implementations of the
// slot format meet here; without this they can drift and a freshly created
// COREOTG.DAT would be silently refused on the device, with the only symptom
// being "the On-The-Go list never survives a power cycle".
func TestEncodeOTGSlotMatchesMakeOTG(t *testing.T) {
	repo := repoRoot(t)
	if repo == "" {
		t.Skip("not inside the ipod_theme tree (set CORE_REPO)")
	}
	py := python3(t)
	if py == "" {
		t.Skip("python3 not on PATH")
	}
	out := filepath.Join(t.TempDir(), "otg.bin")
	cmd := exec.Command(py, filepath.Join(repo, "tools", "make_otg.py"), "--emit", out)
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("make_otg.py --emit: %v\n%s", err, b)
	}
	b, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != OTGMinBytes {
		t.Fatalf("--emit wrote %d bytes, want %d", len(b), OTGMinBytes)
	}
	slot, err := EncodeOTGSlot(fixtureEntries, fixtureSeq, fixtureGen)
	if err != nil {
		t.Fatal(err)
	}
	if !equalBytes(slot[:], b[:OTGSlotBytes]) {
		t.Errorf("slot 0 differs from make_otg.py --emit\n go %x\n py %x",
			slot[:64], b[:64])
	}
	for i, v := range b[OTGSlotBytes:] {
		if v != 0 {
			t.Fatalf("slot 1 byte %d is %#02x, want 0 (the device's first save lands there)", i, v)
		}
	}
}

// The empty slot PLAYLIST has to match too — it is what a fresh volume's five
// slots hold, and the firmware probes it to decide whether to show them.
func TestEmptyOTGSlotFileMatchesMakeOTG(t *testing.T) {
	repo := repoRoot(t)
	if repo == "" {
		t.Skip("not inside the ipod_theme tree (set CORE_REPO)")
	}
	py := python3(t)
	if py == "" {
		t.Skip("python3 not on PATH")
	}
	out := filepath.Join(t.TempDir(), "slot.m3u8")
	cmd := exec.Command(py, filepath.Join(repo, "tools", "make_otg.py"), "--emit-slot", out)
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("make_otg.py --emit-slot: %v\n%s", err, b)
	}
	b, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	mine, err := EmptyOTGSlotFile(len(b))
	if err != nil {
		t.Fatal(err)
	}
	if !equalBytes(mine, b) {
		t.Errorf("the empty slot playlist differs from make_otg.py --emit-slot\n go %q\n py %q",
			mine[:80], b[:80])
	}
}

func TestOTGSlotRoundTrip(t *testing.T) {
	for _, tc := range []struct {
		name string
		e    []OTGEntry
	}{
		{"empty", nil},
		{"one", []OTGEntry{{1, 2}}},
		{"the fixture", fixtureEntries},
		{"full", func() []OTGEntry {
			e := make([]OTGEntry, OTGMax)
			for i := range e {
				e[i] = OTGEntry{uint32(i + 1), uint32(i + 1000)}
			}
			return e
		}()},
	} {
		t.Run(tc.name, func(t *testing.T) {
			rec, err := EncodeOTGSlot(tc.e, 77, 5)
			if err != nil {
				t.Fatal(err)
			}
			seq, gen, got, ok := DecodeOTGSlot(rec[:])
			if !ok {
				t.Fatal("does not decode")
			}
			if seq != 77 || gen != 5 {
				t.Errorf("seq/gen = %d/%d, want 77/5", seq, gen)
			}
			if len(got) != len(tc.e) {
				t.Fatalf("%d entries back, want %d", len(got), len(tc.e))
			}
			for i := range got {
				if got[i] != tc.e[i] {
					t.Errorf("entry %d = %+v, want %+v", i, got[i], tc.e[i])
				}
			}
			// The same list must always encode to the same bytes: the
			// reserved word and the padding are zero and the CRC covers
			// them, which is what makes the three implementations
			// comparable at all.
			again, _ := EncodeOTGSlot(tc.e, 77, 5)
			if !equalBytes(rec[:], again[:]) {
				t.Error("encoding is not deterministic")
			}
		})
	}
	if _, err := EncodeOTGSlot(make([]OTGEntry, OTGMax+1), 1, 0); err == nil {
		t.Error("encoding more than OTG_MAX entries was not refused")
	}
}

func TestOTGSlotRejects(t *testing.T) {
	good, err := EncodeOTGSlot(fixtureEntries, 9, 1)
	if err != nil {
		t.Fatal(err)
	}
	fix := func(b []byte) { // re-CRC, so it is the FIELD that is refused
		binary.LittleEndian.PutUint32(b[otgOffCRC:], crc32.ChecksumIEEE(b[:otgOffCRC]))
	}
	for _, tc := range []struct {
		name string
		mut  func(b []byte)
	}{
		{"bad magic", func(b []byte) { b[0] ^= 0xFF }},
		{"version 0", func(b []byte) { binary.LittleEndian.PutUint16(b[otgOffVersion:], 0); fix(b) }},
		{"a future version", func(b []byte) { binary.LittleEndian.PutUint16(b[otgOffVersion:], 2); fix(b) }},
		{"count past the ceiling", func(b []byte) {
			binary.LittleEndian.PutUint16(b[otgOffCount:], OTGMax+1)
			fix(b)
		}},
		{"a flipped header bit", func(b []byte) { b[otgOffSeq] ^= 1 }},
		{"a flipped entry bit", func(b []byte) { b[otgOffEntries] ^= 1 }},
		{"a flipped padding bit", func(b []byte) { b[otgOffCRC-1] ^= 1 }},
		{"a flipped CRC bit", func(b []byte) { b[otgOffCRC] ^= 1 }},
		{"all zero", func(b []byte) {
			for i := range b {
				b[i] = 0
			}
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			b := make([]byte, OTGSlotBytes)
			copy(b, good[:])
			tc.mut(b)
			if _, _, _, ok := DecodeOTGSlot(b); ok {
				t.Error("decoded a slot that should have been refused")
			}
		})
	}
	if _, _, _, ok := DecodeOTGSlot(good[:OTGSlotBytes-1]); ok {
		t.Error("a short buffer decoded")
	}

	// A (0, 0) pair inside the declared count is DROPPED, not stored: it is
	// what the padding is made of, so a hand-edited file cannot inject an
	// entry that binds to nothing.
	b := make([]byte, OTGSlotBytes)
	copy(b, good[:])
	binary.LittleEndian.PutUint32(b[otgOffEntries+otgEntryBytes:], 0)
	binary.LittleEndian.PutUint32(b[otgOffEntries+otgEntryBytes+4:], 0)
	fix(b)
	_, _, entries, ok := DecodeOTGSlot(b)
	if !ok || len(entries) != 2 ||
		entries[0] != fixtureEntries[0] || entries[1] != fixtureEntries[2] {
		t.Errorf("a null pair inside the count was not dropped: %v (ok %v)", entries, ok)
	}
}

func TestOTGSeqNewerWraps(t *testing.T) {
	if !OTGSeqNewer(0, 0xFFFFFFFF) {
		t.Error("seq 0 should be newer than 0xFFFFFFFF")
	}
	if OTGSeqNewer(0xFFFFFFFF, 0) || OTGSeqNewer(5, 5) {
		t.Error("the wrapping comparison is not the firmware's")
	}
	// OTGFileValid picks the newer slot by that rule, not by >.
	blob := make([]byte, OTGMinBytes)
	a, _ := EncodeOTGSlot([]OTGEntry{{1, 1}}, 0xFFFFFFFF, 0)
	b, _ := EncodeOTGSlot([]OTGEntry{{2, 2}}, 0, 0)
	copy(blob, a[:])
	copy(blob[OTGSlotBytes:], b[:])
	if seq, ok := OTGFileValid(blob); !ok || seq != 0 {
		t.Errorf("OTGFileValid = %d/%v across a wrap, want 0/true", seq, ok)
	}
}

func TestOTGSlotIndex(t *testing.T) {
	for name, want := range map[string]int{
		"On-The-Go 1.m3u8":  1,
		"On-The-Go 5.m3u8":  OTGPlaylistSlots,
		"on-the-go 3.M3U8":  3,
		"On-The-Go 3.m3u":   3,
		"On-The-Go 6.m3u8":  0,
		"On-The-Go 0.m3u8":  0,
		"On-The-Go.m3u8":    0,
		"On-The-Go 12.m3u8": 0,
		"On-The-Go 1":       0, // no extension: not a playlist file
		"On-The-Go 1.txt":   0,
		"Favourites.m3u8":   0,
		"":                  0,
	} {
		if got := OTGSlotIndex(name); got != want {
			t.Errorf("OTGSlotIndex(%q) = %d, want %d", name, got, want)
		}
	}
	for n := 1; n <= OTGPlaylistSlots; n++ {
		if got := OTGSlotIndex(OTGSlotName(n)); got != n {
			t.Errorf("OTGSlotName(%d) does not round-trip: %d", n, got)
		}
	}
}

func TestEnsureOTGCreatesAndIsIdempotent(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, OTGName)

	created, err := EnsureOTG(root)
	if err != nil {
		t.Fatal(err)
	}
	if !created {
		t.Fatal("first EnsureOTG reported created=false")
	}
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != OTGFileBytes {
		t.Errorf("file is %d bytes, want %d", len(b), OTGFileBytes)
	}
	seq, gen, entries, ok := DecodeOTGSlot(b[:OTGSlotBytes])
	if !ok || seq != 1 || gen != 0 || len(entries) != 0 {
		t.Errorf("slot 0 = seq %d gen %d %d entries (ok %v), want an empty list at seq 1",
			seq, gen, len(entries), ok)
	}
	if _, _, _, ok := DecodeOTGSlot(b[OTGSlotBytes : 2*OTGSlotBytes]); ok {
		t.Error("slot 1 decodes; it must be empty so the first save lands there")
	}
	for i, v := range b[2*OTGSlotBytes:] {
		if v != 0 {
			t.Fatalf("padding byte %d is %#02x, want 0", i+2*OTGSlotBytes, v)
		}
	}

	// The list the user built is not something a re-sync may throw away.
	saved, err := EncodeOTGSlot(fixtureEntries, 57, 9)
	if err != nil {
		t.Fatal(err)
	}
	blob := make([]byte, OTGFileBytes)
	copy(blob[OTGSlotBytes:], saved[:]) // as the device leaves it: slot 1
	writeFixture(t, path, blob)
	before := stat(t, path)
	created, err = EnsureOTG(root)
	if err != nil {
		t.Fatal(err)
	}
	if created {
		t.Error("rewrote a COREOTG.DAT that already held a valid slot")
	}
	assertUntouched(t, path, before, blob)
}

func TestEnsureOTGRewritesInvalid(t *testing.T) {
	for _, tc := range []struct {
		name string
		blob []byte
	}{
		{"empty file", nil},
		{"shorter than two slots", make([]byte, OTGSlotBytes)},
		{"long enough but holding nothing valid", make([]byte, OTGFileBytes)},
		{"a torn slot", func() []byte {
			b := make([]byte, OTGFileBytes)
			rec, _ := EncodeOTGSlot(fixtureEntries, 3, 0)
			copy(b, rec[:])
			b[40] ^= 1
			return b
		}()},
	} {
		t.Run(tc.name, func(t *testing.T) {
			root := t.TempDir()
			path := filepath.Join(root, OTGName)
			writeFixture(t, path, tc.blob)
			created, err := EnsureOTG(root)
			if err != nil {
				t.Fatal(err)
			}
			if !created {
				t.Fatal("left a file the firmware would refuse")
			}
			b, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if seq, ok := OTGFileValid(b); !ok || seq != 1 {
				t.Errorf("the rewritten file is seq %d valid=%v, want 1/true", seq, ok)
			}
		})
	}
}

func TestEnsureOTGSlotsCreatesOnlyAbsent(t *testing.T) {
	root := t.TempDir()
	music := filepath.Join(root, MusicDir)

	created, err := EnsureOTGSlots(music)
	if err != nil {
		t.Fatal(err)
	}
	if len(created) != OTGPlaylistSlots {
		t.Fatalf("created %v, want all %d", created, OTGPlaylistSlots)
	}
	for n := 1; n <= OTGPlaylistSlots; n++ {
		path := filepath.Join(music, PlaylistDir, OTGSlotName(n))
		info, err := OTGSlotFileState(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.State != OTGSlotEmpty {
			t.Errorf("%s is %q, want %q", OTGSlotName(n), info.State, OTGSlotEmpty)
		}
		if info.Size != OTGSlotFileBytes {
			t.Errorf("%s is %d bytes, want %d", OTGSlotName(n), info.Size, OTGSlotFileBytes)
		}
	}

	// A second run creates nothing.
	created, err = EnsureOTGSlots(music)
	if err != nil {
		t.Fatal(err)
	}
	if len(created) != 0 {
		t.Errorf("a second run created %v", created)
	}

	// A file already at a slot name is NEVER rewritten — not a list the user
	// saved on the device, and not a playlist of their own that happens to
	// use the name. This is stricter than EnsureConfig/EnsureLog on purpose:
	// there is nothing here the device would refuse.
	foreign := []byte("#EXTM3U\n/Music/Mine/track.flac\n")
	path := filepath.Join(music, PlaylistDir, OTGSlotName(2))
	writeFixture(t, path, foreign)
	saved, err := otgSlotFileForTest(8192, []string{"/Music/A - B/01.flac"}, 12)
	if err != nil {
		t.Fatal(err)
	}
	savedPath := filepath.Join(music, PlaylistDir, OTGSlotName(4))
	writeFixture(t, savedPath, saved)

	beforeF, beforeS := stat(t, path), stat(t, savedPath)
	created, err = EnsureOTGSlots(music)
	if err != nil {
		t.Fatal(err)
	}
	if len(created) != 0 {
		t.Errorf("a run over existing slots created %v", created)
	}
	assertUntouched(t, path, beforeF, foreign)
	assertUntouched(t, savedPath, beforeS, saved)

	// And one absent among four present is the only one made.
	if err := os.Remove(filepath.Join(music, PlaylistDir, OTGSlotName(3))); err != nil {
		t.Fatal(err)
	}
	created, err = EnsureOTGSlots(music)
	if err != nil {
		t.Fatal(err)
	}
	if len(created) != 1 || created[0] != OTGSlotName(3) {
		t.Errorf("created %v, want just %s", created, OTGSlotName(3))
	}
}

// otgSlotFileForTest builds a USED slot playlist — the shape the device
// writes, which the host only ever reads.
func otgSlotFileForTest(size int, entries []string, gen uint16) ([]byte, error) {
	return otgSlotFile(size, entries, gen)
}

func TestReadOTGSlotFile(t *testing.T) {
	dir := t.TempDir()
	tracks := []string{
		"/Music/Artist - Album/01 Song.flac",
		"/Music/Artist - Album/02 Other.fla",
		"/Music/Loose.flac",
	}

	t.Run("used", func(t *testing.T) {
		path := filepath.Join(dir, "used.m3u8")
		b, err := otgSlotFileForTest(OTGSlotFileBytes, tracks, 42)
		if err != nil {
			t.Fatal(err)
		}
		writeFixture(t, path, b)
		got, info, err := ReadOTGSlotFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.State != OTGSlotUsed || info.Count != 3 || info.Gen != 42 {
			t.Errorf("info = %+v", info)
		}
		if strings.Join(got, "|") != strings.Join(tracks, "|") {
			t.Errorf("entries = %q, want %q", got, tracks)
		}
	})

	t.Run("empty", func(t *testing.T) {
		path := filepath.Join(dir, "empty.m3u8")
		b, err := EmptyOTGSlotFile(OTGSlotFileMin)
		if err != nil {
			t.Fatal(err)
		}
		writeFixture(t, path, b)
		got, info, err := ReadOTGSlotFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.State != OTGSlotEmpty || len(got) != 0 {
			t.Errorf("info = %+v, entries %q", info, got)
		}
	})

	t.Run("damaged", func(t *testing.T) {
		// A cut before the LAST write leaves the old header — an older gen —
		// over the new trailer. That is the tear the write order manufactures.
		path := filepath.Join(dir, "torn.m3u8")
		b, err := otgSlotFileForTest(OTGSlotFileBytes, tracks, 42)
		if err != nil {
			t.Fatal(err)
		}
		hdr := []byte(strings.Replace(string(b[8:8+otgHdrBytes]), "gen=00042", "gen=00041", 1))
		copy(b[8:], hdr)
		writeFixture(t, path, b)
		got, info, err := ReadOTGSlotFile(path)
		if err == nil {
			t.Error("a torn save was reported as fine")
		}
		if info.State != OTGSlotDamaged {
			t.Errorf("state = %q, want %q", info.State, OTGSlotDamaged)
		}
		if len(got) != 3 {
			t.Errorf("a damaged file still hands back what it holds: %q", got)
		}
	})

	t.Run("count mismatch", func(t *testing.T) {
		path := filepath.Join(dir, "short.m3u8")
		b, err := otgSlotFileForTest(OTGSlotFileBytes, tracks, 7)
		if err != nil {
			t.Fatal(err)
		}
		copy(b[8:], []byte(strings.Replace(string(b[8:8+otgHdrBytes]),
			"count=00003", "count=00005", 1)))
		writeFixture(t, path, b)
		if _, info, err := ReadOTGSlotFile(path); err == nil ||
			info.State != OTGSlotDamaged {
			t.Errorf("a line count that disagrees with the header is a tear: %+v %v", info, err)
		}
	})

	t.Run("foreign", func(t *testing.T) {
		path := filepath.Join(dir, "foreign.m3u8")
		writeFixture(t, path, []byte("#EXTM3U\n/Music/Mine/a.flac\n/Music/Mine/b.flac\n"))
		got, info, err := ReadOTGSlotFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.State != OTGSlotForeign || len(got) != 2 {
			t.Errorf("info = %+v, entries %q; a playlist with no directive is "+
				"an ordinary playlist", info, got)
		}
	})

	t.Run("absent", func(t *testing.T) {
		if _, info, err := ReadOTGSlotFile(filepath.Join(dir, "nope.m3u8")); err == nil ||
			info.State != OTGSlotAbsent {
			t.Errorf("a missing slot should say so: %+v %v", info, err)
		}
	})
}

func TestOTGSlotFileSizesAreRefused(t *testing.T) {
	for _, size := range []int{0, OTGSlotFileMin - OTGSlotSizeGrain, 5000} {
		if _, err := EmptyOTGSlotFile(size); err == nil {
			t.Errorf("size %d was accepted; the firmware only writes a slot file "+
				"of at least %d bytes and a multiple of %d",
				size, OTGSlotFileMin, OTGSlotSizeGrain)
		}
	}
	// The host-created size holds a full 512-track list at the worst-case
	// path length, whole — that is the whole reason it is 128 KiB.
	worst := make([]string, OTGMax)
	for i := range worst {
		worst[i] = "/" + strings.Repeat("x", PathMax)
	}
	if _, err := otgSlotFileForTest(OTGSlotFileBytes, worst, 0); err != nil {
		t.Errorf("a full list at the worst-case path length does not fit: %v", err)
	}
}
