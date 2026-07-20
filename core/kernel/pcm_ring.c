/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/pcm_ring.c — SPSC PCM ring (see pcm_ring.h).
 *
 * The data-visibility guarantee lives in write()/read(): each side stores
 * (or reads) the ring slots first, then publishes its index with release
 * ordering, and acquire-loads the other side's index before trusting the
 * slots it points at. fill()/free() are advisory counts and use acquire
 * loads of both indices.
 */

#include "pcm_ring.h"

/* ---- SPSC index ordering helpers (mirror apps/audio/engine.c) -------- */

/* Publish one's own index with release ordering (after storing slots). */
static inline void ring_publish(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/* Read the *other* side's index with acquire ordering (before reading slots). */
static inline uint32_t ring_observe(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/* Read one's own index. Relaxed suffices — a thread sees its own writes in
 * program order. */
static inline uint32_t ring_own(const uint32_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

/* ---- public API ------------------------------------------------------ */

void pcm_ring_init(pcm_ring_t *r, int16_t *storage, uint32_t cap_frames)
{
    r->buf        = storage;
    r->cap_frames = cap_frames;
    r->wr         = 0;
    r->rd         = 0;
}

uint32_t pcm_ring_fill(const pcm_ring_t *r)
{
    uint32_t w  = ring_observe(&r->wr);
    uint32_t rd = ring_observe(&r->rd);
    return w - rd;
}

uint32_t pcm_ring_free(const pcm_ring_t *r)
{
    return r->cap_frames - pcm_ring_fill(r);
}

uint32_t pcm_ring_write(pcm_ring_t *r, const int16_t *src, uint32_t frames)
{
    uint32_t w    = ring_own(&r->wr);        /* producer owns wr           */
    uint32_t rd   = ring_observe(&r->rd);    /* acquire the consumer's rd  */
    uint32_t used = w - rd;
    uint32_t free = r->cap_frames - used;
    if (frames > free) {
        frames = free;
    }

    uint32_t mask = r->cap_frames - 1u;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = (w + i) & mask;
        r->buf[idx * 2 + 0] = src[i * 2 + 0];   /* L */
        r->buf[idx * 2 + 1] = src[i * 2 + 1];   /* R */
    }

    ring_publish(&r->wr, w + frames);        /* release-publish new wr     */
    return frames;
}

uint32_t pcm_ring_read(pcm_ring_t *r, int16_t *dst, uint32_t frames)
{
    uint32_t rd    = ring_own(&r->rd);       /* consumer owns rd           */
    uint32_t w     = ring_observe(&r->wr);   /* acquire the producer's wr  */
    uint32_t avail = w - rd;
    if (frames > avail) {
        frames = avail;
    }

    uint32_t mask = r->cap_frames - 1u;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = (rd + i) & mask;
        dst[i * 2 + 0] = r->buf[idx * 2 + 0];   /* L */
        dst[i * 2 + 1] = r->buf[idx * 2 + 1];   /* R */
    }

    ring_publish(&r->rd, rd + frames);       /* release-publish new rd     */
    return frames;
}
