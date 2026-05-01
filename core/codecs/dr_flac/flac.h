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

#endif /* CORE_CODECS_FLAC_H */
