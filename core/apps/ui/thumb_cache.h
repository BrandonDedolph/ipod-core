/*
 * core/apps/ui/thumb_cache.h — small album-art thumbnails for list rows.
 *
 * Sibling to art_cache. art_cache is sized for active listening (a few
 * full-resolution decodes for Now Playing); thumb_cache is sized for
 * browsing — many small entries so a user can scroll through all of
 * their album list without thrashing. Keeping them separate avoids the
 * working-set conflict: a navigation flurry would otherwise evict the
 * playing track's NP art from a unified pool, and a long playback run
 * would gradually starve the browse view.
 *
 * Each slot holds a single 22 × 22 RGB565 buffer. 32 slots × ~1 KB ≈
 * 31 KB total — negligible against the static-RAM budget. 32 is
 * comfortably more than LIST_VISIBLE_ROWS (7) plus typical scroll
 * lookahead, so for any reasonably-sized library the working set lives
 * entirely in cache.
 *
 * Decode budget: get() is expected to be called from the draw path,
 * and a JPEG decode is too slow to hide under one frame. The intended
 * usage is "look up cheaply at draw time; populate on user-driven
 * events (entering the list, scrolling)" via prime(). Today prime()
 * just decodes synchronously — on host that's fast enough; on hardware
 * we'll need a background decode task before this can ship.
 */

#ifndef CORE_APPS_UI_THUMB_CACHE_H
#define CORE_APPS_UI_THUMB_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define THUMB_CACHE_W   22
#define THUMB_CACHE_H   22

/*
 * Ensure `song_idx` has a decoded thumbnail in the cache. Idempotent:
 * if the slot is already populated nothing happens. Songs with no
 * embedded art are recorded as "no art" so we don't repeatedly probe
 * the tagcache for the same negative answer.
 */
void thumb_cache_prime(int song_idx);

/*
 * Same shape as thumb_cache_prime, but the JPEG bytes come from the
 * caller rather than the song-art tagcache. Used for artist photos
 * (which live in a parallel section of the .tcdb) and any other
 * future per-non-song images. `key` must be unique against all other
 * cache occupants — convention is to use negative pseudo-indices for
 * non-song entries (e.g. -1000 - artist_idx for artists) so they
 * never collide with real song_idx values.
 *
 * If the slot is already populated for this key, the call is a no-op.
 * On decode failure the slot is recorded as "no art" so the caller
 * doesn't retry every frame.
 */
void thumb_cache_prime_bytes(int key, const void *bytes, size_t len);

/*
 * Look up a previously-primed thumbnail. Returns a pointer to the
 * row-major 22 × 22 RGB565 buffer, or NULL if the song isn't cached,
 * has no embedded art, or its decode failed. Renderers fall back to
 * a placeholder fill on NULL.
 *
 * The pointer is valid until the next prime() that triggers an
 * eviction, or thumb_cache_invalidate_all(); callers should not stash
 * across frames.
 *
 * get() bumps the LRU stamp so a thumb the user is actively staring
 * at survives the next eviction round.
 */
const uint16_t *thumb_cache_get(int song_idx);

/*
 * Drop every cached entry. Call after a tagcache reload — the song
 * indices that were valid before may now point at completely different
 * tracks.
 */
void thumb_cache_invalidate_all(void);

#endif /* CORE_APPS_UI_THUMB_CACHE_H */
