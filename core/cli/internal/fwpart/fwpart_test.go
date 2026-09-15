package fwpart

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"os"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
)

// --- a synthetic partition built from the measured device facts ------
//
// The real partition is 131,475,456 bytes and the last image alone is
// 67 MB, so the test cannot hold one. sparse below claims the real size
// while storing only the bytes that were actually written; everything
// else reads back as zero, which is what the unwritten parts of a
// firmware partition look like anyway.

// sparse is an io.ReaderAt over a set of written extents, zero
// elsewhere, with a declared size much larger than what it stores.
type sparse struct {
	size    int64
	extents []Write // kept sorted by Off, non-overlapping
}

func (s *sparse) put(off int64, data []byte) {
	if off < 0 || off+int64(len(data)) > s.size {
		panic(fmt.Sprintf("sparse: write of %d at %#x past size %d", len(data), off, s.size))
	}
	// Merge with (and overwrite) any extents this write touches, so an
	// in-place patch of a stored body behaves like a real device
	// rather than leaving two overlapping copies.
	lo, hi := off, off+int64(len(data))
	for _, e := range s.extents {
		if e.Off < hi && e.End() > lo {
			lo, hi = min64(lo, e.Off), max64(hi, e.End())
		}
	}
	buf := make([]byte, hi-lo)
	if _, err := s.ReadAt(buf, lo); err != nil && err != io.EOF {
		panic(err)
	}
	copy(buf[off-lo:], data)

	kept := make([]Write, 0, len(s.extents)+1)
	for _, e := range s.extents {
		if e.Off >= lo && e.End() <= hi {
			continue
		}
		kept = append(kept, e)
	}
	ins := len(kept)
	for i := range kept {
		if kept[i].Off > lo {
			ins = i
			break
		}
	}
	kept = append(kept, Write{})
	copy(kept[ins+1:], kept[ins:])
	kept[ins] = Write{Off: lo, Data: buf}
	s.extents = kept
}

// apply performs a planned write against the stored bytes.
func (s *sparse) apply(w Write) { s.put(w.Off, w.Data) }

func (s *sparse) clone() *sparse {
	c := &sparse{size: s.size}
	for _, e := range s.extents {
		c.extents = append(c.extents, Write{Off: e.Off, Data: append([]byte(nil), e.Data...)})
	}
	return c
}

func (s *sparse) ReadAt(p []byte, off int64) (int, error) {
	if off < 0 {
		return 0, errors.New("sparse: negative offset")
	}
	if off >= s.size {
		return 0, io.EOF
	}
	n := len(p)
	if int64(n) > s.size-off {
		n = int(s.size - off)
	}
	for i := range p[:n] {
		p[i] = 0
	}
	for _, e := range s.extents {
		lo, hi := max64(e.Off, off), min64(e.End(), off+int64(n))
		if lo < hi {
			copy(p[lo-off:hi-off], e.Data[lo-e.Off:hi-e.Off])
		}
	}
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}

func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

func min64(a, b int64) int64 {
	if a < b {
		return a
	}
	return b
}

// Measured facts (core/docs/design/companion-app-plan.md §0, confirmed
// against both ipodpatcher dumps).
const (
	realPartitionSize = 131475456
	dirStart          = 0x4200
	ososDevOffset     = 0x4800
	ososBodyOffset    = 0x5000
	ososLength        = 237640
	ososCapacity      = 7618560 // 0x749000 - 0x5000
	rsrcDevOffset     = 0x749000
	aupdDevOffset     = 0xC4A000
	hibeDevOffset     = 0xD51800
)

func tag(s string) [4]byte {
	var b [4]byte
	copy(b[:], s)
	return b
}

// preamble builds the first 512 bytes: the STOP sign, the copyright
// banner, the "]ih[" marker and the LE32 0x4000 pointer that puts the
// directory at 0x4200, the LE16 version.
func preamble(version uint16, dirPtr uint32) []byte {
	head := make([]byte, 512)
	art := []string{
		"{{~~  /-----\\   ",
		"{{~~ /       \\  ",
		"{{~~|         | ",
		"{{~~| S T O P | ",
		"{{~~|         | ",
		"{{~~ \\       /  ",
		"{{~~  \\-----/   ",
	}
	off := 0
	for _, line := range art {
		off += copy(head[off:], line)
	}
	off += copy(head[off:], "Copyright(C) 2001 Apple Computer, Inc.")
	for i := off; i < firmware.DirectoryMarkerOffset-1; i++ {
		head[i] = '-'
	}
	copy(head[firmware.DirectoryMarkerOffset:], firmware.DirectoryMarker[:])
	head[0x104] = byte(dirPtr)
	head[0x105] = byte(dirPtr >> 8)
	head[0x106] = byte(dirPtr >> 16)
	head[0x107] = byte(dirPtr >> 24)
	head[0x10A] = byte(version)
	head[0x10B] = byte(version >> 8)
	return head
}

// fixture is a synthetic partition plus the bodies it was built from.
type fixture struct {
	part   Partition
	sp     *sparse
	bodies map[string][]byte
}

func randBytes(seed int64, n int) []byte {
	b := make([]byte, n)
	rand.New(rand.NewSource(seed)).Read(b)
	return b
}

// buildFixture assembles a partition with the four real entries, real
// offsets and lengths for OSOS, and shorter-but-plausible bodies for
// the three images we never touch (their real lengths add up to 74 MB
// and nothing in this test needs them to be full size — the directory
// still carries the real DevOffsets, which is what capacity depends
// on).
func buildFixture(t *testing.T) *fixture { return buildFixtureRSRC(t, rsrcDevOffset) }

// buildFixtureRSRC lets a test move the image after OSOS, which is what
// sets the OSOS capacity.
func buildFixtureRSRC(t *testing.T, rsrcOff uint32) *fixture {
	t.Helper()
	f := &fixture{
		sp:     &sparse{size: realPartitionSize},
		bodies: map[string][]byte{},
	}
	f.sp.put(0, preamble(3, 0x4000))

	type spec struct {
		typ            string
		id             uint32
		devOffset      uint32
		length         int
		addr, ent      uint32
		vers, loadAdr2 uint32
	}
	specs := []spec{
		{"soso", 0, ososDevOffset, ososLength, 0x10000000, 0, 0xB012, 0xFFFFFFFF},
		{"crsr", 0, rsrcOff, 64 << 10, 0, 0x600, 0xB012, 0xFFFFFFFF},
		{"dpua", 1, aupdDevOffset, 32 << 10, 0x10000000, 0, 0xB012, 0xFFFFFFFF},
		{"ebih", 1, hibeDevOffset, 16 << 10, 0x10000000, 0, 0, 0xFFFFFFFF},
	}

	var dir bytes.Buffer
	for i, s := range specs {
		body := randBytes(int64(i)+1, s.length)
		f.bodies[s.typ] = body
		f.sp.put(int64(s.devOffset)+BodyBias, body)
		e := firmware.DirectoryEntry{
			ContainerID: tag("!ATA"),
			ImageType:   tag(s.typ),
			ImageID:     s.id,
			DevOffset:   s.devOffset,
			Length:      uint32(s.length),
			LoadAddr:    s.addr,
			EntryOffset: s.ent,
			Checksum:    ImageChecksum(body),
			Version:     s.vers,
			LoadAddr2:   s.loadAdr2,
		}
		if err := firmware.WriteDirectoryEntry(&dir, e); err != nil {
			t.Fatalf("encode entry %d: %v", i, err)
		}
	}
	// The row that ends the list. On both real dumps it is zero except
	// for LoadAddr2, which reads 0xFFFFFFFF — mirror that here so the
	// terminator rule is tested against the shape the device has, not
	// the tidier one the plan described.
	end := make([]byte, EntrySize)
	copy(end[36:], []byte{0xFF, 0xFF, 0xFF, 0xFF})
	dir.Write(end)
	f.sp.put(dirStart, dir.Bytes())
	f.part = Partition{R: f.sp, Size: realPartitionSize}
	return f
}

func TestParseSyntheticPartition(t *testing.T) {
	f := buildFixture(t)
	d, err := Parse(f.part)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if d.Version != 3 {
		t.Errorf("Version = %d, want 3", d.Version)
	}
	if d.Start != dirStart {
		t.Errorf("Start = %#x, want %#x", d.Start, dirStart)
	}
	if len(d.Entries) != 4 {
		t.Fatalf("got %d entries, want 4", len(d.Entries))
	}
	want := []string{"osos", "rsrc", "aupd", "hibe"}
	for i, w := range want {
		if got := d.Entries[i].LogicalImageType(); got != w {
			t.Errorf("entry %d type = %q, want %q", i, got, w)
		}
	}

	idx, e, ok := d.OSOS()
	if !ok {
		t.Fatal("OSOS not found")
	}
	if idx != 0 {
		t.Errorf("OSOS idx = %d, want 0", idx)
	}
	if BodyOffset(e) != ososBodyOffset {
		t.Errorf("OSOS body at %#x, want %#x", BodyOffset(e), ososBodyOffset)
	}
	if e.Length != ososLength {
		t.Errorf("OSOS Length = %d, want %d", e.Length, ososLength)
	}
	if got := d.Capacity(idx); got != ososCapacity {
		t.Errorf("Capacity(OSOS) = %d, want %d", got, ososCapacity)
	}
	if got := d.EntryOffset(idx); got != dirStart {
		t.Errorf("EntryOffset(0) = %#x, want %#x", got, dirStart)
	}

	// The last entry's capacity runs to the end of the partition.
	last := len(d.Entries) - 1
	wantLast := uint32(realPartitionSize - BodyOffset(d.Entries[last]))
	if got := d.Capacity(last); got != wantLast {
		t.Errorf("Capacity(last) = %d, want %d", got, wantLast)
	}

	for i := range d.Entries {
		if err := VerifyEntry(f.part, d.Entries[i]); err != nil {
			t.Errorf("VerifyEntry(%d): %v", i, err)
		}
	}
	body, err := ReadBody(f.part, e)
	if err != nil {
		t.Fatalf("ReadBody: %v", err)
	}
	if !bytes.Equal(body, f.bodies["soso"]) {
		t.Error("ReadBody returned different bytes than were written")
	}
	if len(body) != ososLength {
		t.Errorf("ReadBody returned %d bytes, want %d", len(body), ososLength)
	}
}

func TestVerifyEntryDetectsCorruption(t *testing.T) {
	f := buildFixture(t)
	d, err := Parse(f.part)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	_, e, _ := d.OSOS()
	e.Checksum++
	if err := VerifyEntry(f.part, e); !errors.Is(err, ErrVerify) {
		t.Fatalf("VerifyEntry with a wrong checksum: %v, want ErrVerify", err)
	}
}

// newImage is a stand-in for core.bin at the size it actually has today
// (v0.1.2, 367,608 bytes).
func newImage() []byte { return randBytes(42, 367608) }

func TestPlanWriteOSOS(t *testing.T) {
	f := buildFixture(t)
	d, err := Parse(f.part)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	idx, old, _ := d.OSOS()
	image := newImage()

	for _, tc := range []struct {
		sectorSize int
		wantOff    int64
	}{
		{2048, 0x4000}, // what the USB bridge reports
		{512, 0x4200},  // what an ATA bridge or an image file gives
	} {
		t.Run(fmt.Sprintf("sector%d", tc.sectorSize), func(t *testing.T) {
			writes, e, err := PlanWrite(d, idx, image, tc.sectorSize)
			if err != nil {
				t.Fatalf("PlanWrite: %v", err)
			}
			if len(writes) != 2 {
				t.Fatalf("got %d writes, want 2", len(writes))
			}

			// Write 1: the body, zero-padded up to 0x800.
			if writes[0].Off != ososBodyOffset {
				t.Errorf("body write at %#x, want %#x", writes[0].Off, ososBodyOffset)
			}
			if len(writes[0].Data) != 368640 {
				t.Errorf("body write is %d bytes, want 368640", len(writes[0].Data))
			}
			if len(writes[0].Data)%BodyAlign != 0 {
				t.Errorf("body write of %d is not 0x800-aligned", len(writes[0].Data))
			}
			if !bytes.Equal(writes[0].Data[:len(image)], image) {
				t.Error("body write does not start with the image")
			}
			for i, b := range writes[0].Data[len(image):] {
				if b != 0 {
					t.Fatalf("padding byte %d is %#x, want 0", i, b)
				}
			}

			// Write 2: exactly one sector, holding the entry.
			if writes[1].Off != tc.wantOff {
				t.Errorf("directory write at %#x, want %#x", writes[1].Off, tc.wantOff)
			}
			if len(writes[1].Data) != tc.sectorSize {
				t.Errorf("directory write is %d bytes, want %d", len(writes[1].Data), tc.sectorSize)
			}

			// The re-encoded entry: Length and Checksum change, nothing else.
			within := int(int64(dirStart) - writes[1].Off)
			got, err := firmware.ReadDirectoryEntry(
				bytes.NewReader(writes[1].Data[within : within+EntrySize]))
			if err != nil {
				t.Fatalf("decode written entry: %v", err)
			}
			if got != e {
				t.Errorf("entry in the sector = %+v, PlanWrite returned %+v", got, e)
			}
			if got.Length != 367608 {
				t.Errorf("Length = %d, want 367608", got.Length)
			}
			if want := ImageChecksum(image); got.Checksum != want {
				t.Errorf("Checksum = %#08x, want the plain sum %#08x", got.Checksum, want)
			}
			if got.Checksum == firmware.Checksum(firmware.ModelIPodVideo, image) {
				t.Error("Checksum was seeded with the model number; the partition entry has no seed")
			}
			if got.ContainerID != old.ContainerID || got.ImageType != old.ImageType ||
				got.ImageID != old.ImageID || got.DevOffset != old.DevOffset ||
				got.LoadAddr != old.LoadAddr || got.EntryOffset != old.EntryOffset ||
				got.Version != old.Version || got.LoadAddr2 != old.LoadAddr2 {
				t.Errorf("a field other than Length/Checksum changed:\n old %+v\n new %+v", old, got)
			}

			// Every other byte of that sector is the old content.
			before, err := readAt(f.part, writes[1].Off, int64(tc.sectorSize))
			if err != nil {
				t.Fatalf("read sector: %v", err)
			}
			for i := range before {
				if i >= within && i < within+EntrySize {
					continue
				}
				if before[i] != writes[1].Data[i] {
					t.Fatalf("byte %d of the sector changed (%#x -> %#x); only the entry may change",
						i, before[i], writes[1].Data[i])
				}
			}
			// In particular the neighbouring RSRC row is untouched.
			if tc.sectorSize == 2048 {
				rsrcAt := within + EntrySize
				want, err := readAt(f.part, int64(dirStart+EntrySize), EntrySize)
				if err != nil {
					t.Fatalf("read RSRC row: %v", err)
				}
				if !bytes.Equal(writes[1].Data[rsrcAt:rsrcAt+EntrySize], want) {
					t.Error("the RSRC row in the sector was modified")
				}
			}
		})
	}
}

func TestPlanWriteRefusesOversizeImage(t *testing.T) {
	f := buildFixture(t)
	d, err := Parse(f.part)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	idx, _, _ := d.OSOS()

	if _, _, err := PlanWrite(d, idx, make([]byte, ososCapacity+1), 2048); !errors.Is(err, ErrImageTooLarge) {
		t.Errorf("PlanWrite(capacity+1) = %v, want ErrImageTooLarge", err)
	}
	// Exactly capacity is fine here: 7,618,560 is a multiple of 0x800,
	// so the zero padding adds nothing.
	if _, _, err := PlanWrite(d, idx, make([]byte, ososCapacity), 2048); err != nil {
		t.Errorf("PlanWrite(capacity) = %v, want success", err)
	}
	// If the gap were not 0x800-aligned, an image that fits would still
	// be refused when its padding would run into the next image — the
	// padded write is what reaches the disk, not the image.
	odd := buildFixtureRSRC(t, rsrcDevOffset+0x100)
	od, err := Parse(odd.part)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	oi, _, _ := od.OSOS()
	if got := od.Capacity(oi); got != ososCapacity+0x100 {
		t.Fatalf("capacity = %d, want %d", got, ososCapacity+0x100)
	}
	if _, _, err := PlanWrite(od, oi, make([]byte, ososCapacity+0x100), 2048); !errors.Is(err, ErrImageTooLarge) {
		t.Errorf("PlanWrite(unaligned capacity) = %v, want ErrImageTooLarge", err)
	}
	if _, _, err := PlanWrite(d, idx, nil, 2048); err == nil {
		t.Error("PlanWrite with an empty image succeeded")
	}
	for _, s := range []int{0, -512, 1000} {
		if _, _, err := PlanWrite(d, idx, newImage(), s); !errors.Is(err, ErrBadSectorSize) {
			t.Errorf("PlanWrite(sectorSize=%d) = %v, want ErrBadSectorSize", s, err)
		}
	}
	if _, _, err := PlanWrite(d, 99, newImage(), 2048); err == nil {
		t.Error("PlanWrite with an out-of-range index succeeded")
	}
}

func TestVerifyWrittenRoundTrip(t *testing.T) {
	f := buildFixture(t)
	d, err := Parse(f.part)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	idx, _, _ := d.OSOS()
	image := newImage()
	writes, e, err := PlanWrite(d, idx, image, 2048)
	if err != nil {
		t.Fatalf("PlanWrite: %v", err)
	}

	applied := f.sp.clone()
	for _, w := range writes {
		applied.apply(w)
	}
	after := Partition{R: applied, Size: realPartitionSize}
	if err := VerifyWritten(after, e, image); err != nil {
		t.Fatalf("VerifyWritten after applying the plan: %v", err)
	}
	// The new directory parses and the other three entries still verify.
	d2, err := Parse(after)
	if err != nil {
		t.Fatalf("Parse after write: %v", err)
	}
	if len(d2.Entries) != 4 {
		t.Fatalf("got %d entries after the write, want 4", len(d2.Entries))
	}
	for i := 1; i < len(d2.Entries); i++ {
		if d2.Entries[i] != d.Entries[i] {
			t.Errorf("entry %d changed: %+v -> %+v", i, d.Entries[i], d2.Entries[i])
		}
		if err := VerifyEntry(after, d2.Entries[i]); err != nil {
			t.Errorf("VerifyEntry(%d) after the write: %v", i, err)
		}
	}

	t.Run("flipped body byte", func(t *testing.T) {
		bad := applied.clone()
		one, err := readAt(Partition{R: bad, Size: realPartitionSize}, ososBodyOffset+1234, 1)
		if err != nil {
			t.Fatalf("read: %v", err)
		}
		bad.apply(Write{Off: ososBodyOffset + 1234, Data: []byte{one[0] ^ 0x01}})
		err = VerifyWritten(Partition{R: bad, Size: realPartitionSize}, e, image)
		if !errors.Is(err, ErrVerify) {
			t.Fatalf("VerifyWritten with a flipped body byte = %v, want ErrVerify", err)
		}
	})

	t.Run("entry checksum off by one", func(t *testing.T) {
		bad := applied.clone()
		off := e
		off.Checksum++
		var enc bytes.Buffer
		if err := firmware.WriteDirectoryEntry(&enc, off); err != nil {
			t.Fatalf("encode: %v", err)
		}
		bad.apply(Write{Off: dirStart, Data: enc.Bytes()})
		p := Partition{R: bad, Size: realPartitionSize}
		// Against the entry we believe we wrote: the row on the
		// device no longer matches it.
		if err := VerifyWritten(p, e, image); !errors.Is(err, ErrVerify) {
			t.Fatalf("VerifyWritten against the planned entry = %v, want ErrVerify", err)
		}
		// And against the corrupted entry itself: the body does not
		// sum to the checksum it claims.
		if err := VerifyWritten(p, off, image); !errors.Is(err, ErrVerify) {
			t.Fatalf("VerifyWritten against the corrupted entry = %v, want ErrVerify", err)
		}
		if err := VerifyEntry(p, off); !errors.Is(err, ErrVerify) {
			t.Fatalf("VerifyEntry on the corrupted entry = %v, want ErrVerify", err)
		}
	})
}

func TestCheckPreambleRejects(t *testing.T) {
	good := preamble(3, 0x4000)
	if err := CheckPreamble(good); err != nil {
		t.Fatalf("CheckPreamble on a good head: %v", err)
	}

	cases := []struct {
		name   string
		mangle func([]byte) []byte
	}{
		{"no {{~~", func(b []byte) []byte { copy(b, "ABCD"); return b }},
		{"no copyright", func(b []byte) []byte {
			i := bytes.Index(b, PreambleCopyright)
			copy(b[i:], bytes.Repeat([]byte("-"), len(PreambleCopyright)))
			return b
		}},
		{"no marker", func(b []byte) []byte {
			copy(b[firmware.DirectoryMarkerOffset:], "xxxx")
			return b
		}},
		{"truncated", func(b []byte) []byte { return b[:0x80] }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			head := tc.mangle(append([]byte(nil), good...))
			if err := CheckPreamble(head); !errors.Is(err, ErrNoPreamble) {
				t.Fatalf("CheckPreamble = %v, want ErrNoPreamble", err)
			}
			// And Parse refuses the whole partition.
			sp := &sparse{size: realPartitionSize}
			sp.put(0, append(head, make([]byte, 512-len(head))...)[:512])
			if _, err := Parse(Partition{R: sp, Size: realPartitionSize}); err == nil {
				t.Fatal("Parse accepted a partition with a bad preamble")
			}
		})
	}

	t.Run("version 4", func(t *testing.T) {
		sp := &sparse{size: realPartitionSize}
		sp.put(0, preamble(4, 0x4000))
		_, err := Parse(Partition{R: sp, Size: realPartitionSize})
		if !errors.Is(err, firmware.ErrUnknownDirectoryVersion) {
			t.Fatalf("Parse with directory version 4 = %v, want ErrUnknownDirectoryVersion", err)
		}
	})
}

// --- the real dumps --------------------------------------------------
//
// Gated on CORE_FWPART_DUMP (a colon-separated list of partition dumps,
// or a single path). The dumps are 131 MB of someone's device and are
// never committed; without the variable this skips.

func TestRealDumps(t *testing.T) {
	list := os.Getenv("CORE_FWPART_DUMP")
	if list == "" {
		t.Skip("CORE_FWPART_DUMP not set; no partition dump to check against")
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
			d, err := Parse(p)
			if err != nil {
				t.Fatalf("Parse: %v", err)
			}
			t.Logf("%d bytes, directory v%d at %#x, %d entries",
				st.Size(), d.Version, d.Start, len(d.Entries))
			if d.Version != 2 && d.Version != 3 {
				t.Errorf("directory version %d", d.Version)
			}
			for i, e := range d.Entries {
				sum, err := EntryChecksum(p, e)
				if err != nil {
					t.Errorf("entry %d (%s): %v", i, e.LogicalImageType(), err)
					continue
				}
				state := "OK"
				if sum != e.Checksum {
					state = "BAD"
				}
				t.Logf("  %d %s devOffset=%#x body=%#x len=%d chksum=%#08x recomputed=%#08x %s",
					i, e.LogicalImageType(), e.DevOffset, BodyOffset(e), e.Length,
					e.Checksum, sum, state)
			}

			idx, osos, ok := d.OSOS()
			if !ok {
				t.Fatal("no OSOS entry")
			}
			// The OSOS checksum is the one fact the flash path depends
			// on, so it is a failure, not a log line. dpua/ebih may
			// legitimately mismatch (Apple does not keep them current)
			// and are only reported above.
			if err := VerifyEntry(p, osos); err != nil {
				t.Errorf("OSOS: %v", err)
			}
			if osos.DevOffset != ososDevOffset {
				t.Errorf("OSOS DevOffset = %#x, want %#x", osos.DevOffset, ososDevOffset)
			}
			if BodyOffset(osos) != ososBodyOffset {
				t.Errorf("OSOS body at %#x, want %#x", BodyOffset(osos), ososBodyOffset)
			}
			if got := d.Capacity(idx); got != ososCapacity {
				t.Errorf("OSOS capacity = %d, want %d", got, ososCapacity)
			}
			// Our image and Apple's are the two we have measured.
			switch osos.Length {
			case 237640:
				if osos.Checksum != 0x01589d64 {
					t.Errorf("our OSOS checksum = %#08x, want 0x01589d64", osos.Checksum)
				}
			case 7618128:
				if osos.EntryOffset != 0x736000 {
					t.Errorf("Apple OSOS entryOffset = %#x, want 0x736000", osos.EntryOffset)
				}
			default:
				t.Logf("OSOS length %d is neither of the two measured images", osos.Length)
			}
			if osos.LoadAddr != 0x10000000 {
				t.Errorf("OSOS addr = %#x, want 0x10000000", osos.LoadAddr)
			}
			if osos.Version != 0xB012 || osos.LoadAddr2 != 0xFFFFFFFF {
				t.Errorf("OSOS vers = %#x, la2 = %#x; want 0xb012 / 0xffffffff",
					osos.Version, osos.LoadAddr2)
			}
		})
	}
}

func splitList(s string) []string {
	var out []string
	for _, p := range bytes.Split([]byte(s), []byte{':'}) {
		if len(p) > 0 {
			out = append(out, string(p))
		}
	}
	return out
}
