package cidx

import (
	"bytes"
	"encoding/binary"
	"hash/crc32"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

func sample() []Record {
	return []Record{
		{DurationS: 231, Track: 1, Disc: 1, Folder: "Justin Bieber - Changes",
			File: "01. Intentions.flac", Title: "Intentions", Artist: "Justin Bieber",
			Genre: "Pop", FolderHash: library.NameHash("Justin Bieber - Changes"),
			FileHash: library.NameHash("01. Intentions.flac")},
		{DurationS: 0, Track: 65535, Disc: 3, Folder: "A - B", File: "02. X.flac",
			Title: "X", Artist: "A", Genre: ""},
	}
}

func TestEncodeHeaderAndLayout(t *testing.T) {
	b := Encode(sample())
	if len(b) != HeaderV2+2*RecordSize {
		t.Fatalf("file is %d bytes, want %d", len(b), HeaderV2+2*RecordSize)
	}
	if string(b[0:4]) != "CIDX" {
		t.Errorf("magic = %q", b[0:4])
	}
	if got := binary.LittleEndian.Uint16(b[4:6]); got != 2 {
		t.Errorf("version = %d, want 2", got)
	}
	if got := binary.LittleEndian.Uint16(b[6:8]); got != 256 {
		t.Errorf("record size = %d, want 256", got)
	}
	if got := binary.LittleEndian.Uint32(b[8:12]); got != 2 {
		t.Errorf("count = %d, want 2", got)
	}
	// The CRC covers the records only; the header's own fields are validated
	// individually by the firmware.
	if got, want := binary.LittleEndian.Uint32(b[12:16]), crc32.ChecksumIEEE(b[16:]); got != want {
		t.Errorf("CRC = %08X, want zlib's %08X over the records", got, want)
	}
	// Field offsets, as core/kernel/main.c reads them.
	rec := b[16:272]
	if got := string(bytes.TrimRight(rec[8:72], "\x00")); got != "Justin Bieber - Changes" {
		t.Errorf("folder field = %q", got)
	}
	if got := string(bytes.TrimRight(rec[72:136], "\x00")); got != "01. Intentions.flac" {
		t.Errorf("file field = %q", got)
	}
	if got := binary.LittleEndian.Uint32(rec[248:252]); got != library.NameHash("Justin Bieber - Changes") {
		t.Errorf("folder hash = %08X", got)
	}
}

func TestEncodeDecodeRoundTrip(t *testing.T) {
	in := sample()
	out, err := Decode(Encode(in))
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != len(in) {
		t.Fatalf("decoded %d records, want %d", len(out), len(in))
	}
	for i := range in {
		if out[i] != in[i] {
			t.Errorf("record %d:\n got %+v\nwant %+v", i, out[i], in[i])
		}
	}
}

func TestEncodeTruncatesFieldsOnRuneBoundaries(t *testing.T) {
	// 48-byte title field: 47 usable bytes, and a rune that straddles the cut
	// is dropped whole rather than written as a broken half.
	title := strings.Repeat("a", 46) + "\u00e9" + "tail"
	out, err := Decode(Encode([]Record{{Title: title, Genre: strings.Repeat("g", 40)}}))
	if err != nil {
		t.Fatal(err)
	}
	if got := out[0].Title; got != strings.Repeat("a", 46) {
		t.Errorf("title = %q (%d bytes)", got, len(got))
	}
	if got := out[0].Genre; got != strings.Repeat("g", 23) {
		t.Errorf("genre = %q (%d bytes), want 23", got, len(got))
	}
}

func TestDecodeEmptyIndex(t *testing.T) {
	recs, err := Decode(Encode(nil))
	if err != nil || len(recs) != 0 {
		t.Fatalf("empty index: %v, %d records", err, len(recs))
	}
}

// TestDecodeRejectsWhatTheFirmwareRejects: every check here is one the device
// makes in idx_header_parse/library_load_index. A file that gets past them but
// is wrong is parsed field by field as plausible garbage — durations, titles
// and hashes from wherever the offsets happened to land.
func TestDecodeRejectsWhatTheFirmwareRejects(t *testing.T) {
	good := Encode(sample())

	mutate := func(f func(b []byte)) []byte {
		b := append([]byte(nil), good...)
		f(b)
		return b
	}
	cases := []struct {
		name string
		data []byte
		want string
	}{
		{"short file", good[:8], "shorter than"},
		{"wrong magic", mutate(func(b []byte) { b[0] = 'X' }), "not a CIDX"},
		{"unknown version", mutate(func(b []byte) { binary.LittleEndian.PutUint16(b[4:6], 3) }), "version 3"},
		{"wrong record size", mutate(func(b []byte) { binary.LittleEndian.PutUint16(b[6:8], 128) }), "record size 128"},
		{"count disagrees with the size", mutate(func(b []byte) { binary.LittleEndian.PutUint32(b[8:12], 3) }), "truncated or padded"},
		{"truncated body", good[:len(good)-1], "truncated or padded"},
		{"records do not match the CRC", mutate(func(b []byte) { b[20] ^= 0xFF }), "CRC-32"},
		{"absurd count", mutate(func(b []byte) { binary.LittleEndian.PutUint32(b[8:12], 0xFFFFFFF0) }), "cannot be a real record count"},
	}
	for _, c := range cases {
		_, err := Decode(c.data)
		if err == nil {
			t.Errorf("%s: accepted", c.name)
			continue
		}
		if !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: error %q, want it to mention %q", c.name, err, c.want)
		}
	}
}

// TestDecodeAcceptsV1: a device may still carry an index built before the CRC
// existed. The reader accepts it on the size check alone, exactly like the
// firmware; the writer never produces one.
func TestDecodeAcceptsV1(t *testing.T) {
	v2 := Encode(sample())
	v1 := append([]byte(nil), v2[:12]...)
	binary.LittleEndian.PutUint16(v1[4:6], 1)
	v1 = append(v1, v2[16:]...)
	recs, err := Decode(v1)
	if err != nil {
		t.Fatalf("v1 rejected: %v", err)
	}
	if len(recs) != 2 || recs[0].Title != "Intentions" {
		t.Errorf("v1 decode = %+v", recs)
	}
}

func TestRecordsFromScan(t *testing.T) {
	s := &library.Scan{Albums: []library.Album{{
		DeviceFolder:  "The Kid LAROI - F_CK LOVE 3+_ OVER YOU",
		DisplayFolder: "The Kid LAROI - F*CK LOVE 3+: OVER YOU",
		Tracks: []library.Track{{
			DeviceName: "01. Stay.flac", Title: "Stay", Artist: "The Kid LAROI",
			Genre: "Hip-Hop", Disc: 1, Track: 1, DurationS: 141,
		}},
	}}}
	recs := RecordsFromScan(s)
	if len(recs) != 1 {
		t.Fatalf("got %d records", len(recs))
	}
	r := recs[0]
	// The record DISPLAYS the real album name and LOCATES by the FAT-safe one.
	if r.Folder != "The Kid LAROI - F*CK LOVE 3+: OVER YOU" {
		t.Errorf("folder = %q", r.Folder)
	}
	if r.FolderHash != library.NameHash("The Kid LAROI - F_CK LOVE 3+_ OVER YOU") {
		t.Error("folder hash is not over the FAT-safe name")
	}
	if r.FileHash != library.NameHash("01. Stay.flac") {
		t.Error("file hash is not over the device filename")
	}
	if RecordsFromScan(nil) != nil {
		t.Error("a nil scan should produce no records")
	}
}
