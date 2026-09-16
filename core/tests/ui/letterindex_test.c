/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/letterindex_test.c — the A-Z run index (core/ui/letterindex.c) on
 * the host.
 *
 * THE POINT OF THIS FILE. The locator used to be Songs-only because the only
 * thing it could do was walk the list row by row; this index is what makes it
 * affordable on every long list, and it is the piece that decides whether a
 * list gets a plate AT ALL. Both of those are judgements no one can see on
 * the device: an index that quietly said "not sorted" would simply look like
 * a feature that had not been built.
 *
 * The per-row initial is injected, so a "list" here is a string of letters —
 * which means the collation cases that actually bite (a '#' group before A
 * AND a second one after Z, rows that are in no order at all) are one literal
 * each.
 *
 * The step oracle is deliberately the same set of cases as wheel_test's §6
 * over `list_letter_step`: the two must agree, because Stage B swaps one for
 * the other under the same wheel.
 */

#include <stdio.h>
#include <string.h>

#include "letterindex.h"

#include "../xfail.h"

/* The list under the index: a string of per-row initials ('.' = no initial,
 * i.e. a row that is not part of the alphabetical order). */
static const char *g_rows;
static int         g_rows_n;

static char initial_fn(int row)
{
    if (!g_rows || row < 0 || row >= g_rows_n) return 0;
    char c = g_rows[row];
    return (c == '.') ? (char)0 : c;
}

/* Sentinels around the index: a build that runs off the end of letter[] /
 * first[] / end[] writes into these, and every case checks them. */
static struct {
    uint32_t    guard_lo;
    letteridx_t ix;
    uint32_t    guard_hi;
} g_box;

static void box_reset(void)
{
    memset(&g_box, 0, sizeof g_box);
    g_box.guard_lo = 0xA5A5A5A5u;
    g_box.guard_hi = 0x5A5A5A5Au;
}

static int box_intact(void)
{
    return g_box.guard_lo == 0xA5A5A5A5u && g_box.guard_hi == 0x5A5A5A5Au;
}

/* Build over `rows`, which may be shorter than `n` — the tail repeats its
 * last character, so a 48-row list is a short literal plus a pad. */
static char g_list[8192];

static int build(const char *pattern, int n, letteridx_t **out)
{
    int plen = (int)strlen(pattern);
    if (n > (int)sizeof g_list - 1) n = (int)sizeof g_list - 1;
    for (int i = 0; i < n; i++) g_list[i] = pattern[i < plen ? i : plen - 1];
    g_list[n] = '\0';
    g_rows   = g_list;
    g_rows_n = n;
    box_reset();
    int ok = letteridx_build(&g_box.ix, n, initial_fn);
    *out = &g_box.ix;
    return ok;
}

/* letter_at over every row must reproduce the source exactly. */
static int letters_match(const letteridx_t *ix, int n)
{
    for (int i = 0; i < n; i++) {
        char want = (g_list[i] == '.') ? (char)0 : g_list[i];
        if (letteridx_letter_at(ix, i) != want) {
            fprintf(stderr, "[letterindex] row %d: letter %d, want %d\n",
                    i, letteridx_letter_at(ix, i), want);
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    xfail_ctx c = { "letterindex", 0, 0, 0 };
    letteridx_t *ix;

    /* ---- 1. the thresholds, by value ------------------------------------ */
    /* Pinned so a retune after a bench is a visible one-line change here as
     * well as in the header — these two numbers ARE the feature's taste. */
    xpect(&c, "thresholds are 48 rows / 4 runs",
          LETTERIDX_MIN_ROWS == 48 && LETTERIDX_MIN_RUNS == 4);

    /* One row short of the threshold: no letters, whatever the runs. */
    build("ABCDEFGH", LETTERIDX_MIN_ROWS - 1, &ix);
    xpect(&c, "a list under MIN_ROWS has no letters",
          ix->valid == 0 && letteridx_letter_at(ix, 0) == 0 &&
          letteridx_step(ix, 4, 1) == 4 && box_intact());
    build("ABCDEFGH", LETTERIDX_MIN_ROWS, &ix);
    xpect(&c, "...and at MIN_ROWS it has them", ix->valid == 1);

    /* Long enough, but everything under one letter: nothing to aim at. */
    build("A", 200, &ix);
    xpect(&c, "a long list with one run has no letters",
          ix->valid == 0 && ix->n == 0 && box_intact());
    build("ABC", 200, &ix);      /* 3 runs: A, B, then C padded to the end */
    xpect(&c, "...nor with three, one short of MIN_RUNS", ix->valid == 0);
    build("ABCD", 200, &ix);
    xpect(&c, "...and four is enough", ix->valid == 1 && ix->n == 4);

    /* ---- 2. an unsorted list is not an alphabetised one ------------------ */
    /* Genres before the sort that landed with this file: intern order, so the
     * initial changes on nearly every row. The run cap is what notices. */
    {
        static char ab[600];
        for (int i = 0; i < 600; i++) ab[i] = (char)('A' + (i & 1));
        ab[599] = '\0';
        g_rows = ab; g_rows_n = 599;
        box_reset();
        int ok = letteridx_build(&g_box.ix, 599, initial_fn);
        xpect(&c, "an ABABAB... list is not sorted: no letters, arrays intact",
              ok == 0 && g_box.ix.valid == 0 && g_box.ix.n == 0 && box_intact());
    }
    /* Exactly at the cap is still a list, one past it is not. */
    {
        static char many[600];
        int n = 0;
        for (int r = 0; r < LETTERIDX_MAX_RUNS; r++) {
            for (int k = 0; k < 2; k++) many[n++] = (char)('A' + (r % 26));
            /* a '.' between runs so repeats of the same letter stay distinct */
            many[n++] = '.';
        }
        many[n] = '\0';
        g_rows = many; g_rows_n = n;
        box_reset();
        int ok = letteridx_build(&g_box.ix, n, initial_fn);
        xpect(&c, "exactly LETTERIDX_MAX_RUNS runs still indexes",
              ok == 1 && g_box.ix.n == LETTERIDX_MAX_RUNS && box_intact());
        for (int k = 0; k < 2; k++) many[n++] = 'Q';
        many[n] = '\0';
        g_rows_n = n;
        box_reset();
        ok = letteridx_build(&g_box.ix, n, initial_fn);
        xpect(&c, "one run past the cap is 'not sorted', inside the arrays",
              ok == 0 && g_box.ix.valid == 0 && box_intact());
    }

    /* ---- 3. letters and run heads --------------------------------------- */
    /* "AAABBCDDDD" — wheel_test §6's list, padded to a real length so the
     * thresholds let it through. The pad extends the D run. */
    build("AAABBCDDDD", 60, &ix);
    xpect(&c, "runs are the letter changes: A@0 B@3 C@5 D@6",
          ix->valid && ix->n == 4 &&
          ix->letter[0] == 'A' && ix->first[0] == 0 &&
          ix->letter[1] == 'B' && ix->first[1] == 3 &&
          ix->letter[2] == 'C' && ix->first[2] == 5 &&
          ix->letter[3] == 'D' && ix->first[3] == 6);
    xpect(&c, "letter_at reproduces every row", letters_match(ix, 60));
    xpect(&c, "run_of finds the run a row sits in",
          letteridx_run_of(ix, 0) == 0 && letteridx_run_of(ix, 2) == 0 &&
          letteridx_run_of(ix, 3) == 1 && letteridx_run_of(ix, 5) == 2 &&
          letteridx_run_of(ix, 59) == 3);
    xpect(&c, "run_of is -1 off both ends",
          letteridx_run_of(ix, -1) == -1 && letteridx_run_of(ix, 60) == -1);

    /* The step oracle — the same cases as wheel_test §6 over the walk. */
    xpect(&c, "step: mid-A forward -> first B", letteridx_step(ix, 1, 1) == 3);
    xpect(&c, "step: first B forward -> C", letteridx_step(ix, 3, 1) == 5);
    xpect(&c, "step: C forward -> first D", letteridx_step(ix, 5, 1) == 6);
    xpect(&c, "step: in the last letter forward stays",
          letteridx_step(ix, 6, 1) == 6 && letteridx_step(ix, 59, 1) == 59);
    xpect(&c, "step: mid-D back -> head of D", letteridx_step(ix, 8, -1) == 6);
    xpect(&c, "step: head of D back -> head of C", letteridx_step(ix, 6, -1) == 5);
    xpect(&c, "step: C back -> head of B", letteridx_step(ix, 5, -1) == 3);
    xpect(&c, "step: head of B back -> head of A", letteridx_step(ix, 3, -1) == 0);
    xpect(&c, "step: mid-A back -> head of A, then stays",
          letteridx_step(ix, 2, -1) == 0 && letteridx_step(ix, 0, -1) == 0);
    xpect(&c, "step: a row off the list is not a step",
          letteridx_step(ix, -1, 1) == -1 && letteridx_step(ix, 60, -1) == 60);

    /* ---- 4. the trailing '#' group, after Z ------------------------------ */
    /* title_cmp puts every UTF-8 lead byte after 'Z', so a library with an
     * accented title has TWO '#' groups. Both must be steppable, which is the
     * whole reason this is a run list. */
    build("##ABYZ##", 60, &ix);
    xpect(&c, "'#' before A and '#' after Z are different runs",
          ix->valid && ix->n == 6 &&
          ix->letter[0] == '#' && ix->first[0] == 0 &&
          ix->letter[5] == '#' && ix->first[5] == 6);
    xpect(&c, "a forward step reaches the trailing '#'",
          letteridx_step(ix, 5, 1) == 6);
    xpect(&c, "...and a back step out of it lands on Z, not on the leading '#'",
          letteridx_step(ix, 6, -1) == 5);
    xpect(&c, "the leading '#' is still reachable backwards",
          letteridx_step(ix, 2, -1) == 0);

    /* ---- 5. rows that are in no order ----------------------------------- */
    /* The album list's synthetic "All Songs" row at 0, plus one in the middle
     * to prove the rule is about the ROW and not about being first. */
    build(".AABB.CCDD", 60, &ix);
    xpect(&c, "a row with no initial belongs to no run",
          letteridx_letter_at(ix, 0) == 0 && letteridx_run_of(ix, 0) == -1 &&
          letteridx_letter_at(ix, 5) == 0 && letteridx_run_of(ix, 5) == -1);
    xpect(&c, "letter_at reproduces every row, gaps included",
          letters_match(ix, 60));
    xpect(&c, "runs start after the gap, never on it",
          ix->n == 4 && ix->first[0] == 1 && ix->first[2] == 6);
    xpect(&c, "a step from the All Songs row goes to the first letter",
          letteridx_step(ix, 0, 1) == 1);
    xpect(&c, "...and backwards from it there is nowhere to go",
          letteridx_step(ix, 0, -1) == 0);
    xpect(&c, "no step ever lands on a row with no initial",
          letteridx_step(ix, 4, 1) == 6 && letteridx_step(ix, 6, -1) == 3);
    xpect(&c, "a step FROM the interior gap goes on to the next run",
          letteridx_step(ix, 5, 1) == 6 && letteridx_step(ix, 5, -1) == 3);

    /* ---- 6. a real-sized library ---------------------------------------- */
    /* 6000 songs, LIB_MAX_SONGS, in the shape a title sort produces: a run of
     * digits and punctuation, the alphabet, then the accented tail. */
    {
        static char big[6001];
        int n = 0;
        for (int i = 0; i < 120; i++) big[n++] = '#';
        for (int L = 0; L < 26; L++) {
            for (int i = 0; i < 220; i++) big[n++] = (char)('A' + L);
        }
        while (n < 6000) big[n++] = '#';
        big[n] = '\0';
        g_rows = big; g_rows_n = n;
        box_reset();
        int ok = letteridx_build(&g_box.ix, n, initial_fn);
        ix = &g_box.ix;
        memcpy(g_list, big, (size_t)n + 1);       /* for letters_match */
        xpect(&c, "6000 rows: 28 runs, built inside the arrays",
              ok == 1 && ix->n == 28 && box_intact());
        xpect(&c, "letter_at reproduces all 6000 rows", letters_match(ix, n));
        /* 27 forward detents cross the whole library; the 28th stays. */
        int sel = 0, steps = 0;
        for (;;) {
            int nxt = letteridx_step(ix, sel, 1);
            if (nxt == sel) break;
            sel = nxt;
            if (++steps > 64) break;
        }
        xpect(&c, "27 letter detents cross the library and the end holds",
              steps == 27 && sel == (int)ix->first[27]);
        for (;;) {
            int prv = letteridx_step(ix, sel, -1);
            if (prv == sel) break;
            sel = prv;
            if (--steps < -64) break;
        }
        xpect(&c, "...and 27 back return to row 0", steps == 0 && sel == 0);
    }

    /* ---- 7. a NULL row source is not a list ----------------------------- */
    box_reset();
    xpect(&c, "no initial source: no letters, and nothing reads the arrays",
          letteridx_build(&g_box.ix, 1000, 0) == 0 && g_box.ix.valid == 0 &&
          letteridx_letter_at(&g_box.ix, 0) == 0 &&
          letteridx_step(&g_box.ix, 10, 1) == 10 && box_intact());

    return xfail_done(&c);
}
