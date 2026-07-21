/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/diskbuf.c — large watermarked anti-skip buffer.
 * See diskbuf.h for the rationale (bursty MB-scale read-ahead so the drive head
 * parks between bursts instead of reading continuously).
 */

#include "diskbuf.h"

/* memcpy: our word-optimised one on bare metal, libc's on host tests. */
#ifdef CORE_FREESTANDING
#include "../lib/mem.h"
#else
#include <string.h>
#endif

/* Cap on a single synchronous fallback fetch inside diskbuf_read(): only hit
 * when the decoder outruns the pump and finds the ring empty (open-time priming,
 * or a starved buffer). Kept small so even that path blocks only briefly. */
#define DISKBUF_SYNC_CHUNK (32u * 1024u)

/* Position the backing source at absolute `at`, skipping a redundant seek when
 * it is already there. Returns 1 on success, 0 if the backing seek failed. */
static int inner_seek_to(diskbuf_t *db, int64_t at)
{
    if (db->inner_pos == at) {
        return 1;
    }
    if (!db->inner->seek(db->inner->userdata, (int)at, DECODER_SEEK_SET)) {
        return 0;
    }
    db->inner_pos = at;
    return 1;
}

/*
 * Read up to `want` bytes from the backing source into the ring at `wr`,
 * evicting already-consumed bytes (those before `rd`) if the ring is full.
 * Clamped to the contiguous room before the ring wraps, so one backing read is
 * always a single flat span. Returns bytes fetched (0 = EOF or no room).
 */
static uint32_t diskbuf_fetch(diskbuf_t *db, uint32_t want)
{
    uint32_t buffered = (uint32_t)(db->wr - db->win_lo);
    uint32_t space    = db->cap - buffered;

    /* Make room by dropping consumed history [win_lo, rd) — never past rd. */
    if (space < want) {
        uint32_t need      = want - space;
        uint32_t evictable = (uint32_t)(db->rd - db->win_lo);
        uint32_t ev        = need < evictable ? need : evictable;
        db->win_lo += (int64_t)ev;
        space      += ev;
    }
    if (space == 0) {
        return 0;                         /* ring full up to the read cursor */
    }
    if (want > space) {
        want = space;
    }

    /* Don't straddle the ring wrap in a single backing read. */
    uint32_t woff   = (uint32_t)(db->wr % db->cap);
    uint32_t contig = db->cap - woff;
    if (want > contig) {
        want = contig;
    }

    if (!inner_seek_to(db, db->wr)) {
        return 0;
    }
    size_t n = db->inner->read(db->inner->userdata, db->buf + woff, want);
    if (n == 0) {
        db->eos = 1;
        return 0;                         /* end of stream */
    }
    db->inner_pos += (int64_t)n;
    db->wr        += (int64_t)n;
    return (uint32_t)n;
}

static size_t diskbuf_read(void *ud, void *dst_, size_t bytes)
{
    diskbuf_t *db  = (diskbuf_t *)ud;
    uint8_t   *dst = (uint8_t *)dst_;
    size_t     done = 0;

    while (done < bytes) {
        if (db->rd < db->wr) {
            /* Serve the contiguous run available from the ring. */
            uint32_t avail  = (uint32_t)(db->wr - db->rd);
            uint32_t roff   = (uint32_t)(db->rd % db->cap);
            uint32_t contig = db->cap - roff;
            size_t   take   = bytes - done;
            if (take > avail) {
                take = avail;
            }
            if (take > contig) {
                take = contig;
            }
            memcpy(dst + done, db->buf + roff, take);
            done   += take;
            db->rd += (int64_t)take;
            continue;
        }
        /* Ring drained at the cursor: synchronously fetch a bounded chunk. In
         * steady state the pump keeps this from happening; this is the priming
         * / starvation fallback. */
        if (diskbuf_fetch(db, DISKBUF_SYNC_CHUNK) == 0) {
            break;                        /* end of stream */
        }
    }
    return done;
}

static int diskbuf_seek(void *ud, int offset, int origin)
{
    diskbuf_t *db = (diskbuf_t *)ud;
    int64_t    target;

    if (origin == DECODER_SEEK_SET) {
        target = (int64_t)offset;
    } else if (origin == DECODER_SEEK_CUR) {
        target = db->rd + (int64_t)offset;
    } else {
        /* SEEK_END: only the backing source knows the length. Delegate, adopt
         * the resolved absolute position, and reset the window there. */
        if (!db->inner->seek(db->inner->userdata, offset, DECODER_SEEK_END)) {
            return 0;
        }
        target = db->inner->tell(db->inner->userdata);
        if (target < 0) {
            return 0;
        }
        db->inner_pos = target;
        db->win_lo = db->wr = db->rd = target;
        return 1;
    }

    if (target < 0) {
        return 0;
    }
    if (target >= db->win_lo && target <= db->wr) {
        db->rd = target;                  /* inside the buffered window: cheap */
        return 1;
    }
    /* Outside the window: reset. The backing source is repositioned lazily on
     * the next fetch (inner_seek_to), matching the read-ahead shim's laziness so
     * dr_flac's open-time seek-to-EOF stays free. */
    db->win_lo = db->wr = db->rd = target;
    db->eos = 0;
    return 1;
}

static int64_t diskbuf_tell(void *ud)
{
    return ((diskbuf_t *)ud)->rd;
}

void diskbuf_init(diskbuf_t *db, decoder_source_t *inner,
                  uint8_t *buf, uint32_t cap, uint32_t low, uint32_t high)
{
    db->inner     = inner;
    db->buf       = buf;
    db->cap       = cap;
    db->win_lo    = 0;
    db->wr        = 0;
    db->rd        = 0;
    db->inner_pos = -1;                    /* unknown → first access seeks */
    db->low       = low;
    db->high      = high;
    db->filling   = 1;                     /* pre-load from the first pump */
    db->eos       = 0;
}

void diskbuf_as_source(diskbuf_t *db, decoder_source_t *out)
{
    out->read     = diskbuf_read;
    out->seek     = diskbuf_seek;
    out->tell     = diskbuf_tell;
    out->userdata = db;
}

uint32_t diskbuf_fill_ahead(const diskbuf_t *db)
{
    return (uint32_t)(db->wr - db->rd);
}

uint32_t diskbuf_pump(diskbuf_t *db, uint32_t chunk)
{
    uint32_t ahead = (uint32_t)(db->wr - db->rd);

    if (db->filling) {
        /* Bursting: keep reading until we top out or hit EOF, then go idle. */
        if (db->eos || ahead >= db->high) {
            db->filling = 0;
            return 0;
        }
        return diskbuf_fetch(db, chunk);
    }
    /* Idle (drive head parked): only wake to refill once the decoder has drained
     * the buffer below the low watermark — the hysteresis that makes I/O bursty. */
    if (!db->eos && ahead < db->low) {
        db->filling = 1;
        return diskbuf_fetch(db, chunk);
    }
    return 0;
}
