package id3

import (
	"encoding/binary"
	"testing"
)

// ---------- a tag builder -------------------------------------------------

type builder struct {
	buf   []byte
	major byte
}

func newTag(major byte, flags byte) *builder {
	b := &builder{major: major}
	b.buf = append(b.buf, 'I', 'D', '3', major, 0, flags, 0, 0, 0, 0)
	return b
}

func (b *builder) frame(id string, body []byte, fflags uint16) *builder {
	if b.major == 2 {
		n := len(body)
		b.buf = append(b.buf, id[0], id[1], id[2],
			byte(n>>16), byte(n>>8), byte(n))
	} else {
		var sz [4]byte
		n := uint32(len(body))
		if b.major >= 4 {
			sz = [4]byte{byte(n>>21) & 0x7f, byte(n>>14) & 0x7f, byte(n>>7) & 0x7f, byte(n) & 0x7f}
		} else {
			binary.BigEndian.PutUint32(sz[:], n)
		}
		b.buf = append(b.buf, id[0], id[1], id[2], id[3])
		b.buf = append(b.buf, sz[:]...)
		b.buf = append(b.buf, byte(fflags>>8), byte(fflags))
	}
	b.buf = append(b.buf, body...)
	return b
}

func (b *builder) text(id string, enc byte, text []byte) *builder {
	return b.frame(id, append([]byte{enc}, text...), 0)
}

func (b *builder) ascii(id, s string) *builder {
	return b.text(id, 0, []byte(s))
}

func (b *builder) raw(p []byte) *builder {
	b.buf = append(b.buf, p...)
	return b
}

// done closes the tag and appends `frames` MPEG-1 128 kbps 44.1 kHz stereo
// frames, so the result is a file Read() will accept.
func (b *builder) done(frames int) []byte {
	n := uint32(len(b.buf) - 10)
	b.buf[6] = byte(n>>21) & 0x7f
	b.buf[7] = byte(n>>14) & 0x7f
	b.buf[8] = byte(n>>7) & 0x7f
	b.buf[9] = byte(n) & 0x7f
	return append(b.buf, mpegFrames(frames)...)
}

func mpegFrames(n int) []byte {
	out := make([]byte, 0, n*417)
	for i := 0; i < n; i++ {
		f := make([]byte, 417)
		f[0], f[1], f[2], f[3] = 0xff, 0xfb, 0x90, 0x00
		out = append(out, f...)
	}
	return out
}

func read(t *testing.T, b []byte) *Meta {
	t.Helper()
	m, err := Read(b)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	return m
}

// ---------- text frames ---------------------------------------------------

func TestV23TextFrames(t *testing.T) {
	m := read(t, newTag(3, 0).
		ascii("TIT2", "Eroica").
		ascii("TPE1", "Czech National Symphony Orchestra").
		ascii("TALB", "Symphony No. 3").
		ascii("TCON", "Classical").
		ascii("TRCK", "7/12").
		ascii("TPOS", "1/2").
		ascii("TYER", "1804").
		done(40))

	want := map[string]string{
		"title":  "Eroica",
		"artist": "Czech National Symphony Orchestra",
		"album":  "Symphony No. 3",
		"genre":  "Classical",
		"track":  "7/12",
		"disc":   "1/2",
		"date":   "1804",
	}
	for k, v := range want {
		if got := m.Tags[k]; got != v {
			t.Errorf("Tags[%q] = %q, want %q", k, got, v)
		}
	}
	if m.SampleRate != 44100 {
		t.Errorf("SampleRate = %d, want 44100", m.SampleRate)
	}
}

// v2.2's three-character ids and 6-byte frame headers.
func TestV22Frames(t *testing.T) {
	m := read(t, newTag(2, 0).
		ascii("TT2", "Old Tag").
		ascii("TP1", "Someone").
		ascii("TAL", "An Album").
		ascii("TRK", "3").
		done(40))
	if m.Tags["title"] != "Old Tag" || m.Tags["artist"] != "Someone" ||
		m.Tags["album"] != "An Album" || m.Tags["track"] != "3" {
		t.Errorf("v2.2 tags = %v", m.Tags)
	}
}

// v2.4 sizes are SYNC-SAFE. A body of 200 bytes reads as 200 one way and 328
// the other, so a parser that gets it wrong loses every frame after it.
func TestV24SyncSafeFrameSizes(t *testing.T) {
	long := make([]byte, 200)
	for i := range long {
		long[i] = 'a'
	}
	m := read(t, newTag(4, 0).
		text("TIT2", 0, long).
		ascii("TPE1", "After The Long One").
		ascii("TDRC", "2021-05-01").
		done(40))
	if len(m.Tags["title"]) != 200 {
		t.Errorf("title is %d bytes, want 200", len(m.Tags["title"]))
	}
	if m.Tags["artist"] != "After The Long One" {
		t.Errorf("the frame after a 200-byte one was lost: %v", m.Tags)
	}
	if m.Tags["date"] != "2021-05-01" {
		t.Errorf("date = %q", m.Tags["date"])
	}
}

func TestV24DataLengthIndicator(t *testing.T) {
	body := append([]byte{0, 0, 0, 10, 0}, []byte("Indicated")...)
	m := read(t, newTag(4, 0).frame("TIT2", body, 0x0001).done(40))
	if m.Tags["title"] != "Indicated" {
		t.Errorf("title = %q, want %q", m.Tags["title"], "Indicated")
	}
}

func TestTextEncodings(t *testing.T) {
	cases := []struct {
		name string
		enc  byte
		in   []byte
		want string
	}{
		{"latin1", 0, []byte{'B', 'e', 'y', 'o', 'n', 'c', 0xe9}, "Beyoncé"},
		{"utf8", 3, []byte("Björk"), "Björk"},
		{"utf16le-bom", 1, []byte{0xff, 0xfe, 'B', 0, 'j', 0, 0xf6, 0, 'r', 0, 'k', 0}, "Björk"},
		{"utf16be-bom", 1, []byte{0xfe, 0xff, 0, 'B', 0, 'j', 0, 0xf6, 0, 'r', 0, 'k'}, "Björk"},
		{"utf16be-nobom", 2, []byte{0, 'B', 0, 'j', 0, 0xf6, 0, 'r', 0, 'k'}, "Björk"},
		// U+1D11E, a surrogate pair.
		{"surrogate", 1, []byte{0xff, 0xfe, 0x34, 0xd8, 0x1e, 0xdd}, "\U0001D11E"},
		// v2.4 allows several NUL-separated values; ffprobe reports the first.
		{"multi-value", 0, []byte{'F', 'i', 'r', 's', 't', 0, 'S', 'e', 'c'}, "First"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			m := read(t, newTag(3, 0).text("TIT2", c.enc, c.in).done(40))
			if m.Tags["title"] != c.want {
				t.Errorf("title = %q, want %q", m.Tags["title"], c.want)
			}
		})
	}
}

// TCON's numeric forms, resolved against the ID3v1 table exactly as ffprobe
// and the firmware do.
func TestTCONGenres(t *testing.T) {
	cases := []struct{ in, want string }{
		{"17", "Rock"},
		{"(17)", "Rock"},
		{"(17)Rock", "Rock"},
		{"(RX)", "Remix"},
		{"(CR)", "Cover"},
		{"Rock", "Rock"},
		{"(255)", "(255)"},
		{"(0)", "Blues"},
		{"0", "Blues"},
		{"(17)Progressive Rock", "Progressive Rock"},
	}
	for _, c := range cases {
		m := read(t, newTag(3, 0).ascii("TCON", c.in).done(40))
		if got := m.Tags["genre"]; got != c.want {
			t.Errorf("TCON %q -> %q, want %q", c.in, got, c.want)
		}
	}
}

// ---------- awkward tags ---------------------------------------------------

func TestExtendedHeaderIsSkipped(t *testing.T) {
	// v2.3: the size is what FOLLOWS the field.
	b := newTag(3, 0x40).raw([]byte{0, 0, 0, 6, 0, 0, 0, 0, 0, 0}).
		ascii("TIT2", "Past The Extended Header")
	if got := read(t, b.done(40)).Tags["title"]; got != "Past The Extended Header" {
		t.Errorf("v2.3 extended header: title = %q", got)
	}
	// v2.4: a sync-safe size that INCLUDES the field.
	b = newTag(4, 0x40).raw([]byte{0, 0, 0, 6, 1, 0}).
		ascii("TIT2", "Past The v2.4 One")
	if got := read(t, b.done(40)).Tags["title"]; got != "Past The v2.4 One" {
		t.Errorf("v2.4 extended header: title = %q", got)
	}
}

func TestUnsynchronisedTagIsOpaque(t *testing.T) {
	m := read(t, newTag(3, 0x80).ascii("TIT2", "Unsynchronised").done(40))
	if m.Tags["title"] != "" {
		t.Errorf("an unsynchronised tag should yield no text, got %q", m.Tags["title"])
	}
	if m.SampleRate != 44100 || m.DurationSeconds() == 0 {
		t.Errorf("the stream behind it must still be read: %d Hz, %d s",
			m.SampleRate, m.DurationSeconds())
	}
}

func TestOversizedFrameStopsTheWalk(t *testing.T) {
	b := newTag(3, 0)
	b.buf = append(b.buf, 'T', 'I', 'T', '2', 0, 0, 0xff, 0xff, 0, 0)
	m := read(t, b.done(40))
	if m.Tags["title"] != "" {
		t.Errorf("a frame longer than the tag should be ignored, got %q", m.Tags["title"])
	}
}

func TestPaddingEndsTheWalk(t *testing.T) {
	b := newTag(3, 0).ascii("TIT2", "Before Padding")
	b.buf = append(b.buf, make([]byte, 256)...)
	if got := read(t, b.done(40)).Tags["title"]; got != "Before Padding" {
		t.Errorf("title = %q", got)
	}
}

func TestNotAnMP3(t *testing.T) {
	if _, err := Read(make([]byte, 8192)); err != ErrNotMP3 {
		t.Errorf("a file of zeroes: err = %v, want ErrNotMP3", err)
	}
	junk := make([]byte, 8192)
	for i := range junk {
		junk[i] = 0xff
	}
	if _, err := Read(junk); err != ErrNotMP3 {
		t.Errorf("a file of 0xff: err = %v, want ErrNotMP3", err)
	}
	if _, err := Read(nil); err != ErrNotMP3 {
		t.Errorf("an empty file: err = %v, want ErrNotMP3", err)
	}
}

// ---------- APIC -----------------------------------------------------------

func TestAPICFrontCover(t *testing.T) {
	art := []byte{0xde, 0xad, 0xbe, 0xef, 0x01, 0x02}
	body := []byte{0} // encoding
	body = append(body, []byte("image/jpeg")...)
	body = append(body, 0, 3) // NUL, picture type 3 = front cover
	body = append(body, []byte("cover")...)
	body = append(body, 0)
	body = append(body, art...)

	other := []byte{0}
	other = append(other, []byte("image/png")...)
	other = append(other, 0, 5) // type 5 = leaflet
	other = append(other, 0)    // empty description
	other = append(other, 0x89, 'P')

	m := read(t, newTag(3, 0).frame("APIC", other, 0).frame("APIC", body, 0).done(40))
	if len(m.Pictures) != 2 {
		t.Fatalf("got %d pictures, want 2", len(m.Pictures))
	}
	fc := m.FrontCover()
	if fc == nil || fc.MIME != "image/jpeg" || string(fc.Data) != string(art) {
		t.Fatalf("FrontCover = %+v, want the type-3 JPEG", fc)
	}
}

func TestAPICSkippedBySizeDoesNotSwallowTheTitle(t *testing.T) {
	art := make([]byte, 4096)
	body := append([]byte{0}, []byte("image/jpeg")...)
	body = append(body, 0, 3, 0)
	body = append(body, art...)
	m := read(t, newTag(3, 0).frame("APIC", body, 0).ascii("TIT2", "After The Cover").done(40))
	if m.Tags["title"] != "After The Cover" {
		t.Errorf("title = %q", m.Tags["title"])
	}
}

// ---------- duration -------------------------------------------------------

// xingFrame builds a first frame carrying a Xing/Info header. Returns the
// whole 417-byte frame.
func xingFrame(sig string, frames, byteCount uint32, toc bool, lame bool, delay, padding uint32) []byte {
	f := make([]byte, 417)
	f[0], f[1], f[2], f[3] = 0xff, 0xfb, 0x90, 0x00
	o := 4 + 32
	copy(f[o:], sig)
	o += 4
	var flags uint32 = 1 | 2 | 8
	if toc {
		flags |= 4
	}
	binary.BigEndian.PutUint32(f[o:], flags)
	o += 4
	binary.BigEndian.PutUint32(f[o:], frames)
	o += 4
	binary.BigEndian.PutUint32(f[o:], byteCount)
	o += 4
	if toc {
		for i := 0; i < 100; i++ {
			f[o+i] = byte(i * 256 / 100)
		}
		o += 100
	}
	binary.BigEndian.PutUint32(f[o:], 70) // quality
	o += 4
	if lame {
		copy(f[o:], "LAME3.100")
		g := f[o+21:]
		g[0] = byte(delay >> 4)
		g[1] = byte((delay&0x0f)<<4 | (padding>>8)&0x0f)
		g[2] = byte(padding)
	}
	return f
}

func TestDurationFromXing(t *testing.T) {
	// 100 frames of 1152 samples, less 576 of delay and 1404 of padding:
	// 115200 - 1980 = 113220 samples at 44100 = 2.567 s -> 2.
	file := append(xingFrame("Xing", 100, 417*101, true, true, 576, 1404), mpegFrames(100)...)
	m := read(t, file)
	if !m.Tagged || !m.VBR {
		t.Errorf("Tagged=%v VBR=%v, want true/true", m.Tagged, m.VBR)
	}
	if m.TotalSamples != 100*1152-576-1404 {
		t.Errorf("TotalSamples = %d, want %d", m.TotalSamples, 100*1152-576-1404)
	}
	if m.DurationSeconds() != 2 {
		t.Errorf("DurationSeconds = %d, want 2", m.DurationSeconds())
	}

	// "Info" is the same header on a constant-bitrate file.
	m = read(t, append(xingFrame("Info", 100, 417*101, true, true, 576, 0), mpegFrames(100)...))
	if !m.Tagged || m.VBR {
		t.Errorf("Info: Tagged=%v VBR=%v, want true/false", m.Tagged, m.VBR)
	}

	// No LAME extension: nothing is trimmed.
	m = read(t, append(xingFrame("Xing", 100, 417*101, true, false, 0, 0), mpegFrames(100)...))
	if m.TotalSamples != 100*1152 {
		t.Errorf("without a LAME tag TotalSamples = %d, want %d", m.TotalSamples, 100*1152)
	}
}

// LAME, Lavc and Lavf all stamp the delay/padding extension, and ffmpeg's own
// reader honours all three. Missing one means a remuxed file's duration is
// long by the encoder delay, which is a one-second index difference on a short
// track and a parity failure against ffprobe.
func TestEncoderStrings(t *testing.T) {
	for _, enc := range []string{"LAME", "Lavc", "Lavf"} {
		f := xingFrame("Xing", 100, 417*101, true, true, 576, 1404)
		copy(f[4+32+4+4+4+4+100+4:], enc) // over the 9-byte encoder string
		m := read(t, append(f, mpegFrames(100)...))
		if m.TotalSamples != 100*1152-576-1404 {
			t.Errorf("%s: TotalSamples = %d, want %d (delay/padding not read)",
				enc, m.TotalSamples, 100*1152-576-1404)
		}
	}
	// Anything else is not the extension and must not be read as one.
	f := xingFrame("Xing", 100, 417*101, true, true, 576, 1404)
	copy(f[4+32+4+4+4+4+100+4:], "Xxxx")
	if m := read(t, append(f, mpegFrames(100)...)); m.TotalSamples != 100*1152 {
		t.Errorf("an unknown encoder string was read as LAME: %d", m.TotalSamples)
	}
}

func TestDurationFromVBRI(t *testing.T) {
	f := make([]byte, 417)
	f[0], f[1], f[2], f[3] = 0xff, 0xfb, 0x90, 0x00
	o := 4 + 32
	copy(f[o:], "VBRI")
	binary.BigEndian.PutUint32(f[o+10:], 417*41) // bytes
	binary.BigEndian.PutUint32(f[o+14:], 40)     // frames
	m := read(t, append(f, mpegFrames(40)...))
	if !m.Tagged || !m.VBR || m.TotalSamples != 40*1152 {
		t.Errorf("VBRI: Tagged=%v VBR=%v TotalSamples=%d", m.Tagged, m.VBR, m.TotalSamples)
	}
}

func TestDurationCBREstimate(t *testing.T) {
	// 40 frames of 417 bytes at 128 kbps, 44.1 kHz: exact for CBR.
	m := read(t, mpegFrames(40))
	if m.Tagged {
		t.Errorf("an untagged stream must not report Tagged")
	}
	want := uint64(40*417) * 8 * 44100 / 128000
	if m.TotalSamples != want {
		t.Errorf("TotalSamples = %d, want %d", m.TotalSamples, want)
	}
}

// APEv2 global flags, from the spec.
const (
	apeHasHeader = 0x80000000
	apeIsHeader  = 0x20000000
)

// apeFooter is a spec-correct 32-byte APEv2 footer: "APETAGEX", version, tag
// size, ITEM COUNT, then the global flags at +20 — the same layout
// core/codecs/pvmp3/mp3_frame.c reads, and the same one ffmpeg's apetag.c and
// mutagen's apev2.py write.
func apeFooter(size, items, flags uint32) []byte {
	f := make([]byte, 32)
	copy(f, "APETAGEX")
	binary.LittleEndian.PutUint32(f[8:], 2000)
	binary.LittleEndian.PutUint32(f[12:], size)
	binary.LittleEndian.PutUint32(f[16:], items)
	binary.LittleEndian.PutUint32(f[20:], flags)
	return f
}

// The tail tags bound the audio: counting them as audio inflates an untagged
// file's estimated duration.
func TestTailTagsBoundTheAudio(t *testing.T) {
	bare := read(t, mpegFrames(40))
	audio := mpegFrames(40)

	withV1 := append(append([]byte{}, audio...), make([]byte, 128)...)
	copy(withV1[len(audio):], "TAG")
	if got := read(t, withV1); got.TotalSamples != bare.TotalSamples {
		t.Errorf("ID3v1 tail changed the duration: %d vs %d", got.TotalSamples, bare.TotalSamples)
	}

	// A footerless tag: 32 items + a 32-byte footer = 64 bytes on disk.
	withAPE := append(append([]byte{}, audio...), make([]byte, 32)...)
	withAPE = append(withAPE, apeFooter(64, 3, 0)...)
	if got := read(t, withAPE); got.TotalSamples != bare.TotalSamples {
		t.Errorf("APE tail changed the duration: %d vs %d", got.TotalSamples, bare.TotalSamples)
	}

	// With a header, which is what every current tagger writes: the tag is
	// its stated size PLUS the 32-byte header in front of the items. Reading
	// the flags at +16 (the item count) or testing bit 29 leaves those 32
	// bytes in the audio region.
	hdr := append([]byte{}, audio...)
	hdr = append(hdr, apeFooter(64, 3, apeHasHeader|apeIsHeader)...) // the header
	hdr = append(hdr, make([]byte, 32)...)                           // the items
	hdr = append(hdr, apeFooter(64, 3, apeHasHeader)...)             // the footer
	if got := read(t, hdr); got.TotalSamples != bare.TotalSamples {
		t.Errorf("APE tag with a header changed the duration: %d vs %d (the 32-byte header was counted as audio)",
			got.TotalSamples, bare.TotalSamples)
	}

	// An item count that happens to have bit 29 set must not read as flags.
	count := append(append([]byte{}, audio...), make([]byte, 32)...)
	count = append(count, apeFooter(64, 0x20000000, 0)...)
	if got := read(t, count); got.TotalSamples != bare.TotalSamples {
		t.Errorf("the item count was read as the flags: %d vs %d", got.TotalSamples, bare.TotalSamples)
	}

	// Both tags, in the order a Windows tagger leaves them: APE, then ID3v1.
	both := append([]byte{}, audio...)
	both = append(both, apeFooter(64, 3, apeHasHeader|apeIsHeader)...)
	both = append(both, make([]byte, 32)...)
	both = append(both, apeFooter(64, 3, apeHasHeader)...)
	v1 := make([]byte, 128)
	copy(v1, "TAG")
	both = append(both, v1...)
	if got := read(t, both); got.TotalSamples != bare.TotalSamples {
		t.Errorf("APE + ID3v1 tail changed the duration: %d vs %d", got.TotalSamples, bare.TotalSamples)
	}
}

// Two-header validation: a lone 0xff that looks like a sync word is not a
// frame, and the scan must step over it to the real audio.
func TestJunkPrefixIsSkipped(t *testing.T) {
	junk := make([]byte, 1000)
	for i := range junk {
		if i%3 == 0 {
			junk[i] = 0xff
		} else {
			junk[i] = 0xfb
		}
	}
	m := read(t, append(junk, mpegFrames(40)...))
	if m.SampleRate != 44100 {
		t.Errorf("SampleRate = %d after a junk prefix", m.SampleRate)
	}
	bare := read(t, mpegFrames(40))
	if m.TotalSamples != bare.TotalSamples {
		t.Errorf("junk counted as audio: %d vs %d", m.TotalSamples, bare.TotalSamples)
	}
}

func TestNilMetaAccessors(t *testing.T) {
	var m *Meta
	if m.DurationSeconds() != 0 || m.Tag("title") != "" || m.FrontCover() != nil {
		t.Error("a nil Meta must answer zero, not panic")
	}
}
