package coreart

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// twoByTwoPNG is the smallest picture with a distinct colour in every corner:
// red, green / blue, white. Scaling it up to 28 puts each corner's colour back
// in the corresponding corner, which is the cheap end-to-end proof that the
// scaler is not flipping, transposing or channel-swapping anything.
func twoByTwoPNG(t *testing.T) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, 2, 2))
	img.Set(0, 0, color.RGBA{255, 0, 0, 255})
	img.Set(1, 0, color.RGBA{0, 255, 0, 255})
	img.Set(0, 1, color.RGBA{0, 0, 255, 255})
	img.Set(1, 1, color.RGBA{255, 255, 255, 255})
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatalf("encode png: %v", err)
	}
	return buf.Bytes()
}

func TestRenderHeader(t *testing.T) {
	img := image.NewRGBA(image.Rect(0, 0, 4, 4))
	for y := 0; y < 4; y++ {
		for x := 0; x < 4; x++ {
			img.Set(x, y, color.RGBA{255, 255, 255, 255})
		}
	}
	b := Render(img, ThumbSize)

	if want := HeaderLen + ThumbSize*ThumbSize*2; len(b) != want {
		t.Fatalf("length = %d, want %d", len(b), want)
	}
	if string(b[0:4]) != "CART" {
		t.Errorf("magic = %q, want %q", b[0:4], "CART")
	}
	if v := binary.LittleEndian.Uint16(b[4:6]); v != Version {
		t.Errorf("version = %d, want %d", v, Version)
	}
	if w := binary.LittleEndian.Uint16(b[6:8]); w != ThumbSize {
		t.Errorf("width = %d, want %d", w, ThumbSize)
	}
	if h := binary.LittleEndian.Uint16(b[8:10]); h != ThumbSize {
		t.Errorf("height = %d, want %d", h, ThumbSize)
	}
	if r := binary.LittleEndian.Uint16(b[10:12]); r != 0 {
		t.Errorf("reserved = %d, want 0", r)
	}
	// A solid-white source must pack to 0xFFFF everywhere: it is the one
	// value where rounding and truncation agree, so a wrong shift or a
	// byte-swap shows up immediately.
	for i := HeaderLen; i < len(b); i += 2 {
		if v := binary.LittleEndian.Uint16(b[i:]); v != 0xFFFF {
			t.Fatalf("pixel at byte %d = %#04x, want 0xffff", i, v)
			break
		}
	}
}

func TestPack565Rounding(t *testing.T) {
	cases := []struct {
		r, g, b    uint8
		r5, g6, b5 uint32
	}{
		{0, 0, 0, 0, 0, 0},
		{255, 255, 255, 31, 63, 31},
		{128, 128, 128, 16, 32, 16},
		// 4 is 0.486 of a 5-bit level: rounds down where truncation also
		// gives 0. 5 is 0.608 of a level and rounds UP to 1, where
		// truncation (v>>3) would still say 0.
		{4, 4, 4, 0, 1, 0},
		{5, 5, 5, 1, 1, 1},
	}
	for _, c := range cases {
		got := pack565(c.r, c.g, c.b)
		want := uint16(c.r5<<11 | c.g6<<5 | c.b5)
		if got != want {
			t.Errorf("pack565(%d,%d,%d) = %#04x, want %#04x", c.r, c.g, c.b, got, want)
		}
	}
}

func TestScaleCornersFromTinyPNG(t *testing.T) {
	pic := &flac.Picture{Type: flac.PictureTypeFrontCover, MIME: "image/png", Data: twoByTwoPNG(t)}
	img, err := FromPicture(pic)
	if err != nil {
		t.Fatalf("FromPicture: %v", err)
	}
	px := ToRGB565(img, ThumbSize)
	if len(px) != ThumbSize*ThumbSize {
		t.Fatalf("len = %d, want %d", len(px), ThumbSize*ThumbSize)
	}
	at := func(x, y int) (r, g, b int) {
		v := px[y*ThumbSize+x]
		return int(v>>11) & 31, int(v>>5) & 63, int(v) & 31
	}
	// Lanczos3 on a 2x2 source is a very smooth ramp — the extreme corner is
	// the only place the source colour survives nearly pure. Assert the
	// dominant channel, and that the channels that should be dark are dark,
	// rather than an exact 565 word: the exact word is a property of
	// x/image's weight table, which is not ours to pin.
	type want struct {
		name          string
		x, y          int
		hiR, hiG, hiB bool
	}
	for _, w := range []want{
		{"top-left red", 0, 0, true, false, false},
		{"top-right green", ThumbSize - 1, 0, false, true, false},
		{"bottom-left blue", 0, ThumbSize - 1, false, false, true},
		{"bottom-right white", ThumbSize - 1, ThumbSize - 1, true, true, true},
	} {
		r, g, b := at(w.x, w.y)
		check := func(label string, v, max int, hi bool) {
			frac := float64(v) / float64(max)
			if hi && frac < 0.85 {
				t.Errorf("%s: %s = %d/%d (%.2f), want bright", w.name, label, v, max, frac)
			}
			if !hi && frac > 0.20 {
				t.Errorf("%s: %s = %d/%d (%.2f), want dark", w.name, label, v, max, frac)
			}
		}
		check("R", r, 31, w.hiR)
		check("G", g, 63, w.hiG)
		check("B", b, 31, w.hiB)
	}
}

func TestValid(t *testing.T) {
	img, err := FromPicture(&flac.Picture{MIME: "image/png", Data: twoByTwoPNG(t)})
	if err != nil {
		t.Fatalf("FromPicture: %v", err)
	}
	art := Render(img, ArtSize)
	thm := Render(img, ThumbSize)

	if !Valid(art, ArtSize) {
		t.Error("Valid rejected our own folder.art")
	}
	if !Valid(thm, ThumbSize) {
		t.Error("Valid rejected our own folder.thm")
	}
	if !Valid(art, 0) || !Valid(thm, 0) {
		t.Error("Valid(size=0) rejected a sidecar the firmware would accept")
	}
	// The size argument is the caller asserting WHICH square this is: a
	// 120 where the list chip expects 28 is a file the firmware loads and
	// then box-averages every time it scrolls past.
	if Valid(art, ThumbSize) {
		t.Error("Valid accepted a 120x120 sidecar as a 28x28")
	}
	if Valid(thm, ArtSize) {
		t.Error("Valid accepted a 28x28 sidecar as a 120x120")
	}

	bad := append([]byte(nil), thm...)
	bad[0] = 'D'
	if Valid(bad, ThumbSize) {
		t.Error("Valid accepted a bad magic")
	}

	oversize := append([]byte(nil), art...)
	binary.LittleEndian.PutUint16(oversize[6:8], MaxDim+1)
	if Valid(oversize, 0) {
		t.Error("Valid accepted a width past ARTCACHE_MAX_DIM")
	}
	zero := append([]byte(nil), art...)
	binary.LittleEndian.PutUint16(zero[8:10], 0)
	if Valid(zero, 0) {
		t.Error("Valid accepted a zero height")
	}
	if Valid(thm[:len(thm)-2], ThumbSize) {
		t.Error("Valid accepted a sidecar two bytes short of its own dimensions")
	}
	if Valid(thm[:HeaderLen-1], 0) {
		t.Error("Valid accepted a file shorter than the header")
	}
}

// buildFLACWithPicture assembles a real (silent) FLAC carrying pics.
func buildFLACWithPicture(t *testing.T, pics []flac.Picture) []byte {
	t.Helper()
	return flac.BuildFile(
		flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
		map[string]string{"title": "T", "artist": "A", "album": "B"},
		pics,
	)
}

func TestWriteAlbum(t *testing.T) {
	dir := t.TempDir()
	raw := buildFLACWithPicture(t, []flac.Picture{{
		Type: flac.PictureTypeFrontCover, MIME: "image/png", Data: twoByTwoPNG(t),
	}})
	src := filepath.Join(dir, "01. Track.flac")
	if err := os.WriteFile(src, raw, 0o644); err != nil {
		t.Fatal(err)
	}
	m, err := flac.ReadFile(src)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	res, err := WriteAlbum(dir, m)
	if err != nil {
		t.Fatalf("WriteAlbum: %v", err)
	}
	if res.NoPicture || !res.Wrote() {
		t.Fatalf("Result = %+v, want both sidecars written", res)
	}
	if res.ArtDim != ArtSize || res.ThumbDim != ThumbSize {
		t.Errorf("dims = %d/%d, want %d/%d", res.ArtDim, res.ThumbDim, ArtSize, ThumbSize)
	}
	for _, c := range []struct {
		path string
		dim  int
	}{{res.ArtPath, ArtSize}, {res.ThumbPath, ThumbSize}} {
		b, err := os.ReadFile(c.path)
		if err != nil {
			t.Fatalf("read %s: %v", c.path, err)
		}
		if !Valid(b, c.dim) {
			t.Errorf("%s: the firmware's header check rejects it", c.path)
		}
	}
	// No temp files left behind.
	ents, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range ents {
		if len(e.Name()) > 0 && e.Name()[0] == '.' {
			t.Errorf("leftover temp file %q", e.Name())
		}
	}

	// The 28 is rendered from the source, not from the 120. Proving that
	// negatively is awkward, so prove it positively: a 28 downscaled from
	// the already-resampled 120 differs from the one we wrote.
	src120, err := FromPicture(&flac.Picture{MIME: "image/png", Data: twoByTwoPNG(t)})
	if err != nil {
		t.Fatal(err)
	}
	direct := Render(src120, ThumbSize)
	onDisk, err := os.ReadFile(res.ThumbPath)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(direct, onDisk) {
		t.Error("folder.thm is not the source picture scaled straight to 28")
	}
}

func TestWriteAlbumNoPictureIsNotAnError(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "01. Track.flac")
	if err := os.WriteFile(src, buildFLACWithPicture(t, nil), 0o644); err != nil {
		t.Fatal(err)
	}
	m, err := flac.ReadFile(src)
	if err != nil {
		t.Fatal(err)
	}
	res, err := WriteAlbum(dir, m)
	if err != nil {
		t.Fatalf("WriteAlbum on a picture-less FLAC returned %v, want nil", err)
	}
	if !res.NoPicture || res.Wrote() {
		t.Fatalf("Result = %+v, want NoPicture and nothing written", res)
	}
	if _, err := os.Stat(filepath.Join(dir, ArtName)); !os.IsNotExist(err) {
		t.Error("folder.art was created for an album with no cover")
	}
}

func TestFirstFLAC(t *testing.T) {
	dir := t.TempDir()
	for _, n := range []string{"cover.jpg", "02. B.flac", "01. A.flac", "notes.txt"} {
		if err := os.WriteFile(filepath.Join(dir, n), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	got, err := FirstFLAC(dir)
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Base(got) != "01. A.flac" {
		t.Errorf("FirstFLAC = %q, want 01. A.flac", filepath.Base(got))
	}

	empty := t.TempDir()
	got, err = FirstFLAC(empty)
	if err != nil || got != "" {
		t.Errorf("FirstFLAC(empty) = %q, %v; want \"\", nil", got, err)
	}
}

// TestSizesMatchFirmware pins ArtSize/ThumbSize to the two #defines in
// core/ui/artcache.h. If someone retunes the list chip on the device, this test
// is what tells the host side that every sidecar in the field is now the wrong
// size. Skips when the firmware tree is not next to us.
func TestSizesMatchFirmware(t *testing.T) {
	path := firmwareFile(t, filepath.Join("core", "ui", "artcache.h"))
	b, err := os.ReadFile(path)
	if err != nil {
		t.Skipf("no firmware tree: %v", err)
	}
	for _, c := range []struct {
		define string
		want   int
	}{
		{"ARTCACHE_DIM", ThumbSize},
		{"ARTCACHE_MAX_DIM", ArtSize},
	} {
		re := regexp.MustCompile(`(?m)^#define\s+` + c.define + `\s+(\d+)`)
		m := re.FindSubmatch(b)
		if m == nil {
			t.Errorf("%s: no #define %s", path, c.define)
			continue
		}
		got, err := strconv.Atoi(string(m[1]))
		if err != nil {
			t.Fatal(err)
		}
		if got != c.want {
			t.Errorf("%s = %d in %s, but this package uses %d", c.define, got, path, c.want)
		}
	}
}

// firmwareFile resolves rel against the repo root: $CORE_REPO if set, else by
// walking up from the working directory until a go.mod's parent chain reaches a
// directory holding "core".
func firmwareFile(t *testing.T, rel string) string {
	t.Helper()
	if root := os.Getenv("CORE_REPO"); root != "" {
		return filepath.Join(root, rel)
	}
	dir, err := os.Getwd()
	if err != nil {
		t.Skipf("getwd: %v", err)
	}
	for i := 0; i < 8; i++ {
		if st, err := os.Stat(filepath.Join(dir, rel)); err == nil && !st.IsDir() {
			return filepath.Join(dir, rel)
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	t.Skip("firmware tree not found (set CORE_REPO)")
	return ""
}
