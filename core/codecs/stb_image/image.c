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

    /* Nearest-neighbor downscale + RGB888 -> RGB565. We use 32-bit
     * intermediates for the (sx = x * w / target_w) computation to
     * avoid overflow on huge inputs (a 16384×16384 JPEG with target
     * 84 would compute x*w up to ~1.4M — well under int32 max). */
    for (int y = 0; y < target_h; y++) {
        int sy = (y * h) / target_h;
        if (sy >= h) sy = h - 1;
        const unsigned char *row = src + (size_t)sy * (size_t)w * 3;
        for (int x = 0; x < target_w; x++) {
            int sx = (x * w) / target_w;
            if (sx >= w) sx = w - 1;
            const unsigned char *p = row + (size_t)sx * 3;
            uint16_t r = (uint16_t)((p[0] & 0xF8) << 8);
            uint16_t g = (uint16_t)((p[1] & 0xFC) << 3);
            uint16_t b = (uint16_t)( p[2]         >> 3);
            out[y * target_w + x] = (uint16_t)(r | g | b);
        }
    }

    stbi_image_free(src);
    return 0;
}
