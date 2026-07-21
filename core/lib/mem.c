/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/lib/mem.c — freestanding memcpy / memset / memmove / memcmp.
 *
 * Word-at-a-time when both ends are 4-byte aligned (the common case for the
 * codec's sample buffers), byte-wise otherwise. Correct for any alignment;
 * the fast path is just an optimisation. No libc, no builtins.
 */

#include "mem.h"

#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    /* Word-copy the aligned middle when src and dst share alignment. */
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0u) {
        uint32_t       *dw = (uint32_t *)d;
        const uint32_t *sw = (const uint32_t *)s;
        while (n >= 4u) {
            *dw++ = *sw++;
            n -= 4u;
        }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while (n-- > 0u) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t  b = (uint8_t)c;

    if (((uintptr_t)d & 3u) == 0u) {
        uint32_t  w  = (uint32_t)b;
        w |= w << 8;
        w |= w << 16;
        uint32_t *dw = (uint32_t *)d;
        while (n >= 4u) {
            *dw++ = w;
            n -= 4u;
        }
        d = (uint8_t *)dw;
    }
    while (n-- > 0u) {
        *d++ = b;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0u) {
        return dst;
    }
    if (d < s) {
        return memcpy(dst, src, n);      /* no overlap in the forward copy */
    }
    /* dst > src: copy backwards. */
    d += n;
    s += n;
    while (n-- > 0u) {
        *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n-- > 0u) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }
    return 0;
}
