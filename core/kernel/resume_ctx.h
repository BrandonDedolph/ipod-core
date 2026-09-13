/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kernel/resume_ctx.h — the pure half of the resume QUEUE CONTEXT.
 *
 * The boot path saves not just which track was playing but what it was
 * playing IN, so a cold boot brings back the Songs list, the artist, the
 * genre or the Shuffle Songs draw instead of the track's album. Every queue
 * this firmware builds is a function of the library plus a few words; this
 * header holds the parts of that which touch no device state — which list a
 * Songs view is, the one library-order draw that has to be repeatable, and
 * the settings_t bookkeeping — so tests/kernel/resume_test.c links the real
 * thing rather than a copy (contrast resume_find_song, which is pasted into
 * the test and diffed against main.c by check_resume_parity.py).
 */
#ifndef CORE_KERNEL_RESUME_CTX_H
#define CORE_KERNEL_RESUME_CTX_H

#include <stdint.h>
#include "../ui/settings.h"

/*
 * Which RESUME_KIND a Songs view built by songview_build(genre, artist) is.
 * The genre list is entered from Genres with no artist; the artist list from
 * an artist's album browser with no genre; Songs from the Music menu with
 * neither. Nothing builds both, so genre simply wins.
 */
static inline int resume_kind_of_view(int genre, const char *artist)
{
    if (genre >= 0)            return RESUME_KIND_GENRE;
    if (artist && artist[0])   return RESUME_KIND_ARTIST;
    return RESUME_KIND_SONGS;
}

/*
 * Shuffle Songs' library order: the song indices 0..n-1, Fisher-Yates dealt
 * by a tiny LCG started at `seed`. PURE — the same (seed, n) is the same
 * order, which is how a saved seed brings a Shuffle Songs queue back after a
 * power cut. The low bit of an LCG is the least random, hence the >> 1. The
 * caller draws the seed (main.c ORs the timer with 1 so it is never 0, which
 * the record uses for "no seed").
 */
static inline void lib_shuffle_order(uint16_t *ord, int n, uint32_t seed)
{
    uint32_t rng = seed;
    for (int i = 0; i < n; i++) {
        ord[i] = (uint16_t)i;
    }
    for (int i = n - 1; i > 0; i--) {
        rng = rng * 1664525u + 1013904223u;
        int j = (int)((rng >> 1) % (uint32_t)(i + 1));
        uint16_t t = ord[i]; ord[i] = ord[j]; ord[j] = t;
    }
}

/* Everything one capture writes: the locator plus the queue context. */
typedef struct {
    uint32_t hash;          /* name_hash of the track's stem; 0 = nothing    */
    uint32_t secs;          /* elapsed seconds                               */
    uint32_t total;         /* track length, the locator's cross-check       */
    int      kind;          /* RESUME_KIND_*                                 */
    int      qidx;          /* queue index of the track                      */
    uint32_t seed;          /* Shuffle Songs' library-order seed, else 0     */
    uint32_t order_seed;    /* player_order_seed()                           */
    int      order_keep;    /* player_order_keep()                           */
    uint32_t ctx_hash;      /* RESUME_KIND_PLAYLIST: name_hash of the
                             * playlist's ext-trimmed filename — the one
                             * word that says WHICH playlist, since nothing
                             * in the song's own record does. Stored as 0
                             * for every other kind, whatever is passed.   */
} resume_ctx_t;

/*
 * Store `c` into the settings record's resume fields. Returns 1 when
 * something changed (the caller schedules a write), 0 when the record
 * already held exactly this — a capture is idempotent, so callers can be
 * liberal about when they call it without costing a disk write.
 */
static inline int resume_ctx_store(settings_t *s, const resume_ctx_t *c)
{
    int      qidx = c->qidx < 0 ? 0 : (c->qidx > 0xFFFF ? 0xFFFF : c->qidx);
    uint8_t  kind = (uint8_t)(c->kind >= 0 && c->kind <= RESUME_KIND_MAX ? c->kind : 0);
    uint32_t ctx  = (kind == RESUME_KIND_PLAYLIST) ? c->ctx_hash : 0u;

    if (s->resume_hash == c->hash && s->resume_secs == c->secs &&
        s->resume_total == c->total && s->resume_kind == kind &&
        s->resume_qidx == (uint16_t)qidx && s->resume_seed == c->seed &&
        s->resume_order_seed == c->order_seed &&
        s->resume_order_keep == c->order_keep &&
        s->resume_ctx_hash == ctx) {
        return 0;
    }
    s->resume_hash       = c->hash;
    s->resume_secs       = c->secs;
    s->resume_total      = c->total;
    s->resume_kind       = kind;
    s->resume_flags      = 0;
    s->resume_qidx       = (uint16_t)qidx;
    s->resume_seed       = c->seed;
    s->resume_order_seed = c->order_seed;
    s->resume_order_keep = c->order_keep;
    s->resume_ctx_hash   = ctx;
    return 1;
}

/* Forget the locator and its context. 1 when there was something to forget. */
static inline int resume_ctx_clear(settings_t *s)
{
    resume_ctx_t z = { 0, 0, 0, RESUME_KIND_NONE, 0, 0, 0, 0, 0 };
    return resume_ctx_store(s, &z);
}

#endif /* CORE_KERNEL_RESUME_CTX_H */
