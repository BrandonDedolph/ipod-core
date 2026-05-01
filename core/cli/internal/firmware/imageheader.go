package firmware

import (
	"encoding/binary"
	"errors"
	"io"
)

// DirectoryEntry is one row of the iPod firmware-partition image
// directory — 40 bytes. See core/docs/hw/08-boot-dock.md for the full
// layout.
type DirectoryEntry struct {
	ContainerID [4]byte // "!ATA" or "DNAN"
	ImageType   [4]byte // "soso" (OSOS), "crsr" (RSRC), etc. — stored byte-reversed of the logical name
	ImageID     uint32
	DevOffset   uint32 // bytes from start of firmware partition
	Length      uint32 // image length in bytes
	LoadAddr    uint32 // DRAM load address
	EntryOffset uint32 // entry point within the image (0 = start)
	Checksum    uint32 // additive checksum (model + sum-of-bytes)
	Version     uint32
	LoadAddr2   uint32 // secondary load address
}

// LogicalImageType returns the byte-reversed image-type tag.
//
// On disk the four bytes spell e.g. "soso"; reversed, "osos" (which
// Apple/iPodLinux docs conventionally write uppercase as "OSOS"). The
// returned string is lowercase to match the on-disk encoding exactly.
func (e *DirectoryEntry) LogicalImageType() string {
	return string([]byte{e.ImageType[3], e.ImageType[2], e.ImageType[1], e.ImageType[0]})
}

// IsOSOS reports whether this entry is the main OS image — the one
// our bootloader takes over.
func (e *DirectoryEntry) IsOSOS() bool {
	// "soso" reversed = "OSOS"
	return e.ImageType == [4]byte{'s', 'o', 's', 'o'}
}

// ReadDirectoryEntry decodes one 40-byte entry from r.
//
// Multi-byte fields are little-endian per the iPod boot ROM expectation.
func ReadDirectoryEntry(r io.Reader) (DirectoryEntry, error) {
	var raw [40]byte
	if _, err := io.ReadFull(r, raw[:]); err != nil {
		return DirectoryEntry{}, err
	}

	var e DirectoryEntry
	copy(e.ContainerID[:], raw[0:4])
	copy(e.ImageType[:], raw[4:8])
	e.ImageID = binary.LittleEndian.Uint32(raw[8:12])
	e.DevOffset = binary.LittleEndian.Uint32(raw[12:16])
	e.Length = binary.LittleEndian.Uint32(raw[16:20])
	e.LoadAddr = binary.LittleEndian.Uint32(raw[20:24])
	e.EntryOffset = binary.LittleEndian.Uint32(raw[24:28])
	e.Checksum = binary.LittleEndian.Uint32(raw[28:32])
	e.Version = binary.LittleEndian.Uint32(raw[32:36])
	e.LoadAddr2 = binary.LittleEndian.Uint32(raw[36:40])
	return e, nil
}

// WriteDirectoryEntry encodes e into w.
func WriteDirectoryEntry(w io.Writer, e DirectoryEntry) error {
	var raw [40]byte
	copy(raw[0:4], e.ContainerID[:])
	copy(raw[4:8], e.ImageType[:])
	binary.LittleEndian.PutUint32(raw[8:12], e.ImageID)
	binary.LittleEndian.PutUint32(raw[12:16], e.DevOffset)
	binary.LittleEndian.PutUint32(raw[16:20], e.Length)
	binary.LittleEndian.PutUint32(raw[20:24], e.LoadAddr)
	binary.LittleEndian.PutUint32(raw[24:28], e.EntryOffset)
	binary.LittleEndian.PutUint32(raw[28:32], e.Checksum)
	binary.LittleEndian.PutUint32(raw[32:36], e.Version)
	binary.LittleEndian.PutUint32(raw[36:40], e.LoadAddr2)
	_, err := w.Write(raw[:])
	return err
}

// DirectoryMarker is the magic bytes that mark the start of the image
// directory (at byte offset 0x100 of the firmware partition). It's the
// reversed ASCII of "[hi]".
var DirectoryMarker = [4]byte{']', 'i', 'h', '['}

// ErrBadDirectoryMarker is returned when the magic bytes don't match.
var ErrBadDirectoryMarker = errors.New("firmware partition: directory marker mismatch")

// CheckDirectoryMarker verifies the 4 bytes are the expected marker.
// Returns ErrBadDirectoryMarker if not.
func CheckDirectoryMarker(b [4]byte) error {
	if b != DirectoryMarker {
		return ErrBadDirectoryMarker
	}
	return nil
}
