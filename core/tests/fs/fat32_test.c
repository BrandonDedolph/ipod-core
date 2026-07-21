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

/* readdir callback: append each surfaced entry to a fixed-size collector. */
static int dir_collect(void *ud, const fat32_dirent_t *ent)
{
    struct { fat32_dirent_t v[8]; int n; } *c = ud;
    if (c->n < (int)(sizeof c->v / sizeof c->v[0])) {
        c->v[c->n++] = *ent;
    }
    return 0;   /* keep going */
}

/* readdir callback: count one entry, then ask to stop (nonzero return). */
static int dir_stop_first(void *ud, const fat32_dirent_t *ent)
{
    (void)ent;
    (*(int *)ud)++;
    return 1;   /* stop after the first entry */
}

/* ---- second, in-RAM image: a root with a subdirectory to descend into ----
 * The meson-generated image has no subdirectory, so to prove fat32_readdir
 * enumerates a NON-root directory (and correctly hides "." / "..") we hand-
 * build a tiny FAT32 volume here and serve it through a memory-backed block
 * callback. BytesPerSector 512 (sec_ratio 1) keeps the layout arithmetic
 * plain: with RSVD=1, one FAT of one sector, data_start is FS-sector 2, so
 * cluster N lands on FS-sector N. Root (clus 2) holds one subdir "MUSIC"
 * (clus 3); MUSIC holds "." / ".." plus the real child "SONG.TXT" (clus 4). */
#define MEM_BPS   512u
#define MEM_SECS  8u
static uint8_t g_mem[MEM_SECS * MEM_BPS];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Write one 32-byte directory entry: 11-byte raw 8.3 field, attr, cluster,
 * size. `raw` must be exactly 11 bytes (space-padded, no dot). */
static void put_dirent(uint8_t *e, const char *raw, uint8_t attr,
                       uint32_t clus, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, raw, 11);
    e[11] = attr;
    put16(&e[20], (uint16_t)(clus >> 16));
    put16(&e[26], (uint16_t)(clus & 0xFFFF));
    put32(&e[28], size);
}

static void build_subdir_image(void)
{
    memset(g_mem, 0, sizeof g_mem);

    /* boot sector / BPB */
    uint8_t *bs = g_mem;
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(&bs[3], "MSDOS5.0", 8);
    put16(&bs[11], MEM_BPS);     /* BytesPerSec */
    bs[13] = 1;                  /* SecPerClus  */
    put16(&bs[14], 1);           /* RsvdSecCnt  */
    bs[16] = 1;                  /* NumFATs     */
    bs[21] = 0xF8;               /* media       */
    put32(&bs[36], 1);           /* FATSz32     */
    put32(&bs[44], 2);           /* RootClus    */
    bs[510] = 0x55; bs[511] = 0xAA;

    /* FAT (FS-sector 1): mark every used cluster as a single-cluster chain. */
    uint8_t *fat = &g_mem[1 * MEM_BPS];
    put32(&fat[0 * 4], 0x0FFFFFF8u);   /* media    */
    put32(&fat[1 * 4], 0x0FFFFFFFu);   /* reserved */
    put32(&fat[2 * 4], 0x0FFFFFFFu);   /* root  (clus 2) EOC */
    put32(&fat[3 * 4], 0x0FFFFFFFu);   /* MUSIC (clus 3) EOC */
    put32(&fat[4 * 4], 0x0FFFFFFFu);   /* SONG  (clus 4) EOC */

    /* Root directory (cluster 2 == FS-sector 2): one subdirectory. */
    uint8_t *root = &g_mem[2 * MEM_BPS];
    put_dirent(&root[0], "MUSIC      ", 0x10, 3, 0);  /* on-disk size is 0 */
    /* root[32..] stays 0x00 => end of directory */

    /* MUSIC subdirectory (cluster 3 == FS-sector 3): "." / ".." + one child. */
    uint8_t *sub = &g_mem[3 * MEM_BPS];
    put_dirent(&sub[0],  ".          ", 0x10, 3, 0);  /* self   */
    put_dirent(&sub[32], "..         ", 0x10, 2, 0);  /* parent */
    put_dirent(&sub[64], "SONG    TXT", 0x20, 4, 100);
    /* sub[96..] stays 0x00 => end of directory */
}

/* Memory-backed 512-byte block read over g_mem (part_lba 0). */
static int mem_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if ((lba + count) * 512u > sizeof g_mem) {
        return -1;
    }
    memcpy(buf, &g_mem[lba * 512u], count * 512u);
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

    /* ---- root-directory enumeration ----
     * The synthetic image's root holds exactly two real entries: the 8.3 file
     * HELLO.TXT (cluster 3, 3000 bytes) and the long-named Intentions.flac
     * (cluster 5, 500 bytes). No volume label and no "." / ".." (FAT32 root
     * dirs have none). Collect every emitted entry and check them all. */
    struct { fat32_dirent_t v[8]; int n; } coll = { .n = 0 };
    fails += check("readdir_root returns 0",
                   fat32_readdir_root(&fs, dir_collect, &coll) == 0);
    fails += check("readdir_root emits exactly 2 entries", coll.n == 2);

    /* Locate each expected entry by name (order is disk order, but assert by
     * name so the test doesn't over-specify the traversal). */
    const fat32_dirent_t *hello = NULL, *intent = NULL;
    for (int i = 0; i < coll.n; i++) {
        if (strcmp(coll.v[i].name, "HELLO.TXT") == 0)       hello  = &coll.v[i];
        if (strcmp(coll.v[i].name, "Intentions.flac") == 0) intent = &coll.v[i];
    }
    fails += check("readdir emits HELLO.TXT (8.3 formatted)", hello != NULL);
    fails += check("readdir emits Intentions.flac (LFN reassembled)",
                   intent != NULL);

    /* No volume-label / dot entries leaked in under any other name. */
    int only_expected = 1;
    for (int i = 0; i < coll.n; i++) {
        if (strcmp(coll.v[i].name, "HELLO.TXT") != 0 &&
            strcmp(coll.v[i].name, "Intentions.flac") != 0) {
            only_expected = 0;
        }
    }
    fails += check("readdir emits no label/dot/extra entries", only_expected);

    /* size + is_dir correctness for a known file. */
    fails += check("HELLO.TXT: is_dir=0, size=3000, clus=3",
                   hello && hello->is_dir == 0 && hello->size == 3000 &&
                   hello->first_clus == 3);
    fails += check("Intentions.flac: is_dir=0, size=500, clus=5",
                   intent && intent->is_dir == 0 && intent->size == 500 &&
                   intent->first_clus == 5);

    /* Early-stop: a callback that returns nonzero on the first entry stops the
     * walk immediately, and the whole call still reports success (0). */
    int stop_count = 0;
    fails += check("readdir early-stop returns 0",
                   fat32_readdir_root(&fs, dir_stop_first, &stop_count) == 0);
    fails += check("readdir early-stop invoked cb exactly once",
                   stop_count == 1);

    /* ---- fat32_readdir(root) parity with fat32_readdir_root ----
     * Enumerating fs->root_clus explicitly must yield byte-for-byte the same
     * entries the root wrapper does (the wrapper is now a thin call into it). */
    struct { fat32_dirent_t v[8]; int n; } rc = { .n = 0 };
    fails += check("fat32_readdir(root_clus) returns 0",
                   fat32_readdir(&fs, fs.root_clus, dir_collect, &rc) == 0);
    fails += check("fat32_readdir(root_clus) emits same count as _root",
                   rc.n == coll.n);
    int parity_ok = (rc.n == coll.n);
    for (int i = 0; i < rc.n && parity_ok; i++) {
        /* order is identical (same walk), so compare position-for-position */
        parity_ok = strcmp(rc.v[i].name, coll.v[i].name) == 0 &&
                    rc.v[i].first_clus == coll.v[i].first_clus &&
                    rc.v[i].size == coll.v[i].size &&
                    rc.v[i].is_dir == coll.v[i].is_dir;
    }
    fails += check("fat32_readdir(root_clus) entries identical to _root",
                   parity_ok);

    fclose(g_img);

    /* ---- descent into a subdirectory (in-RAM image) ----
     * The meson image has no subdir, so exercise fat32_readdir on a non-root
     * directory against a hand-built volume: root -> MUSIC/ -> SONG.TXT. */
    build_subdir_image();
    fat32_t mfs;
    fails += check("mem-image mount returns 0",
                   fat32_mount(&mfs, mem_read, NULL, 0) == 0);

    /* Root enumeration surfaces the subdirectory with is_dir=1 and its
     * on-disk cluster; size is forced to 0 for a directory. */
    struct { fat32_dirent_t v[8]; int n; } mroot = { .n = 0 };
    fails += check("mem readdir(root) returns 0",
                   fat32_readdir(&mfs, mfs.root_clus, dir_collect, &mroot) == 0);
    fails += check("mem root emits exactly 1 entry", mroot.n == 1);
    const fat32_dirent_t *music =
        (mroot.n == 1 && strcmp(mroot.v[0].name, "MUSIC") == 0)
            ? &mroot.v[0] : NULL;
    fails += check("mem root emits MUSIC as a directory",
                   music && music->is_dir == 1);
    fails += check("MUSIC: is_dir=1, size=0, clus=3",
                   music && music->is_dir == 1 && music->size == 0 &&
                   music->first_clus == 3);

    /* Descend: enumerate MUSIC by its first cluster. "." and ".." must NOT be
     * emitted; only the real child SONG.TXT (a file) is. */
    struct { fat32_dirent_t v[8]; int n; } msub = { .n = 0 };
    uint32_t music_clus = music ? music->first_clus : 0;
    fails += check("mem readdir(MUSIC) returns 0",
                   fat32_readdir(&mfs, music_clus, dir_collect, &msub) == 0);
    fails += check("MUSIC emits exactly 1 real child (no . / ..)",
                   msub.n == 1);
    int no_dots = 1;
    for (int i = 0; i < msub.n; i++) {
        if (msub.v[i].name[0] == '.') {
            no_dots = 0;
        }
    }
    fails += check("MUSIC never emits a '.' or '..' entry", no_dots);
    const fat32_dirent_t *song =
        (msub.n == 1 && strcmp(msub.v[0].name, "SONG.TXT") == 0)
            ? &msub.v[0] : NULL;
    fails += check("MUSIC/SONG.TXT: is_dir=0, size=100, clus=4",
                   song && song->is_dir == 0 && song->size == 100 &&
                   song->first_clus == 4);

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
