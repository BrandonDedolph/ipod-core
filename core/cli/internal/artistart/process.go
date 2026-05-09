package artistart

import (
	"bytes"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"

	_ "image/png" /* register PNG decoder for image.Decode */
)

// Process decodes raw image bytes from any registered format (JPEG /
// PNG today), box-averages it down to fit within `maxDim` × `maxDim`
// preserving aspect, and re-encodes as JPEG at the given quality.
//
// Returned bytes are typically 5–15 KB at 128×128 q=85, suitable for
// embedding in a binary tagcache. The firmware decodes them through
// the existing image_jpeg_decode_rgb565 path with no special-casing.
func Process(raw []byte, maxDim int, quality int) ([]byte, error) {
	if maxDim <= 0 || maxDim > 4096 {
		return nil, errors.New("maxDim out of range")
	}
	if quality < 1 || quality > 100 {
		return nil, errors.New("quality out of range")
	}
	src, _, err := image.Decode(bytes.NewReader(raw))
	if err != nil {
		return nil, fmt.Errorf("decode: %w", err)
	}
	dstW, dstH := fitBox(src.Bounds().Dx(), src.Bounds().Dy(), maxDim, maxDim)
	dst := boxDownscale(src, dstW, dstH)

	var out bytes.Buffer
	if err := jpeg.Encode(&out, dst, &jpeg.Options{Quality: quality}); err != nil {
		return nil, fmt.Errorf("encode: %w", err)
	}
	return out.Bytes(), nil
}

// fitBox returns the largest (w, h) <= (maxW, maxH) that preserves
// the source aspect ratio. Caller-side rounding is tolerant: a
// landscape 2000×1200 fitted into 128×128 returns 128×77.
func fitBox(srcW, srcH, maxW, maxH int) (int, int) {
	if srcW <= maxW && srcH <= maxH {
		return srcW, srcH
	}
	/* Compare aspect ratios via cross-multiplication to stay in
	 * integer math; positive vs negative tells us which axis
	 * dominates. */
	if srcW*maxH > srcH*maxW {
		return maxW, srcH * maxW / srcW
	}
	return srcW * maxH / srcH, maxH
}

// boxDownscale produces a dstW × dstH RGBA image whose pixels are the
// unweighted mean of all source pixels in their corresponding cell.
// Same algorithm as the firmware's image_jpeg_decode_rgb565 box pass —
// keeps the host-side preview consistent with what hardware will
// render. Source images smaller than the dst on either axis collapse
// to nearest-neighbor on that axis (cell width 1).
func boxDownscale(src image.Image, dstW, dstH int) *image.RGBA {
	dst := image.NewRGBA(image.Rect(0, 0, dstW, dstH))
	b := src.Bounds()
	srcW, srcH := b.Dx(), b.Dy()

	for y := 0; y < dstH; y++ {
		sy0 := y * srcH / dstH
		sy1 := (y + 1) * srcH / dstH
		if sy1 <= sy0 {
			sy1 = sy0 + 1
		}
		if sy1 > srcH {
			sy1 = srcH
		}
		for x := 0; x < dstW; x++ {
			sx0 := x * srcW / dstW
			sx1 := (x + 1) * srcW / dstW
			if sx1 <= sx0 {
				sx1 = sx0 + 1
			}
			if sx1 > srcW {
				sx1 = srcW
			}
			var rSum, gSum, bSum, count uint64
			for j := sy0; j < sy1; j++ {
				for i := sx0; i < sx1; i++ {
					r, g, bb, _ := src.At(b.Min.X+i, b.Min.Y+j).RGBA()
					/* RGBA() returns 16-bit values; shift to 8-bit. */
					rSum += uint64(r >> 8)
					gSum += uint64(g >> 8)
					bSum += uint64(bb >> 8)
					count++
				}
			}
			dst.SetRGBA(x, y, color.RGBA{
				R: uint8(rSum / count),
				G: uint8(gSum / count),
				B: uint8(bSum / count),
				A: 255,
			})
		}
	}
	return dst
}
