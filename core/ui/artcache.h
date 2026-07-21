/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/artcache.h — incremental album-art thumbnail cache.
 *
 * The album LIST wants a small cover chip next to every row, but loading a
 * cover is disk I/O and doing it synchronously during a scroll starved the
 * audio DMA (audible stutter). This cache decouples the two: rows QUEUE the
 * slots they want, and the main loop calls artcache_pump() once per pass —
 * which loads AT MOST ONE thumbnail per call, spreading the I/O across many
 * passes so the decode/DMA loop is never blocked.
 *
 * Each slot caches one album's cover pre-shrunk to ARTCACHE_DIM x ARTCACHE_DIM
 * RGB565. On pump, the album directory is enumerated for a "folder.thm"
 * (preferred, 24x24) or "folder.art" (fallback, up to 120x120) CoreArt sidecar;
 * the file is read into a module-static scratch buffer, its "CART" header +
 * dims validated, and thumb_downscale_rgb565() shrinks it into the slot. A slot
 * that can't be loaded (no art / bad header / read error) is marked FAILED so
 * pump never retries it.
 *
 * Freestanding: no libc/libm/malloc, no allocation, integer only. All storage
 * (slots + scratch) is module-static and bounded.
 */
#ifndef CORE_UI_ARTCACHE_H
#define CORE_UI_ARTCACHE_H

#include <stdint.h>
#include "../fs/fat32.h"

#define ARTCACHE_DIM   22          /* list-chip cover is 22x22 RGB565          */
#define ARTCACHE_SLOTS 64          /* one slot per cacheable album row         */

/* Clear every slot to EMPTY (forgetting all cached pixels and pending work). */
void artcache_reset(void);

/*
 * Mark `slot` pending: it should load the cover living in the album directory
 * whose first cluster is `dir_clus`. Out-of-range slots are ignored. If the
 * slot already holds this same dir_clus (QUEUED/LOADED/FAILED) the call is a
 * no-op, so re-queuing a visible row on every frame is cheap and never re-loads
 * or re-fails it; queuing a DIFFERENT dir_clus re-arms the slot as QUEUED.
 */
void artcache_queue(int slot, uint32_t dir_clus);

/*
 * Do at most one unit of loading work: find the first QUEUED slot, load +
 * downscale its cover (→ LOADED) or mark it FAILED, and return 1. If no slot is
 * QUEUED, do nothing and return 0. Call once per main-loop pass.
 */
int artcache_pump(fat32_t *fs);

/*
 * Return the slot's ARTCACHE_DIM x ARTCACHE_DIM RGB565 pixels if it is LOADED,
 * else NULL (EMPTY / QUEUED / FAILED / out-of-range).
 */
const uint16_t *artcache_get(int slot);

#endif /* CORE_UI_ARTCACHE_H */
