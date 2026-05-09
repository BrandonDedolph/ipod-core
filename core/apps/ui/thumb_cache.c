/*
 * core/apps/ui/thumb_cache.c — many-slot LRU for list-row thumbnails.
 *
 * Mirrors the structure of art_cache.c but with smaller buffers and
 * many more slots. 32 × (22 · 22 · 2) ≈ 31 KB. The buffer-per-slot
 * design (rather than a single shared scratch + indirection) keeps
 * the lookup hot path branch-free: get() is one linear scan over a
 * small array of ints + a pointer return.
 */

#include "thumb_cache.h"

#include "../db/tagcache.h"
#include "../../codecs/stb_image/image.h"
#include "../../hal/hal.h"

#include <stddef.h>

#define THUMB_CACHE_SLOTS  32

typedef struct {
    int      song_idx;        /* -1 = empty slot, never primed */
    bool     has_art;         /* true iff pixels[] is populated */
    uint64_t last_used;       /* monotonic; higher = more recent */
    uint16_t pixels[THUMB_CACHE_W * THUMB_CACHE_H];
} slot_t;

static slot_t   g_slots[THUMB_CACHE_SLOTS];
static uint64_t g_lru_counter;
static bool     g_initialized;

static void ensure_init(void) {
    if (g_initialized) return;
    for (int i = 0; i < THUMB_CACHE_SLOTS; i++) {
        g_slots[i].song_idx = -1;
    }
    g_initialized = true;
}

static slot_t *find_slot(int song_idx) {
    for (int i = 0; i < THUMB_CACHE_SLOTS; i++) {
        if (g_slots[i].song_idx == song_idx) return &g_slots[i];
    }
    return NULL;
}

static slot_t *evict_target(void) {
    slot_t *oldest = &g_slots[0];
    for (int i = 0; i < THUMB_CACHE_SLOTS; i++) {
        if (g_slots[i].song_idx < 0) return &g_slots[i];
        if (g_slots[i].last_used < oldest->last_used) oldest = &g_slots[i];
    }
    return oldest;
}

void thumb_cache_prime(int song_idx) {
    ensure_init();
    if (song_idx < 0) return;

    slot_t *s = find_slot(song_idx);
    if (s) {
        s->last_used = ++g_lru_counter;
        return;
    }

    size_t art_len = 0;
    const void *art_bytes = tagcache_song_art_bytes(song_idx, &art_len);

    s = evict_target();
    s->song_idx  = song_idx;
    s->last_used = ++g_lru_counter;

    if (!art_bytes || art_len == 0) {
        s->has_art = false;
        return;
    }

    int rc = image_jpeg_decode_rgb565(art_bytes, art_len,
                                      THUMB_CACHE_W, THUMB_CACHE_H,
                                      s->pixels);
    s->has_art = (rc == 0);
    if (!s->has_art) {
        log_printf("thumb_cache: decode failed for song %d (%zu B JPEG)",
                   song_idx, art_len);
    }
}

const uint16_t *thumb_cache_get(int song_idx) {
    ensure_init();
    if (song_idx < 0) return NULL;
    slot_t *s = find_slot(song_idx);
    if (!s || !s->has_art) return NULL;
    s->last_used = ++g_lru_counter;
    return s->pixels;
}

void thumb_cache_invalidate_all(void) {
    ensure_init();
    for (int i = 0; i < THUMB_CACHE_SLOTS; i++) {
        g_slots[i].song_idx = -1;
        g_slots[i].has_art  = false;
    }
    g_lru_counter = 0;
}
