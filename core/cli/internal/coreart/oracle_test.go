package coreart

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// The art oracle: hold this package's pipeline to tools/coreart.py's, which is
// ffmpeg's. Same stretch to N x N, same lanczos (a = 3), same rounding.
//
// WHERE THE REFERENCE IS TAKEN. The enforced comparison is against ffmpeg's
// rgb24 output at the target size — that is, after the decode and the scale,
// but BEFORE the 16-bit quantisation. The reason is measured, not assumed:
// swscale dithers its RGB565 output with a fixed ordered pattern that cannot be
// switched off. `-sws_dither none` documents itself as "no dithering" but only
// reaches the explicit scale filter; the RGB565 pack happens in the
// auto-inserted output converter, and a 16x16 patch of a single solid colour
// still comes back as three distinct 565 words in a positional pattern (checked
// on ffmpeg here, with the flag passed globally, as a scale= option, with the
// conversion forced into the filter chain, and with accurate_rnd — all four
// dither identically). Differencing a dithered quantiser against our
// deterministic one measures ffmpeg's dither, not our pipeline: on the 99 MC
// covers it alone contributes mean 2.14 with NO scaling involved, which
// exhausts the whole budget below before the scaler has done anything.
//
// So the plan's bound is applied where it says what it means to say — the
// pixels we compute — and the RGB565 comparison the plan literally writes is
// kept as a reported diagnostic (TestOracleFFmpeg565), printed next to the
// dither floor so the residual can be attributed rather than argued about.
//
// Both sides are expanded to 8 bits per channel before differencing, so the
// numbers are in familiar units; one 5-bit level is ~8.2 of them.
const (
	oracleMeanLimit = 2.0
	oracleP99Limit  = 12
	oracleMaxLimit  = 40
)

// The 565 diagnostic does not get a fixed ceiling — a fixed number would be
// measuring ffmpeg's dither, which varies with the picture. It is held instead
// to the dither floor it prints beside itself: our pixels may sit at most the
// plan's own tolerance further from ffmpeg's RGB565 than ffmpeg's OWN rgb24
// does once our quantiser is applied to it. That is the strongest statement
// this comparison can make, and it still fails loudly on a channel swap, a
// byte-order flip or a lost scale step.

func findFFmpeg(t *testing.T) string {
	t.Helper()
	if p, err := exec.LookPath("ffmpeg"); err == nil {
		return p
	}
	// Debian/WSL puts it in /usr/sbin, which is not on a non-root PATH.
	for _, p := range []string{"/usr/sbin/ffmpeg", "/usr/local/bin/ffmpeg"} {
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	t.Skip("ffmpeg not found — the art oracle needs it")
	return ""
}

// dist accumulates absolute per-channel differences without keeping them: the
// values are 0..255, so a 256-bin histogram is exact for the mean, the
// percentile and the max alike.
type dist struct {
	bins  [256]uint64
	count uint64
	sum   uint64
}

func (d *dist) add(delta int) {
	if delta < 0 {
		delta = -delta
	}
	d.bins[delta]++
	d.count++
	d.sum += uint64(delta)
}

func (d *dist) merge(o *dist) {
	for i := range d.bins {
		d.bins[i] += o.bins[i]
	}
	d.count += o.count
	d.sum += o.sum
}

func (d *dist) mean() float64 {
	if d.count == 0 {
		return 0
	}
	return float64(d.sum) / float64(d.count)
}

// percentile returns the smallest delta v such that at least p of the samples
// are <= v.
func (d *dist) percentile(p float64) int {
	if d.count == 0 {
		return 0
	}
	target := uint64(p * float64(d.count))
	var seen uint64
	for v, n := range d.bins {
		seen += n
		if seen >= target {
			return v
		}
	}
	return 255
}

func (d *dist) max() int {
	for v := 255; v >= 0; v-- {
		if d.bins[v] > 0 {
			return v
		}
	}
	return 0
}

func (d *dist) String() string {
	return fmt.Sprintf("mean %.3f  p99 %d  max %d  (%d samples)",
		d.mean(), d.percentile(0.99), d.max(), d.count)
}

// expand5/expand6 are the exact inverses of pack565's quantisers: the 8-bit
// value a level stands for. Bit replication (v<<3 | v>>2) would do almost as
// well, but it biases low by up to 4 at the top of the range, which would show
// up in the mean as a difference that is ours, not ffmpeg's.
func expand5(v int) int { return (v*255 + 15) / 31 }
func expand6(v int) int { return (v*255 + 31) / 63 }

// oracleSource is one picture to compare on.
type oracleSource struct {
	name string // album folder name, or the synthetic fixture's name
	path string // the picture on disk, ready for ffmpeg -i
	img  image.Image
}

// runFFmpeg is tools/coreart.py's extract() invocation, parameterised by
// output pixel format so the same scale can be read both before (rgb24) and
// after (rgb565le) ffmpeg's quantiser.
func runFFmpeg(t *testing.T, ffmpeg, picPath string, size int, pixFmt string, wantBytes int) []byte {
	t.Helper()
	cmd := exec.Command(ffmpeg, "-v", "error", "-i", picPath,
		"-vf", fmt.Sprintf("scale=%d:%d:flags=lanczos", size, size),
		"-sws_dither", "none",
		"-pix_fmt", pixFmt, "-f", "rawvideo", "-")
	var out, errb bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &errb
	if err := cmd.Run(); err != nil {
		t.Fatalf("ffmpeg %s @%d %s: %v\n%s", picPath, size, pixFmt, err, errb.String())
	}
	raw := out.Bytes()
	if len(raw) != wantBytes {
		t.Fatalf("ffmpeg %s @%d %s: %d bytes, want %d\n%s",
			picPath, size, pixFmt, len(raw), wantBytes, errb.String())
	}
	return raw
}

func unpack565(raw []byte) []uint16 {
	px := make([]uint16, len(raw)/2)
	for i := range px {
		px[i] = binary.LittleEndian.Uint16(raw[i*2:])
	}
	return px
}

// scaleToRGBA is ToRGB565's first half: everything up to, but not including,
// the 5/6/5 pack.
func scaleToRGBA(img image.Image, size int) *image.RGBA {
	src := toRGBA(img)
	dst := image.NewRGBA(image.Rect(0, 0, size, size))
	lanczos3.Scale(dst, dst.Bounds(), src, src.Bounds(), srcOp, nil)
	return dst
}

// compareRGB24 differences our scaled 8-bit pixels against ffmpeg's.
func compareRGB24(ours *image.RGBA, theirs []byte) *dist {
	d := &dist{}
	n := len(theirs) / 3
	for i := 0; i < n; i++ {
		for c := 0; c < 3; c++ {
			d.add(int(ours.Pix[i*4+c]) - int(theirs[i*3+c]))
		}
	}
	return d
}

func compare(ours, theirs []uint16) *dist {
	d := &dist{}
	for i := range ours {
		a, b := ours[i], theirs[i]
		d.add(expand5(int(a>>11)&31) - expand5(int(b>>11)&31))
		d.add(expand6(int(a>>5)&63) - expand6(int(b>>5)&63))
		d.add(expand5(int(a)&31) - expand5(int(b)&31))
	}
	return d
}

// syntheticJPEG is the fallback subject when the MC tree is not mounted.
//
// It has to behave like an album cover, not like a scaler test chart. A
// checkerboard, a one-pixel diagonal or a long axis-aligned hard edge is where
// any two resamplers disagree most: the output there is decided by aliasing and
// by sub-pixel phase, so differencing two different aliases says nothing about
// whether the two pipelines agree. (Measured: a fixture built that way scored
// mean 7.4 at N=28 while the median real cover scores 1.05.)
//
// Real cover art — photographs, paintings, grain — has a broadband spectrum:
// detail at every scale, no single edge carrying the whole error. So this is
// four octaves of value noise per channel over a smooth two-axis ramp, with a
// soft vignette, encoded at quality 90 so Go's encoder subsamples chroma 4:2:0
// and the JPEG's own chroma handling is part of the subject too. Deterministic:
// the noise is a hash of the lattice coordinates, so the fixture is the same
// picture on every machine and no binary has to be checked in.
func syntheticJPEG(t *testing.T, dir string) oracleSource {
	t.Helper()
	const n = 600
	img := image.NewRGBA(image.Rect(0, 0, n, n))
	for y := 0; y < n; y++ {
		fy := float64(y) / float64(n-1)
		for x := 0; x < n; x++ {
			fx := float64(x) / float64(n-1)
			base := [3]float64{40 + fx*150, 70 + fy*130, 110 + (fx+fy)*55}
			var px [3]float64
			for c := 0; c < 3; c++ {
				v := base[c]
				amp, freq := 60.0, 2.6
				for oct := 0; oct < 4; oct++ {
					v += amp * valueNoise(fx*freq, fy*freq, c*17+oct)
					amp, freq = amp*0.34, freq*2.2
				}
				// Soft vignette: a low-frequency multiplicative shade, the
				// commonest large-scale structure in real cover art.
				dx, dy := fx-0.5, fy-0.5
				v *= 1.0 - 0.45*(dx*dx+dy*dy)
				px[c] = v
			}
			img.Set(x, y, color.RGBA{clamp8(px[0]), clamp8(px[1]), clamp8(px[2]), 255})
		}
	}
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: 90}); err != nil {
		t.Fatalf("encode jpeg: %v", err)
	}
	path := filepath.Join(dir, "synthetic.jpg")
	if err := os.WriteFile(path, buf.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	dec, _, err := image.Decode(bytes.NewReader(buf.Bytes()))
	if err != nil {
		t.Fatalf("decode jpeg: %v", err)
	}
	return oracleSource{name: fmt.Sprintf("synthetic %dx%d q90", n, n), path: path, img: dec}
}

func clamp8(v float64) uint8 {
	if v < 0 {
		return 0
	}
	if v > 255 {
		return 255
	}
	return uint8(v + 0.5)
}

// valueNoise is smoothstep-interpolated lattice noise in [-1, 1].
func valueNoise(x, y float64, seed int) float64 {
	xi, yi := math.Floor(x), math.Floor(y)
	tx, ty := smoothstep(x-xi), smoothstep(y-yi)
	x0, y0 := int(xi), int(yi)
	v00 := lattice(x0, y0, seed)
	v10 := lattice(x0+1, y0, seed)
	v01 := lattice(x0, y0+1, seed)
	v11 := lattice(x0+1, y0+1, seed)
	return (v00*(1-tx)+v10*tx)*(1-ty) + (v01*(1-tx)+v11*tx)*ty
}

func smoothstep(t float64) float64 { return t * t * (3 - 2*t) }

// lattice hashes a lattice point to [-1, 1]. Splitmix-style mixing; the exact
// constants do not matter, only that the result is well spread and stable.
func lattice(x, y, seed int) float64 {
	h := uint64(x)*0x9E3779B97F4A7C15 ^ uint64(y)*0xBF58476D1CE4E5B9 ^ uint64(seed)*0x94D049BB133111EB
	h ^= h >> 30
	h *= 0xBF58476D1CE4E5B9
	h ^= h >> 27
	h *= 0x94D049BB133111EB
	h ^= h >> 31
	return float64(h>>11)/float64(1<<53)*2 - 1
}

// mcSources extracts the front cover of the first FLAC in every immediate
// subfolder of root. Albums with no FLAC or no PICTURE block are simply absent
// from the result (t.Log says how many).
func mcSources(t *testing.T, root, tmp string) []oracleSource {
	t.Helper()
	ents, err := os.ReadDir(root)
	if err != nil {
		t.Skipf("CORE_PARITY_SRC=%s: %v", root, err)
	}
	var dirs []string
	for _, e := range ents {
		if e.IsDir() {
			dirs = append(dirs, e.Name())
		}
	}
	sort.Strings(dirs)

	var srcs []oracleSource
	var noFLAC, noPic int
	for _, name := range dirs {
		dir := filepath.Join(root, name)
		f, err := FirstFLAC(dir)
		if err != nil || f == "" {
			noFLAC++
			continue
		}
		m, err := flac.ReadFile(f)
		if err != nil {
			t.Errorf("%s: %v", f, err)
			continue
		}
		pic := m.FrontCover()
		if pic == nil {
			noPic++
			continue
		}
		cfg, format, err := image.DecodeConfig(bytes.NewReader(pic.Data))
		if err != nil {
			t.Errorf("%s: cover (declared %s) does not decode: %v", f, pic.MIME, err)
			continue
		}
		ext := "." + format
		path := filepath.Join(tmp, fmt.Sprintf("%04d%s", len(srcs), ext))
		if err := os.WriteFile(path, pic.Data, 0o644); err != nil {
			t.Fatal(err)
		}
		img, err := FromPicture(pic)
		if err != nil {
			t.Errorf("%s: %v", f, err)
			continue
		}
		_ = cfg
		srcs = append(srcs, oracleSource{name: name, path: path, img: img})
	}
	t.Logf("CORE_PARITY_SRC=%s: %d folder(s), %d with a cover, %d with no FLAC, %d with no picture",
		root, len(dirs), len(srcs), noFLAC, noPic)
	return srcs
}

// oracleSources returns the pictures to compare on: every album cover under
// CORE_PARITY_SRC, or the synthetic fixture when that is not mounted.
func oracleSources(t *testing.T, tmp string) []oracleSource {
	t.Helper()
	if root := os.Getenv("CORE_PARITY_SRC"); root != "" {
		if srcs := mcSources(t, root, tmp); len(srcs) > 0 {
			return srcs
		}
	}
	// No MC tree (or nothing in it had a cover): the oracle still runs, on a
	// fixture generated here, so any box with ffmpeg gets the comparison
	// rather than a skip.
	return []oracleSource{syntheticJPEG(t, tmp)}
}

// TestOracleFFmpeg is the gate: our decode + Lanczos3 stretch against ffmpeg's,
// per channel in 8 bits. See the comment at the top of this file for why the
// reference is ffmpeg's rgb24 rather than its rgb565le.
//
// RULING (review, 2026-09-14): the plan's mean 2.0 / p99 12 bound is enforced
// over the CORPUS — every pixel of every source pooled — not per source. The
// per-source numbers are printed, and each source outside the plan's figures
// (including its max 40) is counted in a census, but they do not fail the
// test. The reason is what the outliers are: fine-detail 1280 px covers
// crushed 10-45x, where ffmpeg's YUV-space scaling and our RGB-space scaling
// legitimately land on different pixels around hard edges. On the 99 MC
// covers the corpus sits at mean 0.65 / p99 6 (N=120) and 1.43 / 11 (N=28)
// while 9 and 15 sources exceed a per-source gate; a per-source gate would be
// tuned to the worst cover in one person's library, and the corpus gate still
// fails loudly on a channel swap, a lost scale step or a wrong kernel (bilinear
// scores 6.4 / 13.2 here).
func TestOracleFFmpeg(t *testing.T) {
	ffmpeg := findFFmpeg(t)
	tmp := t.TempDir()
	srcs := oracleSources(t, tmp)

	for _, size := range []int{ArtSize, ThumbSize} {
		size := size
		t.Run(fmt.Sprintf("N%d", size), func(t *testing.T) {
			total := &dist{}
			var worstName string
			var worstMean float64
			var worstDist *dist
			var over int
			for _, s := range srcs {
				ours := scaleToRGBA(s.img, size)
				theirs := runFFmpeg(t, ffmpeg, s.path, size, "rgb24", size*size*3)
				d := compareRGB24(ours, theirs)
				total.merge(d)
				if d.mean() > worstMean {
					worstMean, worstName, worstDist = d.mean(), s.name, d
				}
				if d.mean() > oracleMeanLimit || d.percentile(0.99) > oracleP99Limit || d.max() > oracleMaxLimit {
					t.Logf("%s @%d: %s — outside the per-source figures (reported, not a failure)", s.name, size, d)
					over++
				}
			}
			t.Logf("N=%d over %d source(s): %s", size, len(srcs), total)
			if worstDist != nil {
				t.Logf("N=%d worst by mean: %q — %s", size, worstName, worstDist)
			}
			t.Logf("N=%d census: %d of %d source(s) outside mean %.1f / p99 %d / max %d",
				size, over, len(srcs), oracleMeanLimit, oracleP99Limit, oracleMaxLimit)
			if m := total.mean(); m > oracleMeanLimit {
				t.Errorf("N=%d corpus: %s — mean over %.1f", size, total, oracleMeanLimit)
			}
			if p := total.percentile(0.99); p > oracleP99Limit {
				t.Errorf("N=%d corpus: %s — p99 over %d", size, total, oracleP99Limit)
			}
		})
	}
}

// TestOracleFFmpeg565 runs the plan's literal comparison — our CoreArt pixels
// against ffmpeg's rgb565le — and REPORTS it. Alongside it, the dither floor:
// ffmpeg's own rgb24 at the same size, packed by our quantiser, differenced
// against ffmpeg's rgb565le. That floor is the part of the delta no
// implementation of this package could remove; whatever our number exceeds it
// by is ours. On the 99 MC covers the two are within a hair of each other.
func TestOracleFFmpeg565(t *testing.T) {
	ffmpeg := findFFmpeg(t)
	tmp := t.TempDir()
	srcs := oracleSources(t, tmp)

	for _, size := range []int{ArtSize, ThumbSize} {
		size := size
		t.Run(fmt.Sprintf("N%d", size), func(t *testing.T) {
			total, floor := &dist{}, &dist{}
			var worstName string
			var worstMean float64
			var worstDist *dist
			for _, s := range srcs {
				theirs := unpack565(runFFmpeg(t, ffmpeg, s.path, size, "rgb565le", size*size*2))
				ours := ToRGB565(s.img, size)
				d := compare(ours, theirs)
				total.merge(d)
				if d.mean() > worstMean {
					worstMean, worstName, worstDist = d.mean(), s.name, d
				}

				rgb24 := runFFmpeg(t, ffmpeg, s.path, size, "rgb24", size*size*3)
				requantised := make([]uint16, size*size)
				for i := range requantised {
					requantised[i] = pack565(rgb24[i*3], rgb24[i*3+1], rgb24[i*3+2])
				}
				floor.merge(compare(requantised, theirs))
			}
			t.Logf("N=%d over %d source(s):        ours vs ffmpeg 565: %s", size, len(srcs), total)
			t.Logf("N=%d dither floor (ffmpeg's own rgb24, our pack): %s", size, floor)
			if worstDist != nil {
				t.Logf("N=%d worst by mean: %q — %s", size, worstName, worstDist)
			}
			if m, f := total.mean(), floor.mean(); m > f+oracleMeanLimit {
				t.Errorf("N=%d: %s — mean is %.3f over the %.3f dither floor, more than the %.1f tolerance",
					size, total, m, f, oracleMeanLimit)
			}
			if p, f := total.percentile(0.99), floor.percentile(0.99); p > f+oracleP99Limit {
				t.Errorf("N=%d: %s — p99 is %d over the %d dither floor, more than %d",
					size, total, p, f, oracleP99Limit)
			}
			if mx, f := total.max(), floor.max(); mx > f+oracleMaxLimit {
				t.Errorf("N=%d: %s — max is %d over the %d dither floor, more than %d",
					size, total, mx, f, oracleMaxLimit)
			}
		})
	}
}
