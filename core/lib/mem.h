/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/lib/mem.h — freestanding memory primitives.
 *
 * The bare-metal build links no libc, but vendored code (dr_flac) and the
 * compiler itself can emit calls to the standard names memcpy / memset /
 * memmove / memcmp. We provide them here so those resolve, and so the codec
 * config macros (DRFLAC_COPY_MEMORY / DRFLAC_ZERO_MEMORY) route through real,
 * word-optimised implementations rather than byte loops on the decode hot
 * path. Standard signatures on purpose — these ARE memcpy et al.
 */
#ifndef CORE_LIB_MEM_H
#define CORE_LIB_MEM_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

#endif /* CORE_LIB_MEM_H */
