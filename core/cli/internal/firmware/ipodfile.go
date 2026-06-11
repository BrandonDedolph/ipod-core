package firmware

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

// ModelName is the 4-byte ASCII tag that identifies which iPod model an
// .ipod-format image targets. Distinct from ModelNum, which seeds the
// additive checksum — the two are independent fields baked into different
// places in the file format.
type ModelName [4]byte

// Known model names. The set we actually care about is just iPodVideo.
var (
	ModelNameIPodVideo = ModelName{'i', 'p', 'v', 'd'}
	ModelNameIPodNano  = ModelName{'n', 'a', 'n', 'o'}
)

// ModelNumForName maps an embedded .ipod model name to the checksum
// seed for that model (per core/docs/hw/08-boot-dock.md: "ipvd" for
// Video, "nano" for Nano). ok is false for names we don't know.
func ModelNumForName(name ModelName) (num ModelNum, ok bool) {
	switch name {
	case ModelNameIPodVideo:
		return ModelIPodVideo, true
	case ModelNameIPodNano:
		return ModelIPodNano, true
	}
	return 0, false
}

// IPodFileHeaderSize is the fixed 8-byte prefix of an .ipod-format file:
// 4 bytes of big-endian additive checksum + 4 bytes of model name.
const IPodFileHeaderSize = 8

// ErrShortIPodFile is returned when a putative .ipod file is shorter than
// the 8-byte header.
var ErrShortIPodFile = errors.New(".ipod file: truncated before header")

// ErrIPodChecksumMismatch is returned by ReadIPodFile when the embedded
// checksum doesn't match a fresh computation over the image bytes.
var ErrIPodChecksumMismatch = errors.New(".ipod file: checksum mismatch")

// ErrUnknownModelName is returned by ReadIPodFile when the embedded
// 4-byte model name isn't one we know a checksum seed for, so the
// checksum can't be verified.
var ErrUnknownModelName = errors.New(".ipod file: unknown model name")

// WriteIPodFile emits an .ipod-format image to w: a 4-byte big-endian
// additive checksum (computed over `image` with `model` as the seed),
// then 4 bytes of model name, then the raw image bytes. The total
// written is `IPodFileHeaderSize + len(image)`.
//
// Per core/docs/hw/08-boot-dock.md:
//
//	chksum = ModelNum + sum(image[i])    // 32-bit additive, wraps
//	file   = [BE32 chksum][name[4]][image...]
//
// ipodpatcher (the GPL-2 C tool we're replacing) accepts files in this
// format; the Apple boot ROM consumes the partition layout, not the
// .ipod file directly — the install/update flow lifts the image out of
// the file and writes it to the firmware partition.
func WriteIPodFile(w io.Writer, model ModelNum, name ModelName, image []byte) error {
	sum := Checksum(model, image)

	var hdr [IPodFileHeaderSize]byte
	binary.BigEndian.PutUint32(hdr[0:4], sum)
	copy(hdr[4:8], name[:])

	if _, err := w.Write(hdr[:]); err != nil {
		return fmt.Errorf(".ipod header write: %w", err)
	}
	if _, err := w.Write(image); err != nil {
		return fmt.Errorf(".ipod image write: %w", err)
	}
	return nil
}

// ReadIPodFile parses an .ipod-format image from r. Returns the model
// name, image bytes, and any error.
//
// The checksum seed is derived from the embedded model name (via
// ModelNumForName), so a Nano image verifies with the Nano seed and a
// Video image with the Video seed. If the name isn't one we know,
// ErrUnknownModelName is returned — with the name and image bytes
// still populated, consistent with the recovery-tool philosophy below.
//
// The embedded checksum is verified against a fresh computation; on
// mismatch the image bytes are still returned alongside ErrIPodChecksumMismatch
// so callers can choose whether to trust them (e.g. recovery tools that
// want to inspect a corrupt image).
func ReadIPodFile(r io.Reader) (ModelName, []byte, error) {
	var hdr [IPodFileHeaderSize]byte
	n, err := io.ReadFull(r, hdr[:])
	if err != nil {
		if errors.Is(err, io.ErrUnexpectedEOF) || (errors.Is(err, io.EOF) && n < IPodFileHeaderSize) {
			return ModelName{}, nil, ErrShortIPodFile
		}
		return ModelName{}, nil, err
	}

	want := binary.BigEndian.Uint32(hdr[0:4])
	var name ModelName
	copy(name[:], hdr[4:8])

	image, err := io.ReadAll(r)
	if err != nil {
		return name, nil, fmt.Errorf(".ipod image read: %w", err)
	}

	model, ok := ModelNumForName(name)
	if !ok {
		return name, image, fmt.Errorf("%w: %q", ErrUnknownModelName, string(name[:]))
	}

	if got := Checksum(model, image); got != want {
		return name, image, fmt.Errorf("%w: stored %#x, computed %#x",
			ErrIPodChecksumMismatch, want, got)
	}
	return name, image, nil
}
