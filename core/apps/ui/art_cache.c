/*
 * core/apps/ui/art_cache.c — fixed-size LRU for decoded album art.
 *
 * Four slots. Each slot holds the small and big buffers for one song;
 * we always decode both sizes together because (a) the NP screen
 * routinely flips between pages, so paying the decode once for both is
 * cheaper than two decodes spread across two SELECT presses, and (b)
 * the JPEG header / IDCT is the bulk of the cost — scaling adds
 * little. Slots are never resized, so there's no fragmentation; LRU
 * picks the oldest read for eviction.
 *
 * Static storage: 4 × (84·84 + 180·180) × 2 B = ~308 KB. Comfortably
 * inside the 2 MB static-RAM budget in PLAN.md, and the only album-art
 * memory in the system once Now Playing migrates off its inline
 * buffers.
 */

#include "art_cache.h"

#include "../db/tagcache.h"
#include "../../codecs/stb_image/image.h"
#include "../../hal/hal.h"

#include <stddef.h>

#define ART_CACHE_SLOTS  4

typedef struct {
    int      song_idx;        /* -1 = empty slot, never primed */
    bool     has_art;          /* true iff small/big are populated */
    uint64_t last_used;        /* monotonic; higher = more recent */
    uint16_t small[ART_CACHE_SMALL_W * ART_CACHE_SMALL_H];
    uint16_t big  [ART_CACHE_BIG_W   * ART_CACHE_BIG_H];
} slot_t;

static slot_t   g_slots[ART_CACHE_SLOTS];
static uint64_t g_lru_counter;
static bool     g_initialized;

static void ensure_init(void) {
    if (g_initialized) return;
    for (int i = 0; i < ART_CACHE_SLOTS; i++) {
        g_slots[i].song_idx = -1;
    }
    g_initialized = true;
}

/* Find a slot already holding `song_idx`. Returns NULL if none. */
static slot_t *find_slot(int song_idx) {
    for (int i = 0; i < ART_CACHE_SLOTS; i++) {
        if (g_slots[i].song_idx == song_idx) return &g_slots[i];
    }
    return NULL;
}

/* Pick the slot to evict: prefer empty (-1), else the one with the
 * oldest last_used stamp. Always returns a valid slot. */
static slot_t *evict_target(void) {
    slot_t *oldest = &g_slots[0];
    for (int i = 0; i < ART_CACHE_SLOTS; i++) {
        if (g_slots[i].song_idx < 0) return &g_slots[i];
        if (g_slots[i].last_used < oldest->last_used) oldest = &g_slots[i];
    }
    return oldest;
}

void art_cache_prime(int song_idx) {
    ensure_init();
    if (song_idx < 0) return;

    slot_t *s = find_slot(song_idx);
    if (s) {
        /* Already primed — refresh the LRU stamp so a song the user
         * is actively listening to doesn't get evicted by a navigation
         * flurry that touches other entries. */
        s->last_used = ++g_lru_counter;
        return;
    }

    size_t art_len = 0;
    const void *art_bytes = tagcache_song_art_bytes(song_idx, &art_len);

    s = evict_target();
    s->song_idx  = song_idx;
    s->last_used = ++g_lru_counter;

    if (!art_bytes || art_len == 0) {
        /* Tagcache says no embedded art. Record as "no art" rather
         * than leaving the slot empty, so subsequent primes hit the
         * fast no-op path instead of re-asking the tagcache. */
        s->has_art = false;
        return;
    }

    int rc1 = image_jpeg_decode_rgb565(art_bytes, art_len,
                                       ART_CACHE_SMALL_W, ART_CACHE_SMALL_H,
                                       s->small);
    int rc2 = image_jpeg_decode_rgb565(art_bytes, art_len,
                                       ART_CACHE_BIG_W, ART_CACHE_BIG_H,
                                       s->big);
    s->has_art = (rc1 == 0 && rc2 == 0);
    if (!s->has_art) {
        log_printf("art_cache: decode failed for song %d (%zu B JPEG)",
                   song_idx, art_len);
    }
}

const uint16_t *art_cache_get(int song_idx, art_size_t size) {
    ensure_init();
    if (song_idx < 0) return NULL;

    slot_t *s = find_slot(song_idx);
    if (!s || !s->has_art) return NULL;

    s->last_used = ++g_lru_counter;
    return (size == ART_SIZE_BIG) ? s->big : s->small;
}

void art_cache_invalidate_all(void) {
    ensure_init();
    for (int i = 0; i < ART_CACHE_SLOTS; i++) {
        g_slots[i].song_idx = -1;
        g_slots[i].has_art  = false;
    }
    g_lru_counter = 0;
}
