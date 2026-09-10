/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/artcache_test.c — the album-cover cache (core/ui/artcache.c) driven
 * the way the album list drives it, on the host.
 *
 * WHAT THIS MEASURES. Every cache miss is a DISK READ issued from the main
 * loop, synchronously, while the user is scrolling — and on the device that
 * read can land on a parked platter. So the questions that matter are not
 * "does a cover come back" but "how many reads does a scroll gesture cost, and
 * which covers do they fetch first". Both were unanswerable before this file:
 * the cache was only ever exercised by flashing the device and watching chips
 * fill in.
 *
 * fat32_read_file is stubbed here (artcache.c's only FS entry point): each
 * album's cover is a synthetic 28x28 CoreArt sidecar at cluster 1000+album,
 * and the stub counts calls and records their order, which is the oracle.
 */

#include <stdio.h>
#include <string.h>

#include "artcache.h"
#include "../xfail.h"

/* ---- fat32_read_file stub: one synthetic 28x28 folder.thm per album ------ */

#define THM_BYTES (ARTCACHE_HDR_LEN + ARTCACHE_DIM * ARTCACHE_DIM * 2)
#define CLUS_BASE 1000u

static int      g_reads;               /* calls since the last reset        */
static int      g_read_log[512];       /* album index of each read, in order */

static void reads_reset(void)
{
    g_reads = 0;
}

int32_t fat32_read_file(fat32_t *fs, uint32_t clus, void *buf, uint32_t maxlen)
{
    (void)fs;
    if (g_reads < (int)(sizeof g_read_log / sizeof g_read_log[0])) {
        g_read_log[g_reads] = (int)(clus - CLUS_BASE);
    }
    g_reads++;
    if (maxlen < THM_BYTES) {
        return -1;
    }
    uint8_t *p = (uint8_t *)buf;
    p[0] = 'C'; p[1] = 'A'; p[2] = 'R'; p[3] = 'T';
    p[4] = 1;   p[5] = 0;                          /* version           */
    p[6] = ARTCACHE_DIM; p[7] = 0;                 /* width             */
    p[8] = ARTCACHE_DIM; p[9] = 0;                 /* height            */
    p[10] = 0;  p[11] = 0;
    /* Pixels carry the album index so a loaded chip can be checked. */
    uint16_t *px = (uint16_t *)(p + ARTCACHE_HDR_LEN);
    for (int i = 0; i < ARTCACHE_DIM * ARTCACHE_DIM; i++) {
        px[i] = (uint16_t)(clus - CLUS_BASE);
    }
    return (int32_t)THM_BYTES;
}

/* ---- the album list, as the cache sees it ------------------------------- */

#define ROWS 6                          /* LIST_ROWS2: chips on screen at once */
#define ALBUMS 64

static fat32_t g_fs;                    /* never dereferenced by the stub    */

static void library_register(void)
{
    for (int a = 0; a < ALBUMS; a++) {
        artcache_queue(a, CLUS_BASE + (uint32_t)a, THM_BYTES, 0, 0);
    }
}

/* One album-list paint of the window [top, top+ROWS): exactly what
 * albumlist_row_draw does per row — queue (idempotent) + get. */
static void paint(int top)
{
    for (int r = 0; r < ROWS; r++) {
        int a = top + r;
        artcache_queue(a, CLUS_BASE + (uint32_t)a, THM_BYTES, 0, 0);
        (void)artcache_get(a);
    }
}

/* Pump until nothing is queued (the idle "blast" case). */
static void pump_all(void)
{
    while (artcache_pump(&g_fs)) { }
}

static int window_resident(int top)
{
    int n = 0;
    for (int r = 0; r < ROWS; r++) {
        if (artcache_peek(top + r)) n++;
    }
    return n;
}

int main(void)
{
    xfail_ctx c = { "artcache", 0, 0, 0 };

    /* ---- 1. a resident cover is served without a read ------------------ */
    artcache_reset();
    library_register();
    paint(0);
    reads_reset();
    pump_all();
    xpect(&c, "a first paint costs one read per visible row", g_reads == ROWS);
    reads_reset();
    paint(0);
    pump_all();
    xpect(&c, "repainting the same window costs no reads", g_reads == 0);
    xpect(&c, "and every visible chip is resident", window_resident(0) == ROWS);
    const uint16_t *px = artcache_peek(3);
    xpect(&c, "the pixels are the album's own", px && px[0] == 3);

    /* ---- 2. scroll down then back up: how much of the way back re-reads -- */
    artcache_reset();
    library_register();
    const int DOWN = 20;
    for (int top = 0; top <= DOWN; top++) {
        paint(top);
        pump_all();
    }
    reads_reset();
    for (int top = DOWN - 1; top >= 0; top--) {
        paint(top);
        pump_all();
    }
    printf("[artcache] down %d rows then back up: %d re-reads with %d ways\n",
           DOWN, g_reads, ARTCACHE_WAYS);
    /* The way count bounds how far back a reversal stays free: the rows just
     * above the window are the most recently drawn after the window itself,
     * so at least (WAYS - ROWS) rows of the way back must be hits. (16 ways
     * measured 10 re-reads on this exact gesture; 32 must measure none.) */
    int allowed = DOWN - (ARTCACHE_WAYS - ROWS);
    if (allowed < 0) allowed = 0;
    xpect(&c, "reversing direction re-reads at most what the ways cannot hold",
          g_reads <= allowed);
    xpect(&c, "a 20-row round trip is free at the shipped way count",
          g_reads == 0);

    /* ---- 3. a fast scroll outruns the pump: what loads first when it stops */
    /* Fifteen detents with no pump in between — the loop was busy painting —
     * then the wheel stops on window 14..19 and the pump gets six reads. The
     * user is looking at rows 14..19; every read spent on a row that already
     * scrolled off screen is a read the visible chips wait behind. */
    artcache_reset();
    library_register();
    for (int top = 0; top <= 14; top++) {
        paint(top);
    }
    reads_reset();
    for (int k = 0; k < ROWS; k++) {
        artcache_pump(&g_fs);
    }
    printf("[artcache] after a 15-row burst, plain pump's first %d reads:", ROWS);
    for (int i = 0; i < g_reads; i++) printf(" %d", g_read_log[i]);
    printf(" -> %d of %d visible chips resident\n", window_resident(14), ROWS);
    /* Not asserted: FIFO is the fallback, and this is the measurement that
     * says why the album list must not use it as the primary order. */

    /* Same burst, but the pump is told the window. Every one of the first six
     * reads must be a visible row, and in row order (top first). */
    artcache_reset();
    library_register();
    for (int top = 0; top <= 14; top++) {
        paint(top);
    }
    int want[ROWS];
    for (int r = 0; r < ROWS; r++) want[r] = 14 + r;
    reads_reset();
    for (int k = 0; k < ROWS; k++) {
        artcache_pump_for(&g_fs, want, ROWS);
    }
    int in_order = (g_reads == ROWS);
    for (int i = 0; i < ROWS && i < g_reads; i++) {
        if (g_read_log[i] != want[i]) in_order = 0;
    }
    printf("[artcache] same burst, pump_for's first %d reads:", ROWS);
    for (int i = 0; i < g_reads; i++) printf(" %d", g_read_log[i]);
    printf(" -> %d of %d visible chips resident\n", window_resident(14), ROWS);
    xpect(&c, "told the window, the first reads after a burst are the rows on screen",
          window_resident(14) == ROWS);
    xpect(&c, "and they arrive top row first", in_order);

    /* ---- 4. a window the list has not painted yet still gets its reads --- */
    /* The selection moved and the paint is pending (throttled while playing):
     * rows 20..25 have never been through artcache_get, so no way holds them.
     * pump_for must claim for them rather than fall through to the backlog. */
    for (int r = 0; r < ROWS; r++) want[r] = 20 + r;
    reads_reset();
    artcache_pump_for(&g_fs, want, ROWS);
    xpect(&c, "an unpainted row is claimed and read by pump_for",
          g_reads == 1 && g_read_log[0] == 20 && artcache_peek(20) != 0);

    /* ---- 5. once the window is resident, the backlog drains oldest-first --- */
    for (int r = 0; r < ROWS; r++) want[r] = 14 + r;
    for (int k = 0; k < ROWS; k++) artcache_pump_for(&g_fs, want, ROWS);
    reads_reset();
    int got = artcache_pump_for(&g_fs, want, ROWS);
    xpect(&c, "with the window resident, pump_for serves a scrolled-off row",
          got == 1 && g_reads == 1 && g_read_log[0] < 14);

    /* ---- 6. a row that is not an album is skipped, not an error ------------ */
    int mixed[3] = { -1, 30, ARTCACHE_SLOTS + 5 };
    reads_reset();
    artcache_pump_for(&g_fs, mixed, 3);
    xpect(&c, "-1 and out-of-range rows are skipped by pump_for",
          g_reads == 1 && g_read_log[0] == 30);

    /* ---- 7. an empty want list is just the plain pump --------------------- */
    artcache_reset();
    library_register();
    reads_reset();
    xpect(&c, "pump_for with nothing wanted and nothing queued does no I/O",
          artcache_pump_for(&g_fs, 0, 0) == 0 && g_reads == 0);

    return xfail_done(&c);
}
