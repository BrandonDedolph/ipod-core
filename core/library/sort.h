/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/sort.h — the index-array sort the library views are built with.
 *
 * Every alphabetised list on the device — Songs, Albums, an album's tracklist
 * by index track number — is an array of uint16_t indices into a table,
 * sorted through a comparison over those indices. Both library sorts were
 * insertion sorts, which is fine at a few hundred entries and quadratic
 * beyond that: at the old 1200-song cap the song sort already cost ~720k
 * case-insensitive string compares, and raising the cap would have made
 * "Loading Library" grow with the SQUARE of the library. This is O(n log n),
 * STABLE (so equal titles keep their load order, and rows the index does not
 * know keep directory order), and needs one scratch array of the same length.
 *
 * WHY THIS FILE EXISTS. It was a static in kernel/main.c, which the host
 * cannot compile, so the only evidence the sort was correct was a comment
 * saying it had been "verified against a reference implementation over 200
 * randomised trials" — somewhere, once. Stability in particular is a
 * property a sort can lose in a one-character edit (`<=` to `<`) with no
 * visible effect on any list that happens to have no ties. Pure: no
 * hardware, no globals; the same sort.c links into core.elf and the host
 * test.
 */

#ifndef CORE_LIBRARY_SORT_H
#define CORE_LIBRARY_SORT_H

#include <stdint.h>

/* Compare the elements at indices `a` and `b`; strcmp-signed. */
typedef int (*idx_cmp_fn)(uint16_t a, uint16_t b);

/*
 * Sort `a[0..n)` ascending by `cmp`, stably. `tmp` is scratch of at least n
 * entries; its contents afterwards are unspecified. n <= 0 is a no-op.
 */
void merge_sort_idx(uint16_t *a, int n, uint16_t *tmp, idx_cmp_fn cmp);

#endif /* CORE_LIBRARY_SORT_H */
