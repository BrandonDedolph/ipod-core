/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/cache.h — PP5022 unified-cache control.
 *
 * The core boots with the cache OFF, so every instruction fetch and data
 * access pays SDRAM wait-states — crippling for tight loops like audio decode.
 * cache_init() turns the 8 KB unified cache on (see docs/hw/01-soc-pp5022.md,
 * "Cache"). It's write-back, so cache_commit() must flush before any DMA agent
 * reads CPU-written memory (the I2S audio buffer) — otherwise the DMA sees
 * stale SDRAM. No MMU; the cache init does not remap the vectors.
 */
#ifndef CORE_KERNEL_CACHE_H
#define CORE_KERNEL_CACHE_H

/* Enable + prime the unified cache. Call once after clock_init(), before
 * cache-sensitive work begins. */
void cache_init(void);

/* Flush dirty lines back to SDRAM (lines stay valid/clean, not evicted). Call
 * after the CPU fills a buffer a DMA engine will then read. No-op if the cache
 * is disabled. Safe from an ISR. */
void cache_commit(void);

#endif /* CORE_KERNEL_CACHE_H */
