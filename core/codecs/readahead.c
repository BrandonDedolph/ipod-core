/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/readahead.c — block-buffering shim over a decoder_source_t.
 * See readahead.h for the rationale (collapsing a decoder's many tiny reads
 * into a few large backing-source reads).
 */

#include "readahead.h"

/* memcpy: our word-optimised one on bare metal, libc's on host tests. */
#ifdef CORE_FREESTANDING
#include "../lib/mem.h"
#else
#include <string.h>
#endif

/* Position the backing source at absolute `at`, avoiding a redundant seek when
 * it is already there. Returns 1 on success, 0 if the backing seek failed. */
static int inner_seek_to(readahead_t *ra, int64_t at)
{
    if (ra->inner_pos == at) {
        return 1;
    }
    if (!ra->inner->seek(ra->inner->userdata, (int)at, DECODER_SEEK_SET)) {
        return 0;
    }
    ra->inner_pos = at;
    return 1;
}

/* Refill the buffer with up to `cap` bytes starting at absolute `at`. Returns
 * the number of bytes read (0 at end-of-stream or on a backing error). */
static uint32_t refill(readahead_t *ra, int64_t at)
{
    if (!inner_seek_to(ra, at)) {
        ra->buf_len = 0;
        ra->buf_pos = -1;
        return 0;
    }
    size_t n = ra->inner->read(ra->inner->userdata, ra->buf, ra->cap);
    ra->buf_pos    = at;
    ra->buf_len    = (uint32_t)n;
    ra->inner_pos += (int64_t)n;
    return (uint32_t)n;
}

/* True if absolute `pos` is inside the currently-buffered window. */
static int in_window(const readahead_t *ra, int64_t pos)
{
    return ra->buf_len > 0 &&
           pos >= ra->buf_pos &&
           pos <  ra->buf_pos + (int64_t)ra->buf_len;
}

static size_t ra_read(void *ud, void *dst_, size_t bytes)
{
    readahead_t *ra  = (readahead_t *)ud;
    uint8_t     *dst = (uint8_t *)dst_;
    size_t       done = 0;

    while (done < bytes) {
        if (in_window(ra, ra->cursor)) {
            /* Serve the contiguous run available from the buffer. */
            uint32_t off   = (uint32_t)(ra->cursor - ra->buf_pos);
            uint32_t avail = ra->buf_len - off;
            size_t   take  = bytes - done;
            if (take > avail) {
                take = avail;
            }
            memcpy(dst + done, ra->buf + off, take);
            done       += take;
            ra->cursor += (int64_t)take;
            continue;
        }

        size_t remaining = bytes - done;
        if (remaining >= ra->cap) {
            /* Bulk read: bypass the buffer entirely so a big streaming read
             * isn't chopped into cap-sized pieces and doesn't evict the
             * buffered window. */
            if (!inner_seek_to(ra, ra->cursor)) {
                break;
            }
            size_t n = ra->inner->read(ra->inner->userdata, dst + done, remaining);
            if (n == 0) {
                break;                       /* end of stream */
            }
            ra->inner_pos += (int64_t)n;
            ra->cursor    += (int64_t)n;
            done          += n;
            continue;
        }

        /* Small read past the window: refill a fresh block at the cursor. */
        if (refill(ra, ra->cursor) == 0) {
            break;                           /* end of stream */
        }
    }
    return done;
}

static int ra_seek(void *ud, int offset, int origin)
{
    readahead_t *ra = (readahead_t *)ud;
    int64_t      target;

    if (origin == DECODER_SEEK_SET) {
        target = (int64_t)offset;
    } else if (origin == DECODER_SEEK_CUR) {
        target = ra->cursor + (int64_t)offset;
    } else {
        /* SEEK_END: only the backing source knows the length. Delegate, read
         * back the resolved absolute position, and adopt it. */
        if (!ra->inner->seek(ra->inner->userdata, offset, DECODER_SEEK_END)) {
            return 0;
        }
        target = ra->inner->tell(ra->inner->userdata);
        ra->inner_pos = target;
    }

    if (target < 0) {
        return 0;
    }
    ra->cursor = target;                     /* lazy: physical refill deferred */
    return 1;
}

static int64_t ra_tell(void *ud)
{
    return ((readahead_t *)ud)->cursor;
}

void readahead_init(readahead_t *ra, decoder_source_t *inner,
                    uint8_t *buf, uint32_t cap)
{
    ra->inner     = inner;
    ra->buf       = buf;
    ra->cap       = cap;
    ra->buf_pos   = -1;
    ra->buf_len   = 0;
    ra->cursor    = 0;
    ra->inner_pos = -1;                      /* unknown → first access seeks   */
}

void readahead_as_source(readahead_t *ra, decoder_source_t *out)
{
    out->read     = ra_read;
    out->seek     = ra_seek;
    out->tell     = ra_tell;
    out->userdata = ra;
}
