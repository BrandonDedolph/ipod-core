/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/sort.c — bottom-up merge sort over an index array.
 *
 * Moved VERBATIM out of kernel/main.c (only the `static` came off). See
 * sort.h for why.
 */

#include "sort.h"

void merge_sort_idx(uint16_t *a, int n, uint16_t *tmp, idx_cmp_fn cmp)
{
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width;
            int hi  = lo + 2 * width;
            if (mid > n) mid = n;
            if (hi  > n) hi  = n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                /* <= keeps the sort STABLE: a tie takes the left run first. */
                tmp[k++] = (cmp(a[i], a[j]) <= 0) ? a[i++] : a[j++];
            }
            while (i < mid) tmp[k++] = a[i++];
            while (j < hi)  tmp[k++] = a[j++];
        }
        for (int i = 0; i < n; i++) a[i] = tmp[i];
    }
}
