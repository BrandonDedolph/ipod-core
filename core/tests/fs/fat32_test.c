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
 *
 * Two further volumes are hand-built in RAM below: one with a subdirectory
 * (descent, "." / ".." hiding), and one holding the file shapes a corrupt or
 * half-written disk hands the STREAMING reader — an empty file, a garbage
 * first cluster, a chain shorter than the size — served through a block
 * callback that can be told to fail a given sector once or forever. Those
 * cases pin down that 0 from fat32_stream_read means end-of-file and nothing
 * else, and that a failed read leaves the cursor where it was.
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

/* Fault injection for the memory image: any read touching g_fail_lba fails
 * while g_fail_left > 0 (each failure consumes one), or forever when it is
 * negative. Everything else, including a read that merely spans a different
 * sector, succeeds. This is how a "transient" sector error is staged: fail
 * once, then let the retry through. */
#define NO_FAIL 0xFFFFFFFFu
static uint32_t g_fail_lba  = NO_FAIL;
static int      g_fail_left = 0;

static void fail_sector(uint32_t lba, int times)   /* times < 0: forever */
{
    g_fail_lba  = lba;
    g_fail_left = times;
}

/* Memory-backed 512-byte block read over g_mem (part_lba 0). */
static int mem_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if ((lba + count) * 512u > sizeof g_mem) {
        return -1;
    }
    if (g_fail_lba != NO_FAIL && lba <= g_fail_lba && g_fail_lba < lba + count &&
        g_fail_left != 0) {
        if (g_fail_left > 0) {
            g_fail_left--;
        }
        return -1;
    }
    memcpy(buf, &g_mem[lba * 512u], count * 512u);
    return 0;
}

/* ---- third image: the file shapes a corrupt or half-written volume hands
 * the streaming reader, on the same 512-byte geometry (cluster N == sector N,
 * data from sector 2). All in the root, so nothing here disturbs the
 * subdirectory assertions above:
 *
 *   EMPTY.TXT    cluster 0, size 0        a legitimately empty file
 *   BADCLUS.TXT  cluster 0x1000, size 100 a real size, an unaddressable first
 *                                         cluster (the volume tops out at 128)
 *   SHORT.TXT    cluster 3 -> EOC, size 1034
 *                                         the chain ends after ONE cluster but
 *                                         the size claims three
 *   TWO.TXT      clusters 4 -> 5, size 1000, byte[i] = (i * 7) & 0xFF
 *                                         a healthy two-cluster file, for the
 *                                         transient-error cases
 *   BADDIR       a subdirectory whose cluster is 0x1000
 */
#define BAD_CLUS 0x1000u

static void build_stream_image(void)
{
    memset(g_mem, 0, sizeof g_mem);

    uint8_t *bs = g_mem;
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(&bs[3], "MSDOS5.0", 8);
    put16(&bs[11], MEM_BPS);
    bs[13] = 1;
    put16(&bs[14], 1);
    bs[16] = 1;
    bs[21] = 0xF8;
    put32(&bs[36], 1);
    put32(&bs[44], 2);
    bs[510] = 0x55; bs[511] = 0xAA;

    uint8_t *fat = &g_mem[1 * MEM_BPS];
    put32(&fat[0 * 4], 0x0FFFFFF8u);
    put32(&fat[1 * 4], 0x0FFFFFFFu);
    put32(&fat[2 * 4], 0x0FFFFFFFu);   /* root            EOC */
    put32(&fat[3 * 4], 0x0FFFFFFFu);   /* SHORT.TXT       EOC after one cluster */
    put32(&fat[4 * 4], 5);             /* TWO.TXT 4 -> 5 */
    put32(&fat[5 * 4], 0x0FFFFFFFu);   /* TWO.TXT 5      EOC */

    uint8_t *root = &g_mem[2 * MEM_BPS];
    put_dirent(&root[0],   "EMPTY   TXT", 0x20, 0,        0);
    put_dirent(&root[32],  "BADCLUS TXT", 0x20, BAD_CLUS, 100);
    put_dirent(&root[64],  "SHORT   TXT", 0x20, 3,        1034);
    put_dirent(&root[96],  "TWO     TXT", 0x20, 4,        1000);
    put_dirent(&root[128], "BADDIR     ", 0x10, BAD_CLUS, 0);

    for (int i = 0; i < 1000; i++) {
        g_mem[4 * MEM_BPS + i] = (uint8_t)((i * 7) & 0xFF);   /* 4 then 5 */
    }
    for (int i = 0; i < 512; i++) {
        g_mem[3 * MEM_BPS + i] = (uint8_t)(0xA5 ^ i);
    }
}

static int two_ok(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        if (p[i] != (uint8_t)((i * 7) & 0xFF)) {
            return 0;
        }
    }
    return 1;
}

/* One mount per fault-injection case, so each starts with COLD fat32.c
 * caches (they are tagged by fat32_t pointer — see the comment at the tests). */
static fat32_t g_fs_fatfail, g_fs_datfail, g_fs_persist;

/* And one for the BPB whose geometry does not fit in 32 bits. */
static fat32_t g_fs_overflow;

/* ---- fourth image: long-name runs with stale fragments in front of them ----
 *
 * Same 512-byte geometry; the root spans clusters 2 -> 3 (two sectors, 32
 * slots) because the shapes below need more than 16. File clusters are never
 * read, so every file points at cluster 4 with a token size.
 *
 * The VFAT 8.3 checksum (byte 13 of every LFN fragment) is what binds a run
 * to its 8.3 entry, and the 0x40 flag on a fragment marks the physically
 * FIRST piece of a run. The reader used to latch "bad" the moment a
 * fragment's checksum differed from the run in progress — which is the first
 * fragment of the REAL run whenever a stale fragment precedes it — so the
 * file fell back to its 8.3 name with name_lossy 0, and the library (which
 * binds a file to its index record by the hash of the full name) never
 * matched it: listed, unplayable. */
static fat32_t g_fs_lfn;

static uint8_t lfn_sum(const char *raw11)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) {
        s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + (uint8_t)raw11[i]);
    }
    return s;
}

/* One LFN fragment: sequence `seq` (1-based), `last` sets the 0x40 first-
 * piece flag, checksum `sum`, and the 13 units this fragment carries taken
 * from `name` at (seq-1)*13 — characters, then one 0x0000 terminator, then
 * 0xFFFF padding, exactly as a conforming writer lays them out. */
static void put_lfn(uint8_t *e, int seq, int last, uint8_t sum, const char *name)
{
    static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    size_t len = strlen(name);
    memset(e, 0, 32);
    e[0]  = (uint8_t)(seq | (last ? 0x40 : 0));
    e[11] = 0x0F;
    e[13] = sum;
    for (int k = 0; k < 13; k++) {
        size_t   idx = (size_t)(seq - 1) * 13u + (size_t)k;
        uint16_t u   = idx < len ? (uint8_t)name[idx] : idx == len ? 0x0000u : 0xFFFFu;
        put16(&e[pos[k]], u);
    }
}

/* A whole run for `name`, bound to the 8.3 entry `raw11`, in physical order
 * (highest sequence first, 0x40 on it). Returns the number of slots used. */
static int put_lfn_run(uint8_t *e, const char *name, const char *raw11)
{
    int     n   = (int)((strlen(name) + 1u + 12u) / 13u);   /* incl. terminator */
    uint8_t sum = lfn_sum(raw11);
    for (int i = 0; i < n; i++) {
        int seq = n - i;
        put_lfn(e + i * 32, seq, seq == n, sum, name);
    }
    return n;
}

static void build_lfn_image(void)
{
    memset(g_mem, 0, sizeof g_mem);

    uint8_t *bs = g_mem;
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(&bs[3], "MSDOS5.0", 8);
    put16(&bs[11], MEM_BPS);
    bs[13] = 1;
    put16(&bs[14], 1);
    bs[16] = 1;
    bs[21] = 0xF8;
    put32(&bs[36], 1);
    put32(&bs[44], 2);
    bs[510] = 0x55; bs[511] = 0xAA;

    uint8_t *fat = &g_mem[1 * MEM_BPS];
    put32(&fat[0 * 4], 0x0FFFFFF8u);
    put32(&fat[1 * 4], 0x0FFFFFFFu);
    put32(&fat[2 * 4], 3);             /* root 2 -> 3 */
    put32(&fat[3 * 4], 0x0FFFFFFFu);
    put32(&fat[4 * 4], 0x0FFFFFFFu);

    uint8_t *e = &g_mem[2 * MEM_BPS];  /* root: clusters 2 and 3 are adjacent */

    /* 1. THE BUG. One stale fragment (seq 1, no 0x40, a checksum for a short
     *    name that is not here) directly ahead of a good two-fragment run. */
    put_lfn(e, 1, 0, lfn_sum("NOTTHIS FLA"), "Ghost.flac");            e += 32;
    e += 32 * put_lfn_run(e, "Intentions.flac", "INTENT~1FLA");
    put_dirent(e, "INTENT~1FLA", 0x20, 4, 100);                          e += 32;

    /* 2. A whole stale run (0x40 and all) ahead of a good one: two runs back
     *    to back, no 8.3 entry between them. */
    e += 32 * put_lfn_run(e, "Stale name.txt", "STALE   TXT");
    e += 32 * put_lfn_run(e, "Second file.txt", "SECOND~1TXT");
    put_dirent(e, "SECOND~1TXT", 0x20, 4, 100);                          e += 32;

    /* 3. A stale fragment carrying the SAME checksum as the run that follows
     *    (the leftover of a rewrite of this very entry). Only the 0x40 flag
     *    separates them, and it must. */
    put_lfn(e, 1, 0, lfn_sum("THIRD~1 TXT"), "Third fXXX.txt");        e += 32;
    e += 32 * put_lfn_run(e, "Third file.txt", "THIRD~1 TXT");
    put_dirent(e, "THIRD~1 TXT", 0x20, 4, 100);                          e += 32;

    /* 4. A good run with ONE corrupt checksum byte, on its seq-1 fragment.
     *    Restarting on the mismatch must not leave a partial run that names
     *    the file by its first 13 characters: this is a fallback to 8.3. */
    e += 32 * put_lfn_run(e, "Fourth file.txt", "FOURTH~1TXT");
    e[-32 + 13] ^= 0x55;                                                 /* seq 1 is the last slot written */
    put_dirent(e, "FOURTH~1TXT", 0x20, 4, 100);                          e += 32;

    /* 5. The case that already worked and must keep working: a stale run
     *    directly ahead of an 8.3 entry it does not belong to. */
    e += 32 * put_lfn_run(e, "Ghost.flac", "NOTTHIS FLA");
    put_dirent(e, "REAL    TXT", 0x20, 4, 100);                          e += 32;
    /* e[0] == 0x00 from here: end of directory */
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

    /* ---- path resolution over the same volume ----
     * fat32_resolve_path is the segment walk a playlist entry goes through:
     * "Music/Artist - Album/track.flac" -> one directory entry. The same
     * root -> MUSIC/ -> SONG.TXT image covers every shape of it: a file two
     * levels down, a directory as the final entry, the separator and case
     * forms a desktop tool writes, and every way a path fails. */
    {
        fat32_dirent_t d;
        uint32_t parent;

        memset(&d, 0, sizeof d); parent = 0;
        fails += check("resolve MUSIC/SONG.TXT",
                       fat32_resolve_path(&mfs, mfs.root_clus, "MUSIC/SONG.TXT",
                                          &d, &parent) == 0 &&
                       d.first_clus == 4 && d.size == 100 && d.is_dir == 0 &&
                       strcmp(d.name, "SONG.TXT") == 0 && parent == 3);
        memset(&d, 0, sizeof d); parent = 0;
        fails += check("resolve is ASCII case-insensitive per segment",
                       fat32_resolve_path(&mfs, mfs.root_clus, "music/song.txt",
                                          &d, &parent) == 0 &&
                       d.first_clus == 4 && strcmp(d.name, "SONG.TXT") == 0);
        fails += check("resolve accepts a leading '/' (volume-root form)",
                       fat32_resolve_path(&mfs, mfs.root_clus, "/MUSIC/SONG.TXT",
                                          &d, &parent) == 0 && d.first_clus == 4);
        fails += check("resolve accepts '\\' separators and a leading one",
                       fat32_resolve_path(&mfs, mfs.root_clus, "\\MUSIC\\SONG.TXT",
                                          &d, &parent) == 0 && d.first_clus == 4);
        fails += check("resolve collapses repeated and trailing separators",
                       fat32_resolve_path(&mfs, mfs.root_clus, "MUSIC//SONG.TXT/",
                                          &d, &parent) == 0 && d.first_clus == 4);
        memset(&d, 0, sizeof d); parent = 0;
        fails += check("resolve a directory as the final entry",
                       fat32_resolve_path(&mfs, mfs.root_clus, "MUSIC",
                                          &d, &parent) == 0 &&
                       d.first_clus == 3 && d.is_dir == 1 &&
                       parent == mfs.root_clus);
        fails += check("resolve a directory with a trailing separator",
                       fat32_resolve_path(&mfs, mfs.root_clus, "MUSIC/",
                                          &d, &parent) == 0 && d.first_clus == 3);
        memset(&d, 0, sizeof d); parent = 0;
        fails += check("resolve relative to a non-root start directory",
                       fat32_resolve_path(&mfs, 3, "SONG.TXT", &d, &parent) == 0 &&
                       d.first_clus == 4 && parent == 3);

        /* Not found, in each place it can be not found. */
        fails += check("resolve: missing last segment is ENOENT",
                       fat32_resolve_path(&mfs, mfs.root_clus, "MUSIC/NOPE.TXT",
                                          &d, &parent) == FAT32_ENOENT);
        fails += check("resolve: missing first segment is ENOENT",
                       fat32_resolve_path(&mfs, mfs.root_clus, "NOPE/SONG.TXT",
                                          &d, &parent) == FAT32_ENOENT);
        fails += check("resolve: a file used as a folder is ENOENT",
                       fat32_resolve_path(&mfs, mfs.root_clus, "MUSIC/SONG.TXT/X",
                                          &d, &parent) == FAT32_ENOENT);

        /* Malformed: refused before any directory is read. */
        fails += check("resolve: empty path is EINVAL",
                       fat32_resolve_path(&mfs, mfs.root_clus, "", &d, &parent)
                           == FAT32_EINVAL);
        fails += check("resolve: a bare separator is EINVAL",
                       fat32_resolve_path(&mfs, mfs.root_clus, "/", &d, &parent)
                           == FAT32_EINVAL);
        fails += check("resolve: '..' is never walked (EINVAL, not ENOENT)",
                       fat32_resolve_path(&mfs, mfs.root_clus,
                                          "MUSIC/../MUSIC/SONG.TXT",
                                          &d, &parent) == FAT32_EINVAL);
        fails += check("resolve: '.' is refused too",
                       fat32_resolve_path(&mfs, mfs.root_clus, "./MUSIC",
                                          &d, &parent) == FAT32_EINVAL);
        fails += check("resolve: a '...' segment is a NAME, not a dot entry",
                       fat32_resolve_path(&mfs, mfs.root_clus, ".../SONG.TXT",
                                          &d, &parent) == FAT32_ENOENT);
        {
            /* FAT32_PATH_SEGS_MAX + 1 segments of an existing name: refused
             * by the count, not by the walk (the walk would fail on segment
             * two, which is a file — so ENOENT would mean the cap was not
             * checked first). */
            char deep[FAT32_PATH_SEGS_MAX * 6 + 8];
            int  di = 0;
            for (uint32_t k = 0; k < FAT32_PATH_SEGS_MAX + 1; k++) {
                memcpy(deep + di, "MUSIC/", 6);
                di += 6;
            }
            deep[di] = '\0';
            fails += check("resolve: more than FAT32_PATH_SEGS_MAX segments is EINVAL",
                           fat32_resolve_path(&mfs, mfs.root_clus, deep,
                                              &d, &parent) == FAT32_EINVAL);
            deep[FAT32_PATH_SEGS_MAX * 6 - 1] = '\0';   /* exactly the cap */
            fails += check("resolve: exactly FAT32_PATH_SEGS_MAX segments is walked",
                           fat32_resolve_path(&mfs, mfs.root_clus, deep,
                                              &d, &parent) == FAT32_ENOENT);
        }
        {
            /* A segment longer than any legal long name. */
            static char longseg[FAT32_NAME_BYTES + 2];
            memset(longseg, 'A', FAT32_NAME_BYTES);
            longseg[FAT32_NAME_BYTES] = '\0';
            fails += check("resolve: an over-long segment is EINVAL",
                           fat32_resolve_path(&mfs, mfs.root_clus, longseg,
                                              &d, &parent) == FAT32_EINVAL);
        }
        {
            /* No NUL within FAT32_PATH_MAX: refused by the bounded scan. */
            static char unterminated[FAT32_PATH_MAX + 4];
            memset(unterminated, 'M', sizeof unterminated);
            fails += check("resolve: no NUL within FAT32_PATH_MAX is EINVAL",
                           fat32_resolve_path(&mfs, mfs.root_clus, unterminated,
                                              &d, &parent) == FAT32_EINVAL);
        }
        fails += check("resolve: null arguments are EINVAL",
                       fat32_resolve_path(NULL, 2, "MUSIC", &d, &parent) == FAT32_EINVAL &&
                       fat32_resolve_path(&mfs, 2, NULL, &d, &parent) == FAT32_EINVAL &&
                       fat32_resolve_path(&mfs, 2, "MUSIC", NULL, &parent) == FAT32_EINVAL &&
                       fat32_resolve_path(&mfs, 2, "MUSIC", &d, NULL) == FAT32_EINVAL);

        /* A disk error in the middle of the walk is reported as one, not as
         * "no such file": sector 3 is the MUSIC directory. */
        fail_sector(3, -1);
        fails += check("resolve: a failed directory read is EIO, not ENOENT",
                       fat32_resolve_path(&mfs, mfs.root_clus, "MUSIC/SONG.TXT",
                                          &d, &parent) == FAT32_EIO);
        fail_sector(NO_FAIL, 0);
        fails += check("resolve: an unaddressable start directory is ECORRUPT",
                       fat32_resolve_path(&mfs, 0x1000u, "SONG.TXT",
                                          &d, &parent) == FAT32_ECORRUPT);
    }

    /* ---- the streaming reader on corrupt and half-written files ----
     *
     * THE BUG. fat32_stream_open on a garbage first cluster with a nonzero
     * size succeeded, and the first fat32_stream_read returned 0 — the same
     * answer as a legitimately empty file. fat32_read_file had already been
     * changed to return FAT32_ECORRUPT for exactly this, and player.c tells
     * an error from EOF, but the stream (the path the player actually uses)
     * never handed it an error to distinguish: a corrupt entry played as a
     * zero-length track and the player moved on. The same collapse happened
     * mid-file: after a FAT read failure the cursor was zeroed, so the retry
     * got EOF for a file with bytes left.
     *
     * A FRESH fat32_t (sfs) rather than mfs, and one more per fault-injection
     * block below: fat32.c's FAT and data caches are tagged by the fs
     * POINTER, so a sector already cached for one fat32_t is never fetched
     * again for it — and a fault staged on that sector would never reach the
     * block callback. Each block that injects a fault mounts its own object,
     * so its caches are cold. They are distinct file-scope objects, not
     * block-scoped locals, because the compiler may give sequential locals
     * the same stack slot, i.e. the same pointer, i.e. the same cache tag. */
    build_stream_image();
    fat32_t sfs;
    fails += check("stream-image mount returns 0",
                   fat32_mount(&sfs, mem_read, NULL, 0) == 0);
    fails += check("stream-image: the bad cluster really is unaddressable",
                   BAD_CLUS >= sfs.max_clus);
    {
        struct { fat32_dirent_t v[8]; int n; } r = { .n = 0 };
        fails += check("stream-image root lists all 5 entries",
                       fat32_readdir(&sfs, sfs.root_clus, dir_collect, &r) == 0 &&
                       r.n == 5);
    }

    /* A genuinely empty file is a clean EOF: 0, and 0 again, and skip skips
     * nothing. This is the other half of "distinguishable": the fix must not
     * turn empty files into errors. */
    {
        fat32_stream_t es;
        uint8_t b[16];
        fat32_stream_open(&es, &sfs, 0, 0);
        fails += check("empty file: first stream_read is a clean EOF (0)",
                       fat32_stream_read(&es, b, sizeof b) == 0);
        fails += check("empty file: stream_skip skips nothing",
                       fat32_stream_skip(&es, 10) == 0);
        fails += check("empty file: stream_read stays a clean EOF",
                       fat32_stream_read(&es, b, sizeof b) == 0);
    }

    /* A bad first cluster with bytes to deliver is ECORRUPT, not EOF — on the
     * first read, on the next read (no sticky "EOF" state), and after a skip,
     * which has no error channel and so must defer to the read. */
    {
        fat32_stream_t bs;
        uint8_t b[128];
        memset(b, 0xCC, sizeof b);
        fat32_stream_open(&bs, &sfs, BAD_CLUS, 100);
        int32_t r1 = fat32_stream_read(&bs, b, sizeof b);
        fails += check("bad first cluster: stream_read returns ECORRUPT, not 0",
                       r1 == FAT32_ECORRUPT);
        fails += check("bad first cluster: nothing was written to the buffer",
                       b[0] == 0xCC);
        fails += check("bad first cluster: a second read is still ECORRUPT",
                       fat32_stream_read(&bs, b, sizeof b) == FAT32_ECORRUPT);
        fails += check("bad first cluster: stream_skip skips nothing",
                       fat32_stream_skip(&bs, 50) == 0);
        fails += check("bad first cluster: the read after a skip is ECORRUPT",
                       fat32_stream_read(&bs, b, sizeof b) == FAT32_ECORRUPT);
        fails += check("bad first cluster: read_file agrees (ECORRUPT)",
                       fat32_read_file(&sfs, BAD_CLUS, b, 100) == FAT32_ECORRUPT);
    }

    /* A chain that ends before the size does: the first cluster reads, then
     * the step to the next finds EOC with 522 bytes still owed. That is
     * corruption (metadata and allocation disagree), and it used to surface
     * as a short read followed by a clean EOF — the tail of the track just
     * gone. The cursor must not move on the failure. */
    {
        fat32_stream_t ss;
        uint8_t b[512];
        fat32_stream_open(&ss, &sfs, 3, 1034);
        int32_t got = fat32_stream_read(&ss, b, 512);
        int first_ok = (got == 512);
        for (int i = 0; i < 512 && first_ok; i++) {
            first_ok = b[i] == (uint8_t)(0xA5 ^ i);
        }
        fails += check("short chain: the one real cluster reads correctly",
                       first_ok);
        fails += check("short chain: the next read is ECORRUPT, not EOF",
                       fat32_stream_read(&ss, b, 512) == FAT32_ECORRUPT);
        fails += check("short chain: the failed read left the cursor alone",
                       ss.remaining == 522 && ss.clus == 3);
        fails += check("short chain: skip stops at the break",
                       fat32_stream_skip(&ss, 522) == 0);
        fails += check("short chain: still ECORRUPT after the skip",
                       fat32_stream_read(&ss, b, 512) == FAT32_ECORRUPT);
    }

    /* TRANSIENT FAT FAILURE. TWO.TXT: 512 bytes from cluster 4, then the FAT
     * (sector 1) is unreadable when the reader steps to cluster 5. The call
     * must fail (not return 512 short, which reads as EOF-ish), leave the
     * cursor exactly where it was, and — once the sector reads again — the
     * SAME call must succeed without a reopen. Under the old code the cursor
     * was zeroed by the failure and the retry returned 0: silent EOF. */
    {
        fat32_stream_t ts;
        static uint8_t b[1000];
        fails += check("transient FAT error: fresh mount (cold caches)",
                       fat32_mount(&g_fs_fatfail, mem_read, NULL, 0) == 0);
        fat32_stream_open(&ts, &g_fs_fatfail, 4, 1000);
        fail_sector(1, 1);                          /* the FAT, once */
        int32_t r1 = fat32_stream_read(&ts, b, 1000);
        fails += check("transient FAT error: stream_read reports EIO",
                       r1 == FAT32_EIO);
        fails += check("transient FAT error: the cursor is unchanged",
                       ts.clus == 4 && ts.clus_off == 0 && ts.remaining == 1000);
        fails += check("transient FAT error: the fault was consumed",
                       g_fail_left == 0);
        memset(b, 0xCC, sizeof b);
        int32_t r2 = fat32_stream_read(&ts, b, 1000);
        fails += check("transient FAT error: the retry delivers all 1000 bytes",
                       r2 == 1000 && two_ok(b, 1000));
        fails += check("transient FAT error: then a clean EOF",
                       fat32_stream_read(&ts, b, 16) == 0);
        fail_sector(NO_FAIL, 0);
    }

    /* TRANSIENT DATA FAILURE, mid-call: cluster 4 reads, cluster 5 (sector 5)
     * fails. The bytes already copied are NOT counted — the call fails as a
     * whole and the cursor is back at the start, so the caller's own byte
     * position (player.c keeps one) stays true. */
    {
        fat32_stream_t ds;
        static uint8_t b[1000];
        fails += check("transient data error: fresh mount (cold caches)",
                       fat32_mount(&g_fs_datfail, mem_read, NULL, 0) == 0);
        fat32_stream_open(&ds, &g_fs_datfail, 4, 1000);
        fail_sector(5, 1);
        int32_t r1 = fat32_stream_read(&ds, b, 1000);
        fails += check("transient data error: stream_read reports EIO",
                       r1 == FAT32_EIO);
        fails += check("transient data error: the cursor is unchanged",
                       ds.clus == 4 && ds.clus_off == 0 && ds.remaining == 1000);
        memset(b, 0xCC, sizeof b);
        fails += check("transient data error: the retry delivers all 1000 bytes",
                       fat32_stream_read(&ds, b, 1000) == 1000 && two_ok(b, 1000));
        fail_sector(NO_FAIL, 0);
    }

    /* PERSISTENT data failure: every call is an error, never an EOF. */
    {
        fat32_stream_t ps;
        static uint8_t b[1000];
        fails += check("persistent data error: fresh mount (cold caches)",
                       fat32_mount(&g_fs_persist, mem_read, NULL, 0) == 0);
        fat32_stream_open(&ps, &g_fs_persist, 4, 1000);
        fail_sector(4, -1);
        fails += check("persistent data error: first read is EIO",
                       fat32_stream_read(&ps, b, 1000) == FAT32_EIO);
        fails += check("persistent data error: second read is EIO, not EOF",
                       fat32_stream_read(&ps, b, 1000) == FAT32_EIO);
        fails += check("persistent data error: nothing was consumed",
                       ps.remaining == 1000);
        fail_sector(NO_FAIL, 0);
    }

    /* ---- directory reads: what the library loader has to act on ----
     *
     * The loader ignored every readdir return code, so a failed album walk
     * was indistinguishable from an empty album. The loader's retry lives in
     * kernel/main.c (static; not reachable from here), but the contract it
     * relies on is testable: a failing walk must SAY so, and a walk retried
     * after the fault clears must deliver the full listing — the reader
     * keeps no state that would make the second attempt fail. And a
     * directory whose cluster is unaddressable is ECORRUPT, not empty. */
    {
        struct { fat32_dirent_t v[8]; int n; } r = { .n = 0 };
        fail_sector(2, 1);                          /* the root, once */
        int rc1 = fat32_readdir(&sfs, sfs.root_clus, dir_collect, &r);
        fails += check("transient dir error: readdir reports EIO",
                       rc1 == FAT32_EIO);
        fails += check("transient dir error: no entries were surfaced",
                       r.n == 0);
        int rc2 = fat32_readdir(&sfs, sfs.root_clus, dir_collect, &r);
        fails += check("transient dir error: the retry lists everything",
                       rc2 == 0 && r.n == 5);
        fail_sector(NO_FAIL, 0);
    }
    {
        struct { fat32_dirent_t v[8]; int n; } r = { .n = 0 };
        fail_sector(2, -1);
        fails += check("persistent dir error: readdir is EIO",
                       fat32_readdir(&sfs, sfs.root_clus, dir_collect, &r) == FAT32_EIO);
        fails += check("persistent dir error: still EIO on retry, no entries",
                       fat32_readdir(&sfs, sfs.root_clus, dir_collect, &r) == FAT32_EIO &&
                       r.n == 0);
        fail_sector(NO_FAIL, 0);
    }
    {
        struct { fat32_dirent_t v[8]; int n; } r = { .n = 0 };
        fails += check("readdir on an unaddressable cluster is ECORRUPT, not empty",
                       fat32_readdir(&sfs, BAD_CLUS, dir_collect, &r) == FAT32_ECORRUPT &&
                       r.n == 0);
        fails += check("readdir on cluster 0 is ECORRUPT, not empty",
                       fat32_readdir(&sfs, 0, dir_collect, &r) == FAT32_ECORRUPT &&
                       r.n == 0);
        /* And the lookup built on it agrees: not "no such file". */
        uint32_t c = 0, s = 0;
        fails += check("open_in on an unaddressable directory is ECORRUPT, not ENOENT",
                       fat32_open_in(&sfs, BAD_CLUS, "X.TXT", &c, &s) == FAT32_ECORRUPT);
    }

    /* ---- long-name runs with stale fragments in front of them ----
     * See build_lfn_image. Five entries come out; what matters is WHICH name
     * each carries, and that none of the good ones is flagged lossy. */
    {
        build_lfn_image();
        struct { fat32_dirent_t v[8]; int n; } r = { .n = 0 };
        fails += check("lfn-image mount returns 0",
                       fat32_mount(&g_fs_lfn, mem_read, NULL, 0) == 0);
        fails += check("lfn-image: readdir lists exactly 5 entries",
                       fat32_readdir(&g_fs_lfn, g_fs_lfn.root_clus, dir_collect, &r) == 0 &&
                       r.n == 5);
        const fat32_dirent_t *by[5] = { 0 };
        for (int i = 0; i < r.n && i < 5; i++) {
            by[i] = &r.v[i];
        }
        fails += check("a stale fragment ahead of a good run: the good run names the file",
                       by[0] && strcmp(by[0]->name, "Intentions.flac") == 0 &&
                       by[0]->name_lossy == 0);
        fails += check("a stale WHOLE run ahead of a good run: the good run names the file",
                       by[1] && strcmp(by[1]->name, "Second file.txt") == 0 &&
                       by[1]->name_lossy == 0);
        fails += check("a stale fragment with the SAME checksum: the 0x40 piece starts the real run",
                       by[2] && strcmp(by[2]->name, "Third file.txt") == 0 &&
                       by[2]->name_lossy == 0);
        fails += check("one corrupt checksum byte mid-run: 8.3 fallback, not a 13-char name",
                       by[3] && strcmp(by[3]->name, "FOURTH~1.TXT") == 0);
        fails += check("a stale run ahead of an unrelated 8.3 entry is still discarded",
                       by[4] && strcmp(by[4]->name, "REAL.TXT") == 0 &&
                       by[4]->name_lossy == 0);
        int no_ghosts = 1;
        for (int i = 0; i < r.n; i++) {
            if (strcmp(r.v[i].name, "Ghost.flac") == 0 ||
                strcmp(r.v[i].name, "Stale name.txt") == 0 ||
                strcmp(r.v[i].name, "Third fXXX.txt") == 0) {
                no_ghosts = 0;
            }
        }
        fails += check("no stale name is ever surfaced", no_ghosts);
        /* And the lookup built on the walk finds the file by its real name. */
        uint32_t c = 0, s = 0;
        fails += check("open by the long name behind a stale fragment succeeds",
                       fat32_open(&g_fs_lfn, "Intentions.flac", &c, &s) == 0 &&
                       c == 4 && s == 100);
    }

    /* ---- a BPB whose data-region start wraps 32 bits ----
     *
     * data_start = RsvdSecCnt + NumFATs * FATSz32, every term straight off
     * the disk. With NumFATs 2 and FATSz32 0x80000000 the product wraps to 0
     * and data_start lands on FS-sector 1 — INSIDE the FAT — while the root
     * cluster still validates and the mount used to report success. The
     * cluster ceiling already clamps the same product; the region start must
     * refuse. (Same in-RAM stream image, with only the two BPB fields
     * changed; a fresh fat32_t so no cached boot sector is consulted.) */
    {
        build_stream_image();
        uint8_t *bs = g_mem;
        bs[16] = 2;                              /* NumFATs */
        put32(&bs[36], 0x80000000u);             /* FATSz32: 2 * this wraps */
        fails += check("BPB with an overflowing data_start is refused at mount",
                       fat32_mount(&g_fs_overflow, mem_read, NULL, 0) != 0);
        /* And the same fields at the last value that does NOT wrap still
         * mount — the guard is on the arithmetic, not on big FATs. */
        put32(&bs[36], 0x7FFFFFFFu);             /* 1 + 2 * 0x7FFFFFFF = 0xFFFFFFFF */
        fails += check("BPB with a data_start of exactly 0xFFFFFFFF still mounts",
                       fat32_mount(&g_fs_overflow, mem_read, NULL, 0) == 0 &&
                       g_fs_overflow.data_start == 0xFFFFFFFFu);
    }

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
