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
    bool     occupied;        /* false = never primed; key/has_art/pixels are stale */
    int      key;             /* song_idx for song art; pseudo-key for artist art */
    bool     has_art;         /* true iff pixels[] is populated */
    uint64_t last_used;       /* monotonic; higher = more recent */
    uint16_t pixels[THUMB_CACHE_W * THUMB_CACHE_H];
} slot_t;

static slot_t   g_slots[THUMB_CACHE_SLOTS];
static uint64_t g_lru_counter;
static bool     g_initialized;

static void ensure_init(void) {
    if (g_initialized) return;
    /* `occupied` defaults to false from BSS zero-init; nothing else
     * needs initializing for the first lookup to behave correctly. */
    g_initialized = true;
}

static slot_t *find_slot(int key) {
    for (int i = 0; i < THUMB_CACHE_SLOTS; i++) {
        if (g_slots[i].occupied && g_slots[i].key == key) return &g_slots[i];
    }
    return NULL;
}

static slot_t *evict_target(void) {
    slot_t *oldest = &g_slots[0];
    for (int i = 0; i < THUMB_CACHE_SLOTS; i++) {
        if (!g_slots[i].occupied) return &g_slots[i];
        if (g_slots[i].last_used < oldest->last_used) oldest = &g_slots[i];
    }
    return oldest;
}

/* Internal core: populate a slot for `key` given pre-fetched bytes.
 * Both public prime variants funnel through here so the LRU behavior
 * and decode-failure caching stays in one place. */
static void prime_bytes_internal(int key, const void *bytes, size_t len) {
    slot_t *s = find_slot(key);
    if (s) {
        s->last_used = ++g_lru_counter;
        return;
    }
    s = evict_target();
    s->occupied  = true;
    s->key       = key;
    s->last_used = ++g_lru_counter;

    if (!bytes || len == 0) {
        s->has_art = false;
        return;
    }
    int rc = image_jpeg_decode_rgb565(bytes, len,
                                      THUMB_CACHE_W, THUMB_CACHE_H,
                                      s->pixels);
    s->has_art = (rc == 0);
    if (!s->has_art) {
        log_printf("thumb_cache: decode failed for key %d (%zu B JPEG)",
                   key, len);
    }
}

void thumb_cache_prime(int song_idx) {
    ensure_init();
    if (song_idx < 0) return;
    /* Hot path first — slot already populated, just bump LRU. Avoids
     * the tagcache lookup on every draw of an already-decoded row. */
    slot_t *s = find_slot(song_idx);
    if (s) {
        s->last_used = ++g_lru_counter;
        return;
    }
    size_t art_len = 0;
    const void *art_bytes = tagcache_song_art_bytes(song_idx, &art_len);
    prime_bytes_internal(song_idx, art_bytes, art_len);
}

void thumb_cache_prime_bytes(int key, const void *bytes, size_t len) {
    ensure_init();
    prime_bytes_internal(key, bytes, len);
}

const uint16_t *thumb_cache_get(int key) {
    ensure_init();
    slot_t *s = find_slot(key);
    if (!s || !s->has_art) return NULL;
    s->last_used = ++g_lru_counter;
    return s->pixels;
}

void thumb_cache_invalidate_all(void) {
    ensure_init();
    for (int i = 0; i < THUMB_CACHE_SLOTS; i++) {
        g_slots[i].occupied = false;
        g_slots[i].has_art  = false;
    }
    g_lru_counter = 0;
}
