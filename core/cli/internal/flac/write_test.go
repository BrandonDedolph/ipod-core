package flac

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// --- fixtures ----------------------------------------------------------------

// testPNG encodes a real PNG of w x h, so image.DecodeConfig (and ffprobe)
// see the dimensions the PICTURE block will claim.
func testPNG(t *testing.T, w, h int) []byte {
	t.Helper()
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			img.Set(x, y, color.NRGBA{uint8(x * 7), uint8(y * 11), 0x40, 0xff})
		}
	}
	var b bytes.Buffer
	if err := png.Encode(&b, img); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func testJPEG(t *testing.T, w, h int) []byte {
	t.Helper()
	img := image.NewYCbCr(image.Rect(0, 0, w, h), image.YCbCrSubsampleRatio420)
	for i := range img.Y {
		img.Y[i] = uint8(i)
	}
	var b bytes.Buffer
	if err := jpeg.Encode(&b, img, &jpeg.Options{Quality: 80}); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

var testInfo = StreamInfo{44100, 2, 16, 44100 * 2}

// fixture writes a FLAC file with the given pictures and padding and returns
// its path.
func fixture(t *testing.T, name string, pics []Picture, padding int) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	data := BuildFileP(testInfo, map[string]string{"TITLE": "Silence", "ARTIST": "Core"}, pics, padding)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// --- assertions --------------------------------------------------------------

// chainOf re-reads a file's metadata region. Tests use it to look at the block
// layout directly instead of inferring it from Read.
func chainOf(t *testing.T, path string) *chain {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	c, err := readChain(f)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	return c
}

// audioDigest hashes everything from the first audio frame to EOF — the bytes
// no writer in this package is allowed to change.
func audioDigest(t *testing.T, path string) string {
	t.Helper()
	c := chainOf(t, path)
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if int64(len(raw)) <= c.audioOff {
		t.Fatalf("%s: no audio after offset %d", path, c.audioOff)
	}
	sum := sha256.Sum256(raw[c.audioOff:])
	return hex.EncodeToString(sum[:])
}

func fileSize(t *testing.T, path string) int64 {
	t.Helper()
	st, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	return st.Size()
}

// blockLayout renders the chain as "type:bodylen" for readable failures.
func blockLayout(c *chain) string {
	var parts []string
	for _, b := range c.blocks {
		parts = append(parts, fmt.Sprintf("%d:%d", b.typ, len(b.body)))
	}
	return strings.Join(parts, " ")
}

func mustReadFile(t *testing.T, path string) *Meta {
	t.Helper()
	m, err := ReadFile(path)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	return m
}

// pictureBlockLen is the on-disk cost of the PICTURE block WritePicture would
// write for pic, header included — the number the in-place arithmetic turns on.
func pictureBlockLen(t *testing.T, pic Picture) int {
	t.Helper()
	if pic.Type == 0 {
		pic.Type = PictureTypeFrontCover
	}
	if err := describeImage(&pic); err != nil {
		t.Fatal(err)
	}
	return blockHeaderLen + len(buildPicture(pic))
}

// --- the two paths -----------------------------------------------------------

// TestWritePictureInPlace is the padding path: enough room already in the
// file, so the size does not move a byte and neither does the audio.
func TestWritePictureInPlace(t *testing.T) {
	cover := testPNG(t, 24, 16)
	pic := Picture{MIME: "image/png", Data: cover}
	need := pictureBlockLen(t, pic)

	path := fixture(t, "in-place.flac", nil, need+1024)
	before := fileSize(t, path)
	beforeAudio := audioDigest(t, path)
	beforeBytes, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}

	if err := WritePicture(path, pic); err != nil {
		t.Fatalf("WritePicture: %v", err)
	}

	if got := fileSize(t, path); got != before {
		t.Errorf("file size %d, want %d (the in-place path must not resize the file)", got, before)
	}
	if got := audioDigest(t, path); got != beforeAudio {
		t.Errorf("audio frames changed: %s -> %s", beforeAudio, got)
	}
	afterBytes, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	// Marker + STREAMINFO header + body must be byte-identical.
	head := 4 + blockHeaderLen + streamInfoLen
	if !bytes.Equal(beforeBytes[:head], afterBytes[:head]) {
		t.Error("the fLaC marker or STREAMINFO changed")
	}

	c := chainOf(t, path)
	if got := blockLayout(c); !strings.Contains(got, fmt.Sprintf("%d:", blockPicture)) {
		t.Fatalf("no PICTURE block after the write: %s", got)
	}
	last := c.blocks[len(c.blocks)-1]
	if last.typ != blockPadding {
		t.Errorf("last block is type %d, want PADDING; layout %s", last.typ, blockLayout(c))
	}
	// The padding gave up exactly the picture block's size and kept the rest.
	if want := 1024; len(last.body) != want {
		t.Errorf("padding left %d bytes, want %d; layout %s", len(last.body), want, blockLayout(c))
	}

	m := mustReadFile(t, path)
	if len(m.Pictures) != 1 {
		t.Fatalf("got %d pictures, want 1", len(m.Pictures))
	}
	got := m.Pictures[0]
	if got.Type != PictureTypeFrontCover || got.MIME != "image/png" ||
		got.Width != 24 || got.Height != 16 || got.Depth != 32 || got.Colors != 0 {
		t.Errorf("picture = %+v, want type 3 image/png 24x16 depth 32", Picture{
			Type: got.Type, MIME: got.MIME, Width: got.Width, Height: got.Height, Depth: got.Depth, Colors: got.Colors})
	}
	if !bytes.Equal(got.Data, cover) {
		t.Error("picture data read back differs from what was written")
	}
	if m.Tag("title") != "Silence" || m.Info != testInfo {
		t.Errorf("tags or STREAMINFO disturbed: %q %+v", m.Tag("title"), m.Info)
	}
}

// TestWritePictureRewrite is the other path: no padding at all, so the file is
// rebuilt around the same audio.
func TestWritePictureRewrite(t *testing.T) {
	cover := testJPEG(t, 32, 32)
	path := fixture(t, "rewrite.flac", nil, 0)
	before := fileSize(t, path)
	beforeAudio := audioDigest(t, path)

	if err := WritePicture(path, Picture{Data: cover}); err != nil {
		t.Fatalf("WritePicture: %v", err)
	}

	if got := fileSize(t, path); got <= before {
		t.Errorf("file size %d, want more than %d", got, before)
	}
	if got := audioDigest(t, path); got != beforeAudio {
		t.Errorf("audio frames changed: %s -> %s", beforeAudio, got)
	}
	c := chainOf(t, path)
	last := c.blocks[len(c.blocks)-1]
	if last.typ != blockPadding || len(last.body) != DefaultPadding {
		t.Errorf("last block is type %d of %d bytes, want PADDING of %d; layout %s",
			last.typ, len(last.body), DefaultPadding, blockLayout(c))
	}
	m := mustReadFile(t, path)
	if len(m.Pictures) != 1 {
		t.Fatalf("got %d pictures, want 1", len(m.Pictures))
	}
	if m.Pictures[0].MIME != "image/jpeg" || m.Pictures[0].Width != 32 || m.Pictures[0].Depth != 24 {
		t.Errorf("picture = %+v, want image/jpeg 32x32 depth 24", m.Pictures[0].MIME)
	}
	// No temporary file left behind.
	leftovers := tempLeftovers(t, filepath.Dir(path))
	if len(leftovers) != 0 {
		t.Errorf("temporary files left behind: %v", leftovers)
	}
}

func tempLeftovers(t *testing.T, dir string) []string {
	t.Helper()
	var out []string
	err := filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.IsDir() && strings.Contains(d.Name(), ".core-") {
			out = append(out, p)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	sort.Strings(out)
	return out
}

// TestWritePicturePathChoice pins the arithmetic that decides between the two
// paths: the leftover has to be 0, or at least a block header. One or two
// spare bytes cannot be expressed as a PADDING block, so the file is rewritten.
func TestWritePicturePathChoice(t *testing.T) {
	cover := testPNG(t, 8, 8)
	pic := Picture{Data: cover}
	need := pictureBlockLen(t, pic)

	cases := []struct {
		name     string
		padding  int // PADDING body in the fixture
		inPlace  bool
		wantLast byte
		wantPad  int // -1: do not check (the last block is the picture itself)
	}{
		{"exact fit", need - blockHeaderLen, true, blockPicture, -1},
		{"one byte over", need - blockHeaderLen + 1, false, blockPadding, DefaultPadding},
		{"three bytes over", need - blockHeaderLen + 3, false, blockPadding, DefaultPadding},
		{"empty padding left", need - blockHeaderLen + 4, true, blockPadding, 0},
		{"room to spare", need - blockHeaderLen + 64, true, blockPadding, 60},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			path := fixture(t, "choice.flac", nil, tc.padding)
			before := fileSize(t, path)
			beforeAudio := audioDigest(t, path)

			if err := WritePicture(path, pic); err != nil {
				t.Fatalf("WritePicture: %v", err)
			}
			same := fileSize(t, path) == before
			if same != tc.inPlace {
				t.Errorf("size unchanged = %v, want in-place = %v (before %d, after %d)",
					same, tc.inPlace, before, fileSize(t, path))
			}
			if got := audioDigest(t, path); got != beforeAudio {
				t.Errorf("audio frames changed: %s -> %s", beforeAudio, got)
			}
			c := chainOf(t, path)
			last := c.blocks[len(c.blocks)-1]
			if last.typ != tc.wantLast || (tc.wantPad >= 0 && len(last.body) != tc.wantPad) {
				t.Errorf("last block type %d len %d, want type %d len %d; layout %s",
					last.typ, len(last.body), tc.wantLast, tc.wantPad, blockLayout(c))
			}
			if n := len(mustReadFile(t, path).Pictures); n != 1 {
				t.Errorf("got %d pictures, want 1", n)
			}
		})
	}
}

// TestWritePictureReplaces: a second call swaps the cover instead of stacking
// another block, on both paths.
func TestWritePictureReplaces(t *testing.T) {
	for _, padding := range []int{0, 64 << 10} {
		t.Run(fmt.Sprintf("padding=%d", padding), func(t *testing.T) {
			first := testPNG(t, 10, 10)
			path := fixture(t, "replace.flac", []Picture{{Type: PictureTypeFrontCover, MIME: "image/png", Data: first}}, padding)
			beforeAudio := audioDigest(t, path)

			second := testJPEG(t, 48, 32)
			if err := WritePicture(path, Picture{Data: second}); err != nil {
				t.Fatalf("WritePicture: %v", err)
			}
			if got := audioDigest(t, path); got != beforeAudio {
				t.Errorf("audio frames changed: %s -> %s", beforeAudio, got)
			}
			m := mustReadFile(t, path)
			if len(m.Pictures) != 1 {
				t.Fatalf("got %d pictures, want exactly 1: %s", len(m.Pictures), blockLayout(chainOf(t, path)))
			}
			if !bytes.Equal(m.Pictures[0].Data, second) || m.Pictures[0].MIME != "image/jpeg" {
				t.Error("the old cover is still there")
			}
			if m.Pictures[0].Width != 48 || m.Pictures[0].Height != 32 {
				t.Errorf("dims %dx%d, want 48x32", m.Pictures[0].Width, m.Pictures[0].Height)
			}
		})
	}
}

// TestWritePictureLastBlockIsPicture covers the layout BuildFile produces with
// no padding: the PICTURE carries the last-metadata-block flag, so replacing it
// has to move that flag somewhere valid.
func TestWritePictureLastBlockIsPicture(t *testing.T) {
	old := testPNG(t, 12, 12)
	path := fixture(t, "piclast.flac", []Picture{{Type: PictureTypeFrontCover, MIME: "image/png", Data: old}}, 0)

	c := chainOf(t, path)
	if c.blocks[len(c.blocks)-1].typ != blockPicture {
		t.Fatalf("fixture does not end in a PICTURE: %s", blockLayout(c))
	}
	beforeAudio := audioDigest(t, path)

	// Same size in, so the chain is the same length: the exact-fit branch with
	// a picture as the final block.
	if err := WritePicture(path, Picture{Data: testPNG(t, 12, 12)}); err != nil {
		t.Fatalf("WritePicture: %v", err)
	}
	if got := audioDigest(t, path); got != beforeAudio {
		t.Errorf("audio frames changed: %s -> %s", beforeAudio, got)
	}
	m := mustReadFile(t, path) // Read stops at the last-block flag: a wrong flag is a parse error or a lost block.
	if len(m.Pictures) != 1 || m.Tag("artist") != "Core" {
		t.Errorf("chain broken after the write: %d pictures, artist %q, layout %s",
			len(m.Pictures), m.Tag("artist"), blockLayout(chainOf(t, path)))
	}
}

// TestWritePictureKeepsOtherPictureTypes: only the same type is replaced, so a
// back cover or an artist photo survives.
func TestWritePictureKeepsOtherPictureTypes(t *testing.T) {
	back := testPNG(t, 6, 6)
	path := fixture(t, "types.flac", []Picture{
		{Type: PictureTypeFrontCover, MIME: "image/png", Data: testPNG(t, 4, 4)},
		{Type: 4, MIME: "image/png", Data: back},
	}, 32<<10)

	if err := WritePicture(path, Picture{Data: testPNG(t, 20, 20)}); err != nil {
		t.Fatalf("WritePicture: %v", err)
	}
	m := mustReadFile(t, path)
	if len(m.Pictures) != 2 {
		t.Fatalf("got %d pictures, want 2", len(m.Pictures))
	}
	var fronts, backs int
	for _, p := range m.Pictures {
		switch p.Type {
		case PictureTypeFrontCover:
			fronts++
			if p.Width != 20 {
				t.Errorf("front cover is %dx%d, want 20x20", p.Width, p.Height)
			}
		case 4:
			backs++
			if !bytes.Equal(p.Data, back) {
				t.Error("the back cover was rewritten")
			}
		}
	}
	if fronts != 1 || backs != 1 {
		t.Errorf("got %d front and %d back covers, want 1 and 1", fronts, backs)
	}
}

// TestRemovePictures turns the pictures back into padding without resizing.
func TestRemovePictures(t *testing.T) {
	path := fixture(t, "remove.flac", []Picture{
		{Type: PictureTypeFrontCover, MIME: "image/png", Data: testPNG(t, 30, 30)},
		{Type: 4, MIME: "image/png", Data: testPNG(t, 8, 8)},
	}, 128)
	before := fileSize(t, path)
	beforeAudio := audioDigest(t, path)

	if err := RemovePictures(path); err != nil {
		t.Fatalf("RemovePictures: %v", err)
	}
	if got := fileSize(t, path); got != before {
		t.Errorf("file size %d, want %d", got, before)
	}
	if got := audioDigest(t, path); got != beforeAudio {
		t.Errorf("audio frames changed: %s -> %s", beforeAudio, got)
	}
	m := mustReadFile(t, path)
	if len(m.Pictures) != 0 {
		t.Errorf("got %d pictures, want none", len(m.Pictures))
	}
	if m.Tag("title") != "Silence" {
		t.Errorf("tags lost: %q", m.Tag("title"))
	}
	c := chainOf(t, path)
	if last := c.blocks[len(c.blocks)-1]; last.typ != blockPadding {
		t.Errorf("last block is type %d, want PADDING; layout %s", last.typ, blockLayout(c))
	}
}

// --- refusals ----------------------------------------------------------------

// TestWritePictureTruncatedFile: a file cut off inside its metadata chain is
// an error, and not one byte of it is written.
func TestWritePictureTruncatedFile(t *testing.T) {
	whole := BuildFileP(testInfo, map[string]string{"TITLE": "Silence"}, nil, 4096)
	c := chainOf(t, writeTemp(t, "whole.flac", whole))

	cases := map[string]int{
		"mid header":     int(c.audioOff) - 4096 - 2, // inside the PADDING header
		"mid body":       int(c.audioOff) - 100,      // inside the PADDING body
		"marker only":    4,
		"not a flac":     0,
		"nothing at all": 0,
	}
	for name, cut := range cases {
		t.Run(name, func(t *testing.T) {
			data := whole[:cut]
			if name == "not a flac" {
				data = []byte("ID3\x04junkjunkjunk")
			}
			path := writeTemp(t, "cut.flac", data)
			before, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			err = WritePicture(path, Picture{Data: testPNG(t, 4, 4)})
			if err == nil {
				t.Fatal("WritePicture accepted a truncated file")
			}
			after, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(before, after) {
				t.Errorf("the broken file was modified (%d -> %d bytes)", len(before), len(after))
			}
			if l := tempLeftovers(t, filepath.Dir(path)); len(l) != 0 {
				t.Errorf("temporary files left behind: %v", l)
			}
		})
	}
}

func writeTemp(t *testing.T, name string, data []byte) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// TestWritePictureRejectsBadInput: no data, not an image, and a block too big
// for the 24-bit length field. Each leaves the file exactly as it was.
func TestWritePictureRejectsBadInput(t *testing.T) {
	// A valid PNG header with 16 MiB of junk glued on: DecodeConfig only reads
	// the header, so this is a decodable picture whose block cannot be encoded.
	huge := append(testPNG(t, 2, 2), make([]byte, maxBlockBody)...)

	cases := []struct {
		name string
		pic  Picture
		want string
	}{
		{"no data", Picture{MIME: "image/png"}, "no data"},
		{"not an image", Picture{Data: []byte("this is not a picture at all")}, "not a decodable"},
		{"over the format limit", Picture{Data: huge}, "format limit"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			path := fixture(t, "bad.flac", nil, 8192)
			before, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			err = WritePicture(path, tc.pic)
			if err == nil {
				t.Fatal("WritePicture accepted it")
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("error %q, want it to mention %q", err, tc.want)
			}
			after, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(before, after) {
				t.Error("the file was modified despite the error")
			}
		})
	}
}

// TestWritePictureRewriteSurvivesACrash simulates losing power between the
// temporary file being fsynced and the rename: the original must still be the
// original, complete and readable, and no debris is left in the folder.
func TestWritePictureRewriteSurvivesACrash(t *testing.T) {
	path := fixture(t, "crash.flac", nil, 0) // no padding: the rewrite path
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}

	boom := errors.New("power loss")
	var sawTemp string
	testHookBeforeRename = func(tmp string) error {
		sawTemp = tmp
		// The new file is already complete at this point.
		m, err := ReadFile(tmp)
		if err != nil {
			t.Errorf("the temporary file is not a valid FLAC: %v", err)
		} else if len(m.Pictures) != 1 {
			t.Errorf("temporary file has %d pictures, want 1", len(m.Pictures))
		}
		return boom
	}
	defer func() { testHookBeforeRename = nil }()

	err = WritePicture(path, Picture{Data: testPNG(t, 16, 16)})
	if !errors.Is(err, boom) {
		t.Fatalf("WritePicture error = %v, want %v", err, boom)
	}
	if sawTemp == "" {
		t.Fatal("the rewrite path did not go through a temporary file")
	}
	if filepath.Dir(sawTemp) != filepath.Dir(path) {
		t.Errorf("temporary file %s is not beside the original", sawTemp)
	}
	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(before, after) {
		t.Error("the original changed before the rename")
	}
	if l := tempLeftovers(t, filepath.Dir(path)); len(l) != 0 {
		t.Errorf("temporary files left behind: %v", l)
	}
}

// --- third-party agreement ---------------------------------------------------

// probeStreams returns one "index,codec_type,codec_name" line per stream.
func probeStreams(ffprobe, path string) (string, error) {
	out, err := exec.Command(ffprobe, "-v", "error", "-show_entries",
		"stream=index,codec_type,codec_name", "-of", "csv=p=0", path).Output()
	if err != nil {
		var ee *exec.ExitError
		if asExitError(err, &ee) {
			return "", fmt.Errorf("ffprobe: %v: %s", err, bytes.TrimSpace(ee.Stderr))
		}
		return "", err
	}
	return strings.TrimSpace(string(out)), nil
}

func decodeClean(t *testing.T, ffmpeg, path string) {
	t.Helper()
	cmd := exec.Command(ffmpeg, "-v", "error", "-i", path, "-f", "null", "-")
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		t.Fatalf("ffmpeg decode of %s failed: %v\n%s", path, err, stderr.String())
	}
	if stderr.Len() != 0 {
		t.Errorf("ffmpeg wrote to stderr at -v error for %s:\n%s", path, stderr.String())
	}
}

// TestWritePictureAgreesWithFFprobe is the outside opinion: after the write
// the duration is the same to the microsecond, the audio stream is still
// there, the cover shows up as a video stream, and a full decode is silent.
func TestWritePictureAgreesWithFFprobe(t *testing.T) {
	ffprobe, ffmpeg := lookTool("ffprobe"), lookTool("ffmpeg")
	if ffprobe == "" || ffmpeg == "" {
		t.Skip("ffprobe/ffmpeg not installed")
	}
	cover := testPNG(t, 64, 64)
	need := pictureBlockLen(t, Picture{Data: cover})

	cases := []struct {
		name    string
		padding int
		codec   string
		data    []byte
	}{
		{"in place, png", need + 4096, "png", cover},
		{"rewrite, png", 0, "png", cover},
		{"rewrite, jpeg", 0, "mjpeg", testJPEG(t, 96, 96)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			path := fixture(t, "probe.flac", nil, tc.padding)
			beforeDur, err := probeDuration(ffprobe, path)
			if err != nil {
				t.Fatal(err)
			}
			beforeStreams, err := probeStreams(ffprobe, path)
			if err != nil {
				t.Fatal(err)
			}
			beforeAudio := audioDigest(t, path)

			if err := WritePicture(path, Picture{Data: tc.data}); err != nil {
				t.Fatalf("WritePicture: %v", err)
			}

			if got := audioDigest(t, path); got != beforeAudio {
				t.Errorf("audio frames changed: %s -> %s", beforeAudio, got)
			}
			afterDur, err := probeDuration(ffprobe, path)
			if err != nil {
				t.Fatal(err)
			}
			if diff := afterDur - beforeDur; diff > 1e-6 || diff < -1e-6 {
				t.Errorf("duration %v -> %v", beforeDur, afterDur)
			}
			afterStreams, err := probeStreams(ffprobe, path)
			if err != nil {
				t.Fatal(err)
			}
			t.Logf("streams before: %q  after: %q", beforeStreams, afterStreams)
			if !strings.Contains(afterStreams, "flac") {
				t.Errorf("audio stream gone: %q", afterStreams)
			}
			if !strings.Contains(afterStreams, tc.codec+",video") {
				t.Errorf("attached picture not seen: %q, want a %s,video stream", afterStreams, tc.codec)
			}
			decodeClean(t, ffmpeg, path)
		})
	}
}

// TestWritePictureOnARealFile runs the writer over a COPY of a file from the
// real library (CORE_PARITY_SRC), which is the only subject here that was not
// produced by this package's own builder: real SEEKTABLE, real vendor string,
// real padding, a real cover already in place.
func TestWritePictureOnARealFile(t *testing.T) {
	src := os.Getenv("CORE_PARITY_SRC")
	if src == "" {
		t.Skip("CORE_PARITY_SRC not set")
	}
	ffprobe, ffmpeg := lookTool("ffprobe"), lookTool("ffmpeg")
	if ffprobe == "" || ffmpeg == "" {
		t.Skip("ffprobe/ffmpeg not installed")
	}
	var found string
	filepath.WalkDir(src, func(p string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() || filepath.Ext(p) != ".flac" {
			return nil //nolint:nilerr // an unreadable corner of the tree is not this test's business
		}
		found = p
		return fs.SkipAll
	})
	if found == "" {
		t.Skipf("no .flac under %s", src)
	}

	// Copy, never touch the original.
	raw, err := os.ReadFile(found)
	if err != nil {
		t.Fatal(err)
	}
	path := writeTemp(t, "real.flac", raw)
	beforeDur, err := probeDuration(ffprobe, path)
	if err != nil {
		t.Fatal(err)
	}
	beforeStreams, err := probeStreams(ffprobe, path)
	if err != nil {
		t.Fatal(err)
	}
	beforeAudio := audioDigest(t, path)
	beforeSize := fileSize(t, path)
	t.Logf("%s\n  layout before: %s\n  streams before: %q", found, blockLayout(chainOf(t, path)), beforeStreams)

	if err := WritePicture(path, Picture{Data: testPNG(t, 500, 500)}); err != nil {
		t.Fatalf("WritePicture: %v", err)
	}
	t.Logf("  layout after:  %s  (%d -> %d bytes)", blockLayout(chainOf(t, path)), beforeSize, fileSize(t, path))

	if got := audioDigest(t, path); got != beforeAudio {
		t.Errorf("audio frames changed: %s -> %s", beforeAudio, got)
	}
	afterDur, err := probeDuration(ffprobe, path)
	if err != nil {
		t.Fatal(err)
	}
	if diff := afterDur - beforeDur; diff > 1e-6 || diff < -1e-6 {
		t.Errorf("duration %v -> %v", beforeDur, afterDur)
	}
	afterStreams, err := probeStreams(ffprobe, path)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("  streams after:  %q", afterStreams)
	if !strings.Contains(afterStreams, "png,video") {
		t.Errorf("attached picture not seen: %q", afterStreams)
	}
	m := mustReadFile(t, path)
	if len(m.Pictures) != 1 || m.Pictures[0].Width != 500 {
		t.Errorf("got %d pictures, first %dx%d, want one 500x500", len(m.Pictures), m.Pictures[0].Width, m.Pictures[0].Height)
	}
	decodeClean(t, ffmpeg, path)
}
