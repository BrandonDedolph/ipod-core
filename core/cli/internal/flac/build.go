package flac

import (
	"encoding/binary"
	"sort"
)

// BuildFile assembles a small but genuinely valid FLAC file: the fLaC marker, a
// STREAMINFO block carrying info, a VORBIS_COMMENT block, one PICTURE block per
// entry in pics, and then real audio frames of digital silence for
// info.TotalSamples samples. ffprobe reports the STREAMINFO duration for it and
// ffmpeg decodes it without complaint, so tests in this package and in the
// library/art packages can synthesise fixtures without shelling out to ffmpeg.
//
// It is not an encoder: every frame is made of CONSTANT subframes holding the
// sample value zero, which is the cheapest legal way to say "silence".
//
// Defaults applied: block size 4096 (min = max in STREAMINFO), frame sizes 0
// (unknown), MD5 all zero (unknown, so decoders skip the check).
func BuildFile(info StreamInfo, tags map[string]string, pics []Picture) []byte {
	if info.SampleRate == 0 {
		info.SampleRate = 44100
	}
	if info.Channels == 0 {
		info.Channels = 2
	}
	if info.BitsPerSample == 0 {
		info.BitsPerSample = 16
	}

	const blockSize = 4096

	var out []byte
	out = append(out, 'f', 'L', 'a', 'C')

	blocks := [][]byte{buildStreamInfo(info, blockSize), buildVorbisComment(tags)}
	types := []byte{blockStreamInfo, blockVorbisComment}
	for _, p := range pics {
		blocks = append(blocks, buildPicture(p))
		types = append(types, blockPicture)
	}
	for i, b := range blocks {
		out = appendBlock(out, types[i], i == len(blocks)-1, b)
	}

	// Audio: one frame per 4096-sample block, plus a short final frame for the
	// remainder, so the decoded stream is exactly TotalSamples long.
	frame := uint64(0)
	for done := uint64(0); done < info.TotalSamples; frame++ {
		n := info.TotalSamples - done
		if n > blockSize {
			n = blockSize
		}
		out = append(out, buildFrame(info, uint32(n), frame)...)
		done += n
	}
	return out
}

func appendBlock(dst []byte, typ byte, last bool, body []byte) []byte {
	h := typ & 0x7f
	if last {
		h |= 0x80
	}
	n := len(body)
	dst = append(dst, h, byte(n>>16), byte(n>>8), byte(n))
	return append(dst, body...)
}

func buildStreamInfo(info StreamInfo, blockSize uint16) []byte {
	b := make([]byte, streamInfoLen)
	binary.BigEndian.PutUint16(b[0:2], blockSize) // min block size
	binary.BigEndian.PutUint16(b[2:4], blockSize) // max block size
	// bytes 4..9: min/max frame size stay zero = unknown.
	w := uint64(info.SampleRate)<<44 |
		uint64(info.Channels-1)<<41 |
		uint64(info.BitsPerSample-1)<<36 |
		(info.TotalSamples & 0xf_ffff_ffff)
	binary.BigEndian.PutUint64(b[10:18], w)
	// bytes 18..33: MD5 stays zero = unknown.
	return b
}

func buildVorbisComment(tags map[string]string) []byte {
	const vendor = "core"
	keys := make([]string, 0, len(tags))
	for k := range tags {
		keys = append(keys, k)
	}
	sort.Strings(keys) // deterministic output

	var b []byte
	b = appendU32LE(b, uint32(len(vendor)))
	b = append(b, vendor...)
	b = appendU32LE(b, uint32(len(keys)))
	for _, k := range keys {
		c := k + "=" + tags[k]
		b = appendU32LE(b, uint32(len(c)))
		b = append(b, c...)
	}
	return b
}

func buildPicture(p Picture) []byte {
	var b []byte
	b = appendU32BE(b, p.Type)
	b = appendU32BE(b, uint32(len(p.MIME)))
	b = append(b, p.MIME...)
	b = appendU32BE(b, 0)                   // description length
	b = appendU32BE(b, 0)                   // width
	b = appendU32BE(b, 0)                   // height
	b = appendU32BE(b, 0)                   // colour depth
	b = appendU32BE(b, 0)                   // colour count (0 = not indexed)
	b = appendU32BE(b, uint32(len(p.Data))) //
	return append(b, p.Data...)
}

func appendU32LE(b []byte, v uint32) []byte {
	return append(b, byte(v), byte(v>>8), byte(v>>16), byte(v>>24))
}

func appendU32BE(b []byte, v uint32) []byte {
	return append(b, byte(v>>24), byte(v>>16), byte(v>>8), byte(v))
}

// buildFrame emits one FLAC audio frame of n silent samples per channel.
//
// Frame header (bit fields, MSB first):
//
//	14  sync code 0b11111111111110
//	 1  reserved (0)
//	 1  blocking strategy: 0 = fixed block size, the coded number is a frame number
//	 4  block size code: 0b1100 = 4096, 0b0111 = "16 bits follow, holding n-1"
//	 4  sample rate code: 0b0000 = take it from STREAMINFO
//	 4  channel assignment: channels-1, independently coded
//	 3  sample size code: 0b000 = take it from STREAMINFO
//	 1  reserved (0)
//	    UTF-8-style coded frame number (1..7 bytes)
//	    [16 bits of n-1 when the block size code is 0b0111]
//	 8  CRC-8 (poly 0x07, init 0) over the header bytes above
//
// Then one subframe per channel: 1 zero bit, 6 bits of subframe type
// (0b000000 = CONSTANT), 1 bit "wasted bits" flag (0), then the constant
// sample value in bits-per-sample bits — zero, i.e. silence. The frame is
// zero-padded to a byte boundary and closed with a CRC-16 (poly 0x8005,
// init 0) over everything from the sync code onwards.
func buildFrame(info StreamInfo, n uint32, frameNumber uint64) []byte {
	var w bitWriter
	w.write(0x3ffe, 14) // sync
	w.write(0, 1)       // reserved
	w.write(0, 1)       // fixed block size
	explicit := n != 4096
	if explicit {
		w.write(0x7, 4) // 16-bit block size follows
	} else {
		w.write(0xc, 4) // 4096
	}
	w.write(0, 4)                       // sample rate from STREAMINFO
	w.write(uint64(info.Channels-1), 4) // independent channels
	w.write(0, 3)                       // sample size from STREAMINFO
	w.write(0, 1)                       // reserved
	w.bytes = append(w.bytes, utf8Number(frameNumber)...)
	if explicit {
		w.write(uint64(n-1), 16)
	}
	w.bytes = append(w.bytes, crc8(w.bytes))

	for c := uint8(0); c < info.Channels; c++ {
		w.write(0, 8)                       // CONSTANT subframe header
		w.write(0, int(info.BitsPerSample)) // the constant sample: silence
	}
	w.pad()

	sum := crc16(w.bytes)
	return append(w.bytes, byte(sum>>8), byte(sum))
}

// bitWriter accumulates whole bytes; every call site leaves it byte-aligned
// before reading w.bytes.
type bitWriter struct {
	bytes []byte
	cur   byte
	nbits uint
}

func (w *bitWriter) write(v uint64, n int) {
	for i := n - 1; i >= 0; i-- {
		w.cur = w.cur<<1 | byte((v>>uint(i))&1)
		w.nbits++
		if w.nbits == 8 {
			w.bytes = append(w.bytes, w.cur)
			w.cur, w.nbits = 0, 0
		}
	}
}

func (w *bitWriter) pad() {
	if w.nbits != 0 {
		w.write(0, int(8-w.nbits))
	}
}

// utf8Number encodes v the way FLAC codes frame and sample numbers: UTF-8's
// byte pattern extended to 36 bits (up to 7 bytes).
func utf8Number(v uint64) []byte {
	switch {
	case v < 0x80:
		return []byte{byte(v)}
	case v < 0x800:
		return []byte{byte(0xc0 | v>>6), byte(0x80 | v&0x3f)}
	case v < 0x10000:
		return []byte{byte(0xe0 | v>>12), byte(0x80 | (v>>6)&0x3f), byte(0x80 | v&0x3f)}
	case v < 0x200000:
		return []byte{byte(0xf0 | v>>18), byte(0x80 | (v>>12)&0x3f), byte(0x80 | (v>>6)&0x3f), byte(0x80 | v&0x3f)}
	case v < 0x4000000:
		return []byte{byte(0xf8 | v>>24), byte(0x80 | (v>>18)&0x3f), byte(0x80 | (v>>12)&0x3f), byte(0x80 | (v>>6)&0x3f), byte(0x80 | v&0x3f)}
	case v < 0x80000000:
		return []byte{byte(0xfc | v>>30), byte(0x80 | (v>>24)&0x3f), byte(0x80 | (v>>18)&0x3f), byte(0x80 | (v>>12)&0x3f), byte(0x80 | (v>>6)&0x3f), byte(0x80 | v&0x3f)}
	default:
		return []byte{0xfe, byte(0x80 | (v>>30)&0x3f), byte(0x80 | (v>>24)&0x3f), byte(0x80 | (v>>18)&0x3f), byte(0x80 | (v>>12)&0x3f), byte(0x80 | (v>>6)&0x3f), byte(0x80 | v&0x3f)}
	}
}

func crc8(b []byte) byte {
	var c byte
	for _, x := range b {
		c ^= x
		for i := 0; i < 8; i++ {
			if c&0x80 != 0 {
				c = c<<1 ^ 0x07
			} else {
				c <<= 1
			}
		}
	}
	return c
}

func crc16(b []byte) uint16 {
	var c uint16
	for _, x := range b {
		c ^= uint16(x) << 8
		for i := 0; i < 8; i++ {
			if c&0x8000 != 0 {
				c = c<<1 ^ 0x8005
			} else {
				c <<= 1
			}
		}
	}
	return c
}
