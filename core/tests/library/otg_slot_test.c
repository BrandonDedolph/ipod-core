/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/library/otg_slot_test.c — the SAVED On-The-Go playlists
 * (library/otg_slot.c), the FOURTH thing in the firmware that writes to the
 * user's disk and the only one whose file is read back through fs/fat32.c.
 *
 * The volume is the device's geometry — BytesPerSector 2048, one FS-sector
 * per cluster, a non-zero partition base — so a 4096-byte staging write is
 * always TWO cluster runs and never one, and On-The-Go 1.m3u8 is deliberately
 * FRAGMENTED (20 -> 40 -> 21 -> 41) so a resolver that assumed contiguity
 * writes into the wrong place and the recorder says so.
 *
 *   /Music/Artist - Album/01 Song.flac          a normal row
 *   /Music/Artist - Album/02 Other.fla          a 4-char-less extension,
 *                                               written back EXACTLY
 *   /Music/<180 characters>/Track.flac          over M3U_PATH_MAX: unsaveable
 *   /Music/Loose.flac                           a track in the library root
 *   /Music/Playlists/On-The-Go 1.m3u8           empty, fragmented: the target
 *   /Music/Playlists/On-The-Go 2.m3u8           used, three entries
 *   /Music/Playlists/On-The-Go 3.m3u8           torn: header gen 5, trailer 4
 *   /Music/Playlists/On-The-Go 4.m3u8           FOREIGN: a plain playlist
 *   /Music/Playlists/Favourites.m3u8            an ordinary playlist
 *
 * What is asserted is what the feature rests on: a slot file's classification
 * (empty / used / damaged / foreign) from one 512-byte read; that an empty
 * slot is hidden from the Playlists list and a damaged or foreign one is not;
 * the EXACT BYTES a save puts on the disk, down to the padding; that stage 0
 * is written LAST (the whole tear-detection argument); that a row whose name
 * cannot be written is counted rather than silently dropped; and that the
 * parse straight after a save reads what was just written — which it does
 * only because otg_slot_save() drops fat32.c's sector cache.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../library/otg_slot.h"
#include "../../library/playlist.h"
#include "../../library/names.h"

static int g_fails;

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
    return cond;
}

/* ---- the volume --------------------------------------------------------- */

#define BPS        2048u
#define PART_LBA   64u
#define FS_SECS    96u
#define TOT_LBA    (PART_LBA + FS_SECS * (BPS / 512u))

static uint8_t g_disk[TOT_LBA * 512u];

/* Cluster N lives at FS-sector N (RsvdSecCnt 1 + FATSz32 1 = data_start 2,
 * SecPerClus 1), so its absolute LBA is PART_LBA + N * 4. */
static uint32_t clus_lba(uint32_t c) { return PART_LBA + c * 4u; }
static uint8_t *fs_sec(uint32_t s)   { return &g_disk[PART_LBA * 512u + s * BPS]; }
static uint8_t *clus_ptr(uint32_t c) { return fs_sec(c); }

enum {
    C_ROOT = 2, C_MUSIC = 3, C_ROOTFLAC = 4, C_ALBUM = 5, C_PLDIR = 6,
    C_LONG = 7, C_SONG1 = 10, C_SONG2 = 11, C_NOTES = 12, C_LONGTRK = 13,
    C_LOOSE = 14,
    C_SLOT1 = 20, C_SLOT1B = 40, C_SLOT1C = 21, C_SLOT1D = 41,  /* fragmented */
    C_SLOT2 = 22, C_SLOT3 = 26, C_SLOT4 = 30, C_FAV = 31
};

/* On-The-Go 1's cluster chain, in order: the save must follow it. */
static const uint32_t SLOT1_CHAIN[4] = { C_SLOT1, C_SLOT1B, C_SLOT1C, C_SLOT1D };
#define SLOT1_BYTES 8192u
#define SLOT2_BYTES 8192u
#define SLOT3_BYTES 8192u

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_dirent(uint8_t *e, const char *raw11, uint8_t attr,
                       uint32_t clus, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, raw11, 11);
    e[11] = attr;
    put16(&e[20], (uint16_t)(clus >> 16));
    put16(&e[26], (uint16_t)(clus & 0xFFFFu));
    put32(&e[28], size);
}

static uint8_t lfn_sum(const char *s11)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) {
        s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + (uint8_t)s11[i]);
    }
    return s;
}

/* A real VFAT run (highest sequence first, 0x40 on it) followed by its 8.3
 * entry, so the long names go through the same decode a device does. */
static int put_lfn(uint8_t *e, const char *longname, const char *raw11,
                   uint8_t attr, uint32_t clus, uint32_t size)
{
    static const uint8_t pos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    static uint16_t units[13 * 20];
    int nu = 0;
    for (const char *p = longname; *p; p++) {
        units[nu++] = (uint8_t)*p;
    }
    units[nu++] = 0x0000;
    while (nu % 13) {
        units[nu++] = 0xFFFF;
    }
    int nent = nu / 13;
    uint8_t sum = lfn_sum(raw11);
    for (int k = 0; k < nent; k++) {
        int seq = nent - k;
        uint8_t *le = e + k * 32;
        memset(le, 0, 32);
        le[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));
        for (int j = 0; j < 13; j++) {
            uint16_t u = units[(seq - 1) * 13 + j];
            le[pos[j]]     = (uint8_t)u;
            le[pos[j] + 1] = (uint8_t)(u >> 8);
        }
        le[11] = 0x0F;
        le[13] = sum;
    }
    put_dirent(e + nent * 32, raw11, attr, clus, size);
    return (nent + 1) * 32;
}

/* The 180-character album folder: "/Music/<180>/Track.flac" canonicalises to
 * 197 bytes, six past M3U_PATH_MAX, so its row is unsaveable. */
static char g_long_name[181];

static void make_long_name(void)
{
    for (int i = 0; i < 180; i++) {
        g_long_name[i] = (char)('A' + (i % 26));
    }
    g_long_name[180] = '\0';
}

/* The slot files' contents, built with the same rules library/otg_slot.c
 * writes them by — independently, so the two can disagree. */
static uint32_t crc32_ref(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/*
 * Build the expected bytes of a whole slot file: #EXTM3U, the fixed-width
 * directive, one line per entry, the trailer, newlines to the last byte.
 * `hdr_count` / `hdr_gen` override what the header claims, which is how a
 * torn file is staged.
 */
static uint32_t build_slot_file(uint8_t *out, uint32_t size,
                                const char *const *entries, int n,
                                uint16_t gen, int hdr_count, int hdr_gen)
{
    static uint8_t body[16384];
    uint32_t bn = 0;
    for (int i = 0; i < n; i++) {
        uint32_t l = (uint32_t)strlen(entries[i]);
        memcpy(&body[bn], entries[i], l);
        bn += l;
        body[bn++] = '\n';
    }
    uint32_t crc = crc32_ref(body, bn);

    uint32_t o = 0;
    memcpy(&out[o], "#EXTM3U\n", 8); o += 8;
    o += (uint32_t)snprintf((char *)&out[o], OTG_HDR_LINE_BYTES + 1,
                            "#CORE-OTG v1 count=%05d crc=%08X gen=%05d\n",
                            hdr_count < 0 ? n : hdr_count, crc,
                            hdr_gen < 0 ? (int)gen : hdr_gen);
    memcpy(&out[o], body, bn); o += bn;
    o += (uint32_t)snprintf((char *)&out[o], OTG_END_LINE_BYTES + 1,
                            "#CORE-OTG-END gen=%05d\n", (int)gen);
    memset(&out[o], '\n', size - o);
    return crc;
}

/* Scatter `bytes` over a cluster chain, as the file really lies on the disk. */
static void place_file(const uint32_t *chain, uint32_t nclus,
                       const uint8_t *bytes, uint32_t n)
{
    uint32_t done = 0;
    for (uint32_t i = 0; i < nclus && done < n; i++) {
        uint32_t take = n - done < BPS ? n - done : BPS;
        memcpy(clus_ptr(chain[i]), bytes + done, take);
        done += take;
    }
}

static const char *SLOT2_ENTRIES[3] = {
    "/Music/Artist - Album/01 Song.flac",
    "/Music/Artist - Album/02 Other.fla",
    "/Music/Loose.flac",
};
static const char *SLOT3_ENTRIES[2] = {
    "/Music/Artist - Album/01 Song.flac",
    "/Music/Loose.flac",
};

static void build_image(void)
{
    memset(g_disk, 0, sizeof g_disk);
    make_long_name();

    uint8_t *bs = fs_sec(0);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(&bs[3], "MSDOS5.0", 8);
    put16(&bs[11], BPS);
    bs[13] = 1;                  /* SecPerClus */
    put16(&bs[14], 1);           /* RsvdSecCnt */
    bs[16] = 1;                  /* NumFATs    */
    bs[21] = 0xF8;
    put32(&bs[32], FS_SECS);
    put32(&bs[36], 1);           /* FATSz32    */
    put32(&bs[44], C_ROOT);
    bs[510] = 0x55; bs[511] = 0xAA;

    uint8_t *fat = fs_sec(1);
    put32(&fat[0], 0x0FFFFFF8u);
    put32(&fat[4], 0x0FFFFFFFu);
    for (uint32_t c = 2; c < FS_SECS; c++) {
        put32(&fat[c * 4], 0x0FFFFFFFu);
    }
    /* On-The-Go 1 is fragmented and out of order. */
    for (uint32_t i = 0; i + 1 < 4; i++) {
        put32(&fat[SLOT1_CHAIN[i] * 4], SLOT1_CHAIN[i + 1]);
    }
    /* Slots 2 and 3 are contiguous four-cluster runs. */
    for (uint32_t i = 0; i < 3; i++) {
        put32(&fat[(C_SLOT2 + i) * 4], C_SLOT2 + i + 1);
        put32(&fat[(C_SLOT3 + i) * 4], C_SLOT3 + i + 1);
    }

    uint8_t *d;
    int off;

    d = clus_ptr(C_ROOT); off = 0;
    off += put_lfn(d + off, "Music",     "MUSIC      ", 0x10, C_MUSIC, 0);
    off += put_lfn(d + off, "Root.flac", "ROOT    FLA", 0x20, C_ROOTFLAC, 700);

    d = clus_ptr(C_MUSIC); off = 0;
    put_dirent(d + off, ".          ", 0x10, C_MUSIC, 0); off += 32;
    put_dirent(d + off, "..         ", 0x10, C_ROOT, 0);  off += 32;
    off += put_lfn(d + off, "Artist - Album", "ARTIST~1   ", 0x10, C_ALBUM, 0);
    off += put_lfn(d + off, "Playlists",      "PLAYLI~1   ", 0x10, C_PLDIR, 0);
    off += put_lfn(d + off, g_long_name,      "LONGAL~1   ", 0x10, C_LONG, 0);
    off += put_lfn(d + off, "Loose.flac",     "LOOSE   FLA", 0x20, C_LOOSE, 300);

    d = clus_ptr(C_ALBUM); off = 0;
    put_dirent(d + off, ".          ", 0x10, C_ALBUM, 0); off += 32;
    put_dirent(d + off, "..         ", 0x10, C_MUSIC, 0); off += 32;
    off += put_lfn(d + off, "01 Song.flac", "01SONG~1FLA", 0x20, C_SONG1, 1000);
    /* A 3-character extension with a long name: the saved line must carry
     * ".fla" exactly, not a normalised ".flac". */
    off += put_lfn(d + off, "02 Other.fla", "02OTHE~1FLA", 0x20, C_SONG2, 2000);
    put_dirent(d + off, "NOTES   TXT", 0x20, C_NOTES, 10); off += 32;

    d = clus_ptr(C_LONG); off = 0;
    put_dirent(d + off, ".          ", 0x10, C_LONG, 0);  off += 32;
    put_dirent(d + off, "..         ", 0x10, C_MUSIC, 0); off += 32;
    off += put_lfn(d + off, "Track.flac", "TRACK   FLA", 0x20, C_LONGTRK, 500);

    d = clus_ptr(C_PLDIR); off = 0;
    put_dirent(d + off, ".          ", 0x10, C_PLDIR, 0); off += 32;
    put_dirent(d + off, "..         ", 0x10, C_MUSIC, 0); off += 32;
    off += put_lfn(d + off, "On-The-Go 1.m3u8", "ONTHEG~1M3U", 0x20,
                   C_SLOT1, SLOT1_BYTES);
    off += put_lfn(d + off, "On-The-Go 2.m3u8", "ONTHEG~2M3U", 0x20,
                   C_SLOT2, SLOT2_BYTES);
    off += put_lfn(d + off, "On-The-Go 3.m3u8", "ONTHEG~3M3U", 0x20,
                   C_SLOT3, SLOT3_BYTES);
    off += put_lfn(d + off, "On-The-Go 4.m3u8", "ONTHEG~4M3U", 0x20,
                   C_SLOT4, 64);
    off += put_lfn(d + off, "Favourites.m3u8", "FAVOUR~1M3U", 0x20, C_FAV, 45);

    /* Slot 1: the empty form. Slot 2: three entries. Slot 3: TORN — the
     * header says gen 5 and the trailer says 4, which is what a power cut
     * before the last write leaves behind. Slot 4: a plain playlist with no
     * directive at all (a user's own file at that name). */
    {
        static uint8_t buf[16384];
        (void)build_slot_file(buf, SLOT1_BYTES, 0, 0, 0, -1, -1);
        place_file(SLOT1_CHAIN, 4, buf, SLOT1_BYTES);

        (void)build_slot_file(buf, SLOT2_BYTES, SLOT2_ENTRIES, 3, 7, -1, -1);
        for (uint32_t i = 0; i < 4; i++) {
            memcpy(clus_ptr(C_SLOT2 + i), buf + i * BPS, BPS);
        }

        (void)build_slot_file(buf, SLOT3_BYTES, SLOT3_ENTRIES, 2, 4, -1, 5);
        for (uint32_t i = 0; i < 4; i++) {
            memcpy(clus_ptr(C_SLOT3 + i), buf + i * BPS, BPS);
        }
    }
    memcpy(clus_ptr(C_SLOT4), "#EXTM3U\n/Music/Artist - Album/01 Song.flac\n", 43);
    memcpy(clus_ptr(C_FAV),   "#EXTM3U\n/Music/Artist - Album/01 Song.flac\n", 43);
}

/* ---- the block device --------------------------------------------------- */

static int mem_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if ((uint64_t)lba + count > TOT_LBA) {
        return -1;
    }
    memcpy(buf, &g_disk[lba * 512u], count * 512u);
    return 0;
}

/* The recorder. Refuses anything the driver would refuse, AND anything that
 * leaves the slot file's own clusters or crosses one — which the driver could
 * not know to refuse. */
#define WRITES_MAX 64
static struct { uint32_t lba, count; } g_w[WRITES_MAX];
static uint32_t g_writes;
static int      g_stray;
static int      g_wfail_left;
static const uint32_t *g_target_chain;
static uint32_t g_target_nclus;

static void target(const uint32_t *chain, uint32_t n)
{
    g_target_chain = chain;
    g_target_nclus = n;
}

static int mem_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (g_writes < WRITES_MAX) {
        g_w[g_writes].lba   = lba;
        g_w[g_writes].count = count;
    }
    g_writes++;
    if (count == 0 || (count % ATA_PHYS_LOG) != 0 || (lba % ATA_PHYS_LOG) != 0 ||
        ((uintptr_t)buf & 1u) != 0) {
        g_stray = 1;
        return -1;
    }
    int inside = 0;
    for (uint32_t i = 0; i < g_target_nclus; i++) {
        uint32_t base = clus_lba(g_target_chain[i]);
        if (lba >= base && lba + count <= base + BPS / 512u) {
            inside = 1;
        }
    }
    if (!inside) {
        g_stray = 1;
        return -1;
    }
    if (g_wfail_left > 0) {
        g_wfail_left--;
        return -3;
    }
    memcpy(&g_disk[lba * 512u], buf, count * 512u);
    return 0;
}

static void reset_writes(void)
{
    g_writes = 0; g_stray = 0; g_wfail_left = 0;
    memset(g_w, 0, sizeof g_w);
}

/* Read a file back off the RAM disk, following the chain. */
static void read_file(const uint32_t *chain, uint32_t nclus, uint8_t *out,
                      uint32_t n)
{
    uint32_t done = 0;
    for (uint32_t i = 0; i < nclus && done < n; i++) {
        uint32_t take = n - done < BPS ? n - done : BPS;
        memcpy(out + done, clus_ptr(chain[i]), take);
        done += take;
    }
}

/* ---- fixtures ----------------------------------------------------------- */

static fat32_t            g_fs;
static otg_save_scratch_t g_scr;
static otg_save_stats_t   g_st;
static playlist_t         g_pl[PLAYLIST_MAX];
static playlist_scratch_t g_plscr;
static playlist_track_t   g_rows[PLAYLIST_TRACKS_MAX];

static const playlist_t *find_pl(int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(g_pl[i].name, name) == 0) {
            return &g_pl[i];
        }
    }
    return 0;
}

/* ---- 1. naming ---------------------------------------------------------- */

static void test_naming(void)
{
    check("\"On-The-Go 3\" is slot 3", otg_slot_index("On-The-Go 3") == 3);
    check("\"On-The-Go 1\" is slot 1", otg_slot_index("On-The-Go 1") == 1);
    check("\"On-The-Go 5\" is the last slot",
          otg_slot_index("On-The-Go 5") == (int)OTG_SLOTS);
    check("the match is case-insensitive", otg_slot_index("on-the-go 1") == 1 &&
          otg_slot_index("ON-THE-GO 2") == 2);
    check("\"On-The-Go 6\" is not a slot", otg_slot_index("On-The-Go 6") == 0);
    check("\"On-The-Go 0\" is not a slot", otg_slot_index("On-The-Go 0") == 0);
    check("\"On-The-Go\" is not a slot", otg_slot_index("On-The-Go") == 0);
    check("the name is EXT-TRIMMED, so the filename is not a slot name",
          otg_slot_index("On-The-Go 1.m3u8") == 0);
    check("trailing anything is not a slot",
          otg_slot_index("On-The-Go 12") == 0 &&
          otg_slot_index("On-The-Go 1 (old)") == 0);
    check("a shorter or different name is not a slot",
          otg_slot_index("On-The") == 0 && otg_slot_index("") == 0 &&
          otg_slot_index("Off-The-Go 1") == 0 && otg_slot_index(0) == 0);

    char buf[OTG_SLOT_NAME_BYTES];
    otg_slot_name(buf, 2);
    check("otg_slot_name writes the name back", strcmp(buf, "On-The-Go 2") == 0);
    check("...and round-trips through the index", otg_slot_index(buf) == 2);
    otg_slot_name(buf, 0);
    check("an out-of-range slot names nothing", buf[0] == '\0');
    otg_slot_name(buf, (int)OTG_SLOTS + 1);
    check("...at either end", buf[0] == '\0');
}

/* ---- 2. probe ----------------------------------------------------------- */

static void test_probe(void)
{
    otg_slot_info_t info;

    check("an EMPTY slot probes as present with no entries",
          otg_slot_probe(&g_fs, C_SLOT1, SLOT1_BYTES, &info) == 0 &&
          info.present && info.count == 0 && info.gen == 0 && info.crc == 0);

    check("a USED slot probes as present with its count and gen",
          otg_slot_probe(&g_fs, C_SLOT2, SLOT2_BYTES, &info) == 0 &&
          info.present && info.count == 3 && info.gen == 7 && info.crc != 0);

    check("a TORN slot still probes (the header is intact)",
          otg_slot_probe(&g_fs, C_SLOT3, SLOT3_BYTES, &info) == 0 &&
          info.present && info.gen == 5);

    check("a FOREIGN playlist at a slot name is not a slot file",
          otg_slot_probe(&g_fs, C_SLOT4, 64, &info) == 0 && !info.present);

    check("a file too small to hold a header is not a slot file",
          otg_slot_probe(&g_fs, C_FAV, 8, &info) == 0 && !info.present);

    check("null arguments are refused",
          otg_slot_probe(0, C_SLOT1, SLOT1_BYTES, &info) < 0 &&
          otg_slot_probe(&g_fs, C_SLOT1, SLOT1_BYTES, 0) < 0);

    /* A directive on line 1, with no #EXTM3U before it, still parses — the
     * scan is line-oriented, not offset-based. */
    {
        static uint8_t buf[BPS];
        memset(buf, '\n', sizeof buf);
        memcpy(buf, "#CORE-OTG v1 count=00004 crc=DEADBEEF gen=00009\n",
               OTG_HDR_LINE_BYTES);
        memcpy(clus_ptr(C_SLOT4), buf, BPS);
        fat32_cache_drop(&g_fs);
        check("a directive on line 1 parses",
              otg_slot_probe(&g_fs, C_SLOT4, BPS, &info) == 0 && info.present &&
              info.count == 4 && info.crc == 0xDEADBEEFu && info.gen == 9);

        /* Past the first 512 bytes it is "not present": the probe reads one
         * sector and nothing more, which is what makes listing five slots
         * five reads. */
        memset(buf, '\n', sizeof buf);
        memcpy(buf + 600, "#CORE-OTG v1 count=00004 crc=DEADBEEF gen=00009\n",
               OTG_HDR_LINE_BYTES);
        memcpy(clus_ptr(C_SLOT4), buf, BPS);
        fat32_cache_drop(&g_fs);
        check("a directive past the first 512 bytes is not found",
              otg_slot_probe(&g_fs, C_SLOT4, BPS, &info) == 0 && !info.present);

        /* A mangled field is not a directive: every part is fixed width, so
         * anything else is somebody else's comment. */
        memset(buf, '\n', sizeof buf);
        memcpy(buf, "#CORE-OTG v1 count=0004 crc=DEADBEEF gen=00009\n", 46);
        memcpy(clus_ptr(C_SLOT4), buf, BPS);
        fat32_cache_drop(&g_fs);
        check("a directive of the wrong width is not one",
              otg_slot_probe(&g_fs, C_SLOT4, BPS, &info) == 0 && !info.present);

        /* count past the ceiling is not a directive either. */
        memset(buf, '\n', sizeof buf);
        memcpy(buf, "#CORE-OTG v1 count=00513 crc=00000000 gen=00000\n",
               OTG_HDR_LINE_BYTES);
        memcpy(clus_ptr(C_SLOT4), buf, BPS);
        fat32_cache_drop(&g_fs);
        check("count > OTG_MAX is not a directive",
              otg_slot_probe(&g_fs, C_SLOT4, BPS, &info) == 0 && !info.present);
    }
    build_image();                        /* put slot 4 back */
    fat32_cache_drop(&g_fs);
}

/* ---- 3. verify ---------------------------------------------------------- */

static void test_verify(void)
{
    otg_slot_info_t info;

    check("an intact used slot verifies",
          otg_slot_verify(&g_fs, C_SLOT2, SLOT2_BYTES, 3, &info) == 0 &&
          info.present && !info.damaged);

    check("an empty slot verifies",
          otg_slot_verify(&g_fs, C_SLOT1, SLOT1_BYTES, 0, &info) == 0 &&
          info.present && !info.damaged);

    check("a header gen that disagrees with the trailer is DAMAGED",
          otg_slot_verify(&g_fs, C_SLOT3, SLOT3_BYTES, 2, &info) == 0 &&
          info.present && info.damaged);

    check("a parsed line count that disagrees with the header is DAMAGED",
          otg_slot_verify(&g_fs, C_SLOT2, SLOT2_BYTES, 2, &info) == 0 &&
          info.damaged);

    check("a foreign playlist is not a slot and is never 'damaged'",
          otg_slot_verify(&g_fs, C_SLOT4, 64, 1, &info) == 0 &&
          !info.present && !info.damaged);
}

/* ---- 4. the Playlists listing ------------------------------------------- */

static void test_scan_hides_empty_slots(void)
{
    uint32_t dir = 0;
    int trunc = -1;

    int n = playlist_scan(&g_fs, C_MUSIC, g_pl, PLAYLIST_MAX, &dir, &trunc, 0);
    check("without the filter every playlist file is listed",
          n == 5 && find_pl(n, "On-The-Go 1") && find_pl(n, "On-The-Go 2") &&
          find_pl(n, "On-The-Go 3") && find_pl(n, "On-The-Go 4") &&
          find_pl(n, "Favourites"));

    n = playlist_scan(&g_fs, C_MUSIC, g_pl, PLAYLIST_MAX, &dir, &trunc, 1);
    check("the filter hides the EMPTY slot", n == 4 && !find_pl(n, "On-The-Go 1"));
    check("...and keeps the used one", find_pl(n, "On-The-Go 2") != 0);
    check("...and the damaged one (the user saved that; they must be told)",
          find_pl(n, "On-The-Go 3") != 0);
    check("...and the foreign one (that is their own playlist)",
          find_pl(n, "On-The-Go 4") != 0);
    check("...and every ordinary playlist", find_pl(n, "Favourites") != 0);
    check("the surviving order is still A-Z",
          strcmp(g_pl[0].name, "Favourites") == 0 &&
          strcmp(g_pl[1].name, "On-The-Go 2") == 0 &&
          strcmp(g_pl[2].name, "On-The-Go 3") == 0 &&
          strcmp(g_pl[3].name, "On-The-Go 4") == 0);
}

/* ---- 5. save ------------------------------------------------------------ */

/* What the save is asked to write. The middle two rows are the ones that
 * cannot be written; the last is a duplicate, which is allowed. */
static const otg_save_row_t SAVE_ROWS[6] = {
    { C_ALBUM, C_SONG1   },   /* /Music/Artist - Album/01 Song.flac */
    { C_ALBUM, C_SONG2   },   /* /Music/Artist - Album/02 Other.fla */
    { C_LONG,  C_LONGTRK },   /* the 180-char folder: over M3U_PATH_MAX */
    { C_MUSIC, C_LOOSE   },   /* /Music/Loose.flac — the library root */
    { C_ALBUM, C_SONG1   },   /* the duplicate */
    { C_ALBUM, 999       },   /* no such file in that folder */
};

static const char *SAVE_EXPECT[4] = {
    "/Music/Artist - Album/01 Song.flac",
    "/Music/Artist - Album/02 Other.fla",
    "/Music/Loose.flac",
    "/Music/Artist - Album/01 Song.flac",
};

static void test_save(void)
{
    static uint8_t want[SLOT1_BYTES];
    static uint8_t got[SLOT1_BYTES];

    /* Warm fat32.c's data-sector cache with the slot's OLD first sector. If
     * otg_slot_save() ever stops calling fat32_cache_drop(), the probe and
     * the parse below serve these stale bytes and the two checks at the end
     * fail — which is the only way this hazard is visible from the host. */
    otg_slot_info_t before;
    check("the target slot starts empty",
          otg_slot_probe(&g_fs, C_SLOT1, SLOT1_BYTES, &before) == 0 &&
          before.present && before.count == 0);

    target(SLOT1_CHAIN, 4);
    reset_writes();
    int rc = otg_slot_save(&g_fs, C_SLOT1, SLOT1_BYTES, C_MUSIC, "/Music/",
                           SAVE_ROWS, 6, 42, mem_write, &g_scr, &g_st);
    check("the save succeeds", rc == 0);
    check("no write left the file's clusters or crossed one", !g_stray);
    check("four rows were written", g_st.written == 4);
    check("the long folder and the missing file are counted unsaveable",
          g_st.unsaveable == 2);
    check("nothing was a read error, nothing was truncated",
          g_st.io_err == 0 && g_st.truncated == 0);

    uint32_t crc = build_slot_file(want, SLOT1_BYTES, SAVE_EXPECT, 4, 42, -1, -1);
    read_file(SLOT1_CHAIN, 4, got, SLOT1_BYTES);
    check("the file is EXACTLY the expected bytes, padding included",
          memcmp(got, want, SLOT1_BYTES) == 0);
    check("the header's CRC is the CRC of the entry lines", g_st.crc == crc);

    /*
     * The write ORDER is the tear-detection argument: stage 0 (the header and
     * the first lines) must be the LAST thing on the platter, so a cut before
     * it leaves an old header over a new trailer.
     *
     * 8192 bytes is two stages, each two cluster runs: stage 1's clusters
     * first, then stage 0's.
     */
    check("the save is four runs — two stages, two clusters each",
          g_writes == 4);
    check("stage 1 is written FIRST",
          g_w[0].lba == clus_lba(SLOT1_CHAIN[2]) && g_w[0].count == 4 &&
          g_w[1].lba == clus_lba(SLOT1_CHAIN[3]) && g_w[1].count == 4);
    check("...and STAGE 0 LAST, after the whole tail",
          g_w[2].lba == clus_lba(SLOT1_CHAIN[0]) && g_w[2].count == 4 &&
          g_w[3].lba == clus_lba(SLOT1_CHAIN[1]) && g_w[3].count == 4);

    /* The file the device just wrote has to read back as the playlist it is —
     * through the ordinary reader, with no special case for a slot. */
    otg_slot_info_t info;
    check("the freshly written slot probes as used",
          otg_slot_probe(&g_fs, C_SLOT1, SLOT1_BYTES, &info) == 0 &&
          info.present && info.count == 4 && info.gen == 42 && info.crc == crc);
    check("...and verifies intact",
          otg_slot_verify(&g_fs, C_SLOT1, SLOT1_BYTES, 4, &info) == 0 &&
          !info.damaged);

    uint32_t dir = 0;
    int trunc = 0;
    int n = playlist_scan(&g_fs, C_MUSIC, g_pl, PLAYLIST_MAX, &dir, &trunc, 1);
    const playlist_t *pl = find_pl(n, "On-The-Go 1");
    check("a saved slot is listed like any other playlist", pl != 0);
    if (pl) {
        playlist_stats_t st;
        int r = playlist_resolve(&g_fs, pl, "Music/Playlists", g_rows,
                                 PLAYLIST_TRACKS_MAX, &g_plscr, &st);
        check("...and resolves to the same rows, in the same order",
              r == 4 && st.listed == 4 &&
              g_rows[0].clus == C_SONG1 && g_rows[1].clus == C_SONG2 &&
              g_rows[2].clus == C_LOOSE && g_rows[3].clus == C_SONG1);
        check("...with the on-disk names the save read out of the dirents",
              r == 4 && strcmp(g_rows[1].name, "02 Other") == 0 &&
              g_rows[1].file_hash == name_hash("02 Other.fla"));
        check("...and nothing missing or unplayable",
              st.missing == 0 && st.unplayable == 0 && st.rejected == 0);
    }
}

/* ---- 6. erase, truncation and refusals ---------------------------------- */

static void test_erase_and_refusals(void)
{
    static uint8_t want[SLOT1_BYTES];
    static uint8_t got[SLOT1_BYTES];

    /* Delete Playlist: the empty form over the same file, same writer. */
    target(SLOT1_CHAIN, 4);
    reset_writes();
    check("erase succeeds",
          otg_slot_erase(&g_fs, C_SLOT1, SLOT1_BYTES, 43, mem_write,
                         &g_scr, &g_st) == 0 && g_st.written == 0);
    (void)build_slot_file(want, SLOT1_BYTES, 0, 0, 43, -1, -1);
    read_file(SLOT1_CHAIN, 4, got, SLOT1_BYTES);
    check("...and writes exactly the empty form", memcmp(got, want, SLOT1_BYTES) == 0);

    otg_slot_info_t info;
    check("an erased slot is empty again and free for the next Save",
          otg_slot_probe(&g_fs, C_SLOT1, SLOT1_BYTES, &info) == 0 &&
          info.present && info.count == 0);

    uint32_t dir = 0; int trunc = 0;
    int n = playlist_scan(&g_fs, C_MUSIC, g_pl, PLAYLIST_MAX, &dir, &trunc, 1);
    check("...and it disappears from the Playlists list again",
          find_pl(n, "On-The-Go 1") == 0);

    /* Truncation: more rows than the file holds. 8192 bytes takes 8 + 48
     * bytes of header, 24 of trailer and 35 per line, so 231 lines fit. */
    {
        static otg_save_row_t many[400];
        for (int i = 0; i < 400; i++) {
            many[i].dir_clus  = C_ALBUM;
            many[i].file_clus = C_SONG1;
        }
        target(SLOT1_CHAIN, 4);
        reset_writes();
        check("a list bigger than the file still writes a well-formed file",
              otg_slot_save(&g_fs, C_SLOT1, SLOT1_BYTES, C_MUSIC, "/Music/",
                            many, 400, 44, mem_write, &g_scr, &g_st) == 0);
        check("...and says it was truncated", g_st.truncated == 1);
        check("...having written as many as fit",
              g_st.written == (SLOT1_BYTES - 8u - OTG_HDR_LINE_BYTES -
                               OTG_END_LINE_BYTES) / 35u);
        check("...which still verifies as intact",
              otg_slot_verify(&g_fs, C_SLOT1, SLOT1_BYTES, g_st.written,
                              &info) == 0 && !info.damaged);
    }

    /* Sizes the format cannot use are refused before a byte is written. */
    reset_writes();
    check("a file smaller than the minimum is refused",
          otg_slot_save(&g_fs, C_SLOT1, OTG_SLOT_FILE_MIN - 1u, C_MUSIC,
                        "/Music/", SAVE_ROWS, 1, 1, mem_write, &g_scr, &g_st) < 0);
    check("a size that is not a multiple of the grain is refused",
          otg_slot_save(&g_fs, C_SLOT1, 5000, C_MUSIC, "/Music/",
                        SAVE_ROWS, 1, 1, mem_write, &g_scr, &g_st) < 0);
    check("a root prefix that does not end in '/' is refused",
          otg_slot_save(&g_fs, C_SLOT1, SLOT1_BYTES, C_MUSIC, "/Music",
                        SAVE_ROWS, 1, 1, mem_write, &g_scr, &g_st) < 0);
    check("a relative root prefix is refused",
          otg_slot_save(&g_fs, C_SLOT1, SLOT1_BYTES, C_MUSIC, "Music/",
                        SAVE_ROWS, 1, 1, mem_write, &g_scr, &g_st) < 0);
    check("null arguments are refused",
          otg_slot_save(0, C_SLOT1, SLOT1_BYTES, C_MUSIC, "/Music/",
                        SAVE_ROWS, 1, 1, mem_write, &g_scr, &g_st) < 0 &&
          otg_slot_save(&g_fs, C_SLOT1, SLOT1_BYTES, C_MUSIC, "/Music/",
                        SAVE_ROWS, 1, 1, 0, &g_scr, &g_st) < 0 &&
          otg_slot_save(&g_fs, C_SLOT1, SLOT1_BYTES, C_MUSIC, "/Music/",
                        SAVE_ROWS, 1, 1, mem_write, 0, &g_st) < 0 &&
          otg_slot_save(&g_fs, C_SLOT1, SLOT1_BYTES, C_MUSIC, "/Music/",
                        SAVE_ROWS, 1, 1, mem_write, &g_scr, 0) < 0);
    check("...and nothing was written by any of them", g_writes == 0);

    /* A write that fails part-way: the call says so, and the file it leaves
     * behind is DAMAGED — which is exactly what the next reader reports. */
    target(SLOT1_CHAIN, 4);
    reset_writes();
    g_wfail_left = 1;
    check("a failing write is reported",
          otg_slot_save(&g_fs, C_SLOT1, SLOT1_BYTES, C_MUSIC, "/Music/",
                        SAVE_ROWS, 6, 45, mem_write, &g_scr, &g_st) == -3);
    check("...and it stopped there, rather than carrying on", g_writes == 1);
}

/* ---- 7. a folder that will not give up its names ------------------------ */

static void test_unnameable_folder(void)
{
    /*
     * The album directory is replaced with bytes no entry can be read out of.
     * Nothing about that turns a row into "the user did not add this" — the
     * whole point of the stats is that a row which cannot be WRITTEN is
     * counted, never silently dropped, so the banner can say "Saved 1 of 6"
     * instead of the user finding out on a train.
     */
    static uint8_t keep[BPS];
    memcpy(keep, clus_ptr(C_ALBUM), BPS);
    memset(clus_ptr(C_ALBUM), 0xFF, BPS);
    fat32_cache_drop(&g_fs);

    target(SLOT1_CHAIN, 4);
    reset_writes();
    int rc = otg_slot_save(&g_fs, C_SLOT1, SLOT1_BYTES, C_MUSIC, "/Music/",
                           SAVE_ROWS, 6, 46, mem_write, &g_scr, &g_st);
    check("a save over an unnameable album still writes a well-formed file",
          rc == 0 && !g_stray);
    check("...with only the row it could name — Loose.flac, in Music/",
          g_st.written == 1);
    check("...and every other row counted, not silently dropped",
          g_st.unsaveable + g_st.io_err == 5);

    otg_slot_info_t info;
    check("...and the file it left behind is intact, not damaged",
          otg_slot_verify(&g_fs, C_SLOT1, SLOT1_BYTES, 1, &info) == 0 &&
          info.present && !info.damaged && info.count == 1);

    memcpy(clus_ptr(C_ALBUM), keep, BPS);
    fat32_cache_drop(&g_fs);
}

int main(void)
{
    build_image();
    check("mount", fat32_mount(&g_fs, mem_read, 0, PART_LBA) == 0);

    /* The one instance lives in kernel/main.c's .bss; keep its size honest. */
    printf("sizeof(otg_save_scratch_t) = %u\n", (unsigned)sizeof(otg_save_scratch_t));
    check("otg_save_scratch_t stays under 12 KB",
          sizeof(otg_save_scratch_t) <= 12u * 1024u);
    check("the write quantum is the drive's physical sector",
          OTG_SAVE_STAGE % (ATA_PHYS_LOG * ATA_SECTOR_SZ) == 0 &&
          OTG_SLOT_SIZE_GRAIN % (ATA_PHYS_LOG * ATA_SECTOR_SZ) == 0);
    check("the header and trailer widths are what the format says",
          OTG_HDR_LINE_BYTES == 48 && OTG_END_LINE_BYTES == 24);
    check("the host-created size holds 512 worst-case lines",
          OTG_SLOT_FILE_BYTES >= 8u + OTG_HDR_LINE_BYTES + OTG_END_LINE_BYTES +
                                 OTG_MAX * (M3U_PATH_MAX + 2u));

    test_naming();
    test_probe();
    test_verify();
    test_scan_hides_empty_slots();
    test_save();
    test_erase_and_refusals();
    test_unnameable_folder();

    printf("otg_slot_test: %s (%d failure%s)\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
