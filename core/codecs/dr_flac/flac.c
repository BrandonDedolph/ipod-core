/*
 * core/codecs/dr_flac/flac.c — FLAC decoder, wrapping dr_flac.
 *
 * dr_flac decodes natively to int32_t PCM at the file's actual bit
 * depth (16/20/24-bit). We expose s16 to the audio engine, so the
 * wrapper does an arithmetic downshift when bps > 16.
 *
 * The decoder_t.opaque field holds a drflac* directly. dr_flac uses
 * malloc/free internally (default DRFLAC_MALLOC); for the firmware
 * target we'll later override with our static sub-allocator via
 * DRFLAC_MALLOC / DRFLAC_FREE macros. On the host (sim, KAT) the
 * default allocator is fine.
 */

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO          /* no fopen / fread paths */
#define DR_FLAC_NO_OGG            /* skip Ogg-FLAC for now; can re-enable */
#include "dr_flac.h"

#include "flac.h"
#include "../decoder.h"

#include <stddef.h>
#include <stdint.h>

static int flac_open(decoder_t *d, const void *src, size_t src_len) {
    if (!d || !src || src_len == 0) {
        return DECODER_ERR_INVALID;
    }
    drflac *f = drflac_open_memory(src, src_len, NULL);
    if (!f) {
        return DECODER_ERR_INVALID;
    }

    d->opaque          = f;
    d->sample_rate     = f->sampleRate;
    d->channels        = (uint16_t)f->channels;
    d->bits_per_sample = (uint16_t)f->bitsPerSample;
    d->total_frames    = f->totalPCMFrameCount;
    return DECODER_OK;
}

static int flac_decode(decoder_t *d, int16_t *out, int max_frames) {
    if (!d || !d->opaque || !out || max_frames <= 0) {
        return DECODER_ERR_INVALID;
    }
    drflac *f = (drflac *)d->opaque;

    /* dr_flac has a direct s16 reader — saves us the manual downshift. */
    drflac_uint64 got = drflac_read_pcm_frames_s16(
        f, (drflac_uint64)max_frames, out);

    if (got > (drflac_uint64)INT32_MAX) {
        /* Shouldn't happen given max_frames is int, but guard the cast. */
        return DECODER_ERR_INTERNAL;
    }
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
