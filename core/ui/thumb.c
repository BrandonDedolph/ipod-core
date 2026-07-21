/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/thumb.c — freestanding nearest-neighbor RGB565 downscaler.
 *
 * The whole module is one pure function: no state, no allocation, no libc.
 * The only arithmetic is integer multiply/divide, which the ARM7TDMI's
 * runtime (__aeabi_idiv, already linked via -lgcc) handles — so this drops
 * straight into the freestanding core.elf UI path as well as the host test.
 *
 * Nearest-neighbor keeps it cheap and exact: each destination pixel maps to
 * exactly one source pixel via (y*src_h/dst_h, x*src_w/dst_w). For a shrink
 * (dst <= src) the largest index the map yields is (dst-1)*src/dst < src, so
 * every read stays in bounds without a clamp; the guards below only reject
 * degenerate (non-positive) dimensions.
 */

#include "thumb.h"

void thumb_downscale_rgb565(const uint16_t *src, int src_w, int src_h,
                            uint16_t *dst, int dst_w, int dst_h)
{
    if (src == 0 || dst == 0) {
        return;
    }
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = (y * src_h) / dst_h;          /* in [0, src_h) */
        const uint16_t *srow = src + (long)sy * src_w;
        uint16_t *drow = dst + (long)y * dst_w;
        for (int x = 0; x < dst_w; x++) {
            int sx = (x * src_w) / dst_w;       /* in [0, src_w) */
            drow[x] = srow[sx];
        }
    }
}
