/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fs/fat32_test.c — host test for the read-only FAT32 reader.
 *
 * Mounts the synthetic image built by tests/scripts/make_fat32_image.py
 * (BytesPerSector 2048, so sec_ratio = 4; a file spanning two clusters)
 * through a file-backed 512-byte block callback, then checks mount, the
 * 8.3 lookup (case-insensitive, hit + miss), the VFAT long-name lookup
 * (Intentions.flac, only findable by long name), and the file read (full
 * and partial). The image path is argv[1] (passed by meson).
 */

#include "fat32.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static FILE *g_img;

/* 512-byte block read from the image file. part_lba is 0 (the image is the
 * volume itself, no MBR), so `lba` is a plain FS offset in 512-byte units. */
static int img_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if (fseek(g_img, (long)lba * 512L, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(buf, 512, count, g_img) != count) {
        return -1;
    }
    return 0;
}

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <fat32-image>\n", argv[0]);
        return 2;
    }
    g_img = fopen(argv[1], "rb");
    if (!g_img) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    int fails = 0;
    fat32_t fs;

    fails += check("mount returns 0", fat32_mount(&fs, img_read, NULL, 0) == 0);
    fails += check("BytesPerSec = 2048", fs.bytes_per_sec == 2048);
    fails += check("sec_ratio = 4", fs.sec_ratio == 4);
    fails += check("root cluster = 2", fs.root_clus == 2);

    uint32_t clus = 0, size = 0;

    /* exact-name lookup */
    fails += check("open HELLO.TXT returns 0",
                   fat32_open(&fs, "HELLO.TXT", &clus, &size) == 0);
    fails += check("HELLO.TXT first cluster = 3", clus == 3);
    fails += check("HELLO.TXT size = 3000", size == 3000);

    /* case-insensitive */
    uint32_t c2 = 0, s2 = 0;
    fails += check("open hello.txt (lowercase) returns 0",
                   fat32_open(&fs, "hello.txt", &c2, &s2) == 0 &&
                   c2 == 3 && s2 == 3000);

    /* miss */
    uint32_t c3, s3;
    fails += check("open NOPE.TXT returns -1",
                   fat32_open(&fs, "NOPE.TXT", &c3, &s3) == -1);

    /* VFAT long name: "Intentions.flac" has a 4-char extension, so its short
     * name is mangled (INTENT~1.FLA) and it can only be found by long name. */
    uint32_t lc = 0, ls = 0;
    fails += check("open Intentions.flac (long name) returns 0",
                   fat32_open(&fs, "Intentions.flac", &lc, &ls) == 0);
    fails += check("Intentions.flac first cluster = 5", lc == 5);
    fails += check("Intentions.flac size = 500", ls == 500);

    /* long-name match is case-insensitive */
    uint32_t lc2 = 0, ls2 = 0;
    fails += check("open intentions.FLAC (mixed case) returns 0",
                   fat32_open(&fs, "intentions.FLAC", &lc2, &ls2) == 0 &&
                   lc2 == 5 && ls2 == 500);

    /* the mangled 8.3 short name still resolves via the fallback path */
    uint32_t lc3 = 0, ls3 = 0;
    fails += check("open INTENT~1.FLA (8.3 fallback) returns 0",
                   fat32_open(&fs, "INTENT~1.FLA", &lc3, &ls3) == 0 &&
                   lc3 == 5 && ls3 == 500);

    /* the file's 500 bytes read back correctly (content follows LFN entries) */
    static uint8_t lbuf[2048];
    int32_t ln = fat32_read_file(&fs, lc, lbuf, ls);
    int lfn_content_ok = (ln == 500);
    for (int i = 0; i < 500 && lfn_content_ok; i++) {
        lfn_content_ok = lbuf[i] == (uint8_t)((i ^ 0x5A) & 0xFF);
    }
    fails += check("Intentions.flac reads 500 correct bytes", lfn_content_ok);

    /* a long name that isn't present still misses */
    uint32_t c4, s4;
    fails += check("open Nonexistent.flac returns -1",
                   fat32_open(&fs, "Nonexistent.flac", &c4, &s4) == -1);

    /* full read: 3000 bytes of the i&0xFF pattern, spanning 2 clusters */
    static uint8_t buf[8192];
    int32_t n = fat32_read_file(&fs, clus, buf, size);
    fails += check("read_file returns 3000", n == 3000);
    int content_ok = 1;
    for (int i = 0; i < 3000; i++) {
        if (buf[i] != (uint8_t)(i & 0xFF)) {
            content_ok = 0;
            printf("  content mismatch at %d: got %02X want %02X\n",
                   i, buf[i], (uint8_t)(i & 0xFF));
            break;
        }
    }
    fails += check("read_file content matches pattern (2-cluster span)",
                   content_ok);

    /* partial read: first 100 bytes only */
    memset(buf, 0xCC, sizeof buf);
    int32_t p = fat32_read_file(&fs, clus, buf, 100);
    int partial_ok = (p == 100);
    for (int i = 0; i < 100 && partial_ok; i++) {
        partial_ok = buf[i] == (uint8_t)(i & 0xFF);
    }
    fails += check("partial read (maxlen=100) returns 100 correct bytes",
                   partial_ok);

    /* ---- streaming reader (forward cursor) ----
     * HELLO.TXT is 3000 bytes of i&0xFF spanning clusters 3->4 at the
     * 2048-byte (one-cluster) boundary. Read it forward in small 7-byte
     * chunks and verify every byte, the cross-cluster follow, and clean EOF. */
    fat32_stream_t st;
    fat32_stream_open(&st, &fs, clus, size);
    static uint8_t sbuf[8192];
    uint32_t spos = 0;
    int stream_ok = 1;
    for (;;) {
        int32_t got = fat32_stream_read(&st, &sbuf[spos], 7);
        if (got < 0) {
            stream_ok = 0;
            break;
        }
        if (got == 0) {
            break;                       /* EOF */
        }
        spos += (uint32_t)got;
        if (spos > sizeof sbuf) {        /* runaway guard */
            stream_ok = 0;
            break;
        }
    }
    fails += check("stream (7-byte chunks) totals 3000 bytes", spos == 3000);
    if (stream_ok) {
        for (uint32_t i = 0; i < spos; i++) {
            if (sbuf[i] != (uint8_t)(i & 0xFF)) {
                stream_ok = 0;
                printf("  stream mismatch at %u: got %02X want %02X\n",
                       i, sbuf[i], (uint8_t)(i & 0xFF));
                break;
            }
        }
    }
    fails += check("stream (7-byte chunks) content matches across clusters",
                   stream_ok);

    /* One big read is clamped to the file size (remaining), not the request. */
    fat32_stream_open(&st, &fs, clus, size);
    memset(sbuf, 0xCC, sizeof sbuf);
    int32_t big = fat32_stream_read(&st, sbuf, 4096);
    int big_ok = (big == 3000);
    for (int i = 0; i < 3000 && big_ok; i++) {
        big_ok = sbuf[i] == (uint8_t)(i & 0xFF);
    }
    fails += check("stream big read (len>size) returns 3000 correct bytes",
                   big_ok);
    fails += check("stream at EOF returns 0",
                   fat32_stream_read(&st, sbuf, 16) == 0);

    /* Aligned full-sector read (the direct-into-buffer fast path), then the
     * 952-byte tail in the next cluster — exercises both paths + the follow. */
    fat32_stream_open(&st, &fs, clus, size);
    memset(sbuf, 0xCC, sizeof sbuf);
    int32_t a1 = fat32_stream_read(&st, sbuf, 2048);           /* whole clus 3 */
    int32_t a2 = fat32_stream_read(&st, &sbuf[2048], 4096);    /* tail, clus 4 */
    int aligned_ok = (a1 == 2048 && a2 == 952);
    for (int i = 0; i < 3000 && aligned_ok; i++) {
        aligned_ok = sbuf[i] == (uint8_t)(i & 0xFF);
    }
    fails += check("stream aligned 2048 + 952 tail crosses cluster cleanly",
                   aligned_ok);

    fclose(g_img);
    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
