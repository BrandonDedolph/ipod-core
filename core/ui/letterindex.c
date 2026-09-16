/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/letterindex.c — where each letter starts on an alphabetised list.
 * See letterindex.h for why the index is a list of runs rather than a table
 * keyed by letter, and for what makes a list "long enough to have letters".
 */

#include "letterindex.h"

/* An empty index answers 0 / -1 / "stay" to everything, so every early return
 * from the build can leave it in one state and no accessor needs a special
 * case for a half-built walk. */
static void letteridx_clear(letteridx_t *ix, int count)
{
    ix->valid = 0;
    ix->n     = 0;
    ix->count = (uint16_t)((count > 0 && count <= 0xFFFF) ? count : 0);
}

int letteridx_build(letteridx_t *ix, int count, letteridx_initial_fn at)
{
    letteridx_clear(ix, count);
    if (!at || count < LETTERIDX_MIN_ROWS || count > 0xFFFF) {
        return 0;
    }

    char cur = 0;                    /* letter of the run in progress, 0 none */
    for (int row = 0; row < count; row++) {
        char c = at(row);
        if (c == 0) {
            cur = 0;                 /* not in the order: ends the run */
            continue;
        }
        if (c == cur) {
            ix->end[ix->n - 1] = (uint16_t)(row + 1);
            continue;
        }
        if (ix->n >= LETTERIDX_MAX_RUNS) {
            /* More runs than the alphabet has: the list is not sorted by the
             * key it was asked about. Stop here rather than walk the rest —
             * and clear, so a caller that ignores the return value cannot
             * read a truncated index as a whole one. */
            letteridx_clear(ix, count);
            return 0;
        }
        ix->letter[ix->n] = c;
        ix->first [ix->n] = (uint16_t)row;
        ix->end   [ix->n] = (uint16_t)(row + 1);
        ix->n++;
        cur = c;
    }

    if (ix->n < LETTERIDX_MIN_RUNS) {
        letteridx_clear(ix, count);
        return 0;
    }
    ix->valid = 1;
    return 1;
}

/*
 * The last run whose head is at or before `row`, or -1 when `row` is ahead of
 * every run. NOT the same question as "which run is row in": a row past a
 * run's end and before the next run's head (a gap: rows with no initial)
 * answers with the run BEHIND it, which is what a backward step wants.
 */
static int run_at_or_before(const letteridx_t *ix, int row)
{
    int lo = 0, hi = (int)ix->n - 1, r = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((int)ix->first[mid] <= row) {
            r  = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return r;
}

int letteridx_run_of(const letteridx_t *ix, int row)
{
    if (!ix->valid || row < 0 || row >= (int)ix->count) {
        return -1;
    }
    int r = run_at_or_before(ix, row);
    if (r < 0 || row >= (int)ix->end[r]) {
        return -1;                   /* before the first run, or in a gap */
    }
    return r;
}

char letteridx_letter_at(const letteridx_t *ix, int row)
{
    int r = letteridx_run_of(ix, row);
    return (r < 0) ? (char)0 : ix->letter[r];
}

int letteridx_step(const letteridx_t *ix, int sel, int dir)
{
    if (!ix->valid || sel < 0 || sel >= (int)ix->count) {
        return sel;
    }
    int r = run_at_or_before(ix, sel);
    if (dir > 0) {
        /* Inside run r or in the gap after it, the next letter is run r + 1;
         * ahead of every run (r < 0) it is run 0. */
        int nxt = r + 1;
        return (nxt >= (int)ix->n) ? sel : (int)ix->first[nxt];
    }
    if (r < 0) {
        return sel;                  /* nothing behind it */
    }
    if (sel > (int)ix->first[r]) {
        return (int)ix->first[r];    /* mid-run (or just past it): its head */
    }
    return (r == 0) ? sel : (int)ix->first[r - 1];
}
