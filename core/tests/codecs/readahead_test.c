/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/readahead_test.c — host test for the read-ahead source shim.
 *
 * The shim (codecs/readahead.c) sits between a decoder and its byte source to
 * collapse a decoder's many tiny reads into a few large backing reads — the
 * fix for the ~27 s on-device MP3 startup (dr_mp3's ID3v2 scan). This test
 * backs it with an in-RAM buffer that COUNTS how many times it is read/seeked,
 * then checks two things at once:
 *   1. Correctness: bytes returned through the shim are identical to the
 *      reference, across tiny/bulk reads and SET/CUR/END seeks.
 *   2. Coalescing: thousands of 3-byte reads hit the backing source only
 *      ~ceil(len/cap) times, not once per read — the reason the shim exists.
 *
 * The code under test is the SAME readahead.c the ARM build compiles.
 */

#include "readahead.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fail = 1;
    }
    return cond;
}

/* --- instrumented in-RAM backing source (stands in for the disk) --- */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    long           n_reads;   /* backing read()  calls */
    long           n_seeks;   /* backing seek()  calls */
} counting_src_t;

static size_t cs_read(void *ud, void *buf, size_t n)
{
    counting_src_t *m = (counting_src_t *)ud;
    m->n_reads++;
    size_t avail = (m->pos < m->len) ? (m->len - m->pos) : 0;
    if (n > avail) {
        n = avail;
    }
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}
static int cs_seek(void *ud, int offset, int origin)
{
    counting_src_t *m = (counting_src_t *)ud;
    m->n_seeks++;
    long base = (origin == DECODER_SEEK_SET) ? 0
              : (origin == DECODER_SEEK_END) ? (long)m->len
                                             : (long)m->pos;
    long np = base + offset;
    if (np < 0 || (size_t)np > m->len) {
        return 0;
    }
    m->pos = (size_t)np;
    return 1;
}
static int64_t cs_tell(void *ud)
{
    return (int64_t)((counting_src_t *)ud)->pos;
}

static void src_init(counting_src_t *m, decoder_source_t *s,
                     const uint8_t *data, size_t len)
{
    m->data = data;
    m->len  = len;
    m->pos  = 0;
    m->n_reads = 0;
    m->n_seeks = 0;
    s->read = cs_read;
    s->seek = cs_seek;
    s->tell = cs_tell;
    s->userdata = m;
}

int main(void)
{
    /* Deterministic pseudo-random reference (LCG — no rand(), reproducible). */
    enum { LEN = 200000u, CAP = 4096u };
    static uint8_t ref[LEN];
    static uint8_t buf[CAP];
    uint32_t x = 0x1234567u;
    for (size_t i = 0; i < LEN; i++) {
        x = x * 1103515245u + 12345u;
        ref[i] = (uint8_t)(x >> 16);
    }

    counting_src_t   cs;
    decoder_source_t inner, ra_src;
    readahead_t      ra;

    /* --- Test 1: many tiny sequential reads are byte-exact AND coalesced --- */
    {
        src_init(&cs, &inner, ref, LEN);
        readahead_init(&ra, &inner, buf, CAP);
        readahead_as_source(&ra, &ra_src);

        int ok = 1;
        size_t total = 0;
        uint8_t tmp[3];
        for (;;) {
            size_t got = ra_src.read(ra_src.userdata, tmp, sizeof tmp);
            if (got == 0) {
                break;
            }
            if (memcmp(tmp, ref + total, got) != 0) {
                ok = 0;
                break;
            }
            total += got;
        }
        check("tiny-reads-exact", ok && total == LEN);
        check("tiny-reads-tell", ra_src.tell(ra_src.userdata) == (int64_t)LEN);

        /* ~66667 three-byte reads must touch the disk only ~ceil(LEN/CAP)=49
         * times. Allow a little slack; the point is it is O(LEN/CAP), not
         * O(reads). */
        printf("  backing reads=%ld seeks=%ld (tiny reads issued=%zu)\n",
               cs.n_reads, cs.n_seeks, (total + 2) / 3);
        check("tiny-reads-coalesced", cs.n_reads <= (long)(LEN / CAP) + 3);
    }

    /* --- Test 2: a read >= cap bypasses the buffer, still exact --- */
    {
        src_init(&cs, &inner, ref, LEN);
        readahead_init(&ra, &inner, buf, CAP);
        readahead_as_source(&ra, &ra_src);

        static uint8_t big[50000];
        size_t got = ra_src.read(ra_src.userdata, big, sizeof big);
        int ok = (got == sizeof big) && (memcmp(big, ref, sizeof big) == 0);
        check("bulk-read-exact", ok);
        check("bulk-read-tell", ra_src.tell(ra_src.userdata) == (int64_t)sizeof big);
    }

    /* --- Test 3: SET/CUR/END seeks reposition correctly --- */
    {
        src_init(&cs, &inner, ref, LEN);
        readahead_init(&ra, &inner, buf, CAP);
        readahead_as_source(&ra, &ra_src);

        uint8_t tmp[64];
        int ok = 1;

        /* SEEK_SET into the middle. */
        ok &= ra_src.seek(ra_src.userdata, 100000, DECODER_SEEK_SET);
        ok &= (ra_src.read(ra_src.userdata, tmp, 64) == 64);
        ok &= (memcmp(tmp, ref + 100000, 64) == 0);

        /* SEEK_CUR forward from there. */
        ok &= ra_src.seek(ra_src.userdata, 500, DECODER_SEEK_CUR);
        ok &= (ra_src.read(ra_src.userdata, tmp, 64) == 64);
        ok &= (memcmp(tmp, ref + 100000 + 64 + 500, 64) == 0);

        /* SEEK_END: 0 offset → EOF (read returns 0). */
        ok &= ra_src.seek(ra_src.userdata, 0, DECODER_SEEK_END);
        ok &= (ra_src.tell(ra_src.userdata) == (int64_t)LEN);
        ok &= (ra_src.read(ra_src.userdata, tmp, 64) == 0);

        /* SEEK_END: -64 → last 64 bytes. */
        ok &= ra_src.seek(ra_src.userdata, -64, DECODER_SEEK_END);
        ok &= (ra_src.read(ra_src.userdata, tmp, 64) == 64);
        ok &= (memcmp(tmp, ref + LEN - 64, 64) == 0);

        check("seeks-exact", ok);
    }

    /* --- Test 4: a backward seek within the buffered window costs no disk --- */
    {
        src_init(&cs, &inner, ref, LEN);
        readahead_init(&ra, &inner, buf, CAP);
        readahead_as_source(&ra, &ra_src);

        uint8_t tmp[16];
        /* First read primes a block covering [0, CAP). */
        ra_src.read(ra_src.userdata, tmp, 16);
        long reads_after_prime = cs.n_reads;

        /* Re-read bytes still inside that block: must not touch the disk. */
        int ok = 1;
        ra_src.seek(ra_src.userdata, 10, DECODER_SEEK_SET);
        ok &= (ra_src.read(ra_src.userdata, tmp, 16) == 16);
        ok &= (memcmp(tmp, ref + 10, 16) == 0);
        ok &= (cs.n_reads == reads_after_prime);   /* served from RAM */
        check("window-reread-no-disk", ok);
    }

    printf("readahead_test: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
