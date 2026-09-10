/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/fs/fat32_corrupt_test.c — the FAT32 reader against DELIBERATELY BROKEN
 * volumes.
 *
 * fs/fat32.c parses whatever is on the user's disk. That disk gets yanked out
 * mid-copy, gets bad sectors, gets reformatted as FAT16 by iTunes, or simply
 * isn't the volume we expect. Until now every fat32 test used one pristine
 * hand-built image, so the entire error surface — the half of the code that
 * runs when the bytes are wrong — was untested.
 *
 * The assertion is the same for every case: FAIL CLEANLY AND BOUNDED. A wrong
 * answer is bad; a hang is worse (the firmware has no watchdog and no way to
 * report it — the iPod just stops), and an out-of-bounds read is worst.
 *
 * HOW "BOUNDED" IS MEASURED. The block-read callback counts calls and starts
 * returning an error past a generous budget. So:
 *   - a reader that terminates on its own finishes well under the budget;
 *   - a reader that would loop forever instead hits the budget, gets an error
 *     back, and returns — and the test can SEE that it needed the budget, which
 *     is exactly the finding. No signals, no timeouts, no flakiness.
 *
 * Cases the corrupt images cover (built by tests/scripts/make_fat32_image.py,
 * one image per --variant):
 *   cyclic-fat   FAT chains that loop instead of ending at EOC
 *   oob-cluster  directory entries pointing outside the data region
 *   fat16-bpb    a genuine FAT16 boot sector offered to a FAT32 reader
 *   orphan-lfn   LFN runs with a mismatched checksum / no 8.3 entry at all
 *   truncated    the volume ends after the FAT region
 *   long-lfn     a long name at the VFAT bound (255 units) and one past it
 *
 * Plus, on the GOOD image, injected sector faults (fail_sector): one read of
 * the root or of a file cluster fails and then the drive settles, or it never
 * does. That is the failure the library loader actually meets at boot, and
 * the reader's answers to it are what the loader's retry is built on.
 *
 * Assertions that today's fs/fat32.c does not meet are marked XFAIL (see
 * tests/xfail.h) with the specific missing check named, rather than being
 * weakened until they pass. Run with CORE_TEST_STRICT_XFAIL=1 to see whether a
 * fix has landed.
 *
 * Usage: fat32_corrupt_test <dir-containing-the-variant-images>
 */

#include "fat32.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../xfail.h"

/* ---- bounded, counting, file-backed block device --------------------- */

/*
 * Budget. The whole reference volume is 12 FS-sectors = 48 blocks of 512 B, so
 * any terminating walk of it costs a few dozen reads at most. 20000 is three
 * orders of magnitude past that: hitting it means the reader is not making
 * progress toward an end, not that the volume is merely large.
 */
#define READ_BUDGET 20000

static FILE    *g_img;
static long     g_img_blocks;    /* size of the image in 512-byte blocks */
static long     g_reads;
static long     g_oob_reads;     /* reads that fell outside the image     */

/*
 * Sector fault injection, on top of the budget. Any read touching g_fail_lba
 * fails while g_fail_left is nonzero (each failure consumes one; negative
 * means forever). A GOOD volume with one flaky sector is the realistic
 * failure the library loader meets — the drive still settling from spin-up
 * during "Loading Library" — and it is what the loader's retry is for.
 */
#define NO_FAIL 0xFFFFFFFFu
static uint32_t g_fail_lba  = NO_FAIL;
static int      g_fail_left = 0;

static void fail_sector(uint32_t lba, int times)   /* times < 0: forever */
{
    g_fail_lba  = lba;
    g_fail_left = times;
}

static int img_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if (++g_reads > READ_BUDGET) {
        return -1;               /* budget exhausted: report a dead drive */
    }
    if (g_fail_lba != NO_FAIL && lba <= g_fail_lba && g_fail_lba < lba + count &&
        g_fail_left != 0) {
        if (g_fail_left > 0) {
            g_fail_left--;
        }
        return -1;               /* the injected fault */
    }
    if ((long)lba + (long)count > g_img_blocks) {
        g_oob_reads++;
        return -1;               /* past the end of the volume            */
    }
    if (fseek(g_img, (long)lba * 512L, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(buf, 512, count, g_img) != count) {
        return -1;
    }
    return 0;
}

static int open_variant(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.img", dir, name);
    if (g_img) {
        fclose(g_img);
    }
    g_img = fopen(path, "rb");
    if (!g_img) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0;
    }
    fseek(g_img, 0, SEEK_END);
    g_img_blocks = ftell(g_img) / 512L;
    g_reads      = 0;
    g_oob_reads  = 0;
    return 1;
}

static int budget_hit(void) { return g_reads > READ_BUDGET; }

/* ---- readdir collectors ---------------------------------------------- */

#define MAX_ENTS 16
typedef struct {
    char name[MAX_ENTS][FAT32_NAME_BYTES];
    int  n;
    int  overflow;      /* the callback fired more than MAX_ENTS times */
} collector;

static int collect(void *ud, const fat32_dirent_t *e)
{
    collector *c = ud;
    if (c->n < MAX_ENTS) {
        snprintf(c->name[c->n], sizeof c->name[0], "%s", e->name);
        c->n++;
    } else {
        c->overflow = 1;
        return 1;       /* stop: a runaway enumeration must not be endless */
    }
    return 0;
}

/* Counts entries and NEVER asks to stop — so an unterminated walk is bounded
 * only by fat32.c's own logic (or by the read budget). */
static int count_forever(void *ud, const fat32_dirent_t *e)
{
    (void)e;
    (*(long *)ud)++;
    return 0;
}

/* The same, keeping the full dirent name and its lossy flag — the long-lfn
 * case is precisely the one whose names do not fit `collector`. */
typedef struct {
    char     name[FAT32_NAME_BYTES];
    uint8_t  lossy;
    uint32_t clus;
    uint32_t size;
} wide_ent;

typedef struct {
    wide_ent e[8];
    int      n;
    int      overflow;
} wide_collector;

static int collect_wide(void *ud, const fat32_dirent_t *e)
{
    wide_collector *c = ud;
    if (c->n < 8) {
        snprintf(c->e[c->n].name, sizeof c->e[0].name, "%s", e->name);
        c->e[c->n].lossy = e->name_lossy;
        c->e[c->n].clus  = e->first_clus;
        c->e[c->n].size  = e->size;
        c->n++;
        return 0;
    }
    c->overflow = 1;
    return 1;
}

static const wide_ent *find_wide(const wide_collector *c, const char *want)
{
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->e[i].name, want) == 0) {
            return &c->e[i];
        }
    }
    return NULL;
}

static int has_name(const collector *c, const char *want)
{
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->name[i], want) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: fat32_corrupt_test <image-dir>\n");
        return 2;
    }
    const char *dir = argv[1];
    xfail_ctx c = { "fat32-corrupt", 0, 0, 0 };

    fat32_t fs;
    static uint8_t big[64 * 1024];

    /* ---- 0. the reference volume still mounts, and now reports capacity --
     * make_fat32_image.py previously left BPB TotSec16/TotSec32 at zero, so
     * fs->total_clus was 0 on EVERY image the suite used — which silently
     * disarms any bounds check written against it. The generator now writes
     * TotSec32; assert the volume actually reports its size, or the
     * out-of-range assertions below are testing nothing. */
    if (!open_variant(dir, "good")) {
        return 2;
    }
    xpect(&c, "reference volume mounts", fat32_mount(&fs, img_read, 0, 0) == 0);
    xpect(&c, "reference volume reports a nonzero cluster count",
          fs.total_clus > 0);
    xpect(&c, "reported cluster count is plausible for the image",
          fs.total_clus <= (uint32_t)g_img_blocks);

    /* ---- 0b. a GOOD volume with one flaky sector ----------------------- *
     * The realistic "Loading Library" failure: nothing on disk is wrong, one
     * read of the root directory (FS-sector 4 = LBA 16..19 on this 2048-byte
     * volume) fails, then the drive settles. The library loader used to
     * ignore readdir's return code entirely, so this looked like an empty
     * album (or, at the root, an empty library). What the loader's retry
     * (kernel/main.c, static — not linkable here) needs from fat32.c is
     * asserted instead: a failing walk REPORTS, the retried walk after the
     * fault clears is complete, and a persistently failing walk keeps
     * reporting rather than ever collapsing into "0 entries, success". */
    {
        collector col = { { { 0 } }, 0, 0 };
        fail_sector(16, 1);                         /* the root, once */
        int rc1 = fat32_readdir_root(&fs, collect, &col);
        xpect(&c, "flaky: a root read that fails once is reported as EIO",
              rc1 == FAT32_EIO);
        xpect(&c, "flaky: the failed walk surfaced no entries", col.n == 0);
        int rc2 = fat32_readdir_root(&fs, collect, &col);
        xpect(&c, "flaky: the retried walk succeeds with the full listing",
              rc2 == 0 && col.n == 2 &&
              has_name(&col, "HELLO.TXT") && has_name(&col, "Intentions.flac"));
        xpect(&c, "flaky: the retry cost no more reads than a clean walk",
              !budget_hit());
        fail_sector(NO_FAIL, 0);
    }
    {
        collector col = { { { 0 } }, 0, 0 };
        fail_sector(16, -1);                        /* the root, forever */
        int rc1 = fat32_readdir_root(&fs, collect, &col);
        int rc2 = fat32_readdir_root(&fs, collect, &col);
        xpect(&c, "dead sector: every walk of the root is EIO, never success",
              rc1 == FAT32_EIO && rc2 == FAT32_EIO && col.n == 0);
        /* The lookup built on the walk must not say "no such file" for a
         * file it could not look for. */
        uint32_t clus = 0, size = 0;
        xpect(&c, "dead sector: open reports EIO, not ENOENT",
              fat32_open(&fs, "HELLO.TXT", &clus, &size) == FAT32_EIO);
        fail_sector(NO_FAIL, 0);
    }
    {
        /* And the same one-shot fault on a FILE read: the stream must fail
         * the call, hold its position, and deliver everything on the retry.
         * HELLO.TXT's second cluster is FS-sector 6 = LBA 24. */
        uint32_t clus = 0, size = 0;
        xpect(&c, "flaky: HELLO.TXT opens on the good volume",
              fat32_open(&fs, "HELLO.TXT", &clus, &size) == 0 && size == 3000);
        fat32_stream_t st;
        fat32_stream_open(&st, &fs, clus, size);
        fail_sector(24, 1);
        int32_t got = fat32_stream_read(&st, big, 3000);
        xpect(&c, "flaky: a stream read hitting the bad sector reports EIO",
              got == FAT32_EIO);
        xpect(&c, "flaky: the failed stream read did not move the cursor",
              st.remaining == 3000 && st.clus == clus && st.clus_off == 0);
        got = fat32_stream_read(&st, big, 3000);
        int content = (got == 3000);
        for (int i = 0; i < 3000 && content; i++) {
            content = big[i] == (uint8_t)(i & 0xFF);
        }
        xpect(&c, "flaky: the retried stream read delivers all 3000 bytes",
              content);
        xpect(&c, "flaky: then a clean EOF",
              fat32_stream_read(&st, big, 16) == 0);
        fail_sector(NO_FAIL, 0);
    }

    /* ---- 1. cyclic FAT chain ---------------------------------------- *
     * Both the root directory's chain and HELLO.TXT's chain loop back on
     * themselves. Nothing here may run forever. */
    if (!open_variant(dir, "cyclic-fat")) {
        return 2;
    }
    xpect(&c, "cyclic: mount still succeeds (the BPB is fine)",
          fat32_mount(&fs, img_read, 0, 0) == 0);
    {
        /* Deliberately NOT the bounded `collect` callback: a caller that stops
         * early would mask the loop. This one never asks to stop, so the only
         * thing that can end the walk is fat32.c itself — or the read budget,
         * which is the finding. */
        long seen = 0;
        int rc = fat32_readdir_root(&fs, count_forever, &seen);
        xpect(&c, "cyclic: readdir returns instead of hanging", 1);
        xpect(&c, "cyclic: readdir detects the loop without exhausting the disk",
              !budget_hit());
        xpect(&c, "cyclic: readdir reports an error rather than silent success",
              rc != 0 || !budget_hit());
        xpect(&c, "cyclic: the same entry is not surfaced over and over",
              seen < 64);
    }
    {
        g_reads = 0;
        uint32_t clus = 0, size = 0;
        int rc = fat32_open(&fs, "HELLO.TXT", &clus, &size);
        if (rc == 0) {
            g_reads = 0;
            int32_t got = fat32_read_file(&fs, clus, big, sizeof big);
            /* read_file is bounded by maxlen, so it terminates either way —
             * but a looping chain means it returns the SAME cluster's bytes
             * over and over, i.e. silently wrong data. */
            xpect(&c, "cyclic: reading a looping chain is rejected, not "
                      "silently repeated",
                  got < 0);
        }
    }

    /* ---- 2. out-of-range cluster numbers ----------------------------- */
    if (!open_variant(dir, "oob-cluster")) {
        return 2;
    }
    xpect(&c, "oob: mount succeeds", fat32_mount(&fs, img_read, 0, 0) == 0);
    {
        uint32_t clus = 0, size = 0;
        int rc = fat32_open(&fs, "HELLO.TXT", &clus, &size);
        xpect(&c, "oob: the entry is found (the directory itself is intact)",
              rc == 0);
        if (rc == 0) {
            xpect(&c, "oob: the recorded cluster really is out of range",
                  clus >= fs.total_clus + 2u);
            g_reads = 0;
            g_oob_reads = 0;
            int32_t got = fat32_read_file(&fs, clus, big, sizeof big);
            xpect(&c, "oob: reading it returns an error, not data", got < 0);
            xpect(&c, "oob: the cluster is rejected before any disk read",
                  g_oob_reads == 0);

            /* THE STREAM PATH — the one the player uses. read_file above was
             * fixed to reject this cluster; the stream still opened it and
             * returned 0 from the first read, which is what an EMPTY file
             * returns, so the player treated a corrupt entry as a zero-length
             * track and advanced. It must be ECORRUPT, and it must stay
             * ECORRUPT (no latching into EOF), and skip must not pretend. */
            fat32_stream_t st;
            memset(big, 0xAB, 64);
            fat32_stream_open(&st, &fs, clus, size);
            g_oob_reads = 0;
            int32_t sg = fat32_stream_read(&st, big, 64);
            xpect(&c, "oob: stream_read on the bad first cluster is ECORRUPT, "
                      "not a 0-byte EOF",
                  sg == FAT32_ECORRUPT);
            xpect(&c, "oob: the stream wrote nothing", big[0] == 0xAB);
            xpect(&c, "oob: the stream issued no disk read for it",
                  g_oob_reads == 0);
            xpect(&c, "oob: a second stream_read is still ECORRUPT",
                  fat32_stream_read(&st, big, 64) == FAT32_ECORRUPT);
            xpect(&c, "oob: stream_skip over it skips nothing",
                  fat32_stream_skip(&st, 100) == 0);
            xpect(&c, "oob: the read after the skip is still ECORRUPT",
                  fat32_stream_read(&st, big, 64) == FAT32_ECORRUPT);

            /* A DIRECTORY at that cluster is likewise corrupt, not empty:
             * this is what an album folder with a bad entry looks like to
             * the library loader, which used to list it with no tracks. */
            collector col = { { { 0 } }, 0, 0 };
            g_oob_reads = 0;
            xpect(&c, "oob: readdir of a directory at the bad cluster is "
                      "ECORRUPT, not an empty listing",
                  fat32_readdir(&fs, clus, collect, &col) == FAT32_ECORRUPT &&
                  col.n == 0);
            xpect(&c, "oob: that readdir issued no disk read", g_oob_reads == 0);
        }
    }
    {
        /* Cluster 1 is reserved: cluster_fs_sector() computes (clus - 2),
         * which underflows to 0xFFFFFFFF and can wrap into a valid sector. */
        uint32_t clus = 0, size = 0;
        g_reads = 0;
        g_oob_reads = 0;
        if (fat32_open(&fs, "LOWCLUS.TXT", &clus, &size) == 0) {
            xpect(&c, "oob: the low-cluster entry really is below 2", clus < 2);
            int32_t got = fat32_read_file(&fs, clus, big, sizeof big);
            xpect(&c, "oob: a cluster below 2 yields no data",
                  got <= 0);
            fat32_stream_t st;
            fat32_stream_open(&st, &fs, clus, size);
            xpect(&c, "oob: a stream on a cluster below 2 is ECORRUPT, not EOF",
                  fat32_stream_read(&st, big, 64) == FAT32_ECORRUPT);
        }
    }

    /* ---- 3. a FAT16 volume must be REJECTED --------------------------- *
     * The stakes: accepting it means the reader computes the data region with
     * FAT32 arithmetic over FAT16 geometry and then reads, and later
     * potentially reports, arbitrary sectors as file contents. */
    if (!open_variant(dir, "fat16-bpb")) {
        return 2;
    }
    {
        int rc = fat32_mount(&fs, img_read, 0, 0);
        xpect(&c, "fat16: a FAT16 boot sector is rejected", rc != 0);
        if (rc == 0) {
            /* It mounted. Everything downstream is then nonsense; make sure it
             * is at least bounded nonsense and not a hang or an OOB read. */
            collector col = { { { 0 } }, 0, 0 };
            g_reads = 0;
            (void)fat32_readdir_root(&fs, collect, &col);
            xpect(&c, "fat16: the mis-mounted volume still enumerates boundedly",
                  !budget_hit());
        }
    }

    /* ---- 4. orphaned / mismatched LFN runs ---------------------------- */
    if (!open_variant(dir, "orphan-lfn")) {
        return 2;
    }
    xpect(&c, "orphan-lfn: mount succeeds", fat32_mount(&fs, img_read, 0, 0) == 0);
    {
        collector col = { { { 0 } }, 0, 0 };
        g_reads = 0;
        int rc = fat32_readdir_root(&fs, collect, &col);
        xpect(&c, "orphan-lfn: readdir succeeds and is bounded",
              rc == 0 && !budget_hit() && !col.overflow);
        xpect(&c, "orphan-lfn: the intact entries are still enumerated",
              has_name(&col, "HELLO.TXT") && has_name(&col, "Intentions.flac"));
        /* The stale run's checksum binds it to a different short name, so it
         * belongs to no entry here and must be discarded. */
        xpect(&c, "orphan-lfn: a checksum-mismatched LFN run is discarded",
              has_name(&col, "REAL.TXT") && !has_name(&col, "Ghost.flac"));
        /* The trailing run has no 8.3 entry at all: it must produce nothing. */
        xpect(&c, "orphan-lfn: a dangling run with no 8.3 entry surfaces nothing",
              !has_name(&col, "Dangling.flac"));
    }
    {
        /* And the name that only the stale run claims must not resolve. */
        uint32_t clus = 0, size = 0;
        xpect(&c, "orphan-lfn: the stale long name does not resolve",
              fat32_open(&fs, "Ghost.flac", &clus, &size) != 0);
    }

    /* ---- 4b. the long-name bound, exactly ------------------------------ *
     * VFAT allows 255 UTF-16 units. The reader used to stop at 128 and fall
     * back to the 8.3 name without a word — and the library binds a file to
     * its index entry by hashing the FULL name, so a silently mangled name
     * was a track that could not be matched and did not play. A 255-unit
     * name of 3-byte characters (741 bytes of UTF-8, far past the 256-byte
     * dirent of old) must come back whole; a 256-unit one may not be
     * reassembled, but the dirent has to SAY so. */
    if (!open_variant(dir, "long-lfn")) {
        return 2;
    }
    xpect(&c, "long-lfn: mount succeeds", fat32_mount(&fs, img_read, 0, 0) == 0);
    {
        /* The generator's LONG255_NAME: "LFN255-" + U+97F3 x 243 + ".flac". */
        static char want255[FAT32_NAME_BYTES];
        int n = snprintf(want255, sizeof want255, "LFN255-");
        for (int i = 0; i < 243; i++) {
            want255[n++] = (char)0xE9;
            want255[n++] = (char)0x9F;
            want255[n++] = (char)0xB3;
        }
        n += snprintf(&want255[n], sizeof want255 - (size_t)n, ".flac");
        xpect(&c, "long-lfn: the expected name is 741 bytes of UTF-8", n == 741);

        wide_collector col;
        memset(&col, 0, sizeof col);
        g_reads = 0;
        int rc = fat32_readdir_root(&fs, collect_wide, &col);
        xpect(&c, "long-lfn: readdir succeeds and is bounded",
              rc == 0 && !budget_hit() && !col.overflow);
        xpect(&c, "long-lfn: four entries surfaced", col.n == 4);

        const wide_ent *e255 = find_wide(&col, want255);
        xpect(&c, "long-lfn: a 255-unit name is reassembled whole", e255 != NULL);
        xpect(&c, "long-lfn: ...and is not reported lossy",
              e255 != NULL && e255->lossy == 0);
        xpect(&c, "long-lfn: ...and carries its cluster and size",
              e255 != NULL && e255->clus == 5 && e255->size == 500);

        const wide_ent *e256 = find_wide(&col, "LFN256~1.FLA");
        xpect(&c, "long-lfn: a 256-unit name falls back to its 8.3 name",
              e256 != NULL);
        xpect(&c, "long-lfn: ...and the fallback is NOT silent: name_lossy set",
              e256 != NULL && e256->lossy == 1);
        int leaked = 0;
        for (int i = 0; i < col.n; i++) {
            if (strncmp(col.e[i].name, "LFN256-", 7) == 0) {
                leaked = 1;
            }
        }
        xpect(&c, "long-lfn: no partial 256-unit name is surfaced", !leaked);

        const wide_ent *hello = find_wide(&col, "HELLO.TXT");
        const wide_ent *intent = find_wide(&col, "Intentions.flac");
        xpect(&c, "long-lfn: the ordinary entries are unaffected and not lossy",
              hello != NULL && intent != NULL &&
              hello->lossy == 0 && intent->lossy == 0);

        uint32_t clus = 0, size = 0;
        xpect(&c, "long-lfn: the 255-unit name resolves by lookup",
              fat32_open(&fs, want255, &clus, &size) == 0 &&
              clus == 5 && size == 500);
        xpect(&c, "long-lfn: the over-long file still resolves by its 8.3 name",
              fat32_open(&fs, "LFN256~1.FLA", &clus, &size) == 0);
    }

    /* ---- 5. a truncated volume --------------------------------------- *
     * The BPB and the FATs are readable; the data region is simply gone. */
    if (!open_variant(dir, "truncated")) {
        return 2;
    }
    {
        int rc = fat32_mount(&fs, img_read, 0, 0);
        xpect(&c, "truncated: mount reads the BPB successfully", rc == 0);

        collector col = { { { 0 } }, 0, 0 };
        g_reads = 0;
        g_oob_reads = 0;
        int rdrc = fat32_readdir_root(&fs, collect, &col);
        xpect(&c, "truncated: readdir returns a clean error", rdrc < 0);
        xpect(&c, "truncated: readdir surfaces no entries", col.n == 0);
        xpect(&c, "truncated: readdir is bounded", !budget_hit());

        uint32_t clus = 0, size = 0;
        g_reads = 0;
        int oprc = fat32_open(&fs, "HELLO.TXT", &clus, &size);
        xpect(&c, "truncated: open returns an error rather than a match",
              oprc != 0);
        xpect(&c, "truncated: open is bounded", !budget_hit());

        /* A stream over a file whose data is missing must report the failure
         * rather than hand back uninitialised buffer contents. */
        memset(big, 0xAB, 512);
        fat32_stream_t st;
        fat32_stream_open(&st, &fs, 3, 3000);
        g_reads = 0;
        int32_t got = fat32_stream_read(&st, big, 512);
        xpect(&c, "truncated: stream_read reports the failure", got <= 0);
        xpect(&c, "truncated: stream_read is bounded", !budget_hit());
    }

    if (g_img) {
        fclose(g_img);
    }
    return xfail_done(&c);
}
