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

#endif /* CORE_CODECS_FLAC_H */
