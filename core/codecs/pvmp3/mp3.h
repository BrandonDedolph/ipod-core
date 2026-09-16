/*
 * core/codecs/pvmp3/mp3.h — MP3 decoder ops, vendored pvmp3 under the hood.
 */

#ifndef CORE_CODECS_MP3_H
#define CORE_CODECS_MP3_H

#include "../decoder.h"

/*
 * Returns the MP3 decoder ops singleton. Implementation lives in mp3.c; the
 * fixed-point frame decoder is AOSP's pvmp3 (upstream/, Apache-2.0).
 */
const decoder_ops_t *mp3_decoder_ops(void);

/*
 * Streaming open: decode directly from a pull byte source (`src`) instead of
 * an in-RAM buffer, so a multi-MB MP3 never has to be fully resident. On
 * success populates `d` (metadata + opaque decoder state) and returns
 * DECODER_OK; thereafter drive it with the ops from mp3_decoder_ops()
 * (decode/seek/close) — they operate on `d` regardless of how it was opened.
 * `alloc` must be non-NULL on hw: it backs two blocks taken at open and never
 * grown — pvmp3's 21.4 KB of working memory and this wrapper's ~6.9 KB of
 * frame and PCM staging, about 28 KB of arena in total.
 *
 * Returns DECODER_ERR_UNSUPPORTED for a stream the DAC cannot clock: MPEG-2.5
 * (8/11.025/12 kHz) and MPEG-2's 16 kHz, none of which the WM8758B reaches.
 * DECODER_ERR_INVALID means no MPEG Layer III frame was found at all.
 */
int mp3_open_stream(decoder_t *d, decoder_source_t *src,
                    const decoder_alloc_t *alloc);

#endif /* CORE_CODECS_MP3_H */
