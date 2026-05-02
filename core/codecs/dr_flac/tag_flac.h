/*
 * core/codecs/dr_flac/tag_flac.h — Vorbis-comment reader for FLAC.
 *
 * Reads TITLE / ARTIST / ALBUM out of a FLAC file's VORBIS_COMMENT
 * metadata block. Doesn't decode audio. Doesn't depend on the
 * decoder_t ABI — caller passes the file's bytes; we use dr_flac's
 * `with_metadata` open path to read the metadata blocks, then close
 * immediately.
 *
 * Why a separate module from flac.c (the decoder wrapper): the audio
 * engine doesn't need tags, so flac.c uses the cheaper `drflac_open_memory`
 * path that skips the metadata callback. The tag reader threads its
 * own onMeta through the more expensive `_with_metadata` open. Two
 * different responsibilities, two different callers.
 */

#ifndef CORE_CODECS_DR_FLAC_TAG_FLAC_H
#define CORE_CODECS_DR_FLAC_TAG_FLAC_H

#include "../tags.h"

#include <stddef.h>

/*
 * Read Vorbis comments from `bytes` (a complete FLAC file in memory)
 * into `*out`. Zeroes `*out` first, then fills any of TITLE / ARTIST /
 * ALBUM that are present (case-insensitive on the key).
 *
 * Returns:
 *    0 on success (even if no tags found — check the found_* flags).
 *   -1 if dr_flac couldn't open the input (truncated, malformed, etc).
 */
int tag_flac_read(const void *bytes, size_t len, audio_tags_t *out);

#endif /* CORE_CODECS_DR_FLAC_TAG_FLAC_H */
