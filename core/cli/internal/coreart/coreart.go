// Package coreart turns an album's embedded cover picture into the two
// CoreArt sidecars the firmware blits: folder.art (120x120) and folder.thm
// (28x28).
//
// The device has no FPU and no JPEG decoder on the audio path, so the cover is
// pre-rendered on the host to a fixed-size RGB565 bitmap that the firmware
// reads straight into a scratch buffer and copies to the framebuffer. This
// package is the Go port of tools/coreart.py, which shells out to ffmpeg for
// the decode/scale/convert; the pipeline here is deliberately the same shape so
// the two can be compared pixel for pixel (see the oracle test):
//
//	decode (image/jpeg, image/png)
//	  -> image.RGBA
//	  -> Lanczos3 stretch to size x size (no crop, exactly ffmpeg "scale=N:N")
//	  -> rounded RGB565 pack
//	  -> CART container
//
// CoreArt container (little-endian), matching core/ui/artcache.c's validation:
//
//	off  0 : magic "CART"      (4 bytes)
//	off  4 : u16 version = 1
//	off  6 : u16 width
//	off  8 : u16 height
//	off 10 : u16 reserved = 0
//	off 12 : width*height u16 RGB565 pixels, row-major
//
// Both sizes are rendered from the SAME source picture, independently — the
// thumbnail is never a downscale of the 120, because a 28x28 Lanczos of the
// original is visibly cleaner than a Lanczos of an already-resampled 120.
package coreart

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"image"
	"image/draw"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"

	// Registered for image.Decode: the format is decided by content sniffing,
	// not by the PICTURE block's declared MIME type, because a tagger that
	// wrote "image/jpeg" over PNG bytes is common enough to be worth ignoring.
	_ "image/jpeg"
	_ "image/png"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/id3"
	xdraw "golang.org/x/image/draw"
)

// The two sizes the firmware reads, and nothing else. ArtSize is
// ARTCACHE_MAX_DIM and ThumbSize is ARTCACHE_DIM in core/ui/artcache.h; the
// thumbnail MUST be exactly ARTCACHE_DIM so the device copies it 1:1 instead of
// box-averaging it at load time.
const (
	ArtSize   = 120
	ThumbSize = 28
)

// Container constants.
const (
	// HeaderLen is ARTCACHE_HDR_LEN.
	HeaderLen = 12
	// Version is the only container version the firmware has ever seen.
	Version = 1
	// MaxDim is ARTCACHE_MAX_DIM: the largest square the firmware's scratch
	// buffer can hold, so the largest sidecar it will accept.
	MaxDim = 120
)

// Magic is the container's leading 4 bytes.
var Magic = [4]byte{'C', 'A', 'R', 'T'}

// Sidecar file names, written into the album folder next to the tracks.
const (
	ArtName   = "folder.art"
	ThumbName = "folder.thm"
)

// ErrNoPicture reports that a FLAC carries no PICTURE block. WriteAlbum does
// not return it — an album with no cover is a normal outcome, not a failure —
// but FromPicture does, so a caller that asked for one specific file's art
// gets a real error.
var ErrNoPicture = errors.New("no embedded picture")

// lanczos3 is ffmpeg's "flags=lanczos": a windowed sinc with a = 3.
//
// x/image ships ApproxBiLinear, BiLinear and CatmullRom but no Lanczos, so the
// kernel is spelled out here. x/image normalises the sampled weights so they
// sum to one, which is what swscale does too, so no extra correction is needed.
var lanczos3 = &xdraw.Kernel{
	Support: 3,
	At: func(t float64) float64 {
		if t < 0 {
			t = -t
		}
		if t >= 3 {
			return 0
		}
		return sinc(t) * sinc(t/3)
	},
}

// srcOp is draw.Src spelled once.
var srcOp = xdraw.Src

func sinc(t float64) float64 {
	if t == 0 {
		return 1
	}
	t *= math.Pi
	return math.Sin(t) / t
}

// FromPicture decodes a FLAC PICTURE block into an image.
//
// The format comes from the bytes (image.Decode sniffs), not from p.MIME; the
// MIME string is only used to make a failure message say what the file claimed
// to be. JPEG and PNG are registered, which covers every cover in the library
// and every cover any tagger writes; a GIF or BMP cover would fail loudly here
// rather than silently produce a blank chip on the device.
func FromPicture(p *flac.Picture) (image.Image, error) {
	if p == nil || len(p.Data) == 0 {
		return nil, ErrNoPicture
	}
	img, _, err := image.Decode(bytes.NewReader(p.Data))
	if err != nil {
		claimed := p.MIME
		if claimed == "" {
			claimed = "(no MIME)"
		}
		return nil, fmt.Errorf("decode picture (declared %s, %d bytes): %w", claimed, len(p.Data), err)
	}
	return img, nil
}

// ToRGB565 stretches img to size x size and packs it as RGB565.
//
// "Stretch", not "fit": a non-square cover is squashed, exactly like ffmpeg's
// scale=N:N with no aspect flags. Cropping or letterboxing would disagree with
// the reference implementation on every non-square cover, and the firmware
// draws the sidecar into a fixed square box regardless.
//
// The source is converted to image.RGBA first so that one code path covers
// YCbCr (JPEG), paletted and NRGBA (PNG) sources; image/draw does the colour
// conversion. ffmpeg instead scales in the decoder's own YUV space and converts
// afterwards — an accepted difference, bounded by the oracle test.
func ToRGB565(img image.Image, size int) []uint16 {
	if size <= 0 {
		return nil
	}
	src := toRGBA(img)
	dst := image.NewRGBA(image.Rect(0, 0, size, size))
	lanczos3.Scale(dst, dst.Bounds(), src, src.Bounds(), srcOp, nil)

	out := make([]uint16, size*size)
	for i := range out {
		p := dst.Pix[i*4 : i*4+4 : i*4+4]
		out[i] = pack565(p[0], p[1], p[2])
	}
	return out
}

// pack565 quantises 8-bit channels to 5/6/5 by rounding to the nearest
// representable level. Truncation (v>>3) is the cheaper spelling and is what
// some converters do, but it darkens every channel by half a level on average,
// which is visible as a green-grey cast on the device's 320x240 panel.
func pack565(r, g, b uint8) uint16 {
	r5 := (uint32(r)*31 + 127) / 255
	g6 := (uint32(g)*63 + 127) / 255
	b5 := (uint32(b)*31 + 127) / 255
	return uint16(r5<<11 | g6<<5 | b5)
}

// toRGBA returns img as an *image.RGBA whose bounds start at the origin.
func toRGBA(img image.Image) *image.RGBA {
	if r, ok := img.(*image.RGBA); ok && r.Bounds().Min == (image.Point{}) {
		return r
	}
	b := img.Bounds()
	dst := image.NewRGBA(image.Rect(0, 0, b.Dx(), b.Dy()))
	draw.Draw(dst, dst.Bounds(), img, b.Min, draw.Src)
	return dst
}

// Render returns the complete CoreArt file bytes for img at size x size.
func Render(img image.Image, size int) []byte {
	px := ToRGB565(img, size)
	out := make([]byte, HeaderLen+len(px)*2)
	copy(out[0:4], Magic[:])
	binary.LittleEndian.PutUint16(out[4:6], Version)
	binary.LittleEndian.PutUint16(out[6:8], uint16(size))
	binary.LittleEndian.PutUint16(out[8:10], uint16(size))
	binary.LittleEndian.PutUint16(out[10:12], 0)
	for i, v := range px {
		binary.LittleEndian.PutUint16(out[HeaderLen+i*2:], v)
	}
	return out
}

// Valid reports whether b is a sidecar the firmware would accept, reproducing
// load_one() in core/ui/artcache.c line for line: header present, "CART" magic,
// 0 < w,h <= ARTCACHE_MAX_DIM, and enough bytes behind the header for w*h
// pixels. Note what the firmware does NOT check — the version word and the
// reserved word are never read — so neither does this.
//
// size > 0 additionally requires the sidecar to be exactly that square, which
// is how a caller asserts "this is the 28x28 the list chip needs, not some
// other size the firmware would tolerate but resample". size <= 0 asks only
// the firmware's own question.
func Valid(b []byte, size int) bool {
	if len(b) < HeaderLen {
		return false
	}
	if b[0] != 'C' || b[1] != 'A' || b[2] != 'R' || b[3] != 'T' {
		return false
	}
	w := int(b[6]) | int(b[7])<<8
	h := int(b[8]) | int(b[9])<<8
	if w <= 0 || h <= 0 || w > MaxDim || h > MaxDim {
		return false
	}
	if HeaderLen+w*h*2 > len(b) {
		return false
	}
	if size > 0 && (w != size || h != size) {
		return false
	}
	return true
}

// Result says what WriteAlbum did.
type Result struct {
	// Dir is the album folder that was written into.
	Dir string
	// ArtPath and ThumbPath are the files written, or "" when that sidecar
	// was not written.
	ArtPath   string
	ThumbPath string
	// ArtDim and ThumbDim are the squares actually rendered.
	ArtDim   int
	ThumbDim int
	// NoPicture is true when the FLAC carried no PICTURE block. That is not
	// an error: plenty of albums simply have no embedded cover, and the
	// firmware draws a placeholder chip for them.
	NoPicture bool
	// SrcW and SrcH are the decoded source picture's pixel dimensions, for
	// the one-line summary the CLI prints.
	SrcW, SrcH int
}

// Wrote reports whether any sidecar was written.
func (r Result) Wrote() bool { return r.ArtPath != "" || r.ThumbPath != "" }

// WriteAlbum renders folder.art and folder.thm into dir from the cover
// embedded in first.
//
// Both sidecars come from the same decoded source picture, scaled
// independently — the 28 is NOT a downscale of the 120. Each is written to a
// temp file in dir and renamed into place, so a reader (including the device,
// mid-sync) sees either the previous sidecar or the complete new one, never a
// truncated prefix that would fail the firmware's length check and latch the
// album as "no art" for the rest of the session.
//
// A file with no picture returns a Result with NoPicture set and a nil error.
func WriteAlbum(dir string, first *flac.Meta) (Result, error) {
	return WriteAlbumPicture(dir, first.FrontCover())
}

// WriteAlbumPicture is WriteAlbum given the cover directly, which is what the
// callers that read it with ReadCover have — the source may be a FLAC PICTURE
// block or an MP3's APIC frame, and by this point the difference is gone.
// A nil picture returns a Result with NoPicture set and a nil error.
func WriteAlbumPicture(dir string, pic *flac.Picture) (Result, error) {
	res := Result{Dir: dir}
	if pic == nil {
		res.NoPicture = true
		return res, nil
	}
	img, err := FromPicture(pic)
	if err != nil {
		if errors.Is(err, ErrNoPicture) {
			res.NoPicture = true
			return res, nil
		}
		return res, err
	}
	res.SrcW, res.SrcH = img.Bounds().Dx(), img.Bounds().Dy()

	artPath := filepath.Join(dir, ArtName)
	thumbPath := filepath.Join(dir, ThumbName)
	if err := writeAtomic(artPath, Render(img, ArtSize)); err != nil {
		return res, err
	}
	res.ArtPath, res.ArtDim = artPath, ArtSize
	if err := writeAtomic(thumbPath, Render(img, ThumbSize)); err != nil {
		return res, err
	}
	res.ThumbPath, res.ThumbDim = thumbPath, ThumbSize
	return res, nil
}

// writeAtomic writes b to path via a temp file in the same directory.
func writeAtomic(path string, b []byte) error {
	dir := filepath.Dir(path)
	f, err := os.CreateTemp(dir, "."+filepath.Base(path)+".tmp*")
	if err != nil {
		return fmt.Errorf("create temp for %s: %w", path, err)
	}
	tmp := f.Name()
	defer func() {
		if tmp != "" {
			_ = os.Remove(tmp)
		}
	}()
	if _, err := f.Write(b); err != nil {
		f.Close()
		return fmt.Errorf("write %s: %w", tmp, err)
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return fmt.Errorf("sync %s: %w", tmp, err)
	}
	if err := f.Close(); err != nil {
		return fmt.Errorf("close %s: %w", tmp, err)
	}
	// CreateTemp makes the file 0600; sidecars sit next to the music and are
	// read by whatever mounts the device, so widen to the usual 0644.
	if err := os.Chmod(tmp, 0o644); err != nil {
		return fmt.Errorf("chmod %s: %w", tmp, err)
	}
	if err := os.Rename(tmp, path); err != nil {
		return fmt.Errorf("rename %s -> %s: %w", tmp, path, err)
	}
	tmp = ""
	return nil
}

// FirstAudio returns the first playable file in dir, matching
// tools/coreart.py's rule: each extension globbed separately and the union
// sorted, so on a case-sensitive filesystem every lowercase name sorts before
// every uppercase one only by code point, exactly as Python's sorted() orders
// them. Returns "" when the folder holds none.
func FirstAudio(dir string) (string, error) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return "", err
	}
	var names []string
	for _, e := range ents {
		if e.IsDir() {
			continue
		}
		n := e.Name()
		for _, ext := range []string{".flac", ".FLAC", ".mp3", ".MP3"} {
			if strings.HasSuffix(n, ext) {
				names = append(names, n)
				break
			}
		}
	}
	if len(names) == 0 {
		return "", nil
	}
	sort.Strings(names)
	return filepath.Join(dir, names[0]), nil
}

// ReadCover returns one file's embedded front cover, whatever format it is: a
// FLAC PICTURE block or an MP3 APIC frame, both of which the rest of the art
// pipeline sees as the same flac.Picture. Returns nil (and no error) when the
// file parses but carries no picture.
func ReadCover(path string) (*flac.Picture, error) {
	if strings.EqualFold(filepath.Ext(path), ".mp3") {
		m, err := id3.ReadFile(path)
		if err != nil {
			return nil, err
		}
		return m.FrontCover(), nil
	}
	m, err := flac.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return m.FrontCover(), nil
}
