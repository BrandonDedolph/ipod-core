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

#endif /* CORE_CODECS_MP3_H */
