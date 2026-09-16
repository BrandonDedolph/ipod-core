/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/letterindex.h — where each letter starts on an alphabetised list.
 *
 * WHY THIS FILE EXISTS
 *
 * The A-Z locator (the 66x66 plate, and the wheel stepping a letter per
 * detent) used to be SONGS ONLY, and for one reason: everything it needed
 * was a linear walk. `list_letter_step` asks the screen for the initial of
 * row after row until the letter changes, which is O(rows in the letter) per
 * detent, and the plate's letter came from one `initial_of()` on the selected
 * row. That is affordable exactly once — on the one list whose order was
 * known to be a title sort.
 *
 * Every long list on this device is alphabetised: Songs, a genre's songs, an
 * artist's All Songs, Artists, Albums, Playlists and (since the sort that
 * landed with this file) Genres. What they do NOT share is the KEY they are
 * sorted by — Artists sort past a leading "The ", Albums by the album half of
 * the folder name — so "the initial of row i" is a per-screen question, and
 * the answer has to come from the screen. This module turns that per-row
 * answer into a small index of RUNS, built once per list, and then answers
 * "the letter at row i" and "the head of the next/previous letter" out of it
 * in O(log runs) with no further calls.
 *
 * RUNS, NOT A 27-SLOT TABLE. The library's collation (library/names.c
 * title_cmp) is a byte-wise, ASCII-case-folded compare, so every byte below
 * 'A' (space, punctuation, digits) sorts BEFORE A and every byte above 'Z'
 * (`[`, `_`, and every UTF-8 lead byte >= 0x80) sorts AFTER Z. `initial_of`
 * calls both groups '#'. So '#' is not one contiguous group: "1979" is at the
 * top of the list and "Elan" spelt with an E-acute is at the bottom. A table
 * keyed by letter would collapse the two and make the trailing group
 * unreachable by a letter step. A run list keeps them apart, and the run CAP
 * is also what tells a list that is not sorted at all from one that is,
 * without a second walk.
 *
 * WHEN A LIST HAS LETTERS. Aiming has to beat scrolling, or the cue is a
 * distraction: LETTERIDX_MIN_ROWS rows and LETTERIDX_MIN_RUNS distinct runs,
 * both pinned by the test so a bench can retune them in one line.
 *
 * DEPENDENCIES: <stdint.h>. No hardware, no globals, no library headers —
 * the per-row initial is injected, the same seam ui/wheel.c uses.
 */

#ifndef CORE_UI_LETTERINDEX_H
#define CORE_UI_LETTERINDEX_H

#include <stdint.h>

/* '#', A..Z, a trailing '#', and slack. A list that needs MORE runs than this
 * is not sorted by the key it was asked about (letters alternating), which is
 * exactly the case that must not get a plate. */
#define LETTERIDX_MAX_RUNS  40

/*
 * The thresholds. At WHEEL_VEL_MAX (8) rows per detent a 48-row list is six
 * detents end to end, and a letter step across the alphabet is up to 27 — so
 * below 48 rows the letter is the SLOWER control as well as the noisier one.
 * (Apple's 5G used ~100; our lists are 6-8 rows tall rather than 9-11, so the
 * same "a few screens" is a smaller number of rows.) Four runs because a list
 * with three letters in it is aimed at with the eye.
 */
#define LETTERIDX_MIN_ROWS  48
#define LETTERIDX_MIN_RUNS   4

/*
 * A run is a maximal span of consecutive rows sharing one initial. Rows whose
 * initial is 0 ("this row is not part of the order" — the album list's
 * synthetic All Songs row) belong to no run: they break the run in progress
 * and are never a run head, so `first[r] .. end[r)` can have gaps between
 * runs and each run needs its own end.
 */
typedef struct {
    uint8_t  valid;                       /* 0: this list has no letters       */
    uint8_t  n;                           /* runs                              */
    uint16_t count;                       /* rows the index was built over     */
    char     letter[LETTERIDX_MAX_RUNS];  /* run initial ('#', 'A'..'Z')       */
    uint16_t first [LETTERIDX_MAX_RUNS];  /* first row of the run              */
    uint16_t end   [LETTERIDX_MAX_RUNS];  /* one past its last row             */
} letteridx_t;

/* The initial of row `row` on the list being indexed, or 0 for a row that is
 * not part of the alphabetical order. */
typedef char (*letteridx_initial_fn)(int row);

/*
 * Walk rows 0..count-1 once and record the runs. Returns ix->valid: 0 (and an
 * index that answers 0 / -1 / "stay" to everything) when `at` is NULL, when
 * count is under LETTERIDX_MIN_ROWS or above what a uint16_t row index can
 * hold, when the list yields fewer than LETTERIDX_MIN_RUNS runs, or when it
 * yields more than LETTERIDX_MAX_RUNS — the last being an unsorted list.
 */
int  letteridx_build(letteridx_t *ix, int count, letteridx_initial_fn at);

/* The run `row` sits in, or -1 (invalid index, row out of range, or a row
 * that belongs to no run). Binary search over the run heads. */
int  letteridx_run_of(const letteridx_t *ix, int row);

/* The letter to show for `row`: its run's, or 0 when it is in no run. */
char letteridx_letter_at(const letteridx_t *ix, int row);

/*
 * Where a letter detent from `sel` lands. Forward: the head of the next run.
 * Backward: the head of sel's OWN run when sel is not already on it, else the
 * head of the previous run — so a back-step is never a no-op mid-letter, which
 * is the rule ui/wheel.c's list_letter_step walk already implements. `sel` is
 * returned unchanged at either end and on a list with no letters.
 */
int  letteridx_step(const letteridx_t *ix, int sel, int dir);

#endif /* CORE_UI_LETTERINDEX_H */
