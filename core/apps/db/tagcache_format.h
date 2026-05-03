/*
 * core/apps/db/tagcache_format.h — on-disk constants for the binary
 * tagcache (.tcdb) the firmware mmaps at startup.
 *
 * The Go-side encoder lives at core/cli/internal/tagcache/format.go;
 * any change to the layout here MUST be mirrored there (and the
 * Version constant bumped in both places). The Go round-trip test
 * + the C-side parser unit test should both fail loudly if the two
 * sides drift.
 *
 * File layout (all integers little-endian, no padding):
 *
 *   [0..132)   Header   — magic, version, section offsets/lengths
 *   [...)      Songs    — fixed 40-byte records, song_count of them
 *   [...)      Uniq     — four arrays of u32 string-table offsets
 *   [...)      Groups   — four per-dimension blocks; each block is
 *                          n u32 offsets (relative to that block's
 *                          start) followed by [u32 count, u32*count
 *                          song-indices] tuples
 *   [...)      Strings  — null-terminated UTF-8, deduped, offset 0
 *                          reserved for ""
 *   [...)      Art      — concatenated raw JPEG bytes, content-deduped
 */

#ifndef CORE_APPS_DB_TAGCACHE_FORMAT_H
#define CORE_APPS_DB_TAGCACHE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/* Four-byte magic at file offset 0. */
#define TCDB_MAGIC0 'T'
#define TCDB_MAGIC1 'C'
#define TCDB_MAGIC2 'D'
#define TCDB_MAGIC3 'B'

/* Bumped on incompatible layout changes. The reader rejects mismatches. */
#define TCDB_VERSION 1u

/* Byte size of the fixed header. The first byte after this is the
 * start of the song-record array. Mirrors core/cli/internal/tagcache/
 * format.go::HeaderSize. */
#define TCDB_HEADER_SIZE 132

/* Byte size of one song record. Mirrors SongRecord in format.go. */
#define TCDB_SONG_RECORD_SIZE 40

/* Sentinel index value (-1, stored as a sign-extended i32) used in
 * song records when the file's tag for a given dimension is absent. */
#define TCDB_MISSING_TAG (-1)

/* Header field byte offsets (from file start). Helps the C parser
 * read the header without depending on struct layout / packing. */
#define TCDB_OFF_MAGIC               0   /* 4 bytes */
#define TCDB_OFF_VERSION             4   /* u32 */
#define TCDB_OFF_SONG_COUNT          8   /* u32 */
#define TCDB_OFF_N_ARTISTS          12   /* u32 */
#define TCDB_OFF_N_ALBUMS           16   /* u32 */
#define TCDB_OFF_N_GENRES           20   /* u32 */
#define TCDB_OFF_N_COMPOSERS        24   /* u32 */
#define TCDB_OFF_SONGS_OFF          28   /* u64 */
#define TCDB_OFF_ARTIST_IDX_OFF     36   /* u64 */
#define TCDB_OFF_ALBUM_IDX_OFF      44   /* u64 */
#define TCDB_OFF_GENRE_IDX_OFF      52   /* u64 */
#define TCDB_OFF_COMPOSER_IDX_OFF   60   /* u64 */
#define TCDB_OFF_ARTIST_GROUPS_OFF  68   /* u64 */
#define TCDB_OFF_ALBUM_GROUPS_OFF   76   /* u64 */
#define TCDB_OFF_GENRE_GROUPS_OFF   84   /* u64 */
#define TCDB_OFF_COMPOSER_GROUPS_OFF 92  /* u64 */
#define TCDB_OFF_STRINGS_OFF       100   /* u64 */
#define TCDB_OFF_STRINGS_LEN       108   /* u64 */
#define TCDB_OFF_ART_OFF           116   /* u64 */
#define TCDB_OFF_ART_LEN           124   /* u64 */

/* Song-record field offsets (from start of one record). */
#define TCDB_REC_TITLE_OFF        0   /* u32 */
#define TCDB_REC_PATH_OFF         4   /* u32 */
#define TCDB_REC_ARTIST_IDX       8   /* i32 */
#define TCDB_REC_ALBUM_IDX       12   /* i32 */
#define TCDB_REC_GENRE_IDX       16   /* i32 */
#define TCDB_REC_COMPOSER_IDX    20   /* i32 */
#define TCDB_REC_ART_OFF         24   /* u64 — relative to header.art_off */
#define TCDB_REC_ART_LEN         32   /* u64 */

#endif /* CORE_APPS_DB_TAGCACHE_FORMAT_H */
