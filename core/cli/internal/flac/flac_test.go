package flac

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// --- synthetic file assembly -------------------------------------------------

type rawBlock struct {
	typ  byte
	body []byte
}

// buildFLAC glues a marker onto raw metadata blocks. The last block gets the
// last-block flag; nothing else is appended, so a parser that reads past it is
// reading off the end of the slice.
func buildFLAC(blocks ...rawBlock) []byte {
	out := []byte("fLaC")
	for i, b := range blocks {
		out = appendBlock(out, b.typ, i == len(blocks)-1, b.body)
	}
	return out
}

func streamInfoBody(rate uint32, ch, bps uint8, total uint64) []byte {
	return buildStreamInfo(StreamInfo{rate, ch, bps, total}, 4096)
}

// vorbisBody writes comments verbatim, so a test can hand it a malformed entry.
func vorbisBody(vendor string, comments ...string) []byte {
	var b []byte
	b = appendU32LE(b, uint32(len(vendor)))
	b = append(b, vendor...)
	b = appendU32LE(b, uint32(len(comments)))
	for _, c := range comments {
		b = appendU32LE(b, uint32(len(c)))
		b = append(b, c...)
	}
	return b
}

func pictureBody(typ uint32, mime, desc string, data []byte) []byte {
	var b []byte
	b = appendU32BE(b, typ)
	b = appendU32BE(b, uint32(len(mime)))
	b = append(b, mime...)
	b = appendU32BE(b, uint32(len(desc)))
	b = append(b, desc...)
	for i := 0; i < 4; i++ {
		b = appendU32BE(b, 0)
	}
	b = appendU32BE(b, uint32(len(data)))
	return append(b, data...)
}

func mustRead(t *testing.T, b []byte) *Meta {
	t.Helper()
	m, err := Read(bytes.NewReader(b))
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	return m
}

// --- STREAMINFO --------------------------------------------------------------

func TestStreamInfoBitLayout(t *testing.T) {
	cases := []StreamInfo{
		{44100, 2, 16, 0xf_ffff_ffff}, // the 36-bit total_samples ceiling
		{48000, 1, 24, 48000},
		{96000, 8, 32, 0},
		{1, 1, 4, 1},                // the minimum of every field
		{0xfffff, 8, 32, 123456789}, // the 20-bit sample-rate ceiling
	}
	for _, want := range cases {
		m := mustRead(t, buildFLAC(rawBlock{blockStreamInfo, streamInfoBody(want.SampleRate, want.Channels, want.BitsPerSample, want.TotalSamples)}))
		if m.Info != want {
			t.Errorf("round trip: got %+v want %+v", m.Info, want)
		}
	}
}

// TestStreamInfoFixedBytes pins the packing against a hand-computed block so a
// future refactor cannot quietly shift a field by a bit.
func TestStreamInfoFixedBytes(t *testing.T) {
	// sample_rate 44100 = 0xAC44 -> 20 bits 0x0AC44
	// channels-1 = 1, bps-1 = 15, total = 0x0_0002_B110 (176400)
	// word = 0AC44 | 1<<41 ... assembled by hand:
	var w uint64 = 44100<<44 | 1<<41 | 15<<36 | 176400
	body := make([]byte, streamInfoLen)
	binary.BigEndian.PutUint64(body[10:18], w)
	m := mustRead(t, buildFLAC(rawBlock{blockStreamInfo, body}))
	if m.Info.SampleRate != 44100 || m.Info.Channels != 2 || m.Info.BitsPerSample != 16 || m.Info.TotalSamples != 176400 {
		t.Fatalf("got %+v", m.Info)
	}
	if got := m.DurationSeconds(); got != 4 {
		t.Fatalf("DurationSeconds = %d, want 4", got)
	}
}

func TestDurationSecondsTruncatesAndZeroes(t *testing.T) {
	cases := []struct {
		info StreamInfo
		want uint32
	}{
		{StreamInfo{44100, 2, 16, 44100*3 + 44099}, 3}, // truncation, never rounding
		{StreamInfo{44100, 2, 16, 0}, 0},               // unknown length
		{StreamInfo{0, 2, 16, 44100}, 0},               // unknown rate
		{StreamInfo{44100, 2, 16, 44099}, 0},
		{StreamInfo{48000, 2, 24, 48000 * 3600}, 3600},
	}
	for _, c := range cases {
		m := &Meta{Info: c.info}
		if got := m.DurationSeconds(); got != c.want {
			t.Errorf("%+v: got %d want %d", c.info, got, c.want)
		}
	}
	var nilMeta *Meta
	if got := nilMeta.DurationSeconds(); got != 0 {
		t.Errorf("nil meta: got %d", got)
	}
}

// --- VORBIS_COMMENT ----------------------------------------------------------

func TestVorbisComments(t *testing.T) {
	body := vorbisBody("reference libFLAC",
		"TITLE=First",
		"artist=Someone",
		"TiTlE=Second",    // duplicate in different case: last wins
		"ALBUM=a=b=c",     // only the FIRST '=' splits
		"EMPTY=",          //
		"noequals",        // malformed: ignored
		"=novalue",        // empty key: ignored
		"GENRE=Pop, Rock", //
		"COMMENT=caf\xe9", // Latin-1 byte: kept verbatim
	)
	m := mustRead(t, buildFLAC(
		rawBlock{blockStreamInfo, streamInfoBody(44100, 2, 16, 44100)},
		rawBlock{blockVorbisComment, body},
	))
	want := map[string]string{
		"title":   "Second",
		"artist":  "Someone",
		"album":   "a=b=c",
		"empty":   "",
		"genre":   "Pop, Rock",
		"comment": "caf\xe9",
	}
	for k, v := range want {
		if got := m.Tags[k]; got != v {
			t.Errorf("tag %q = %q, want %q", k, got, v)
		}
	}
	if len(m.Tags) != len(want) {
		t.Errorf("tags = %v, want exactly %d entries", m.Tags, len(want))
	}
	if _, ok := m.Tags["noequals"]; ok {
		t.Error("comment without '=' was kept")
	}
	if _, ok := m.Tags[""]; ok {
		t.Error("comment with an empty key was kept")
	}
	if !bytes.Equal([]byte(m.Tags["comment"]), []byte{'c', 'a', 'f', 0xe9}) {
		t.Errorf("invalid UTF-8 value was not preserved byte for byte: % x", m.Tags["comment"])
	}
}

func TestTagFirstNonEmpty(t *testing.T) {
	m := &Meta{Tags: map[string]string{"albumartist": "Band", "artist": "", "album": "Rec"}}
	if got := m.Tag("artist", "albumartist"); got != "Band" {
		t.Errorf("empty artist should fall through: got %q", got)
	}
	if got := m.Tag("album"); got != "Rec" {
		t.Errorf("got %q", got)
	}
	if got := m.Tag("missing", "alsomissing"); got != "" {
		t.Errorf("got %q", got)
	}
	if got := m.Tag(); got != "" {
		t.Errorf("no keys: got %q", got)
	}
}

// --- PICTURE -----------------------------------------------------------------

func TestPictureSelection(t *testing.T) {
	si := rawBlock{blockStreamInfo, streamInfoBody(44100, 2, 16, 44100)}

	// Front cover is not first in the file but wins anyway.
	m := mustRead(t, buildFLAC(si,
		rawBlock{blockPicture, pictureBody(2, "image/png", "artist", []byte("back"))},
		rawBlock{blockPicture, pictureBody(PictureTypeFrontCover, "image/jpeg", "cover", []byte("front"))},
		rawBlock{blockPicture, pictureBody(PictureTypeFrontCover, "image/png", "cover2", []byte("second-front"))},
	))
	if len(m.Pictures) != 3 {
		t.Fatalf("got %d pictures", len(m.Pictures))
	}
	fc := m.FrontCover()
	if fc == nil || string(fc.Data) != "front" || fc.MIME != "image/jpeg" || fc.Type != 3 {
		t.Fatalf("FrontCover = %+v", fc)
	}

	// No type 3 at all: the first picture is used.
	m = mustRead(t, buildFLAC(si,
		rawBlock{blockPicture, pictureBody(6, "image/png", "", []byte("media"))},
		rawBlock{blockPicture, pictureBody(4, "image/png", "", []byte("back"))},
	))
	if fc := m.FrontCover(); fc == nil || string(fc.Data) != "media" {
		t.Fatalf("FrontCover = %+v", fc)
	}

	// No pictures at all.
	m = mustRead(t, buildFLAC(si))
	if fc := m.FrontCover(); fc != nil {
		t.Fatalf("FrontCover = %+v, want nil", fc)
	}
	var nilMeta *Meta
	if fc := nilMeta.FrontCover(); fc != nil {
		t.Fatalf("nil meta: %+v", fc)
	}
}

func TestPictureEmptyData(t *testing.T) {
	m := mustRead(t, buildFLAC(
		rawBlock{blockStreamInfo, streamInfoBody(44100, 2, 16, 44100)},
		rawBlock{blockPicture, pictureBody(3, "-->", "link", nil)},
	))
	if len(m.Pictures) != 1 || m.Pictures[0].MIME != "-->" || len(m.Pictures[0].Data) != 0 {
		t.Fatalf("got %+v", m.Pictures)
	}
}

// --- walk behaviour ----------------------------------------------------------

func TestUnknownBlocksSkipped(t *testing.T) {
	m := mustRead(t, buildFLAC(
		rawBlock{blockStreamInfo, streamInfoBody(48000, 1, 24, 96000)},
		rawBlock{3, bytes.Repeat([]byte{0xAB}, 300)},         // SEEKTABLE
		rawBlock{2, []byte("APPLnonsense")},                  // APPLICATION
		rawBlock{blockVorbisComment, vorbisBody("v", "X=1")}, //
		rawBlock{5, bytes.Repeat([]byte{0x11}, 17)},          // CUESHEET
		rawBlock{99, []byte("reserved")},                     // reserved type
		rawBlock{1, bytes.Repeat([]byte{0}, 4096)},           // PADDING
	))
	if m.Info.SampleRate != 48000 || m.Tags["x"] != "1" {
		t.Fatalf("got %+v tags %v", m.Info, m.Tags)
	}
}

// errAfter returns data and then fails every subsequent read, proving the
// parser stops at the last-flagged block instead of touching the audio.
type errAfter struct {
	r    *bytes.Reader
	boom error
}

func (e *errAfter) Read(p []byte) (int, error) {
	n, err := e.r.Read(p)
	if err == io.EOF {
		return n, e.boom
	}
	return n, err
}

func (e *errAfter) Seek(off int64, whence int) (int64, error) { return e.r.Seek(off, whence) }

func TestLastFlagEndsTheWalk(t *testing.T) {
	boom := errors.New("read past the last metadata block")
	good := buildFLAC(
		rawBlock{blockStreamInfo, streamInfoBody(44100, 2, 16, 44100)},
		rawBlock{blockVorbisComment, vorbisBody("v", "TITLE=Song")},
	)
	m, err := Read(&errAfter{r: bytes.NewReader(good), boom: boom})
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if m.Tags["title"] != "Song" {
		t.Fatalf("tags %v", m.Tags)
	}

	// Same stream with the last flag cleared on the final block: now the parser
	// must go looking for another header and hit the poisoned read.
	bad := append([]byte(nil), good...)
	bad[len(bad)-len(vorbisBody("v", "TITLE=Song"))-4] &^= 0x80
	if _, err := Read(&errAfter{r: bytes.NewReader(bad), boom: boom}); err == nil {
		t.Fatal("want an error when no block is flagged last")
	}
}

func TestErrors(t *testing.T) {
	si := streamInfoBody(44100, 2, 16, 44100)
	full := buildFLAC(
		rawBlock{blockStreamInfo, si},
		rawBlock{blockVorbisComment, vorbisBody("v", "TITLE=Song")},
	)

	cases := []struct {
		name string
		in   []byte
		want string
	}{
		{"empty", nil, "fLaC"},
		{"short marker", []byte("fLa"), "fLaC"},
		{"wrong marker", append([]byte("OggS"), full[4:]...), "fLaC"},
		{"header cut", full[:6], "truncated metadata block header"},
		{"streaminfo body cut", full[:4+4+10], "truncated metadata block 0"},
		{"vorbis body cut", full[:len(full)-3], "truncated metadata block 1"},
		{"streaminfo too short", buildFLAC(rawBlock{blockStreamInfo, si[:20]}), "STREAMINFO is 20 bytes"},
		{"first block not streaminfo", buildFLAC(rawBlock{blockVorbisComment, vorbisBody("v")}), "first metadata block is type 4"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			_, err := Read(bytes.NewReader(c.in))
			if err == nil {
				t.Fatal("want an error")
			}
			if !strings.Contains(err.Error(), c.want) {
				t.Fatalf("error %q does not mention %q", err, c.want)
			}
		})
	}
}

func TestTruncatedUnknownBlockIsAnError(t *testing.T) {
	// A PADDING block whose body is not all there: skipping by seeking would
	// hide this, so the parser consumes skipped blocks instead.
	b := buildFLAC(
		rawBlock{blockStreamInfo, streamInfoBody(44100, 2, 16, 44100)},
		rawBlock{1, bytes.Repeat([]byte{0}, 1000)},
	)
	_, err := Read(bytes.NewReader(b[:len(b)-500]))
	if err == nil || !strings.Contains(err.Error(), "truncated metadata block 1 (type 1)") {
		t.Fatalf("err = %v", err)
	}
}

func TestInnerFieldsBoundToTheBlock(t *testing.T) {
	si := rawBlock{blockStreamInfo, streamInfoBody(44100, 2, 16, 44100)}

	// A comment whose declared length reaches past the end of its own block.
	vc := vorbisBody("v", "TITLE=Song")
	binary.LittleEndian.PutUint32(vc[len(vc)-len("TITLE=Song")-4:], 1<<20)
	if _, err := Read(bytes.NewReader(buildFLAC(si, rawBlock{blockVorbisComment, vc}))); err == nil ||
		!strings.Contains(err.Error(), "past the end of its metadata block") {
		t.Fatalf("vorbis overrun: err = %v", err)
	}

	// A picture whose data length lies.
	pic := pictureBody(3, "image/png", "", []byte("xy"))
	binary.BigEndian.PutUint32(pic[len(pic)-2-4:], 1<<20)
	if _, err := Read(bytes.NewReader(buildFLAC(si, rawBlock{blockPicture, pic}))); err == nil ||
		!strings.Contains(err.Error(), "past the end of its metadata block") {
		t.Fatalf("picture overrun: err = %v", err)
	}

	// A picture truncated mid-MIME-string.
	if _, err := Read(bytes.NewReader(buildFLAC(si, rawBlock{blockPicture, pic[:6]}))); err == nil {
		t.Fatal("short picture: want an error")
	}
}

func TestMaxBlockSizeIsAboveWhatTheHeaderCanExpress(t *testing.T) {
	// The block header's length field is 24 bits, so the largest body any
	// FLAC file can declare is 16 MiB - 1, comfortably under the 64 MiB cap.
	// The cap therefore only ever fires on a forged length; what a real
	// corrupt file produces is a clean truncation error, not a huge read.
	if MaxBlockSize <= 0xffffff {
		t.Fatalf("MaxBlockSize %d is inside the 24-bit length range", MaxBlockSize)
	}
	b := append([]byte("fLaC"), blockStreamInfo, 0xff, 0xff, 0xff)
	_, err := Read(bytes.NewReader(b))
	if err == nil || !strings.Contains(err.Error(), "truncated metadata block 0") {
		t.Fatalf("err = %v", err)
	}
	if !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Fatalf("truncation should wrap io.ErrUnexpectedEOF: %v", err)
	}
}

func TestReadFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "a.flac")
	want := StreamInfo{44100, 2, 16, 44100 * 2}
	if err := os.WriteFile(path, BuildFile(want, map[string]string{"TITLE": "T"}, nil), 0o644); err != nil {
		t.Fatal(err)
	}
	m, err := ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if m.Info != want || m.Tag("title") != "T" {
		t.Fatalf("got %+v %v", m.Info, m.Tags)
	}
	if _, err := ReadFile(filepath.Join(dir, "nope.flac")); err == nil {
		t.Fatal("want an error for a missing file")
	}
	if err := os.WriteFile(path, []byte("not a flac at all"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err = ReadFile(path)
	if err == nil || !strings.Contains(err.Error(), path) {
		t.Fatalf("error should name the file: %v", err)
	}
	if !errors.Is(err, ErrNotFLAC) {
		t.Fatalf("error should wrap ErrNotFLAC: %v", err)
	}
}

// --- BuildFile round trip ----------------------------------------------------

func TestBuildFileRoundTrip(t *testing.T) {
	info := StreamInfo{48000, 2, 24, 48000*7 + 11}
	tags := map[string]string{"TITLE": "Song", "ARTIST": "Band", "tracknumber": "3"}
	pics := []Picture{
		{Type: 2, MIME: "image/png", Data: []byte("back")},
		{Type: 3, MIME: "image/jpeg", Data: bytes.Repeat([]byte{0xFF}, 1000)},
	}
	m := mustRead(t, BuildFile(info, tags, pics))
	if m.Info != info {
		t.Errorf("info = %+v want %+v", m.Info, info)
	}
	if m.Tag("title") != "Song" || m.Tag("artist") != "Band" || m.Tag("tracknumber") != "3" {
		t.Errorf("tags = %v", m.Tags)
	}
	if len(m.Pictures) != 2 {
		t.Fatalf("pictures = %d", len(m.Pictures))
	}
	if fc := m.FrontCover(); fc == nil || fc.MIME != "image/jpeg" || len(fc.Data) != 1000 {
		t.Errorf("front cover = %+v", fc)
	}
	if got := m.DurationSeconds(); got != 7 {
		t.Errorf("duration = %d want 7", got)
	}
}

func TestBuildFileDefaults(t *testing.T) {
	m := mustRead(t, BuildFile(StreamInfo{TotalSamples: 44100}, nil, nil))
	if m.Info != (StreamInfo{44100, 2, 16, 44100}) {
		t.Fatalf("defaults = %+v", m.Info)
	}
}
