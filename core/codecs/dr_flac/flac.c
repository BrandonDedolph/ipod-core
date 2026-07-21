/*
 * core/codecs/dr_flac/flac.c — FLAC decoder, wrapping dr_flac.
 *
 * dr_flac decodes natively to int32_t at the source's bit depth
 * (16/20/24-bit). We expose 16-bit signed interleaved PCM via
 * drflac_read_pcm_frames_s16, which downconverts internally with an
 * arithmetic right-shift (truncate-toward-negative-infinity, no
 * dither). That introduces a sub-LSB DC bias on 24-bit material —
 * acceptable for playback; the audio engine doesn't get to dither
 * because we ship 16-bit out anyway.
 *
 * Allocator: dr_flac exposes drflac_allocation_callbacks so we can
 * thread our static sub-allocator through cleanly. When the caller
 * passes a non-NULL decoder_alloc_t we translate; NULL falls through
 * to dr_flac's default (DRFLAC_MALLOC / DRFLAC_FREE / DRFLAC_REALLOC,
 * which are libc malloc unless we override the macros at compile time).
 *
 * Channels: we hard-cap at 2 (stereo). FLAC supports up to 8 channels;
 * the audio engine downstream only expects mono/stereo, and a 5.1
 * FLAC would silently overflow caller buffers on decode. We refuse
 * to open such streams with DECODER_ERR_UNSUPPORTED.
 */

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO          /* no fopen / fread paths */
#define DR_FLAC_NO_OGG            /* skip Ogg-FLAC for now; can re-enable */

/*
 * Freestanding build (bare-metal ARM, -DCORE_FREESTANDING): route dr_flac's
 * config hooks away from libc. Assertions compile out; the default MALLOC/
 * REALLOC/FREE become NULL/no-op because we ALWAYS pass allocation callbacks
 * (the static arena) so the defaults are never taken; memory ops go through
 * our word-optimised memcpy/memset. FLAC decode is pure integer, so no libm.
 */
#ifdef CORE_FREESTANDING
#include "../../lib/mem.h"
#define DRFLAC_ASSERT(expr)            ((void)0)
#define DRFLAC_MALLOC(sz)             ((void *)0)
#define DRFLAC_REALLOC(p, sz)         ((void *)0)
#define DRFLAC_FREE(p)                 ((void)0)
#define DRFLAC_COPY_MEMORY(dst, src, sz) memcpy((dst), (src), (sz))
#define DRFLAC_ZERO_MEMORY(p, sz)        memset((p), 0, (sz))
#endif

#include "dr_flac.h"

#include "flac.h"
#include "../decoder.h"

#include <stddef.h>
#include <stdint.h>

/* ---------- Allocator translation ---------------------------------- */

static void *flac_malloc_thunk(size_t bytes, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    return a->alloc(a->userdata, bytes);
}
static void *flac_realloc_thunk(void *ptr, size_t bytes, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    return a->realloc(a->userdata, ptr, bytes);
}
static void flac_free_thunk(void *ptr, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    a->free(a->userdata, ptr);
}

/* ---------- ops ---------------------------------------------------- */

static int flac_open(decoder_t *d, const void *src, size_t src_len,
                     const decoder_alloc_t *alloc) {
    if (!d || !src || src_len == 0) {
        return DECODER_ERR_INVALID;
    }

    drflac *f;
    if (alloc && alloc->alloc && alloc->realloc && alloc->free) {
        drflac_allocation_callbacks cb = {
            .pUserData = (void *)alloc,
            .onMalloc  = flac_malloc_thunk,
            .onRealloc = flac_realloc_thunk,
            .onFree    = flac_free_thunk,
        };
        f = drflac_open_memory(src, src_len, &cb);
    } else {
        f = drflac_open_memory(src, src_len, NULL);
    }
    if (!f) {
        /* dr_flac collapses all open failures (malformed header,
         * truncated stream, OOM) into a NULL return; we can't
         * distinguish them. */
        return DECODER_ERR_INVALID;
    }

    if (f->channels > 2) {
        drflac_close(f);
        return DECODER_ERR_UNSUPPORTED;
    }

    d->opaque          = f;
    d->sample_rate     = f->sampleRate;
    d->channels        = (uint16_t)f->channels;
    d->bits_per_sample = (uint16_t)f->bitsPerSample;
    d->total_frames    = f->totalPCMFrameCount;
    return DECODER_OK;
}

/* ---------- Streaming open (decoder_source_t -> dr_flac procs) ------ */

static size_t flac_on_read(void *ud, void *buf, size_t bytes) {
    decoder_source_t *s = (decoder_source_t *)ud;
    return s->read(s->userdata, buf, bytes);
}
static drflac_bool32 flac_on_seek(void *ud, int offset,
                                  drflac_seek_origin origin) {
    decoder_source_t *s = (decoder_source_t *)ud;
    int org;
    if (origin == DRFLAC_SEEK_SET) {
        org = DECODER_SEEK_SET;
    } else if (origin == DRFLAC_SEEK_END) {
        org = DECODER_SEEK_END;
    } else {
        org = DECODER_SEEK_CUR;
    }
    return s->seek(s->userdata, offset, org) ? DRFLAC_TRUE : DRFLAC_FALSE;
}
static drflac_bool32 flac_on_tell(void *ud, drflac_int64 *cursor) {
    decoder_source_t *s = (decoder_source_t *)ud;
    int64_t p = s->tell(s->userdata);
    if (p < 0) {
        return DRFLAC_FALSE;
    }
    *cursor = (drflac_int64)p;
    return DRFLAC_TRUE;
}

int flac_open_stream(decoder_t *d, decoder_source_t *src,
                     const decoder_alloc_t *alloc) {
    if (!d || !src || !src->read || !src->seek || !src->tell) {
        return DECODER_ERR_INVALID;
    }

    drflac_allocation_callbacks cb;
    const drflac_allocation_callbacks *pcb = NULL;
    if (alloc && alloc->alloc && alloc->realloc && alloc->free) {
        cb.pUserData = (void *)alloc;
        cb.onMalloc  = flac_malloc_thunk;
        cb.onRealloc = flac_realloc_thunk;
        cb.onFree    = flac_free_thunk;
        pcb = &cb;
    }

    drflac *f = drflac_open(flac_on_read, flac_on_seek, flac_on_tell,
                            src, pcb);
    if (!f) {
        return DECODER_ERR_INVALID;
    }
    if (f->channels > 2) {
        drflac_close(f);
        return DECODER_ERR_UNSUPPORTED;
    }

    d->opaque          = f;
    d->sample_rate     = f->sampleRate;
    d->channels        = (uint16_t)f->channels;
    d->bits_per_sample = (uint16_t)f->bitsPerSample;
    d->total_frames    = f->totalPCMFrameCount;
    d->ops             = flac_decoder_ops();
    return DECODER_OK;
}

static int flac_decode(decoder_t *d, int16_t *out, int max_frames) {
    if (!d || !d->opaque || !out || max_frames <= 0) {
        return DECODER_ERR_INVALID;
    }
    drflac *f = (drflac *)d->opaque;

    drflac_uint64 got = drflac_read_pcm_frames_s16(
        f, (drflac_uint64)max_frames, out);

    /* dr_flac returns 0 for both EOS and mid-stream decode failure;
     * we can't distinguish without inspecting state, so EOS wins.
     * If the stream is corrupt mid-decode, downstream will hear
     * silence and the position counter will drift — acceptable for
     * a music player; we don't promise glitch-free decode of broken
     * files. */
    return (int)got;
}

static int flac_seek(decoder_t *d, uint64_t target_frame) {
    if (!d || !d->opaque) return DECODER_ERR_INVALID;
    drflac *f = (drflac *)d->opaque;
    drflac_bool32 ok = drflac_seek_to_pcm_frame(f, (drflac_uint64)target_frame);
    return ok ? DECODER_OK : DECODER_ERR_INTERNAL;
}

static void flac_close(decoder_t *d) {
    if (!d) return;
    if (d->opaque) {
        drflac_close((drflac *)d->opaque);
        d->opaque = NULL;
    }
    d->ops = NULL;
}

static const decoder_ops_t FLAC_OPS = {
    .name   = "flac",
    .open   = flac_open,
    .decode = flac_decode,
    .seek   = flac_seek,
    .close  = flac_close,
};

const decoder_ops_t *flac_decoder_ops(void) {
    return &FLAC_OPS;
}
