/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/evlog_test.c — host tests for the on-disk event log
 * (kernel/evlog.c), the SECOND thing in the firmware that writes to the disk.
 *
 * Nothing here touches a drive. The module's write and wake are function
 * pointers, so the test hands it a RAM disk and a recorder, and the reads go
 * through the volume's block callback over the same RAM. Layers:
 *
 *   1. FORMAT. Header and ring-block round trips; every way a block can be
 *      wrong (magic, version, block size, count, a flipped text byte, a
 *      flipped header byte, an over-long length); the slot arithmetic.
 *      Plus the fixture tools/make_log.py --emit writes, decoded with THIS
 *      decoder — the host encoder and the device decoder are independent
 *      implementations of one spec.
 *
 *   2. ADDRESSING, on a volume with BytesPerSector 2048 at a non-zero
 *      partition base, CORELOG.BIN deliberately FRAGMENTED (chain
 *      3 -> 5 -> 4 -> 7 -> 6 -> 8): every block's LBA is the test's own
 *      formula over the chain, so a resolver that assumed contiguity, forgot
 *      sec_ratio or the partition base, or walked one hop too far is caught.
 *      The recorder REFUSES any write outside the file's own clusters.
 *
 *   3. MOUNT. Fresh file; absent; bad header magic; header/dirent size
 *      disagreement; not a whole number of blocks; header unreadable
 *      (retried, then OFF); a transient read error (retried, ON).
 *
 *   4. FLUSH POLICY, through cfg_commit's gate: idle waits for a full block
 *      and the debounce, and never wakes a parked drive; forced writes a
 *      partial block, marks it FINAL, wakes the drive first; the battery
 *      gate defers idle and forced but not LAST; one block per call; a
 *      failed write keeps the bytes pending and three in a row turn the log
 *      off; ring overflow drops the oldest bytes and says so in the block.
 *
 *   5. RING ORDER + BOOT SCAN. Nine blocks into a five-slot ring, then
 *      remount: the binary search finds the newest, the next seq and boot
 *      id follow, a torn block at the cursor is written again, a partially
 *      filled ring resolves, an unreadable slot during the scan leaves the
 *      log OFF, and on a 64-slot ring the scan costs ceil(log2 64) + 2
 *      reads, not 64. The python-written fixture ring is scanned too.
 *
 * WHAT THIS CANNOT PROVE: that the drive does what the write says. That is
 * the on-device procedure in kernel/config.c's banner, owed here too.
 */

#include <stdio.h>
#include <string.h>

#include "fat32.h"
#include "cfg_commit.h"
#include "evlog.h"

static int g_fails;

static void check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
}

/* ---------------------------------------------------------------------------
 * In-RAM FAT32 volume.
 *
 *   BytesPerSec 2048  => sec_ratio 4
 *   SecPerClus  1     => one cluster = one FS-sector = 4 LBAs = ONE BLOCK
 *   RsvdSecCnt  1, NumFATs 1, FATSz32 1 => data_start = FS-sector 2,
 *                        so cluster N lives at FS-sector N
 *   part_lba    64
 *
 * Block i of CORELOG.BIN is cluster chain[i], at absolute LBA
 *     64 + chain[i] * 4
 * Root is cluster 2. The chain is whatever the test asks for.
 * ------------------------------------------------------------------------- */
#define IMG_BPS       2048u
#define IMG_PART_LBA  64u
#define IMG_FS_SECS   80u        /* room for a 65-block file (clusters 3..67) */
#define IMG_TOT_LBA   (IMG_PART_LBA + IMG_FS_SECS * (IMG_BPS / 512u))

static uint8_t g_disk[IMG_TOT_LBA * 512u];

static uint32_t g_chain[IMG_FS_SECS];
static uint32_t g_nblocks;

static uint32_t clus_lba(uint32_t clus) { return IMG_PART_LBA + clus * 4u; }
static uint32_t block_lba_expected(uint32_t block) { return clus_lba(g_chain[block]); }
static uint8_t *block_ptr(uint32_t block) { return &g_disk[block_lba_expected(block) * 512u]; }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_dirent(uint8_t *e, const char *raw, uint8_t attr,
                       uint32_t clus, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, raw, 11);
    e[11] = attr;
    put16(&e[20], (uint16_t)(clus >> 16));
    put16(&e[26], (uint16_t)(clus & 0xFFFFu));
    put32(&e[28], size);
}

static uint8_t *fs_sec(uint32_t s)
{
    return &g_disk[(IMG_PART_LBA * 512u) + s * IMG_BPS];
}

/*
 * Build the volume with CORELOG.BIN as `nblocks` blocks over `chain`
 * (nblocks clusters, in that order). `dirent_size` overrides the size the
 * directory entry records (0 = nblocks * 2048); `present` 0 omits the file.
 * The header block is stamped with `hdr_count` (0 = nblocks) so the
 * header-vs-dirent disagreement can be staged. Ring blocks start zero.
 */
static void build_image(uint32_t nblocks, const uint32_t *chain,
                        uint32_t dirent_size, uint32_t hdr_count, int present)
{
    memset(g_disk, 0, sizeof g_disk);
    g_nblocks = nblocks;
    for (uint32_t i = 0; i < nblocks; i++) {
        g_chain[i] = chain[i];
    }

    uint8_t *bs = fs_sec(0);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(&bs[3], "MSDOS5.0", 8);
    put16(&bs[11], IMG_BPS);
    bs[13] = 1;
    put16(&bs[14], 1);
    bs[16] = 1;
    bs[21] = 0xF8;
    put32(&bs[32], IMG_FS_SECS);
    put32(&bs[36], 1);
    put32(&bs[44], 2);
    bs[510] = 0x55; bs[511] = 0xAA;

    uint8_t *fat = fs_sec(1);
    put32(&fat[0 * 4], 0x0FFFFFF8u);
    put32(&fat[1 * 4], 0x0FFFFFFFu);
    put32(&fat[2 * 4], 0x0FFFFFFFu);           /* root */
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t nx = (i + 1 < nblocks) ? chain[i + 1] : 0x0FFFFFFFu;
        put32(&fat[chain[i] * 4], nx);
    }

    uint8_t *root = fs_sec(2);
    if (present) {
        put_dirent(&root[0], "CORELOG BIN", 0x20, chain[0],
                   dirent_size ? dirent_size : nblocks * EVLOG_BLOCK_BYTES);
    } else {
        put_dirent(&root[0], "OTHER   TXT", 0x20, chain[0], 4096);
    }
    put_dirent(&root[32], "CORECFG DAT", 0x20, 70, 2048);   /* a neighbour */

    evlog_header_encode(block_ptr(0), hdr_count ? hdr_count : nblocks, 0x5EEDF00Du);
}

/* The small, FRAGMENTED file: six blocks, chain 3 -> 5 -> 4 -> 7 -> 6 -> 8. */
static const uint32_t CHAIN6[6] = { 3, 5, 4, 7, 6, 8 };

/* ---- read side: fault injection as in config_test ---------------------- */

#define NO_FAIL 0xFFFFFFFFu
static uint32_t g_fail_lba  = NO_FAIL;
static uint32_t g_fail_n    = 0;
static int      g_fail_left = 0;
static uint32_t g_reads;

static void fail_sectors(uint32_t lba, uint32_t n, int times)
{
    g_fail_lba = lba; g_fail_n = n; g_fail_left = times;
}

static void fail_none(void) { g_fail_lba = NO_FAIL; g_fail_n = 0; g_fail_left = 0; }

static int mem_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    g_reads++;
    if ((uint64_t)lba + count > IMG_TOT_LBA) {
        return -1;
    }
    if (g_fail_lba != NO_FAIL && g_fail_left != 0 &&
        lba < g_fail_lba + g_fail_n && g_fail_lba < lba + count) {
        if (g_fail_left > 0) {
            g_fail_left--;
        }
        return -1;
    }
    memcpy(buf, &g_disk[lba * 512u], count * 512u);
    return 0;
}

/* ---- write side: the recorder --------------------------------------------
 * Refuses (and flags) anything the driver would refuse — misaligned LBA or
 * count, an odd buffer — AND anything outside CORELOG.BIN's own clusters,
 * which the driver could not know to refuse. */
static uint32_t g_writes;
static uint32_t g_last_wlba;
static int      g_stray_write;
static int      g_wfail_left;       /* fail the next N writes with -3 */
static uint32_t g_wakes;

static int mem_write(uint32_t lba, uint32_t count, const void *buf)
{
    g_writes++;
    g_last_wlba = lba;
    if (count != EVLOG_BLOCK_SECTORS || (lba % ATA_PHYS_LOG) != 0 ||
        ((uintptr_t)buf & 1u) != 0) {
        g_stray_write = 1;
        return -1;
    }
    int inside = 0;
    for (uint32_t i = 1; i < g_nblocks; i++) {      /* block 0 is NEVER written */
        if (lba == block_lba_expected(i)) {
            inside = 1;
        }
    }
    if (!inside) {
        g_stray_write = 1;
        return -1;
    }
    if (g_wfail_left > 0) {
        g_wfail_left--;
        return -3;
    }
    memcpy(&g_disk[lba * 512u], buf, count * 512u);
    return 0;
}

static int mem_wake(void)
{
    g_wakes++;
    return 0;
}

static void reset_io(void)
{
    fail_none();
    g_reads = 0; g_writes = 0; g_last_wlba = 0; g_stray_write = 0;
    g_wfail_left = 0; g_wakes = 0;
}

/* ---- helpers ------------------------------------------------------------ */

static fat32_t g_fs;

static int mount_fs(void)
{
    return fat32_mount(&g_fs, mem_read, 0, IMG_PART_LBA);
}

static cfg_commit_env_t env_at(uint32_t now_us, int parked, int battery_ok)
{
    cfg_commit_env_t e;
    e.now_us        = now_us;
    e.parked        = parked;
    e.player_active = 0;
    e.battery_ok    = battery_ok;
    e.writable      = 0;              /* ignored by evlog_flush; proves it */
    return e;
}

static void say(const char *s)
{
    while (*s) {
        evlog_capture((uint8_t)*s++);
    }
}

static void say_n(uint32_t n, char c)
{
    while (n--) {
        evlog_capture((uint8_t)c);
    }
}

/* Drain the capture ring without writing: mount a disabled log is not
 * enough (capture keeps going), so consume through a forced flush into a
 * scratch volume. Used between cases that must start with an empty ring. */
static void drain_ring(void)
{
    build_image(6, CHAIN6, 0, 0, 1);
    check("(drain) mount", mount_fs() == 0 && evlog_mount(&g_fs, mem_write, mem_wake) == 1);
    cfg_commit_env_t e = env_at(1000, 0, 1);
    for (int i = 0; i < 16 && evlog_pending(); i++) {
        (void)evlog_flush(CFG_COMMIT_FORCE, &e);
    }
    check("(drain) ring empty", evlog_pending() == 0);
    reset_io();
}

/* Decode the block at ring slot for `seq` straight out of the RAM disk,
 * through the TEST's own address formula. */
static int disk_block(uint32_t seq, uint32_t *oseq, uint16_t *oboot,
                      uint32_t *olen, int *ofinal, const uint8_t **otext)
{
    uint32_t idx = evlog_block_index(seq, g_nblocks);
    const uint8_t *blk = block_ptr(idx);
    if (!evlog_block_decode(blk, oseq, oboot, olen, ofinal)) {
        return 0;
    }
    *otext = blk + EVLOG_HDR_BYTES;
    return 1;
}

/* ---- 1. format --------------------------------------------------------- */

static uint32_t g_scratch[EVLOG_BLOCK_BYTES / 4u];

static void test_format(void)
{
    uint8_t *blk = (uint8_t *)g_scratch;
    uint32_t count = 0, fid = 0;

    evlog_header_encode(blk, 2048, 0xA5A5F00Fu);
    check("header round-trips", evlog_header_decode(blk, &count, &fid) &&
                                count == 2048 && fid == 0xA5A5F00Fu);
    check("header magic is CLOG", memcmp(blk, "CLOG", 4) == 0);
    check("header block size is 2048 LE", blk[6] == 0x00 && blk[7] == 0x08);
    blk[0] ^= 1;
    check("header bad magic rejected", !evlog_header_decode(blk, &count, &fid));
    evlog_header_encode(blk, 2048, 1); blk[4] = 2;
    check("header future version rejected", !evlog_header_decode(blk, &count, &fid));
    evlog_header_encode(blk, 2048, 1); blk[6] = 0x00; blk[7] = 0x04;
    check("header wrong block size rejected", !evlog_header_decode(blk, &count, &fid));
    evlog_header_encode(blk, 1, 1);
    check("header count 1 (no ring) rejected", !evlog_header_decode(blk, &count, &fid));
    evlog_header_encode(blk, EVLOG_MAX_BLOCKS + 1, 1);
    check("header count over the ceiling rejected", !evlog_header_decode(blk, &count, &fid));
    evlog_header_encode(blk, 2048, 1); blk[12] ^= 1;
    check("header flipped file id fails the CRC", !evlog_header_decode(blk, &count, &fid));
    memset(blk, 0, EVLOG_BLOCK_BYTES);
    check("all-zero header rejected", !evlog_header_decode(blk, &count, &fid));
    check("header null args rejected", !evlog_header_decode(0, &count, &fid) &&
                                       !evlog_header_decode(blk, 0, &fid));

    static const uint8_t text[] = "core: kernel alive\ncore: batt 4123 mV\n";
    uint32_t seq = 0, len = 0; uint16_t boot = 0; int final = 0;
    evlog_block_encode(blk, 0x12345678u, 7, text, sizeof text - 1, 0);
    check("block round-trips", evlog_block_decode(blk, &seq, &boot, &len, &final) &&
                               seq == 0x12345678u && boot == 7 &&
                               len == sizeof text - 1 && final == 0 &&
                               memcmp(blk + EVLOG_HDR_BYTES, text, len) == 0);
    check("block magic is CLOB", memcmp(blk, "CLOB", 4) == 0);
    check("block padding is zero", blk[EVLOG_BLOCK_BYTES - 1] == 0 &&
                                   blk[EVLOG_HDR_BYTES + len] == 0);
    evlog_block_encode(blk, 9, 3, text, sizeof text - 1, 1);
    check("FINAL flag round-trips", evlog_block_decode(blk, &seq, &boot, &len, &final) &&
                                    final == 1 && len == sizeof text - 1 && seq == 9);
    check("FINAL is bit 15 of len", (blk[11] & 0x80) != 0);
    blk[EVLOG_HDR_BYTES + 3] ^= 0x20;
    check("flipped text byte fails the CRC", !evlog_block_decode(blk, &seq, &boot, &len, &final));
    evlog_block_encode(blk, 9, 3, text, sizeof text - 1, 0);
    blk[5] ^= 0x01;                                  /* seq: the header IS covered */
    check("flipped header byte fails the CRC", !evlog_block_decode(blk, &seq, &boot, &len, &final));
    evlog_block_encode(blk, 9, 3, text, sizeof text - 1, 0);
    blk[EVLOG_BLOCK_BYTES - 1] ^= 0x01;              /* padding is covered too */
    check("flipped padding byte fails the CRC", !evlog_block_decode(blk, &seq, &boot, &len, &final));
    evlog_block_encode(blk, 9, 3, text, sizeof text - 1, 0);
    blk[10] = 0xF1; blk[11] = 0x07;                  /* len 2033 > 2032, CRC not recomputed */
    check("over-long length rejected", !evlog_block_decode(blk, &seq, &boot, &len, &final));
    blk[0] ^= 1;
    check("block bad magic rejected", !evlog_block_decode(blk, &seq, &boot, &len, &final));
    memset(blk, 0, EVLOG_BLOCK_BYTES);
    check("all-zero (unwritten) block rejected", !evlog_block_decode(blk, &seq, &boot, &len, &final));

    /* A full block: 2032 bytes exactly, nothing lost. */
    static uint8_t big[EVLOG_TEXT_BYTES + 10];
    for (uint32_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)('a' + i % 26);
    evlog_block_encode(blk, 1, 1, big, EVLOG_TEXT_BYTES, 0);
    check("full 2032-byte block round-trips",
          evlog_block_decode(blk, &seq, &boot, &len, &final) && len == EVLOG_TEXT_BYTES &&
          memcmp(blk + EVLOG_HDR_BYTES, big, EVLOG_TEXT_BYTES) == 0);
    evlog_block_encode(blk, 1, 1, big, sizeof big, 0);
    check("encode clamps text to 2032", evlog_block_decode(blk, &seq, &boot, &len, &final) &&
                                        len == EVLOG_TEXT_BYTES);

    check("slot arithmetic: seq 0 -> block 1", evlog_block_index(0, 2048) == 1);
    check("slot arithmetic: seq 2046 -> block 2047", evlog_block_index(2046, 2048) == 2047);
    check("slot arithmetic: seq 2047 wraps to block 1", evlog_block_index(2047, 2048) == 1);
    check("slot arithmetic: 5-slot ring, seq 9 -> block 5", evlog_block_index(9, 6) == 5);
    check("slot arithmetic: no ring -> 0", evlog_block_index(0, 1) == 0);

    check("crc32 is zlib's", evlog_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
}

/* ---- 1b. the host tool's fixture ---------------------------------------- */

static uint8_t g_fixture[6 * EVLOG_BLOCK_BYTES];
static int     g_have_fixture;

static void test_fixture(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("cannot open fixture %s\n", path);
        g_fails++;
        return;
    }
    size_t n = fread(g_fixture, 1, sizeof g_fixture, f);
    fclose(f);
    check("fixture is six blocks", n == sizeof g_fixture);
    if (n != sizeof g_fixture) {
        return;
    }
    g_have_fixture = 1;

    uint32_t count = 0, fid = 0;
    check("fixture header decodes with the firmware decoder",
          evlog_header_decode(g_fixture, &count, &fid) && count == 6 && fid == 0x0BADCAFEu);

    /* make_log.py's FIXTURE_ENTRIES: nine writes into five slots. */
    uint32_t seq = 0, len = 0; uint16_t boot = 0; int final = 0;
    const uint8_t *b8 = g_fixture + evlog_block_index(8, 6) * EVLOG_BLOCK_BYTES;
    check("fixture seq 8 decodes: boot 2, not final, its text",
          evlog_block_decode(b8, &seq, &boot, &len, &final) && seq == 8 && boot == 2 &&
          !final && len == 19 && memcmp(b8 + EVLOG_HDR_BYTES, "core: boot 2 third\n", 19) == 0);
    const uint8_t *b5 = g_fixture + evlog_block_index(5, 6) * EVLOG_BLOCK_BYTES;
    check("fixture seq 5 decodes: boot 1, FINAL (a forced flush)",
          evlog_block_decode(b5, &seq, &boot, &len, &final) && seq == 5 && boot == 1 && final);
    check("fixture seq 5 sits in slot 1 (wrapped)", b5 == g_fixture + 1 * EVLOG_BLOCK_BYTES);
    const uint8_t *b4 = g_fixture + evlog_block_index(4, 6) * EVLOG_BLOCK_BYTES;
    check("fixture seq 4 survives in slot 5",
          evlog_block_decode(b4, &seq, &boot, &len, &final) && seq == 4 &&
          b4 == g_fixture + 5 * EVLOG_BLOCK_BYTES);
}

/* ---- 2 + 3. addressing and mount ---------------------------------------- */

static void test_mount(void)
{
    /* Fresh file: header only, ring zero. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    check("mount: fs", mount_fs() == 0);
    check("mount: fresh file -> ON", evlog_mount(&g_fs, mem_write, mem_wake) == 1);
    check("mount: fresh -> seq 0, boot 1, no previous",
          evlog_enabled() && evlog_seq() == 0 && evlog_boot_id() == 1 && evlog_prev_final() == -1);
    check("mount: nothing was written", g_writes == 0);

    /* Addresses: the test's own chain formula vs the module's resolver. */
    uint32_t lba = 0;
    check("probe: header block 0 -> cluster 3 = LBA 76",
          evlog_probe_header_lba(&lba) == 0 && lba == block_lba_expected(0) && lba == 64 + 3 * 4);
    check("probe: header probe null out refused", evlog_probe_header_lba(0) != 0);
    check("probe: seq 0 -> block 1 = cluster 5 (LBA 84)",
          evlog_probe_lba(0, &lba) == 0 && lba == 84);
    check("probe: seq 1 -> block 2 = cluster 4 (LBA 80), the chain goes BACKWARDS",
          evlog_probe_lba(1, &lba) == 0 && lba == 80 && lba == block_lba_expected(2));
    check("probe: seq 3 -> block 4 = cluster 6 (LBA 88)",
          evlog_probe_lba(3, &lba) == 0 && lba == 88 && lba == block_lba_expected(4));
    check("probe: seq 4 -> block 5 = cluster 8 (LBA 96)",
          evlog_probe_lba(4, &lba) == 0 && lba == 96 && lba == block_lba_expected(5));
    check("probe: seq 5 wraps to block 1 (LBA 84)",
          evlog_probe_lba(5, &lba) == 0 && lba == 84);
    check("probe: never block 0", evlog_probe_lba(0, &lba) == 0 && lba != block_lba_expected(0));
    check("probe: null out refused", evlog_probe_lba(0, 0) != 0);

    /* Absent. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 0);
    mount_fs();
    check("mount: absent -> OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0 && !evlog_enabled());
    check("mount: absent -> probe refuses", evlog_probe_lba(0, &lba) != 0 && lba == 0 &&
                                           evlog_probe_header_lba(&lba) != 0 && lba == 0);

    /* Header magic wrong. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    block_ptr(0)[0] ^= 0xFF;
    mount_fs();
    check("mount: bad header magic -> OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0);

    /* Header CRC wrong. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    block_ptr(0)[13] ^= 0x01;
    mount_fs();
    check("mount: bad header CRC -> OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0);

    /* Header says 6 blocks, the directory entry says 5. */
    reset_io();
    build_image(6, CHAIN6, 5 * EVLOG_BLOCK_BYTES, 6, 1);
    mount_fs();
    check("mount: header count != dirent size -> OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0);

    /* Header says 5, file is 6: still a disagreement. */
    reset_io();
    build_image(6, CHAIN6, 0, 5, 1);
    mount_fs();
    check("mount: header count < dirent size -> OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0);

    /* Not a whole number of blocks. */
    reset_io();
    build_image(6, CHAIN6, 6 * EVLOG_BLOCK_BYTES - 512, 0, 1);
    mount_fs();
    check("mount: ragged size -> OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0);

    /* Too small for a ring. */
    reset_io();
    build_image(6, CHAIN6, EVLOG_BLOCK_BYTES, 0, 1);
    mount_fs();
    check("mount: one-block file -> OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0);

    /* A chain SHORTER than the size: the dirent says 6 blocks, the FAT
     * links only 3. Mount is fine (the header and the cursor resolve), but
     * a block past the chain must refuse rather than address anything. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    put32(&fs_sec(1)[4 * 4], 0x0FFFFFFFu);          /* cluster 4 (block 2) is now EOC */
    mount_fs();
    check("mount: short chain still mounts", evlog_mount(&g_fs, mem_write, mem_wake) == 1);
    check("probe: seq 1 (block 2, the last linked) resolves", evlog_probe_lba(1, &lba) == 0 && lba == 80);
    check("probe: seq 2 (block 3, past the chain) REFUSES", evlog_probe_lba(2, &lba) != 0 && lba == 0);

    /* Header unreadable, forever: retried, then OFF. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    mount_fs();
    fail_sectors(block_lba_expected(0), 4, -1);
    check("mount: header unreadable -> OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0);

    /* Header unreadable ONCE: the retry settles it. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    mount_fs();
    fail_sectors(block_lba_expected(0), 4, 1);
    check("mount: transient header read error -> retried, ON",
          evlog_mount(&g_fs, mem_write, mem_wake) == 1);

    /* Root unreadable forever. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    mount_fs();
    fail_sectors(clus_lba(2), 4, -1);
    check("mount: root unreadable -> OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0);

    /* Null arguments. */
    check("mount: null fs -> OFF", evlog_mount(0, mem_write, mem_wake) == 0);
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    mount_fs();
    check("mount: null write -> OFF", evlog_mount(&g_fs, 0, mem_wake) == 0);
}

/* ---- 4. flush policy ---------------------------------------------------- */

static void test_flush_policy(void)
{
    uint32_t seq = 0, len = 0; uint16_t boot = 0; int final = 0;
    const uint8_t *text = 0;
    cfg_commit_env_t e;

    drain_ring();
    build_image(6, CHAIN6, 0, 0, 1);
    mount_fs();
    check("policy: mount ON", evlog_mount(&g_fs, mem_write, mem_wake) == 1);
    reset_io();

    /* Nothing pending: even a forced flush writes nothing. */
    e = env_at(1000, 0, 1);
    check("policy: empty ring, forced -> NONE, no write",
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_NONE && g_writes == 0);

    /* A few bytes: idle waits for a whole block. */
    say("core: kernel alive\n");
    check("policy: 19 bytes pending", evlog_pending() == 19);
    check("policy: idle with a partial block -> NONE", evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_NONE);
    e = env_at(1000 + 2 * CFG_SAVE_DEBOUNCE_US, 0, 1);
    check("policy: idle, partial, well past any debounce -> still NONE",
          evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_NONE && g_writes == 0);

    /* Forced: writes the partial block now, FINAL, no debounce. */
    check("policy: forced, drive up -> WROTE", evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_WROTE);
    check("policy: one write, at seq 0's block, no wake",
          g_writes == 1 && g_last_wlba == block_lba_expected(1) && g_wakes == 0 && !g_stray_write);
    check("policy: the block holds seq 0, boot 1, FINAL, the 19 bytes",
          disk_block(0, &seq, &boot, &len, &final, &text) && seq == 0 && boot == 1 &&
          final == 1 && len == 19 && memcmp(text, "core: kernel alive\n", 19) == 0);
    check("policy: ring consumed, cursor advanced",
          evlog_pending() == 0 && evlog_seq() == 1 && evlog_last_rc() == 0);

    /* A full block: idle writes it — after the debounce, and only with the
     * drive up. */
    say_n(EVLOG_TEXT_BYTES + 5, 'x');
    e = env_at(5000000, 1, 1);
    check("policy: idle, full block, drive PARKED -> NONE, no wake, no write",
          evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_NONE && g_writes == 1 && g_wakes == 0);
    e = env_at(5000000, 0, 1);
    check("policy: idle, full block, drive up -> NONE (debounce starts)",
          evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_NONE && g_writes == 1);
    e = env_at(5000000 + CFG_SAVE_DEBOUNCE_US - 1, 0, 1);
    check("policy: one us short of the debounce -> NONE",
          evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_NONE && g_writes == 1);
    e = env_at(5000000 + CFG_SAVE_DEBOUNCE_US, 0, 1);
    check("policy: debounce elapsed -> WROTE", evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_WROTE);
    check("policy: a full 2032-byte block, seq 1, NOT final, 5 bytes left over",
          disk_block(1, &seq, &boot, &len, &final, &text) && seq == 1 && !final &&
          len == EVLOG_TEXT_BYTES && text[0] == 'x' && text[EVLOG_TEXT_BYTES - 1] == 'x' &&
          evlog_pending() == 5 && evlog_seq() == 2 && g_writes == 2);
    check("policy: the leftover does not flush idle",
          evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_NONE && g_writes == 2);

    /* Three full blocks pending: ONE per call, even with the debounce long
     * past. */
    say_n(3 * EVLOG_TEXT_BYTES, 'y');
    check("policy: 3 blocks + 5 bytes pending", evlog_pending() == 3 * EVLOG_TEXT_BYTES + 5);
    e = env_at(20000000, 0, 1);
    check("policy: first pass arms the debounce", evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_NONE);
    e = env_at(20000000 + CFG_SAVE_DEBOUNCE_US, 0, 1);
    check("policy: one block per pass (1)", evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_WROTE && g_writes == 3);
    check("policy: the second block waits for its own debounce",
          evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_NONE && g_writes == 3);
    e = env_at(20000000 + 2 * CFG_SAVE_DEBOUNCE_US, 0, 1);
    check("policy: one block per pass (2)", evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_WROTE && g_writes == 4);
    check("policy: the first of those blocks began with the 5 leftover x's",
          disk_block(2, &seq, &boot, &len, &final, &text) && seq == 2 &&
          text[0] == 'x' && text[4] == 'x' && text[5] == 'y');

    /* Forced with a parked drive: wake FIRST, then write. */
    e = env_at(30000000, 1, 1);
    check("policy: forced, parked -> wakes then WROTE",
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_WROTE && g_wakes == 1 && g_writes == 5);
    check("policy: forced block is FINAL and full",
          disk_block(4, &seq, &boot, &len, &final, &text) && seq == 4 && final && len == EVLOG_TEXT_BYTES);

    /* Battery below disk-safe: idle and forced are refused, LAST is not.
     * Top the 5 leftover bytes up to exactly one block first, so idle has
     * a block to be refused over. */
    say_n(EVLOG_TEXT_BYTES - 5, 'z');
    check("policy: exactly one block pending", evlog_pending() == EVLOG_TEXT_BYTES);
    e = env_at(40000000, 0, 0);
    check("policy: idle below disk-safe, inside the debounce -> NONE (the gate debounces first)",
          evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_NONE && g_writes == 5);
    e = env_at(40000000 + CFG_SAVE_DEBOUNCE_US, 0, 0);
    check("policy: idle below disk-safe -> DEFERRED (logged once)",
          evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_DEFERRED && g_writes == 5);
    check("policy: idle below disk-safe again -> DEFERRED_QUIET",
          evlog_flush(CFG_COMMIT_IDLE, &e) == EVLOG_FLUSH_DEFERRED_QUIET && g_writes == 5);
    check("policy: forced below disk-safe -> DEFERRED_QUIET (same episode)",
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_DEFERRED_QUIET && g_writes == 5);
    check("policy: LAST below disk-safe -> WROTE (the exempt flush)",
          evlog_flush(CFG_COMMIT_LAST, &e) == EVLOG_FLUSH_WROTE && g_writes == 6);
    check("policy: the last write's block is FINAL",
          disk_block(5, &seq, &boot, &len, &final, &text) && seq == 5 && final);
    check("policy: ring drained", evlog_pending() == 0);
    check("policy: no stray write anywhere", !g_stray_write);

    /* A failed write keeps the bytes; three in a row turn the log off. */
    say("core: one\n");
    g_wfail_left = 1;
    e = env_at(50000000, 0, 1);
    check("policy: failed write -> FAILED, rc -3, bytes still pending, seq unchanged",
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_FAILED && evlog_last_rc() == -3 &&
          evlog_pending() == 10 && evlog_seq() == 6 && evlog_failures() == 1 && evlog_enabled());
    check("policy: the next forced flush retries the SAME block and succeeds",
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_WROTE && evlog_seq() == 7 &&
          evlog_failures() == 0 && evlog_pending() == 0);
    say("core: two\n");
    g_wfail_left = 3;
    check("policy: three failures in a row",
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_FAILED &&
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_FAILED && evlog_enabled() &&
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_FAILED);
    check("policy: -> log OFF for the session", !evlog_enabled() && evlog_failures() == 3);
    uint32_t w = g_writes;
    check("policy: OFF -> flush is NONE, no write",
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_NONE && g_writes == w);

    /* Disabled log (absent file): capture still runs, nothing writes. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 0);
    mount_fs();
    check("policy: absent file mounts OFF", evlog_mount(&g_fs, mem_write, mem_wake) == 0);
    say("core: nobody hears this\n");
    e = env_at(60000000, 0, 1);
    check("policy: OFF -> forced flush NONE, zero writes",
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_NONE &&
          evlog_flush(CFG_COMMIT_LAST, &e) == EVLOG_FLUSH_NONE && g_writes == 0);
    check("policy: null env -> NONE", evlog_flush(CFG_COMMIT_FORCE, 0) == EVLOG_FLUSH_NONE);

    /* Overflow: the ring keeps the NEWEST bytes and the next block says how
     * many were lost. */
    drain_ring();
    build_image(6, CHAIN6, 0, 0, 1);
    mount_fs();
    evlog_mount(&g_fs, mem_write, mem_wake);
    reset_io();
    say_n(EVLOG_RING_BYTES, 'o');          /* fills the ring exactly */
    check("overflow: ring full, nothing dropped yet",
          evlog_pending() == EVLOG_RING_BYTES && evlog_dropped() == 0);
    say("core: newest\n");                 /* 13 more: 13 oldest go */
    check("overflow: 13 bytes dropped", evlog_dropped() == 13 && evlog_pending() == EVLOG_RING_BYTES);
    e = env_at(70000000, 0, 1);
    check("overflow: forced -> WROTE", evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_WROTE);
    static const char marker[] = "<evlog: 13 B dropped>\n";
    check("overflow: the block opens with the drop marker",
          disk_block(0, &seq, &boot, &len, &final, &text) && len == EVLOG_TEXT_BYTES &&
          memcmp(text, marker, sizeof marker - 1) == 0 && text[sizeof marker - 1] == 'o');
    check("overflow: the marker is not repeated",
          evlog_dropped() == 0 && evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_WROTE &&
          disk_block(1, &seq, &boot, &len, &final, &text) && text[0] == 'o');
    for (int i = 0; i < 8 && evlog_pending(); i++) {
        (void)evlog_flush(CFG_COMMIT_FORCE, &e);
    }
    check("overflow: the newest line came through last",
          evlog_pending() == 0 &&
          disk_block(evlog_seq() - 1, &seq, &boot, &len, &final, &text) &&
          len >= 13 && memcmp(text + len - 13, "core: newest\n", 13) == 0);
    check("overflow: no stray write", !g_stray_write);
}

/* ---- 5. ring order and the boot scan ------------------------------------ */

static void write_blocks(uint32_t n, const char *tag)
{
    cfg_commit_env_t e = env_at(1000, 0, 1);
    for (uint32_t i = 0; i < n; i++) {
        char line[32];
        snprintf(line, sizeof line, "%s %u\n", tag, (unsigned)i);
        say(line);
        int r = evlog_flush(CFG_COMMIT_FORCE, &e);
        if (r != EVLOG_FLUSH_WROTE) {
            printf("  write_blocks: flush %u returned %d\n", (unsigned)i, r);
            g_fails++;
        }
    }
}

static int slot_has(uint32_t seq_expect, const char *text_expect)
{
    uint32_t seq = 0, len = 0; uint16_t boot = 0; int final = 0;
    const uint8_t *text = 0;
    if (!disk_block(seq_expect, &seq, &boot, &len, &final, &text)) {
        return 0;
    }
    return seq == seq_expect && len == strlen(text_expect) &&
           memcmp(text, text_expect, len) == 0;
}

static void test_ring_and_scan(void)
{
    uint32_t seq = 0, len = 0; uint16_t boot = 0; int final = 0;
    const uint8_t *text = 0;

    drain_ring();
    build_image(6, CHAIN6, 0, 0, 1);
    mount_fs();
    evlog_mount(&g_fs, mem_write, mem_wake);
    reset_io();

    /* Nine blocks into five slots: the ring holds 4..8. */
    write_blocks(9, "blk");
    check("ring: nine writes, no stray", g_writes == 9 && !g_stray_write && evlog_seq() == 9);
    check("ring: slot for seq 4 holds 'blk 4'", slot_has(4, "blk 4\n"));
    check("ring: slot for seq 5 (wrapped onto slot 1) holds 'blk 5'",
          slot_has(5, "blk 5\n") && evlog_block_index(5, 6) == 1);
    check("ring: seq 0 was overwritten by seq 5",
          disk_block(0, &seq, &boot, &len, &final, &text) && seq == 5);
    check("ring: seqs 6, 7, 8 in slots 2, 3, 4",
          slot_has(6, "blk 6\n") && slot_has(7, "blk 7\n") && slot_has(8, "blk 8\n"));
    check("ring: block 0 (the header) is untouched",
          memcmp(block_ptr(0), "CLOG", 4) == 0);
    uint32_t hc = 0, hf = 0;
    check("ring: header still validates", evlog_header_decode(block_ptr(0), &hc, &hf) && hc == 6);
    check("ring: every block carries boot 1",
          disk_block(8, &seq, &boot, &len, &final, &text) && boot == 1);

    /* REMOUNT: the scan must find seq 8 as the newest. */
    reset_io();
    mount_fs();
    check("scan: remount ON", evlog_mount(&g_fs, mem_write, mem_wake) == 1);
    check("scan: next seq 9, boot 2, previous block was FINAL",
          evlog_seq() == 9 && evlog_boot_id() == 2 && evlog_prev_final() == 1);
    check("scan: mount wrote nothing", g_writes == 0);
    say("core: boot two\n");
    cfg_commit_env_t e = env_at(1000, 0, 1);
    check("scan: the next block lands at seq 9, boot 2, in slot 5",
          evlog_flush(CFG_COMMIT_FORCE, &e) == EVLOG_FLUSH_WROTE &&
          g_last_wlba == block_lba_expected(5) &&
          disk_block(9, &seq, &boot, &len, &final, &text) && seq == 9 && boot == 2);

    /* Slots now: 0=seq5 1=seq6 2=seq7 3=seq8 4=seq9. A torn block MID-LAP
     * (seq 6, slot 1): the search probes slots 2, 3, 4 on this ring and
     * never looks at slot 1, so the tear is invisible and the cursor is
     * still seq 10. (Had it probed the torn slot it would have stopped
     * early and re-written the blocks after it — either way a slot of this
     * lap, inside the file.) */
    reset_io();
    block_ptr(evlog_block_index(6, 6))[EVLOG_HDR_BYTES + 1] ^= 0x40;
    mount_fs();
    check("scan: a torn block mid-lap the search does not probe is invisible",
          evlog_mount(&g_fs, mem_write, mem_wake) == 1 &&
          evlog_seq() == 10 && evlog_boot_id() == 3 && evlog_prev_final() == 1);

    /* The lap-boundary tear: seq 10 goes to slot 0; tear THAT write. Slot 0
     * is now invalid, but the previous lap is intact from slot 1 (seqs 6..9
     * — restore 6 first), so the scan anchors on slot 1 and still finds
     * seq 9 newest: the torn seq 10 is simply written again, and the dump
     * keeps one continuous run. */
    block_ptr(evlog_block_index(6, 6))[EVLOG_HDR_BYTES + 1] ^= 0x40;   /* un-tear 6 */
    block_ptr(evlog_block_index(10, 6))[EVLOG_HDR_BYTES + 2] ^= 0x40;  /* tear slot 0 */
    mount_fs();
    check("scan: a torn slot 0 after a wrap anchors on slot 1 (next seq 10)",
          evlog_mount(&g_fs, mem_write, mem_wake) == 1 &&
          evlog_seq() == 10 && evlog_boot_id() == 3 && evlog_prev_final() == 1);

    /* A partially filled ring, and a torn write AT the cursor. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    mount_fs();
    evlog_mount(&g_fs, mem_write, mem_wake);
    write_blocks(4, "part");                         /* slots 0..3 written */
    reset_io();
    mount_fs();
    check("scan: partially filled ring -> next seq 4",
          evlog_mount(&g_fs, mem_write, mem_wake) == 1 && evlog_seq() == 4 && evlog_boot_id() == 2);
    block_ptr(evlog_block_index(4, 6))[EVLOG_HDR_BYTES] = 'z';   /* a torn seq-4 write */
    memcpy(block_ptr(evlog_block_index(4, 6)), "CLOB", 4);
    mount_fs();
    check("scan: a torn block AT the cursor is simply written again (next seq 4)",
          evlog_mount(&g_fs, mem_write, mem_wake) == 1 && evlog_seq() == 4);

    /* A torn FIRST write (seq 0, slot 0) with nothing else: fresh. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    memcpy(block_ptr(1), "CLOB", 4);
    mount_fs();
    check("scan: a torn very first write -> fresh (next seq 0, boot 1)",
          evlog_mount(&g_fs, mem_write, mem_wake) == 1 && evlog_seq() == 0 &&
          evlog_boot_id() == 1 && evlog_prev_final() == -1);

    /* An unreadable slot during the scan: the cursor is unknowable. OFF.
     * With seqs 0..2 in slots 0..2 the search probes slots 2 and 3 (never
     * slot 1), so the failure goes on slot 2 — one the search reads. */
    reset_io();
    build_image(6, CHAIN6, 0, 0, 1);
    mount_fs();
    evlog_mount(&g_fs, mem_write, mem_wake);
    write_blocks(3, "x");
    reset_io();
    fail_sectors(block_lba_expected(evlog_block_index(2, 6)), 4, -1);   /* slot 2 */
    mount_fs();
    check("scan: an unreadable slot -> OFF, rather than guess the cursor",
          evlog_mount(&g_fs, mem_write, mem_wake) == 0 && !evlog_enabled());
    reset_io();
    fail_sectors(block_lba_expected(evlog_block_index(2, 6)), 4, 1);    /* once */
    mount_fs();
    check("scan: a transient slot read error is retried -> ON, next seq 3",
          evlog_mount(&g_fs, mem_write, mem_wake) == 1 && evlog_seq() == 3);

    /* The python-written fixture ring, scanned by the firmware. */
    if (g_have_fixture) {
        reset_io();
        build_image(6, CHAIN6, 0, 0, 1);
        for (uint32_t i = 0; i < 6; i++) {
            memcpy(block_ptr(i), g_fixture + i * EVLOG_BLOCK_BYTES, EVLOG_BLOCK_BYTES);
        }
        mount_fs();
        check("fixture: the host tool's ring mounts ON",
              evlog_mount(&g_fs, mem_write, mem_wake) == 1);
        check("fixture: scan finds seq 8 newest -> next 9, boot 3, not final",
              evlog_seq() == 9 && evlog_boot_id() == 3 && evlog_prev_final() == 0);
    }

    /* A 64-slot ring: the scan is a binary search, not a sweep. */
    {
        static uint32_t chain65[65];
        for (uint32_t i = 0; i < 65; i++) chain65[i] = 3 + i;      /* contiguous */
        drain_ring();
        build_image(65, chain65, 0, 0, 1);
        mount_fs();
        check("big: mount ON", evlog_mount(&g_fs, mem_write, mem_wake) == 1);
        reset_io();
        write_blocks(100, "big");                    /* 100 into 64: holds 36..99 */
        check("big: 100 writes, no stray", g_writes == 100 && !g_stray_write);
        check("big: seq 99 in slot 1 + 99 % 64 = 36",
              slot_has(99, "big 99\n") && evlog_block_index(99, 65) == 36);
        check("big: seq 36 survives in slot 37", slot_has(36, "big 36\n"));
        check("big: seq 35 is gone (overwritten by 99)",
              disk_block(35, &seq, &boot, &len, &final, &text) && seq == 99);

        reset_io();
        mount_fs();
        uint32_t reads_before = g_reads;
        check("big: remount finds next seq 100", evlog_mount(&g_fs, mem_write, mem_wake) == 1 &&
                                                 evlog_seq() == 100 && evlog_boot_id() == 2);
        uint32_t scan_reads = g_reads - reads_before;
        /* root (1) + header (1) + slot 0 (1) + ceil(log2 64) = 6 probes, and
         * the FAT sector once or twice for the chain walk. Far fewer than
         * the 64 a sweep would take. */
        printf("  big: mount took %u reads\n", (unsigned)scan_reads);
        check("big: the scan is O(log n) reads (<= 12), not 64", scan_reads <= 12);

        /* And with the cursor in the second half of the ring. */
        write_blocks(30, "more");                    /* next = 130 -> slot 1 + 130%64 = 3 */
        reset_io();
        mount_fs();
        reads_before = g_reads;
        check("big: cursor past the wrap resolves (next 130)",
              evlog_mount(&g_fs, mem_write, mem_wake) == 1 && evlog_seq() == 130);
        check("big: still O(log n)", g_reads - reads_before <= 12);
    }
}

int main(int argc, char **argv)
{
    test_format();
    if (argc > 1) {
        test_fixture(argv[1]);
    } else {
        printf("(no fixture path given: host/device format agreement not checked)\n");
    }
    test_mount();
    test_flush_policy();
    test_ring_and_scan();

    printf("%s: %d failure(s)\n", g_fails ? "FAILED" : "OK", g_fails);
    return g_fails ? 1 : 0;
}
