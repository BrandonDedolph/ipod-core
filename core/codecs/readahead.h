/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/readahead.h — a buffering shim over a decoder_source_t.
 *
 * Why this exists: some decoders (notably dr_mp3, scanning an ID3v2 tag and
 * probing frame headers) issue thousands of tiny SEQUENTIAL reads — a few
 * bytes each. On hardware every one of those becomes a FAT cluster-walk plus a
 * 512-byte sector fetch over PIO, so skipping a ~250 KB embedded-album-art tag
 * took ~27 s at boot. This shim reads the backing source in large BLOCKS into a
 * fixed RAM buffer and satisfies the small reads from RAM, collapsing N tiny
 * disk hits into N/BLOCK big ones.
 *
 * It presents the SAME decoder_source_t interface it wraps, so it drops in
 * transparently between the file source and the codec:
 *
 *     readahead_t ra;
 *     readahead_init(&ra, &file_src, buf, sizeof buf);
 *     decoder_source_t buffered;
 *     readahead_as_source(&ra, &buffered);
 *     flac_open_stream(&dec, &buffered, &alloc);   // reads through the buffer
 *
 * Behaviour:
 *   - Forward reads are the fast path: served from the buffer, refilling in
 *     `cap`-sized blocks as the cursor advances.
 *   - A read at least as large as the buffer bypasses it (read straight from
 *     the backing source into the caller), so bulk streaming reads aren't
 *     throttled by the buffer size and don't evict useful buffered bytes.
 *   - Seeks are lazy: they only move the logical cursor. A later read that
 *     lands inside the currently-buffered window costs nothing; one outside it
 *     refills. SEEK_END is delegated to the backing source (which knows the
 *     size) — dr_flac/dr_mp3 seek to END at open() to size the file.
 *
 * Freestanding-clean: no libc beyond memcpy (routed through lib/mem.h on the
 * bare-metal build), no allocation — the caller owns the buffer. Fully
 * host-testable: back it with an in-memory decoder_source_t.
 */
#ifndef CORE_CODECS_READAHEAD_H
#define CORE_CODECS_READAHEAD_H

#include <stddef.h>
#include <stdint.h>
#include "decoder.h"

typedef struct {
    decoder_source_t *inner;   /* backing byte source (owned by caller)        */
    uint8_t          *buf;     /* caller-provided block buffer                  */
    uint32_t          cap;     /* buffer capacity in bytes                      */
    int64_t           buf_pos; /* absolute file offset of buf[0]; -1 = empty    */
    uint32_t          buf_len; /* valid bytes currently in buf                  */
    int64_t           cursor;  /* our logical absolute position (what tell sees)*/
    int64_t           inner_pos;/* where `inner` is physically positioned, or -1
                                  if unknown, so we skip redundant inner seeks   */
} readahead_t;

/*
 * Initialise a read-ahead over `inner`, buffering into `buf` (`cap` bytes).
 * The logical cursor starts at 0; the backing source is not touched until the
 * first read. `buf` must stay valid for the life of the shim.
 */
void readahead_init(readahead_t *ra, decoder_source_t *inner,
                    uint8_t *buf, uint32_t cap);

/*
 * Fill `*out` with a decoder_source_t whose read/seek/tell route through `ra`.
 * Hand `out` (not the backing source) to the codec's open_stream().
 */
void readahead_as_source(readahead_t *ra, decoder_source_t *out);

#endif /* CORE_CODECS_READAHEAD_H */
