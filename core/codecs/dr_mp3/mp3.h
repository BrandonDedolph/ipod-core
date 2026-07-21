/*
 * core/codecs/dr_mp3/mp3.h — MP3 decoder ops, vendored dr_mp3 under the hood.
 */

#ifndef CORE_CODECS_MP3_H
#define CORE_CODECS_MP3_H

#include "../decoder.h"

/*
 * Returns the MP3 decoder ops singleton. Implementation lives in
 * mp3.c; uses dr_mp3.h to do the actual decoding.
 */
const decoder_ops_t *mp3_decoder_ops(void);

/*
 * Streaming open: decode directly from a pull byte source (`src`) instead of
 * an in-RAM buffer, so a multi-MB MP3 never has to be fully resident. On
 * success populates `d` (metadata + opaque drmp3 handle) and returns
 * DECODER_OK; thereafter drive it with the ops from mp3_decoder_ops()
 * (decode/seek/close) — they operate on `d` regardless of how it was opened.
 * `alloc` must be non-NULL on hw (backs dr_mp3's internal buffers).
 */
int mp3_open_stream(decoder_t *d, decoder_source_t *src,
                    const decoder_alloc_t *alloc);

#endif /* CORE_CODECS_MP3_H */
