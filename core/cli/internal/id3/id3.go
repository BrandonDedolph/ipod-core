// Package id3 reads an MP3's ID3v2 tags, its embedded cover and its duration.
//
// It is the MP3 counterpart of internal/flac, and it exists for the same
// reason: `core sync` must stamp a byte-identical CORELIB.IDX to
// tools/build_index.py for the same tree, and build_index.py gets its fields
// from ffprobe. Shelling out to ffprobe per track would make the Go tool
// depend on ffmpeg being installed, which is the dependency the Go port exists
// to remove — so the rules ffprobe applies are implemented here and pinned
// against it by parity_test.go.
//
// Tag keys are ffprobe's names, not ID3's frame ids (title, artist,
// album_artist, album, genre, track, disc, date), so the scanner's lookups
// work unchanged across both formats.
//
// What is deliberately not here: ReplayGain (TXXX frames), lyrics, chapters,
// and any attempt to reverse ID3v2's unsynchronisation scheme — the firmware
// does not read those either (core/codecs/pvmp3/id3_meta.c), and the two
// readers agreeing matters more than either being exhaustive.
package id3

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"strings"
	"unicode/utf16"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// Bounds. A tag past maxTagBytes is a corrupt size, not a tag; a frame past
// maxFrameBytes is art we skip anyway (APIC is read separately, by size).
const (
	maxTagBytes   = 64 << 20
	maxFrameBytes = 32 << 20
)

// ErrNotMP3 is returned when no MPEG Layer III frame could be found.
var ErrNotMP3 = errors.New("id3: no MPEG Layer III frame in the file")

// Meta is everything ReadFile recovered: the tags, the embedded pictures, and
// the stream facts the duration comes from.
type Meta struct {
	Tags     map[string]string // ffprobe's key names, lower-case
	Pictures []flac.Picture    // APIC frames, in file order

	// SampleRate and TotalSamples describe the AUDIO, and are what
	// DurationSeconds divides. TotalSamples already has the LAME encoder
	// delay and end padding taken off, which is ffprobe's rule.
	SampleRate   uint32
	TotalSamples uint64

	// VBR is true when the file carries a Xing (not Info) or VBRI header,
	// and Tagged is true when it carries either — a file with neither has an
	// ESTIMATED duration, exact only if it really is constant bitrate.
	VBR    bool
	Tagged bool
}

// DurationSeconds is the track length, truncated to whole seconds. That is
// what ffprobe reports as format.duration for an MP3 and what
// tools/build_index.py stores, so the integer division is the contract, not a
// rounding choice.
func (m *Meta) DurationSeconds() uint32 {
	if m == nil || m.SampleRate == 0 || m.TotalSamples == 0 {
		return 0
	}
	return uint32(m.TotalSamples / uint64(m.SampleRate))
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

// FrontCover returns the first APIC of type 3 (front cover), else the first
// picture of any type, else nil. It hands back a *flac.Picture because that is
// this tree's generic embedded-cover record and the art pipeline reads only
// its MIME and Data — an APIC fills the same struct rather than getting a
// near-identical twin.
func (m *Meta) FrontCover() *flac.Picture {
	if m == nil || len(m.Pictures) == 0 {
		return nil
	}
	for i := range m.Pictures {
		if m.Pictures[i].Type == flac.PictureTypeFrontCover {
			return &m.Pictures[i]
		}
	}
	return &m.Pictures[0]
}

// ReadFile parses the MP3 at path.
func ReadFile(path string) (*Meta, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	m, err := Read(b)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return m, nil
}

// Read parses a whole MP3 held in memory.
//
// Whole-file, unlike internal/flac's streaming reader, because an MP3 has no
// container: the audio length has to be found from the LAST bytes (an ID3v1 or
// APE tail bounds it) as well as the first, and the files this runs over are a
// few megabytes on a host with gigabytes.
func Read(b []byte) (*Meta, error) {
	m := &Meta{Tags: map[string]string{}}

	start := id3v2Len(b)
	parseID3v2(b[:min(start, len(b))], m)

	end := len(b) - tailTagLen(b[start:])
	if end < start {
		end = len(b)
	}
	if err := scanStream(b, start, end, m); err != nil {
		return nil, err
	}
	return m, nil
}

// ---------- ID3v2 --------------------------------------------------------

func syncsafe(b []byte) uint32 {
	return uint32(b[0]&0x7f)<<21 | uint32(b[1]&0x7f)<<14 |
		uint32(b[2]&0x7f)<<7 | uint32(b[3]&0x7f)
}

// id3v2One is the total length of the tag at b[0:], or 0 if there is none.
func id3v2One(b []byte) int {
	if len(b) < 10 || string(b[:3]) != "ID3" {
		return 0
	}
	if b[3] < 2 || b[3] > 4 || b[4] == 0xff {
		return 0
	}
	// The size is four sync-safe bytes: bit 7 of each is always clear, so a
	// would-be sync word can never appear inside it.
	if (b[6]|b[7]|b[8]|b[9])&0x80 != 0 {
		return 0
	}
	n := 10 + int(syncsafe(b[6:10]))
	if b[5]&0x10 != 0 {
		n += 10 // v2.4 footer
	}
	return n
}

// id3v2Len is the total length of the ID3v2 tag(s) at the head of b. Tags
// chain: a tagger that appends rather than rewrites leaves two.
func id3v2Len(b []byte) int {
	total := 0
	for {
		n := id3v2One(b[total:])
		if n == 0 || n > len(b)-total {
			return total
		}
		total += n
	}
}

// tailTagLen is the ID3v1 and/or APE tag at the end of b, which bounds the
// audio on that side.
func tailTagLen(b []byte) int {
	total := 0
	if len(b) >= 128 && string(b[len(b)-128:len(b)-125]) == "TAG" {
		total = 128
	}
	if rest := len(b) - total; rest >= 32 {
		f := b[rest-32 : rest]
		// APEv2 footer: "APETAGEX", version, tag size (items + footer, not
		// the header), item COUNT, then the global flags at +20 — bit 31
		// "a header precedes the items", bit 29 "this block is the header".
		// Same layout core/codecs/pvmp3/mp3_frame.c reads.
		if string(f[:8]) == "APETAGEX" {
			size := binary.LittleEndian.Uint32(f[12:16])
			flags := binary.LittleEndian.Uint32(f[20:24])
			// Bit 29 says this block is the tag's HEADER, so the items follow
			// it and nothing ends here.
			if flags&0x20000000 == 0 {
				whole := int(size)
				if flags&0x80000000 != 0 {
					whole += 32 // a header precedes the items as well
				}
				if whole >= 32 && whole <= rest {
					total += whole
				}
			}
		}
	}
	return total
}

// textFrames maps the ID3 frame ids we keep to ffprobe's key names. v2.2's
// three-character spellings are included: they are rare but they are what a
// decade-old library is full of.
var textFrames = map[string]string{
	"TIT2": "title", "TT2": "title",
	"TPE1": "artist", "TP1": "artist",
	"TPE2": "album_artist", "TP2": "album_artist",
	"TALB": "album", "TAL": "album",
	"TCON": "genre", "TCO": "genre",
	"TRCK": "track", "TRK": "track",
	"TPOS": "disc", "TPA": "disc",
	"TDRC": "date", "TYER": "date", "TYE": "date",
	"TCOM": "composer", "TCM": "composer",
}

func parseID3v2(b []byte, m *Meta) {
	if len(b) < 10 || string(b[:3]) != "ID3" {
		return
	}
	major := b[3]
	flags := b[5]
	// Unsynchronisation rewrites every 0xff 0x00 pair inside the tag, so the
	// frame sizes no longer describe the bytes on disk. Reversing it would
	// mean rebuilding the whole tag; the firmware does not either, and the
	// two readers agreeing is the point. Treat the tag as opaque.
	if flags&0x80 != 0 {
		return
	}
	size := int(syncsafe(b[6:10]))
	if size > maxTagBytes {
		return
	}
	p, limit := 10, 10+size
	if limit > len(b) {
		limit = len(b)
	}

	if flags&0x40 != 0 { // extended header
		if p+4 > limit {
			return
		}
		ext := int(binary.BigEndian.Uint32(b[p : p+4]))
		if major >= 4 {
			// v2.4 states a sync-safe size that INCLUDES the field itself.
			ext = int(syncsafe(b[p : p+4]))
			ext -= 4
		}
		p += 4
		if ext < 0 || p+ext > limit {
			return
		}
		p += ext
	}

	idLen, hdrLen := 4, 10
	if major == 2 {
		idLen, hdrLen = 3, 6
	}

	for p+hdrLen <= limit {
		if b[p] == 0 { // padding: the frames are done
			return
		}
		id := string(b[p : p+idLen])
		var fsize int
		var fflags uint16
		if major == 2 {
			fsize = int(b[p+3])<<16 | int(b[p+4])<<8 | int(b[p+5])
		} else if major >= 4 {
			fsize = int(syncsafe(b[p+4 : p+8]))
			fflags = binary.BigEndian.Uint16(b[p+8 : p+10])
		} else {
			fsize = int(binary.BigEndian.Uint32(b[p+4 : p+8]))
			fflags = binary.BigEndian.Uint16(b[p+8 : p+10])
		}
		p += hdrLen
		if fsize <= 0 || fsize > limit-p || fsize > maxFrameBytes {
			return
		}
		body := b[p : p+fsize]
		p += fsize

		// Compressed, encrypted or per-frame unsynchronised bodies are not the
		// bytes the size describes.
		opaque := uint16(0x00c0)
		if major >= 4 {
			opaque = 0x000e
		}
		if fflags&opaque != 0 {
			continue
		}
		// v2.4's data-length indicator is four extra bytes in front.
		if major >= 4 && fflags&0x0001 != 0 {
			if len(body) <= 4 {
				continue
			}
			body = body[4:]
		}

		switch {
		case id == "APIC" || id == "PIC":
			if pic, ok := parseAPIC(id, body); ok {
				m.Pictures = append(m.Pictures, pic)
			}
		case id == "TXXX" || id == "TXX":
			// description + value, not a plain text frame. Out of scope.
		default:
			key, ok := textFrames[id]
			if !ok {
				continue
			}
			v := decodeText(body)
			if v == "" {
				continue
			}
			if key == "genre" {
				v = resolveTCON(v)
			}
			// First wins, so TPE1 beats a TPE2 that came before it only by
			// the scanner's key order, not by tag order.
			if _, seen := m.Tags[key]; !seen {
				m.Tags[key] = v
			}
		}
	}
}

// decodeText turns a text frame body (encoding byte, then the text) into a Go
// string, taking the first NUL-separated value — v2.4 allows several and that
// is what ffprobe reports.
func decodeText(body []byte) string {
	if len(body) < 1 {
		return ""
	}
	enc, text := body[0], body[1:]
	switch enc {
	case 0: // ISO-8859-1
		if i := indexByte(text, 0); i >= 0 {
			text = text[:i]
		}
		var sb strings.Builder
		for _, c := range text {
			sb.WriteRune(rune(c))
		}
		return strings.TrimRight(sb.String(), "\x00")
	case 3: // UTF-8
		if i := indexByte(text, 0); i >= 0 {
			text = text[:i]
		}
		return strings.ToValidUTF8(string(text), "")
	case 1, 2: // UTF-16, with a BOM or big-endian without one
		big := enc == 2
		if enc == 1 && len(text) >= 2 {
			switch {
			case text[0] == 0xff && text[1] == 0xfe:
				big, text = false, text[2:]
			case text[0] == 0xfe && text[1] == 0xff:
				big, text = true, text[2:]
			}
			// A missing BOM is malformed; UTF-16LE is what the taggers that
			// get this wrong write, so assume it rather than drop the frame.
		}
		units := make([]uint16, 0, len(text)/2)
		for i := 0; i+1 < len(text); i += 2 {
			var u uint16
			if big {
				u = uint16(text[i])<<8 | uint16(text[i+1])
			} else {
				u = uint16(text[i+1])<<8 | uint16(text[i])
			}
			if u == 0 {
				break
			}
			units = append(units, u)
		}
		return strings.ToValidUTF8(string(utf16.Decode(units)), "")
	}
	return ""
}

func indexByte(b []byte, c byte) int {
	for i := range b {
		if b[i] == c {
			return i
		}
	}
	return -1
}

func parseAPIC(id string, body []byte) (flac.Picture, bool) {
	if len(body) < 2 {
		return flac.Picture{}, false
	}
	enc := body[0]
	p := 1
	var mime string
	if id == "PIC" {
		// v2.2: a three-character format code ("JPG", "PNG") instead of a MIME
		// type.
		if len(body) < 5 {
			return flac.Picture{}, false
		}
		switch strings.ToUpper(string(body[1:4])) {
		case "PNG":
			mime = "image/png"
		default:
			mime = "image/jpeg"
		}
		p = 4
	} else {
		i := indexByte(body[p:], 0)
		if i < 0 {
			return flac.Picture{}, false
		}
		mime = string(body[p : p+i])
		p += i + 1
	}
	if p >= len(body) {
		return flac.Picture{}, false
	}
	pictype := uint32(body[p])
	p++
	// The description, terminated by a NUL in the frame's encoding.
	if enc == 1 || enc == 2 {
		for p+1 < len(body) && !(body[p] == 0 && body[p+1] == 0) {
			p += 2
		}
		p += 2
	} else {
		i := indexByte(body[p:], 0)
		if i < 0 {
			return flac.Picture{}, false
		}
		p += i + 1
	}
	if p >= len(body) {
		return flac.Picture{}, false
	}
	data := make([]byte, len(body)-p)
	copy(data, body[p:])
	return flac.Picture{Type: pictype, MIME: mime, Data: data}, true
}

// resolveTCON turns TCON's numeric forms into genre names, which is what
// ffprobe reports and what the firmware shows: "17" and "(17)" are Rock,
// "(RX)" is Remix, "(CR)" is Cover, and real text after a code wins over it.
func resolveTCON(s string) string {
	if s == "" {
		return s
	}
	if s[0] == '(' && len(s) >= 2 && s[1] != '(' {
		close := strings.IndexByte(s, ')')
		if close > 1 {
			code, tail := s[1:close], s[close+1:]
			if tail != "" {
				return tail
			}
			switch code {
			case "RX":
				return "Remix"
			case "CR":
				return "Cover"
			}
			if n, ok := allDigits(code); ok && n < len(v1Genres) {
				return v1Genres[n]
			}
		}
		return s
	}
	if n, ok := allDigits(s); ok && n < len(v1Genres) {
		return v1Genres[n]
	}
	return s
}

func allDigits(s string) (int, bool) {
	if s == "" || len(s) > 5 {
		return 0, false
	}
	n := 0
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
		n = n*10 + int(s[i]-'0')
	}
	return n, true
}

// ---------- the stream ----------------------------------------------------

var (
	bitrateV1 = [16]int{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}
	bitrateV2 = [16]int{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}
	sampleRT  = [4][3]int{{11025, 12000, 8000}, {}, {22050, 24000, 16000}, {44100, 48000, 32000}}
)

type header struct {
	version  int // raw header field: 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5
	rate     int
	bitrate  int // bits/s
	frameLen int
	spf      int
	channels int
	sideInfo int
}

// parseHeader reads a 4-byte MPEG header, accepting Layer III only.
func parseHeader(b []byte) (header, bool) {
	if len(b) < 4 || b[0] != 0xff || b[1]&0xe0 != 0xe0 {
		return header{}, false
	}
	ver := int(b[1]>>3) & 3
	layer := int(b[1]>>1) & 3
	brIx := int(b[2] >> 4)
	srIx := int(b[2]>>2) & 3
	pad := int(b[2]>>1) & 1
	mode := int(b[3] >> 6)
	if ver == 1 || layer != 1 || brIx == 0 || brIx == 15 || srIx == 3 {
		return header{}, false
	}
	h := header{version: ver, rate: sampleRT[ver][srIx]}
	if ver == 3 {
		h.bitrate = bitrateV1[brIx] * 1000
		h.spf = 1152
		h.frameLen = 144*h.bitrate/h.rate + pad
	} else {
		h.bitrate = bitrateV2[brIx] * 1000
		h.spf = 576
		h.frameLen = 72*h.bitrate/h.rate + pad
	}
	h.channels = 2
	if mode == 3 {
		h.channels = 1
	}
	if ver == 3 {
		h.sideInfo = 32
		if h.channels == 1 {
			h.sideInfo = 17
		}
	} else {
		h.sideInfo = 17
		if h.channels == 1 {
			h.sideInfo = 9
		}
	}
	if h.frameLen < 4 || h.frameLen > 1441 {
		return header{}, false
	}
	return h, true
}

// scanStream finds the first frame between start and end and works out the
// rate and the length. Mirrors core/codecs/pvmp3/mp3_frame.c's mp3_stream_scan.
func scanStream(b []byte, start, end int, m *Meta) error {
	const scanLimit = 64 << 10
	limit := start + scanLimit
	if limit > end {
		limit = end
	}
	first := -1
	var h header
	for i := start; i+4 <= limit; i++ {
		if b[i] != 0xff {
			continue
		}
		hh, ok := parseHeader(b[i:])
		if !ok {
			continue
		}
		// Two-header validation: one sync word is worth nothing, since cover
		// art and APE values are full of 0xff bytes. The frame's own length
		// has to land on another header describing the same stream.
		next := i + hh.frameLen
		if next+4 > end {
			first, h = i, hh // the file holds exactly one frame
			break
		}
		if h2, ok2 := parseHeader(b[next:]); ok2 &&
			h2.version == hh.version && h2.rate == hh.rate && h2.channels == hh.channels {
			first, h = i, hh
			break
		}
	}
	if first < 0 {
		return ErrNotMP3
	}
	m.SampleRate = uint32(h.rate)

	frames, byteCount, delay, padding, isVBR, tagged := parseVBRTag(b[first:min(first+256, end)], h)
	m.VBR, m.Tagged = isVBR, tagged

	if tagged && frames > 0 {
		raw := uint64(frames) * uint64(h.spf)
		trim := uint64(delay) + uint64(padding)
		if raw > trim {
			m.TotalSamples = raw - trim
		}
		return nil
	}

	// No usable tag: estimate from the first frame's bitrate over the audio
	// region. Exact for constant bitrate, which is what an untagged file
	// almost always is.
	audio := end - first
	if byteCount > 0 && byteCount <= audio {
		audio = byteCount
	}
	if h.bitrate > 0 && audio > 0 {
		m.TotalSamples = uint64(audio) * 8 * uint64(h.rate) / uint64(h.bitrate)
	}
	return nil
}

// parseVBRTag reads the Xing/Info or VBRI header the first frame may carry.
func parseVBRTag(f []byte, h header) (frames, byteCount, delay, padding int, vbr, tagged bool) {
	off := 4 + h.sideInfo
	if off+8 <= len(f) && (string(f[off:off+4]) == "Xing" || string(f[off:off+4]) == "Info") {
		tagged = true
		vbr = string(f[off:off+4]) == "Xing"
		flags := binary.BigEndian.Uint32(f[off+4 : off+8])
		p := off + 8
		if flags&1 != 0 {
			if p+4 > len(f) {
				return
			}
			frames = int(binary.BigEndian.Uint32(f[p : p+4]))
			p += 4
		}
		if flags&2 != 0 {
			if p+4 > len(f) {
				return
			}
			byteCount = int(binary.BigEndian.Uint32(f[p : p+4]))
			p += 4
		}
		if flags&4 != 0 {
			p += 100
		}
		if flags&8 != 0 {
			p += 4
		}
		// The LAME extension: a 9-byte encoder string, then at +21 two packed
		// 12-bit fields — the encoder delay and the end padding ffprobe
		// subtracts from the duration. Three writers stamp it and ffmpeg's own
		// reader honours all three, so we do too: "LAME", "Lavc" when
		// libavcodec encoded, "Lavf" when libavformat only remuxed. Same list
		// as core/codecs/pvmp3/mp3_frame.c.
		if p+24 <= len(f) && (string(f[p:p+4]) == "LAME" ||
			string(f[p:p+4]) == "Lavc" || string(f[p:p+4]) == "Lavf") {
			g := f[p+21 : p+24]
			delay = int(g[0])<<4 | int(g[1])>>4
			padding = int(g[1]&0x0f)<<8 | int(g[2])
		}
		return
	}
	if off = 4 + 32; off+26 <= len(f) && string(f[off:off+4]) == "VBRI" {
		tagged, vbr = true, true
		byteCount = int(binary.BigEndian.Uint32(f[off+10 : off+14]))
		frames = int(binary.BigEndian.Uint32(f[off+14 : off+18]))
	}
	return
}
