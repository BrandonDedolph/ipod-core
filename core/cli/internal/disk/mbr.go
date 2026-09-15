package disk

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// MBR layout constants. This is the classic DOS partition table and
// nothing more: no EBR chain, no GPT. An iPod has exactly two primary
// partitions and the boot ROM cannot read anything else, so a disk that
// needs more parsing than this is a disk that is not an iPod.
const (
	// MBRSize is how many bytes of sector 0 the table lives in.
	MBRSize = 512
	// mbrTableOffset is where the four 16-byte entries start.
	mbrTableOffset = 0x1BE
	// mbrEntrySize is one entry.
	mbrEntrySize = 16
	// mbrSignature is the 0x55 0xAA at the end of sector 0.
	mbrSigOffset = 0x1FE
)

// Partition types this project cares about.
const (
	// TypeEmpty (0x00) is what Apple marks the firmware partition. It
	// is NOT an unused slot: it has a real start LBA and a real
	// length, and the "empty" type is exactly what keeps every OS on
	// earth from trying to mount it. A table walker that skips type
	// 0x00 skips the firmware.
	TypeEmpty byte = 0x00
	// TypeFAT32LBA (0x0B) and TypeFAT32CHS (0x0C) are the two shapes
	// the music partition takes ("winpod"). A macOS-formatted iPod
	// would be HFS+ (0xAF) and this project has never seen one.
	TypeFAT32CHS byte = 0x0B
	TypeFAT32LBA byte = 0x0C
)

// ErrNoMBR means sector 0 has no 0x55AA signature.
var ErrNoMBR = errors.New("disk: sector 0 has no MBR signature")

// Partition is one row of the MBR.
//
// StartLBA and NumSectors are in the transport's LOGICAL sectors, not
// in 512-byte units. On the 5.5G over USB that unit is 2048, so
// partition 0 (start 63, 64197 sectors) is byte 129,024 .. 131,604,480
// — the 131,475,456-byte partition the dumps in the bring-up folder
// are. Multiply with ByteStart/ByteLength and a sector size the caller
// has cross-checked, never with a hard-coded 512.
type Partition struct {
	// Index is the slot, 0..3.
	Index int
	// Bootable is the 0x80 status byte.
	Bootable bool
	// Type is the partition type byte.
	Type byte
	// StartLBA is the first sector.
	StartLBA uint32
	// NumSectors is the length in sectors.
	NumSectors uint32
}

// Used reports whether the slot describes an extent at all. An unused
// slot is all zeros; type 0x00 with a nonzero extent is the firmware
// partition, which is very much used.
func (p Partition) Used() bool { return p.StartLBA != 0 || p.NumSectors != 0 }

// EndLBA is the last sector of the partition (inclusive), which is the
// number ipodpatcher prints.
func (p Partition) EndLBA() uint32 {
	if p.NumSectors == 0 {
		return p.StartLBA
	}
	return p.StartLBA + p.NumSectors - 1
}

// ByteStart is the first byte of the partition at this sector size.
func (p Partition) ByteStart(sectorSize int) int64 {
	return int64(p.StartLBA) * int64(sectorSize)
}

// ByteLength is the size of the partition in bytes at this sector size.
func (p Partition) ByteLength(sectorSize int) int64 {
	return int64(p.NumSectors) * int64(sectorSize)
}

// TypeName is the human label, for `core info`.
func (p Partition) TypeName() string {
	switch p.Type {
	case TypeEmpty:
		return "Empty (0x00)"
	case TypeFAT32CHS:
		return "W95 FAT32 (0x0b)"
	case TypeFAT32LBA:
		return "W95 FAT32 LBA (0x0c)"
	case 0x06:
		return "FAT16 (0x06)"
	case 0xAF:
		return "HFS/HFS+ (0xaf)"
	case 0xEE:
		return "GPT protective (0xee)"
	default:
		return fmt.Sprintf("type %#02x", p.Type)
	}
}

// ParseMBR reads the four primary partition entries from sector 0.
//
// It returns all four slots, unused ones included, so the caller can
// say "partition 0" and mean slot 0 — the iPod's firmware partition is
// identified by its slot, and compacting the list would renumber it.
func ParseMBR(sector []byte) ([]Partition, error) {
	if len(sector) < MBRSize {
		return nil, fmt.Errorf("%w: got %d bytes, need %d", ErrNoMBR, len(sector), MBRSize)
	}
	if sector[mbrSigOffset] != 0x55 || sector[mbrSigOffset+1] != 0xAA {
		return nil, fmt.Errorf("%w: bytes at %#x are %#02x %#02x, want 0x55 0xaa",
			ErrNoMBR, mbrSigOffset, sector[mbrSigOffset], sector[mbrSigOffset+1])
	}
	parts := make([]Partition, 4)
	for i := 0; i < 4; i++ {
		e := sector[mbrTableOffset+i*mbrEntrySize:]
		parts[i] = Partition{
			Index:      i,
			Bootable:   e[0] == 0x80,
			Type:       e[4],
			StartLBA:   binary.LittleEndian.Uint32(e[8:12]),
			NumSectors: binary.LittleEndian.Uint32(e[12:16]),
		}
	}
	return parts, nil
}

// BuildMBR is the inverse, used only by tests and by --dry-run fakes.
// Nothing in the write path ever builds a partition table: the one on
// the device is the only copy of it that exists.
func BuildMBR(parts []Partition) []byte {
	sector := make([]byte, MBRSize)
	for i, p := range parts {
		if i >= 4 {
			break
		}
		e := sector[mbrTableOffset+i*mbrEntrySize:]
		if p.Bootable {
			e[0] = 0x80
		}
		e[4] = p.Type
		binary.LittleEndian.PutUint32(e[8:12], p.StartLBA)
		binary.LittleEndian.PutUint32(e[12:16], p.NumSectors)
	}
	sector[mbrSigOffset] = 0x55
	sector[mbrSigOffset+1] = 0xAA
	return sector
}
