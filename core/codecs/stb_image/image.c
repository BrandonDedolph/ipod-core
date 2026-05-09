/*
 * core/codecs/stb_image/image.c — JPEG decode wrapper using stb_image.
 *
 * stb_image is a multi-format image loader; we compile it JPEG-only
 * via STBI_NO_* defines so the binary doesn't carry PNG / BMP / TGA /
 * etc. STBI_NO_STDIO drops fopen-based loaders — we always decode from
 * memory buffers (FLAC PICTURE / ID3v2 APIC payloads). STBI_NO_LINEAR
 * and STBI_NO_HDR drop float-output paths we don't need.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"

#include "image.h"

int image_jpeg_decode_rgb565(const void *bytes, size_t len,
                             int target_w, int target_h,
                             uint16_t *out) {
    if (!bytes || len == 0 || target_w <= 0 || target_h <= 0 || !out) {
        return -1;
    }

    int w = 0, h = 0, src_channels = 0;
    /* Force 3 channels (RGB) regardless of source format. stbi handles
     * grayscale/RGB transparently. We don't need alpha for album art. */
    unsigned char *src = stbi_load_from_memory(
        (const stbi_uc *)bytes, (int)len, &w, &h, &src_channels, 3);
    if (!src || w <= 0 || h <= 0) {
        if (src) stbi_image_free(src);
        return -1;
    }

    /* Box-average downscale to RGB888, then quantize to RGB565.
     *
     * Each destination pixel covers a rectangle of source pixels and
     * gets the unweighted mean of all RGB triples in that rectangle.
     * Drops detail much more gracefully than nearest-neighbor at the
     * downscale ratios album art hits in practice — a 500×500 source
     * resampled to a 22×22 list thumb visits ~520 source pixels per
     * dest, so NN throws ~99.8% of the data away, while box average
     * preserves it as smoothed luminance. The visible payoff is on
     * any thumbnail where detail (text, faces, fine textures) used to
     * read as random noise.
     *
     * If the target is bigger than the source on either axis the
     * source rectangle collapses to one pixel and we degenerate to
     * NN sampling — fine, since album art is essentially always at
     * least our target size.
     *
     * Averaging is in sRGB rather than linear light. That's slightly
     * incorrect for high-contrast edges (the result reads ~5% darker
     * than a gamma-correct mean) but matches what stb_image_resize
     * does by default and is plenty good for tiny album-art tiles. */
    for (int y = 0; y < target_h; y++) {
        int sy0 = (y     * h) / target_h;
        int sy1 = ((y+1) * h) / target_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h)    sy1 = h;
        for (int x = 0; x < target_w; x++) {
            int sx0 = (x     * w) / target_w;
            int sx1 = ((x+1) * w) / target_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w)    sx1 = w;

            uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
            uint32_t count = 0;
            for (int j = sy0; j < sy1; j++) {
                const unsigned char *row = src + (size_t)j * (size_t)w * 3;
                for (int i = sx0; i < sx1; i++) {
                    const unsigned char *p = row + (size_t)i * 3;
                    r_sum += p[0];
                    g_sum += p[1];
                    b_sum += p[2];
                    count++;
                }
            }
            uint8_t r = (uint8_t)(r_sum / count);
            uint8_t g = (uint8_t)(g_sum / count);
            uint8_t b = (uint8_t)(b_sum / count);
            uint16_t r5 = (uint16_t)((r & 0xF8) << 8);
            uint16_t g6 = (uint16_t)((g & 0xFC) << 3);
            uint16_t b5 = (uint16_t)( b         >> 3);
            out[y * target_w + x] = (uint16_t)(r5 | g6 | b5);
        }
    }

    stbi_image_free(src);
    return 0;
}
