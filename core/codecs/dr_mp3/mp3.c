/*
 * core/codecs/dr_mp3/mp3.c — MP3 decoder, wrapping dr_mp3.
 *
 * dr_mp3 decodes natively to int16_t PCM (no extra downconversion
 * step needed, unlike dr_flac which has to drop bits for >16-bit
 * source). It uses one allocation at init for the bitstream / IMDCT
 * working memory and (in our config) doesn't allocate on the
 * decode hot path.
 *
 * Allocator: dr_mp3 mirrors dr_flac's drmp3_allocation_callbacks
 * struct. Same translation pattern as flac.c. When the caller passes
 * NULL for decoder_alloc_t, we fall through to libc malloc — fine on
 * sim; on hw (no malloc) the caller MUST pass a non-NULL struct
 * backed by the audio engine's static sub-allocator, otherwise dr_mp3
 * will silently fail at its first internal alloc.
 *
 * Channels: dr_mp3 only ever emits mono or stereo (MP3 doesn't have
 * higher channel counts in the formats we care about), so no guard
 * needed beyond the "channels > 2" check we apply to all wrappers
 * for consistency.
 *
 * Lossy codec testing: MP3 isn't bit-stable across decoder
 * implementations, so the KAT compares against PCM captured *from
 * dr_mp3 itself* on first generation, not against external truth.
 * That gives us regression protection (any change to dr_mp3 or our
 * wrapper will trip the KAT) without requiring a perfect oracle.
 */

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO           /* no fopen / fread paths */
#include "dr_mp3.h"

#include "mp3.h"
#include "../decoder.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Allocator translation ---------------------------------- */

static void *mp3_malloc_thunk(size_t bytes, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    return a->alloc(a->userdata, bytes);
}
static void *mp3_realloc_thunk(void *ptr, size_t bytes, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    return a->realloc(a->userdata, ptr, bytes);
}
static void mp3_free_thunk(void *ptr, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    a->free(a->userdata, ptr);
}

/* ---------- ops ---------------------------------------------------- */

/*
 * dr_mp3 wants its own drmp3 struct held by the caller (it doesn't
 * heap-allocate the struct itself — the caller does). We wrap it in
 * a small holder so decoder_t.opaque has somewhere to live and so
 * we can free it on close().
 */
typedef struct {
    drmp3                    decoder;
    const decoder_alloc_t   *alloc;       /* may be NULL */
    drmp3_allocation_callbacks cb_storage; /* if alloc != NULL */
} mp3_state_t;

static void *mp3_state_alloc(const decoder_alloc_t *alloc) {
    if (alloc && alloc->alloc) {
        return alloc->alloc(alloc->userdata, sizeof(mp3_state_t));
    }
    return malloc(sizeof(mp3_state_t));
}

static void mp3_state_free(mp3_state_t *s) {
    if (!s) return;
    if (s->alloc && s->alloc->free) {
        s->alloc->free(s->alloc->userdata, s);
    } else {
        free(s);
    }
}

static int mp3_open(decoder_t *d, const void *src, size_t src_len,
                    const decoder_alloc_t *alloc) {
    if (!d || !src || src_len == 0) {
        return DECODER_ERR_INVALID;
    }

    mp3_state_t *s = mp3_state_alloc(alloc);
    if (!s) return DECODER_ERR_INTERNAL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;

    drmp3_allocation_callbacks *pcb = NULL;
    if (alloc && alloc->alloc && alloc->realloc && alloc->free) {
        s->cb_storage.pUserData = (void *)alloc;
        s->cb_storage.onMalloc  = mp3_malloc_thunk;
        s->cb_storage.onRealloc = mp3_realloc_thunk;
        s->cb_storage.onFree    = mp3_free_thunk;
        pcb = &s->cb_storage;
    }

    if (!drmp3_init_memory(&s->decoder, src, src_len, pcb)) {
        mp3_state_free(s);
        return DECODER_ERR_INVALID;
    }

    if (s->decoder.channels > 2) {
        drmp3_uninit(&s->decoder);
        mp3_state_free(s);
        return DECODER_ERR_UNSUPPORTED;
    }

    /* total_frames: cheap when the file has a Xing/Info LAME tag (the
     * vast majority of MP3s in the wild) — dr_mp3 already cached the
     * count during init. For tagless files this scans the stream to
     * count frames, then seeks back to frame 0; that scan is O(file)
     * but the bytes are already in memory at this point so no I/O.
     * Worst case is ~tens of ms per untagged track, paid once at play
     * time. The UI's progress bar and remaining-time label require it.
     *
     * A future tagcache build pass should compute and cache this
     * offline so we never scan on-device, but the on-the-fly path
     * keeps the firmware honest in the meantime. */
    drmp3_uint64 pcm_frames = drmp3_get_pcm_frame_count(&s->decoder);

    d->opaque          = s;
    d->sample_rate     = s->decoder.sampleRate;
    d->channels        = (uint16_t)s->decoder.channels;
    d->bits_per_sample = 16;          /* dr_mp3 emits s16 directly */
    d->total_frames    = (uint64_t)pcm_frames;
    return DECODER_OK;
}

static int mp3_decode(decoder_t *d, int16_t *out, int max_frames) {
    if (!d || !d->opaque || !out || max_frames <= 0) {
        return DECODER_ERR_INVALID;
    }
    mp3_state_t *s = (mp3_state_t *)d->opaque;

    drmp3_uint64 got = drmp3_read_pcm_frames_s16(
        &s->decoder, (drmp3_uint64)max_frames, out);

    return (int)got;   /* 0 on EOS, > 0 on success */
}

static int mp3_seek(decoder_t *d, uint64_t target_frame) {
    if (!d || !d->opaque) return DECODER_ERR_INVALID;
    mp3_state_t *s = (mp3_state_t *)d->opaque;
    drmp3_bool32 ok = drmp3_seek_to_pcm_frame(
        &s->decoder, (drmp3_uint64)target_frame);
    return ok ? DECODER_OK : DECODER_ERR_INTERNAL;
}

static void mp3_close(decoder_t *d) {
    if (!d) return;
    if (d->opaque) {
        mp3_state_t *s = (mp3_state_t *)d->opaque;
        drmp3_uninit(&s->decoder);
        mp3_state_free(s);
        d->opaque = NULL;
    }
    d->ops = NULL;
}

static const decoder_ops_t MP3_OPS = {
    .name   = "mp3",
    .open   = mp3_open,
    .decode = mp3_decode,
    .seek   = mp3_seek,
    .close  = mp3_close,
};

const decoder_ops_t *mp3_decoder_ops(void) {
    return &MP3_OPS;
}
