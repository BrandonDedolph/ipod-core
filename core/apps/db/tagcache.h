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

#endif /* CORE_APPS_DB_TAGCACHE_H */
