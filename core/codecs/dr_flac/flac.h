/*
 * core/codecs/dr_flac/flac.h — FLAC decoder ops, vendored dr_flac under the hood.
 */

#ifndef CORE_CODECS_FLAC_H
#define CORE_CODECS_FLAC_H

#include "../decoder.h"

/*
 * Returns the FLAC decoder ops singleton. Implementation lives in
 * flac.c; uses dr_flac.h to do the actual decoding.
 */
const decoder_ops_t *flac_decoder_ops(void);

/*
 * Streaming open: decode directly from a pull byte source (`src`) instead of
 * an in-RAM buffer, so a multi-MB FLAC never has to be fully resident. On
 * success populates `d` (metadata + opaque drflac handle) and returns
 * DECODER_OK; thereafter drive it with the ops from flac_decoder_ops()
 * (decode/seek/close) — they operate on `d` regardless of how it was opened.
 * `alloc` must be non-NULL on hw (backs dr_flac's internal buffers).
 */
int flac_open_stream(decoder_t *d, decoder_source_t *src,
                     const decoder_alloc_t *alloc);

/*
 * ReplayGain: apply `db_q8` (gain in 1/256 dB, e.g. -1864 for "-7.28 dB") as a
 * digital pre-scale on everything decode() emits from now on.
 *
 * `peak_q16` is the file's REPLAYGAIN_*_PEAK, linear in Q16 (65536 = full
 * scale), and it CAPS the gain: the scale is held to full_scale / peak, so
 * the loudest sample in the file lands at 0 dBFS and never beyond. That is
 * the ReplayGain spec's "prevent clipping" rule, and it is what stops a
 * positive gain from being hard clipping. Pass 0 when the file has no peak
 * tag: a positive gain is then treated as if the peak were full scale — no
 * boost, because nothing says a boost is safe — and a negative gain applies
 * as tagged. The int16 saturate after the multiply still exists, but only as
 * a backstop for rounding at the cap; with the cap it is never the thing
 * shaping the sound. It used to be: the gain was applied uncapped and
 * "clipping handled by saturating", which on 34 tracks of the library
 * (positive gain, peak 1.0) was audible crunch on every peak (2026-09-24).
 *
 * Returns the gain ACTUALLY in force, in 1/256 dB (the tagged value, or the
 * cap when the cap was lower), so the caller can report it.
 *
 * Call AFTER open, only when the file actually carries a REPLAYGAIN_* tag —
 * a 0 dB gain is unity, so an untagged file must not call this at all (that
 * keeps the decode path bit-identical to the no-ReplayGain build, which the
 * codec KATs assert).
 *
 * The setting is per-open and global to the wrapper: only one FLAC decoder is
 * ever live in this firmware, and every open() resets it to unity.
 */
int flac_set_gain_db_q8(decoder_t *d, int db_q8, uint32_t peak_q16);

#endif /* CORE_CODECS_FLAC_H */
