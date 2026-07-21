/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/flac_meta.h — FLAC tag/duration reader (metadata only).
 *
 * Reads a FLAC file's METADATA_BLOCKs up front WITHOUT decoding any audio,
 * so the UI can show real track info (title/artist/album/genre/track#/year
 * + duration) instead of guessing from the folder name. It pulls bytes
 * through a decoder_source_t, so it composes with the same fat_src +
 * read-ahead path the streaming decoder uses (see codecs/dr_flac/flac.c).
 *
 * Two blocks carry what we want:
 *   - STREAMINFO (type 0): sample_rate + total_samples -> duration_s.
 *   - VORBIS_COMMENT (type 4): "KEY=value" tags. Little-endian, unlike the
 *     big-endian block headers.
 * Everything else (SEEKTABLE, PADDING, PICTURE, ...) is skipped. Parsing
 * stops at the block whose header sets the last-block flag.
 *
 * Freestanding-clean: no libc/libm/malloc, no allocation — all buffers are
 * on the caller's stack (the flac_meta_t) or small locals. Strings are
 * bounded and copied as printable ASCII 0x20..0x7E only, dropping UTF-8
 * multibyte bytes to match the atlas font's coverage (same policy as
 * kernel/main.c's copy_display_name).
 */
#ifndef CORE_CODECS_FLAC_META_H
#define CORE_CODECS_FLAC_META_H

#include <stdint.h>

#include "decoder.h"

typedef struct {
    int      have;          /* 1 if the file parsed as FLAC (STREAMINFO seen) */
    uint32_t duration_s;    /* total_samples / sample_rate, 0 if unknown      */
    uint32_t sample_rate;   /* Hz, from STREAMINFO (0 if unknown)             */
    char     title[64];     /* NUL-terminated, printable-ASCII, truncated     */
    char     artist[64];    /* prefers ARTIST, falls back to ALBUMARTIST      */
    char     album[64];
    char     genre[32];
    int      track;         /* TRACKNUMBER as int, 0 if absent                */
    int      year;          /* DATE/YEAR as int, 0 if absent                  */
} flac_meta_t;

/*
 * Parse metadata from an open source positioned at the START of the file.
 * Reads only the metadata blocks (never the audio) and consumes bytes via
 * the source (seeking past blocks it doesn't need). *out is fully zeroed
 * first, then populated.
 *
 * Returns 0 on success (out->have == 1), -1 on not-a-FLAC / truncated
 * header (out is left zeroed, out->have == 0).
 */
int flac_meta_read(decoder_source_t *src, flac_meta_t *out);

#endif /* CORE_CODECS_FLAC_META_H */
