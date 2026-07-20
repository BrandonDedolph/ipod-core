/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/pcm_ring_test.c — host test for the SPSC PCM ring.
 *
 * Single-threaded: true producer/consumer concurrency isn't deterministically
 * host-testable (and the release/acquire ordering is a compile-time property),
 * so this exercises the logic that bugs actually hide in — index math, the
 * power-of-two mask/wrap, free/fill accounting, over-write and under-read
 * clamping, and end-to-end FIFO byte integrity across many wrap cycles.
 *
 * Frame convention: frame n carries L=n, R=-n so a swapped or dropped channel
 * is caught, not just a dropped frame.
 */

#include "pcm_ring.h"

#include <stdint.h>
#include <stdio.h>

static int fails = 0;

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        fails++;
    }
    return cond;
}

int main(void)
{
    enum { CAP = 8 };                 /* small power-of-two to force wraps */
    static int16_t storage[CAP * 2];
    pcm_ring_t r;

    /* ---- init ---- */
    pcm_ring_init(&r, storage, CAP);
    check("init: fill == 0", pcm_ring_fill(&r) == 0);
    check("init: free == CAP", pcm_ring_free(&r) == CAP);

    /* Staging buffers sized for the largest chunk the FIFO loop uses
     * (CAP+2 frames), not just CAP — the loop reads/writes up to CAP+2. */
    enum { CHUNK = CAP + 2 };
    int16_t src[CHUNK * 2];
    int16_t dst[CHUNK * 2];

    /* ---- basic write/read ---- */
    for (int i = 0; i < CAP; i++) {
        src[i * 2 + 0] = (int16_t)i;
        src[i * 2 + 1] = (int16_t)-i;
    }

    check("write 3 returns 3", pcm_ring_write(&r, src, 3) == 3);
    check("after write 3: fill == 3", pcm_ring_fill(&r) == 3);
    check("after write 3: free == CAP-3", pcm_ring_free(&r) == CAP - 3);

    check("read 3 returns 3", pcm_ring_read(&r, dst, 3) == 3);
    int basic_ok = 1;
    for (int i = 0; i < 3; i++) {
        if (dst[i * 2 + 0] != (int16_t)i || dst[i * 2 + 1] != (int16_t)-i) {
            basic_ok = 0;
        }
    }
    check("read 3: L/R values match", basic_ok);
    check("after read 3: fill == 0", pcm_ring_fill(&r) == 0);

    /* ---- over-write is clamped to free space, no corruption ---- */
    pcm_ring_init(&r, storage, CAP);
    check("over-write CAP+5 returns CAP", pcm_ring_write(&r, src, CAP + 5) == CAP);
    check("ring full: free == 0", pcm_ring_free(&r) == 0);
    check("write into full ring returns 0", pcm_ring_write(&r, src, 4) == 0);

    /* ---- under-read is clamped to fill ---- */
    check("read CAP+5 from full ring returns CAP",
          pcm_ring_read(&r, dst, CAP + 5) == CAP);
    check("read from empty ring returns 0", pcm_ring_read(&r, dst, 4) == 0);

    /* ---- FIFO integrity across many wrap cycles ----
     * Push a long monotone frame sequence through the tiny ring in
     * mismatched write/read chunk sizes, so the masked indices wrap many
     * times. Every frame must come out exactly once, in order, L/R intact. */
    pcm_ring_init(&r, storage, CAP);
    const int TOTAL = 1000;
    int next_write = 0;   /* next frame value to enqueue */
    int next_read  = 0;   /* next frame value expected   */
    int wsz = 1, rsz = 1; /* varying chunk sizes         */
    int fifo_ok = 1;
    int max_fill_seen = 0;

    while (next_read < TOTAL) {
        /* Producer: stage up to wsz new frames and write what fits. */
        int staged = 0;
        while (staged < wsz && next_write < TOTAL) {
            int v = next_write + staged;
            src[staged * 2 + 0] = (int16_t)v;
            src[staged * 2 + 1] = (int16_t)-v;
            staged++;
        }
        uint32_t wrote = pcm_ring_write(&r, src, (uint32_t)staged);
        next_write += (int)wrote;

        if ((int)pcm_ring_fill(&r) > max_fill_seen) {
            max_fill_seen = (int)pcm_ring_fill(&r);
        }

        /* Consumer: read up to rsz frames and verify each in order. */
        uint32_t got = pcm_ring_read(&r, dst, (uint32_t)rsz);
        for (uint32_t i = 0; i < got; i++) {
            if (dst[i * 2 + 0] != (int16_t)next_read ||
                dst[i * 2 + 1] != (int16_t)-next_read) {
                fifo_ok = 0;
            }
            next_read++;
        }

        /* Vary chunk sizes to shuffle the wrap alignment (1..CAP+2). */
        wsz = wsz % (CAP + 2) + 1;
        rsz = rsz % (CAP + 2) + 1;
    }

    check("FIFO: all 1000 frames in order, L/R intact", fifo_ok);
    check("FIFO: fill never exceeded CAP", max_fill_seen <= CAP);
    check("FIFO: drained empty at end", pcm_ring_fill(&r) == 0);

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
