/*
 * core/apps/ui/art_cache.h — decoded album-art LRU.
 *
 * Album art arrives as JPEG bytes in the tagcache. Decoding to RGB565 +
 * scaling to a target size is the slow part (~80 ms cold for a 500×500
 * source on hardware, per PLAN.md performance budget). Doing that on
 * every track switch — or worse, on every draw — would drop frames and
 * stutter the UI.
 *
 * This module caches the decoded pixel buffers keyed by song index. The
 * UI primes the cache when a song starts playing; renderers (Now Playing
 * pages, list-row thumbnails when those land) read pixels back at draw
 * time with a single hash lookup. Same-album track switches are free
 * after the first track decodes.
 *
 * Each cache slot holds both NP sizes (84×84 small, 180×180 big) so the
 * NP screen can flip between the default page and the big-art page
 * without re-decoding. Storage is fully static — no malloc per the
 * project's design principles. LRU eviction picks the least-recently-
 * read slot when the cache is full and a new song arrives.
 *
 * Concurrency: single-threaded UI loop. The cache is not safe to call
 * from the codec thread; that's fine because every call site is in the
 * UI/main path (cabinet at play time, NP renderers at draw time).
 */

#ifndef CORE_APPS_UI_ART_CACHE_H
#define CORE_APPS_UI_ART_CACHE_H

#include <stdbool.h>
#include <stdint.h>

/* Sizes mirror the buffers Now Playing renders. The cache is the
 * canonical owner of the pixel buffers and the NP screen is one of
 * several future readers, so the constants live here.
 *
 * SMALL (130×130) fills the left half of the NP default page. It's a
 * deliberate bump over the 84×84 in the original Linen design: the
 * stock layout left a ~50 px dead zone between the art and the "Up
 * next" row, which felt visually thin on a 320×240 panel. BIG
 * (180×180) is the full-screen big-art page. */
#define ART_CACHE_SMALL_W   130
#define ART_CACHE_SMALL_H   130
#define ART_CACHE_BIG_W     180
#define ART_CACHE_BIG_H     180

typedef enum {
    ART_SIZE_SMALL = 0,    /* 84×84 — NP default page, future list thumbnails */
    ART_SIZE_BIG   = 1,    /* 180×180 — NP big-art page */
} art_size_t;

/*
 * Ensure `song_idx`'s art is decoded and resident. Idempotent: if the
 * slot is already populated nothing happens. If the song has no
 * embedded art (tagcache returns NULL bytes), the entry is recorded as
 * "no art" so subsequent primes don't re-check the tagcache.
 *
 * Decode failures (malformed JPEG, OOM in stb_image) are also cached
 * as "no art" — we don't want to retry a broken file on every draw.
 *
 * Call this from the play path after a successful audio_engine_play(),
 * not from the draw path: the JPEG decode is too slow to do under a
 * frame budget.
 */
void art_cache_prime(int song_idx);

/*
 * Read a previously-primed art buffer at `size`. Returns a pointer to
 * the row-major RGB565 pixel buffer (size determined by `size`), or
 * NULL if the song isn't cached, has no art, or decode failed.
 *
 * The pointer is valid until the next call to art_cache_prime() or
 * art_cache_invalidate_all() — i.e. effectively until the next track
 * change. Renderers should not stash it across frames.
 *
 * Calling get() bumps the LRU stamp so the slot survives the next
 * eviction round.
 */
const uint16_t *art_cache_get(int song_idx, art_size_t size);

/*
 * Drop every cached entry. Call after a tagcache reload — the song
 * indices that were valid before may now point at completely different
 * tracks, and the cached pixels would be wrong.
 */
void art_cache_invalidate_all(void);

#endif /* CORE_APPS_UI_ART_CACHE_H */
