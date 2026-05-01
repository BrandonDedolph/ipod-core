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
 * (compiled-in const data); callers must not free it.
 *
 * Indices that are out of range return a non-NULL "(?)" placeholder
 * — never NULL — so callers don't need to defensively-check.
 */
const char *tagcache_artist_name(int idx);
const char *tagcache_album_name(int idx);
const char *tagcache_song_title(int idx);
const char *tagcache_genre_name(int idx);
const char *tagcache_composer_name(int idx);

#endif /* CORE_APPS_DB_TAGCACHE_H */
