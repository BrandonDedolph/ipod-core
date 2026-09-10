/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/library/sort_test.c — the index-array merge sort (core/library/sort.c)
 * on the host.
 *
 * Every alphabetised list on the device is produced by this one function,
 * and two of its properties are load-bearing in ways no list would show at a
 * glance: it must be STABLE (equal titles keep load order; in an album's
 * tracklist, rows the index does not know keep DIRECTORY order — the sort
 * is what makes "ties keep their order" true), and it must stay inside
 * a[0..n) and tmp[0..n) for every n, including the ones that are not a
 * power of two and leave a short final run at every merge width.
 *
 * The oracle is an insertion sort that is stable by construction, run over
 * the same key table; the sorted index arrays must be IDENTICAL, not merely
 * both ordered. Sentinels bracket both arrays.
 */

#include <stdio.h>
#include <string.h>

#include "../../library/sort.h"
#include "../xfail.h"

#define MAX_N 6000                       /* LIB_MAX_SONGS: the largest sort */

/* The key table the comparisons index. Small key range => many ties. */
static uint16_t g_key[MAX_N];
static int      g_cmp_calls;
static int      g_cmp_bad;               /* calls with an index outside [0,n) */
static int      g_n;

static int key_cmp(uint16_t a, uint16_t b)
{
    g_cmp_calls++;
    if (a >= g_n || b >= g_n) g_cmp_bad++;
    return (int)g_key[a] - (int)g_key[b];
}

/* A stable insertion sort over the same keys: the reference. */
static void ref_sort(uint16_t *a, int n)
{
    for (int i = 1; i < n; i++) {
        uint16_t v = a[i];
        int j = i - 1;
        while (j >= 0 && g_key[a[j]] > g_key[v]) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

static uint32_t g_rng = 12345u;
static uint32_t rnd(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

/* Sentinel-bracketed arrays; the guard words must survive every sort. */
#define GUARD 0xBEEF
static struct { uint16_t pre; uint16_t a[MAX_N]; uint16_t post; } g_arr;
static struct { uint16_t pre; uint16_t t[MAX_N]; uint16_t post; } g_tmp;
static uint16_t g_ref[MAX_N];

enum { RANDOM, DESCENDING, ASCENDING };

/* Fill keys — random over `range` values (0 = all equal), or already in
 * descending / ascending order — in identity index order, sort both ways,
 * and return whether the results match exactly. */
static int trial(int n, int range, int mode)
{
    g_n = n;
    for (int i = 0; i < n; i++) {
        g_key[i] = (uint16_t)(range ? rnd() % (uint32_t)range : 7u);
        if (mode == DESCENDING) g_key[i] = (uint16_t)(n - i);
        if (mode == ASCENDING)  g_key[i] = (uint16_t)i;
        g_arr.a[i] = g_ref[i] = (uint16_t)i;
    }
    g_arr.pre = g_arr.post = g_tmp.pre = g_tmp.post = GUARD;
    memset(g_tmp.t, 0xA5, sizeof g_tmp.t);
    g_cmp_bad = 0;
    merge_sort_idx(g_arr.a, n, g_tmp.t, key_cmp);
    ref_sort(g_ref, n);
    return memcmp(g_arr.a, g_ref, (size_t)n * sizeof g_ref[0]) == 0 &&
           g_arr.pre == GUARD && g_arr.post == GUARD &&
           g_tmp.pre == GUARD && g_tmp.post == GUARD && g_cmp_bad == 0;
}

int main(void)
{
    xfail_ctx c = { "sort", 0, 0, 0 };

    /* ---- degenerate sizes ---------------------------------------------- */
    g_n = 1; g_arr.a[0] = 0; g_key[0] = 3;
    g_cmp_calls = 0;
    merge_sort_idx(g_arr.a, 0, g_tmp.t, key_cmp);
    xpect(&c, "n = 0 is a no-op and compares nothing", g_cmp_calls == 0);
    merge_sort_idx(g_arr.a, 1, g_tmp.t, key_cmp);
    xpect(&c, "n = 1 is a no-op and compares nothing",
          g_cmp_calls == 0 && g_arr.a[0] == 0);
    merge_sort_idx(g_arr.a, -5, g_tmp.t, key_cmp);
    xpect(&c, "negative n is a no-op", g_cmp_calls == 0);

    /* ---- the smallest merges, by hand ----------------------------------- */
    g_n = 2; g_key[0] = 9; g_key[1] = 4; g_arr.a[0] = 0; g_arr.a[1] = 1;
    merge_sort_idx(g_arr.a, 2, g_tmp.t, key_cmp);
    xpect(&c, "two out of order are swapped", g_arr.a[0] == 1 && g_arr.a[1] == 0);
    g_n = 2; g_key[0] = 4; g_key[1] = 4; g_arr.a[0] = 0; g_arr.a[1] = 1;
    merge_sort_idx(g_arr.a, 2, g_tmp.t, key_cmp);
    xpect(&c, "two equal keep their order (the <= in the merge)",
          g_arr.a[0] == 0 && g_arr.a[1] == 1);
    /* Three: the second merge width has a one-element right run. */
    g_n = 3; g_key[0] = 2; g_key[1] = 2; g_key[2] = 1;
    g_arr.a[0] = 0; g_arr.a[1] = 1; g_arr.a[2] = 2;
    merge_sort_idx(g_arr.a, 3, g_tmp.t, key_cmp);
    xpect(&c, "odd n: short final run merges, ties stay ordered",
          g_arr.a[0] == 2 && g_arr.a[1] == 0 && g_arr.a[2] == 1);

    /* ---- against the stable reference, many sizes, many tie densities -- */
    int all_ok = 1, trials = 0;
    static const int sizes[] = { 2, 3, 5, 7, 8, 9, 15, 16, 17, 31, 33, 100,
                                 127, 128, 129, 255, 257, 1000, 1023, 1025 };
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        for (int range = 0; range <= 64; range += 8) {          /* 0 = all equal */
            for (int rep = 0; rep < 3; rep++) {
                if (!trial(sizes[s], range, RANDOM)) {
                    all_ok = 0;
                    fprintf(stderr, "  mismatch: n=%d range=%d rep=%d\n",
                            sizes[s], range, rep);
                }
                trials++;
            }
        }
    }
    xpect(&c, "matches a stable reference (order AND tie order) on every trial",
          all_ok);
    printf("[sort] %d randomised trials\n", trials);

    xpect(&c, "all-equal keys come out in load order", trial(500, 0, RANDOM));
    xpect(&c, "already sorted input is unchanged", trial(300, 0, ASCENDING));
    xpect(&c, "descending input is reversed", trial(300, 0, DESCENDING));

    /* ---- the largest sort the device performs -------------------------- */
    xpect(&c, "LIB_MAX_SONGS entries with heavy ties: correct, in bounds",
          trial(MAX_N, 26, RANDOM));
    xpect(&c, "LIB_MAX_SONGS distinct keys: correct, in bounds",
          trial(MAX_N, 65535, RANDOM));

    /* ---- what the comparison is asked --------------------------------- */
    g_cmp_calls = 0;
    trial(1000, 1000, RANDOM);
    xpect(&c, "O(n log n): 1000 entries take < 12k compares (not ~500k)",
          g_cmp_calls < 12000);

    return xfail_done(&c);
}
