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
 * No malloc on the hot path. play() allocates one buffer of `len`
 * bytes for the source file copy; stop() frees it. Decoder may
 * malloc internally on open() — when we land the audio engine on
 * hw, the caller will pass a non-NULL decoder_alloc_t backed by our
 * static sub-allocator.
 */

#include "engine.h"
#include "../../codecs/decoder.h"
#include "../../hal/hal.h"

#include <stdlib.h>
#include <string.h>

/* Power-of-two capacity → wrap by mask. */
#define RING_MASK (AUDIO_RING_FRAMES - 1u)

/* ---------- Ring helpers (SPSC) ----------------------------------- */

/* Frames currently buffered. Consumer-side count: read by the producer
 * to decide if there's space to write; read by the consumer to decide
 * if there's data to drain. Both sides treat the indices as volatile;
 * a stale read just means "I'll act on slightly outdated info" which
 * for SPSC is always conservative. */
static inline uint32_t ring_used(const audio_engine_t *e) {
    return (uint32_t)(e->write_idx - e->read_idx);
}
static inline uint32_t ring_free(const audio_engine_t *e) {
    return AUDIO_RING_FRAMES - ring_used(e);
}

/* ---------- HAL fill callback (consumer side) -------------------- */

static int engine_fill_cb(void *user, int16_t *out, int frames) {
    audio_engine_t *e = (audio_engine_t *)user;

    int wrote = 0;
    uint32_t r = e->read_idx;
    uint32_t w = e->write_idx;
    uint32_t avail = (uint32_t)(w - r);
    if (avail > (uint32_t)frames) avail = (uint32_t)frames;

    while (wrote < (int)avail) {
        uint32_t idx = (r + wrote) & RING_MASK;
        out[wrote * 2 + 0] = e->ring[idx * 2 + 0];
        out[wrote * 2 + 1] = e->ring[idx * 2 + 1];
        wrote++;
    }

    /* Bulk update read_idx exactly once at the end. The producer may
     * read a stale (smaller) value mid-update; that just means it
     * sees the ring as fuller than it really is and produces less.
     * Always conservative, never corrupting. */
    e->read_idx = r + (uint32_t)wrote;
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
        free(e->src_bytes);
        e->src_bytes = NULL;
        e->src_len = 0;
        return -3;
    }
    e->ops          = ops;
    e->has_decoder  = true;
    e->decoder_eof  = false;
    e->sample_rate  = e->decoder.sample_rate;
    e->channels     = e->decoder.channels;

    /* Reset the ring. Safe — we know nothing else is using it (we
     * either just initialized, or just stopped which clears these). */
    e->write_idx = 0;
    e->read_idx  = 0;

    /* Configure the HAL output for this stream. We always emit
     * stereo from the engine, regardless of source channel count
     * (mono gets duplicated in the pump). */
    if (hal_audio_init(e->sample_rate, /*channels=*/2) != 0) {
        engine_release_decoder(e);
        return -4;
    }

    hal_audio_set_source(engine_fill_cb, e);
    hal_audio_start();
    e->playing = true;
    return 0;
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
    /* Detach the source before tearing down — quiescence guarantee
     * means the next callback definitely won't observe stale state. */
    hal_audio_set_source(NULL, NULL);
    engine_release_decoder(e);
    e->write_idx = 0;
    e->read_idx  = 0;
}

/* ---------- Pump (producer side) ----------------------------------- */

int audio_engine_pump(audio_engine_t *e) {
    if (!e->has_decoder || e->decoder_eof) return 0;

    uint32_t free_frames = ring_free(e);
    if (free_frames == 0) return 0;

    /* Decode in two passes if we straddle the ring's wrap point.
     * First pass: from write_idx to end-of-array. Second: from
     * start-of-array to read_idx (if needed). */
    int total = 0;
    while (free_frames > 0 && !e->decoder_eof) {
        uint32_t w_pos     = e->write_idx & RING_MASK;
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
        e->write_idx += (uint32_t)got;
        free_frames  -= (uint32_t)got;
        total        += got;

        /* If decoder gave us less than asked, it might be at a frame
         * boundary; loop again with fresh free_frames count and try
         * to pull more this same call. */
    }
    return total;
}

/* ---------- Status ------------------------------------------------- */

bool audio_engine_eos(const audio_engine_t *e) {
    return e->decoder_eof && (e->write_idx == e->read_idx);
}

bool audio_engine_is_playing(const audio_engine_t *e) {
    return e->playing;
}

uint32_t audio_engine_ring_fill(const audio_engine_t *e) {
    return ring_used(e);
}
