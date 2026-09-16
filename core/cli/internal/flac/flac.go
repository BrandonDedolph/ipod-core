// Package flac reads FLAC metadata blocks: STREAMINFO, VORBIS_COMMENT and
// PICTURE. It never reads audio frames — the walk stops at the block whose
// "last-metadata-block" flag is set.
//
// The parser is deliberately small and total: every length is bounded, every
// short read is an error, and unknown block types are skipped without
// interpretation. See https://xiph.org/flac/format.html for the on-disk layout.
package flac

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
)

// MaxBlockSize is the hard cap on a single metadata block. The 24-bit length
// field allows 16 MiB, so this is never a real limit for a well-formed file;
// it exists so a corrupt header cannot make us allocate wildly.
const MaxBlockSize = 64 << 20

// Metadata block types we care about.
const (
	blockStreamInfo    = 0
	blockVorbisComment = 4
	blockPicture       = 6
)

// PictureTypeFrontCover is the ID3v2 APIC picture type for a front cover.
const PictureTypeFrontCover = 3

// streamInfoLen is the fixed size of a STREAMINFO block body.
const streamInfoLen = 34

// StreamInfo is the subset of the STREAMINFO block the library needs.
type StreamInfo struct {
	SampleRate    uint32
	Channels      uint8
	BitsPerSample uint8
	TotalSamples  uint64
}

// Picture is one PICTURE block. The art pipeline decodes the image itself, so
// the dimension fields are advisory on read — they are what the file claims,
// not what the bytes are. WritePicture ignores whatever the caller puts in
// them and fills them from the image.
type Picture struct {
	Type   uint32
	MIME   string
	Width  uint32
	Height uint32
	Depth  uint32 // bits per pixel
	Colors uint32 // palette size for indexed images, 0 otherwise
	Data   []byte
}

// Meta is everything Read recovered from a file's metadata blocks.
type Meta struct {
	Info     StreamInfo
	Tags     map[string]string // keys lower-cased; last duplicate wins
	Pictures []Picture
}

// ErrNotFLAC is returned when the stream does not start with the fLaC marker.
var ErrNotFLAC = errors.New("flac: missing fLaC marker")

// ReadFile parses the metadata of the FLAC file at path.
func ReadFile(path string) (*Meta, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	m, err := Read(f)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return m, nil
}

// Read parses the metadata blocks of a FLAC stream. It stops at the block
// flagged last, so the audio frames are never touched. The reader is consumed
// sequentially; Read never seeks, so a short file is reported as truncation
// rather than silently skipped over.
func Read(r io.ReadSeeker) (*Meta, error) {
	return readFrom(bufio.NewReaderSize(r, 64<<10))
}

func readFrom(r io.Reader) (*Meta, error) {
	var marker [4]byte
	if _, err := io.ReadFull(r, marker[:]); err != nil {
		return nil, ErrNotFLAC
	}
	if string(marker[:]) != "fLaC" {
		return nil, ErrNotFLAC
	}

	m := &Meta{Tags: make(map[string]string)}
	sawStreamInfo := false

	for n := 0; ; n++ {
		var hdr [4]byte
		if _, err := io.ReadFull(r, hdr[:]); err != nil {
			return nil, fmt.Errorf("flac: truncated metadata block header %d: %w", n, shortErr(err))
		}
		// Block header: 1 bit "last", 7 bits type, 24 bits body length.
		last := hdr[0]&0x80 != 0
		typ := hdr[0] & 0x7f
		length := uint32(hdr[1])<<16 | uint32(hdr[2])<<8 | uint32(hdr[3])

		if length > MaxBlockSize {
			return nil, fmt.Errorf("flac: metadata block %d (type %d) is %d bytes, over the %d byte cap", n, typ, length, MaxBlockSize)
		}

		switch {
		case n == 0 && typ != blockStreamInfo:
			return nil, fmt.Errorf("flac: first metadata block is type %d, want STREAMINFO", typ)

		case typ == blockStreamInfo:
			body, err := readBlock(r, length, n, typ)
			if err != nil {
				return nil, err
			}
			if len(body) < streamInfoLen {
				return nil, fmt.Errorf("flac: STREAMINFO is %d bytes, want at least %d", len(body), streamInfoLen)
			}
			m.Info = parseStreamInfo(body)
			sawStreamInfo = true

		case typ == blockVorbisComment:
			body, err := readBlock(r, length, n, typ)
			if err != nil {
				return nil, err
			}
			if err := parseVorbisComment(body, m.Tags); err != nil {
				return nil, err
			}

		case typ == blockPicture:
			body, err := readBlock(r, length, n, typ)
			if err != nil {
				return nil, err
			}
			p, err := parsePicture(body)
			if err != nil {
				return nil, err
			}
			m.Pictures = append(m.Pictures, p)

		default:
			// PADDING, APPLICATION, SEEKTABLE, CUESHEET, anything reserved:
			// consumed (not seeked past) so that a truncated tail is an error.
			if _, err := io.CopyN(io.Discard, r, int64(length)); err != nil {
				return nil, fmt.Errorf("flac: truncated metadata block %d (type %d): %w", n, typ, shortErr(err))
			}
		}

		if last {
			break
		}
	}

	if !sawStreamInfo {
		return nil, errors.New("flac: no STREAMINFO block")
	}
	return m, nil
}

func readBlock(r io.Reader, length uint32, n int, typ byte) ([]byte, error) {
	body := make([]byte, length)
	if _, err := io.ReadFull(r, body); err != nil {
		return nil, fmt.Errorf("flac: truncated metadata block %d (type %d), want %d bytes: %w", n, typ, length, shortErr(err))
	}
	return body, nil
}

func shortErr(err error) error {
	if errors.Is(err, io.EOF) {
		return io.ErrUnexpectedEOF
	}
	return err
}

// parseStreamInfo decodes the fixed 34-byte STREAMINFO body.
//
// Byte layout (big-endian, bit 0 = MSB of byte 0):
//
//	bytes  0..1   u16  minimum block size (samples)
//	bytes  2..3   u16  maximum block size (samples)
//	bytes  4..6   u24  minimum frame size (bytes)
//	bytes  7..9   u24  maximum frame size (bytes)
//	bytes 10..17  one 64-bit big-endian word packed as:
//	                bits 63..44  (20 bits)  sample rate in Hz
//	                bits 43..41  ( 3 bits)  number of channels minus one
//	                bits 40..36  ( 5 bits)  bits per sample minus one
//	                bits 35..0   (36 bits)  total samples in the stream
//	bytes 18..33  16 B  MD5 of the unencoded audio (all zero = unknown)
//
// So sample_rate starts at byte 10 and total_samples straddles bytes 13..17,
// which is why the whole thing is read as one word rather than byte by byte.
func parseStreamInfo(b []byte) StreamInfo {
	w := binary.BigEndian.Uint64(b[10:18])
	return StreamInfo{
		SampleRate:    uint32(w >> 44),
		Channels:      uint8((w>>41)&0x7) + 1,
		BitsPerSample: uint8((w>>36)&0x1f) + 1,
		TotalSamples:  w & 0xf_ffff_ffff,
	}
}

// parseVorbisComment decodes a VORBIS_COMMENT body into tags. Unlike the rest
// of FLAC this block is little-endian (it is lifted verbatim from Vorbis).
//
//	u32le vendor length, vendor bytes
//	u32le comment count
//	per comment: u32le length, then "KEY=value"
//
// The key is everything before the FIRST '=' (lower-cased, ASCII per spec);
// the value is the remaining bytes verbatim — invalid UTF-8 is preserved, not
// replaced. The last duplicate of a key wins, matching ffprobe.
func parseVorbisComment(b []byte, tags map[string]string) error {
	c := cursor{b: b}
	vendorLen, err := c.u32le("vendor length")
	if err != nil {
		return err
	}
	if _, err := c.take(vendorLen, "vendor string"); err != nil {
		return err
	}
	count, err := c.u32le("comment count")
	if err != nil {
		return err
	}
	for i := uint32(0); i < count; i++ {
		n, err := c.u32le(fmt.Sprintf("comment %d length", i))
		if err != nil {
			return err
		}
		raw, err := c.take(n, fmt.Sprintf("comment %d", i))
		if err != nil {
			return err
		}
		eq := indexByte(raw, '=')
		if eq <= 0 {
			// No '=' at all, or an empty field name: malformed, ignored.
			continue
		}
		tags[asciiLower(string(raw[:eq]))] = string(raw[eq+1:])
	}
	return nil
}

// parsePicture decodes a PICTURE body (all big-endian):
//
//	u32 picture type, u32 MIME length, MIME, u32 description length,
//	description, u32 width, u32 height, u32 colour depth, u32 colour count,
//	u32 data length, data
func parsePicture(b []byte) (Picture, error) {
	var p Picture
	c := cursor{b: b}
	typ, err := c.u32be("picture type")
	if err != nil {
		return p, err
	}
	mimeLen, err := c.u32be("MIME length")
	if err != nil {
		return p, err
	}
	mime, err := c.take(mimeLen, "MIME type")
	if err != nil {
		return p, err
	}
	descLen, err := c.u32be("description length")
	if err != nil {
		return p, err
	}
	if _, err := c.take(descLen, "description"); err != nil {
		return p, err
	}
	dims := make([]uint32, 4)
	for i, what := range []string{"width", "height", "colour depth", "colour count"} {
		v, err := c.u32be(what)
		if err != nil {
			return p, err
		}
		dims[i] = v
	}
	dataLen, err := c.u32be("data length")
	if err != nil {
		return p, err
	}
	data, err := c.take(dataLen, "picture data")
	if err != nil {
		return p, err
	}
	p.Type = typ
	p.MIME = string(mime)
	p.Width, p.Height, p.Depth, p.Colors = dims[0], dims[1], dims[2], dims[3]
	p.Data = data
	return p, nil
}

// cursor is a bounds-checked reader over one metadata block body.
type cursor struct {
	b []byte
	i int
}

func (c *cursor) take(n uint32, what string) ([]byte, error) {
	if uint64(n) > uint64(len(c.b)-c.i) {
		return nil, fmt.Errorf("flac: %s runs %d bytes past the end of its metadata block", what, uint64(n)-uint64(len(c.b)-c.i))
	}
	out := c.b[c.i : c.i+int(n)]
	c.i += int(n)
	return out, nil
}

func (c *cursor) u32le(what string) (uint32, error) {
	b, err := c.take(4, what)
	if err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint32(b), nil
}

func (c *cursor) u32be(what string) (uint32, error) {
	b, err := c.take(4, what)
	if err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint32(b), nil
}

func indexByte(b []byte, c byte) int {
	for i := range b {
		if b[i] == c {
			return i
		}
	}
	return -1
}

// asciiLower lower-cases A-Z only. Vorbis field names are ASCII 0x20..0x7D by
// spec, so Unicode case folding would only add surprises.
func asciiLower(s string) string {
	need := false
	for i := 0; i < len(s); i++ {
		if s[i] >= 'A' && s[i] <= 'Z' {
			need = true
			break
		}
	}
	if !need {
		return s
	}
	b := []byte(s)
	for i := range b {
		if b[i] >= 'A' && b[i] <= 'Z' {
			b[i] += 'a' - 'A'
		}
	}
	return string(b)
}

// DurationSeconds is total samples divided by sample rate, truncated. This is
// exactly what ffprobe reports as format.duration for FLAC, and what
// tools/build_index.py stores, so the integer division is the contract — not a
// rounding choice. Zero when either field is missing.
func (m *Meta) DurationSeconds() uint32 {
	if m == nil || m.Info.SampleRate == 0 || m.Info.TotalSamples == 0 {
		return 0
	}
	return uint32(m.Info.TotalSamples / uint64(m.Info.SampleRate))
}

// FrontCover returns the first picture of type 3 (front cover), else the first
// picture of any type, else nil.
func (m *Meta) FrontCover() *Picture {
	if m == nil || len(m.Pictures) == 0 {
		return nil
	}
	for i := range m.Pictures {
		if m.Pictures[i].Type == PictureTypeFrontCover {
			return &m.Pictures[i]
		}
	}
	return &m.Pictures[0]
}

// Tag returns the value of the first of keys that is present and non-empty.
func (m *Meta) Tag(keys ...string) string {
	if m == nil {
		return ""
	}
	for _, k := range keys {
		if v := m.Tags[k]; v != "" {
			return v
		}
	}
	return ""
}
