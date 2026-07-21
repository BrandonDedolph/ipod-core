/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/thumb_test.c — host test for the nearest-neighbor RGB565 downscaler.
 *
 * thumb.c (ui/thumb.c) shrinks a loaded folder.art (120x120) to a list chip or
 * album-detail cover when no dedicated folder.thm is on disk. It is pure integer
 * math with no allocation, so the on-device path and this test compile the SAME
 * source. This proves three things:
 *   1. Identity: dst dims == src dims is an exact 1:1 copy.
 *   2. Correctness: a hand-worked 4x4 -> 2x2 picks the expected source pixels.
 *   3. Bounds: a non-integer ratio (120 -> 22) reads only in-range source pixels
 *      and never writes past dst (sentinel guard cells bracket the buffer).
 */

#include "thumb.h"

#include <stdint.h>
#include <stdio.h>

static int g_fail = 0;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fail = 1;
    }
    return cond;
}

int main(void)
{
    /* --- Test 1: identity when dst dims == src dims --- */
    {
        enum { W = 5, H = 3 };
        uint16_t src[W * H], dst[W * H];
        for (int i = 0; i < W * H; i++) {
            src[i] = (uint16_t)(0x1000 + i);
            dst[i] = 0xFFFF;
        }
        thumb_downscale_rgb565(src, W, H, dst, W, H);
        int ok = 1;
        for (int i = 0; i < W * H; i++) {
            if (dst[i] != src[i]) {
                ok = 0;
            }
        }
        check("identity-copy", ok);
    }

    /* --- Test 2: hand-worked 4x4 -> 2x2 --- *
     * sx = x*4/2 -> {0, 2};  sy = y*4/2 -> {0, 2}
     * so dst picks src indices: (0,0)=0, (0,2)=2, (2,0)=8, (2,2)=10 */
    {
        uint16_t src[16], dst[4];
        for (int i = 0; i < 16; i++) {
            src[i] = (uint16_t)(100 + i);
        }
        thumb_downscale_rgb565(src, 4, 4, dst, 2, 2);
        int ok = dst[0] == src[0] && dst[1] == src[2] &&
                 dst[2] == src[8] && dst[3] == src[10];
        printf("  4x4->2x2 got {%u,%u,%u,%u} want {%u,%u,%u,%u}\n",
               dst[0], dst[1], dst[2], dst[3],
               src[0], src[2], src[8], src[10]);
        check("downscale-4x4-to-2x2", ok);
    }

    /* --- Test 3: non-integer ratio 120 -> 22, in-bounds + no OOB write --- */
    {
        enum { SW = 120, SH = 120, DW = 22, DH = 22 };
        static uint16_t src[SW * SH];
        /* dst bracketed by sentinel cells: [0]=head guard, [DW*DH+1]=tail guard */
        static uint16_t dstbuf[DW * DH + 2];
        for (int i = 0; i < SW * SH; i++) {
            src[i] = (uint16_t)(i & 0xFFFF);
        }
        dstbuf[0] = 0xDEAD;
        dstbuf[DW * DH + 1] = 0xBEEF;
        uint16_t *dst = &dstbuf[1];

        thumb_downscale_rgb565(src, SW, SH, dst, DW, DH);

        /* Every dst pixel must equal the exact source pixel the map selects,
         * and that source index must be strictly inside src. */
        int ok = 1, max_idx = 0;
        for (int y = 0; y < DH; y++) {
            for (int x = 0; x < DW; x++) {
                int sx = (x * SW) / DW;
                int sy = (y * SH) / DH;
                int idx = sy * SW + sx;
                if (idx < 0 || idx >= SW * SH) {
                    ok = 0;
                }
                if (idx > max_idx) {
                    max_idx = idx;
                }
                if (dst[y * DW + x] != src[idx]) {
                    ok = 0;
                }
            }
        }
        printf("  120->22 max src index used=%d (limit=%d)\n", max_idx, SW * SH);
        check("downscale-120-to-22-inbounds", ok && max_idx < SW * SH);
        check("downscale-120-to-22-no-oob-write",
              dstbuf[0] == 0xDEAD && dstbuf[DW * DH + 1] == 0xBEEF);
    }

    /* --- Test 4: zero/negative dims are a no-op (dst untouched) --- */
    {
        uint16_t src[4] = { 1, 2, 3, 4 };
        uint16_t dst[4] = { 0xAAAA, 0xAAAA, 0xAAAA, 0xAAAA };
        thumb_downscale_rgb565(src, 2, 2, dst, 0, 2);
        thumb_downscale_rgb565(src, 2, 2, dst, 2, -1);
        int ok = dst[0] == 0xAAAA && dst[1] == 0xAAAA &&
                 dst[2] == 0xAAAA && dst[3] == 0xAAAA;
        check("zero-dims-noop", ok);
    }

    printf("thumb_test: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
