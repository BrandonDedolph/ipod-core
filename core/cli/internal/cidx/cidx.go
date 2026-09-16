// Package cidx encodes and decodes CORELIB.IDX, the device library index.
//
// The iPod's FAT volume is READ-ONLY to the firmware, so the index cannot be
// built on-device: the host reads every track's tags and emits ONE binary file
// the firmware loads in a single read. The device falls back to a per-file tag
// scan only if this file is absent or fails validation.
//
// Layout (little-endian), the same one tools/build_index.py writes and
// core/library/idx.c validates:
//
//	header v2: magic "CIDX"(4), u16 version=2, u16 rec_size=256, u32 count,
//	           u32 crc32 — zlib CRC-32 over the `count` records that follow
//	record (256 B): u32 duration_s, u16 track, u16 disc,
//	                folder[64], file[64], title[48], artist[40], genre[24],
//	                u32 folder_hash, u32 file_hash
//
// v1 (a 12-byte header with no CRC, same records) is still accepted, because a
// device may carry an index built before the CRC existed; it is never written.
package cidx

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/library"
)

// Sizes and versions, mirroring core/library/idx.h.
const (
	RecordSize  = 256
	HeaderV1    = 12
	HeaderV2    = 16
	Version     = 2
	FolderField = 64
	FileField   = 64
	TitleField  = 48
	ArtistField = 40
	GenreField  = 24
)

// Magic is the file's first four bytes.
var Magic = [4]byte{'C', 'I', 'D', 'X'}

// Record is one song, as the firmware reads it. Folder/File/Title/Artist/Genre
// are full strings here; Encode is what truncates them to their field widths.
type Record struct {
	DurationS   uint32
	Track, Disc uint16
	Folder      string // "Artist - Album" for DISPLAY
	File        string // "NN. Title.flac" / "NN. Title.mp3" — the locator
	Title       string
	Artist      string
	Genre       string
	FolderHash  uint32 // NameHash of the FAT-safe folder name
	FileHash    uint32 // NameHash of File
}

// Encode writes the whole file: a v2 header followed by the records. The CRC
// covers the records only — the header fields it would otherwise have to skip
// are validated by the firmware individually (magic, version, rec_size, and
// count against the file size), so nothing in the file is unchecked.
func Encode(recs []Record) []byte {
	out := make([]byte, HeaderV2+RecordSize*len(recs))
	body := out[HeaderV2:]
	for i, r := range recs {
		encodeRecord(body[i*RecordSize:(i+1)*RecordSize], r)
	}
	copy(out[0:4], Magic[:])
	binary.LittleEndian.PutUint16(out[4:6], Version)
	binary.LittleEndian.PutUint16(out[6:8], RecordSize)
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(recs)))
	binary.LittleEndian.PutUint32(out[12:16], crc32.ChecksumIEEE(body))
	return out
}

func encodeRecord(dst []byte, r Record) {
	binary.LittleEndian.PutUint32(dst[0:4], r.DurationS)
	binary.LittleEndian.PutUint16(dst[4:6], r.Track)
	binary.LittleEndian.PutUint16(dst[6:8], r.Disc)
	off := 8
	for _, f := range []struct {
		s string
		n int
	}{
		{r.Folder, FolderField},
		{r.File, FileField},
		{r.Title, TitleField},
		{r.Artist, ArtistField},
		{r.Genre, GenreField},
	} {
		copy(dst[off:off+f.n], library.UTF8Field(f.s, f.n))
		off += f.n
	}
	binary.LittleEndian.PutUint32(dst[248:252], r.FolderHash)
	binary.LittleEndian.PutUint32(dst[252:256], r.FileHash)
}

// Decode validates a CORELIB.IDX exactly the way the firmware's
// library_load_index does — magic, a version it was written for, the record
// size, the file size against the count, and (v2) the CRC over the records —
// and returns the records. Anything it rejects, the device rejects: it falls
// back to the slow tag scan, or, if the file merely truncated, loads nothing.
func Decode(b []byte) ([]Record, error) {
	if len(b) < HeaderV1 {
		return nil, fmt.Errorf("cidx: %d bytes is shorter than the %d-byte header", len(b), HeaderV1)
	}
	if b[0] != 'C' || b[1] != 'I' || b[2] != 'D' || b[3] != 'X' {
		return nil, fmt.Errorf("cidx: not a CIDX file (magic %q)", b[0:4])
	}
	ver := binary.LittleEndian.Uint16(b[4:6])
	rec := binary.LittleEndian.Uint16(b[6:8])
	if ver != 1 && ver != 2 {
		return nil, fmt.Errorf("cidx: version %d is not one this reader knows (1 or 2)", ver)
	}
	if rec != RecordSize {
		return nil, fmt.Errorf("cidx: record size %d, want %d", rec, RecordSize)
	}
	hdrLen := HeaderV1
	if ver == 2 {
		hdrLen = HeaderV2
		if len(b) < HeaderV2 {
			return nil, fmt.Errorf("cidx: v2 header is %d bytes, file has %d", HeaderV2, len(b))
		}
	}
	count := binary.LittleEndian.Uint32(b[8:12])
	if count > (0xFFFFFFFF-HeaderV2)/RecordSize {
		return nil, fmt.Errorf("cidx: count %d cannot be a real record count", count)
	}
	want := uint64(hdrLen) + uint64(count)*RecordSize
	if uint64(len(b)) != want {
		return nil, fmt.Errorf("cidx: file is %d bytes, header says %d (%d records) — truncated or padded", len(b), want, count)
	}
	body := b[hdrLen:]
	if ver == 2 {
		if got, hdr := crc32.ChecksumIEEE(body), binary.LittleEndian.Uint32(b[12:16]); got != hdr {
			return nil, fmt.Errorf("cidx: records CRC-32 %08X, header says %08X", got, hdr)
		}
	}
	recs := make([]Record, count)
	for i := range recs {
		recs[i] = decodeRecord(body[i*RecordSize:])
	}
	return recs, nil
}

func decodeRecord(b []byte) Record {
	return Record{
		DurationS:  binary.LittleEndian.Uint32(b[0:4]),
		Track:      binary.LittleEndian.Uint16(b[4:6]),
		Disc:       binary.LittleEndian.Uint16(b[6:8]),
		Folder:     field(b[8:72]),
		File:       field(b[72:136]),
		Title:      field(b[136:184]),
		Artist:     field(b[184:224]),
		Genre:      field(b[224:248]),
		FolderHash: binary.LittleEndian.Uint32(b[248:252]),
		FileHash:   binary.LittleEndian.Uint32(b[252:256]),
	}
}

// field reads one NUL-terminated record string, the way the firmware's
// field_copy does: everything up to the first NUL.
func field(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}

// RecordsFromScan turns a scanned source tree into records, in the order the
// scan put them: albums in source order, tracks in (disc, track, position)
// order within each album. That order is the device's playing order.
func RecordsFromScan(s *library.Scan) []Record {
	if s == nil {
		return nil
	}
	recs := make([]Record, 0, s.SongCount())
	for i := range s.Albums {
		a := &s.Albums[i]
		folderHash := library.NameHash(a.DeviceFolder)
		for _, t := range a.Tracks {
			recs = append(recs, Record{
				DurationS:  t.DurationS,
				Track:      uint16(t.Track),
				Disc:       uint16(t.Disc),
				Folder:     a.DisplayFolder,
				File:       t.DeviceName,
				Title:      t.Title,
				Artist:     t.Artist,
				Genre:      t.Genre,
				FolderHash: folderHash,
				FileHash:   library.NameHash(t.DeviceName),
			})
		}
	}
	return recs
}
