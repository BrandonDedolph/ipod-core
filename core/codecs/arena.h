/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/arena.h — static bump arena backing decoder_alloc_t.
 *
 * The freestanding build has no heap, but dr_flac (and other codecs) want an
 * allocator at open() time. A decoder allocates a handful of buffers once
 * when a track opens and frees them all at close — a perfect fit for a bump
 * allocator over a caller-provided static buffer: alloc() bumps a pointer,
 * free() is a no-op, and the whole arena is reclaimed by reset() between
 * tracks. realloc() is supported (alloc-new + copy) since dr_flac uses it.
 *
 * high_water records the peak bytes in use so the arena can be sized from a
 * real decode rather than guessed. On exhaustion alloc() returns NULL (which
 * dr_flac treats as an open failure) and sets `oom`.
 */
#ifndef CORE_CODECS_ARENA_H
#define CORE_CODECS_ARENA_H

#include <stddef.h>

#include "decoder.h"

typedef struct decoder_arena {
    unsigned char *base;
    size_t         cap;
    size_t         used;
    size_t         high_water;   /* peak `used`, for sizing                 */
    int            oom;          /* set once an allocation didn't fit       */
} decoder_arena_t;

/* Bind `buf` (cap bytes) as the arena's backing store, empty. */
void decoder_arena_init(decoder_arena_t *a, void *buf, size_t cap);

/* Reclaim everything (keeps high_water). Call between tracks. */
void decoder_arena_reset(decoder_arena_t *a);

/* A decoder_alloc_t whose alloc/realloc/free are backed by `a`. Pass the
 * result (by &) to a codec open(). */
decoder_alloc_t decoder_arena_allocator(decoder_arena_t *a);

#endif /* CORE_CODECS_ARENA_H */
