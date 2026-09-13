/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/library/playlist_test.c — the playlist read path (library/playlist.c)
 * on the host, against a hand-built in-RAM FAT32 volume laid out the way a
 * device is:
 *
 *   /Root.flac                            a loose track at the volume root
 *   /Music/CORELIB.IDX
 *   /Music/Artist - Album/01 Song.flac    clus 6
 *   /Music/Artist - Album/02 Other.flac   clus 7
 *   /Music/Artist - Album/folder.art
 *   /Music/Artist - Album/notes.txt       not audio
 *   /Music/Playlists/Favourites.m3u8      every entry shape: relative,
 *                                         absolute, backslashed, missing, a
 *                                         non-audio file, a folder, a root
 *                                         file, a duplicate
 *   /Music/Playlists/road trip.m3u        the .m3u spelling
 *   /Music/Playlists/README.TXT           not a playlist
 *   /Music/Playlists/Empty.m3u8           size 0
 *   /Music/Playlists/Sub/                 a folder: ignored
 *   /Music/Playlists/Bad.m3u8             only missing entries
 *   /Music/Playlists/Big.m3u8             130 entries, over the row cap
 *
 * Long names are real VFAT runs (checksum-bound, 0xFFFF padded) so the
 * resolve goes through the same LFN decode a device does. BytesPerSector
 * 512 with one reserved sector and a one-sector FAT, so cluster N is sector
 * N and the layout above is the sector map.
 *
 * What is asserted is what the UI relies on: the list is the .m3u8/.m3u
 * files and nothing else, A-Z, named by filename minus extension; a row
 * carries the on-disk stem exactly as a browse row would, the folder it was
 * found in, and the full-name locator hash; every way an entry fails to
 * resolve is a COUNT, and the rows that do resolve are still returned; a
 * read error on the playlist file itself is the one thing that is an error.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

#define BPS   512u
#define NSEC  32u
static uint8_t g_mem[NSEC * BPS];

/* Sector / cluster map (cluster N == sector N). */
enum {
    S_BOOT = 0, S_FAT = 1, C_ROOT = 2, C_MUSIC = 3, C_ALBUM = 4, C_PLDIR = 5,
    C_SONG1 = 6, C_SONG2 = 7, C_ART = 8, C_NOTES = 9,
    C_FAV = 10, C_ROAD = 11, C_README = 12, C_BAD = 14, C_ROOTFLAC = 15,
    C_BIG = 16, C_BIG_END = 27, C_SUB = 28, C_PLDIR2 = 29
};

#define NO_FAIL 0xFFFFFFFFu
static uint32_t g_fail_lba = NO_FAIL;

static int mem_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    if ((lba + count) * BPS > sizeof g_mem) {
        return -1;
    }
    if (g_fail_lba != NO_FAIL && lba <= g_fail_lba && g_fail_lba < lba + count) {
        return -1;
    }
    memcpy(buf, &g_mem[lba * BPS], count * BPS);
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_dirent(uint8_t *e, const char *raw11, uint8_t attr,
                       uint32_t clus, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, raw11, 11);
    e[11] = attr;
    put16(&e[20], (uint16_t)(clus >> 16));
    put16(&e[26], (uint16_t)(clus & 0xFFFF));
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

/* An LFN run (physically highest sequence first, 0x40 on it) followed by
 * its 8.3 entry. ASCII names only; returns the bytes written. */
static int put_lfn(uint8_t *e, const char *longname, const char *raw11,
                   uint8_t attr, uint32_t clus, uint32_t size)
{
    static const uint8_t pos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    uint16_t units[13 * 20];
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

static uint32_t put_text(uint32_t clus, const char *text)
{
    uint32_t n = (uint32_t)strlen(text);
    memcpy(&g_mem[clus * BPS], text, n);
    return n;
}

static const char FAV_TEXT[] =
    "\xEF\xBB\xBF#EXTM3U\r\n"
    "#EXTINF:200,Song One\r\n"
    "../Artist - Album/01 Song.flac\r\n"
    "/Music/Artist - Album/02 Other.flac\r\n"
    "\\Music\\Artist - Album\\Missing.flac\r\n"
    "../Artist - Album/notes.txt\r\n"
    "../Artist - Album\r\n"
    "/Root.flac\r\n"
    "../Artist - Album/01 Song.flac\r\n";

static const char ROAD_TEXT[] =
    "#EXTM3U\n"
    "../Artist - Album/02 Other.flac\n"
    "/Root.flac\n";

static const char BAD_TEXT[] =
    "/Nowhere/x.flac\n"
    "../gone.flac\n";

#define BIG_LINE  "/Music/Artist - Album/01 Song.flac\n"
#define BIG_LINES 130

static uint32_t g_big_size;

static void build_image(void)
{
    memset(g_mem, 0, sizeof g_mem);

    uint8_t *bs = g_mem;
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(&bs[3], "MSDOS5.0", 8);
    put16(&bs[11], BPS);
    bs[13] = 1;                  /* SecPerClus  */
    put16(&bs[14], 1);           /* RsvdSecCnt  */
    bs[16] = 1;                  /* NumFATs     */
    bs[21] = 0xF8;
    put32(&bs[36], 1);           /* FATSz32     */
    put32(&bs[44], C_ROOT);      /* RootClus    */
    bs[510] = 0x55; bs[511] = 0xAA;

    uint8_t *fat = &g_mem[S_FAT * BPS];
    put32(&fat[0], 0x0FFFFFF8u);
    put32(&fat[4], 0x0FFFFFFFu);
    for (uint32_t c = 2; c < NSEC; c++) {
        put32(&fat[c * 4], 0x0FFFFFFFu);        /* every cluster: EOC */
    }
    for (uint32_t c = C_BIG; c < C_BIG_END; c++) {
        put32(&fat[c * 4], c + 1);              /* Big.m3u8's chain */
    }
    put32(&fat[C_PLDIR * 4], C_PLDIR2);         /* Playlists/ spans two clusters:
                                                 * 17 entries do not fit in 16 */

    uint8_t *d;
    int off;

    /* Root: Music/ and a loose track. */
    d = &g_mem[C_ROOT * BPS]; off = 0;
    off += put_lfn(d + off, "Music",     "MUSIC      ", 0x10, C_MUSIC, 0);
    off += put_lfn(d + off, "Root.flac", "ROOT    FLA", 0x20, C_ROOTFLAC, 700);

    /* Music/: the album, the playlists folder, the index. */
    d = &g_mem[C_MUSIC * BPS]; off = 0;
    put_dirent(d + off, ".          ", 0x10, C_MUSIC, 0); off += 32;
    put_dirent(d + off, "..         ", 0x10, C_ROOT, 0);  off += 32;
    off += put_lfn(d + off, "Artist - Album", "ARTIST~1   ", 0x10, C_ALBUM, 0);
    off += put_lfn(d + off, "Playlists",      "PLAYLI~1   ", 0x10, C_PLDIR, 0);
    put_dirent(d + off, "CORELIB IDX", 0x20, 0, 0);       off += 32;

    /* The album. */
    d = &g_mem[C_ALBUM * BPS]; off = 0;
    put_dirent(d + off, ".          ", 0x10, C_ALBUM, 0); off += 32;
    put_dirent(d + off, "..         ", 0x10, C_MUSIC, 0); off += 32;
    off += put_lfn(d + off, "01 Song.flac",  "01SONG~1FLA", 0x20, C_SONG1, 1000);
    off += put_lfn(d + off, "02 Other.flac", "02OTHE~1FLA", 0x20, C_SONG2, 2000);
    off += put_lfn(d + off, "folder.art",    "FOLDER  ART", 0x20, C_ART, 100);
    put_dirent(d + off, "NOTES   TXT", 0x20, C_NOTES, 10); off += 32;

    /* The playlists folder: 17 entries, so it is built in a 1 KB staging
     * buffer and split over its two-cluster chain (C_PLDIR -> C_PLDIR2). */
    static uint8_t pldir[2 * BPS];
    memset(pldir, 0, sizeof pldir);
    d = pldir; off = 0;
    put_dirent(d + off, ".          ", 0x10, C_PLDIR, 0); off += 32;
    put_dirent(d + off, "..         ", 0x10, C_MUSIC, 0); off += 32;
    off += put_lfn(d + off, "Favourites.m3u8", "FAVOUR~1M3U", 0x20, C_FAV,
                   (uint32_t)(sizeof FAV_TEXT - 1));
    off += put_lfn(d + off, "road trip.m3u",   "ROADTR~1M3U", 0x20, C_ROAD,
                   (uint32_t)(sizeof ROAD_TEXT - 1));
    put_dirent(d + off, "README  TXT", 0x20, C_README, 20); off += 32;
    off += put_lfn(d + off, "Empty.m3u8", "EMPTY   M3U", 0x20, 0, 0);
    off += put_lfn(d + off, "Sub",        "SUB        ", 0x10, C_SUB, 0);
    off += put_lfn(d + off, "Bad.m3u8",   "BAD     M3U", 0x20, C_BAD,
                   (uint32_t)(sizeof BAD_TEXT - 1));
    g_big_size = (uint32_t)strlen(BIG_LINE) * BIG_LINES;
    off += put_lfn(d + off, "Big.m3u8",   "BIG     M3U", 0x20, C_BIG, g_big_size);
    if (off > (int)sizeof pldir) {
        printf("fixture: Playlists/ overflowed its staging buffer\n");
        g_fails++;
    }
    memcpy(&g_mem[C_PLDIR  * BPS], pldir,       BPS);
    memcpy(&g_mem[C_PLDIR2 * BPS], pldir + BPS, BPS);

    /* Sub/: empty but for its dot entries. */
    d = &g_mem[C_SUB * BPS]; off = 0;
    put_dirent(d + off, ".          ", 0x10, C_SUB, 0);   off += 32;
    put_dirent(d + off, "..         ", 0x10, C_PLDIR, 0); off += 32;

    /* File bodies. */
    put_text(C_FAV,  FAV_TEXT);
    put_text(C_ROAD, ROAD_TEXT);
    put_text(C_BAD,  BAD_TEXT);
    {
        uint32_t p = C_BIG * BPS;
        for (int i = 0; i < BIG_LINES; i++) {
            memcpy(&g_mem[p], BIG_LINE, strlen(BIG_LINE));
            p += (uint32_t)strlen(BIG_LINE);
        }
    }
}

/* ---- fixtures ----------------------------------------------------------- */

static fat32_t            g_fs;
static playlist_t         g_pl[PLAYLIST_MAX];
static playlist_track_t   g_rows[PLAYLIST_TRACKS_MAX];
static playlist_scratch_t g_scr;

static const playlist_t *find_pl(int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(g_pl[i].name, name) == 0) {
            return &g_pl[i];
        }
    }
    return 0;
}

/* ---- 1. the listing ----------------------------------------------------- */

static void test_scan(void)
{
    uint32_t dir = 0;
    int trunc = -1;
    int n = playlist_scan(&g_fs, C_MUSIC, g_pl, PLAYLIST_MAX, &dir, &trunc);

    check("scan finds the Playlists folder under the library root",
          dir == C_PLDIR);
    check("scan lists the five playlist files and nothing else", n == 5);
    check("scan is not truncated at the cap", trunc == 0);
    check("scan sorts A-Z, case-insensitively, by the ext-trimmed name",
          n == 5 &&
          strcmp(g_pl[0].name, "Bad") == 0 &&
          strcmp(g_pl[1].name, "Big") == 0 &&
          strcmp(g_pl[2].name, "Empty") == 0 &&
          strcmp(g_pl[3].name, "Favourites") == 0 &&
          strcmp(g_pl[4].name, "road trip") == 0);
    const playlist_t *fav = find_pl(n, "Favourites");
    check("a playlist carries its file's cluster and size",
          fav && fav->clus == C_FAV && fav->size == sizeof FAV_TEXT - 1);
    check("a playlist's hash is name_hash of its display name",
          fav && fav->hash == name_hash("Favourites") &&
          find_pl(n, "road trip") &&
          find_pl(n, "road trip")->hash == name_hash("road trip"));
    check("the .m3u spelling is a playlist too",
          find_pl(n, "road trip") && find_pl(n, "road trip")->clus == C_ROAD);
    check("README.TXT and the Sub folder are not playlists",
          !find_pl(n, "README") && !find_pl(n, "Sub"));
    check("an empty playlist file is still listed",
          find_pl(n, "Empty") && find_pl(n, "Empty")->size == 0);

    /* The cap: the first `max` in directory order are kept and the
     * overflow is reported, then those are sorted. Directory order is
     * Favourites, road trip, Empty, Bad, Big. */
    dir = 0; trunc = -1;
    n = playlist_scan(&g_fs, C_MUSIC, g_pl, 3, &dir, &trunc);
    check("scan at a cap of 3 returns 3 and reports the overflow",
          n == 3 && trunc == 1);
    check("...the first three in directory order, sorted",
          n == 3 &&
          strcmp(g_pl[0].name, "Empty") == 0 &&
          strcmp(g_pl[1].name, "Favourites") == 0 &&
          strcmp(g_pl[2].name, "road trip") == 0);
    n = playlist_scan(&g_fs, C_MUSIC, g_pl, 5, &dir, &trunc);
    check("scan at a cap equal to the count is not truncated",
          n == 5 && trunc == 0);

    /* No Playlists folder (the album has none): zero, not an error. */
    dir = 99; trunc = -1;
    n = playlist_scan(&g_fs, C_ALBUM, g_pl, PLAYLIST_MAX, &dir, &trunc);
    check("a library root with no Playlists folder lists nothing, no error",
          n == 0 && dir == 0 && trunc == 0);

    /* A root that cannot be read is an error, not an empty list. */
    g_fail_lba = C_MUSIC;
    n = playlist_scan(&g_fs, C_MUSIC, g_pl, PLAYLIST_MAX, &dir, &trunc);
    check("an unreadable library root is FAT32_EIO", n == FAT32_EIO);
    g_fail_lba = C_PLDIR;
    n = playlist_scan(&g_fs, C_MUSIC, g_pl, PLAYLIST_MAX, &dir, &trunc);
    check("an unreadable Playlists folder is FAT32_EIO", n == FAT32_EIO);
    g_fail_lba = NO_FAIL;

    check("null arguments are refused",
          playlist_scan(0, C_MUSIC, g_pl, 1, &dir, &trunc) == FAT32_EINVAL &&
          playlist_scan(&g_fs, C_MUSIC, 0, 1, &dir, &trunc) == FAT32_EINVAL &&
          playlist_scan(&g_fs, C_MUSIC, g_pl, 1, 0, &trunc) == FAT32_EINVAL &&
          playlist_scan(&g_fs, C_MUSIC, g_pl, 1, &dir, 0) == FAT32_EINVAL);
}

/* ---- 2. resolving a playlist into rows ---------------------------------- */

static void test_resolve(void)
{
    uint32_t dir; int trunc;
    int n = playlist_scan(&g_fs, C_MUSIC, g_pl, PLAYLIST_MAX, &dir, &trunc);
    const playlist_t *fav = find_pl(n, "Favourites");
    playlist_stats_t st;

    memset(g_rows, 0xA5, sizeof g_rows);
    int r = playlist_resolve(&g_fs, fav, "Music/Playlists",
                             g_rows, PLAYLIST_TRACKS_MAX, &g_scr, &st);
    check("Favourites resolves to its four playable rows", r == 4);
    check("the parser saw every entry line", st.listed == 7);
    check("a relative entry resolves through the playlist's folder",
          r >= 1 && g_rows[0].clus == C_SONG1 && g_rows[0].size == 1000 &&
          g_rows[0].dir_clus == C_ALBUM && g_rows[0].fmt == 0);
    check("a row is named exactly as a browse row names the file",
          r >= 1 && strcmp(g_rows[0].name, "01 Song") == 0);
    {
        char want[NAME_MAX + 1];
        copy_display_name(want, "01 Song.flac", 1);
        check("...byte for byte (copy_display_name, ext trimmed)",
              r >= 1 && strcmp(g_rows[0].name, want) == 0);
    }
    check("a row's locator is name_hash of the FULL on-disk name",
          r >= 1 && g_rows[0].file_hash == name_hash("01 Song.flac"));
    check("an absolute entry resolves from the volume root",
          r >= 2 && g_rows[1].clus == C_SONG2 && g_rows[1].dir_clus == C_ALBUM &&
          strcmp(g_rows[1].name, "02 Other") == 0);
    check("a root-level file resolves with the root as its folder",
          r >= 3 && g_rows[2].clus == C_ROOTFLAC && g_rows[2].dir_clus == C_ROOT &&
          strcmp(g_rows[2].name, "Root") == 0);
    check("a duplicate entry is a second row (a playlist may repeat)",
          r == 4 && g_rows[3].clus == C_SONG1);
    check("the missing (backslashed) entry is counted, not fatal",
          st.missing == 1);
    check("the non-audio file and the folder are counted as unplayable",
          st.unplayable == 2);
    check("no read errors, not truncated",
          st.io_err == 0 && st.truncated == 0);
    check("the parser's own report rides along (BOM + #EXTM3U seen)",
          st.m3u.had_bom == 1 && st.m3u.had_extm3u == 1);

    /* The row cap on the caller's side. */
    r = playlist_resolve(&g_fs, fav, "Music/Playlists", g_rows, 2, &g_scr, &st);
    check("a row array smaller than the playlist is filled and reported",
          r == 2 && st.truncated == 1 && g_rows[1].clus == C_SONG2);
    r = playlist_resolve(&g_fs, fav, "Music/Playlists", 0, 0, &g_scr, &st);
    check("a zero-row call is a counting pass",
          r == 0 && st.listed == 7 && st.truncated == 1);

    /* The .m3u one, with LF line ends and no BOM. */
    r = playlist_resolve(&g_fs, find_pl(n, "road trip"), "Music/Playlists",
                         g_rows, PLAYLIST_TRACKS_MAX, &g_scr, &st);
    check("road trip.m3u resolves both rows",
          r == 2 && g_rows[0].clus == C_SONG2 && g_rows[1].clus == C_ROOTFLAC);

    /* Empty file: nothing, no error. */
    r = playlist_resolve(&g_fs, find_pl(n, "Empty"), "Music/Playlists",
                         g_rows, PLAYLIST_TRACKS_MAX, &g_scr, &st);
    check("an empty playlist is zero rows, not an error",
          r == 0 && st.listed == 0 && st.missing == 0);

    /* Every entry missing: zero rows, all counted. */
    r = playlist_resolve(&g_fs, find_pl(n, "Bad"), "Music/Playlists",
                         g_rows, PLAYLIST_TRACKS_MAX, &g_scr, &st);
    check("a playlist of missing tracks is zero rows with the count",
          r == 0 && st.listed == 2 && st.missing == 2);

    /* Over the row cap: the first PLAYLIST_TRACKS_MAX, and truncated. */
    r = playlist_resolve(&g_fs, find_pl(n, "Big"), "Music/Playlists",
                         g_rows, PLAYLIST_TRACKS_MAX, &g_scr, &st);
    check("a 130-entry playlist yields the first 128 rows",
          r == PLAYLIST_TRACKS_MAX && g_rows[127].clus == C_SONG1);
    check("...and reports the truncation from the parser",
          st.truncated == 1 && st.m3u.total == BIG_LINES);

    /* The wrong base directory: the four relative entries (and the folder
     * among them) miss, the two good absolute ones hit, the bad absolute
     * one still misses. */
    r = playlist_resolve(&g_fs, fav, "Music", g_rows, PLAYLIST_TRACKS_MAX,
                         &g_scr, &st);
    check("with the wrong base dir only the absolute entries resolve",
          r == 2 && g_rows[0].clus == C_SONG2 && g_rows[1].clus == C_ROOTFLAC &&
          st.missing == 5 && st.unplayable == 0);
}

/* ---- 3. the disk failing under the resolve ------------------------------ */

static void test_errors(void)
{
    uint32_t dir; int trunc;
    int n = playlist_scan(&g_fs, C_MUSIC, g_pl, PLAYLIST_MAX, &dir, &trunc);
    const playlist_t *fav = find_pl(n, "Favourites");
    playlist_stats_t st;

    /* The album folder unreadable: its entries are read errors (not
     * "missing" — the file may well be there), the root file still plays. */
    g_fail_lba = C_ALBUM;
    int r = playlist_resolve(&g_fs, fav, "Music/Playlists",
                             g_rows, PLAYLIST_TRACKS_MAX, &g_scr, &st);
    check("an unreadable album folder: the other rows still resolve",
          r == 1 && g_rows[0].clus == C_ROOTFLAC);
    check("...its entries are counted as read errors, not as missing",
          st.io_err == 5 && st.missing == 0 && st.unplayable == 1);
    g_fail_lba = NO_FAIL;

    /* The playlist file itself unreadable: THAT is an error. Read another
     * playlist first so fat32.c's one-sector data cache no longer holds
     * Favourites' body — the fault has to reach the block callback. */
    (void)playlist_resolve(&g_fs, find_pl(n, "road trip"), "Music/Playlists",
                           g_rows, PLAYLIST_TRACKS_MAX, &g_scr, &st);
    g_fail_lba = C_FAV;
    r = playlist_resolve(&g_fs, fav, "Music/Playlists",
                         g_rows, PLAYLIST_TRACKS_MAX, &g_scr, &st);
    check("an unreadable playlist file is a negative return", r < 0);
    g_fail_lba = NO_FAIL;

    check("null arguments are refused, stats still written",
          playlist_resolve(0, fav, "Music/Playlists", g_rows, 1, &g_scr, &st) == M3U_EINVAL &&
          st.listed == 0 &&
          playlist_resolve(&g_fs, 0, "Music/Playlists", g_rows, 1, &g_scr, &st) == M3U_EINVAL &&
          playlist_resolve(&g_fs, fav, "Music/Playlists", g_rows, 1, 0, &st) == M3U_EINVAL &&
          playlist_resolve(&g_fs, fav, "Music/Playlists", g_rows, 1, &g_scr, 0) == M3U_EINVAL);
}

int main(void)
{
    build_image();
    check("mount", fat32_mount(&g_fs, mem_read, 0, 0) == 0);

    /* The scratch is one static in main.c; keep its size honest. */
    check("playlist_scratch_t stays under 36 KB",
          sizeof(playlist_scratch_t) <= 36u * 1024u);
    printf("sizeof(playlist_scratch_t) = %u\n", (unsigned)sizeof(playlist_scratch_t));

    test_scan();
    test_resolve();
    test_errors();

    printf("playlist_test: %s (%d failure%s)\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
