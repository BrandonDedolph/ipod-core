/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/artcache.c — incremental album-art thumbnail cache.
 *
 * State machine per slot: EMPTY → QUEUED → (LOADED | FAILED). artcache_pump()
 * advances exactly ONE QUEUED slot per call, so the disk reads a cover load
 * needs are amortised over many main-loop passes and never stall the audio
 * decode/DMA loop the way a synchronous load during list scroll did.
 *
 * The load path mirrors player.c's load_folder_art(): enumerate the album
 * directory for a CoreArt sidecar, read it into a bounded module-static scratch
 * buffer, validate the "CART" magic + dimensions, then hand the pixels to
 * thumb_downscale_rgb565() to shrink into the slot's 22x22 box. folder.thm
 * (24x24) is preferred over folder.art (up to 120x120) when both exist — either
 * downscales correctly, but the smaller source is a cheaper read.
 *
 * Freestanding: no libc/libm/malloc, no allocation, integer only.
 */

#include "artcache.h"
#include "thumb.h"

/* CoreArt sidecar layout (see tools/coreart.py, player.c load_folder_art):
 * "CART"(4) + u16 version + u16 width + u16 height + u16 reserved, then
 * width*height RGB565 pixels. The scratch buffer is sized for the largest
 * cover we accept (120x120), which comfortably holds a 24x24 folder.thm too. */
#define ART_HDR_LEN   12
#define ART_MAX_DIM   120
#define ART_RAW_MAX   (ART_HDR_LEN + ART_MAX_DIM * ART_MAX_DIM * 2)

enum {
    SLOT_EMPTY = 0,
    SLOT_QUEUED,
    SLOT_LOADED,
    SLOT_FAILED,
};

typedef struct {
    uint8_t  state;
    uint32_t thm_clus, thm_size;               /* pre-indexed folder.thm (24x24) */
    uint32_t art_clus, art_size;               /* pre-indexed folder.art fallback */
    uint16_t px[ARTCACHE_DIM * ARTCACHE_DIM];  /* 22x22 RGB565 (valid iff LOADED) */
} slot_t;

static slot_t   g_slot[ARTCACHE_SLOTS];
static uint8_t  g_scratch[ART_RAW_MAX];

void artcache_reset(void)
{
    for (int i = 0; i < ARTCACHE_SLOTS; i++) {
        g_slot[i].state    = SLOT_EMPTY;
        g_slot[i].thm_clus = 0;
        g_slot[i].art_clus = 0;
    }
}

void artcache_queue(int slot, uint32_t thm_clus, uint32_t thm_size,
                    uint32_t art_clus, uint32_t art_size)
{
    if (slot < 0 || slot >= ARTCACHE_SLOTS) {
        return;
    }
    slot_t *s = &g_slot[slot];
    /* Same cover already tracked in this slot: leave its state (and any cached
     * pixels or prior FAILED verdict) alone so a per-frame re-queue is free. */
    if (s->state != SLOT_EMPTY && s->thm_clus == thm_clus &&
        s->art_clus == art_clus) {
        return;
    }
    s->thm_clus = thm_clus;
    s->thm_size = thm_size;
    s->art_clus = art_clus;
    s->art_size = art_size;
    s->state    = SLOT_QUEUED;
}

const uint16_t *artcache_get(int slot)
{
    if (slot < 0 || slot >= ARTCACHE_SLOTS) {
        return 0;
    }
    if (g_slot[slot].state != SLOT_LOADED) {
        return 0;
    }
    return g_slot[slot].px;
}

/* Read + validate a CoreArt sidecar (clus/size) into g_scratch and downscale it
 * into `dst` (22x22). Returns 1 on success, 0 on any failure. */
static int load_one(fat32_t *fs, uint32_t clus, uint32_t size, uint16_t *dst)
{
    if (clus == 0 || size < ART_HDR_LEN || size > sizeof g_scratch) {
        return 0;
    }
    int32_t n = fat32_read_file(fs, clus, g_scratch, size);
    if (n < (int32_t)ART_HDR_LEN) {
        return 0;
    }
    if (g_scratch[0] != 'C' || g_scratch[1] != 'A' ||
        g_scratch[2] != 'R' || g_scratch[3] != 'T') {
        return 0;
    }
    int w = g_scratch[6] | (g_scratch[7] << 8);
    int h = g_scratch[8] | (g_scratch[9] << 8);
    if (w <= 0 || h <= 0 || w > ART_MAX_DIM || h > ART_MAX_DIM) {
        return 0;
    }
    if ((int32_t)(ART_HDR_LEN + w * h * 2) > n) {
        return 0;
    }
    thumb_downscale_rgb565((const uint16_t *)(g_scratch + ART_HDR_LEN), w, h,
                           dst, ARTCACHE_DIM, ARTCACHE_DIM);
    return 1;
}

int artcache_pump(fat32_t *fs)
{
    int slot = -1;
    for (int i = 0; i < ARTCACHE_SLOTS; i++) {
        if (g_slot[i].state == SLOT_QUEUED) {
            slot = i;
            break;
        }
    }
    if (slot < 0 || fs == 0) {
        return 0;
    }

    slot_t *s = &g_slot[slot];
    /* Clusters were pre-resolved at library-load, so this is a direct file read
     * — no directory scan. Prefer folder.thm: it is pre-baked at exactly
     * ARTCACHE_DIM (28x28) by tools/coreart.py, so the load is a ~1.5KB read +
     * a 1:1 copy (thumb_downscale is identity when src==dst) — no big read, no
     * resample. folder.art (120x120) is only the fallback for an album that
     * somehow lacks a thm (it downscales, still correct just slower). */
    int ok = load_one(fs, s->thm_clus, s->thm_size, s->px) ||
             load_one(fs, s->art_clus, s->art_size, s->px);

    s->state = ok ? SLOT_LOADED : SLOT_FAILED;
    return 1;
}
