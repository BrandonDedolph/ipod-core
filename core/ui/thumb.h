/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/thumb.h — freestanding nearest-neighbor RGB565 downscaler.
 *
 * Album lists want a small cover chip next to each row, and album-detail
 * screens a mid-size cover — but the device can't decode JPEG at runtime.
 * The host tools/coreart.py pre-renders a full-size folder.art (120x120)
 * and a small folder.thm (24x24) per album. When a dedicated folder.thm
 * isn't present, this shrinks the already-loaded folder.art to whatever
 * box the layout needs (e.g. 56x56 detail header, 22x22 list chip).
 *
 * Integer-only, no libc/libm/malloc, no allocation (the caller owns dst):
 * a pure box resampler that links into core.elf and the host thumb_test.
 */

#ifndef CORE_UI_THUMB_H
#define CORE_UI_THUMB_H

#include <stdint.h>

/*
 * Downscale (or copy) the src_w x src_h RGB565 image `src` into the
 * dst_w x dst_h RGB565 box `dst`, both row-major uint16_t. Uses
 * nearest-neighbor sampling:
 *
 *     dst[y*dst_w + x] = src[(y*src_h/dst_h)*src_w + (x*src_w/dst_w)]
 *
 * The map lands in [0,src_w)x[0,src_h) for every dst pixel, so it never
 * reads out of `src` even for non-integer ratios (e.g. 120 -> 22). When
 * dst dims equal src dims it is an exact 1:1 copy. `dst` must hold at
 * least dst_w*dst_h pixels; buffers must not overlap. If any dimension
 * is <= 0 the call is a no-op.
 */
void thumb_downscale_rgb565(const uint16_t *src, int src_w, int src_h,
                            uint16_t *dst, int dst_w, int dst_h);

#endif /* CORE_UI_THUMB_H */
