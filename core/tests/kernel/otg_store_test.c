/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/otg_store_test.c — COREOTG.DAT (kernel/otg_store.c), the THIRD
 * thing in the firmware that writes to the user's disk.
 *
 * Nothing here touches a drive: the module's write and wake are function
 * pointers, so the test hands it a RAM disk and a recorder, and the reads go
 * through the volume's own block callback over the same RAM. Layers:
 *
 *   1. THE SLOT CODEC. Round trips at 0, 1 and 512 entries; every way a slot
 *      can be wrong (magic, version 0 and 2, count 513, a flipped bit in the
 *      header / an entry / the padding / the CRC); a null pair inside the
 *      declared count dropped; and the fixture tools/make_otg.py --emit
 *      writes, decoded by THIS decoder — the host encoder and the device
 *      decoder are independent implementations of one spec, and a drift
 *      between them is a freshly created COREOTG.DAT the device silently
 *      refuses.
 *
 *   2. ADDRESSING, on a volume with BytesPerSector 2048 at a non-zero
 *      partition base, COREOTG.DAT deliberately FRAGMENTED (chain
 *      3 -> 7 -> 4 -> 9 -> 5 -> 11) so a 5120-byte slot STRADDLES cluster
 *      boundaries and a slot's runs are not contiguous. Every run's LBA is
 *      checked against the test's own formula over the chain, so a resolver
 *      that assumed contiguity, forgot sec_ratio or the partition base, or
 *      walked one hop too far is caught. The recorder REFUSES any write
 *      outside COREOTG.DAT's own clusters.
 *
 *   3. MOUNT. Fresh / absent / too small / newest seq wins / a torn slot
 *      loses / both bad stays writable and lands the first save in slot 0 /
 *      unreadable with no valid record refuses to write for the session /
 *      a transient read error is retried / the sequence WRAP resolves.
 *
 *   4. SAVE. Slot alternation, a failed write leaving seq and slot untouched,
 *      and the whole list surviving a save/remount round trip.
 *
 *   5. THE COMMIT GATE, through cfg_commit with a hand-advanced clock: idle
 *      is debounced and never wakes a parked drive, soft skips the debounce
 *      but still never wakes, forced wakes FIRST and then writes, the battery
 *      gate defers everything but LAST, and three consecutive failures drop
 *      the change.
 *
 * WHAT THIS CANNOT PROVE: that the drive does what the write says. That is
 * the on-device procedure at the top of kernel/otg_store.c, owed before the
 * first add ever reaches a platter.
 */

#include <stdio.h>
#include <string.h>

#include "fat32.h"
#include "cfg_commit.h"
#include "otg_store.h"

static int g_fails;

static void check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
}

/* ---------------------------------------------------------------------------
 * In-RAM FAT32 volume — evlog_test's geometry, which is the device's.
 *
 *   BytesPerSec 2048  => sec_ratio 4
 *   SecPerClus  1     => one cluster = one FS-sector = 4 LBAs = 2048 B
 *   RsvdSecCnt  1, NumFATs 1, FATSz32 1 => data_start = FS-sector 2, so
 *                        cluster N lives at FS-sector N
 *   part_lba    64
 *
 * Cluster N is at absolute LBA 64 + N * 4. A 5120-byte slot is two and a half
 * clusters, so NO slot is contiguous and slot 1 does not even start on a
 * cluster boundary — which is the whole point of the fixture.
 * ------------------------------------------------------------------------- */
#define IMG_BPS      2048u
#define IMG_PART_LBA 64u
#define IMG_FS_SECS  32u
#define IMG_TOT_LBA  (IMG_PART_LBA + IMG_FS_SECS * (IMG_BPS / 512u))

static uint8_t g_disk[IMG_TOT_LBA * 512u];

#define CHAIN_MAX 8u
static uint32_t g_chain[CHAIN_MAX];
static uint32_t g_nclus;

static uint32_t clus_lba(uint32_t clus) { return IMG_PART_LBA + clus * 4u; }

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

/* The fragmented default: six clusters, 12288 bytes, two 5120-byte slots and
 * change. */
static const uint32_t CHAIN6[6] = { 3, 7, 4, 9, 5, 11 };

/*
 * Build the volume. `nclus` clusters of COREOTG.DAT over `chain`;
 * `dirent_size` overrides the size the directory entry records (0 = the whole
 * chain); `present` 0 omits the file; `part_lba_extra` shifts the partition
 * base by that many 512-byte sectors, which is how a MISALIGNED volume is
 * staged.
 */
static uint32_t g_part_lba = IMG_PART_LBA;

static void build_image(uint32_t nclus, const uint32_t *chain,
                        uint32_t dirent_size, int present)
{
    memset(g_disk, 0, sizeof g_disk);
    g_part_lba = IMG_PART_LBA;
    g_nclus    = nclus;
    for (uint32_t i = 0; i < nclus; i++) {
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
    put32(&fat[2 * 4], 0x0FFFFFFFu);               /* root */
    for (uint32_t i = 0; i < nclus; i++) {
        put32(&fat[chain[i] * 4],
              (i + 1 < nclus) ? chain[i + 1] : 0x0FFFFFFFu);
    }

    uint8_t *root = fs_sec(2);
    /* A neighbour first, so the root walk really has to look past an entry. */
    put_dirent(&root[0], "CORECFG DAT", 0x20, 20, 2048);
    if (present) {
        put_dirent(&root[32], "COREOTG DAT", 0x20, chain[0],
                   dirent_size ? dirent_size : nclus * IMG_BPS);
    } else {
        put_dirent(&root[32], "OTHER   TXT", 0x20, chain[0], 4096);
    }
}

/* The test's OWN address formula, over the chain, for `bytes` bytes from
 * `byte_off` inside the file. Returns the number of runs. */
typedef struct { uint32_t off, lba, sectors; } run_t;

static uint32_t expect_runs(uint32_t byte_off, uint32_t bytes, run_t *out,
                            uint32_t max)
{
    uint32_t n = 0;
    while (bytes > 0 && n < max) {
        uint32_t hop   = byte_off / IMG_BPS;
        uint32_t in    = byte_off % IMG_BPS;
        uint32_t avail = IMG_BPS - in;
        if (avail > bytes) {
            avail = bytes;
        }
        out[n].off     = byte_off;
        out[n].lba     = clus_lba(g_chain[hop]) + in / 512u;
        out[n].sectors = avail / 512u;
        n++;
        byte_off += avail;
        bytes    -= avail;
    }
    return n;
}

/* Slot 0's first LBA under the current image, by the test's own formula. */
static uint32_t a_lba(void)
{
    run_t r[8];
    (void)expect_runs(0, OTG_SLOT_BYTES, r, 8);
    return r[0].lba;
}

/* ---- read side: fault injection, as in evlog_test ----------------------- */

#define NO_FAIL 0xFFFFFFFFu
static uint32_t g_fail_lba  = NO_FAIL;
static uint32_t g_fail_n    = 0;
static int      g_fail_left = 0;

static void fail_sectors(uint32_t lba, uint32_t n, int times)
{
    g_fail_lba = lba; g_fail_n = n; g_fail_left = times;
}

static void fail_none(void) { g_fail_lba = NO_FAIL; g_fail_n = 0; g_fail_left = 0; }

static int mem_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
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

/* ---- write side: the recorder -------------------------------------------
 * Refuses (and flags) anything the driver would refuse — a misaligned LBA or
 * count, an odd buffer — AND anything outside COREOTG.DAT's own clusters,
 * which the driver could not know to refuse. */
static uint32_t g_writes;
static uint32_t g_wbytes;
static int      g_stray_write;
static int      g_wfail_left;
static uint32_t g_wakes;
static uint32_t g_first_wlba;

static int mem_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (g_writes == 0) {
        g_first_wlba = lba;
    }
    g_writes++;
    g_wbytes += count * 512u;
    if (count == 0 || (count % ATA_PHYS_LOG) != 0 || (lba % ATA_PHYS_LOG) != 0 ||
        ((uintptr_t)buf & 1u) != 0) {
        g_stray_write = 1;
        return -1;
    }
    int inside = 0;
    for (uint32_t i = 0; i < g_nclus; i++) {
        uint32_t base = clus_lba(g_chain[i]);
        if (lba >= base && lba + count <= base + IMG_BPS / 512u) {
            inside = 1;
        }
    }
    if (!inside) {
        g_stray_write = 1;      /* crossed a cluster, or left the file */
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
    g_writes = 0; g_wbytes = 0; g_stray_write = 0; g_wfail_left = 0;
    g_wakes = 0; g_first_wlba = 0;
}

/* ---- helpers ------------------------------------------------------------ */

static fat32_t   g_fs;
static otg_list_t g_list;
static uint8_t   g_slot[OTG_SLOT_BYTES] __attribute__((aligned(4)));

static int mount_fs(void)
{
    return fat32_mount(&g_fs, mem_read, 0, g_part_lba);
}

static cfg_commit_env_t env_at(uint32_t now_us, int parked, int battery_ok)
{
    cfg_commit_env_t e;
    e.now_us        = now_us;
    e.parked        = parked;
    e.player_active = 0;
    e.battery_ok    = battery_ok;
    e.writable      = 0;      /* ignored by otg_store_commit; proves it */
    return e;
}

/* Put an encoded slot straight into the RAM disk, run by run — the test's own
 * writer, so a mount can be staged without going through the module. */
static void place_slot(uint32_t slot, const uint8_t *bytes)
{
    run_t runs[8];
    uint32_t n = expect_runs(slot * OTG_SLOT_BYTES, OTG_SLOT_BYTES, runs, 8);
    uint32_t done = 0;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(&g_disk[runs[i].lba * 512u], bytes + done, runs[i].sectors * 512u);
        done += runs[i].sectors * 512u;
    }
}

static void fill_list(otg_list_t *l, uint32_t n, uint32_t base)
{
    otg_init(l);
    for (uint32_t i = 0; i < n; i++) {
        (void)otg_add(l, base + i, base + i + 1000u);
    }
}

static int lists_equal(const otg_list_t *a, const otg_list_t *b)
{
    if (a->n != b->n) {
        return 0;
    }
    for (uint32_t i = 0; i < a->n; i++) {
        if (a->e[i].folder_hash != b->e[i].folder_hash ||
            a->e[i].file_hash != b->e[i].file_hash) {
            return 0;
        }
    }
    return 1;
}

/* ---- 1. the slot codec -------------------------------------------------- */

static void test_codec(void)
{
    otg_list_t src, out;
    uint32_t seq = 0;

    otg_init(&src);
    otg_slot_encode(g_slot, &src, 1);
    check("an empty list round-trips",
          otg_slot_decode(g_slot, &out, &seq) && out.n == 0 && seq == 1);

    fill_list(&src, 1, 0x1000);
    src.gen = 5;
    otg_slot_encode(g_slot, &src, 77);
    check("one entry round-trips",
          otg_slot_decode(g_slot, &out, &seq) && seq == 77 &&
          lists_equal(&src, &out));
    check("...carrying gen", out.gen == 5);

    fill_list(&src, OTG_MAX, 1);
    otg_slot_encode(g_slot, &src, 2);
    check("a full 512-entry list round-trips",
          otg_slot_decode(g_slot, &out, &seq) && out.n == OTG_MAX &&
          lists_equal(&src, &out));

    /* The reserved word and the padding are zero, and the CRC covers them, so
     * the same list always encodes to the same bytes. */
    {
        static uint8_t again[OTG_SLOT_BYTES];
        otg_slot_encode(again, &src, 2);
        check("encoding is deterministic",
              memcmp(again, g_slot, OTG_SLOT_BYTES) == 0);
    }

    /* Every rejection. */
    struct { const char *what; uint32_t off; uint8_t xor_; } flips[] = {
        { "magic",            0,                     0xFF },
        { "version",          4,                     0xFF },
        { "the count",        6,                     0x01 },
        { "seq",              8,                     0x01 },
        { "gen",              12,                    0x01 },
        { "the first entry",  16,                    0x01 },
        { "the last entry",   16 + 511 * 8 + 7,      0x01 },
        { "the reserved word",4112,                  0x01 },
        { "the padding",      OTG_SLOT_BYTES - 5,    0x01 },
        { "the CRC itself",   OTG_SLOT_BYTES - 4,    0x01 },
    };
    for (unsigned i = 0; i < sizeof flips / sizeof flips[0]; i++) {
        static uint8_t bad[OTG_SLOT_BYTES];
        memcpy(bad, g_slot, OTG_SLOT_BYTES);
        bad[flips[i].off] ^= flips[i].xor_;
        char label[96];
        snprintf(label, sizeof label, "a flipped bit in %s is refused",
                 flips[i].what);
        check(label, otg_slot_decode(bad, &out, &seq) == 0);
    }

    {
        /* Version 0 is not a version and version 2 is a future build's. Both
         * with a CORRECT CRC, so it is the version test that refuses them. */
        static uint8_t bad[OTG_SLOT_BYTES];
        for (uint32_t v = 0; v <= 2; v += 2) {
            memcpy(bad, g_slot, OTG_SLOT_BYTES);
            bad[4] = (uint8_t)v;
            bad[5] = 0;
            uint32_t crc = otg_crc32(bad, OTG_SLOT_BYTES - 4u);
            bad[OTG_SLOT_BYTES - 4] = (uint8_t)crc;
            bad[OTG_SLOT_BYTES - 3] = (uint8_t)(crc >> 8);
            bad[OTG_SLOT_BYTES - 2] = (uint8_t)(crc >> 16);
            bad[OTG_SLOT_BYTES - 1] = (uint8_t)(crc >> 24);
            check(v == 0 ? "version 0 is refused with a good CRC"
                         : "a future version is refused with a good CRC",
                  otg_slot_decode(bad, &out, &seq) == 0);
        }

        /* count 513, CRC fixed up: the range check is what refuses it. */
        memcpy(bad, g_slot, OTG_SLOT_BYTES);
        bad[6] = (uint8_t)((OTG_MAX + 1u) & 0xFFu);
        bad[7] = (uint8_t)((OTG_MAX + 1u) >> 8);
        uint32_t crc = otg_crc32(bad, OTG_SLOT_BYTES - 4u);
        bad[OTG_SLOT_BYTES - 4] = (uint8_t)crc;
        bad[OTG_SLOT_BYTES - 3] = (uint8_t)(crc >> 8);
        bad[OTG_SLOT_BYTES - 2] = (uint8_t)(crc >> 16);
        bad[OTG_SLOT_BYTES - 1] = (uint8_t)(crc >> 24);
        check("count > OTG_MAX is refused even with a good CRC",
              otg_slot_decode(bad, &out, &seq) == 0);

        /* A null pair INSIDE the count — what the padding is made of — is
         * dropped rather than stored, so a hand-edited file cannot inject an
         * entry that binds to nothing. */
        fill_list(&src, 3, 9);
        otg_slot_encode(bad, &src, 3);
        memset(&bad[16 + 8], 0, 8);          /* blank entry 1 */
        crc = otg_crc32(bad, OTG_SLOT_BYTES - 4u);
        bad[OTG_SLOT_BYTES - 4] = (uint8_t)crc;
        bad[OTG_SLOT_BYTES - 3] = (uint8_t)(crc >> 8);
        bad[OTG_SLOT_BYTES - 2] = (uint8_t)(crc >> 16);
        bad[OTG_SLOT_BYTES - 1] = (uint8_t)(crc >> 24);
        check("a null pair inside the count is dropped",
              otg_slot_decode(bad, &out, &seq) && out.n == 2 &&
              out.e[0].folder_hash == 9 && out.e[1].folder_hash == 11);
    }

    {
        static uint8_t zero[OTG_SLOT_BYTES];
        memset(zero, 0, sizeof zero);
        check("an all-zero slot is not a slot",
              otg_slot_decode(zero, &out, &seq) == 0);
    }
    check("null arguments are refused",
          otg_slot_decode(0, &out, &seq) == 0 &&
          otg_slot_decode(g_slot, 0, &seq) == 0 &&
          otg_slot_decode(g_slot, &out, 0) == 0);
}

/* ---- 2. the host fixture ------------------------------------------------ */

/* The three pairs tools/make_otg.py --emit puts in slot 0. */
static void test_fixture(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        check("the make_otg.py fixture opens", 0);
        return;
    }
    static uint8_t blob[OTG_SLOT_BYTES * 2];
    size_t got = fread(blob, 1, sizeof blob, f);
    fclose(f);
    check("the fixture is the two-slot region", got == sizeof blob);

    otg_list_t out;
    uint32_t   seq = 0;
    check("the firmware decodes the HOST tool's slot 0",
          otg_slot_decode(blob, &out, &seq) && seq == 1 && out.gen == 3);
    check("...to the three known pairs",
          out.n == 3 &&
          out.e[0].folder_hash == 0x11111111u && out.e[0].file_hash == 0x22222222u &&
          out.e[1].folder_hash == 0xAABBCCDDu && out.e[1].file_hash == 0x01020304u &&
          out.e[2].folder_hash == 0xFFFFFFFFu && out.e[2].file_hash == 0x00000001u);
    check("...and slot 1 is empty, as the tool leaves it",
          otg_slot_decode(&blob[OTG_SLOT_BYTES], &out, &seq) == 0);
}

/* ---- 3. addressing ------------------------------------------------------ */

static void test_addressing(void)
{
    build_image(6, CHAIN6, 0, 1);
    reset_io();
    check("mount", mount_fs() == 0);
    check("a fragmented COREOTG.DAT mounts",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 0 &&
          otg_store_writable() == 1);

    run_t r0[8], r1[8];
    (void)expect_runs(0, OTG_SLOT_BYTES, r0, 8);
    (void)expect_runs(OTG_SLOT_BYTES, OTG_SLOT_BYTES, r1, 8);

    uint32_t lba = 0;
    check("slot 0's probe is the first run of the test's own formula",
          otg_store_probe_lba(0, &lba) == 0 && lba == r0[0].lba);
    check("slot 1's probe likewise — and it is NOT on a cluster boundary",
          otg_store_probe_lba(1, &lba) == 0 && lba == r1[0].lba &&
          (OTG_SLOT_BYTES % IMG_BPS) != 0);
    check("a slot index past the end is refused",
          otg_store_probe_lba(2, &lba) < 0 && lba == 0);

    /* Every write of a save must land inside a cluster of the file and be
     * physical-sector aligned; the recorder flags anything else. */
    fill_list(&g_list, 40, 0x500);
    reset_io();
    check("a save writes the whole slot", otg_store_save(&g_list) == 0 &&
          g_wbytes == OTG_SLOT_BYTES);
    check("...as three runs, none crossing a cluster",
          g_writes == 3 && !g_stray_write);
    check("...starting at the first run of the slot the alternation picked",
          g_first_wlba == r0[0].lba);

    /* A file whose directory entry is shorter than two slots is refused
     * outright: we never address a byte the entry does not cover. */
    build_image(6, CHAIN6, OTG_STORE_MIN_BYTES - 1u, 1);
    check("remount", mount_fs() == 0);
    check("a too-small COREOTG.DAT is not writable",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 0 &&
          otg_store_writable() == 0);
    check("...and a save writes nothing", otg_store_save(&g_list) < 0);

    /* A chain too short for the two slots: the second slot's runs do not
     * resolve, so the module stays off rather than writing what it can. */
    {
        static const uint32_t CHAIN3[3] = { 3, 7, 4 };
        build_image(3, CHAIN3, OTG_STORE_MIN_BYTES, 1);
        check("remount", mount_fs() == 0);
        check("a chain shorter than the size it claims is not writable",
              otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 0 &&
              otg_store_writable() == 0);
    }

    /* Absent. */
    build_image(6, CHAIN6, 0, 0);
    check("remount", mount_fs() == 0);
    reset_io();
    check("no COREOTG.DAT: empty list, not writable",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 0 &&
          otg_store_writable() == 0 && g_list.n == 0);
    check("...and nothing is ever written", g_writes == 0);

    /* A MISALIGNED partition base: every cluster LBA becomes odd, and the
     * drive would IDNF the access, so the module refuses rather than bounce
     * it (which would rewrite bytes nobody asked to touch). */
    build_image(6, CHAIN6, 0, 1);
    g_part_lba = IMG_PART_LBA + 1u;
    memmove(&g_disk[g_part_lba * 512u], &g_disk[IMG_PART_LBA * 512u],
            (IMG_TOT_LBA - g_part_lba) * 512u);
    check("remount on an odd partition base", mount_fs() == 0);
    check("a misaligned volume is not writable",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 0 &&
          otg_store_writable() == 0);
    g_part_lba = IMG_PART_LBA;
}

/* ---- 4. mount ----------------------------------------------------------- */

static void stage(uint32_t slot, uint32_t n, uint32_t base, uint32_t seq,
                  uint16_t gen)
{
    otg_list_t l;
    fill_list(&l, n, base);
    l.gen = gen;
    otg_slot_encode(g_slot, &l, seq);
    place_slot(slot, g_slot);
}

static void test_mount(void)
{
    /* Fresh file (both slots zero): nothing to load, but writable, and the
     * first save lands in SLOT 0 — which is what make_otg.py --verify tells
     * the person watching the bring-up to expect. */
    build_image(6, CHAIN6, 0, 1);
    reset_io();
    check("mount", mount_fs() == 0);
    check("a fresh file loads nothing and stays writable",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 0 &&
          otg_store_writable() == 1 && otg_store_seq() == 0 && g_list.n == 0);
    fill_list(&g_list, 2, 1);
    run_t r0[8];
    (void)expect_runs(0, OTG_SLOT_BYTES, r0, 8);
    check("the first save lands in slot 0",
          otg_store_save(&g_list) == 0 && g_first_wlba == r0[0].lba);
    check("...at seq 1", otg_store_seq() == 1);

    /* Newest seq wins. */
    build_image(6, CHAIN6, 0, 1);
    stage(0, 3, 100, 9, 1);
    stage(1, 5, 200, 10, 2);
    check("mount", mount_fs() == 0);
    check("the newer slot wins",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 1 &&
          g_list.n == 5 && g_list.e[0].folder_hash == 200 &&
          otg_store_seq() == 10 && g_list.gen == 2);

    /* A torn newer slot loses to the intact older one. */
    build_image(6, CHAIN6, 0, 1);
    stage(0, 3, 100, 9, 1);
    stage(1, 5, 200, 10, 2);
    {
        run_t r[8];
        (void)expect_runs(OTG_SLOT_BYTES, OTG_SLOT_BYTES, r, 8);
        g_disk[r[0].lba * 512u + 40] ^= 0x01;    /* a bit inside slot 1 */
    }
    check("mount", mount_fs() == 0);
    check("a torn newer slot loses to the intact older one",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 1 &&
          g_list.n == 3 && g_list.e[0].folder_hash == 100 &&
          otg_store_seq() == 9);

    /* THE WRAP. 0 is newer than 0xFFFFFFFF; a naive unsigned compare would
     * take the old slot for ever after a wrap. */
    build_image(6, CHAIN6, 0, 1);
    stage(0, 1, 700, 0xFFFFFFFFu, 0);
    stage(1, 2, 800, 0u, 0);
    check("mount", mount_fs() == 0);
    check("seq 0 is newer than seq 0xFFFFFFFF",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 1 &&
          g_list.n == 2 && g_list.e[0].folder_hash == 800);

    /* Both slots damaged: nothing to load, still writable, first save to
     * slot 0 — rewriting is exactly how a damaged file recovers. */
    build_image(6, CHAIN6, 0, 1);
    stage(0, 3, 100, 9, 0);
    stage(1, 3, 100, 10, 0);
    {
        run_t a[8], b[8];
        (void)expect_runs(0, OTG_SLOT_BYTES, a, 8);
        (void)expect_runs(OTG_SLOT_BYTES, OTG_SLOT_BYTES, b, 8);
        g_disk[a[0].lba * 512u + 20] ^= 0x01;
        g_disk[b[0].lba * 512u + 20] ^= 0x01;
    }
    reset_io();
    check("mount", mount_fs() == 0);
    check("two damaged slots: empty list, still writable",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 0 &&
          otg_store_writable() == 1 && g_list.n == 0);
    fill_list(&g_list, 1, 1);
    check("...and the recovery save lands in slot 0",
          otg_store_save(&g_list) == 0 && g_first_wlba == a_lba());

    /* An UNREADABLE slot with no valid record anywhere: fail closed. The
     * unread slot may hold the newest list, and a save from seq 0 would lose
     * to it on the next boot. */
    build_image(6, CHAIN6, 0, 1);
    reset_io();
    check("mount", mount_fs() == 0);
    {
        run_t r[8];
        (void)expect_runs(OTG_SLOT_BYTES, OTG_SLOT_BYTES, r, 8);
        fail_sectors(r[0].lba, r[0].sectors, -1);   /* for ever */
    }
    check("an unreadable slot with nothing valid elsewhere refuses to write",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 0 &&
          otg_store_writable() == 0);
    fail_none();

    /* The same slot unreadable, but the OTHER one holds a good record:
     * saving stays enabled — the next write goes to the unreadable slot with
     * a higher seq, which is how a slot a power cut spoiled comes back. */
    build_image(6, CHAIN6, 0, 1);
    stage(0, 4, 300, 12, 0);
    check("mount", mount_fs() == 0);
    {
        run_t r[8];
        (void)expect_runs(OTG_SLOT_BYTES, OTG_SLOT_BYTES, r, 8);
        fail_sectors(r[0].lba, r[0].sectors, -1);
    }
    check("an unreadable slot beside a good one stays writable",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 1 &&
          otg_store_writable() == 1 && g_list.n == 4 && otg_store_seq() == 12);
    fail_none();

    /* A TRANSIENT read error is retried, not believed. */
    build_image(6, CHAIN6, 0, 1);
    stage(0, 6, 400, 3, 0);
    check("mount", mount_fs() == 0);
    {
        run_t r[8];
        (void)expect_runs(0, OTG_SLOT_BYTES, r, 8);
        fail_sectors(r[0].lba, r[0].sectors, 1);    /* once */
    }
    check("a transient read error is retried and the slot still loads",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 1 &&
          g_list.n == 6 && otg_store_seq() == 3);
    fail_none();

    check("null arguments are refused",
          otg_store_mount(0, mem_write, mem_wake, &g_list) == 0 &&
          otg_store_mount(&g_fs, 0, mem_wake, &g_list) == 0 &&
          otg_store_mount(&g_fs, mem_write, mem_wake, 0) == 0);
}

/* ---- 5. save ------------------------------------------------------------ */

static void test_save(void)
{
    build_image(6, CHAIN6, 0, 1);
    reset_io();
    check("mount", mount_fs() == 0);
    (void)otg_store_mount(&g_fs, mem_write, mem_wake, &g_list);

    run_t r0[8], r1[8];
    (void)expect_runs(0, OTG_SLOT_BYTES, r0, 8);
    (void)expect_runs(OTG_SLOT_BYTES, OTG_SLOT_BYTES, r1, 8);

    fill_list(&g_list, 3, 10);
    reset_io();
    check("save 1 -> slot 0",
          otg_store_save(&g_list) == 0 && g_first_wlba == r0[0].lba &&
          otg_store_seq() == 1);
    (void)otg_add(&g_list, 99, 99);
    reset_io();
    check("save 2 -> slot 1 (never the one we read from)",
          otg_store_save(&g_list) == 0 && g_first_wlba == r1[0].lba &&
          otg_store_seq() == 2);
    (void)otg_add(&g_list, 98, 98);
    reset_io();
    check("save 3 -> slot 0 again", otg_store_save(&g_list) == 0 &&
          g_first_wlba == r0[0].lba && otg_store_seq() == 3);
    check("no write ever left the file's clusters", !g_stray_write);

    /* Remount: the list comes back whole. */
    otg_list_t saved = g_list;
    check("remount", mount_fs() == 0);
    check("a remount brings the whole list back",
          otg_store_mount(&g_fs, mem_write, mem_wake, &g_list) == 1 &&
          lists_equal(&saved, &g_list) && otg_store_seq() == 3);

    /* A failed write leaves the bookkeeping alone, so the next attempt
     * targets the SAME slot and cannot eat the good one. */
    uint32_t before = otg_store_seq();
    reset_io();
    g_wfail_left = 1;
    check("a failed write reports the driver's code", otg_store_save(&g_list) == -3);
    check("...and leaves seq untouched", otg_store_seq() == before);
    reset_io();
    check("...so the retry targets the same slot",
          otg_store_save(&g_list) == 0 && g_first_wlba == r1[0].lba &&
          otg_store_seq() == before + 1u);

    check("a null list is refused", otg_store_save(0) < 0);
}

/* ---- 6. the commit gate ------------------------------------------------- */

static void test_gate(void)
{
    build_image(6, CHAIN6, 0, 1);
    reset_io();
    check("mount", mount_fs() == 0);
    (void)otg_store_mount(&g_fs, mem_write, mem_wake, &g_list);
    otg_store_commit_clear();
    fill_list(&g_list, 2, 1);

    cfg_commit_env_t e = env_at(1000, 0, 1);
    check("nothing pending is nothing to do",
          otg_store_commit(CFG_COMMIT_IDLE, &e, &g_list) == OTG_COMMIT_NONE &&
          !otg_store_pending());

    otg_store_touch(1000);
    check("a change is pending", otg_store_pending());
    e = env_at(1000 + CFG_SAVE_DEBOUNCE_US - 1u, 0, 1);
    reset_io();
    check("an idle commit inside the debounce writes nothing",
          otg_store_commit(CFG_COMMIT_IDLE, &e, &g_list) == OTG_COMMIT_NONE &&
          g_writes == 0 && otg_store_pending());

    e = env_at(1000 + CFG_SAVE_DEBOUNCE_US, 1, 1);      /* parked */
    check("an idle commit never wakes a parked drive",
          otg_store_commit(CFG_COMMIT_IDLE, &e, &g_list) == OTG_COMMIT_NONE &&
          g_writes == 0 && g_wakes == 0 && otg_store_pending());
    check("a soft commit never wakes one either",
          otg_store_commit(CFG_COMMIT_SOFT, &e, &g_list) == OTG_COMMIT_NONE &&
          g_writes == 0 && g_wakes == 0 && otg_store_pending());

    e = env_at(1000 + CFG_SAVE_DEBOUNCE_US, 0, 1);      /* spinning */
    check("an idle commit past the debounce writes",
          otg_store_commit(CFG_COMMIT_IDLE, &e, &g_list) == OTG_COMMIT_WROTE &&
          g_writes > 0 && g_wakes == 0 && !otg_store_pending());

    /* Forced: no debounce, and the drive is woken FIRST so the spin-up is
     * paid on the read path rather than inside the write's DRQ budget. */
    otg_store_touch(2000);
    e = env_at(2001, 1, 1);
    reset_io();
    check("a forced commit wakes the parked drive and then writes",
          otg_store_commit(CFG_COMMIT_FORCE, &e, &g_list) == OTG_COMMIT_WROTE &&
          g_wakes == 1 && g_writes > 0 && !otg_store_pending());

    /* A soft commit on a SPINNING drive skips the debounce. */
    otg_store_touch(3000);
    e = env_at(3001, 0, 1);
    reset_io();
    check("a soft commit on a spinning drive writes at once",
          otg_store_commit(CFG_COMMIT_SOFT, &e, &g_list) == OTG_COMMIT_WROTE &&
          g_writes > 0 && !otg_store_pending());

    /* The battery gate: nothing below the disk-safe line except the one LAST
     * write, and the refusal is reported once and then quietly. */
    otg_store_touch(4000);
    e = env_at(4000 + CFG_SAVE_DEBOUNCE_US, 0, 0);
    reset_io();
    check("the battery gate defers an idle commit, loudly the first time",
          otg_store_commit(CFG_COMMIT_IDLE, &e, &g_list) == OTG_COMMIT_DEFERRED &&
          g_writes == 0 && otg_store_pending());
    check("...and quietly after that",
          otg_store_commit(CFG_COMMIT_IDLE, &e, &g_list) == OTG_COMMIT_DEFERRED_QUIET);
    check("...a forced commit too",
          otg_store_commit(CFG_COMMIT_FORCE, &e, &g_list) == OTG_COMMIT_DEFERRED_QUIET &&
          g_writes == 0);
    check("...but NOT the DISKSAFE last write",
          otg_store_commit(CFG_COMMIT_LAST, &e, &g_list) == OTG_COMMIT_WROTE &&
          g_writes > 0 && !otg_store_pending());

    /* Three consecutive failures drop the change: the previous slot is still
     * intact, and that beats hammering a drive that has refused three times. */
    otg_store_touch(5000);
    e = env_at(5001, 0, 1);
    reset_io();
    g_wfail_left = 3;
    check("the first failed write keeps the change pending",
          otg_store_commit(CFG_COMMIT_FORCE, &e, &g_list) == OTG_COMMIT_FAILED &&
          otg_store_pending());
    check("...and reports the driver's code", otg_store_last_rc() == -3);
    e = env_at(5001 + CFG_SAVE_DEBOUNCE_US * 2u, 0, 1);
    check("the second keeps it too",
          otg_store_commit(CFG_COMMIT_FORCE, &e, &g_list) == OTG_COMMIT_FAILED &&
          otg_store_pending());
    check("the third drops it",
          otg_store_commit(CFG_COMMIT_FORCE, &e, &g_list) == OTG_COMMIT_FAILED &&
          !otg_store_pending());

    /* Not writable: the change is dropped rather than re-decided every pass,
     * and env->writable is this module's own, not the caller's. */
    build_image(6, CHAIN6, 0, 0);
    check("remount", mount_fs() == 0);
    (void)otg_store_mount(&g_fs, mem_write, mem_wake, &g_list);
    otg_store_commit_clear();
    otg_store_touch(6000);
    e = env_at(6000 + CFG_SAVE_DEBOUNCE_US, 0, 1);
    e.writable = 1;                     /* the CALLER's config, not ours */
    reset_io();
    check("with no COREOTG.DAT the change is dropped, not written",
          otg_store_commit(CFG_COMMIT_FORCE, &e, &g_list) == OTG_COMMIT_NONE &&
          g_writes == 0 && !otg_store_pending());

    check("null arguments are refused",
          otg_store_commit(CFG_COMMIT_FORCE, 0, &g_list) == OTG_COMMIT_NONE &&
          otg_store_commit(CFG_COMMIT_FORCE, &e, 0) == OTG_COMMIT_NONE);
}

int main(int argc, char **argv)
{
    printf("COREOTG.DAT: slot %u B, two slots, minimum file %u B\n",
           (unsigned)OTG_SLOT_BYTES, (unsigned)OTG_STORE_MIN_BYTES);

    test_codec();
    if (argc > 1) {
        test_fixture(argv[1]);
    } else {
        printf("(no make_otg.py fixture given; host/device parity not checked)\n");
    }
    test_addressing();
    test_mount();
    test_save();
    test_gate();

    printf("otg_store_test: %s (%d failure%s)\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
