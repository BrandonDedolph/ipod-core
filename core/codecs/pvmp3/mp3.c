/*
 * core/codecs/pvmp3/mp3.c — MP3 decoder, wrapping AOSP's pvmp3.
 *
 * pvmp3 is a FIXED-POINT MPEG-1/2/2.5 Layer III decoder (upstream/, vendored
 * from platform/frameworks/av under Apache-2.0 — see README.md). It replaced
 * dr_mp3, whose float synthesis and IMDCT cost 219 M ARMv4T instructions per
 * second of audio against pvmp3's 15.7 M: on an FPU-less ARM7TDMI that was the
 * whole reason MP3 shipped switched off. Everything here is integer.
 *
 * pvmp3's API is one frame in, one frame out — it has no notion of a file, a
 * tag, a bitrate mode or a seek. That layer is ours:
 *
 *   mp3_frame.c   framing, ID3/APE skipping, Xing/Info/VBRI, the seek map
 *   id3_meta.c    the tags themselves, into the shared flac_meta_t
 *   this file     the decoder_ops_t contract: open / decode / seek / close
 *
 * Feeding it exactly one frame per call is deliberate, not incidental:
 * pvmp3_framedecoder() begins with validate_input(), which walks the WHOLE
 * buffer it is handed parsing every header in it. With one frame that is one
 * header; with the 32 KB read-ahead window it would be O(window) on every
 * frame. Never hand it more than one frame.
 *
 * Allocation: two blocks out of the caller's arena at open() — this state and
 * pvmp3's ~21.4 KB of working memory — and none afterwards. Freed at close by
 * the arena's reset between tracks (its free() is a no-op by design).
 *
 * Gapless is out of scope. The LAME/Xing encoder delay and end padding are
 * read, but only to correct the reported LENGTH so the progress bar and the
 * host index agree; the decoded stream is not trimmed, so a tagged file plays
 * its few tens of milliseconds of encoder padding like any other decoder that
 * does not implement gapless.
 *
 * Lossy codec testing: MP3 is not bit-stable across implementations, so the
 * KAT compares against PCM captured from THIS decoder (regression protection),
 * and a separate tolerance test compares against ffmpeg's decode of real music
 * (truth). See tests/codec-vectors/README.md.
 */

#include "mp3.h"

#include "mp3_frame.h"
#include "upstream/pvmp3decoder_api.h"
#include "upstream/pvmp3_framedecoder.h"

#include <stddef.h>
#include <stdint.h>
#ifdef CORE_FREESTANDING
#include "../../lib/mem.h"       /* memcpy/memset: no libc on hw */
#else
#include <stdlib.h>              /* malloc/free fallback (sim only) */
#include <string.h>
#endif

/* Bytes of junk we will step through looking for the next frame before
 * calling the track finished. Generous enough to walk an untagged blob of
 * cover art wedged between frames, bounded so a corrupt file cannot turn
 * decode() into a whole-file scan over PIO. */
#define MP3_RESYNC_LIMIT 65536u

typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
} mp3_mem_src_t;

typedef struct {
    const decoder_alloc_t *alloc;        /* may be NULL (sim/malloc)        */
    void                  *pv_mem;       /* pvmp3_decoderMemRequirements()  */
    tPVMP3DecoderExternal  ext;
    decoder_source_t      *src;
    decoder_source_t       mem_src;      /* used only by the memory open()  */
    mp3_mem_src_t          mem;
    mp3_stream_info_t      info;
    uint64_t pos;                        /* byte offset of the next header  */
    uint16_t channels;                   /* what open() told the caller     */
    uint8_t  eos;
    int      pcm_len, pcm_pos;           /* staged frames / frames consumed */
    uint8_t  frame[MP3_MAX_FRAME_BYTES]; /* exactly one compressed frame    */
    int16_t  pcm[1152 * 2];              /* exactly one decoded frame       */
} mp3_state_t;

/* ---------- allocation --------------------------------------------------- */

static void *mp3_alloc(const decoder_alloc_t *a, size_t bytes)
{
    if (a && a->alloc) {
        return a->alloc(a->userdata, bytes);
    }
#ifdef CORE_FREESTANDING
    return NULL;            /* no libc heap on hw; the engine always supplies */
#else
    return malloc(bytes);
#endif
}

static void mp3_free(const decoder_alloc_t *a, void *p)
{
    if (!p) return;
    if (a && a->free) {
        a->free(a->userdata, p);
        return;
    }
#ifndef CORE_FREESTANDING
    free(p);
#endif
}

static void mp3_state_free(mp3_state_t *s)
{
    if (!s) return;
    const decoder_alloc_t *a = s->alloc;
    mp3_free(a, s->pv_mem);
    mp3_free(a, s);
}

/* ---------- byte source -------------------------------------------------- */

static int src_seek_abs(mp3_state_t *s, uint64_t pos)
{
    if (pos > 0x7FFFFFFFu) return 0;
    return s->src->seek(s->src->userdata, (int)pos, DECODER_SEEK_SET);
}

static int src_read_exact(mp3_state_t *s, uint8_t *buf, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        size_t got = s->src->read(s->src->userdata, buf + done, n - done);
        if (got == 0) return 0;
        done += (uint32_t)got;
    }
    return 1;
}

/* The memory open()'s source: the KAT and the host suites hand us a whole
 * file in RAM, and the streaming path is the only path. */
static size_t mem_read(void *ud, void *buf, size_t bytes)
{
    mp3_mem_src_t *m = (mp3_mem_src_t *)ud;
    size_t left = m->len - m->pos;
    if (bytes > left) bytes = left;
    if (bytes) {
        memcpy(buf, m->p + m->pos, bytes);
        m->pos += bytes;
    }
    return bytes;
}

static int mem_seek(void *ud, int offset, int origin)
{
    mp3_mem_src_t *m = (mp3_mem_src_t *)ud;
    int64_t base = (origin == DECODER_SEEK_SET) ? 0
                 : (origin == DECODER_SEEK_CUR) ? (int64_t)m->pos
                                                : (int64_t)m->len;
    int64_t want = base + offset;
    if (want < 0 || (uint64_t)want > m->len) return 0;
    m->pos = (size_t)want;
    return 1;
}

static int64_t mem_tell(void *ud)
{
    return (int64_t)((mp3_mem_src_t *)ud)->pos;
}

/* ---------- decode ------------------------------------------------------- */

/*
 * Read and decode the next frame into s->pcm. Returns the PCM frames it
 * produced, or 0 at end of stream (which includes "gave up resyncing").
 */
static int decode_one(mp3_state_t *s)
{
    uint32_t skipped = 0;

    while (!s->eos) {
        if (s->pos + 4u > s->info.audio_end) break;
        if (!src_read_exact(s, s->frame, 4u)) break;

        mp3_header_t h;
        if (!mp3_header_parse(s->frame, &h)) {
            /* Not a frame: step ONE byte and look again. Anything coarser
             * would walk past a frame that starts inside what we skipped. */
            if (++skipped > MP3_RESYNC_LIMIT) break;
            s->pos += 1u;
            if (!src_seek_abs(s, s->pos)) break;
            continue;
        }
        if (s->pos + h.frame_bytes > s->info.audio_end) break;   /* cut short */
        if (!src_read_exact(s, s->frame + 4, h.frame_bytes - 4u)) break;
        s->pos += h.frame_bytes;
        skipped = 0;

        s->ext.pInputBuffer             = s->frame;
        s->ext.inputBufferCurrentLength = (int32)h.frame_bytes;   /* ONE frame */
        s->ext.inputBufferUsedLength    = 0;
        s->ext.inputBufferMaxLength     = (int32)h.frame_bytes;
        s->ext.pOutputBuffer            = s->pcm;
        s->ext.outputFrameSize          = (int32)(sizeof s->pcm / sizeof s->pcm[0]);
        (void)pvmp3_framedecoder(&s->ext, s->pv_mem);

        /*
         * The error code is not the gate; outputFrameSize is. pvmp3 recovers
         * from a reservoir underflow or a CRC mismatch by MUTING the frame —
         * it still emits a full frame of silence and keeps its filterbank
         * history clean — and reports NO_ENOUGH_MAIN_DATA_ERROR for it. That
         * frame belongs in the stream: dropping it would shorten the track by
         * 26 ms and shift everything after it.
         */
        if (s->ext.outputFrameSize <= 0 || s->ext.num_channels <= 0) {
            continue;                       /* nothing usable: next frame */
        }
        if ((uint16_t)s->ext.num_channels != s->channels) {
            /* A stream that changes channel count mid-file would not fit the
             * caller's interleaving. Vanishingly rare; skip the frame rather
             * than write the wrong shape. */
            continue;
        }
        return (int)(s->ext.outputFrameSize / s->ext.num_channels);
    }

    s->eos = 1;
    return 0;
}

static int mp3_decode(decoder_t *d, int16_t *out, int max_frames)
{
    if (!d || !d->opaque || !out || max_frames <= 0) {
        return DECODER_ERR_INVALID;
    }
    mp3_state_t *s  = (mp3_state_t *)d->opaque;
    int          ch = (d->channels == 1u) ? 1 : 2;
    int          done = 0;

    while (done < max_frames) {
        if (s->pcm_pos >= s->pcm_len) {
            int got = decode_one(s);
            if (got <= 0) break;
            s->pcm_len = got;
            s->pcm_pos = 0;
        }
        int n = s->pcm_len - s->pcm_pos;
        if (n > max_frames - done) n = max_frames - done;
        memcpy(out + (size_t)done * (size_t)ch,
               s->pcm + (size_t)s->pcm_pos * (size_t)ch,
               (size_t)n * (size_t)ch * sizeof(int16_t));
        s->pcm_pos += n;
        done       += n;
    }
    return done;                            /* 0 = end of stream */
}

/* ---------- seek --------------------------------------------------------- */

static int mp3_seek(decoder_t *d, uint64_t target_frame)
{
    if (!d || !d->opaque) return DECODER_ERR_INVALID;
    mp3_state_t *s = (mp3_state_t *)d->opaque;

    uint64_t off = mp3_seek_offset(&s->info, target_frame);

    /*
     * Start reading EARLIER than the target. A Layer III frame's main data can
     * live up to 511 bytes back in the bit reservoir, so the frames before the
     * landing have to be decoded for it to have anything to read — pvmp3 mutes
     * a frame whose reservoir is short rather than failing, which is silence
     * where the listener expects music.
     *
     * The back-off is a CONSTANT: 511 bytes of reservoir, rounded up to 512,
     * plus four of the largest frame the format allows. Nothing about the file
     * enters it, which is the point. Every per-file figure available here is
     * either the wrong part of the file (the first frame's length, which on a
     * VBR encode that opens on silence is a quarter of the frames where the
     * listener is actually seeking to) or, without a Xing count, an
     * extrapolation FROM that same first frame — so a "mean" computed from it
     * is the first frame's length wearing a hat.
     *
     * It costs decode time on a seek, and the cost is worst where frames are
     * small: four frames at 320 kbps, about fifteen at 128 kbps, more on a low
     * bitrate — where each frame is correspondingly cheap. That is bounded and
     * paid once per scrub or resume, against a muted landing that is silence
     * the listener can hear. Tightening it means reading the header AT the
     * landing first and scaling by THAT frame's size; worth doing only if the
     * device bench says the seek is slow (STATUS.md lists it).
     */
    uint64_t back = 512u + 4u * (uint64_t)MP3_MAX_FRAME_BYTES;
    uint64_t from = (off > s->info.audio_start + back) ? (off - back)
                                                       : s->info.audio_start;

    if (!src_seek_abs(s, from)) return DECODER_ERR_INTERNAL;
    s->pos     = from;
    s->pcm_len = 0;
    s->pcm_pos = 0;
    s->eos     = 0;
    pvmp3_resetDecoder(s->pv_mem);

    /*
     * Walk to the frame that covers the target, discarding what we decode.
     * That both resyncs to a real frame boundary and warms the reservoir:
     * the first frame after a reset has no history and pvmp3 mutes it, so it
     * must never be the one the listener hears.
     */
    while (s->pos < off) {
        if (decode_one(s) <= 0) break;
    }
    s->pcm_len = 0;
    s->pcm_pos = 0;

    /*
     * Landing past the last frame is NOT a failure. decode() then returns 0
     * and the player treats it exactly as a track that played out — which is
     * what a scrub to the very end should do. Reporting an error would send
     * the player's recovery path back to 0:00 instead.
     */
    return DECODER_OK;
}

/* ---------- open / close ------------------------------------------------- */

static void mp3_close(decoder_t *d)
{
    if (!d) return;
    if (d->opaque) {
        mp3_state_free((mp3_state_t *)d->opaque);
        d->opaque = NULL;
    }
    d->ops = NULL;
}

/*
 * Rates the WM8758B cannot be clocked at (hal/hal.h reaches 44100, 48000,
 * 32000, 24000 and 22050). MPEG-2's 16 kHz and all three MPEG-2.5 rates are
 * below that floor. Refusing them at open is the honest answer: resampling in
 * software on this CPU is exactly the cost that parked MP3 in the first place,
 * and playing them at the wrong rate would be a chipmunk podcast.
 */
static int rate_is_playable(uint32_t rate)
{
    return rate == 44100u || rate == 48000u || rate == 32000u ||
           rate == 24000u || rate == 22050u;
}

static int mp3_open_common(decoder_t *d, mp3_state_t *s)
{
    if (mp3_stream_scan(s->src, &s->info) != 0) {
        return DECODER_ERR_INVALID;         /* no Layer III frame in it */
    }
    if (!rate_is_playable(s->info.first.sample_rate)) {
        return DECODER_ERR_UNSUPPORTED;
    }

    s->pv_mem = mp3_alloc(s->alloc, pvmp3_decoderMemRequirements());
    if (!s->pv_mem) return DECODER_ERR_INTERNAL;

    s->ext.equalizerType = flat;            /* bass/treble live in the WM8758 */
    s->ext.crcEnabled    = 0;               /* a CRC mismatch would only mute */
    pvmp3_InitDecoder(&s->ext, s->pv_mem);

    s->pos      = s->info.audio_start;
    s->channels = s->info.first.channels;
    if (!src_seek_abs(s, s->pos)) return DECODER_ERR_INTERNAL;

    d->opaque          = s;
    d->sample_rate     = s->info.first.sample_rate;
    d->channels        = s->channels;
    d->bits_per_sample = 16;                /* pvmp3 emits s16 directly */
    d->total_frames    = s->info.total_frames;
    d->ops             = mp3_decoder_ops();
    return DECODER_OK;
}

static mp3_state_t *mp3_state_new(const decoder_alloc_t *alloc)
{
    mp3_state_t *s = (mp3_state_t *)mp3_alloc(alloc, sizeof(mp3_state_t));
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->alloc = alloc;
    return s;
}

int mp3_open_stream(decoder_t *d, decoder_source_t *src,
                    const decoder_alloc_t *alloc)
{
    if (!d || !src || !src->read || !src->seek || !src->tell) {
        return DECODER_ERR_INVALID;
    }
    mp3_state_t *s = mp3_state_new(alloc);
    if (!s) return DECODER_ERR_INTERNAL;

    s->src = src;                           /* borrowed: it must outlive us */
    int rc = mp3_open_common(d, s);
    if (rc != DECODER_OK) mp3_state_free(s);
    return rc;
}

static int mp3_open(decoder_t *d, const void *src, size_t src_len,
                    const decoder_alloc_t *alloc)
{
    if (!d || !src || src_len == 0) {
        return DECODER_ERR_INVALID;
    }
    mp3_state_t *s = mp3_state_new(alloc);
    if (!s) return DECODER_ERR_INTERNAL;

    /* A whole file in RAM is just a source that never blocks. Wrapping it
     * keeps ONE decode path: whatever the KAT proves, the device runs. */
    s->mem.p          = (const uint8_t *)src;
    s->mem.len        = src_len;
    s->mem.pos        = 0;
    s->mem_src.read   = mem_read;
    s->mem_src.seek   = mem_seek;
    s->mem_src.tell   = mem_tell;
    s->mem_src.userdata = &s->mem;
    s->src            = &s->mem_src;

    int rc = mp3_open_common(d, s);
    if (rc != DECODER_OK) mp3_state_free(s);
    return rc;
}

static const decoder_ops_t MP3_OPS = {
    .name   = "mp3",
    .open   = mp3_open,
    .decode = mp3_decode,
    .seek   = mp3_seek,
    .close  = mp3_close,
};

const decoder_ops_t *mp3_decoder_ops(void)
{
    return &MP3_OPS;
}
