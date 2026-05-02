/*
 * core/apps/db/tagcache.h — music library index.
 *
 * The audio engine + UI talk to the tagcache through this small API:
 * "give me the list of artists / albums / songs / genres / composers."
 * Implementation detail: the data source.
 *
 * Today: a hardcoded synthetic dataset compiled into the binary, just
 * enough to populate the Music sub-menu with realistic content while
 * the Go-side indexer + on-disk binary format land in a follow-up PR.
 *
 * `tagcache_library_load` swaps the Songs list for real .flac/.mp3
 * files scanned from a directory at process start. Artists / albums /
 * etc. stay synthetic until the proper indexer + drilldown lands.
 *
 * Tomorrow: same API, but reads the binary tagcache file built by
 * `core release tagcache <music-dir>`. Cabinet doesn't change.
 */

#ifndef CORE_APPS_DB_TAGCACHE_H
#define CORE_APPS_DB_TAGCACHE_H

#include <stdint.h>

/* ---------- Library counts --------------------------------------- */

int tagcache_artist_count(void);
int tagcache_album_count(void);
int tagcache_song_count(void);
int tagcache_genre_count(void);
int tagcache_composer_count(void);

/* ---------- Read by index ---------------------------------------- */

/*
 * Each *_name function returns a pointer to a stable, null-terminated
 * UTF-8 string. The pointer is valid for the lifetime of the program
 * (compiled-in const data, or heap data owned by tagcache); callers
 * must not free it.
 *
 * Indices that are out of range return a non-NULL "(?)" placeholder
 * — never NULL — so callers don't need to defensively-check.
 */
const char *tagcache_artist_name(int idx);
const char *tagcache_album_name(int idx);
const char *tagcache_song_title(int idx);
const char *tagcache_genre_name(int idx);
const char *tagcache_composer_name(int idx);

/* ---------- Library scan ----------------------------------------- */

/*
 * Scan `dir` (non-recursively) for files whose extension is .flac or
 * .mp3 (case-insensitive). On success, the Songs list is replaced
 * with the discovered files (sorted alphabetically by filename) and
 * subsequent tagcache_song_path(idx) calls return the absolute path
 * for the corresponding row.
 *
 * Returns the number of songs loaded (>= 0), or -1 on error
 * (directory not readable, OOM mid-scan, etc). On error, the previous
 * library state is preserved.
 *
 * **Call once, at startup.** The strings returned by
 * tagcache_song_title / tagcache_song_path point into heap memory
 * that is freed if library_load is called a second time. If a
 * "rescan" feature is added later, it must invalidate every UI cached
 * pointer first (or this function must be reworked to atomically swap
 * in a new library while keeping the old one alive until quiescence).
 */
int tagcache_library_load(const char *dir);

/*
 * Filesystem path for the song at `idx` if a library has been loaded
 * via tagcache_library_load. Returns NULL when:
 *   - no library has been loaded (synthetic data is in use), or
 *   - idx is out of range.
 *
 * The pointer is owned by the tagcache and is stable until the next
 * tagcache_library_load call (which is "never" in the current
 * single-load contract).
 */
const char *tagcache_song_path(int idx);

/*
 * Per-song artist / album as parsed from the file's tags during
 * library_load. Returns NULL when:
 *   - no library has been loaded, or
 *   - idx is out of range, or
 *   - the file had no ARTIST / ALBUM tag (untagged file or unsupported
 *     codec).
 *
 * Future drilldown UI uses these to group songs by artist/album.
 */
const char *tagcache_song_artist(int idx);
const char *tagcache_song_album(int idx);

/* ---------- Filtered queries (drilldown) ------------------------- */

/*
 * Songs whose ARTIST tag matches the unique-artist at
 * tagcache_artist_name(artist_idx). Returns 0 when no library is
 * loaded or artist_idx is out of range. Songs are presented in the
 * same alphabetical-by-title order as the global Songs list.
 *
 * `n` is the per-artist row index (0..count-1); the corresponding
 * accessors return the song's title and absolute path.
 */
int          tagcache_song_count_for_artist(int artist_idx);
const char  *tagcache_song_title_for_artist(int artist_idx, int n);
const char  *tagcache_song_path_for_artist (int artist_idx, int n);

/*
 * Same shape for the per-album view.
 */
int          tagcache_song_count_for_album(int album_idx);
const char  *tagcache_song_title_for_album(int album_idx, int n);
const char  *tagcache_song_path_for_album (int album_idx, int n);

#endif /* CORE_APPS_DB_TAGCACHE_H */
