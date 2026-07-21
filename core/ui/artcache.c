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
    uint32_t dir_clus;                         /* album directory first cluster */
    uint16_t px[ARTCACHE_DIM * ARTCACHE_DIM];  /* 22x22 RGB565 (valid iff LOADED) */
} slot_t;

static slot_t   g_slot[ARTCACHE_SLOTS];
static uint8_t  g_scratch[ART_RAW_MAX];

void artcache_reset(void)
{
    for (int i = 0; i < ARTCACHE_SLOTS; i++) {
        g_slot[i].state    = SLOT_EMPTY;
        g_slot[i].dir_clus = 0;
    }
}

void artcache_queue(int slot, uint32_t dir_clus)
{
    if (slot < 0 || slot >= ARTCACHE_SLOTS) {
        return;
    }
    slot_t *s = &g_slot[slot];
    /* Same album already tracked in this slot: leave its state (and any cached
     * pixels or prior FAILED verdict) alone so a per-frame re-queue is free. */
    if (s->state != SLOT_EMPTY && s->dir_clus == dir_clus) {
        return;
    }
    s->dir_clus = dir_clus;
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

/* Case-insensitive ASCII compare, self-contained so this module needs nothing
 * from kernel/main.c. Matches only 'a'..'z' folding (CoreArt names are ASCII). */
static int name_eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* readdir userdata: capture the preferred (.thm) and fallback (.art) sidecars
 * as we sweep the album directory once. */
typedef struct {
    uint32_t thm_clus, thm_size;
    uint32_t art_clus, art_size;
} art_scan_t;

static int scan_cb(void *ud, const fat32_dirent_t *e)
{
    art_scan_t *sc = (art_scan_t *)ud;
    if (e->is_dir) {
        return 0;
    }
    if (name_eq_ci(e->name, "folder.thm")) {
        sc->thm_clus = e->first_clus;
        sc->thm_size = e->size;
    } else if (name_eq_ci(e->name, "folder.art")) {
        sc->art_clus = e->first_clus;
        sc->art_size = e->size;
    }
    return 0;
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
    art_scan_t sc = { 0, 0, 0, 0 };
    int ok = 0;

    if (s->dir_clus != 0 && fat32_readdir(fs, s->dir_clus, scan_cb, &sc) == 0) {
        /* Prefer the purpose-built 24x24 folder.thm; fall back to folder.art. */
        ok = load_one(fs, sc.thm_clus, sc.thm_size, s->px) ||
             load_one(fs, sc.art_clus, sc.art_size, s->px);
    }

    s->state = ok ? SLOT_LOADED : SLOT_FAILED;
    return 1;
}
