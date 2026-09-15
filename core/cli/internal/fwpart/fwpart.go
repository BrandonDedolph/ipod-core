// Firmware-partition model: preamble, image directory, OSOS capacity,
// the write set and the read-back verification.
//
// Everything here is pure: it talks to an io.ReaderAt and returns byte
// ranges to write. Nothing in this package opens a device, and nothing
// here writes — producing the writes and performing them are separate
// jobs on purpose, so the write set can be printed by --dry-run and
// reviewed against the checklist in doc.go before any of it reaches a
// disk.
//
// The measured facts this encodes (5.5G 80 GB, dumps taken 2026-07-26
// and 2026-07-27, see core/docs/hw/08-boot-dock.md):
//
//   - The first 512 bytes are the Apple preamble: "{{~~" at 0 and the
//     "Copyright(C) 2001 Apple Computer, Inc." banner. The boot ROM
//     will not load a partition without it, so it is read and never
//     written.
//   - "]ih[" at 0x100, LE32 at 0x104 + 0x200 = the first directory
//     entry (0x4000 + 0x200 = 0x4200 on this device), LE16 format
//     version at 0x10A (3 here; 2 also exists).
//   - Each directory entry is 40 bytes; the list ends at an all-zero
//     row. Four entries on this device: OSOS, RSRC, AUPD, HIBE.
//   - An image body starts at devOffset + 0x800, NOT +0x200, and the
//     entry's chksum is the plain 32-bit wrapping sum of exactly
//     `len` body bytes with NO model seed. The seed 5 belongs to the
//     .ipod transport header alone (see firmware/ipodfile.go).
//   - ipodpatcher zero-pads the body write up to a 0x800 boundary and
//     leaves every other field of the entry, and every other entry,
//     untouched. So do we.

package fwpart

import (
	"bytes"
	"errors"
	"fmt"
	"io"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
)

const (
	// BodyBias is the distance from an entry's DevOffset to the first
	// byte of its image body. Measured, not guessed: the previous
	// value in the docs (0x200) was wrong.
	BodyBias = 0x800

	// BodyAlign is the boundary a body write is zero-padded up to,
	// matching ipodpatcher.
	BodyAlign = 0x800

	// EntrySize is the size of one directory row.
	EntrySize = 40

	// MaxEntries caps the directory walk. The device has four; a
	// partition claiming hundreds is a partition we do not understand,
	// and walking it would just be inventing entries out of whatever
	// bytes follow.
	MaxEntries = 16

	// PreambleWindow is how far into the partition the copyright
	// banner is looked for.
	PreambleWindow = 512
)

// Preamble byte sequences. Both are Apple's; neither is ever written by
// this package.
var (
	// PreambleMagic starts every line of the "STOP" sign at the top of
	// the partition and is the first four bytes of the partition.
	PreambleMagic = []byte("{{~~")
	// PreambleCopyright appears once inside PreambleWindow.
	PreambleCopyright = []byte("Copyright(C)")
)

// Errors reported by this package. They are sentinels because the flash
// path has to distinguish "this is not an iPod firmware partition"
// (refuse, loudly) from "this image will not fit" (refuse, but the user
// can act on it).
var (
	// ErrNoPreamble means the bytes are not an iPod firmware partition:
	// the Apple banner or the directory marker is missing.
	ErrNoPreamble = errors.New("fwpart: Apple firmware-partition preamble not found")
	// ErrNoOSOS means the directory parsed but has no "soso" entry.
	ErrNoOSOS = errors.New("fwpart: no OSOS entry in the image directory")
	// ErrImageTooLarge means the image does not fit in the space
	// between this entry's body and the next entry.
	ErrImageTooLarge = errors.New("fwpart: image exceeds the capacity of this directory entry")
	// ErrBadSectorSize means the sector size is not a usable power of
	// two, or the entry straddles two sectors at that size.
	ErrBadSectorSize = errors.New("fwpart: unusable sector size")
	// ErrOutOfRange means a read or write would fall outside the
	// partition.
	ErrOutOfRange = errors.New("fwpart: range falls outside the partition")
	// ErrVerify means a read-back did not match what was written.
	ErrVerify = errors.New("fwpart: read-back verification failed")
)

// Partition is a firmware partition to read: the bytes, and how many of
// them there are. Size is the size of partition 0, which is what bounds
// the last image's capacity — it is taken from the partition table (or
// the file length, for a dump), never from the directory.
type Partition struct {
	R    io.ReaderAt
	Size int64
}

// Directory is the parsed image directory.
//
// The plan's sketch of this type carried only the three exported
// fields; Capacity needs the partition size and PlanWrite re-reads the
// entry's sector, so the Partition that Parse read is kept here rather
// than threaded through every call. Parse is the only constructor.
type Directory struct {
	// Version is the LE16 directory format version at 0x10A (2 or 3).
	Version uint16
	// Start is the byte offset of the first 40-byte entry.
	Start uint32
	// Entries are the rows up to, but not including, the zero row.
	Entries []firmware.DirectoryEntry

	p Partition
}

// Partition returns the partition this directory was parsed from.
func (d *Directory) Partition() Partition { return d.p }

// CheckPreamble reports whether head is the start of an iPod firmware
// partition: "{{~~" at 0, "Copyright(C)" somewhere in the first 512
// bytes, "]ih[" at 0x100.
//
// This is check (a)(2) of the write-path checklist in doc.go. It is a
// weaker test than "the preamble is byte-exact" on purpose: the only
// copy of a given device's preamble is the one on that device, so there
// is nothing to compare against. What it does prove is that the bytes
// under the write are laid out the way the boot ROM expects.
func CheckPreamble(head []byte) error {
	const need = firmware.DirectoryMarkerOffset + 4
	if len(head) < need {
		return fmt.Errorf("%w: only %d bytes available, need at least %d",
			ErrNoPreamble, len(head), need)
	}
	if !bytes.HasPrefix(head, PreambleMagic) {
		return fmt.Errorf("%w: first bytes are %#x, want %q",
			ErrNoPreamble, head[:4], PreambleMagic)
	}
	window := head
	if len(window) > PreambleWindow {
		window = window[:PreambleWindow]
	}
	if !bytes.Contains(window, PreambleCopyright) {
		return fmt.Errorf("%w: %q not found in the first %d bytes",
			ErrNoPreamble, PreambleCopyright, len(window))
	}
	var marker [4]byte
	copy(marker[:], head[firmware.DirectoryMarkerOffset:])
	if err := firmware.CheckDirectoryMarker(marker); err != nil {
		return fmt.Errorf("%w: %v (found %q at %#x)",
			ErrNoPreamble, err, marker[:], firmware.DirectoryMarkerOffset)
	}
	return nil
}

// Parse reads the preamble and the image directory.
//
// An unrecognized directory format version is an error, not a warning:
// the offsets below are only known to mean what we think they mean for
// versions 2 and 3.
func Parse(p Partition) (*Directory, error) {
	if p.R == nil {
		return nil, errors.New("fwpart: nil reader")
	}
	if p.Size < PreambleWindow {
		return nil, fmt.Errorf("%w: partition is %d bytes, need at least %d",
			ErrNoPreamble, p.Size, PreambleWindow)
	}
	head := make([]byte, PreambleWindow)
	if _, err := p.R.ReadAt(head, 0); err != nil {
		return nil, fmt.Errorf("fwpart: read partition head: %w", err)
	}
	if err := CheckPreamble(head); err != nil {
		return nil, err
	}
	loc, err := firmware.ReadDirectoryLocator(head)
	if err != nil {
		return nil, fmt.Errorf("fwpart: %w", err)
	}

	d := &Directory{Version: loc.Version, Start: loc.Start, p: p}
	if int64(d.Start)+EntrySize > p.Size {
		return nil, fmt.Errorf("%w: directory starts at %#x but the partition is %d bytes",
			ErrOutOfRange, d.Start, p.Size)
	}

	room := (p.Size - int64(d.Start)) / EntrySize
	n := int64(MaxEntries)
	if room < n {
		n = room
	}
	raw := make([]byte, n*EntrySize)
	if _, err := p.R.ReadAt(raw, int64(d.Start)); err != nil {
		return nil, fmt.Errorf("fwpart: read directory at %#x: %w", d.Start, err)
	}
	for i := int64(0); i < n; i++ {
		row := raw[i*EntrySize : (i+1)*EntrySize]
		e, err := firmware.ReadDirectoryEntry(bytes.NewReader(row))
		if err != nil {
			return nil, fmt.Errorf("fwpart: decode directory entry %d: %w", i, err)
		}
		if terminator(row, e) {
			break
		}
		d.Entries = append(d.Entries, e)
	}
	if len(d.Entries) == 0 {
		return nil, fmt.Errorf("fwpart: image directory at %#x is empty", d.Start)
	}
	return d, nil
}

// terminator reports whether a directory row ends the list.
//
// The plan said "entry 4 is zeros". On both dumps it is not quite: the
// row after HIBE is zero except for LoadAddr2, which reads 0xFFFFFFFF
// (bytes 0x42C4..0x42C8). A strict all-zero test therefore walks one
// row too far and reports a fifth, nonexistent image. What actually
// ends the list is the absence of a container and an image type: a row
// with no tags names nothing, whatever trailing bytes happen to follow
// it.
func terminator(row []byte, e firmware.DirectoryEntry) bool {
	var zero [EntrySize]byte
	if bytes.Equal(row, zero[:]) {
		return true
	}
	return e.ContainerID == [4]byte{} && e.ImageType == [4]byte{} &&
		e.DevOffset == 0 && e.Length == 0
}

// OSOS returns the main OS entry — the only one this project ever
// writes.
func (d *Directory) OSOS() (idx int, e firmware.DirectoryEntry, ok bool) {
	for i := range d.Entries {
		if d.Entries[i].IsOSOS() {
			return i, d.Entries[i], true
		}
	}
	return 0, firmware.DirectoryEntry{}, false
}

// BodyOffset is where entry e's image body starts.
func BodyOffset(e firmware.DirectoryEntry) int64 { return int64(e.DevOffset) + BodyBias }

// EntryOffset is where row idx of the directory starts.
func (d *Directory) EntryOffset(idx int) int64 {
	return int64(d.Start) + int64(idx)*EntrySize
}

// Capacity is how many bytes entry idx's body may occupy: up to the
// next entry's DevOffset, or the end of the partition for the last
// entry.
//
// The device's OSOS: next entry (RSRC) is at 0x749000, body starts at
// 0x5000, so 7,618,560 bytes. An image larger than this would overwrite
// Apple's resource image, which is the one thing on the partition we
// cannot rebuild.
func (d *Directory) Capacity(idx int) uint32 {
	if idx < 0 || idx >= len(d.Entries) {
		return 0
	}
	start := BodyOffset(d.Entries[idx])
	var end int64
	if idx+1 < len(d.Entries) {
		end = int64(d.Entries[idx+1].DevOffset)
	} else {
		end = d.p.Size
	}
	if end <= start {
		return 0
	}
	if n := end - start; n < int64(^uint32(0)) {
		return uint32(n)
	}
	return ^uint32(0)
}

// readAt reads exactly n bytes at off, bounded by the partition.
func readAt(p Partition, off int64, n int64) ([]byte, error) {
	if off < 0 || n < 0 || off+n > p.Size {
		return nil, fmt.Errorf("%w: %d bytes at %#x, partition is %d bytes",
			ErrOutOfRange, n, off, p.Size)
	}
	buf := make([]byte, n)
	if _, err := p.R.ReadAt(buf, off); err != nil {
		return nil, fmt.Errorf("fwpart: read %d bytes at %#x: %w", n, off, err)
	}
	return buf, nil
}

// ReadBody returns exactly e.Length bytes of e's image body, from
// DevOffset+0x800. The zero padding ipodpatcher writes past Length is
// not part of the image and not part of the checksum, so it is not
// returned.
func ReadBody(p Partition, e firmware.DirectoryEntry) ([]byte, error) {
	return readAt(p, BodyOffset(e), int64(e.Length))
}

// sumBody streams the plain 32-bit sum of e's body without holding it
// in memory: the HIBE image on this device is 67 MB and inspecting a
// partition checksums every entry.
func sumBody(p Partition, e firmware.DirectoryEntry) (uint32, error) {
	off, n := BodyOffset(e), int64(e.Length)
	if off < 0 || n < 0 || off+n > p.Size {
		return 0, fmt.Errorf("%w: body of %d bytes at %#x, partition is %d bytes",
			ErrOutOfRange, n, off, p.Size)
	}
	var sum uint32
	buf := make([]byte, 1<<20)
	for n > 0 {
		chunk := buf
		if int64(len(chunk)) > n {
			chunk = chunk[:n]
		}
		if _, err := p.R.ReadAt(chunk, off); err != nil {
			return 0, fmt.Errorf("fwpart: read body at %#x: %w", off, err)
		}
		// firmware.Checksum's seed argument doubles as the
		// accumulator, so chunking gives the same answer as one
		// pass over the whole body.
		sum = firmware.Checksum(firmware.ModelNum(sum), chunk)
		off += int64(len(chunk))
		n -= int64(len(chunk))
	}
	return sum, nil
}

// EntryChecksum recomputes the checksum an entry should carry: the
// plain wrapping sum of its Length body bytes, no model seed.
func EntryChecksum(p Partition, e firmware.DirectoryEntry) (uint32, error) {
	return sumBody(p, e)
}

// VerifyEntry checks the stored checksum against a fresh sum of the
// body.
func VerifyEntry(p Partition, e firmware.DirectoryEntry) error {
	got, err := sumBody(p, e)
	if err != nil {
		return err
	}
	if got != e.Checksum {
		return fmt.Errorf("%w: entry %q stores checksum %#08x, body of %d bytes sums to %#08x",
			ErrVerify, e.LogicalImageType(), e.Checksum, e.Length, got)
	}
	return nil
}

// ImageChecksum is the value an entry must carry for this image: the
// plain wrapping sum, no seed.
func ImageChecksum(image []byte) uint32 { return firmware.Checksum(0, image) }

// Write is one byte range to put on the device, exactly as given. The
// caller performs it; this package only computes it.
type Write struct {
	Off  int64
	Data []byte
}

// End is the first byte past the write.
func (w Write) End() int64 { return w.Off + int64(len(w.Data)) }

// PlanWrite produces the complete write set for replacing entry idx's
// image with `image`, and the directory entry that results.
//
// Two writes, in this order:
//
//  1. the body at DevOffset+0x800, zero-padded up to a 0x800
//     boundary (what ipodpatcher does; the padding is outside Length
//     and outside the checksum);
//  2. the single sectorSize-aligned sector that holds the 40-byte
//     entry, re-read from the partition and re-encoded with the new
//     Length and Checksum. Every other field of the entry, every
//     other entry in that sector, and every other byte of the sector
//     are preserved byte-for-byte.
//
// sectorSize is a parameter because it is a property of the transport,
// not of the partition: the USB bridge on this device reports 2048-byte
// logical sectors, an ATA bridge or an image file may be 512. Read it
// from the OS; never assume.
//
// Body first, directory second: an interrupted flash then leaves the
// old entry pointing at a half-written body (the checksum will not
// verify and the boot ROM refuses it), rather than a new entry pointing
// at bytes that were never written.
func PlanWrite(d *Directory, idx int, image []byte, sectorSize int) ([]Write, firmware.DirectoryEntry, error) {
	var none firmware.DirectoryEntry
	if idx < 0 || idx >= len(d.Entries) {
		return nil, none, fmt.Errorf("fwpart: entry %d out of range (%d entries)", idx, len(d.Entries))
	}
	if sectorSize <= 0 || sectorSize&(sectorSize-1) != 0 {
		return nil, none, fmt.Errorf("%w: %d is not a positive power of two", ErrBadSectorSize, sectorSize)
	}
	if len(image) == 0 {
		return nil, none, errors.New("fwpart: refusing to write an empty image")
	}

	e := d.Entries[idx]
	capacity := d.Capacity(idx)
	if uint64(len(image)) > uint64(capacity) {
		return nil, none, fmt.Errorf("%w: %d bytes, entry %q holds %d (body %#x, next image at %#x)",
			ErrImageTooLarge, len(image), e.LogicalImageType(), capacity,
			BodyOffset(e), BodyOffset(e)+int64(capacity))
	}
	padded := (len(image) + BodyAlign - 1) &^ (BodyAlign - 1)
	if uint64(padded) > uint64(capacity) {
		return nil, none, fmt.Errorf("%w: %d bytes pad to %d, entry %q holds %d",
			ErrImageTooLarge, len(image), padded, e.LogicalImageType(), capacity)
	}
	bodyOff := BodyOffset(e)
	if bodyOff+int64(padded) > d.p.Size {
		return nil, none, fmt.Errorf("%w: padded body of %d bytes at %#x, partition is %d bytes",
			ErrOutOfRange, padded, bodyOff, d.p.Size)
	}

	body := make([]byte, padded)
	copy(body, image)

	// The entry's sector, read back and edited in place. Reading it
	// first is what keeps the other rows intact: we never synthesize a
	// directory, we amend one.
	entryOff := d.EntryOffset(idx)
	sectorOff := entryOff &^ int64(sectorSize-1)
	within := entryOff - sectorOff
	if within+EntrySize > int64(sectorSize) {
		return nil, none, fmt.Errorf("%w: entry %d at %#x straddles two %d-byte sectors",
			ErrBadSectorSize, idx, entryOff, sectorSize)
	}
	sector, err := readAt(d.p, sectorOff, int64(sectorSize))
	if err != nil {
		return nil, none, err
	}

	updated := e
	updated.Length = uint32(len(image))
	updated.Checksum = ImageChecksum(image)

	var enc bytes.Buffer
	if err := firmware.WriteDirectoryEntry(&enc, updated); err != nil {
		return nil, none, fmt.Errorf("fwpart: encode directory entry: %w", err)
	}
	copy(sector[within:within+EntrySize], enc.Bytes())

	return []Write{
		{Off: bodyOff, Data: body},
		{Off: sectorOff, Data: sector},
	}, updated, nil
}

// VerifyWritten re-reads the device and proves the write landed: the
// body matches `image` byte for byte, its sum matches the checksum in
// e, and the directory row on the device is e.
//
// This is check (c) of the checklist in doc.go. It reads through the
// same Partition the caller will hand it, so a caller that wants a true
// device read-back must pass a handle that does not serve the write
// back out of its own cache.
func VerifyWritten(p Partition, e firmware.DirectoryEntry, image []byte) error {
	if e.Length != uint32(len(image)) {
		return fmt.Errorf("%w: entry Length is %d, image is %d bytes",
			ErrVerify, e.Length, len(image))
	}
	body, err := ReadBody(p, e)
	if err != nil {
		return err
	}
	if !bytes.Equal(body, image) {
		return fmt.Errorf("%w: body at %#x differs from the image at byte %d",
			ErrVerify, BodyOffset(e), firstDiff(body, image))
	}
	if got := ImageChecksum(image); got != e.Checksum {
		return fmt.Errorf("%w: entry checksum %#08x, image sums to %#08x",
			ErrVerify, e.Checksum, got)
	}

	d, err := Parse(p)
	if err != nil {
		return fmt.Errorf("%w: re-reading the directory: %v", ErrVerify, err)
	}
	for i := range d.Entries {
		if d.Entries[i].ImageType == e.ImageType && d.Entries[i].DevOffset == e.DevOffset {
			if d.Entries[i] != e {
				return fmt.Errorf("%w: directory row %d at %#x reads back as %+v, wrote %+v",
					ErrVerify, i, d.EntryOffset(i), d.Entries[i], e)
			}
			return nil
		}
	}
	return fmt.Errorf("%w: no %q entry at %#x in the directory read back",
		ErrVerify, e.LogicalImageType(), e.DevOffset)
}

// firstDiff reports the index of the first differing byte, or the
// shorter length if one is a prefix of the other.
func firstDiff(a, b []byte) int {
	n := len(a)
	if len(b) < n {
		n = len(b)
	}
	for i := 0; i < n; i++ {
		if a[i] != b[i] {
			return i
		}
	}
	return n
}
