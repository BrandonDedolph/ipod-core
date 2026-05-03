/*
 * core/tests/tcdb_reader_test.c — exercise tagcache_library_load_tcdb
 * against a hand-rolled .tcdb byte string.
 *
 * The byte string is built in memory using the same format constants
 * the parser reads (core/apps/db/tagcache_format.h), so a layout drift
 * between this fixture and the parser surfaces immediately. The Go
 * encoder + this C decoder are independent implementations of the same
 * spec; the round-trip Go test in core/cli/internal/tagcache covers the
 * Go side, this covers the C side.
 *
 * Cases:
 *   1. Happy path: 3 songs, 2 artists, 2 albums, 1 genre, 1 composer,
 *      one song with no tags (idx = -1 for all four dimensions), one
 *      song with embedded art. Verify every public accessor returns
 *      the right thing.
 *   2. Bad magic — must return -1, library state untouched.
 *   3. Wrong version — must return -1.
 *   4. Truncated header — must return -1.
 */

#include "../apps/db/tagcache.h"
#include "../apps/db/tagcache_format.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FAIL(fmt, ...) \
    do { fprintf(stderr, "[tcdb_reader_test:%d] FAIL: " fmt "\n", __LINE__, ##__VA_ARGS__); \
         return 1; } while (0)
#define EXPECT_STREQ(a, b) \
    do { if (!(a) || strcmp((a), (b)) != 0) FAIL("want \"%s\" got \"%s\"", b, (a) ? (a) : "(null)"); } while (0)
#define EXPECT_EQ(a, b) \
    do { if ((a) != (b)) FAIL("want %ld got %ld", (long)(b), (long)(a)); } while (0)

/* ---------- Bytewise builders ----------------------------------- */
/* The encoder uses Go's encoding/binary (LE, no padding). We mirror
 * the field order verbatim in a uint8_t buffer. */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v); put_u32(p + 4, (uint32_t)(v >> 32));
}
static void put_i32(uint8_t *p, int32_t v) { put_u32(p, (uint32_t)v); }

/* Tiny dynamic byte buffer for laying out the fixture bytes. */
typedef struct { uint8_t *data; size_t len, cap; } buf_t;
static void buf_grow(buf_t *b, size_t need) {
    if (b->len + need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need) cap *= 2;
    b->data = (uint8_t *)realloc(b->data, cap);
    b->cap = cap;
}
static size_t buf_emit(buf_t *b, const void *src, size_t n) {
    buf_grow(b, n);
    size_t at = b->len;
    memcpy(b->data + at, src, n);
    b->len += n;
    return at;
}
static size_t buf_emit_u32(buf_t *b, uint32_t v) {
    uint8_t tmp[4]; put_u32(tmp, v); return buf_emit(b, tmp, 4);
}
static size_t buf_emit_str(buf_t *b, const char *s) {
    /* String at offset 0 of the strings table is "" by convention; the
     * parser doesn't enforce that, but it's the spec.  Caller arranges
     * to call this in the right order. */
    return buf_emit(b, s, strlen(s) + 1);
}

/* ---------- Happy-path fixture ---------------------------------- */

static int build_happy_fixture(buf_t *out) {
    /* Songs (post title-sort):
     *   [0] "Apple"   path "/m/a.flac"  artist=0 (X) album=0 (P) genre=0 (Q) composer=0 (Z)  art=jpegA (5 bytes)
     *   [1] "Banana"  path "/m/b.flac"  artist=1 (Y) album=1 (R) genre=0 (Q) composer=0 (Z)  no art
     *   [2] "Cherry"  path "/m/c.flac"  no tags                                                no art
     */
    const char *jpegA = "\xff\xd8\xff\xe0\x00";   /* 5 bytes */
    size_t jpegA_len = 5;

    /* String table: build offsets while emitting. */
    buf_t strings = {0};
    uint32_t S_empty   = (uint32_t)buf_emit_str(&strings, "");
    uint32_t S_apple   = (uint32_t)buf_emit_str(&strings, "Apple");
    uint32_t S_banana  = (uint32_t)buf_emit_str(&strings, "Banana");
    uint32_t S_cherry  = (uint32_t)buf_emit_str(&strings, "Cherry");
    uint32_t S_pa      = (uint32_t)buf_emit_str(&strings, "/m/a.flac");
    uint32_t S_pb      = (uint32_t)buf_emit_str(&strings, "/m/b.flac");
    uint32_t S_pc      = (uint32_t)buf_emit_str(&strings, "/m/c.flac");
    uint32_t S_X       = (uint32_t)buf_emit_str(&strings, "X");
    uint32_t S_Y       = (uint32_t)buf_emit_str(&strings, "Y");
    uint32_t S_P       = (uint32_t)buf_emit_str(&strings, "P");
    uint32_t S_R       = (uint32_t)buf_emit_str(&strings, "R");
    uint32_t S_Q       = (uint32_t)buf_emit_str(&strings, "Q");
    uint32_t S_Z       = (uint32_t)buf_emit_str(&strings, "Z");
    (void)S_empty;

    uint32_t song_count = 3;
    uint32_t n_artists  = 2;   /* X, Y */
    uint32_t n_albums   = 2;   /* P, R */
    uint32_t n_genres   = 1;   /* Q */
    uint32_t n_composers= 1;   /* Z */

    /* Section offsets — same order Go's encoder uses. */
    uint64_t songs_off          = TCDB_HEADER_SIZE;
    uint64_t songs_len          = (uint64_t)song_count * TCDB_SONG_RECORD_SIZE;
    uint64_t artist_idx_off     = songs_off + songs_len;
    uint64_t album_idx_off      = artist_idx_off   + (uint64_t)n_artists   * 4;
    uint64_t genre_idx_off      = album_idx_off    + (uint64_t)n_albums    * 4;
    uint64_t composer_idx_off   = genre_idx_off    + (uint64_t)n_genres    * 4;
    uint64_t uniq_end           = composer_idx_off + (uint64_t)n_composers * 4;

    /* Build the four group blocks now so we know their sizes. Each:
     *   N u32 offsets (relative to block start), then per group
     *   [u32 count, count*u32 indices]. */
    buf_t artist_block = {0};
    {
        /* Two artists (X, Y); X has [0], Y has [1]. */
        uint32_t header_size = n_artists * 4;
        uint32_t off_X = header_size;
        uint32_t off_Y = header_size + 4 + 1*4;     /* X's body: 4 + 1*4 */
        buf_emit_u32(&artist_block, off_X);
        buf_emit_u32(&artist_block, off_Y);
        buf_emit_u32(&artist_block, 1);
        buf_emit_u32(&artist_block, 0);   /* song idx 0 */
        buf_emit_u32(&artist_block, 1);
        buf_emit_u32(&artist_block, 1);   /* song idx 1 */
    }
    buf_t album_block = {0};
    {
        uint32_t header_size = n_albums * 4;
        uint32_t off_P = header_size;
        uint32_t off_R = header_size + 4 + 1*4;
        buf_emit_u32(&album_block, off_P);
        buf_emit_u32(&album_block, off_R);
        buf_emit_u32(&album_block, 1);
        buf_emit_u32(&album_block, 0);
        buf_emit_u32(&album_block, 1);
        buf_emit_u32(&album_block, 1);
    }
    buf_t genre_block = {0};
    {
        /* One genre (Q) with [0, 1]. */
        uint32_t header_size = n_genres * 4;
        uint32_t off_Q = header_size;
        buf_emit_u32(&genre_block, off_Q);
        buf_emit_u32(&genre_block, 2);
        buf_emit_u32(&genre_block, 0);
        buf_emit_u32(&genre_block, 1);
    }
    buf_t composer_block = {0};
    {
        /* One composer (Z) with [0, 1]. */
        uint32_t header_size = n_composers * 4;
        uint32_t off_Z = header_size;
        buf_emit_u32(&composer_block, off_Z);
        buf_emit_u32(&composer_block, 2);
        buf_emit_u32(&composer_block, 0);
        buf_emit_u32(&composer_block, 1);
    }

    uint64_t artist_groups_off   = uniq_end;
    uint64_t album_groups_off    = artist_groups_off   + artist_block.len;
    uint64_t genre_groups_off    = album_groups_off    + album_block.len;
    uint64_t composer_groups_off = genre_groups_off    + genre_block.len;
    uint64_t groups_end          = composer_groups_off + composer_block.len;

    uint64_t strings_off = groups_end;
    uint64_t art_off     = strings_off + strings.len;
    uint64_t art_len     = jpegA_len;
    uint64_t total       = art_off + art_len;

    /* Allocate the full file. */
    out->data = (uint8_t *)calloc(1, total);
    out->cap  = total;
    out->len  = total;
    if (!out->data) return -1;

    /* Header. */
    uint8_t *h = out->data;
    h[TCDB_OFF_MAGIC + 0] = TCDB_MAGIC0;
    h[TCDB_OFF_MAGIC + 1] = TCDB_MAGIC1;
    h[TCDB_OFF_MAGIC + 2] = TCDB_MAGIC2;
    h[TCDB_OFF_MAGIC + 3] = TCDB_MAGIC3;
    put_u32(h + TCDB_OFF_VERSION, TCDB_VERSION);
    put_u32(h + TCDB_OFF_SONG_COUNT, song_count);
    put_u32(h + TCDB_OFF_N_ARTISTS,  n_artists);
    put_u32(h + TCDB_OFF_N_ALBUMS,   n_albums);
    put_u32(h + TCDB_OFF_N_GENRES,   n_genres);
    put_u32(h + TCDB_OFF_N_COMPOSERS,n_composers);
    put_u64(h + TCDB_OFF_SONGS_OFF,         songs_off);
    put_u64(h + TCDB_OFF_ARTIST_IDX_OFF,    artist_idx_off);
    put_u64(h + TCDB_OFF_ALBUM_IDX_OFF,     album_idx_off);
    put_u64(h + TCDB_OFF_GENRE_IDX_OFF,     genre_idx_off);
    put_u64(h + TCDB_OFF_COMPOSER_IDX_OFF,  composer_idx_off);
    put_u64(h + TCDB_OFF_ARTIST_GROUPS_OFF, artist_groups_off);
    put_u64(h + TCDB_OFF_ALBUM_GROUPS_OFF,  album_groups_off);
    put_u64(h + TCDB_OFF_GENRE_GROUPS_OFF,  genre_groups_off);
    put_u64(h + TCDB_OFF_COMPOSER_GROUPS_OFF, composer_groups_off);
    put_u64(h + TCDB_OFF_STRINGS_OFF, strings_off);
    put_u64(h + TCDB_OFF_STRINGS_LEN, strings.len);
    put_u64(h + TCDB_OFF_ART_OFF,     art_off);
    put_u64(h + TCDB_OFF_ART_LEN,     art_len);

    /* Song records. */
    uint32_t s_titles[3] = { S_apple, S_banana, S_cherry };
    uint32_t s_paths [3] = { S_pa,    S_pb,     S_pc     };
    int32_t  s_artist[3] = { 0,       1,        TCDB_MISSING_TAG };
    int32_t  s_album [3] = { 0,       1,        TCDB_MISSING_TAG };
    int32_t  s_genre [3] = { 0,       0,        TCDB_MISSING_TAG };
    int32_t  s_compsr[3] = { 0,       0,        TCDB_MISSING_TAG };
    uint64_t s_artoff[3] = { 0,       0,        0 };
    uint64_t s_artlen[3] = { jpegA_len, 0,      0 };
    for (uint32_t i = 0; i < song_count; i++) {
        uint8_t *r = out->data + songs_off + (uint64_t)i * TCDB_SONG_RECORD_SIZE;
        put_u32(r + TCDB_REC_TITLE_OFF,    s_titles[i]);
        put_u32(r + TCDB_REC_PATH_OFF,     s_paths[i]);
        put_i32(r + TCDB_REC_ARTIST_IDX,   s_artist[i]);
        put_i32(r + TCDB_REC_ALBUM_IDX,    s_album[i]);
        put_i32(r + TCDB_REC_GENRE_IDX,    s_genre[i]);
        put_i32(r + TCDB_REC_COMPOSER_IDX, s_compsr[i]);
        put_u64(r + TCDB_REC_ART_OFF,      s_artoff[i]);
        put_u64(r + TCDB_REC_ART_LEN,      s_artlen[i]);
    }

    /* Uniq tables. */
    put_u32(out->data + artist_idx_off + 0, S_X);
    put_u32(out->data + artist_idx_off + 4, S_Y);
    put_u32(out->data + album_idx_off  + 0, S_P);
    put_u32(out->data + album_idx_off  + 4, S_R);
    put_u32(out->data + genre_idx_off  + 0, S_Q);
    put_u32(out->data + composer_idx_off+0, S_Z);

    /* Group blocks. */
    memcpy(out->data + artist_groups_off,   artist_block.data,   artist_block.len);
    memcpy(out->data + album_groups_off,    album_block.data,    album_block.len);
    memcpy(out->data + genre_groups_off,    genre_block.data,    genre_block.len);
    memcpy(out->data + composer_groups_off, composer_block.data, composer_block.len);

    /* Strings. */
    memcpy(out->data + strings_off, strings.data, strings.len);

    /* Art. */
    memcpy(out->data + art_off, jpegA, jpegA_len);

    free(strings.data);
    free(artist_block.data);
    free(album_block.data);
    free(genre_block.data);
    free(composer_block.data);
    return 0;
}

/* Write `buf` to a temp file and return its path (caller frees). */
static char *write_temp_file(const buf_t *buf) {
    char *path = strdup("/tmp/tcdb_reader_test.XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); free(path); return NULL; }
    /* Use fdopen for portable buffered writes. */
    FILE *fp = fdopen(fd, "wb");
    if (!fp) { close(fd); unlink(path); free(path); return NULL; }
    if (fwrite(buf->data, 1, buf->len, fp) != buf->len) {
        fclose(fp); unlink(path); free(path); return NULL;
    }
    fclose(fp);
    return path;
}

/* ---------- Cases ----------------------------------------------- */

static int test_happy(void) {
    buf_t buf = {0};
    if (build_happy_fixture(&buf) != 0) FAIL("build fixture");
    char *path = write_temp_file(&buf);
    if (!path) FAIL("write temp");

    int n = tagcache_library_load_tcdb(path);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(tagcache_library_loaded(), 1);

    /* Counts. */
    EXPECT_EQ(tagcache_song_count(),     3);
    EXPECT_EQ(tagcache_artist_count(),   2);
    EXPECT_EQ(tagcache_album_count(),    2);
    EXPECT_EQ(tagcache_genre_count(),    1);
    EXPECT_EQ(tagcache_composer_count(), 1);

    /* Song titles + paths. */
    EXPECT_STREQ(tagcache_song_title(0), "Apple");
    EXPECT_STREQ(tagcache_song_title(1), "Banana");
    EXPECT_STREQ(tagcache_song_title(2), "Cherry");
    EXPECT_STREQ(tagcache_song_path(0),  "/m/a.flac");
    EXPECT_STREQ(tagcache_song_path(1),  "/m/b.flac");
    EXPECT_STREQ(tagcache_song_path(2),  "/m/c.flac");

    /* Per-song tag fields, including the all-NULL row 2. */
    EXPECT_STREQ(tagcache_song_artist(0), "X");
    EXPECT_STREQ(tagcache_song_artist(1), "Y");
    if (tagcache_song_artist(2) != NULL) FAIL("song[2] artist should be NULL");
    if (tagcache_song_album (2) != NULL) FAIL("song[2] album  should be NULL");

    /* Uniq lists. */
    EXPECT_STREQ(tagcache_artist_name(0),   "X");
    EXPECT_STREQ(tagcache_artist_name(1),   "Y");
    EXPECT_STREQ(tagcache_album_name(0),    "P");
    EXPECT_STREQ(tagcache_album_name(1),    "R");
    EXPECT_STREQ(tagcache_genre_name(0),    "Q");
    EXPECT_STREQ(tagcache_composer_name(0), "Z");

    /* Drilldown counts + members. */
    EXPECT_EQ(tagcache_song_count_for_artist(0), 1);
    EXPECT_EQ(tagcache_song_count_for_artist(1), 1);
    EXPECT_EQ(tagcache_song_count_for_genre(0),  2);
    EXPECT_EQ(tagcache_song_count_for_composer(0), 2);
    EXPECT_EQ(tagcache_song_index_for_artist(0, 0),  0);
    EXPECT_EQ(tagcache_song_index_for_artist(1, 0),  1);
    EXPECT_EQ(tagcache_song_index_for_genre (0, 0),  0);
    EXPECT_EQ(tagcache_song_index_for_genre (0, 1),  1);
    EXPECT_EQ(tagcache_song_index_for_composer(0, 1), 1);

    /* Art bytes for song 0 only. */
    size_t art_len = 0;
    const void *art = tagcache_song_art_bytes(0, &art_len);
    if (!art || art_len != 5) FAIL("song[0] art: ptr=%p len=%zu", art, art_len);
    if (memcmp(art, "\xff\xd8\xff\xe0\x00", 5) != 0) FAIL("song[0] art bytes mismatch");
    if (tagcache_song_art_bytes(1, &art_len) != NULL) FAIL("song[1] should have no art");

    unlink(path);
    free(path);
    free(buf.data);
    return 0;
}

static int test_bad_magic(void) {
    buf_t buf = {0};
    if (build_happy_fixture(&buf) != 0) FAIL("build fixture");
    buf.data[0] = 'X';   /* corrupt magic */
    char *path = write_temp_file(&buf);
    int rc = tagcache_library_load_tcdb(path);
    EXPECT_EQ(rc, -1);
    unlink(path); free(path); free(buf.data);
    return 0;
}

static int test_bad_version(void) {
    buf_t buf = {0};
    if (build_happy_fixture(&buf) != 0) FAIL("build fixture");
    put_u32(buf.data + TCDB_OFF_VERSION, TCDB_VERSION + 99);
    char *path = write_temp_file(&buf);
    int rc = tagcache_library_load_tcdb(path);
    EXPECT_EQ(rc, -1);
    unlink(path); free(path); free(buf.data);
    return 0;
}

static int test_truncated_header(void) {
    buf_t buf = {0};
    if (build_happy_fixture(&buf) != 0) FAIL("build fixture");
    buf.len = TCDB_HEADER_SIZE - 1;  /* one short of a valid header */
    char *path = write_temp_file(&buf);
    int rc = tagcache_library_load_tcdb(path);
    EXPECT_EQ(rc, -1);
    unlink(path); free(path); free(buf.data);
    return 0;
}

int main(void) {
    int failures = 0;
    if (test_happy()             != 0) failures++;
    if (test_bad_magic()         != 0) failures++;
    if (test_bad_version()       != 0) failures++;
    if (test_truncated_header()  != 0) failures++;
    if (failures > 0) {
        fprintf(stderr, "tcdb_reader_test: %d case%s failed\n",
                failures, failures == 1 ? "" : "s");
        return 1;
    }
    fprintf(stdout, "ok: tcdb reader passed all 4 cases\n");
    return 0;
}
