/*
 * core/apps/audio/engine.c — playback engine implementation.
 *
 * SPSC ring; producer is audio_engine_pump (called from control
 * thread); consumer is engine_fill_cb (called from HAL audio
 * callback thread).
 *
 * Index discipline:
 *   write_idx: total frames the producer has written, monotonic uint32.
 *   read_idx:  total frames the consumer has consumed, monotonic uint32.
 *   buffered = write_idx - read_idx                  (mod 2^32; wraps cleanly)
 *   free     = AUDIO_RING_FRAMES - buffered
 *   The actual array index = idx & (AUDIO_RING_FRAMES - 1) since the
 *   capacity is a power of two.
 *
 * Ordering (matters on dual-core PP5022; no-op on x86 TSO sim):
 *   Producer publishes write_idx with __ATOMIC_RELEASE after writing
 *   ring slots; consumer reads write_idx with __ATOMIC_ACQUIRE before
 *   reading ring slots. Same pattern in reverse for read_idx. The
 *   release/acquire pair gives the consumer a happens-before view of
 *   every ring store the producer committed up to that index.
 *
 * No malloc on the hot path. play() allocates one buffer of `len`
 * bytes for the source-file copy; stop() frees it. Decoder may
 * malloc internally on open() — when we land the audio engine on
 * hw, the caller will pass a non-NULL decoder_alloc_t backed by our
 * static sub-allocator.
 */

#include "engine.h"
#include "../../codecs/decoder.h"
#include "../../hal/hal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Power-of-two capacity → wrap by mask. */
#define RING_MASK (AUDIO_RING_FRAMES - 1u)

/* ---------- SPSC index ordering helpers ---------------------------- */

/* Each side publishes its own index with release ordering. */
static inline void ring_publish(uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/* Reading the *other* side's index uses acquire ordering. */
static inline uint32_t ring_observe(const uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

/* Same-thread reads of one's own index. Relaxed is sufficient because
 * a thread always sees its own writes program-order. */
static inline uint32_t ring_own(const uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

/* ---------- HAL fill callback (consumer side) -------------------- */

static int engine_fill_cb(void *user, int16_t *out, int frames) {
    audio_engine_t *e = (audio_engine_t *)user;

    /* Acquire-load write_idx so all ring stores up to that point are
     * visible to us before we read them. */
    uint32_t w = ring_observe(&e->write_idx);
    uint32_t r = ring_own(&e->read_idx);
    uint32_t avail = (uint32_t)(w - r);
    if (avail > (uint32_t)frames) avail = (uint32_t)frames;

    int wrote = 0;
    while (wrote < (int)avail) {
        uint32_t idx = (r + wrote) & RING_MASK;
        out[wrote * 2 + 0] = e->ring[idx * 2 + 0];
        out[wrote * 2 + 1] = e->ring[idx * 2 + 1];
        wrote++;
    }

    /* Release-publish read_idx exactly once at the end. The producer
     * may read a stale (smaller) value mid-update; that just means it
     * sees the ring as fuller than it really is and produces less.
     * Always conservative, never corrupting. */
    ring_publish(&e->read_idx, r + (uint32_t)wrote);
    return wrote;
}

/* ---------- Lifecycle --------------------------------------------- */

void audio_engine_init(audio_engine_t *e) {
    memset(e, 0, sizeof(*e));
}

void audio_engine_close(audio_engine_t *e) {
    audio_engine_stop(e);
}

/* ---------- Internal: tear down decoder state ---------------------- */

static void engine_release_decoder(audio_engine_t *e) {
    if (e->has_decoder && e->ops) {
        e->ops->close(&e->decoder);
    }
    if (e->src_bytes) {
        free(e->src_bytes);
        e->src_bytes = NULL;
    }
    e->src_len      = 0;
    e->has_decoder  = false;
    e->decoder_eof  = false;
    e->ops          = NULL;
    memset(&e->decoder, 0, sizeof(e->decoder));
}

/* ---------- Play / pause / resume / stop --------------------------- */

int audio_engine_play(audio_engine_t *e,
                      const decoder_ops_t *ops,
                      const void *bytes, size_t len) {
    if (!e || !ops || !bytes || len == 0) return -1;

    /* If something's playing, stop cleanly first so we don't leak. */
    if (e->has_decoder) {
        audio_engine_stop(e);
    }

    /* Take ownership of a copy of the source bytes — the decoder
     * needs them stable for its lifetime. */
    e->src_bytes = malloc(len);
    if (!e->src_bytes) return -2;
    memcpy(e->src_bytes, bytes, len);
    e->src_len = len;

    int rc = ops->open(&e->decoder, e->src_bytes, len, /*alloc=*/NULL);
    if (rc != DECODER_OK) {
        rc = -3;
        goto fail_after_alloc;
    }
    e->ops          = ops;
    e->has_decoder  = true;
    e->decoder_eof  = false;
    e->sample_rate  = e->decoder.sample_rate;
    e->channels     = e->decoder.channels;

    /* Reset the ring before any concurrent observer sees us — at this
     * point set_source(NULL) was called by the prior stop(), so the
     * HAL won't call back into us. */
    e->write_idx = 0;
    e->read_idx  = 0;

    /* Configure the HAL output for this stream. We always emit
     * stereo from the engine, regardless of source channel count
     * (mono gets duplicated in the pump). */
    if (hal_audio_init(e->sample_rate, /*channels=*/2) != 0) {
        rc = -4;
        goto fail_after_decoder;
    }

    hal_audio_set_source(engine_fill_cb, e);
    hal_audio_start();
    e->playing = true;
    return 0;

fail_after_decoder:
    /* Decoder is open; close it before unwinding the source buffer. */
    ops->close(&e->decoder);
    e->ops          = NULL;
    e->has_decoder  = false;
    memset(&e->decoder, 0, sizeof(e->decoder));
    /* fall through */
fail_after_alloc:
    free(e->src_bytes);
    e->src_bytes = NULL;
    e->src_len   = 0;
    return rc;
}

void audio_engine_pause(audio_engine_t *e) {
    if (!e->playing) return;
    hal_audio_stop();
    e->playing = false;
}

void audio_engine_resume(audio_engine_t *e) {
    if (e->playing) return;
    if (!e->has_decoder) return;
    hal_audio_start();
    e->playing = true;
}

void audio_engine_stop(audio_engine_t *e) {
    if (e->playing) {
        hal_audio_stop();
        e->playing = false;
    }
    /* Detach the source before tearing down — the HAL's quiescence
     * guarantee (PR 8) means no callback can observe stale state
     * after this returns. */
    hal_audio_set_source(NULL, NULL);
    engine_release_decoder(e);
    /* Reset indices as plain stores — there's no concurrent observer
     * after set_source(NULL) returned. */
    e->write_idx = 0;
    e->read_idx  = 0;
}

/* ---------- Pump (producer side) ----------------------------------- */

int audio_engine_pump(audio_engine_t *e) {
    if (!e->has_decoder || e->decoder_eof) return 0;

    /* Same-thread read of write_idx; acquire-read of read_idx so we
     * see how far the consumer has drained. */
    uint32_t w = ring_own(&e->write_idx);
    uint32_t r = ring_observe(&e->read_idx);
    uint32_t free_frames = AUDIO_RING_FRAMES - (uint32_t)(w - r);
    if (free_frames == 0) return 0;

    /* Decode in two passes if we straddle the ring's wrap point.
     * First pass: from write_idx to end-of-array. Second: from
     * start-of-array to read_idx (if needed). */
    int total = 0;
    while (free_frames > 0 && !e->decoder_eof) {
        uint32_t w_pos     = w & RING_MASK;
        uint32_t to_end    = AUDIO_RING_FRAMES - w_pos;
        uint32_t this_chunk = (free_frames < to_end) ? free_frames : to_end;
        if (this_chunk > 4096) this_chunk = 4096;   /* keep batches modest */

        int16_t *dst = &e->ring[w_pos * 2];
        int got;

        if (e->channels == 2) {
            got = e->ops->decode(&e->decoder, dst, (int)this_chunk);
        } else {
            /* Mono source: decode into a small staging buffer, then
             * splat L=R into the engine's stereo ring. */
            int16_t mono[1024];
            int batch = (this_chunk < 1024) ? (int)this_chunk : 1024;
            got = e->ops->decode(&e->decoder, mono, batch);
            assert(got <= batch);  /* would scribble past dst otherwise */
            for (int i = 0; i < got; i++) {
                dst[i * 2 + 0] = mono[i];
                dst[i * 2 + 1] = mono[i];
            }
        }

        if (got <= 0) {
            /* 0 = EOS; < 0 = decoder error. Both end the stream. */
            e->decoder_eof = true;
            break;
        }
        w           += (uint32_t)got;
        free_frames -= (uint32_t)got;
        total       += got;

        /* Publish progress so the consumer can drain even mid-batch. */
        ring_publish(&e->write_idx, w);
    }
    return total;
}

/* ---------- Status ------------------------------------------------- */

bool audio_engine_eos(const audio_engine_t *e) {
    /* Acquire on read_idx so we see the consumer's latest progress. */
    uint32_t w = ring_own((const uint32_t *)&e->write_idx);
    uint32_t r = ring_observe((const uint32_t *)&e->read_idx);
    return e->decoder_eof && (w == r);
}

bool audio_engine_is_playing(const audio_engine_t *e) {
    return e->playing;
}

uint32_t audio_engine_ring_fill(const audio_engine_t *e) {
    uint32_t w = ring_own((const uint32_t *)&e->write_idx);
    uint32_t r = ring_observe((const uint32_t *)&e->read_idx);
    return (uint32_t)(w - r);
}
