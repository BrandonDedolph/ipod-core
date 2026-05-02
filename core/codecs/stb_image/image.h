/*
 * core/codecs/stb_image/image.h — JPEG decode + scale to RGB565.
 *
 * Wraps stb_image (JPEG-only build) for the album-art use case. Decodes
 * a JPEG byte buffer, downscales to a target width × height with
 * nearest-neighbor sampling, converts to RGB565 little-endian (matching
 * the LCD framebuffer layout).
 *
 * Why JPEG-only and nearest-neighbor: album art on a 320×240 iPod
 * panel is typically downscaled from 500×500+ to 84×84 or 180×180.
 * Nearest-neighbor introduces some shimmer on diagonal edges but is
 * cheap (no float, no filter kernels) and the perceptual loss against
 * a 16-bit-color tiny target is negligible. JPEG-only because that's
 * what FLAC PICTURE blocks and ID3v2 APIC frames carry in the wild;
 * vendoring stb's PNG decoder too would double our binary size for a
 * format we don't see.
 *
 * Memory: stbi_load_from_memory mallocs a `width*height*3` RGB buffer
 * internally (heap), then we scale + free. No persistent allocations.
 *
 * On hw the JPEG decode will need a budgeted heap; for sim use, libc
 * malloc is fine.
 */

#ifndef CORE_CODECS_STB_IMAGE_IMAGE_H
#define CORE_CODECS_STB_IMAGE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Decode `bytes`/`len` (a complete JPEG file in memory), nearest-
 * neighbor downscale to `target_w × target_h`, and write RGB565 little-
 * endian into `out` (row-major, target_w * target_h pixels).
 *
 * Returns 0 on success, -1 on any failure (not a JPEG, malformed,
 * decoder OOM). On failure `out` is unmodified.
 *
 * `target_w` and `target_h` must both be > 0; `out` must have room
 * for at least target_w * target_h * sizeof(uint16_t) bytes.
 */
int image_jpeg_decode_rgb565(const void *bytes, size_t len,
                             int target_w, int target_h,
                             uint16_t *out);

#endif /* CORE_CODECS_STB_IMAGE_IMAGE_H */
