package fwpart

import (
	"bytes"
	"os"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
)

// --- the Apple-shaped partition --------------------------------------
//
// buildFixture above is our device: an OSOS row with entry point 0 and
// a 237 KB body. This one is a STOCK iPod, which is the shape `core
// install` exists for and the shape no test had until now: the row read
// off bootpartition-backup.bin, len 7,618,128, entryOffset 0x736000,
// addr 0x10000000, vers 0xB012, loadAddr2 0xFFFFFFFF.
//
// The 7.6 MB body is not stored. sparse reads zeros for anything that
// was never written, which is what ReadBody sees, and a body of zeros
// is a body with no version marker — exactly what Apple's is, for this
// purpose.

const (
	appleOSOSLength      = 7618128
	appleOSOSEntryOffset = 0x736000
)

// buildAppleFixture is buildFixture with Apple's OSOS row.
func buildAppleFixture(t *testing.T) *fixture {
	t.Helper()
	f := &fixture{sp: &sparse{size: realPartitionSize}, bodies: map[string][]byte{}}
	f.sp.put(0, preamble(3, 0x4000))

	rows := []firmware.DirectoryEntry{{
		ContainerID: tag("!ATA"),
		ImageType:   tag("soso"),
		DevOffset:   ososDevOffset,
		Length:      appleOSOSLength,
		LoadAddr:    0x10000000,
		EntryOffset: appleOSOSEntryOffset,
		Checksum:    0x12345678, // Apple's; nothing here re-sums 7.6 MB
		Version:     0xB012,
		LoadAddr2:   0xFFFFFFFF,
	}, {
		ContainerID: tag("!ATA"),
		ImageType:   tag("crsr"),
		DevOffset:   rsrcDevOffset,
		Length:      64 << 10,
		EntryOffset: 0x600,
		Version:     0xB012,
		LoadAddr2:   0xFFFFFFFF,
	}}
	var dir bytes.Buffer
	for i, e := range rows {
		if err := firmware.WriteDirectoryEntry(&dir, e); err != nil {
			t.Fatalf("encode entry %d: %v", i, err)
		}
	}
	end := make([]byte, EntrySize)
	copy(end[36:], []byte{0xFF, 0xFF, 0xFF, 0xFF})
	dir.Write(end)
	f.sp.put(dirStart, dir.Bytes())
	f.part = Partition{R: f.sp, Size: realPartitionSize}
	return f
}

// TestPlanWriteCoreImagePolicy is the gate for L1: flashing our image
// onto a stock iPod has to leave a row the boot ROM can enter.
//
// With KeepEntry — what this package did before — the row would say
// "start at 0x736000" over a 368 KB body: recoverable with Select+Play,
// but a device that does not boot.
func TestPlanWriteCoreImagePolicy(t *testing.T) {
	f := buildAppleFixture(t)
	d, err := Parse(f.part)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	idx, old, ok := d.OSOS()
	if !ok {
		t.Fatal("no OSOS entry in the Apple-shaped fixture")
	}
	if old.EntryOffset != appleOSOSEntryOffset || old.Length != appleOSOSLength {
		t.Fatalf("fixture OSOS = len %d entry %#x, want the Apple row",
			old.Length, old.EntryOffset)
	}
	image := newImage()

	writes, got, err := PlanWrite(d, idx, image, 2048, CoreImage)
	if err != nil {
		t.Fatalf("PlanWrite(CoreImage): %v", err)
	}
	if got.EntryOffset != 0 {
		t.Errorf("EntryOffset = %#x, want 0 — the image's entry point is its first byte",
			got.EntryOffset)
	}
	if got.Length != uint32(len(image)) {
		t.Errorf("Length = %d, want %d", got.Length, len(image))
	}
	if want := ImageChecksum(image); got.Checksum != want {
		t.Errorf("Checksum = %#08x, want %#08x", got.Checksum, want)
	}
	if got.LoadAddr != old.LoadAddr || got.Version != old.Version ||
		got.LoadAddr2 != old.LoadAddr2 || got.DevOffset != old.DevOffset ||
		got.ContainerID != old.ContainerID || got.ImageType != old.ImageType ||
		got.ImageID != old.ImageID {
		t.Errorf("a field other than Length/Checksum/EntryOffset changed:\n old %+v\n new %+v",
			old, got)
	}
	if got.LoadAddr != 0x10000000 || got.Version != 0xB012 || got.LoadAddr2 != 0xFFFFFFFF {
		t.Errorf("addr/vers/la2 = %#x/%#x/%#x, want 0x10000000/0xb012/0xffffffff",
			got.LoadAddr, got.Version, got.LoadAddr2)
	}

	// The row as it is actually encoded into the sector write, not just
	// the value returned: the bytes are what reaches the device.
	within := int(int64(dirStart) - writes[1].Off)
	inSector, err := firmware.ReadDirectoryEntry(
		bytes.NewReader(writes[1].Data[within : within+EntrySize]))
	if err != nil {
		t.Fatalf("decode the written row: %v", err)
	}
	if inSector != got {
		t.Errorf("the row in the sector = %+v, PlanWrite returned %+v", inSector, got)
	}

	// And the other policy leaves Apple's value alone, which is the
	// behaviour that made this policy necessary.
	_, kept, err := PlanWrite(d, idx, image, 2048, KeepEntry)
	if err != nil {
		t.Fatalf("PlanWrite(KeepEntry): %v", err)
	}
	if kept.EntryOffset != appleOSOSEntryOffset {
		t.Errorf("KeepEntry EntryOffset = %#x, want %#x",
			kept.EntryOffset, appleOSOSEntryOffset)
	}
	if kept.Length != got.Length || kept.Checksum != got.Checksum {
		t.Error("the two policies differ in something other than EntryOffset")
	}
}

// versionBody is an image body carrying the marker, the way a real one
// does: a NUL-terminated string somewhere in the middle of .rodata.
func versionBody(n int, version, build string) []byte {
	b := make([]byte, n)
	copy(b[n/2:], VersionTag+version+"|"+build+"\x00")
	return b
}

func TestClassify(t *testing.T) {
	apple := firmware.DirectoryEntry{
		Length: appleOSOSLength, EntryOffset: appleOSOSEntryOffset,
		LoadAddr: 0x10000000, Version: 0xB012, LoadAddr2: 0xFFFFFFFF,
	}
	core := firmware.DirectoryEntry{Length: 367608, EntryOffset: 0, LoadAddr: 0x10000000}

	for _, tc := range []struct {
		name     string
		entry    firmware.DirectoryEntry
		body     []byte
		wantKind InstalledKind
		wantDesc string
	}{
		{
			name:     "core with a marker",
			entry:    core,
			body:     versionBody(4096, "v0.1.3", "v0.1.3-2-gdeadbee"),
			wantKind: Core,
			wantDesc: "Core v0.1.3 (build v0.1.3-2-gdeadbee)",
		},
		{
			// The device this project actually runs, until the next
			// flash: our image, no marker, small, entry 0.
			name:     "core before the marker",
			entry:    core,
			body:     make([]byte, 4096),
			wantKind: CoreOld,
			wantDesc: "Core before v0.1.3 (367608 bytes, entry 0x0, no version marker)",
		},
		{
			name:     "apple",
			entry:    apple,
			body:     nil,
			wantKind: Other,
			wantDesc: "Apple firmware (7.6 MB, entry 0x736000)",
		},
		{
			// A marker in a body whose row is Apple's is still Core:
			// the marker is the strongest evidence there is, and this
			// is what a device looks like between the body write and
			// the directory write of an interrupted flash.
			name:     "marker wins over the row",
			entry:    apple,
			body:     versionBody(4096, "v0.1.4", "v0.1.4"),
			wantKind: Core,
			wantDesc: "Core v0.1.4 (build v0.1.4)",
		},
		{
			name:     "somebody else's big image, entry 0",
			entry:    firmware.DirectoryEntry{Length: 2 << 20},
			body:     make([]byte, 16),
			wantKind: Other,
			wantDesc: "unknown firmware (2097152 bytes, entry 0x0)",
		},
		{
			name:     "small but with an entry point",
			entry:    firmware.DirectoryEntry{Length: 4096, EntryOffset: 0x200},
			body:     make([]byte, 16),
			wantKind: Other,
			wantDesc: "unknown firmware (4096 bytes, entry 0x200)",
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got := Classify(tc.entry, tc.body)
			if got.Kind != tc.wantKind {
				t.Errorf("Kind = %s, want %s", got.Kind, tc.wantKind)
			}
			if got.Description != tc.wantDesc {
				t.Errorf("Description = %q, want %q", got.Description, tc.wantDesc)
			}
			if got.IsCore() != (tc.wantKind == Core || tc.wantKind == CoreOld) {
				t.Errorf("IsCore() = %v for kind %s", got.IsCore(), got.Kind)
			}
			if tc.wantKind == Core && got.Version == "" {
				t.Error("a Core classification carries no version")
			}
			if tc.wantKind != Core && (got.Version != "" || got.BuildID != "") {
				t.Errorf("a %s classification carries a version: %+v", got.Kind, got)
			}
		})
	}
}

// The zero Installed must not read as "Core is installed": it is the
// value a caller that never classified anything holds, and it decides
// whether a write is an install or an update.
func TestZeroInstalledIsNotCore(t *testing.T) {
	var i Installed
	if i.IsCore() {
		t.Error("the zero Installed claims Core is installed")
	}
	if i.Kind.String() != "unclassified" {
		t.Errorf("zero Kind prints as %q", i.Kind.String())
	}
}

func TestClassifyPartition(t *testing.T) {
	t.Run("apple", func(t *testing.T) {
		f := buildAppleFixture(t)
		got, osos, err := ClassifyPartition(f.part)
		if err != nil {
			t.Fatalf("ClassifyPartition: %v", err)
		}
		if got.Kind != Other {
			t.Errorf("Kind = %s, want %s (%s)", got.Kind, Other, got.Description)
		}
		if osos.EntryOffset != appleOSOSEntryOffset {
			t.Errorf("OSOS row = %+v", osos)
		}
	})
	t.Run("core with a marker on the device", func(t *testing.T) {
		f := buildFixture(t)
		d, err := Parse(f.part)
		if err != nil {
			t.Fatalf("Parse: %v", err)
		}
		idx, osos, _ := d.OSOS()
		body := versionBody(int(osos.Length), "v0.1.3", "v0.1.3-dirty")
		f.sp.put(BodyOffset(osos), body)
		// Keep the row honest, so the fixture is a device that would
		// verify.
		writes, _, err := PlanWrite(d, idx, body, 2048, CoreImage)
		if err != nil {
			t.Fatalf("PlanWrite: %v", err)
		}
		for _, w := range writes {
			f.sp.apply(w)
		}
		got, _, err := ClassifyPartition(f.part)
		if err != nil {
			t.Fatalf("ClassifyPartition: %v", err)
		}
		if got.Kind != Core || got.Version != "v0.1.3" || got.BuildID != "v0.1.3-dirty" {
			t.Errorf("Classify = %+v, want Core v0.1.3 / v0.1.3-dirty", got)
		}
	})
	t.Run("core before the marker", func(t *testing.T) {
		f := buildFixture(t)
		got, _, err := ClassifyPartition(f.part)
		if err != nil {
			t.Fatalf("ClassifyPartition: %v", err)
		}
		if got.Kind != CoreOld {
			t.Errorf("Kind = %s, want %s (%s)", got.Kind, CoreOld, got.Description)
		}
	})
	t.Run("no directory", func(t *testing.T) {
		sp := &sparse{size: 1 << 20}
		if _, _, err := ClassifyPartition(Partition{R: sp, Size: 1 << 20}); err == nil {
			t.Error("ClassifyPartition on a blank partition succeeded")
		}
	})
}

// An OSOS row whose Length runs past the end of the partition cannot be
// read. That is Other — not Core, and not an error: `core install` has
// to be able to fix exactly that device.
func TestClassifyEntryUnreadableBody(t *testing.T) {
	f := buildAppleFixture(t)
	got := ClassifyEntry(f.part, firmware.DirectoryEntry{
		DevOffset: ososDevOffset, Length: 0xFFFFFF00, EntryOffset: 0,
	})
	if got.Kind != Other {
		t.Errorf("Kind = %s, want %s", got.Kind, Other)
	}
	if !strings.Contains(got.Description, "unreadable") {
		t.Errorf("Description = %q, want it to say the body could not be read", got.Description)
	}
}

func TestPolicyString(t *testing.T) {
	if KeepEntry.String() != "keep-entry" || CoreImage.String() != "core-image" {
		t.Errorf("Policy strings: %s / %s", KeepEntry, CoreImage)
	}
}

// TestRealDumpsClassify is the other half of TestRealDumps: the same
// files, asked the question `core install` asks. Apple's dump must
// classify as Other (or `core install` would refuse to run on a stock
// iPod), and a dump of this project's own device must not.
func TestRealDumpsClassify(t *testing.T) {
	list := os.Getenv("CORE_FWPART_DUMP")
	if list == "" {
		t.Skip("CORE_FWPART_DUMP not set; no partition dump to classify")
	}
	for _, path := range splitList(list) {
		t.Run(path, func(t *testing.T) {
			f, err := os.Open(path)
			if err != nil {
				t.Skipf("open %s: %v", path, err)
			}
			defer f.Close()
			st, err := f.Stat()
			if err != nil {
				t.Fatalf("stat: %v", err)
			}
			p := Partition{R: f, Size: st.Size()}
			got, osos, err := ClassifyPartition(p)
			if err != nil {
				t.Fatalf("ClassifyPartition: %v", err)
			}
			t.Logf("%s: %s — %s (OSOS len %d, entry %#x)",
				path, got.Kind, got.Description, osos.Length, osos.EntryOffset)

			switch {
			case osos.Length == appleOSOSLength:
				if got.Kind != Other {
					t.Errorf("Apple's firmware classified as %s, want %s", got.Kind, Other)
				}
				if !strings.Contains(got.Description, "Apple firmware") {
					t.Errorf("description of Apple's firmware = %q", got.Description)
				}
			case osos.EntryOffset == 0 && osos.Length < CoreMaxImage:
				if !got.IsCore() {
					t.Errorf("a Core-shaped image classified as %s (%s)",
						got.Kind, got.Description)
				}
			default:
				t.Logf("neither shape; classified %s", got.Kind)
			}
		})
	}
}
