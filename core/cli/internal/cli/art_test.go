package cli

import (
	"bytes"
	"image"
	"image/color"
	"image/jpeg"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// coverJPEG is a small but genuine JPEG, so the command exercises the real
// decoder rather than a PNG shortcut.
func coverJPEG(t *testing.T, shade uint8) []byte {
	t.Helper()
	img := image.NewRGBA(image.Rect(0, 0, 64, 64))
	for y := 0; y < 64; y++ {
		for x := 0; x < 64; x++ {
			img.Set(x, y, color.RGBA{shade, uint8(x * 4), uint8(y * 4), 255})
		}
	}
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: 85}); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// writeAlbumDir lays down one album folder holding a single FLAC, with or
// without an embedded cover.
func writeAlbumDir(t *testing.T, root, name string, cover []byte) string {
	t.Helper()
	dir := filepath.Join(root, name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	var pics []flac.Picture
	if cover != nil {
		pics = []flac.Picture{{Type: flac.PictureTypeFrontCover, MIME: "image/jpeg", Data: cover}}
	}
	raw := flac.BuildFile(
		flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
		map[string]string{"title": name, "artist": "A", "album": name}, pics)
	if err := os.WriteFile(filepath.Join(dir, "01. Track.flac"), raw, 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func runArt(t *testing.T, args ...string) (string, error) {
	t.Helper()
	cmd := newArtCmd()
	var out bytes.Buffer
	cmd.SetOut(&out)
	cmd.SetErr(&out)
	cmd.SetArgs(args)
	err := cmd.Execute()
	return out.String(), err
}

func mustValid(t *testing.T, path string, dim int) {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	if !coreart.Valid(b, dim) {
		t.Errorf("%s: the firmware's header check rejects it (%d bytes)", path, len(b))
	}
}

func TestArtBatch(t *testing.T) {
	root := t.TempDir()
	withArt := writeAlbumDir(t, root, "Album A - Artist", coverJPEG(t, 200))
	alsoArt := writeAlbumDir(t, root, "Album B - Artist", coverJPEG(t, 40))
	noArt := writeAlbumDir(t, root, "Album C - Artist", nil)
	// A folder with no FLAC at all.
	empty := filepath.Join(root, "Album D - Artist")
	if err := os.MkdirAll(empty, 0o755); err != nil {
		t.Fatal(err)
	}

	out, err := runArt(t, "--batch", root)
	if err != nil {
		t.Fatalf("art --batch: %v\n%s", err, out)
	}
	for _, want := range []string{
		"folder.art 120x120 + folder.thm 28x28",
		"skip (no embedded art",
		"skip (no FLAC or MP3)",
		"2/4 folder(s) got art, 2 skipped, 0 failed",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("output missing %q:\n%s", want, out)
		}
	}
	for _, dir := range []string{withArt, alsoArt} {
		mustValid(t, filepath.Join(dir, coreart.ArtName), coreart.ArtSize)
		mustValid(t, filepath.Join(dir, coreart.ThumbName), coreart.ThumbSize)
	}
	if _, err := os.Stat(filepath.Join(noArt, coreart.ArtName)); !os.IsNotExist(err) {
		t.Error("an album with no cover got a folder.art")
	}
}

func TestArtAlbumAndSingle(t *testing.T) {
	root := t.TempDir()
	dir := writeAlbumDir(t, root, "Album A - Artist", coverJPEG(t, 120))

	if out, err := runArt(t, "--album", dir); err != nil {
		t.Fatalf("art --album: %v\n%s", err, out)
	}
	mustValid(t, filepath.Join(dir, coreart.ArtName), coreart.ArtSize)
	mustValid(t, filepath.Join(dir, coreart.ThumbName), coreart.ThumbSize)

	out := filepath.Join(root, "one.art")
	if o, err := runArt(t, filepath.Join(dir, "01. Track.flac"), out, "--size", "64"); err != nil {
		t.Fatalf("art single: %v\n%s", err, o)
	}
	mustValid(t, out, 64)
}

func TestArtRejectsImpossibleSizes(t *testing.T) {
	root := t.TempDir()
	dir := writeAlbumDir(t, root, "Album A - Artist", coverJPEG(t, 120))
	// 121 is past ARTCACHE_MAX_DIM: the firmware treats such a sidecar as
	// absent, so writing one is worse than refusing.
	if _, err := runArt(t, "--album", dir, "--art-size", "121"); err == nil {
		t.Error("art --art-size 121 was accepted")
	}
	if _, err := runArt(t, "--album", dir, "--thumb-size", "0"); err == nil {
		t.Error("art --thumb-size 0 was accepted")
	}
}

func TestArtBatchFailsWhenAnAlbumFails(t *testing.T) {
	root := t.TempDir()
	dir := writeAlbumDir(t, root, "Album A - Artist", nil)
	// A FLAC whose PICTURE block holds bytes no decoder recognises: that is a
	// real failure, not a skip, and must reach the exit status.
	raw := flac.BuildFile(
		flac.StreamInfo{SampleRate: 44100, Channels: 2, BitsPerSample: 16, TotalSamples: 4410},
		map[string]string{"title": "x"},
		[]flac.Picture{{Type: flac.PictureTypeFrontCover, MIME: "image/jpeg", Data: []byte("not an image")}})
	if err := os.WriteFile(filepath.Join(dir, "01. Track.flac"), raw, 0o644); err != nil {
		t.Fatal(err)
	}
	out, err := runArt(t, "--batch", root)
	if err == nil {
		t.Fatalf("art --batch returned nil for a failing album:\n%s", out)
	}
	if !strings.Contains(out, "1 failed") {
		t.Errorf("output does not count the failure:\n%s", out)
	}
}
