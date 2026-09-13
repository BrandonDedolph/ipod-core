/*
 * core/kernel/main.c — C entry point, called from boot/crt0.S.
 *
 * Boot bring-up (proven on real 5.5G hardware): UART banner, 30 MHz clock,
 * unified cache, 100 Hz tick + IRQs, then — if the BCM LCD is powered — the
 * disk/audio stack. From there kernel_main mounts the FAT32 volume and runs the
 * menu UI: a main menu (Music / … / Now Playing), a Music sub-menu, and an
 * album/folder browser that streams the selected track through the background
 * player (core/player/player.c) and out the DMA-fed DAC. MENU pops screens; a
 * playing track keeps going while you navigate.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "hw/ata.h"
#include "hw/clickwheel.h"
#include "hw/backlight.h"
#include "hw/battery.h"
#include "hw/i2c.h"
#include "hw/power.h"
#include "hw/audio.h"
#include "hw/piezo.h"
#include "hal.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "sched.h"
#include "timer.h"
#include "irq.h"
#include "clock.h"
#include "cache.h"
#include "console.h"
#include "panic.h"
#include "config.h"
#include "../ui/text.h"
#include "../ui/thumb.h"
#include "../ui/artcache.h"
#include "../ui/screen_charging.h"
#include "../ui/screen_battery.h"
#include "../ui/settings.h"
#include "../ui/palette.h"
#include "../ui/chrome.h"
#include "../library/names.h"
#include "../library/idx.h"
#include "../library/sort.h"
#include "../ui/wheel.h"
#include "hw/volume.h"

/*
 * Idle-task CPU sleep. Program the per-core countdown to wake this core
 * after ~`ms` milliseconds and halt until then (01-soc-pp5022.md, "Sleep
 * / wake"). We use PROC_WAIT_CNT (self-wakes on the countdown), NOT
 * PROC_SLEEP — with no interrupt controller installed yet, sleep-until-
 * interrupt would never wake. Only the CPU runs the kernel in Phase 1,
 * so CPU_CTL is correct. Three NOPs after the write per the doc's
 * pipeline rule.
 */
static void cpu_wait_ms(uint8_t ms) {
    mmio_write32(CPU_CTL_ADDR, PROC_WAIT_CNT | PROC_CNT_MSEC | ms);
    __asm__ volatile("nop\n\tnop\n\tnop");
}

/* Same halt, in MICROseconds (the counter field is 8 bits, so <= 255 us). The
 * doc describes PROC_WAIT_CNT as "sleep until countdown" and does NOT promise an
 * interrupt wake, so anything used while AUDIO IS PLAYING must stay inside the
 * DMA ISR's deadline: the transfer is single-shot and re-kicked in the ISR, and
 * only the 16-frame I2S FIFO (~363 us) covers a late one. */
static void cpu_wait_us(uint8_t us) {
    mmio_write32(CPU_CTL_ADDR, PROC_WAIT_CNT | PROC_CNT_USEC | us);
    __asm__ volatile("nop\n\tnop\n\tnop");
}

/* ---------------------------------------------------------------------------
 * Timed UI windows (volume overlay, lock/unlock plate)
 *
 * USEC_TIMER is a free-running 1 MHz counter, so it WRAPS every ~71.6 min. An
 * absolute "now < deadline" compare is wrong across that wrap (the deadline
 * looks like it's in the distant past, or a stale one springs back to life), so
 * every timed check in this file stores a START stamp and compares an ELAPSED
 * difference — (uint32_t)(now - t0) < span — which is wrap-correct. An expired
 * window disarms itself so a later wrap can never re-open it.
 * ------------------------------------------------------------------------- */
typedef struct { uint32_t t0; int armed; } ui_window_t;

static void ui_window_arm(ui_window_t *w)
{
    w->t0    = mmio_read32(USEC_TIMER_ADDR);
    w->armed = 1;
}

static int ui_window_up(ui_window_t *w, uint32_t span, uint32_t now)
{
    if (!w->armed) return 0;
    if ((uint32_t)(now - w->t0) < span) return 1;
    w->armed = 0;                     /* expired: can't reopen on the next wrap */
    return 0;
}

/* ---------------------------------------------------------------------------
 * Linen theme + Nunito faces
 * ------------------------------------------------------------------------- */

/* Theme tokens (LINEN_*) and the Nunito face macros moved to ui/chrome.h,
 * next to the list chrome that consumes them. */
/* Linear RGB565 blend: fg over bg by alpha a (0..256). Cheap (no gamma) — fine
 * for the subtle 1px anti-alias fringe on modal corners. */
static uint16_t blend565(uint16_t bg, uint16_t fg, int a)
{
    int inv = 256 - a;
    int r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * inv) >> 8;
    int g = (((fg >>  5) & 0x3F) * a + ((bg >>  5) & 0x3F) * inv) >> 8;
    int b = (( fg        & 0x1F) * a + ( bg        & 0x1F) * inv) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* 4x4-supersampled coverage of one corner quadrant, memoized per radius: the
 * count (0..16) of sub-samples inside the circle for corner pixel (ry, rx). The
 * AA rounded rect used to recompute this 16-sample loop for every corner pixel
 * of every paint, for a radius that is always one of a handful of values. */
#define AA_SS 16                            /* S*S sub-samples, S = 4          */
static int     g_aa_r = -1;
static uint8_t g_aa_mask[UI_RR_MAX_R * UI_RR_MAX_R];

static const uint8_t *aa_corner_mask(int r)
{
    if (r != g_aa_r) {
        const int S = 4, cN = r * 2 * S;             /* circle centre, x2S      */
        for (int ry = 0; ry < r; ry++) {
            for (int rx = 0; rx < r; rx++) {
                int inside = 0;
                for (int sy = 0; sy < S; sy++) {
                    int dy = ry * 2 * S + sy * 2 + 1 - cN;
                    for (int sx = 0; sx < S; sx++) {
                        int dx = rx * 2 * S + sx * 2 + 1 - cN;
                        if (dx * dx + dy * dy <= cN * cN) inside++;
                    }
                }
                g_aa_mask[ry * UI_RR_MAX_R + rx] = (uint8_t)inside;
            }
        }
        g_aa_r = r;
    }
    return g_aa_mask;
}

/* Anti-aliased filled rounded rect: solid interior + straight edges, with the
 * four corner quadrants super-sampled (4x4) so their boundary pixels blend into
 * whatever is already in the framebuffer — smooth corners instead of the integer
 * stair-steps of fill_round_rect. Reads the FB, so the background under the
 * corners must already be drawn (true for the modals). Costlier than the plain
 * version, so it's used only for the volume/lock plates (drawn on events). */
static void fill_round_rect_aa(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r < 1) { console_fill_rect(x, y, w, h, c); return; }
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    if (r > UI_RR_MAX_R) r = UI_RR_MAX_R;
    console_fill_rect(x, y + r, w, h - 2 * r, c);          /* solid middle band  */
    /* The corner quadrants are blended straight into the framebuffer below, so
     * they'd be invisible to the damage tracker (the edge fills stop at x+r).
     * Report the whole rect once. */
    console_damage_add(x, y, w, h);
    uint16_t *fb = console_fb();
    const uint8_t *mask = aa_corner_mask(r);
    for (int ry = 0; ry < r; ry++) {
        console_fill_rect(x + r, y + ry,         w - 2 * r, 1, c);   /* top edge */
        console_fill_rect(x + r, y + h - 1 - ry, w - 2 * r, 1, c);   /* bot edge */
        for (int rx = 0; rx < r; rx++) {
            int inside = mask[ry * UI_RR_MAX_R + rx];
            if (inside == 0) continue;
            int a = inside * 256 / AA_SS;
            int xs[2] = { x + rx, x + w - 1 - rx };
            int ys[2] = { y + ry, y + h - 1 - ry };
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) {
                    int px = xs[i], py = ys[j];
                    if (px < 0 || py < 0 || px >= LCD_WIDTH || py >= LCD_HEIGHT) continue;
                    uint16_t *d = &fb[py * LCD_WIDTH + px];
                    *d = (a >= 256) ? c : blend565(*d, c, a);
                }
        }
    }
}

/* Marquee: a single "currently-scrolling" text target (the selected overflowing
 * row, or the now-playing title). Render fns set it while drawing; the main loop
 * scrolls it in place via a tiny partial present. `text` points at our own
 * copy (g_mq.last), so callers may pass a transient buffer safely. */
/* The fingerprint buffer must hold the LONGEST target we can be handed, or the
 * comparison below sees "changed" every frame and the scroll phase resets
 * forever (the title never actually advances). Every target is a display name
 * or a tag string, both bounded by NAME_MAX; anything longer is rejected as
 * non-scrollable in mq_set rather than silently truncated into that trap. */
#define MQ_TEXT_MAX NAME_MAX

static struct {
    int                active, x, y, w;
    int                tw;                 /* measured text width (px)            */
    const char        *text;
    const text_font_t *font;
    uint16_t           ink, bg;
    uint32_t           t0;                 /* phase clock origin (target start)   */
    char               last[MQ_TEXT_MAX + 1]; /* content fingerprint of that target */
    int                last_x, last_y;
    int                cy0, cy1;           /* vertical clip (keeps it in its row) */
} g_mq;

/* Marquee cadence: dwell showing the truncated start, scroll once to reveal the
 * tail, dwell on the tail, then reset to the start and repeat. `t_us` is time
 * SINCE the target was registered (g_mq.t0), so it always begins from the
 * truncated look rather than mid-scroll, and — because callers draw the title
 * through this at the SAME live offset — a full repaint never shows a truncated
 * frame that the scroll loop then has to correct (no wheel/lock stutter). */
#define MQ_US_PER_PX  26000u              /* ~38 px/s scroll                     */
#define MQ_HOLD_START 1500000u            /* dwell on the truncated start (1.5s) */
#define MQ_HOLD_END    900000u            /* dwell on the revealed tail   (0.9s) */

/* Current scroll offset (px) for overflowing text `tw` wide in a `w` window at
 * elapsed phase time `t_us`. 0 while it fits. */
static int mq_offset(int tw, int w, uint32_t t_us)
{
    if (tw <= w) return 0;
    int max_off = tw - w;
    uint32_t scroll = (uint32_t)max_off * MQ_US_PER_PX;
    uint32_t cycle  = MQ_HOLD_START + scroll + MQ_HOLD_END;
    uint32_t tp = t_us % cycle;
    int off;
    if (tp < MQ_HOLD_START) {
        off = 0;                                      /* truncated (start)      */
    } else if (tp < MQ_HOLD_START + scroll) {
        off = (int)((tp - MQ_HOLD_START) / MQ_US_PER_PX);
    } else {
        off = max_off;                                /* hold on the tail       */
    }
    if (off > max_off) off = max_off;
    return off;
}

/* Draw `s` in the window [x, x+w) at baseline y. Clears the text band to bg
 * first, then draws it at its current scroll offset (`t_us` = phase time). */
/* `tw` is the caller's already-measured text_width(s, font) — measuring it here
 * too made the marquee path walk the same string three times per frame. */
static void draw_marquee(int x, int y, int w, const char *s, int tw,
                         const text_font_t *font, uint16_t ink, uint16_t bg,
                         uint32_t t_us, int cy0, int cy1)
{
    /* Clear only the glyph ink box (ascent+descent), NOT the full line_height —
     * line_height includes leading that reaches into a row's sub-line below, so
     * a scrolling title would otherwise erase the artist text under it. The
     * [cy0, cy1) window keeps the clear AND the glyphs inside the row, so tall
     * glyph tops (baseline sits close to the bar top) can't paint above the
     * selection bar into the row above. */
    int asc = text_ascent(font);
    int top = y - asc, bot = y + text_descent(font);
    if (top < cy0) top = cy0;
    if (bot > cy1) bot = cy1;
    if (bot > top) console_fill_rect(x, top, w, bot - top, bg);
    int off = mq_offset(tw, w, t_us);
    text_draw_clip_v(console_fb(), LCD_WIDTH, LCD_HEIGHT, x - off, y, s, font, ink,
                     x, x + w, cy0, cy1);
}

/* Register the selected/overflowing text as the marquee target (only if it
 * actually overflows its window). Resets the phase clock (g_mq.t0) when the
 * target changes — compared by CONTENT + position, since the now-playing title
 * reuses one buffer across tracks so a pointer check would miss the change — so
 * the scroll always restarts from the truncated look on a new row/track. */
static int mq_set(int x, int y, int w, const char *text, int tw,
                  const text_font_t *font, uint16_t ink, uint16_t bg,
                  int cy0, int cy1)
{
    if (tw <= w) return 0;

    /* Length-explicit: a target that doesn't FIT the fingerprint can't be
     * compared (its tail is the part that differs), so it would read "changed"
     * every frame and never scroll. Leave it static instead — it still renders
     * clipped, it just doesn't animate. */
    int len = 0;
    while (text[len]) {
        if (++len > MQ_TEXT_MAX) return 0;
    }

    int changed = (x != g_mq.last_x || y != g_mq.last_y);
    if (!changed) {
        int i = 0;
        while (i < len && text[i] == g_mq.last[i]) i++;
        changed = (i != len || g_mq.last[len] != '\0');
    }
    if (changed) {
        int i = 0;
        for (; i < len; i++) g_mq.last[i] = text[i];
        g_mq.last[i] = '\0';
        g_mq.last_x = x; g_mq.last_y = y;
        g_mq.t0 = mmio_read32(USEC_TIMER_ADDR);
    }
    g_mq.active = 1; g_mq.x = x; g_mq.y = y; g_mq.w = w; g_mq.tw = tw;
    /* Point at our OWN copy (g_mq.last), not the caller's buffer: some callers
     * (e.g. the album list) build the row text in a per-iteration stack local,
     * which is dead by the time the scroll tick re-reads it — a dangling pointer
     * that painted a different album's name. last[] is static and always holds
     * the current target's content, so the periodic redraw stays valid. */
    g_mq.text = g_mq.last; g_mq.font = font; g_mq.ink = ink; g_mq.bg = bg;
    g_mq.cy0 = cy0; g_mq.cy1 = cy1;
    return 1;
}

/* Draw `text` in [x, x+w) at baseline y AND register it as the marquee target.
 * Overflowing text is rendered at its current scroll offset (not a static
 * offset-0), so a full repaint mid-scroll shows the live position rather than a
 * truncated frame the scroll loop then jumps to correct. Non-overflowing text
 * just draws normally. Clears the text band to bg first. */
static void mq_text(int x, int y, int w, const char *text,
                    const text_font_t *font, uint16_t ink, uint16_t bg,
                    int cy0, int cy1)
{
    /* Measure ONCE and pass it down (mq_set + draw_marquee both need it). */
    int tw = text_width(text, font);
    if (tw <= w || !mq_set(x, y, w, text, tw, font, ink, bg, cy0, cy1)) {
        /* Fits (or is too long to marquee safely): just draw it. NO background
         * band clear — the caller already painted the row/selection background,
         * and a clear at y-ascent would bleed ABOVE the selection bar (ascent 14 >
         * the sub-row title lift of 11) and paint a black bar over the row above. */
        ui_text_clip(x, y, text, font, ink, x, x + w);
        return;
    }
    draw_marquee(x, y, w, text, tw, font, ink, bg,
                 mmio_read32(USEC_TIMER_ADDR) - g_mq.t0, cy0, cy1);
}

/* ---------------------------------------------------------------------------
 * File / album browser
 *
 * browse_entry_t + BROWSE_MAX/NAME_MAX are shared with the player (player.h):
 * a folder's worth of entries is copied into the player as its queue.
 * ------------------------------------------------------------------------- */

static browse_entry_t g_browse[BROWSE_MAX];
static int            g_browse_n;

/* Browser navigation. The album LIST (depth 0) is index-driven — see g_albums
 * below — while the TRACKLIST (depth 1) is a live folder read into g_browse.
 * Albums are flat on disk (import flattens multi-disc), so the browser is only
 * ever two levels: g_dir_depth is 0 (albums) or 1 (an album's tracks).
 * g_cur_dir is the cluster of the album whose tracks are currently listed. */
static uint32_t g_cur_dir;
static int      g_dir_depth;

/* Bumped whenever a list's CONTENT is rebuilt or the screen stack moves. The
 * partial-repaint path (list_repaint_partial) compares it, so it can never
 * repaint two rows of a list that is no longer the one it last painted — the
 * screen identity and row count alone can coincide (two genres with the same
 * number of songs, reached faster than one throttled repaint window). */
static uint32_t g_list_epoch;

/* Browser view state (was local to the old browse loop). The album LIST (depth
 * 0) and a single album's TRACKLIST (depth 1) keep SEPARATE selections, so
 * backing out of an album returns the cursor to that album in the list rather
 * than jumping to the top. */
static int g_br_sel, g_br_accum;      /* album list (depth 0)  */
static int g_det_sel, g_det_accum;    /* tracklist   (depth 1) */

/* Index-derived album list — the source of truth is CORELIB.IDX, so an album
 * appears here only if the index references it. Stale/orphan or differently
 * named folders left on disk by an old import simply never show (which is what
 * kept "Kid Laroi" and "The Kid LAROI" from splitting into two entries). Built
 * once by library_ensure, sorted A->Z; g_albumview is the on-screen slice
 * (all, or one artist's). */
#define LIB_MAX_ALBUMS 1024
typedef struct {
    char     folder[NAME_MAX + 1];
    uint32_t clus;                       /* album folder cluster                  */
    uint32_t art_clus, art_size;         /* folder.art (120x120) — now-playing    */
    uint32_t thm_clus, thm_size;         /* folder.thm (28x28)  — list chip       */
    uint8_t  unreadable;                 /* the folder's directory could not be
                                          * read at load, even after retries. Its
                                          * songs have file_clus == 0 — NOT because
                                          * they left the disk, but because we never
                                          * got to look. Cleared (and the songs
                                          * bound) when a later open reads it. */
} lib_album_t;
static lib_album_t g_albums[LIB_MAX_ALBUMS];
static int         g_albums_n;
static uint16_t    g_albumview[LIB_MAX_ALBUMS];
static int         g_albumview_n;

/* Set whenever a library load hits one of the fixed caps (LIB_MAX_SONGS /
 * LIB_MAX_ALBUMS / ARTISTS_MAX / LIB_MAX_GENRES / FOLDER_MAP_MAX), so the
 * truncation isn't silent — the About screen says the library didn't fit. */
static int         g_lib_truncated;

/*
 * The other ways a load can come up short, kept SEPARATE from g_lib_truncated
 * because "Library too large" is the wrong thing to tell the user for any of
 * them — and until these existed, every one of them was silent: the tracks
 * were simply absent, or present and dead, with nothing to say why.
 *
 *   g_lib_unreadable  albums whose folder could not be read (disk error, or a
 *                     corrupt directory cluster) after the retries in
 *                     lib_readdir. Each such album has .unreadable set and all
 *                     its songs unresolved (file_clus == 0), so they list in
 *                     Songs/Genres but cannot play. Opening the album retries
 *                     the read and, on success, resolves it and decrements
 *                     this. UI: About should say "N albums could not be read";
 *                     the album's tracklist should say so instead of showing
 *                     an empty list (see g_browse_err).
 *   g_lib_orphaned    CORELIB.IDX records whose album folder is not on the
 *                     disk at all — a stale index, not a disk fault. Nothing
 *                     to retry; the fix is re-running the host importer. UI:
 *                     "index out of date, N tracks skipped".
 *   g_lib_load_err    nonzero (a FAT32_* code) when the library ROOT itself
 *                     could not be enumerated, so there is no library at all
 *                     this session. The counts above are then meaningless
 *                     (0 songs). UI: a "could not read the disk" screen in
 *                     place of an empty Music menu; to retry, clear
 *                     g_lib_scanned and call library_ensure again.
 */
static int         g_lib_unreadable;
static int         g_lib_orphaned;
static int         g_lib_load_err;
static uint32_t     g_lib_load_ms;   /* boot library load time, shown on About */

/*
 * Boot phase breakdown, all milliseconds, all shown on the About screen. The
 * library load already had a number; the other 8-ish seconds of a cold boot
 * did not, so "make it boot faster" had nowhere to aim. Same reasoning as
 * g_lib_load_ms: a number on the panel is what makes a change here verifiable
 * instead of a matter of impression.
 *
 * g_boot_t0_us is stamped once, as early as the timer allows, and every phase
 * is measured against the live USEC_TIMER rather than accumulated, so a phase
 * we forget to instrument shows up as the gap in the total rather than
 * silently vanishing.
 */
static uint32_t     g_boot_t0_us;    /* stamped right after timer_init        */
static uint32_t     g_boot_lcd_ms;   /* lcd_init(): cold BCM bring-up         */
static uint32_t     g_boot_disk_ms;  /* ata_init + first read + FAT32 mount   */
static uint32_t     g_boot_resume_ms;/* resume_restore(): readdir + open+seek */
/*
 * ...of which. MS_NA (see ui/screen_settings.c) means "this step did not run"
 * — NOT "it took 0 ms". Conflating those made the screen lie exactly when it
 * mattered: after the FLAC seek fix an instant seek looked identical to a
 * skipped one. Seeded to MS_NA before resume_restore(), overwritten by each
 * step that actually executes.
 */
#define BOOT_MS_NA 0xFFFFFFFFu
static uint32_t     g_boot_res_dir_ms  = BOOT_MS_NA; /* album readdir        */
static uint32_t     g_boot_res_open_ms = BOOT_MS_NA; /* open + art + prime   */
static uint32_t     g_boot_res_seek_ms = BOOT_MS_NA; /* player_seek_to()     */
static uint32_t     g_boot_total_ms; /* to the first UI paint                 */

/* ms since g_boot_t0_us (USEC_TIMER wraps; the subtraction is unsigned so a
 * wrap mid-boot still yields the right delta). */
static uint32_t boot_ms_now(void)
{
    return (mmio_read32(USEC_TIMER_ADDR) - g_boot_t0_us) / 1000u;
}

/* Album art (folder.art) location captured while enumerating the current
 * folder; handed to player_play_queue so the player owns/validates it. */
static uint32_t g_art_clus, g_art_size;

/* When set, the album list (depth 0) only shows folders whose "Artist - Album"
 * name has this artist prefix — the Artists → one-artist's-albums drill-down.
 * Empty means the plain "Albums" list (every folder). */
static char g_artist_filter[NAME_MAX + 1];

/* Output volume (WM8758 codec gain, 0..100) + how long the on-screen volume
 * overlay stays up after the last wheel tick (volume-demo.jsx: ~1.5 s then fade).
 * The window is START-stamped, not deadline-stamped — see ui_window_t. */
static int         g_volume = 70;
static ui_window_t g_vol_show;
#define VOL_SHOW_US 1500000u

/* The live Settings state (ui/settings.h model). Declared up here because the
 * library/playback helpers below read it (shuffle, clicker); the navigation and
 * rendering that own it live in the Settings section further down. */
static settings_t g_settings;

/* fat32_readdir callback: collect music subdirectories + playable files into
 * g_browse. Directories named like iPod/OS system folders (or dotfolders) are
 * junk-filtered out; non-playable files are skipped. */
static int browse_collect(void *ud, const fat32_dirent_t *e)
{
    (void)ud;

    /* Capture the folder's album-art sidecar (not shown as a list row). */
    if (!e->is_dir && name_eq_ci(e->name, "folder.art")) {
        g_art_clus = e->first_clus;
        g_art_size = e->size;
        return 0;
    }

    if (g_browse_n >= BROWSE_MAX) return 1;   /* array full: stop enumeration */

    if (e->is_dir) {
        if (is_junk_dir(e->name)) return 0;       /* skip system/Apple folders */
        /* The browser is exactly two levels (albums, then one album's tracks —
         * import flattens multi-disc), so a folder nested INSIDE an album
         * ("Scans", "Artwork") is never navigable. Listing it anyway put a dead
         * entry in g_browse: the wheel ranged over g_browse_n (dirs included)
         * while the tracklist view skipped dirs, so one wheel position selected
         * nothing visible and SELECT there played a different track. */
        if (g_dir_depth != 0) return 0;
        /* Artists drill-down: at the album-list level, keep only this artist's
         * folders (name "Artist - Album"). */
        if (g_dir_depth == 0 && g_artist_filter[0]) {
            char a[NAME_MAX + 1], b2[NAME_MAX + 1];
            split_artist_album(e->name, a, b2);
            if (!name_eq_ci(a, g_artist_filter)) return 0;
        }
        browse_entry_t *b = &g_browse[g_browse_n++];
        copy_display_name(b->name, e->name, 0);   /* keep folder name as-is */
        b->clus   = e->first_clus;
        b->size   = 0;
        b->fmt    = 0;
        b->is_dir = 1;
        return 0;
    }

    /* The album/artist lists (depth 0) show only folders — hide loose files at
     * the root (leftover TEST.*, stray downloads) so they don't clutter it. */
    if (g_dir_depth == 0) return 0;

    int fmt = classify_ext(e->name);
    if (fmt < 0) return 0;
    browse_entry_t *b = &g_browse[g_browse_n++];
    copy_display_name(b->name, e->name, 1);       /* trim ".flac" -> title */
    b->clus   = e->first_clus;
    b->size   = e->size;
    b->fmt    = (uint8_t)fmt;
    b->is_dir = 0;
    b->art_clus = b->art_size = 0;                 /* album play: queue-level art */
    return 0;
}

/* Vertical layout (menus.jsx): a status strip up top, a titled header with a
 * divider, then the scrolling list. */
#define STATUS_Y0  0                      /* status strip band (battery/track)    */
#define STATUS_H   15                     /* strip height                         */
#define HDR_BASE   30                     /* header title text baseline           */
#define HDR_DIV_Y  38                     /* header divider row                    */
#define LIST_Y0    42                     /* first list row top                   */
#define ROW_H      24                     /* px per single-line list row          */
#define LIST_ROWS  8                       /* visible rows: (240-42)/24 ~= 8       */
/* Two-line rows (album/song list: title + artist sub) get a taller row so the
 * bold title (bold_13, 19px ink) and the artist line (regular_9, 14px) don't
 * vertically overlap — 32px clears both so the marquee is cleanly title-only
 * (no artist repaint, no clipping of descenders). Fewer fit on screen. */
#define ROW_H2     32
#define LIST_ROWS2 6                       /* (240-42)/32 ~= 6                     */

/* ---------------------------------------------------------------------------
 * Design-matched list chrome (menus.jsx): status strip, header, rows, scrollbar
 * ------------------------------------------------------------------------- */

/* Hold-switch lock state. While g_locked, all wheel/button input is swallowed
 * (playback keeps running); a brief plate flashes on the engage/disengage edge,
 * and a small padlock stays in the status strip while held. */
static int         g_locked;

/* Last believed headphone state (1 seated, 0 out, -1 unknown) — the unplug
 * edge detector in run_ui(). Starts unknown so a boot with nothing in the
 * jack cannot look like a pull-out. */
static int         g_hp_last = -1;
static ui_window_t g_lock_flash;

/*
 * Now-Playing scrub (seek) state.
 *
 * The decoders and the player have supported seeking all along
 * (player_seek_to); nothing was bound to it, which made long tracks,
 * podcasts and audiobooks effectively unusable.
 *
 * The wheel aims at a target rather than seeking on every detent: a seek stops
 * the DAC, re-primes the ring and on a backward jump rewinds the file, so doing
 * that per detent would stutter and hammer the disk. The target is committed
 * once the wheel goes quiet (SCRUB_COMMIT_US), which is also what makes a long
 * drag across a track cost exactly one seek.
 */
#define SCRUB_COMMIT_US  400000u     /* wheel quiet this long -> perform seek  */
#define SCRUB_EXIT_US   4000000u     /* ...and this long -> back to volume     */
#define SCRUB_STEP_S          5      /* seconds per detent at rest             */

static int      g_np_scrub;          /* 1 = wheel seeks instead of volume      */
static uint32_t g_scrub_target_s;    /* where the wheel is currently aiming    */
static uint32_t g_scrub_last_us;     /* last detent, for commit/exit timing    */
static int      g_scrub_dirty;       /* target moved since the last commit     */

/* SELECT press-length arbitration on Now Playing: tap = scrubber, hold = queue. */
#define SEL_HOLD_US 450000u

static uint32_t g_sel_down_us;
static int      g_sel_pending;

static int np_scrubbing(void)
{
    return g_np_scrub;
}

static void scrub_enter(void)
{
    g_np_scrub       = 1;
    g_scrub_target_s = player_elapsed_s();
    g_scrub_last_us  = mmio_read32(USEC_TIMER_ADDR);
    g_scrub_dirty    = 0;
}

static void scrub_exit(void)
{
    g_np_scrub    = 0;
    g_scrub_dirty = 0;
}
#define LOCK_FLASH_US 1000000u

/* Small padlock glyph (system-screens.jsx corner lock): body + shackle. */
static void draw_lock_glyph(int x, int y, uint16_t c)
{
    console_fill_rect(x,     y + 4, 8, 6, c);   /* body      */
    console_fill_rect(x + 1, y,     2, 5, c);   /* left post */
    console_fill_rect(x + 5, y,     2, 5, c);   /* right post*/
    console_fill_rect(x + 1, y,     6, 2, c);   /* top arch  */
}

/* Cached battery/power readout. The gauge read is an I2C transaction (slow, and
 * shares the codec bus), so sample it a few seconds apart — NOT every present —
 * and hold the last value. mv/pct < 0 means the read failed / not yet sampled. */
static int      g_bat_mv  = -1;
static int      g_bat_pct = -1;
static int      g_bat_ext = 0;               /* external power present            */
static uint32_t g_bat_last_us;
static int      g_bat_raw = -1;              /* 10-bit ADC code, for calibration  */
static int      g_bat_mv_raw = -1;           /* mV before the plausibility clamp  */
static int      g_bat_mv_filt = -1;          /* median of recent samples (policy) */

/* Defined further down; the low-battery policy in battery_refresh() acts
 * through them. Forward-declared here rather than moving battery_refresh(),
 * which sits with the status-strip state it feeds. */
static void settings_commit(int force);
static void resume_capture(void);
_Noreturn static void enter_standby(void);

/*
 * settings_commit() modes.
 *
 *   CFG_COMMIT_IDLE   the main loop: debounced, deferred while the drive is
 *                     parked under a live player, refused below the
 *                     disk-safe battery line.
 *   CFG_COMMIT_FORCE  now (suspend, power-off): no debounce, no parked
 *                     check — still refused below the disk-safe line.
 *   CFG_COMMIT_LAST   the ONE write the low-battery policy makes at the
 *                     DISKSAFE edge, exempt from the battery gate.
 *
 * The exemption is not optional. battery_policy_feed() latches DISKSAFE
 * BEFORE it returns the edge, so by the time battery_refresh() acts on that
 * edge battery_disk_writes_allowed() is already 0 — and a gated commit there
 * refuses the very flush the policy exists to make while the cell still has
 * the energy for it. That is exactly what happened when the flush and the
 * gate landed as two separate changes, each written without the other: the
 * DISKSAFE handler called settings_commit(1), the gate turned it away, and a
 * low-battery shutdown persisted nothing at all — no resume position, no
 * pending setting — while both commit messages described a flush that
 * never ran.
 */
#define CFG_COMMIT_IDLE   0
#define CFG_COMMIT_FORCE  1
#define CFG_COMMIT_LAST   2

/* Measured cost of the last full-frame present; defined with the present
 * throttle further down. Reported on the stats line below so the number the
 * repaint throttle has always been guessing at becomes observable. */
static uint32_t g_present_cost_us;

/* Decimal to UART. Signed, because every battery field reads -1 on a bus
 * failure and printing that as 4294967295 would defeat the purpose. */
static void uart_dec(int v)
{
    char b[12];
    if (v < 0) {
        uart_putc('-');
        v = -v;
    }
    u32_to_dec(b, (unsigned)v);
    uart_puts(b);
}

/* Volume capacity / free (MB), computed once from the FS after mount for the
 * About screen. g_free_mb == 0xFFFFFFFF means the FSInfo free count was absent. */
static uint32_t g_total_mb, g_free_mb = 0xFFFFFFFFu;

/* Returns 1 if it actually resampled this call (so a live screen can repaint). */
static int battery_refresh(int force)
{
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    if (!force && (uint32_t)(now - g_bat_last_us) < 5000000u) {
        return 0;
    }
    g_bat_last_us = now;

    /*
     * ONE conversion. This used to call battery_millivolts() and then
     * battery_percent(), which runs its own — two I2C round trips, two settling
     * delays, and two DIFFERENT samples, so the millivolts on screen did not
     * necessarily correspond to the percentage next to them. It also left
     * nowhere to put a filter later.
     */
    battery_sample_t bs;
    if (battery_sample(&bs) != 0) {
        /* Bus failure is NOT a flat battery. Hold the last good reading rather
         * than reporting -1, which draw_battery() clamps to 0% — i.e. an empty
         * red battery for an I2C hiccup. The policy is told the same thing:
         * battery_policy_feed(-1) touches neither the filter nor the level, so
         * a flaky bus can neither power the device off nor clear a genuine
         * DISKSAFE. */
        (void)battery_policy_feed(-1);
        uart_puts("core: batt read failed\n");
        return 1;
    }
    g_bat_raw    = bs.raw;
    g_bat_mv_raw = bs.mv_raw;
    g_bat_mv     = bs.mv;
    g_bat_ext    = power_is_external();

    /*
     * Low-battery policy (battery.h). The sample goes into the median filter
     * and the policy decides; we act on the EDGE it returns, so each action
     * happens once per crossing. The displayed percentage is converted from
     * the FILTERED millivolts too — same curve, same mapping, just not
     * re-evaluated on a single spin-up-sagged sample every 5 s.
     */
    battery_event_t ev = battery_policy_feed(bs.mv);
    g_bat_mv_filt = battery_filtered_mv();
    g_bat_pct     = battery_percent_from_mv(g_bat_mv_filt);

    /* Same sample, same cadence, feeds what the USER is told. Everything the
     * warning UI does about flapping keys off the FILTERED millivolts and the
     * policy's own level, never a raw reading — see ui/screen_battery.c. */
    battwarn_feed(battery_filter_ready() ? g_bat_mv_filt : -1,
                  (int)battery_policy_level(), g_bat_ext,
                  mmio_read32(USEC_TIMER_ADDR));

    /*
     * One line per sample, so a full discharge can be logged over UART and
     * turned into numbers. There is otherwise NO way to see either half of the
     * battery story on this device: the percent curve is transcribed from a
     * 2005 cell and has never been checked against the one actually fitted,
     * and per-state drain (idle / paused / playing) has never been measured at
     * all. The state flags are on the line because the drain question is
     * entirely "what was the device doing while it fell".
     *
     * raw is the ground truth (no scaling assumed); mv_raw vs mv shows when the
     * plausibility clamp is engaging, which is the difference between a flat
     * cell and a bad read.
     */
    uart_puts("core: batt raw ");   uart_dec(g_bat_raw);
    uart_puts(" mv ");              uart_dec(g_bat_mv_raw);
    uart_puts(" clamped ");         uart_dec(g_bat_mv);
    uart_puts(" filt ");            uart_dec(g_bat_mv_filt);
    uart_puts(" lvl ");             uart_dec((int)battery_policy_level());
    uart_puts(" pct ");             uart_dec(g_bat_pct);
    uart_puts(" ext ");             uart_dec(g_bat_ext);
    uart_puts(" chg ");             uart_dec(power_is_charging());
    uart_puts(" play ");            uart_dec(player_active());
    uart_puts(" paused ");          uart_dec(player_paused());
    /* No backlight state: it is a local in the main loop, not a global, and
     * hoisting it just for a log line is not worth it — hold the backlight
     * fixed for the duration of a measurement run instead. */
    uart_puts(" parked ");          uart_dec(ata_is_parked());
    uart_puts(" up ");              uart_dec((int)(now / 1000000u));
    uart_putc('\n');

    /*
     * Audio health, on the same 5 s cadence.
     *
     * `late` is the counter that did not exist until the LCD IRQ-masking work:
     * completions serviced past the ~363 us the I2S FIFO can cover. It is the
     * ONLY machine-readable signal for the tick-while-the-screen-is-busy
     * failure — audio_underruns() cannot see it, because a late ISR produces
     * no short read, and until now the only detector was a person listening.
     * `present` is the measured cost of the last full present, which every
     * throughput argument about the UI has been reasoning against blind: it
     * starts life as a GUESS of 30000 us and has never been printed.
     */
    uart_puts("core: audio late ");  uart_dec((int)audio_late_kicks());
    uart_puts(" worst_us ");         uart_dec((int)audio_late_worst_us());
    uart_puts(" underruns ");        uart_dec((int)audio_underruns());
    uart_puts(" present_us ");       uart_dec((int)g_present_cost_us);
    uart_putc('\n');

    /*
     * Act on the policy's edge. Logged BEFORE acting, because the shutoff
     * branch never returns and the UART line is the only record of why the
     * device went dark.
     */
    switch (ev) {
    case BATTERY_EVENT_DISKSAFE:
        uart_puts("core: batt DISKSAFE: flushing settings, parking drive\n");
        /*
         * The LAST write. Persist the resume position + any pending change
         * NOW, while the filtered cell is still at ~3500 mV and has the
         * energy to finish one sector plus the FLUSH. From here down nothing
         * else writes: the policy latched DISKSAFE before handing us this
         * edge, so battery_disk_writes_allowed() is ALREADY 0 — which is why
         * this one commit is CFG_COMMIT_LAST (exempt from the gate) and every
         * other one, enter_standby()'s included, is refused until RECOVERED.
         * This mirrors suspend_to_ram()'s "last chance to persist": after
         * this the drive is parked and the next event is power-off.
         */
        resume_capture();
        settings_commit(CFG_COMMIT_LAST);
        /* Park, unless the player is mid-stream: it parks between its own
         * refill bursts (player.c) and its next read would only spin the
         * platters straight back up. Same guard as the main loop's idle
         * spin-down. Reads are harmless to data; writes are what matters. */
        if (!ata_is_parked() && (!player_active() || player_paused())) {
            ata_standby();
        }
        break;

    case BATTERY_EVENT_SHUTOFF:
        uart_puts("core: batt SHUTOFF: entering standby\n");
        /*
         * Say goodbye before the lights go out.
         *
         * Without this the panel went straight from whatever was on it to
         * black — from the outside indistinguishable from a crash, which is
         * the worst thing a device can look like when the real answer is
         * "plug me in". Pause first so the message is read in quiet rather
         * than over a note cut mid-way, and light the backlight: a device
         * that dies face-up on a desk should be legible.
         *
         * The hold is a USEC_TIMER spin, not cpu_wait_ms(): that takes a
         * uint8_t of milliseconds and may wake early on an IRQ, and this is
         * the one message the user gets.
         */
        player_pause();
        backlight_set(g_settings.backlight_bright);
        screen_battery_render(BATTWARN_SHUTOFF);
        lcd_present_fb(console_framebuffer());
        {
            uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
            while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < 1500000u) {
            }
        }
        /* The documented power-off path: stop the player, blank the panel,
         * PMU deep-sleep with wake sources set. Its own settings_commit(1)
         * is the reason the DISKSAFE flush above exists: by now the write
         * gate should refuse it (see battery_disk_writes_allowed). */
        enter_standby();                  /* does not return */

    case BATTERY_EVENT_RECOVERED:
        uart_puts("core: batt RECOVERED: writes allowed again\n");
        /* Nothing else to do: a change left pending by the gate rides out on
         * the main loop's next settings_commit(0). */
        break;

    case BATTERY_EVENT_NONE:
        break;
    }
    return 1;
}

/* Battery glyph: outline + nub + a fill proportional to `pct`. The fill turns a
 * clear RED at <=20% (a low-battery warning), else ink. ~30% larger than the
 * original 17x9 for legibility. Drawn at top-left (x,y), ~24px wide incl. nub. */
#define BATT_LOW_RED 0xE125u        /* distinct warning red (RGB ~226,40,44)    */
static void draw_battery(int x, int y, int pct)
{
    const int w = 22, h = 12;
    console_fill_rect(x, y, w, 1, LINEN_MUTED2);           /* top    */
    console_fill_rect(x, y + h - 1, w, 1, LINEN_MUTED2);   /* bottom */
    console_fill_rect(x, y, 1, h, LINEN_MUTED2);           /* left   */
    console_fill_rect(x + w - 1, y, 1, h, LINEN_MUTED2);   /* right  */
    console_fill_rect(x + w, y + 4, 2, h - 8, LINEN_MUTED2); /* nub   */
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int fw = ((w - 4) * pct) / 100;
    if (fw > 0) {
        console_fill_rect(x + 2, y + 2, fw, h - 4,
                          pct <= 20 ? BATT_LOW_RED : LINEN_INK);
    }
}

/* The top status strip: the now-playing track name on the left (so you always
 * see what's playing while browsing), battery on the right. During bring-up the
 * right side also shows raw millivolts (to calibrate the %-curve; see
 * battery.h "DEVICE-GATED CALIBRATION"). */
static void status_strip_render(void)
{
    /* Left: playing track (or the wordmark when idle). Clipped by the right
     * cluster, which is painted over it. */
    const char *left = player_active() ? track_display(player_track_name())
                                       : "CORE";
    /* CLIP the name before the right-hand cluster rather than drawing it full
     * width and then painting a 70x15 rectangle back over its tail — same look,
     * without rasterising glyphs that are immediately overwritten (and without
     * dirtying that band for a partial present). */
    ui_text_clip(12, STATUS_Y0 + 11, left, FONT_SMALL, LINEN_MUTED2,
                 12, LCD_WIDTH - 70);

    int bx = LCD_WIDTH - 12 - 24;             /* battery block (22 + 2 nub)        */
    draw_battery(bx, STATUS_Y0 + 1, g_bat_pct);

    /* Persistent padlock while Hold is engaged (design keeps it in the strip). */
    if (g_locked) {
        draw_lock_glyph(bx - 14, STATUS_Y0 + 3, LINEN_INK);
    }

    /* Raw mV to the left of the glyph — a device-gated CALIBRATION aid, not part
     * of the design. Off for the shipping look; flip SHOW_BATTERY_MV to 1 to
     * read the millivolts and calibrate battery.c's %-curve (see battery.h). */
#define SHOW_BATTERY_MV 0
    if (SHOW_BATTERY_MV && g_bat_mv > 0 && !g_locked) {
        char mv[8];
        int v = g_bat_mv, i = 0;
        char tmp[8]; int t = 0;
        if (v > 9999) v = 9999;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 7);
        while (t > 0) mv[i++] = tmp[--t];
        mv[i] = '\0';
        int w = text_width(mv, FONT_SMALL);
        ui_text(bx - 5 - w, STATUS_Y0 + 11, mv, FONT_SMALL, LINEN_MUTED2);
    }
}

/* Titled header with an optional back chevron and a right-aligned count/value,
 * plus the divider under it (menus.jsx ScreenHeader). */
/* Convenience: a single-line (24px) row. */
static void list_row(int r, const char *text, const char *sub, const char *right,
                     int chevron, int selected, int greyed, const uint16_t *chip)
{
    ui_list_row(LIST_Y0, r, text, sub, right, chevron, selected, greyed, chip, 0, ROW_H);
}

/* A taller (28px) two-line row for the album list: title + artist sub with a
 * cover chip, so the two lines don't overlap and the marquee stays title-only. */
static void list_row_tall(int r, const char *text, const char *sub, const char *right,
                          int chevron, int selected, int greyed, const uint16_t *chip)
{
    ui_list_row(LIST_Y0, r, text, sub, right, chevron, selected, greyed, chip, 0, ROW_H2);
}

/* Like list_row_tall, but the title takes priority over the right-hand value: a
 * long title spans the full width (over the value) and marquees on select. */
static void list_row_titled(int r, const char *text, const char *sub,
                            const char *right, int selected, const uint16_t *chip)
{
    ui_list_row(LIST_Y0, r, text, sub, right, 0, selected, 0, chip, 1, ROW_H2);
}

/* Slim right-edge scrollbar (menus.jsx Scrollbar); no-op when everything fits.
 * `y0` is the list origin (differs between the full list and the detail view). */
/* ---------------------------------------------------------------------------
 * Album detail view (collection-detail.jsx AlbumDetail): a 56x56 art hero with
 * title + track count, then the folder's tracklist. Shown when you enter an
 * album folder (browser depth > 0). The hero art is the entered folder's
 * folder.art, downscaled on-device to 56x56 (thumb_downscale_rgb565).
 * ------------------------------------------------------------------------- */
#define DET_HERO_Y   42                   /* art hero top                         */
#define DET_ART      56                   /* hero art dimension                   */
#define DET_LIST_Y0  108                  /* tracklist first row top              */
#define DET_ROWS     5                     /* visible rows: (240-108)/24 = 5 fit   */

/* The raw folder.art staging buffer is the ART CACHE's — see artcache_scratch().
 * There used to be a second, byte-identical ART_RAW_MAX array here: 28,812 bytes
 * duplicated for no reason, since the cache's copy is only live inside
 * artcache_pump() and this path (entering an album) never pumps. */
#define ART_RAW_MAX  ARTCACHE_SCRATCH_SZ
static uint16_t g_detail_art[DET_ART * DET_ART]; /* downscaled 56x56 hero           */
static int      g_detail_art_ok;
static char     g_album_title[NAME_MAX + 1];     /* album part of "Artist - Album" */
static char     g_album_artist[NAME_MAX + 1];    /* artist part (empty if none)    */
static int      g_album_track_n;                  /* playable files in the folder   */

/* Per-track disc/duration (from the index) + a display "view" that interleaves
 * "Disc N" header rows for multi-disc albums. Filled in detail_load_meta. A view
 * entry >= 0 is a track index into g_browse; < 0 encodes a header disc as
 * -(disc+1). */
#define DET_VIEW_MAX (BROWSE_MAX + 8)
static uint16_t g_track_dur[BROWSE_MAX];
static uint16_t g_track_num[BROWSE_MAX];
static uint8_t  g_track_disc[BROWSE_MAX];
/* Real (tag) title per browse row, bound from the index in detail_load_meta.
 * The on-disk filename is FAT-sanitized (?,*,:,/ -> _), so the tracklist shows
 * this instead when available; NULL falls back to the filename. */
static const char *g_track_title[BROWSE_MAX];
static int16_t  g_det_view[DET_VIEW_MAX];
static int      g_det_view_n;
static int      g_detail_multidisc;
/* Total runtime of the folder's tracks, summed ONCE in detail_load_meta (the
 * meta line used to re-sum every track on every paint). 0 = durations unknown. */
static uint32_t g_detail_total_s;

/* Read the current folder's folder.art (clus/size captured by browse_load) and
 * downscale it to the 56x56 hero. Leaves g_detail_art_ok=0 if absent/malformed.
 * Same CoreArt "CART" validation the player uses (player.c load_folder_art). */
static void detail_art_load(fat32_t *fs)
{
    g_detail_art_ok = 0;
    if (g_art_clus == 0 || g_art_size < 12 || g_art_size > (uint32_t)ART_RAW_MAX) {
        return;
    }
    uint8_t *raw = artcache_scratch();
    int32_t  n   = fat32_read_file(fs, g_art_clus, raw, g_art_size);
    if (n < 12) {
        return;
    }
    if (raw[0] != 'C' || raw[1] != 'A' || raw[2] != 'R' || raw[3] != 'T') {
        return;
    }
    int w = raw[6] | (raw[7] << 8);
    int h = raw[8] | (raw[9] << 8);
    if (w <= 0 || h <= 0 || w > 120 || h > 120) {
        return;
    }
    if ((int32_t)(12 + w * h * 2) > n) {
        return;
    }
    thumb_box_rgb565((const uint16_t *)(raw + 12), w, h,
                     g_detail_art, DET_ART, DET_ART);
    g_detail_art_ok = 1;
}

/* Animated three-bar "now playing" glyph (collection-detail.jsx NowPlayingDot).
 * Each bar bounces on a triangle wave with its own phase; bottom-anchored at
 * y+9, heights 3..9. `t_us` is the free-running microsecond clock. */
#define NP_BARS_W  9                          /* bounding box width  (x..x+8)      */
#define NP_BARS_H  9                          /* bounding box height (y..y+8)      */

static int nowplaying_bar_h(uint32_t t_ms, int phase)
{
    uint32_t p = (t_ms + (uint32_t)phase) % 760u;      /* period 760 ms          */
    int tri = (p < 380u) ? (int)p : (int)(760u - p);   /* 0..380..0 triangle     */
    return 3 + (tri * 6) / 380;                          /* 3..9 px               */
}

static void nowplaying_bars(int x, int y, uint16_t c, uint32_t t_us)
{
    uint32_t ms = t_us / 1000u;
    static const int ph[3] = { 0, 250, 500 };
    for (int i = 0; i < 3; i++) {
        int h = nowplaying_bar_h(ms, ph[i]);
        console_fill_rect(x + i * 3, y + (NP_BARS_H - h), 2, h, c);
    }
}

/* One tracklist row of the album detail, at list-row `r` showing view entry
 * `vi`. Split out of detail_render so a selection move can repaint just the two
 * rows that changed instead of the whole panel (see list_repaint_partial). */
static void detail_row_draw(int r, int vi)
{
    int ry = DET_LIST_Y0 + r * ROW_H;
    int16_t v = g_det_view[vi];
    /* Single-disc albums have no "DISC N" header rows, so the per-disc number
     * gutter is wasted width — pull the numbers and titles left to reclaim it. */
    int num_rx  = g_detail_multidisc ? 30 : 24;   /* track-number right-align x */
    int title_x = g_detail_multidisc ? 38 : 30;   /* title left x               */
    if (v < 0) {                          /* a "Disc N" section header */
        char h[12];
        int hi = 0;
        for (const char *p = "DISC "; *p; p++) h[hi++] = *p;
        u32_to_dec(h + hi, (unsigned)(-(int)v - 1));
        ui_text(14, ry + 15, h, FONT_SMALL, LINEN_MUTED_D);
        return;
    }
    int idx = v;
    const browse_entry_t *e = &g_browse[idx];
    int is_sel = (idx == g_det_sel);
    if (is_sel) {
        ui_round_rect(6, ry + 1, LCD_WIDTH - 16, ROW_H - 2, 4, LINEN_SEL_BG);
    }
    uint16_t fg = is_sel ? LINEN_SEL_FG : LINEN_INK;
    uint16_t nc = is_sel ? LINEN_SEL_SUB : LINEN_MUTED2;
    /* Left gutter: the now-playing bars for the playing track, else its
     * (per-disc) track number (collection-detail.jsx). */
    const char *playing = player_active() ? player_track_name() : 0;
    if (playing && name_eq_ci(e->name, playing)) {
        nowplaying_bars(15, ry + 6, is_sel ? LINEN_SEL_FG : LINEN_INK,
                        mmio_read32(USEC_TIMER_ADDR));
    } else {
        char num[6];
        u32_to_dec(num, (unsigned)(g_track_num[idx] ? g_track_num[idx]
                                                    : idx + 1));
        int nw = text_width(num, FONT_SMALL);
        ui_text(num_rx - nw, ry + 15, num, FONT_SMALL, nc);
    }
    /* Duration on the right (from the index); the title must stop before it. */
    int title_right = LCD_WIDTH - 16;
    if (g_track_dur[idx]) {
        char dts[FMT_TIME_MAX];
        fmt_time(dts, g_track_dur[idx]);
        int dw = text_width(dts, text_font_bold_12());
        ui_text(LCD_WIDTH - 16 - dw, ry + 15, dts, text_font_bold_12(),
                is_sel ? LINEN_SEL_SUB : LINEN_MUTED_D);
        title_right = LCD_WIDTH - 16 - dw - 8;
    }
    /* Title (clean — number gutter provides the index), indented past it,
     * CLIPPED before the duration so a long title can't overlap it; the
     * selected row's long title scrolls (marquee). */
    const text_font_t *tf = is_sel ? FONT_HEADER : FONT_ROW;
    /* Prefer the real tag title (bound from the index): the filename is
     * FAT-sanitized (e.g. "WHO CARES?" -> "WHO CARES_"), the tag isn't. */
    const char *tt = g_track_title[idx] ? g_track_title[idx]
                                        : track_display(e->name);
    if (is_sel) {
        mq_text(title_x, ry + 15, title_right - title_x, tt, tf, fg,
                LINEN_SEL_BG, ry, ry + ROW_H);
    } else {
        ui_text_clip(title_x, ry + 15, tt, tf, fg, title_x, title_right);
    }
}

/* The selected track's position in the display view (which interleaves the
 * "Disc N" header rows), i.e. the row space the scroll window works in. */
static int detail_sel_view(int sel)
{
    for (int i = 0; i < g_det_view_n; i++) {
        if (g_det_view[i] == (int16_t)sel) return i;
    }
    return 0;
}

static void detail_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    fmt_count(right, sel + 1, g_browse_n > 0 ? g_browse_n : 1);
    ui_header("Albums", right, 1);

    /* Hero art (or a placeholder tile when the folder has no folder.art). */
    if (g_detail_art_ok) {
        console_blit565(12, DET_HERO_Y, DET_ART, DET_ART, g_detail_art);
    } else {
        console_fill_rect(12, DET_HERO_Y, DET_ART, DET_ART, LINEN_BORDER);
    }
    int tx = 12 + DET_ART + 12;
    ui_text(tx, DET_HERO_Y + 15, g_album_title, FONT_HEADER, LINEN_INK);
    if (g_album_artist[0]) {
        ui_text(tx, DET_HERO_Y + 31, g_album_artist, FONT_SUB, LINEN_MUTED_D);
    }
    char meta[40];
    int mi = u32_to_dec(meta, (unsigned)g_album_track_n);
    for (const char *p = (g_album_track_n == 1) ? " track" : " tracks"; *p; p++)
        meta[mi++] = *p;
    /* Append total runtime ("11 tracks · 1h 41m") when durations are known
     * (index path). Summed ONCE in detail_load_meta — this runs on every paint.
     * collection-detail.jsx meta line. */
    if (g_detail_total_s > 0) {
        for (const char *p = " " UI_GLYPH_MIDDOT " "; *p; p++) meta[mi++] = *p;
        uint32_t tot_m = (g_detail_total_s + 59u) / 60u;  /* round UP to minutes */
        uint32_t hh = tot_m / 60u, mm = tot_m % 60u;
        if (hh > 0) {
            mi += u32_to_dec(meta + mi, hh);
            meta[mi++] = 'h'; meta[mi++] = ' ';
        }
        mi += u32_to_dec(meta + mi, mm);
        meta[mi++] = 'm';
    }
    meta[mi] = '\0';
    ui_text(tx, DET_HERO_Y + 47, meta, FONT_SMALL, LINEN_MUTED2);

    console_fill_rect(12, DET_LIST_Y0 - 6, LCD_WIDTH - 24, 1, LINEN_BORDER);

    if (g_browse_n == 0) {
        ui_text(14, DET_LIST_Y0 + 14, "Empty folder", FONT_ROW, LINEN_MUTED);
        return;
    }
    /* Scroll over the display view (tracks + any "Disc N" headers), centered on
     * the selected track's position within it. */
    int top = ui_scroll_window(detail_sel_view(sel), g_det_view_n, DET_ROWS);
    for (int r = 0; r < DET_ROWS; r++) {
        int vi = top + r;
        if (vi >= g_det_view_n) break;
        detail_row_draw(r, vi);
    }
    ui_scrollbar(DET_LIST_Y0, top, DET_ROWS, g_det_view_n);
}

/* A neutral tile shown in a row's chip slot until its real cover loads, so the
 * text doesn't shift right when the thumbnail pops in. Filled once at startup. */
static uint16_t g_chip_ph[ARTCACHE_DIM * ARTCACHE_DIM];
/* Diagnostic placeholders — see albumlist_row_draw. */
static uint16_t g_chip_noclus[ARTCACHE_DIM * ARTCACHE_DIM];
static uint16_t g_chip_noart[ARTCACHE_DIM * ARTCACHE_DIM];

static void chip_placeholder_init(void)
{
    for (int i = 0; i < ARTCACHE_DIM * ARTCACHE_DIM; i++) {
        g_chip_ph[i]     = LINEN_BORDER;
        g_chip_noclus[i] = 0xFD20;             /* amber: no cluster resolved   */
        g_chip_noart[i]  = 0xF800;             /* red:   load gave up          */
    }
}

/* Queue every album row's cover into the incremental thumbnail cache. Called
 * when the album list is (re)entered; artcache_pump loads them a few frames
 * apart off the audio path. NOT reset here — artcache_queue is idempotent (a
 * slot whose cover is unchanged keeps its loaded pixels), so backing out of an
 * album detail re-uses the covers already loaded instead of reloading them all.
 * A changed view (e.g. a different artist filter) re-queues only the slots whose
 * album actually changed. */
static void albumlist_queue_chips(void)
{
    for (int i = 0; i < g_albumview_n; i++) {
        int a = g_albumview[i];
        if (a >= ARTCACHE_SLOTS) continue;
        const lib_album_t *al = &g_albums[a];
        artcache_queue(a, al->thm_clus, al->thm_size, al->art_clus, al->art_size);
    }
}

/*
 * The album list carries a synthetic "All Songs" row at index 0 WHEN it is
 * showing a single artist — browsing an artist by album alone makes a track you
 * remember but cannot place hard to reach. Row 0 is the artist's whole
 * discography in title order; rows 1.. are the albums.
 *
 * Everything that indexes the list has to agree about that offset, so it is
 * expressed once here and every caller goes through albumlist_album_at().
 */
static int albumlist_all_row(void)
{
    return g_artist_filter[0] ? 1 : 0;
}

static int albumlist_count(void)
{
    return g_albumview_n + albumlist_all_row();
}

/* Global g_albums[] index for a list row, or -1 for the "All Songs" row. */
static int albumlist_album_at(int row)
{
    int k = row - albumlist_all_row();
    if (k < 0 || k >= g_albumview_n) {
        return -1;
    }
    return g_albumview[k];
}

/* One album row at list-row `r` showing album-view entry `idx`. Split out of
 * albumlist_render so a selection move can repaint just the rows that changed. */
static void albumlist_row_draw(int r, int idx)
{
    int a = albumlist_album_at(idx);
    if (a < 0) {                            /* the synthetic "All Songs" row */
        list_row_tall(r, "All Songs", 0, 0, 1, idx == g_br_sel, 0, 0);
        return;
    }
    const lib_album_t *e = &g_albums[a];
    /* (Re)queue the visible rows every paint — idempotent for a slot that
     * already holds this album, and it's what gets a cover for rows past the
     * bulk prefetch in albumlist_queue_chips when the library has more albums
     * than the cache has slots.
     *
     * Keyed by the GLOBAL album index, not the row: the row number shifts when
     * an artist filter adds the "All Songs" row, and it means a different album
     * under a different filter — so a row-keyed cache aliases across filters. */
    artcache_queue(a, e->thm_clus, e->thm_size, e->art_clus, e->art_size);
    const uint16_t *chip = artcache_get(a);
    if (!chip) {
        /* DIAGNOSTIC: tint the placeholder by WHY there are no pixels, so a
         * blank chip is self-describing on a device with no serial cable.
         *   grey  = still coming (queued)
         *   amber = no sidecar cluster was ever resolved for this album
         *   red   = tried to load it and gave up
         * Remove once the first-album blank is understood. */
        switch (artcache_state(a)) {
        case ARTCACHE_ST_NOCLUS: chip = g_chip_noclus; break;
        case ARTCACHE_ST_NOART:  chip = g_chip_noart;  break;
        default:                 chip = g_chip_ph;     break;
        }
    }
    /* Show "Album" as the title and the artist as a sub-line (parsed from the
     * "Artist - Album" folder name). In an artist's own list the artist sub
     * is redundant, so drop it there. */
    char artist[NAME_MAX + 1], album[NAME_MAX + 1];
    split_artist_album(e->folder, artist, album);
    const char *sub = (!g_artist_filter[0] && artist[0]) ? artist : 0;
    list_row_tall(r, album, sub, 0, 1, idx == g_br_sel, 0, chip);
}

/* Album LIST (browser depth 0): the folder list with the design chrome, each
 * album row carrying a 22x22 cover chip (or a placeholder until it loads). */
static void albumlist_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    int total = albumlist_count();
    if (total > 0) {
        fmt_count(right, sel + 1, total);
    } else {
        right[0] = '\0';
    }
    /* Header title = the artist when drilled in from Artists, else "Albums". */
    ui_header(g_artist_filter[0] ? g_artist_filter : "Albums", right, 1);

    if (total == 0) {
        ui_text(14, LIST_Y0 + 20, "No albums", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, total, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= total) break;
        albumlist_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS2, total);
}

/* ---------------------------------------------------------------------------
 * Artists (menus.jsx ArtistsList): a list of the unique artist prefixes parsed
 * from the "Artist - Album" folder names. Selecting one filters the album list
 * (g_artist_filter) to just that artist's albums.
 * ------------------------------------------------------------------------- */
#define ARTISTS_MAX 512
/* Each artist carries a representative album cover (its first album's) so the
 * Artists list can show a chip, same as albums — indexed, no per-row scan. */
typedef struct {
    char     name[NAME_MAX + 1];
    uint32_t thm_clus, thm_size, art_clus, art_size;
} lib_artist_t;
static lib_artist_t g_artists[ARTISTS_MAX];
static int          g_artists_n;
static int  g_artist_sel, g_artist_accum;

/* Build the unique, de-duplicated artist list from the index-derived album
 * list, then sort it A->Z. De-dup is by artist_key (leading "The " and case
 * ignored), so a differently-cased folder can't split one artist into two rows.
 * Because g_albums is index-driven, artists of stale on-disk folders never
 * appear here at all. */
static void build_artists(void)
{
    g_list_epoch++;
    g_artists_n = 0;
    for (int i = 0; i < g_albums_n; i++) {
        char artist[NAME_MAX + 1], album[NAME_MAX + 1];
        split_artist_album(g_albums[i].folder, artist, album);
        if (!artist[0]) continue;                 /* no "Artist - " prefix        */
        int found = 0;
        for (int j = 0; j < g_artists_n; j++) {
            if (title_cmp(artist_key(g_artists[j].name), artist_key(artist)) == 0) {
                found = 1;
                break;
            }
        }
        if (found) continue;
        if (g_artists_n >= ARTISTS_MAX) { g_lib_truncated = 1; continue; }
        lib_artist_t *a = &g_artists[g_artists_n];
        int k = 0;
        for (; artist[k] && k < NAME_MAX; k++) a->name[k] = artist[k];
        a->name[k]  = '\0';
        a->thm_clus = g_albums[i].thm_clus;   /* first album = artist's chip */
        a->thm_size = g_albums[i].thm_size;
        a->art_clus = g_albums[i].art_clus;
        a->art_size = g_albums[i].art_size;
        g_artists_n++;
    }

    /* Insertion-sort A->Z by artist_key (so "The xyz" files under X); the whole
     * struct moves, so each artist's chip travels with its name. */
    for (int i = 1; i < g_artists_n; i++) {
        lib_artist_t v = g_artists[i];
        int j = i - 1;
        while (j >= 0 && title_cmp(artist_key(g_artists[j].name),
                                   artist_key(v.name)) > 0) {
            g_artists[j + 1] = g_artists[j];
            j--;
        }
        g_artists[j + 1] = v;
    }
}

/* Build the on-screen album slice (indices into the already-sorted g_albums),
 * either all albums (artist_filter NULL/empty) or one artist's — matched by
 * artist_key so the "The "/case-insensitive grouping matches the Artists menu. */
static void albumview_build(const char *artist_filter)
{
    g_list_epoch++;
    g_albumview_n = 0;
    for (int i = 0; i < g_albums_n && g_albumview_n < LIB_MAX_ALBUMS; i++) {
        if (artist_filter && artist_filter[0]) {
            char a[NAME_MAX + 1], b[NAME_MAX + 1];
            split_artist_album(g_albums[i].folder, a, b);
            if (title_cmp(artist_key(a), artist_key(artist_filter)) != 0) continue;
        }
        g_albumview[g_albumview_n++] = (uint16_t)i;
    }
}

static void artists_row_draw(int r, int idx)
{
    list_row(r, g_artists[idx].name, 0, 0, 1, idx == g_artist_sel, 0, 0);
}

static void artists_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_artists_n > 0) fmt_count(right, sel + 1, g_artists_n);
    else                 right[0] = '\0';
    ui_header("Artists", right, 1);

    if (g_artists_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No artists", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, g_artists_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_artists_n) break;
        artists_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, g_artists_n);
}

static void browse_render(int sel)
{
    if (g_dir_depth > 0) {
        detail_render(sel);
    } else {
        albumlist_render(sel);
    }
}

/* ---------------------------------------------------------------------------
 * Library index (Songs / Genres): a one-time tag scan of every FLAC in the
 * library into RAM. The anti-skip buffer (~30 s) covers playback while the scan
 * hits the disk, so it can run without dropouts. Rebuilt only once per session.
 * ------------------------------------------------------------------------- */
#define LIB_MAX_SONGS  6000
#define LIB_MAX_GENRES 128
#define LIB_TITLE_MAX  48
#define LIB_GENRE_MAX  24

#define LIB_ARTIST_MAX 40
/* Same cap as a browse row (NAME_MAX + NUL): once a song binds to its file,
 * file[] is the on-disk stem exactly as copy_display_name() produces it for
 * the row and the queue, so the two are byte-identical and hash alike. */
#define LIB_FILE_MAX   (NAME_MAX + 1)

typedef struct {
    char     title[LIB_TITLE_MAX];
    char     artist[LIB_ARTIST_MAX];
    /*
     * The track's filename, extension trimmed — for DISPLAY (the queue entry,
     * hence Now Playing) and nothing else. Until the record binds to a file it
     * holds the index's copy; after, the on-disk stem (resolve_art_cb).
     *
     * Never a key. The index field is the first 63 bytes of a name that may be
     * longer, and the device used to trim its extension by searching for the
     * LAST '.' in that truncated string: past ~68 bytes the ".flac" was gone,
     * the cut landed on an interior dot, and "16. TRAGIC (feat. ..." became
     * "16" — a name no directory entry could ever equal. The record still
     * played (file_hash bound it) but its row showed no duration, no title,
     * the wrong gutter number, and resume never found it. Every binding now
     * goes through a hash of the FULL on-disk name, or the cluster it bound.
     */
    char     file[LIB_FILE_MAX];
    uint32_t file_hash;                   /* name_hash of the FULL on-disk name,  */
                                          /* extension included: the record<->  */
                                          /* file locator (host-stamped; the    */
                                          /* scan path computes it at readdir)  */
    uint32_t stem_hash;                   /* name_hash of the on-disk stem — the  */
                                          /* resume locator. Provisional (from  */
                                          /* file[]) until the record binds     */
    uint32_t dir_clus;                    /* album folder (queue context + play)*/
    uint32_t file_clus, file_size;        /* the track file itself (resolved at   */
                                          /* load) — play/shuffle without a scan  */
    uint32_t duration_s;
    uint16_t track, disc;
    int16_t  genre;                       /* index into g_genres, -1 = none     */
} lib_song_t;

static lib_song_t g_songs[LIB_MAX_SONGS];
static int        g_songs_n;
static uint16_t   g_song_sorted[LIB_MAX_SONGS];   /* song indices, title order  */
static char       g_genres[LIB_MAX_GENRES][LIB_GENRE_MAX];
static int        g_genres_n;
static int        g_genre_count[LIB_MAX_GENRES]; /* songs per genre (precomputed) */
static int        g_lib_scanned;

/* Root-folder name -> cluster map (built once), for resolving an index record's
 * album folder to a cluster without a per-album directory read. */
/* MUST be >= LIB_MAX_ALBUMS: a record whose album folder didn't make the map
 * resolves to cluster 0 and every one of its tracks is silently dropped at load
 * (the old 160 cap quietly lost albums 161..256 of a full library). */
#define FOLDER_MAP_MAX LIB_MAX_ALBUMS
static struct { char name[NAME_MAX + 1]; uint32_t clus, hash; } g_folder_map[FOLDER_MAP_MAX];
static int      g_folder_n;
/* folder_hash -> g_folder_map chains (index+1, 0 = end of chain), built by
 * folder_map_index() once the root walk is complete. folder_clus_h() used to
 * walk the whole map for EVERY record — on a full library ~3M hash compares
 * on the boot path, under the "Loading Library" bar. */
#define FOLDER_HASH_BUCKETS 2048         /* power of two > FOLDER_MAP_MAX      */
static uint16_t g_folder_hh[FOLDER_HASH_BUCKETS];
static uint16_t g_folder_hn[FOLDER_MAP_MAX];
static uint32_t g_idx_clus, g_idx_size;           /* CORELIB.IDX location        */

/* Scan temporaries (kept off the browser's g_browse). */
static uint32_t   g_scan_dirs[BROWSE_MAX];
static int        g_scan_dirs_n;
/* hash: name_hash over the FULL on-disk name, taken while the directory entry
 * is in hand — the same locator the index path gets from the host, so the
 * resolve pass binds scanned songs the same way and needs no name compare. */
typedef struct { char name[NAME_MAX + 1]; uint32_t clus, size, hash; } scan_file_t;
static scan_file_t g_scan_files[BROWSE_MAX];
static int         g_scan_files_n;
static uint32_t    g_scan_art_clus, g_scan_art_size;

/* Filtered, on-screen song list (all songs, or one genre's). */
static uint16_t   g_songview[LIB_MAX_SONGS];
static int        g_songview_n;
static int        g_song_sel, g_song_accum;
static int        g_genre_sel, g_genre_accum;

static int16_t genre_intern(const char *g)
{
    if (!g[0]) return -1;
    for (int i = 0; i < g_genres_n; i++) {
        if (name_eq_ci(g_genres[i], g)) return (int16_t)i;
    }
    if (g_genres_n < LIB_MAX_GENRES) {
        int k = 0;
        for (; g[k] && k < LIB_GENRE_MAX - 1; k++) g_genres[g_genres_n][k] = g[k];
        g_genres[g_genres_n][k] = '\0';
        return (int16_t)g_genres_n++;
    }
    g_lib_truncated = 1;               /* out of genre slots */
    return -1;
}

/* ---------------------------------------------------------------------------
 * Lookup indexes: album by folder cluster, song by file hash
 *
 * Chained hash buckets, stored as index+1 so 0 means "end of chain". Both
 * used to be built once, AFTER the load, in library_finish — which fixed the
 * per-use lookups (the resolve pass, the queue builders) but left the load
 * itself linear: album_intern walked every album already listed to de-dupe
 * each record, and folder_clus_h walked the folder map for each one. On a
 * full library that is ~3M compares apiece, on an 80 MHz ARM7, while the user
 * watches "Loading Library". The album buckets are therefore maintained AS
 * albums are added (and rebuilt after the sort moves them); the song buckets
 * are built once the songs are all in, by lookup_build.
 * ------------------------------------------------------------------------- */
#define SONG_HASH_BUCKETS 2048           /* power of two > LIB_MAX_SONGS       */
#define ALBUM_HASH_BUCKETS 512           /* power of two > LIB_MAX_ALBUMS      */

static uint16_t g_song_hh[SONG_HASH_BUCKETS];
static uint16_t g_song_hn[LIB_MAX_SONGS];
static uint16_t g_album_hh[ALBUM_HASH_BUCKETS];
static uint16_t g_album_hn[LIB_MAX_ALBUMS];

static void album_bucket_add(int i)
{
    uint32_t b = g_albums[i].clus & (ALBUM_HASH_BUCKETS - 1);
    g_album_hn[i] = g_album_hh[b];
    g_album_hh[b] = (uint16_t)(i + 1);
}

/* Album index by folder cluster, or -1. O(1): the chain for a cluster holds
 * ~2 albums on a full library. Valid at every moment — the buckets are reset
 * with the album list (albums_reset) and updated by album_intern — so the
 * loader can use it to de-dupe, not only the queue builders afterwards. */
static int album_by_clus(uint32_t dc)
{
    for (int i = g_album_hh[dc & (ALBUM_HASH_BUCKETS - 1)]; i; i = g_album_hn[i - 1]) {
        if (g_albums[i - 1].clus == dc) return i - 1;
    }
    return -1;
}

/* Empty the album list. The ONLY way to: g_albums_n = 0 alone would leave the
 * buckets pointing at stale entries, and album_by_clus would keep answering
 * for albums that are no longer there. */
static void albums_reset(void)
{
    g_albums_n = 0;
    for (int i = 0; i < ALBUM_HASH_BUCKETS; i++) g_album_hh[i] = 0;
}

/* Add (folder, cluster) to the index-derived album list, de-duped by cluster. */
static void album_intern(const char *folder, uint32_t clus)
{
    if (clus == 0) return;
    if (album_by_clus(clus) >= 0) return;          /* already listed */
    if (g_albums_n >= LIB_MAX_ALBUMS) { g_lib_truncated = 1; return; }
    int k = 0;
    for (; folder[k] && k < NAME_MAX; k++) g_albums[g_albums_n].folder[k] = folder[k];
    g_albums[g_albums_n].folder[k] = '\0';
    g_albums[g_albums_n].clus = clus;
    g_albums[g_albums_n].unreadable = 0;     /* until the resolve pass says so */
    album_bucket_add(g_albums_n);
    g_albums_n++;
}

static int scan_dirs_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (!e->is_dir || is_junk_dir(e->name)) return 0;
    if (g_scan_dirs_n < BROWSE_MAX) g_scan_dirs[g_scan_dirs_n++] = e->first_clus;
    album_intern(e->name, e->first_clus);          /* scan fallback album list */
    return 0;
}

static int scan_files_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (!e->is_dir && name_eq_ci(e->name, "folder.art")) {
        g_scan_art_clus = e->first_clus;
        g_scan_art_size = e->size;
        return 0;
    }
    if (e->is_dir || classify_ext(e->name) < 0) return 0;   /* playable files */
    if (g_scan_files_n < BROWSE_MAX) {
        scan_file_t *f = &g_scan_files[g_scan_files_n++];
        copy_display_name(f->name, e->name, 1);
        f->clus = e->first_clus;
        f->size = e->size;
        f->hash = name_hash(e->name);          /* the locator: FULL name, with ext */
    }
    return 0;
}

/* Copy a fixed-length index field (NUL-terminated within it) to a bounded C
 * string, preserving UTF-8 (the atlas covers Latin-1 + smart punctuation); only
 * C0 control bytes become spaces. Byte-bounded — a split multibyte tail just
 * renders as one U+FFFD. */
static void field_copy(char *dst, int dcap, const uint8_t *src, int slen)
{
    int i = 0;
    for (; i < slen && i < dcap - 1 && src[i]; i++) {
        unsigned char c = src[i];
        dst[i] = (c >= 0x20) ? (char)c : ' ';   /* keep ASCII + all UTF-8 bytes */
    }
    dst[i] = '\0';
}

/* Root enumeration for the index path: capture CORELIB.IDX + a folder->cluster
 * map (so records resolve to a cluster with no per-album directory read). */
static int index_root_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (!e->is_dir && name_eq_ci(e->name, "CORELIB.IDX")) {
        g_idx_clus = e->first_clus;
        g_idx_size = e->size;
        return 0;
    }
    if (e->is_dir && !is_junk_dir(e->name)) {
        if (g_folder_n >= FOLDER_MAP_MAX) { g_lib_truncated = 1; return 0; }
        copy_display_name(g_folder_map[g_folder_n].name, e->name, 0);
        /* The index's folder[] field is 64 bytes, so the host writer stores at
         * most 63 chars + NUL (build_index.py ascii_field). Cap the on-disk name
         * to the same 63 chars here, else a >63-char album folder never matches
         * (device kept 64, host kept 63) and all its tracks silently drop. */
        g_folder_map[g_folder_n].name[NAME_MAX - 1] = '\0';
        g_folder_map[g_folder_n].clus = e->first_clus;
        /* Locator hash over the FULL on-disk name (untruncated, folded), so it
         * matches the record's folder_hash even for >63-char folders. */
        g_folder_map[g_folder_n].hash = name_hash(e->name);
        g_folder_n++;
    }
    return 0;
}

/* Chain the folder map by hash. Built after the root walk rather than inside
 * index_root_cb because lib_readdir rewinds g_folder_n and re-runs the walk
 * on a retry, and a chain built during a walk that was then thrown away would
 * point at entries that no longer exist. */
static void folder_map_index(void)
{
    for (int i = 0; i < FOLDER_HASH_BUCKETS; i++) g_folder_hh[i] = 0;
    for (int i = 0; i < g_folder_n; i++) {
        uint32_t b = g_folder_map[i].hash & (FOLDER_HASH_BUCKETS - 1);
        g_folder_hn[i] = g_folder_hh[b];
        g_folder_hh[b] = (uint16_t)(i + 1);
    }
}

/* Resolve an index record's album folder to a cluster. Primary: match the
 * record's precomputed folder_hash (quote/case-folded) against the on-disk
 * folder hashes — one bucket, a chain of ~1. Fallback: the legacy
 * case-insensitive name compare, so a hash mismatch can never regress below
 * the old behaviour; it is linear, but only an orphaned record (or a fold
 * disagreement the parity test exists to prevent) gets that far. */
static uint32_t folder_clus_h(uint32_t hash, const char *name)
{
    for (int i = g_folder_hh[hash & (FOLDER_HASH_BUCKETS - 1)]; i; i = g_folder_hn[i - 1]) {
        if (g_folder_map[i - 1].hash == hash) return g_folder_map[i - 1].clus;
    }
    for (int i = 0; i < g_folder_n; i++) {
        if (name_eq_ci(g_folder_map[i].name, name)) return g_folder_map[i].clus;
    }
    return 0;
}

/* A titled loading screen with a determinate progress bar (0..100%). Rendered
 * from the library load phases so a multi-second first-load shows real progress
 * instead of a frozen splash. */
/*
 * Rate-limited so callers can call it as often as they like.
 *
 * Each call is a console_clear (76,800 pixel writes) plus a full 153,600-byte
 * lcd_present_fb with IRQs masked. Callers sample on an iteration count, which
 * scales with the library rather than with what an eye can follow: the boot
 * resolve pass paints every 4th album, so a 256-album library paid 64 full-frame
 * repaints — a large slice of "Loading Library" was the bar drawing itself.
 *
 * A progress bar needs a handful of updates per second, so drop anything inside
 * LOAD_BAR_MIN_GAP_US of the last paint. 0% and 100% always paint, so the screen
 * still appears immediately and always finishes filled.
 */
#define LOAD_BAR_MIN_GAP_US 120000u

static uint32_t g_load_bar_last_us;
static int      g_load_bar_last_pct = -1;

static void load_bar(const char *title, int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    if (pct != 0 && pct != 100) {
        uint32_t now = mmio_read32(USEC_TIMER_ADDR);
        if (pct == g_load_bar_last_pct ||
            (uint32_t)(now - g_load_bar_last_us) < LOAD_BAR_MIN_GAP_US) {
            return;
        }
    }
    g_load_bar_last_us  = mmio_read32(USEC_TIMER_ADDR);
    g_load_bar_last_pct = pct;

    console_clear(LINEN_SURFACE);
    ui_text_centered(112, title, FONT_TITLE, LINEN_INK);
    int bx = 60, by = 138, bw = LCD_WIDTH - 120, bh = 6;
    ui_round_rect(bx, by, bw, bh, 3, LINEN_BORDER);
    if (pct > 0) {
        int fw = bw * pct / 100;
        if (fw < bh) fw = bh;                 /* keep the rounded cap visible */
        ui_round_rect(bx, by, fw, bh, 3, LINEN_ACCENT);
    }
    lcd_present_fb(console_framebuffer());
}

/*
 * Deferred progress bar.
 *
 * load_bar() is expensive: console_clear() is 76,800 pixel writes and
 * lcd_present_fb() streams the whole 153,600-byte frame with IRQs masked. The
 * queue builders called it every 64 songs, so picking a track out of Songs paid
 * ~19 full-frame repaints — and since the queue build is now O(n) with a hash
 * lookup and a struct copy per entry, the BAR was most of the wait. Reporting
 * progress cost more than the work it was reporting on.
 *
 * So: stay silent until the job has actually run long enough to need feedback,
 * then behave normally. A library that enqueues in 30 ms shows nothing; one slow
 * enough to look hung still gets a bar.
 */
#define LOAD_BAR_DELAY_US 250000u

static uint32_t g_load_bar_t0;
static int      g_load_bar_shown;

static void load_bar_begin(void)
{
    g_load_bar_t0    = mmio_read32(USEC_TIMER_ADDR);
    g_load_bar_shown = 0;
}

static void load_bar_progress(const char *title, int pct)
{
    if (!g_load_bar_shown) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - g_load_bar_t0)
            < LOAD_BAR_DELAY_US) {
            return;
        }
        g_load_bar_shown = 1;                 /* crossed the threshold: show it */
    }
    load_bar(title, pct);
}

/* Songs are keyed by file_hash — the folded hash of the FULL on-disk
 * filename, extension included, which is the one form both sides have
 * exactly: the host stamped it from the name it gave the file, the resolve
 * pass hashes the directory entry. (They used to be keyed by a hash of the
 * ext-trimmed file[] field, which for a long name is a hash of a truncated
 * string that nothing on the disk produces — the bucket missed and a linear
 * sweep quietly made up the difference.) Albums: rebuilt here because the
 * album sort in library_finish has just moved them. */
static void lookup_build(void)
{
    for (int i = 0; i < SONG_HASH_BUCKETS; i++)  g_song_hh[i]  = 0;
    for (int i = 0; i < ALBUM_HASH_BUCKETS; i++) g_album_hh[i] = 0;
    for (int i = 0; i < g_songs_n; i++) {
        uint32_t b = g_songs[i].file_hash & (SONG_HASH_BUCKETS - 1);
        g_song_hn[i] = g_song_hh[b];
        g_song_hh[b] = (uint16_t)(i + 1);
    }
    for (int i = 0; i < g_albums_n; i++) album_bucket_add(i);
}

/* ---------------------------------------------------------------------------
 * Directory reads the library depends on.
 *
 * Every fat32_readdir in the loader used to ignore its return code. The walk
 * can fail — FAT32_EIO when a sector read fails even after player_disk_read's
 * six attempts, FAT32_ECORRUPT for a cyclic or unaddressable chain — and not
 * one caller looked. The consequence was never a crash, which is why it went
 * unnoticed: a failed album walk in the resolve pass just left every song in
 * that album with file_clus == 0, and every later path reads that as "indexed
 * but no longer on disk". The tracks stayed listed in Songs and Genres and
 * did nothing when picked, until reboot; opening the album showed an empty
 * tracklist. The trigger is mundane — the drive still settling from spin-up
 * during "Loading Library" — so this is a bug users hit, not a hypothetical.
 *
 * lib_readdir is fat32_readdir with the two things the loader needs:
 *
 *   1. A RETRY. player_disk_read already retries each sector six times with
 *      short backoffs, so a walk that failed has already burned ~200 ms on
 *      the failing sector; a further LIB_READDIR_RETRY_MS pause and a fresh
 *      walk gives a drive that was mid-settle its second chance. Only an EIO
 *      is retried: ECORRUPT is structural (the bytes are wrong, not late) and
 *      re-walking a cyclic chain costs the full bounded scan again for
 *      nothing. The retry re-runs the callback from the top of the directory,
 *      so the caller's accumulator is REWOUND first (`acc_n`) — otherwise the
 *      entries seen before the failure would be listed twice.
 *
 *   2. A BUDGET. A dead drive fails every read; with a thousand albums to
 *      resolve, retrying each one would turn a failed boot into minutes of
 *      sleeping before the user even sees an error. g_lib_retry_budget is the
 *      number of retries a whole load may spend: a spin-up is one event and a
 *      couple of retries ride it out, so if the budget is gone and reads are
 *      still failing, this is not spin-up and the remaining albums are marked
 *      unreadable at the cost of one plain (already-retried) walk each.
 *
 * The common case — the walk succeeds first time — pays one compare. The
 * loader is not slower for it.
 *
 * On a final failure the accumulator is rewound too: a half-listed directory
 * is not "the files in this album", and presenting it as such is a subtler
 * version of the bug this replaces. The return is the last FAT32_* code.
 * ------------------------------------------------------------------------- */
#define LIB_READDIR_RETRIES   2      /* re-walks per directory, after the first  */
#define LIB_READDIR_RETRY_MS  250    /* settle time before each re-walk           */
#define LIB_LOAD_RETRY_BUDGET 6      /* re-walks a whole library load may spend   */

static int g_lib_retry_budget;

static int lib_readdir(fat32_t *fs, uint32_t clus, fat32_dir_cb cb, void *ud,
                       int *acc_n)
{
    int base = acc_n ? *acc_n : 0;
    int rc   = fat32_readdir(fs, clus, cb, ud);
    for (int attempt = 0;
         rc == FAT32_EIO && attempt < LIB_READDIR_RETRIES && g_lib_retry_budget > 0;
         attempt++) {
        g_lib_retry_budget--;
        sleep_ms(LIB_READDIR_RETRY_MS);
        if (acc_n) *acc_n = base;
        rc = fat32_readdir(fs, clus, cb, ud);
    }
    if (rc != 0 && acc_n) *acc_n = base;
    return rc;
}

/* Resolve pass: ONE readdir per album at load time captures both the folder's
 * cover clusters (folder.art/.thm) AND every track file's own cluster into
 * g_songs — so later cover loads, playing a song, and shuffling the WHOLE
 * library are all direct reads with no per-use directory scan. */
static uint32_t g_res_art_clus, g_res_art_size, g_res_thm_clus, g_res_thm_size;
static uint32_t g_res_album_clus;
static int resolve_art_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (e->is_dir) return 0;
    if (name_eq_ci(e->name, "folder.thm")) {
        g_res_thm_clus = e->first_clus; g_res_thm_size = e->size; return 0;
    }
    if (name_eq_ci(e->name, "folder.art")) {
        g_res_art_clus = e->first_clus; g_res_art_size = e->size; return 0;
    }
    if (classify_ext(e->name) < 0) return 0;      /* a playable track: bind its cluster */
    /* The locator, and the only test: the folded hash of the FULL on-disk name
     * equals the record's file_hash (stamped by build_index.py over the name it
     * gave the file; computed at readdir for a scanned song), within this
     * album, for a record not yet bound. No name compare and no fallback — a
     * record that hashes to no file in its folder is not on the disk. The old
     * "hash didn't line up" sweep over all songs existed to rescue the long
     * names whose truncated file[] could not match the bucket key; that was
     * ~6000 compares per such file at every boot, and it is what let the
     * truncation stay invisible for as long as it did. */
    uint32_t fh = name_hash(e->name);
    if (fh == 0) return 0;                        /* 0 is "no locator", never a match */
    for (int i = g_song_hh[fh & (SONG_HASH_BUCKETS - 1)]; i; i = g_song_hn[i - 1]) {
        lib_song_t *s = &g_songs[i - 1];
        if (s->file_clus || s->dir_clus != g_res_album_clus || s->file_hash != fh) continue;
        s->file_clus = e->first_clus;
        s->file_size = e->size;
        /* Bound. From here on the song is shown and located by its ON-DISK name:
         * the stem, capped exactly as a browse row is (same function, same
         * NAME_MAX), so the queue entry, the tracklist row and this field are
         * the same bytes — and its hash is what resume_capture will store. */
        copy_display_name(s->file, e->name, 1);
        s->stem_hash = name_hash(s->file);
        return 0;
    }
    return 0;
}
/*
 * Resolve ONE album: read its folder, bind its songs' clusters and capture its
 * art. Returns the readdir result. This is the one place an album is marked
 * unreadable or cleared again, so the count in g_lib_unreadable can never
 * drift from the flags — the load-time pass and a later browse_load both come
 * through here.
 *
 * On failure the art fields are left cleared (there is nothing to show) and
 * songs that happened to be bound before the walk failed KEEP their binding:
 * those entries were read correctly, and unbinding a playable track because a
 * sibling's sector was bad would be manufacturing a second failure.
 */
static int album_resolve(fat32_t *fs, int i)
{
    g_res_art_clus = g_res_art_size = g_res_thm_clus = g_res_thm_size = 0;
    g_res_album_clus = g_albums[i].clus;
    int rc = lib_readdir(fs, g_albums[i].clus, resolve_art_cb, 0, 0);
    if (rc != 0) {
        if (!g_albums[i].unreadable) {
            g_albums[i].unreadable = 1;
            g_lib_unreadable++;
        }
        g_albums[i].art_clus = g_albums[i].art_size = 0;
        g_albums[i].thm_clus = g_albums[i].thm_size = 0;
        return rc;
    }
    if (g_albums[i].unreadable) {
        g_albums[i].unreadable = 0;
        g_lib_unreadable--;
    }
    g_albums[i].art_clus = g_res_art_clus;
    g_albums[i].art_size = g_res_art_size;
    g_albums[i].thm_clus = g_res_thm_clus;
    g_albums[i].thm_size = g_res_thm_size;
    return 0;
}

static void library_resolve_art(fat32_t *fs)
{
    for (int s = 0; s < g_songs_n; s++) g_songs[s].file_clus = 0;
    for (int a = 0; a < g_albums_n; a++) g_albums[a].unreadable = 0;
    g_lib_unreadable = 0;

    /*
     * Walk the albums in ASCENDING CLUSTER order, not menu order.
     *
     * This pass reads one directory per album — up to LIB_MAX_ALBUMS of them —
     * and each read is a seek to wherever that folder's directory cluster
     * happens to live. Album (alphabetical) order bears no relation to on-disk
     * order, so the head was being thrown back and forth across an 80 GB platter
     * a couple of hundred times, and seek time, not transfer time, dominated
     * "Loading Library". Sorted, the same reads become a single forward sweep,
     * which is also the order the drive's own readahead can help with.
     *
     * Results are written back through the original index, so nothing outside
     * this function sees a different order.
     */
    static uint16_t order[LIB_MAX_ALBUMS];
    int n = g_albums_n;
    for (int i = 0; i < n; i++) order[i] = (uint16_t)i;
    for (int i = 1; i < n; i++) {             /* insertion sort: n <= 256 */
        uint16_t v = order[i];
        uint32_t k = g_albums[v].clus;
        int j = i - 1;
        while (j >= 0 && g_albums[order[j]].clus > k) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }

    for (int p = 0; p < n; p++) {
        int i = order[p];
        /* load_bar is time-throttled, so calling it per album is free and the
         * bar advances smoothly instead of in 4-album jumps. */
        load_bar("Loading Library", 75 + (n ? p * 25 / n : 25));
        (void)album_resolve(fs, i);    /* failure is recorded on the album */
    }
}

/* Why the last index load was refused (an IDX_* code), for the About screen
 * and the UART. A refused index is not silent: the device falls back to the
 * tag scan, which takes minutes, and the user deserves to know it was the
 * file and not the disk. */
static int g_lib_idx_reject;

/* Refuse the index: log why, discard whatever was parsed before the check
 * failed (a CRC is only known at the end, by which time the records are in
 * g_songs), and return the loader's "fall back to a scan" result. */
static int idx_reject(int why)
{
    static const char *const names[] = {
        "ok", "magic", "version", "recsize", "size", "crc", "read"
    };
    g_lib_idx_reject = why;
    uart_puts("idx: rejected (");
    uart_puts(names[why]);
    uart_puts(")\n");
    g_songs_n = g_genres_n = 0;
    albums_reset();
    g_lib_orphaned = 0;
    return 0;
}

/* Load the whole library from the host-built CORELIB.IDX in ONE streamed pass
 * (no per-file tag reads) — instant Songs/Genres/durations/disc. Returns 1 on
 * success, 0 if the index is absent/bad (caller falls back to a scan).
 * Record (256B, LE): u32 dur, u16 track, u16 disc, folder[64], file[64],
 * title[48], artist[40], genre[24], u32 folder_hash, u32 file_hash. */
static void library_finish(void);        /* sort + genre counts (shared)        */

/* The library root: the "Music" folder if present, else the volume root (kept
 * for back-compat). Music/ holds the album folders + CORELIB.IDX, so the FAT
 * root stays clean (core.ipod / loader.cfg / Apple system folders only). */
static uint32_t g_lib_root_clus;
static int lib_root_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (e->is_dir && name_eq_ci(e->name, "Music")) {
        g_lib_root_clus = e->first_clus;
    }
    return 0;
}
static uint32_t lib_root(fat32_t *fs)
{
    g_lib_root_clus = 0;
    /* If the volume root cannot be read there is no finding Music/, and the
     * fallback to the root cluster below will fail the same way one call later.
     * Record it so the load reports "could not read the disk" rather than
     * quietly producing a library of zero songs. */
    int rc = lib_readdir(fs, fs->root_clus, lib_root_cb, 0, 0);
    if (rc != 0) g_lib_load_err = rc;
    return g_lib_root_clus ? g_lib_root_clus : fs->root_clus;
}

static int library_load_index(fat32_t *fs)
{
    g_idx_clus = 0;
    g_folder_n = 0;
    /* This one walk is the whole folder map: if it fails, every index record
     * resolves to "album not on disk" and the library silently loads EMPTY —
     * the whole-library version of the per-album bug. Rewinding g_folder_n
     * on failure (lib_readdir does) matters here for the same reason: a
     * half-built map would resolve half the library and orphan the rest. */
    int rc = lib_readdir(fs, lib_root(fs), index_root_cb, 0, &g_folder_n);
    if (rc != 0) {
        g_lib_load_err = rc;
        return 0;
    }
    if (g_idx_clus == 0) return 0;
    folder_map_index();                    /* the walk is final: chain it     */
    g_lib_idx_reject = IDX_OK;

    fat32_stream_t st;
    fat32_stream_open(&st, fs, g_idx_clus, g_idx_size);
    uint8_t hdr[IDX_HDR_V2];
    if (fat32_stream_read(&st, hdr, IDX_HDR_V1) != (int32_t)IDX_HDR_V1) {
        return idx_reject(IDX_EREAD);
    }
    /* A v2 header is four bytes longer; on a v1 file those bytes are record 0
     * and the stream cannot rewind, so read them only once the version says
     * they are there. idx_header_parse validates the version. */
    if ((hdr[4] | (hdr[5] << 8)) == 2 &&
        fat32_stream_read(&st, hdr + IDX_HDR_V1, IDX_HDR_V2 - IDX_HDR_V1)
            != (int32_t)(IDX_HDR_V2 - IDX_HDR_V1)) {
        return idx_reject(IDX_EREAD);
    }
    idx_hdr_t h;
    int hrc = idx_header_parse(hdr, g_idx_size, &h);
    if (hrc != IDX_OK) return idx_reject(hrc);
    uint32_t count = h.count;
    uint32_t crc   = 0xFFFFFFFFu;

    g_songs_n = g_genres_n = 0;
    albums_reset();
    g_lib_truncated = 0;
    /* Read the index in 16 KB batches (64 records) rather than 256 B at a time:
     * a 256 B stream read pulls a whole 2048 B FS-sector and hands back 256 B, so
     * per-record reads re-fetched each sector 8x. Batching is ~14 big reads for
     * the whole index instead of ~900 tiny ones — the bulk of the load time. */
    static uint8_t idxbuf[64 * 256];
    uint32_t n = 0;
    while (n < count && g_songs_n < LIB_MAX_SONGS) {
        uint32_t batch = count - n;
        if (batch > 64) batch = 64;
        int32_t got = fat32_stream_read(&st, idxbuf, batch * 256u);
        if (got <= 0) break;
        uint32_t recs = (uint32_t)got / 256u;
        if (recs == 0) break;
        if (h.has_crc) crc = crc32_update(crc, idxbuf, recs * 256u);
        for (uint32_t k = 0; k < recs && g_songs_n < LIB_MAX_SONGS; k++) {
            const uint8_t *r = idxbuf + k * 256u;
            char folder[NAME_MAX + 1];
            field_copy(folder, sizeof folder, r + 8, 64);
            uint32_t folder_hash = (uint32_t)r[248] | ((uint32_t)r[249] << 8) |
                                   ((uint32_t)r[250] << 16) | ((uint32_t)r[251] << 24);
            uint32_t file_hash   = (uint32_t)r[252] | ((uint32_t)r[253] << 8) |
                                   ((uint32_t)r[254] << 16) | ((uint32_t)r[255] << 24);
            uint32_t dc = folder_clus_h(folder_hash, folder);
            if (dc == 0) {
                /* The index names an album folder the disk does not have: a
                 * stale CORELIB.IDX (album deleted, importer not re-run), not
                 * a read fault — the folder map above was read in full. The
                 * record is dropped, but counted, so the user can be told the
                 * index is out of date rather than wondering where it went. */
                g_lib_orphaned++;
                continue;
            }
            lib_song_t *s = &g_songs[g_songs_n];
            s->dir_clus   = dc;
            s->file_hash  = file_hash;
            s->duration_s = (uint32_t)r[0] | ((uint32_t)r[1] << 8) |
                            ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
            s->track = (uint16_t)(r[4] | (r[5] << 8));
            s->disc  = (uint16_t)(r[6] | (r[7] << 8));
            field_copy(s->title,  LIB_TITLE_MAX,  r + 136, 48);
            field_copy(s->artist, LIB_ARTIST_MAX, r + 184, 40);
            field_copy(s->file,   LIB_FILE_MAX,   r + 72,  64);
            trim_audio_ext(s->file);           /* display placeholder, see there */
            /* Provisional resume locator, replaced by the on-disk stem's hash
             * when the record binds. Exact for a name that fit the field. For
             * one that did not it is the hash of a truncated string no
             * directory entry produces — so it can never resume the wrong
             * file, and it still counts as a same-named twin in
             * resume_find_song's ambiguity rule if the record never binds
             * (an unreadable album), which is the conservative side. */
            s->stem_hash = name_hash(s->file);
            char genre[LIB_GENRE_MAX];
            field_copy(genre, LIB_GENRE_MAX, r + 224, 24);
            s->genre = genre_intern(genre);
            g_songs_n++;
            album_intern(folder, dc);          /* album has >=1 indexed song */
        }
        n += recs;
        load_bar("Loading Library",            /* first ~75% = reading the index */
                 count ? (int)(n * 75u / count) : 0);
    }
    if (n < count) g_lib_truncated = 1;    /* ran out of song slots (or of index) */
    /* The CRC is over ALL the records, so it can only be checked when all of
     * them streamed past. A load cut short by LIB_MAX_SONGS has not read them
     * all — it is already flagged truncated, and the host refuses to write an
     * index over the cap, so this is the one case that rides on the size
     * check alone rather than reading the rest of the file for nothing. */
    if (h.has_crc && n == count && ~crc != h.crc) return idx_reject(IDX_ECRC);
    library_finish();
    library_resolve_art(fs);               /* index each album's cover clusters */
    g_lib_scanned = 1;
    return 1;
}

/* Walk the library root (Music/ or the volume root) → album folders → FLAC
 * files, probing each file's tags. */
static void library_scan(fat32_t *fs)
{
    if (g_lib_scanned) return;
    g_songs_n = g_genres_n = g_scan_dirs_n = 0;
    albums_reset();
    g_lib_truncated = 0;
    /* album_intern de-dupes by cluster, so the retry's re-walk of the folder
     * list is idempotent for g_albums; only g_scan_dirs needs the rewind. */
    int rc = lib_readdir(fs, lib_root(fs), scan_dirs_cb, 0, &g_scan_dirs_n);
    if (rc != 0) g_lib_load_err = rc;
    if (g_scan_dirs_n >= BROWSE_MAX) g_lib_truncated = 1;   /* folder list full */
    for (int d = 0; d < g_scan_dirs_n && g_songs_n < LIB_MAX_SONGS; d++) {
        g_scan_files_n = 0;
        g_scan_art_clus = g_scan_art_size = 0;
        /* A failed file walk leaves this album with no songs in the scan; the
         * resolve pass below reads the folder again and is where it gets
         * marked unreadable (or not, if the disk has settled by then — in
         * which case the album browses fine but its tracks are missing from
         * Songs until the next boot; the index path, which ships, has no such
         * gap because its song list does not come from this walk). */
        (void)lib_readdir(fs, g_scan_dirs[d], scan_files_cb, 0, &g_scan_files_n);
        for (int i = 0; i < g_scan_files_n && g_songs_n < LIB_MAX_SONGS; i++) {
            player_pump();                 /* keep audio fed during the scan     */
            if ((g_songs_n & 31) == 0) {   /* live progress every 32 tracks       */
                console_clear(LINEN_SURFACE);
                ui_text_centered(112, "Building Library", FONT_TITLE, LINEN_INK);
                char pg[16];
                int pl = u32_to_dec(pg, (unsigned)g_songs_n);
                for (const char *p = " songs"; *p; p++) pg[pl++] = *p;
                pg[pl] = '\0';
                ui_text_centered(134, pg, FONT_SUB, LINEN_MUTED);
                lcd_present_fb(console_framebuffer());
            }
            flac_meta_t m;
            int ok = (player_probe_meta(g_scan_files[i].clus,
                                        g_scan_files[i].size, &m) == 0);
            lib_song_t *s = &g_songs[g_songs_n++];
            const char *t = (ok && m.have && m.title[0]) ? m.title
                                                         : g_scan_files[i].name;
            field_copy(s->title, LIB_TITLE_MAX, (const uint8_t *)t,
                       (int)sizeof s->title);
            if (ok && m.have && m.artist[0])
                field_copy(s->artist, LIB_ARTIST_MAX, (const uint8_t *)m.artist, 64);
            else
                s->artist[0] = '\0';
            field_copy(s->file, LIB_FILE_MAX,
                       (const uint8_t *)g_scan_files[i].name, NAME_MAX);
            s->file_hash  = g_scan_files[i].hash;  /* binds like an index record */
            s->stem_hash  = name_hash(s->file);    /* exact: this IS the disk stem */
            s->dir_clus   = g_scan_dirs[d];
            s->duration_s = (ok && m.have) ? m.duration_s : 0;
            s->track      = (ok && m.have) ? (uint16_t)m.track : 0;
            s->disc       = 0;
            s->genre      = (ok && m.have) ? genre_intern(m.genre) : -1;
        }
    }
    if (g_songs_n >= LIB_MAX_SONGS) g_lib_truncated = 1;   /* out of song slots */
    library_finish();
    library_resolve_art(fs);               /* index each album's cover clusters */
    g_lib_scanned = 1;
}

/* Scratch for merge_sort_idx. Sized to the larger of the two things sorted. */
static uint16_t g_sort_tmp[LIB_MAX_SONGS];

/* Album sort keys, parsed ONCE. split_artist_album is a string scan, and the
 * old insertion sort re-ran it twice per comparison. */
static char g_album_key[LIB_MAX_ALBUMS][NAME_MAX + 1];

static int song_title_cmp_idx(uint16_t a, uint16_t b)
{
    return title_cmp(g_songs[a].title, g_songs[b].title);
}

static int album_key_cmp_idx(uint16_t a, uint16_t b)
{
    return title_cmp(g_album_key[a], g_album_key[b]);
}

/* Shared post-load: title-sort the index array + precompute per-genre counts. */
static void library_finish(void)
{
    for (int i = 0; i < g_songs_n; i++) g_song_sorted[i] = (uint16_t)i;
    merge_sort_idx(g_song_sorted, g_songs_n, g_sort_tmp, song_title_cmp_idx);
    for (int i = 0; i < g_genres_n; i++) g_genre_count[i] = 0;
    for (int i = 0; i < g_songs_n; i++) {
        int g = g_songs[i].genre;
        if (g >= 0 && g < g_genres_n) g_genre_count[g]++;
    }

    /* Alphabetise the album list A->Z by album title (insertion sort; the list
     * is small). g_albumview is derived from this, so the menu is sorted too.
     * The lookup indexes are (re)built AFTER this, since the sort moves albums. */
    {
        int n = g_albums_n;
        for (int i = 0; i < n; i++) {
            char artist[NAME_MAX + 1];
            split_artist_album(g_albums[i].folder, artist, g_album_key[i]);
            g_albumview[i] = (uint16_t)i;          /* reused as the order array */
        }
        merge_sort_idx(g_albumview, n, g_sort_tmp, album_key_cmp_idx);

        /*
         * Apply the permutation in place by following cycles, so we never need
         * a second full lib_album_t array (85 B x LIB_MAX_ALBUMS). g_albumview
         * is rebuilt by the caller right after this.
         *
         * The swap loop SCATTERS (dst[P[i]] = src[i]); what the sort gives us
         * is a GATHER order (dst[k] = src[order[k]]). So invert first — with
         * the raw order it silently produces the inverse permutation, i.e. a
         * scrambled album list that is still a valid permutation and therefore
         * looks plausible. Verified against a reference implementation over 200
         * randomised trials (order, stability, and this in-place apply).
         */
        for (int k = 0; k < n; k++) g_sort_tmp[g_albumview[k]] = (uint16_t)k;
        for (int i = 0; i < n; i++) {
            while (g_sort_tmp[i] != (uint16_t)i) {
                int j = g_sort_tmp[i];
                lib_album_t t = g_albums[i];
                g_albums[i] = g_albums[j];
                g_albums[j] = t;
                uint16_t tk = g_sort_tmp[i];
                g_sort_tmp[i] = g_sort_tmp[j];
                g_sort_tmp[j] = tk;
            }
        }
    }

    lookup_build();                        /* song-by-name + album-by-cluster */
}

/* Show a "building library" splash then scan (blocks; anti-skip covers audio). */
static void library_ensure(fat32_t *fs)
{
    if (g_lib_scanned) return;
    /* Time the whole load and surface it on About. This is the one long wait at
     * boot and it is dominated by disk seeks, so it is worth being able to see
     * whether a change actually helped instead of judging it by feel. */
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    load_bar("Loading Library", 0);       /* the load phases fill this in */
    g_lib_load_err     = 0;
    g_lib_orphaned     = 0;
    g_lib_unreadable   = 0;
    g_lib_retry_budget = LIB_LOAD_RETRY_BUDGET;
    if (!library_load_index(fs) &&        /* host-built CORELIB.IDX */
        !g_lib_load_err)                  /* ...absent, not unreadable: */
        library_scan(fs);                  /* fallback: per-file tag scan     */
    /* A root that could not be read leaves neither path having run to the
     * end. Mark the load done anyway: library_ensure is called from every
     * Music menu entry, and re-running the retries and sleeps on each
     * keypress against a failing disk would make the whole UI crawl. The
     * error is in g_lib_load_err; a deliberate retry clears g_lib_scanned. */
    g_lib_scanned = 1;
    build_artists();                       /* so About/Artists count is live */
    g_lib_load_ms = (mmio_read32(USEC_TIMER_ADDR) - t0) / 1000u;
}

/* Populate g_songview with the songs to show (genre < 0 = all), title-ordered. */
/*
 * Build the Songs view. `genre` < 0 means every genre; `artist` NULL means every
 * artist. Artist matching folds through artist_key()+title_cmp, the same
 * comparison build_artists() uses to merge "The Kid LAROI" with "Kid Laroi" —
 * so an artist's song list contains exactly the songs its Artists row counted.
 */
/* Whose songs the current view holds ("" = everyone's), for the header. */
static char g_songview_artist[NAME_MAX + 1];

static void songview_build(int genre, const char *artist)
{
    g_list_epoch++;
    copy_display_name(g_songview_artist, artist ? artist : "", 0 /*keep ext*/);
    g_songview_n = 0;
    for (int i = 0; i < g_songs_n; i++) {
        int si = g_song_sorted[i];
        if (genre >= 0 && g_songs[si].genre != genre) {
            continue;
        }
        if (artist && title_cmp(artist_key(g_songs[si].artist),
                                artist_key(artist)) != 0) {
            continue;
        }
        g_songview[g_songview_n++] = (uint16_t)si;
    }
    g_song_sel = g_song_accum = 0;
}

/* Launch a library song in its album's queue (so Next/Prev walk the album). */
/* Play a song picked on the Songs list: the queue is the ENTIRE current song
 * view (all songs, in the displayed order), started at the picked track — so
 * "N of M" is the song's position in the whole library and Prev/Next walk every
 * song, not just the one album. (Same full-queue build as Shuffle Songs, minus
 * the shuffle.) */
static void library_play_song(fat32_t *fs, int songview_idx)
{
    (void)fs;
    if (songview_idx < 0 || songview_idx >= g_songview_n) return;
    uint16_t sel_song = g_songview[songview_idx];

    load_bar_begin();
    /* Shuffle is the user's setting, not something picking a song turns off:
     * forcing it off here left the player un-shuffled for the rest of the
     * session while the UI kept showing the SHUF token. */
    player_set_shuffle(g_settings.shuffle);
    player_queue_begin();
    int start = 0, added = 0;
    for (int i = 0; i < g_songview_n; i++) {
        if ((i & 255) == 0) load_bar_progress("Loading Songs", i * 100 / g_songview_n);
        lib_song_t *s = &g_songs[g_songview[i]];
        /* Record the start position BEFORE the resolved-check: a pick that the
         * index lists but the disk no longer has would otherwise leave start at
         * 0 and play the top of the list. Pointing at `added` lands on the next
         * resolved song instead. */
        if (g_songview[i] == sel_song) start = added;
        if (!s->file_clus) continue;              /* unresolved on disk — skip */
        browse_entry_t e;
        int k = 0;
        for (; s->file[k] && k < NAME_MAX; k++) e.name[k] = s->file[k];
        e.name[k]  = '\0';
        e.clus     = s->file_clus;
        e.size     = s->file_size;
        e.fmt      = 0;
        e.is_dir   = 0;
        int ai = album_by_clus(s->dir_clus);
        e.art_clus = (ai >= 0) ? g_albums[ai].art_clus : 0;
        e.art_size = (ai >= 0) ? g_albums[ai].art_size : 0;
        player_queue_add(&e);
        added++;
    }
    if (start >= added) start = (added > 0) ? added - 1 : 0;   /* nothing after it */
    player_queue_commit(start);
    hal_volume_set(g_volume);
}

/* Tiny LCG for the shuffle pick (no libc; seeded from the free-running timer so
 * each invocation differs). */
static uint32_t g_rng;
static uint32_t rng_next(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 1;                       /* drop the low bit (poor LCG entropy) */
}

/* "Shuffle Songs": fill the play queue with tracks drawn from randomly-ordered
 * albums (bounded by the queue capacity) and start in shuffle mode, so the
 * order is randomised too. A fresh random draw each time it's chosen. Mixed
 * albums => no single cover, so the now-playing art is left empty. */
static void shuffle_songs_play(fat32_t *fs)
{
    library_ensure(fs);
    if (g_songs_n == 0) return;

    load_bar_begin();

    /* Shuffle ALL song indices (Fisher-Yates) — the WHOLE library, not a sample. */
    static uint16_t ord[LIB_MAX_SONGS];
    int ns = g_songs_n;
    for (int i = 0; i < ns; i++) ord[i] = (uint16_t)i;
    g_rng = mmio_read32(USEC_TIMER_ADDR) | 1u;
    for (int i = ns - 1; i > 0; i--) {
        int j = (int)(rng_next() % (uint32_t)(i + 1));
        uint16_t t = ord[i]; ord[i] = ord[j]; ord[j] = t;
    }

    /* Build the full queue from the resolved song index (file cluster + per-track
     * album cover) — every song, in the shuffled order. The Queue view then shows
     * that order, and Now Playing shows each song's own art. */
    player_set_shuffle(0);                 /* already shuffled; play in order */
    player_queue_begin();
    for (int i = 0; i < ns; i++) {
        if ((i & 255) == 0) load_bar_progress("Shuffling Songs", i * 100 / ns);
        lib_song_t *s = &g_songs[ord[i]];
        if (!s->file_clus) continue;       /* unresolved (missing on disk) — skip */
        browse_entry_t e;
        int k = 0;
        for (; s->file[k] && k < NAME_MAX; k++) e.name[k] = s->file[k];
        e.name[k]  = '\0';
        e.clus     = s->file_clus;
        e.size     = s->file_size;
        e.fmt      = 0;                     /* library is FLAC */
        e.is_dir   = 0;
        int ai = album_by_clus(s->dir_clus);
        e.art_clus = (ai >= 0) ? g_albums[ai].art_clus : 0;
        e.art_size = (ai >= 0) ? g_albums[ai].art_size : 0;
        player_queue_add(&e);
    }
    player_queue_commit(0);
    /* The queue is ALREADY shuffled, so it was committed in plain order — but
     * restore the user's setting now, or Shuffle stays off for everything played
     * afterwards while the UI still shows SHUF. */
    player_set_shuffle(g_settings.shuffle);
    hal_volume_set(g_volume);
}

/*
 * The album a song belongs to, resolved through its folder cluster and split
 * out of the "Artist - Album" folder name. Writes "" when the folder isn't in
 * the album table (a track whose folder never became an album entry).
 *
 * Through album_by_clus: the cluster->album hash already exists (the loader
 * builds it for the queue builders), so there is no second table to keep in
 * sync. This used to be its own linear scan over every album, run per visible
 * row per repaint — up to 1024 compares a row, six rows a frame, on the one
 * list the wheel moves fastest.
 */
static void song_album_title(const lib_song_t *sg, char *out)
{
    int ai = album_by_clus(sg->dir_clus);
    if (ai >= 0) {
        char artist[NAME_MAX + 1];
        split_artist_album(g_albums[ai].folder, artist, out);
        return;
    }
    out[0] = '\0';
}

static void songs_row_draw(int r, int idx)
{
    lib_song_t *sg = &g_songs[g_songview[idx]];
    char dur[FMT_TIME_MAX];
    if (sg->duration_s) fmt_time(dur, sg->duration_s); else dur[0] = '\0';

    /*
     * Sub-line: normally the artist, because the unfiltered Songs list mixes
     * every artist together and that is what tells rows apart.
     *
     * In an artist's "All Songs" the header ALREADY names the artist, so
     * repeating it on every row spends the only sub-line on the one fact the
     * user cannot need. Show the ALBUM there instead — in that view it is
     * exactly what distinguishes one row from the next.
     */
    char alb[NAME_MAX + 1];
    const char *sub = sg->artist[0] ? sg->artist : 0;
    if (g_songview_artist[0]) {
        song_album_title(sg, alb);
        sub = alb[0] ? alb : 0;
    }
    list_row_titled(r, sg->title, sub, dur[0] ? dur : 0, idx == g_song_sel, 0);
}

static void songs_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_songview_n > 0) fmt_count(right, sel + 1, g_songview_n);
    else                  right[0] = '\0';
    ui_header(g_songview_artist[0] ? g_songview_artist : "Songs",
                  right, 1);
    if (g_songview_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No songs", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, g_songview_n, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= g_songview_n) break;
        songs_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS2, g_songview_n);
}

static void genres_row_draw(int r, int idx)
{
    char cnt[8];
    u32_to_dec(cnt, (unsigned)g_genre_count[idx]);
    list_row(r, g_genres[idx], 0, cnt, 0, idx == g_genre_sel, 0, 0);
}

static void genres_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_genres_n > 0) fmt_count(right, sel + 1, g_genres_n);
    else                right[0] = '\0';
    ui_header("Genres", right, 1);
    if (g_genres_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No genres", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, g_genres_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_genres_n) break;
        genres_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, g_genres_n);
}

/* ---------------------------------------------------------------------------
 * Queue view: the tracks queued in the current playback (the folder a track was
 * launched from). The playing track carries the animated now-playing bars;
 * SELECT jumps to a track. Reached via SELECT on the Now Playing screen.
 * ------------------------------------------------------------------------- */
static int g_queue_sel, g_queue_accum;

static void queue_row_draw(int r, int idx)
{
    int is_sel = (idx == g_queue_sel);
    list_row(r, track_display(player_queue_name(idx)), 0, 0, 0, is_sel,
             player_queue_is_dir(idx), 0);
    if (idx == player_queue_current()) {      /* the currently-playing track */
        int ry = LIST_Y0 + r * ROW_H;
        nowplaying_bars(LCD_WIDTH - 22, ry + 7,
                        is_sel ? LINEN_SEL_FG : LINEN_INK,
                        mmio_read32(USEC_TIMER_ADDR));
    }
}

static void queue_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    int n   = player_queue_len();
    int cur = player_queue_current();
    char right[12];
    if (n > 0) fmt_count(right, cur + 1, n);
    else       right[0] = '\0';
    ui_header("Now Playing", right, 1);

    if (n == 0) {
        ui_text(14, LIST_Y0 + 20, "Queue empty", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = ui_scroll_window(sel, n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= n) break;
        queue_row_draw(r, idx);
    }
    ui_scrollbar(LIST_Y0, top, LIST_ROWS, n);
}

/* ---------------------------------------------------------------------------
 * Settings (core/ui/settings.h data-driven model). g_settings holds the state;
 * g_set_screen tracks the current sub-screen within the one SCR_SETTINGS stack
 * entry. Slider rows use a brief edit mode (SELECT toggles it, then the wheel
 * adjusts) so the wheel can still move the selection otherwise.
 * ------------------------------------------------------------------------- */
static int g_set_screen;                  /* current settings_screen_t          */
static int g_set_sel;                     /* selection within g_set_screen      */
static int g_set_root_sel;                /* saved ROOT selection               */
static int g_set_accum;                   /* wheel accumulator                  */
static int g_set_editing;                 /* editing a slider row               */

/* Push the FUNCTIONAL settings out to the subsystems. Cosmetic fields are a
 * no-op. The backlight timeout/brightness are read live by the loop. */
static void settings_apply(void)
{
    player_set_shuffle(g_settings.shuffle);
    player_set_repeat((int)g_settings.repeat);
    g_volume = g_settings.volume;
    hal_volume_set(g_volume);
    hal_balance_set(g_settings.balance);
    hal_tone_set(g_settings.bass, g_settings.treble);
    theme_set(g_settings.theme);           /* Linen / Onyx -> live palette swap */
}

/* ---------------------------------------------------------------------------
 * Settings persistence (kernel/config.c) — DEBOUNCED.
 *
 * A disk write per wheel tick would mean dozens of writes for one volume sweep:
 * pointless drive wear, a spin-up the user can hear, and dozens of chances to
 * be interrupted mid-write. So every mutation of g_settings just marks the
 * state dirty and stamps the time; the main loop commits ONE write once the
 * user has stopped fiddling, and leaving the Settings screen forces it
 * immediately (the natural "I'm done" moment).
 *
 * settings_touch() is called from every site that changes g_settings. If you
 * add another, call it there too — a missed call means the change is simply
 * not persisted, which is the safe direction to fail.
 * ------------------------------------------------------------------------- */
#define CFG_SAVE_DEBOUNCE_US  3000000u    /* 3 s after the last change */

static int      g_cfg_dirty;              /* a change is pending a write   */
static int      g_cfg_save_deferred;      /* gate refusal already logged   */
static uint32_t g_cfg_dirty_us;           /* when the last change happened */

static void settings_touch(void)
{
    g_cfg_dirty    = 1;
    g_cfg_dirty_us = mmio_read32(USEC_TIMER_ADDR);
}

/*
 * Commit a pending save. `force` skips the debounce (used when leaving the
 * Settings screen and before suspend). A failure is logged and the dirty flag
 * is cleared anyway: config_save() leaves the previous good slot intact, so the
 * only cost is that this session's change does not persist — and retrying every
 * pass would just hammer a drive that is already unhappy.
 */
static void settings_commit(int force)
{
    if (!g_cfg_dirty) {
        return;
    }
    if (!force) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - g_cfg_dirty_us)
                < CFG_SAVE_DEBOUNCE_US) {
            return;
        }
        /* Don't spin the platters up just to save 1 KB. If the drive is parked
         * while audio plays out of the anti-skip buffer, the write would cost
         * a multi-second, audible spin-up for something with no deadline —
         * so keep the change pending and let it ride out on the next refill,
         * on playback stopping, or on the forced commit at suspend/power-off.
         * The record stays in RAM either way; the only thing at risk is a
         * battery pull, and the forced paths cover every graceful exit. */
        if (ata_is_parked() && player_active()) {
            return;
        }
    }
    /*
     * Refuse the write when the cell is too low to guarantee finishing it.
     *
     * config_save() is the ONLY thing in the firmware that writes to the
     * user's disk, and settings_commit is its only caller, so this one check
     * is the whole write gate. Below the disk-safe threshold the policy in
     * battery.c has already flushed pending changes and parked the drive; a
     * write starting after that would spin the platters back up on a cell that
     * may not have the energy to see it through, and a cut mid-sector is how a
     * config record gets torn.
     *
     * Deliberately BEFORE clearing g_cfg_dirty: the change stays pending, so
     * it lands on the next commit if the charger goes in and the policy
     * recovers. Refusing to write is not the same as discarding the edit.
     *
     * This also covers the forced commit inside enter_standby(): at the
     * shutoff threshold that path would otherwise attempt a write at 3300 mV,
     * which is exactly the moment there is least energy to complete one.
     */
    if (force != CFG_COMMIT_LAST && !battery_disk_writes_allowed()) {
        /* Logged ONCE per refusal, not once per pass. This runs from the main
         * loop — 100 Hz idle, thousands of passes a second while playing —
         * and a UART line is milliseconds of blocking TX at 115200 baud;
         * printed on every pass it would have dragged the whole UI for as
         * long as the cell stayed below the line with a change pending. */
        if (!g_cfg_save_deferred) {
            uart_puts("core: cfg save deferred — battery below disk-safe\n");
            g_cfg_save_deferred = 1;
        }
        return;
    }
    g_cfg_save_deferred = 0;
    g_cfg_dirty = 0;
    if (!config_writable()) {
        return;                            /* no CORECFG.DAT — nothing to do */
    }
    int rc = config_save(&g_settings);
    uart_puts("core: cfg save rc ");
    uart_put_hex32((uint32_t)rc);
    uart_puts(" seq ");
    uart_put_hex32(config_seq());
    uart_putc('\n');
}

static void settings_render_cur(void)
{
    if (g_set_screen == SETTINGS_ABOUT) {
        settings_about_render(g_bat_pct, g_bat_mv, g_bat_raw, g_total_mb, g_free_mb,
                              g_songs_n, g_albums_n, g_artists_n);
        /* The counts above are capped (LIB_MAX_SONGS/ALBUMS, ARTISTS_MAX,
         * LIB_MAX_GENRES). When a load actually hit one of those caps, say so —
         * otherwise a library that's too big just looks like it lost tracks.
         * Drawn here, over the shared About panel, in the gap between the device
         * hero (baseline 62) and the stat columns (baseline 100). */
        /* These are INDEPENDENT: the truncation warning used to be an else-if
         * in front of the load time, so a library that hit a cap hid the number
         * entirely — which is exactly the library you most want the number for. */
        if (g_lib_truncated) {
            ui_text_centered(78, "Library too large " UI_GLYPH_MIDDOT
                                 " some items not shown",
                             FONT_SMALL, BATT_LOW_RED);
        }
    } else if (g_set_screen == SETTINGS_DIAG) {
        /* Boot Details owns the diagnostics now: the cold-boot phase
         * breakdown and the settings-file locator. They used to be squeezed
         * into About's spare gaps, which is how the CFG rows ended up on top
         * of the stat columns. main.c resolves the LBAs here because
         * config_probe_lba() must resolve FRESH, exactly as a save would,
         * rather than reporting something cached. Read-only. */
        uint32_t lba0 = 0, lba1 = 0;
        (void)config_probe_lba(0, &lba0);
        (void)config_probe_lba(1, &lba1);
        const player_stats_t *ps = player_stats();
        settings_diag_render(g_boot_total_ms, g_boot_lcd_ms, g_boot_disk_ms,
                             g_lib_load_ms, g_boot_resume_ms,
                             g_boot_res_dir_ms, g_boot_res_open_ms,
                             g_boot_res_seek_ms,
                             ps ? ps->decode_us_per_kframe : 0,
                             ps ? ps->underruns : 0,
                             config_writable(), config_seq(), lba0, lba1);
    } else {
        settings_render(g_set_screen, &g_settings, g_set_sel);
    }
}

/* Boot splash: Nunito CORE branding on the Linen surface the moment the panel
 * is ours, so the disk-spin-up / mount delay reads as "loading" rather than the
 * leftover chainloader framebuffer. */
static void boot_splash(void)
{
    console_clear(LINEN_SURFACE);
    ui_text_centered(120, "Core Player", FONT_TITLE, LINEN_INK);
    ui_text_centered(142, "loading",     FONT_SUB,   LINEN_MUTED);
    lcd_present_fb(console_framebuffer());
}


/* Right-side circle arc (a ")" shape): radius R, +/- span rows tall, centred at
 * (cx, cy); ~2px thick. The speaker's sound waves + the lock shackle use it. */
/* A 1px-thin right-opening crescent (radius R, +/-span rows), anchored so its
 * near point sits by the speaker cone — the skinny sound wave. */
static void draw_arc_thin(int cx, int cy, int R, int span, uint16_t c)
{
    for (int dy = -span; dy <= span; dy++) {
        int dx = ui_isqrt(R * R - dy * dy);
        console_fill_rect(cx + dx, cy + dy, 1, 1, c);
    }
}

/* Top half of an annulus (ring): outer radius Ro, inner Ri, centred (cx, cy) —
 * a padlock shackle arch. Rows above the inner hole are a solid cap. */
static void draw_ring_top(int cx, int cy, int Ro, int Ri, uint16_t c)
{
    for (int dy = -Ro; dy <= 0; dy++) {
        int xo = ui_isqrt(Ro * Ro - dy * dy);
        if (-dy <= Ri) {
            int xi = ui_isqrt(Ri * Ri - dy * dy);
            console_fill_rect(cx - xo, cy + dy, xo - xi + 1, 1, c);   /* left band  */
            console_fill_rect(cx + xi, cy + dy, xo - xi + 1, 1, c);   /* right band */
        } else {
            console_fill_rect(cx - xo, cy + dy, 2 * xo + 1, 1, c);    /* solid cap  */
        }
    }
}

/* A small right-pointing speaker: driver cabinet + cone, then sound waves that
 * scale with `vol` — two waves loud, one wave quiet, and a mute "X" at 0. */
static void draw_speaker(int sx, int sy, uint16_t c, int vol)
{
    ui_round_rect(sx - 8, sy - 3, 4, 6, 1, c);          /* cabinet             */
    for (int dx = 0; dx <= 4; dx++) {                     /* cone, opening right */
        int half = 2 + dx;
        console_fill_rect(sx - 4 + dx, sy - half, 1, 2 * half, c);
    }
    if (vol <= 0) {                                       /* muted: an X         */
        for (int i = 0; i < 6; i++) {
            console_fill_rect(sx + 3 + i, sy - 3 + i, 2, 1, c);   /* '\'         */
            console_fill_rect(sx + 3 + i, sy + 2 - i, 2, 1, c);   /* '/'         */
        }
        return;
    }
    /* Three skinny (1px) crescent waves anchored at the cone front, growing with
     * volume — closer + thinner than the old 2px arcs (chosen design "B"). */
    if (vol > 5)  draw_arc_thin(sx, sy, 3, 2, c);         /* wave 1: hairline    */
    if (vol > 40) draw_arc_thin(sx, sy, 6, 4, c);         /* wave 2              */
    if (vol > 72) draw_arc_thin(sx, sy, 9, 5, c);         /* wave 3              */
}

/* Centered volume overlay plate (volume-demo.jsx VolumeOverlay): speaker glyph +
 * ink fill bar + big percent, on a light near-surface plate. */
static void volume_overlay_render(int vol)
{
    const int PX = 60, PY = 101, PW = 200, PH = 32;
    fill_round_rect_aa(PX, PY, PW, PH, 8, LINEN_PLATE);    /* raised plate, AA r8  */

    draw_speaker(PX + 16, PY + PH / 2, LINEN_INK, vol);

    /* Fill bar. */
    int bx = PX + 34, by = PY + PH / 2 - 3, bw = PW - 34 - 42, bh = 6;
    console_fill_rect(bx, by, bw, bh, LINEN_TRK);          /* track rgba(ink,0.12) */
    int fw = bw * vol / 100;
    if (fw < 0) fw = 0;
    if (fw > bw) fw = bw;
    console_fill_rect(bx, by, fw, bh, LINEN_INK);

    /* Percent, right-aligned. */
    char p[5];
    u32_to_dec(p, (unsigned)vol);
    int w = text_width(p, text_font_bold_12());       /* percent is 11/700       */
    ui_text(PX + PW - 14 - w, PY + PH / 2 + 4, p, text_font_bold_12(), LINEN_INK);
}

/* Padlock for the lock/unlock plate: a rounded body with a keyhole (punched in
 * the plate colour `bg`) + a curved shackle. Closed = the shackle arch sits
 * latched on the body; open = it's swung up and left so the right end floats off
 * the body (the gap reads as unlatched). `cy` is the body's top edge. */
/* Padlock shackle: a thick semicircular arch (outer radius Ro, inner Ri) centred
 * at (sx, ay) on its diameter line, plus two straight prongs dropping from the
 * arch ends. lL/rL are the left/right prong lengths (independent so the open
 * state can raise one prong). Prong thickness matches the arch band so the join
 * is seamless. Drawn BEFORE the body so the body covers the seated prong feet. */
static void draw_shackle(int sx, int ay, int Ro, int Ri,
                         int lL, int rL, uint16_t c)
{
    int w = Ro - Ri + 1;                              /* band/prong thickness */
    draw_ring_top(sx, ay, Ro, Ri, c);                /* semicircular top     */
    console_fill_rect(sx - Ro, ay, w, lL, c);        /* left prong           */
    console_fill_rect(sx + Ri, ay, w, rL, c);        /* right prong          */
}

/* Keyhole punched in the plate colour `bg`, centred at (kx, ky): a round hole
 * with a tapered slot widening downward, symmetric about kx. */
/* Minimal keyhole: a small round hole + short slot, punched in the plate bg. */
static void draw_keyhole(int kx, int ky, uint16_t bg)
{
    ui_round_rect(kx - 3, ky - 3, 6, 6, 3, bg);    /* round hole (~d6)      */
    console_fill_rect(kx - 1, ky + 2, 2, 4, bg);     /* short slot            */
}

/* Padlock for the lock/unlock plate (design: "Minimal dot"). `cy` is the
 * VERTICAL CENTRE (~32 wide x ~40 tall). `c` = icon colour, `bg` = plate colour.
 * LOCKED = shackle latched, both prongs seated; UNLOCKED = shackle popped
 * straight UP, floating clear of the body (visible gap beneath). */
static void draw_lock_icon(int cx, int cy, int open, uint16_t c, uint16_t bg)
{
    if (open)
        draw_shackle(cx, cy - 14, 10, 6, 6, 6, c);   /* lifted straight up   */
    else
        draw_shackle(cx, cy - 9,  10, 6, 8, 8, c);   /* latched              */

    ui_round_rect(cx - 16, cy - 3, 32, 22, 4, c);  /* body, over prong feet */
    draw_keyhole(cx, cy + 7, bg);
}

/* Centered lock/unlock plate (system-screens.jsx LockedScreen/UnlockedScreen):
 * LOCKED = dark plate + light closed lock; UNLOCKED = light plate + dark open
 * lock. Drawn over whatever screen is currently in the framebuffer. */
static void lock_plate_render(int locked)
{
    const int PX = 70, PY = 65, PW = 180, PH = 110;
    uint16_t plate = locked ? LINEN_INK : LINEN_SURFACE;
    uint16_t fg    = locked ? LINEN_SURFACE : LINEN_INK;
    if (!locked) {                                    /* light plate: draw a ring */
        fill_round_rect_aa(PX - 1, PY - 1, PW + 2, PH + 2, 11, LINEN_BORDER);
    }
    fill_round_rect_aa(PX, PY, PW, PH, 10, plate);
    draw_lock_icon(PX + PW / 2, PY + 45, !locked, fg, plate);  /* cy = vertical centre */
    const char *label = locked ? "LOCKED" : "UNLOCKED";
    int w = text_width(label, FONT_HEADER);
    ui_text(PX + (PW - w) / 2, PY + 84, label, FONT_HEADER, fg);
}

/* Now-playing album art at 120x120 — the stored folder.art's native size, so it
 * blits straight through at full quality (no downscale) and fills the art-left
 * layout. Only art larger than this is box-downscaled (cached per track). */
#define NP_ART_DIM 120
static uint16_t g_np_art[NP_ART_DIM * NP_ART_DIM];
static int      g_np_art_key = -1;

static const uint16_t *np_art_120(void)
{
    if (!player_art_ok()) { g_np_art_key = -1; return 0; }
    int w = player_art_w(), h = player_art_h();
    if (w == NP_ART_DIM && h == NP_ART_DIM) {
        return player_art_pixels();        /* native size: blit as-is */
    }
    /* Keyed on the ART generation, not the queue index: a freshly built queue
     * that happens to start on the same index would otherwise re-use the
     * PREVIOUS album's downscaled cover. player_art_seq() bumps whenever the
     * art changes, including when it is cleared. */
    int key = (int)player_art_seq();
    if (key != g_np_art_key) {
        thumb_box_rgb565(player_art_pixels(), w, h,
                         g_np_art, NP_ART_DIM, NP_ART_DIM);
        g_np_art_key = key;
    }
    return g_np_art;
}

/* Now-playing TRANSPORT strip: the elapsed / −remaining times and the progress
 * bar, i.e. everything that changes once a second. Split out of the full
 * renderer because the rest of the screen (art + metadata) only changes on a
 * track change: the clock tick redraws THIS band and presents only it, instead
 * of clearing the framebuffer, re-blitting the 120x120 cover and rebuilding
 * every metadata string every second and discarding all of it above y=128. */
#define NP_TR_Y 184                        /* transport band top                 */
#define NP_TR_H (LCD_HEIGHT - NP_TR_Y)     /* ...to the bottom of the panel      */

static void nowplaying_transport_render(uint32_t elapsed_s, uint32_t total_s)
{
    console_fill_rect(0, NP_TR_Y, LCD_WIDTH, NP_TR_H, LINEN_SURFACE);

    /* While scrubbing, the bar and the left-hand time show the TARGET, not the
     * live position — the wheel is aiming at a destination and the readout has
     * to be what you are aiming at. The right-hand side becomes the delta from
     * where playback actually is, so a long seek is legible ("+3:41") instead
     * of a remaining figure you have to subtract in your head. */
    int      scrub = np_scrubbing();
    uint32_t shown = scrub ? g_scrub_target_s : elapsed_s;

    char te[FMT_TIME_MAX], tr[FMT_TIME_MAX + 2];   /* tr carries a sign prefix */
    fmt_time(te, shown);
    if (scrub) {
        uint32_t d = (shown > elapsed_s) ? shown - elapsed_s : elapsed_s - shown;
        tr[0] = (shown >= elapsed_s) ? '+' : '-';
        fmt_time(tr + 1, d);
    } else {
        uint32_t rem = (total_s > elapsed_s) ? total_s - elapsed_s : 0;
        tr[0] = '-';
        fmt_time(tr + 1, rem);                         /* "−M:SS" remaining     */
    }
    ui_text(18, 198, te, FONT_SUB, scrub ? LINEN_INK : LINEN_MUTED_D);
    int wtr = text_width(tr, FONT_SUB);
    ui_text(LCD_WIDTH - 18 - wtr, 198, tr, FONT_SUB,
            scrub ? LINEN_INK : LINEN_MUTED_D);

    /* Taller rounded-cap bar (INK fill on a faint ink track, Theme1Live fg).
     * bh 8 with AA pill caps reads smoother than the old 6px integer-stepped
     * caps. */
    int pbx = 18, by = 209, bw = LCD_WIDTH - 36, bh = 8;
    fill_round_rect_aa(pbx, by, bw, bh, bh / 2, LINEN_TRK);
    int fw = (total_s > 0) ? (int)((shown * (uint32_t)bw) / total_s) : 0;
    if (fw > bw) fw = bw;
    if (fw >= bh) {
        fill_round_rect_aa(pbx, by, fw, bh, bh / 2, LINEN_INK);
    } else if (fw > 0) {
        console_fill_rect(pbx, by, fw, bh, LINEN_INK);
    }
    /* A playhead at the target so the aim point is readable even where the
     * filled length is ambiguous (very short or very long tracks). */
    if (scrub) {
        int hx = pbx + fw - 1;
        if (hx < pbx)          hx = pbx;
        if (hx > pbx + bw - 3) hx = pbx + bw - 3;
        console_fill_rect(hx, by - 3, 3, bh + 6, LINEN_INK);
    }
}

/* Now-playing (Linen, interactive-ipod.jsx Theme1Live): a top status row
 * ("Now Playing"/"Paused" + shuffle/repeat + battery), then 88x88 art on the
 * LEFT with a metadata column to its right (SONG N OF M / title / artist /
 * album), and the elapsed / −remaining times + progress bar pinned to the
 * bottom. Art + metadata come from the PLAYING folder, not the browsed one. */
static void nowplaying_render(const char *name, uint32_t elapsed_s,
                              uint32_t total_s, uint32_t buf_pct)
{
    (void)buf_pct;
    console_clear(LINEN_SURFACE);

    /* --- top status row: context label left, state + battery right --------- */
    ui_text(12, 15, player_paused() ? "Paused" : "Now Playing",
            text_font_bold_12(), LINEN_INK);

    int bx = LCD_WIDTH - 12 - 19;                     /* battery block         */
    draw_battery(bx, 3, g_bat_pct);
    int rc = bx - (g_locked ? 18 : 6);                /* right edge for tokens */
    if (g_locked) {
        draw_lock_glyph(bx - 14, 3, LINEN_INK);
    }
    /* Compact shuffle / repeat tokens, right-aligned before the battery. */
    {
        char st[16]; int p = 0;
        if (g_settings.shuffle) { const char *s = "SHUF"; while (*s) st[p++] = *s++; }
        if (g_settings.repeat != REPEAT_OFF) {
            if (p) st[p++] = ' ';
            const char *s = (g_settings.repeat == REPEAT_ONE) ? "RPT1" : "RPT";
            while (*s) st[p++] = *s++;
        }
        st[p] = '\0';
        if (p) {
            int w = text_width(st, FONT_SMALL);
            ui_text(rc - w, 13, st, FONT_SMALL, LINEN_MUTED2);
        }
    }

    /* --- art on the left (native 120x120) ---------------------------------- */
    const int ax = 16, ay = 44;
    const uint16_t *art = np_art_120();
    if (art) {
        console_blit565(ax, ay, NP_ART_DIM, NP_ART_DIM, art);
    } else {
        /* No cover: a neutral tile, same as the list rows' chip placeholder —
         * bare surface here read as a rendering bug, not as "no artwork". */
        console_fill_rect(ax, ay, NP_ART_DIM, NP_ART_DIM, LINEN_BORDER);
    }

    /* --- metadata column, vertically centred beside the taller art --------- */
    const flac_meta_t *m = player_meta();
    const char *title = (m->have && m->title[0]) ? m->title : track_display(name);
    int mx = ax + NP_ART_DIM + 14;                    /* metadata left edge    */
    int mr = LCD_WIDTH - 14;                           /* metadata right edge   */

    /* "TRACK N OF M" eyebrow above the title (design's "Track 04 of 11"). */
    int tot = player_queue_len();
    if (tot > 0) {
        /* 8 + 12 literal chars + two decimals (<=10 digits each) + NUL. The old
         * 24-byte buffer only covered ONE-digit numbers on both sides: any album
         * with >=10 tracks overran it, and a 1200-entry shuffle queue by 5. */
        char num[8 + 12 + 10 + 10 + 1];
        int p = 0;
        for (const char *q = "TRACK   "; *q; q++) num[p++] = *q;   /* air after TRACK */
        p += u32_to_dec(num + p, (unsigned)(player_queue_current() + 1));
        for (const char *q = "     OF     "; *q; q++) num[p++] = *q;  /* extra air both sides */
        p += u32_to_dec(num + p, (unsigned)tot);
        num[p] = '\0';
        ui_text(mx, 72, num, FONT_SMALL, LINEN_MUTED2);
    }

    /* Title (bold 17): drawn at its live scroll offset + registered as the
     * marquee, so volume/lock full repaints never flash the truncated title. */
    mq_text(mx, 94, mr - mx, title, FONT_TITLE, LINEN_INK, LINEN_SURFACE,
            0, LCD_HEIGHT);   /* now-playing title has room — no row clip */

    if (m->have && m->artist[0]) {
        ui_text_clip(mx, 114, m->artist, FONT_SUB, LINEN_MUTED_D, mx, mr);
    }
    if (m->have && m->album[0]) {
        ui_text_clip(mx, 130, m->album, FONT_SUB, LINEN_MUTED2, mx, mr);
    }

    nowplaying_transport_render(elapsed_s, total_s);

    /* Volume overlay rides on top for ~1.5 s after a wheel adjustment. */
    if (ui_window_up(&g_vol_show, VOL_SHOW_US, mmio_read32(USEC_TIMER_ADDR))) {
        volume_overlay_render(g_volume);
    }
}

/* ---------------------------------------------------------------------------
 * Menus (main + Music sub-menu)
 *
 * Both are the same widget over a small {label, active} item list. Inactive
 * items render greyed and SELECT does nothing (features not yet built). The
 * renderer reuses the list row geometry (LIST_Y0/ROW_H); neither menu exceeds
 * LIST_ROWS, so no scrolling window is needed.
 * ------------------------------------------------------------------------- */
typedef struct { const char *label; uint8_t active; } menu_item_t;

/* Main menu. ACTIVE: Music, Now Playing (Now Playing greyed until something is
 * playing — its `active` flag is refreshed from the player before each paint). */
enum { MM_MUSIC, MM_PLAYLISTS, MM_PODCASTS, MM_AUDIOBOOKS, MM_SETTINGS,
       MM_NOWPLAYING, MM_COUNT };
static menu_item_t g_main_menu[MM_COUNT] = {
    { "Music",       1 },
    { "Playlists",   0 },
    { "Podcasts",    0 },
    { "Audiobooks",  0 },
    { "Settings",    1 },
    { "Now Playing", 1 },
};
static int g_main_sel;

/* Music sub-menu. ACTIVE: Albums (enters the junk-filtered folder browser). */
enum { MU_PLAYLISTS, MU_ARTISTS, MU_ALBUMS, MU_SONGS, MU_SHUFFLE, MU_GENRES,
       MU_COMPOSERS, MU_AUDIOBOOKS, MU_COUNT };
static const menu_item_t g_music_menu[MU_COUNT] = {
    { "Playlists",     0 },
    { "Artists",       1 },
    { "Albums",        1 },
    { "Songs",         1 },
    { "Shuffle Songs", 1 },
    { "Genres",        1 },
    { "Composers",     0 },
    { "Audiobooks",    0 },
};
static int g_music_sel;

/* Shared wheel accumulator for the menu screens. */
static int g_menu_accum;

/* Generic menu renderer over a {label, active} list. `back` draws the header
 * back chevron (off for the root menu). */
/* The item list + selection the menu rows are drawn from — set by
 * menu_render_list (and by list_view_current, for the partial repaint path)
 * so menu_row_draw has the same row-drawing signature as every other list. */
static const menu_item_t *g_menu_items;
static int                g_menu_sel;

static void menu_row_draw(int r, int idx)
{
    list_row(r, g_menu_items[idx].label, 0, 0, 1 /*chevron*/, idx == g_menu_sel,
             !g_menu_items[idx].active /*greyed*/, 0);
}

static void menu_render_list(const char *title, const menu_item_t *items,
                             int n, int sel, int back)
{
    g_menu_items = items;
    g_menu_sel   = sel;
    console_clear(LINEN_SURFACE);
    status_strip_render();
    ui_header(title, "", back);
    for (int i = 0; i < n && i < LIST_ROWS; i++) {
        menu_row_draw(i, i);
    }
}

/* Visible main-menu row count: "Now Playing" (the last item) only appears while
 * a track is loaded, so it's simply dropped from the count when idle. */
static int main_menu_count(void)
{
    return player_active() ? MM_COUNT : MM_COUNT - 1;
}

static void main_menu_render(void)
{
    int n = main_menu_count();
    if (g_main_sel >= n) g_main_sel = n - 1;   /* cursor was on a now-gone row */
    menu_render_list("Core", g_main_menu, n, g_main_sel, 0);
}

static void music_menu_render(void)
{
    menu_render_list("Music", g_music_menu, MU_COUNT, g_music_sel, 1);
}

/* ---------------------------------------------------------------------------
 * Screen stack
 * ------------------------------------------------------------------------- */
typedef enum { SCR_MENU, SCR_MUSIC, SCR_ARTISTS, SCR_SONGS, SCR_GENRES,
               SCR_BROWSER, SCR_NOWPLAYING, SCR_QUEUE, SCR_SETTINGS,
               SCR_BATTERY, SCR_CHARGING } screen_t;
/* The deepest legal path is 8: MENU, MUSIC, ARTISTS, BROWSER, SONGS,
 * NOWPLAYING, QUEUE, plus ONE modal (scr_push_modal replaces a modal with a
 * modal, so BATTERY and CHARGING never stack). The headroom is deliberate:
 * this used to be exactly 8 without SONGS counted, and a DISKSAFE edge at
 * the bottom of that path retried the push every pass with dirty set — a
 * full-repaint loop until a button cleared it. A new screen still deserves
 * a look at the arithmetic; the UART line is what says it was wrong. */
#define SCR_STACK_MAX 12
static screen_t g_scr[SCR_STACK_MAX];
static int      g_scr_n;

static void      scr_push(screen_t s) { g_list_epoch++;
                                        if (g_scr_n < SCR_STACK_MAX) g_scr[g_scr_n++] = s;
                                        else uart_puts("core: scr_push overflow, dropped\n"); }
static void      scr_pop(void)        { g_list_epoch++;
                                        if (g_scr_n > 1) g_scr_n--; }
static screen_t  scr_cur(void)        { return g_scr[g_scr_n - 1]; }
static int       scr_is_modal(screen_t s) { return s == SCR_BATTERY || s == SCR_CHARGING; }
/* A modal (BATTERY, CHARGING) over a modal takes its slot instead of a new
 * one: the two are alternatives for the same panel, and popping the top one
 * lands on the real screen underneath, which is what both auto-dismiss paths
 * expect. */
static void      scr_push_modal(screen_t s) { if (scr_is_modal(scr_cur())) {
                                                  g_list_epoch++;
                                                  g_scr[g_scr_n - 1] = s;
                                              } else scr_push(s); }

/* Why the last browse_load produced what it did: 0, or the FAT32_* code of a
 * directory read that failed even after retries. An empty g_browse with a
 * nonzero code is an album that COULD NOT BE READ, not an album with no
 * tracks; the tracklist screen should say so (and offer Back), because the
 * two look identical otherwise and one of them is a disk fault. */
static int g_browse_err;

/*
 * Which library song each g_browse row IS — a g_songs index, or -1 for a file
 * the library has no record of — and the row's ordering key. Both filled by
 * browse_bind() at the end of every browse_load, so the tracklist screen,
 * the album queue and the resume path all read the same answer.
 *
 * Rows bind to songs by FILE CLUSTER: the resolve pass already bound each
 * record to its directory entry (by file_hash), and the row was made from
 * that same entry, so the cluster is the one fact both sides hold exactly.
 * Matching by name was what this replaces — the row's ext-trimmed name
 * against the record's file[] field, which for a long filename is a
 * truncated string no row could equal, so those rows showed no duration,
 * no title and a made-up gutter number while the same file played fine.
 */
static int16_t  g_browse_song[BROWSE_MAX];
static uint32_t g_browse_key[BROWSE_MAX];

static int browse_key_cmp_idx(uint16_t a, uint16_t b)
{
    uint32_t ka = g_browse_key[a], kb = g_browse_key[b];
    return (ka > kb) - (ka < kb);
}

static void browse_bind(uint32_t dir_clus)
{
    int n = g_browse_n;
    for (int i = 0; i < n; i++) {
        g_browse_song[i] = -1;
        g_browse_key[i]  = 0xFFFFFFFFu;       /* unbound rows sort last */
    }
    /* One pass over the library: the album's songs are the ones whose folder
     * is this one, and each finds its row by cluster among at most BROWSE_MAX.
     * ~6000 integer compares plus a few hundred per album open — an event
     * that just read a directory off the disk. */
    for (int s = 0; s < g_songs_n; s++) {
        const lib_song_t *sg = &g_songs[s];
        if (sg->dir_clus != dir_clus || sg->file_clus == 0) continue;
        for (int i = 0; i < n; i++) {
            if (g_browse[i].is_dir || g_browse[i].clus != sg->file_clus) continue;
            g_browse_song[i] = (int16_t)s;
            g_browse_key[i]  = ((uint32_t)sg->disc << 16) | sg->track;
            break;
        }
    }

    /*
     * Order the rows by the index's (disc, track). They arrive in raw FAT
     * directory order — whatever order the importer happened to copy the
     * files in — while the gutter prints the record's track number, so an
     * album whose numbers did not come from the filenames read 7, 2, 11 down
     * the screen. One authority: the index. Ties, and rows the index does not
     * know, keep directory order (the sort is stable; unbound keys are max).
     * The queue is built from g_browse in this order too, so the album PLAYS
     * in it as well as listing in it.
     *
     * Applied in place by following cycles, inverted first — the same steps
     * as library_finish, for the same reason (the sort yields a gather order
     * and the swap loop scatters).
     */
    static uint16_t order[BROWSE_MAX];
    for (int i = 0; i < n; i++) order[i] = (uint16_t)i;
    merge_sort_idx(order, n, g_sort_tmp, browse_key_cmp_idx);
    uint16_t *inv = g_sort_tmp;             /* free again once the sort is done */
    for (int k = 0; k < n; k++) inv[order[k]] = (uint16_t)k;
    for (int i = 0; i < n; i++) {
        while (inv[i] != (uint16_t)i) {
            int j = inv[i];
            browse_entry_t te = g_browse[i];
            g_browse[i] = g_browse[j];
            g_browse[j] = te;
            int16_t ts = g_browse_song[i];
            g_browse_song[i] = g_browse_song[j];
            g_browse_song[j] = ts;
            uint32_t tk = g_browse_key[i];
            g_browse_key[i] = g_browse_key[j];
            g_browse_key[j] = tk;
            uint16_t ti = inv[i];
            inv[i] = inv[j];
            inv[j] = ti;
        }
    }
}

/* Read an album's tracklist (the folder at `dir_clus`) into g_browse, bound to
 * the library and in the index's order (browse_bind). Only ever called at
 * depth 1 now — the album LIST is the index-driven g_albums. */
static void browse_load(fat32_t *fs, uint32_t dir_clus)
{
    g_list_epoch++;
    g_cur_dir  = dir_clus;
    g_browse_n = 0;
    g_art_clus = 0;                      /* re-captured by browse_collect below */
    g_art_size = 0;
    /* One directory, opened by hand: give it the full set of retries rather
     * than whatever the boot load left in the budget — the budget exists to
     * bound a thousand-album boot, not a single user-initiated open. */
    g_lib_retry_budget = LIB_READDIR_RETRIES;
    g_browse_err = lib_readdir(fs, dir_clus, browse_collect, 0, &g_browse_n);
    if (g_browse_err != 0) {
        g_art_clus = g_art_size = 0;     /* nothing from a walk we don't trust */
        return;
    }
    /* The folder just read cleanly. If the load-time pass could not read it,
     * this is the re-attempt the flag promised: resolve it now, so its songs
     * become playable from Songs/Genres/shuffle and the About count drops.
     * Before the bind, which needs the clusters the resolve installs. */
    int ai = album_by_clus(dir_clus);
    if (ai >= 0 && g_albums[ai].unreadable) (void)album_resolve(fs, ai);
    browse_bind(dir_clus);
}

/* After entering an album folder: load its hero art, pull each track's
 * disc/duration/number from the index (matched by folder+filename), and build
 * the display view with "Disc N" section headers for a multi-disc album. */
static void detail_load_meta(fat32_t *fs)
{
    detail_art_load(fs);
    g_album_track_n = 0;
    int maxd = 0;
    for (int i = 0; i < g_browse_n; i++) {
        g_track_dur[i] = 0;
        g_track_disc[i] = 0;
        g_track_num[i] = 0;
        g_track_title[i] = 0;
        if (g_browse[i].is_dir) continue;
        g_album_track_n++;
        /* The row's song, bound by cluster in browse_bind. No longer gated on
         * "loaded from the index": a scanned library binds the same way and
         * has durations and numbers of its own to show. */
        int s = g_browse_song[i];
        if (s >= 0) {
            g_track_dur[i]  = (uint16_t)g_songs[s].duration_s;
            g_track_disc[i] = (uint8_t)g_songs[s].disc;
            g_track_num[i]  = g_songs[s].track;
            if (g_songs[s].title[0]) g_track_title[i] = g_songs[s].title;
            if (g_songs[s].disc > maxd) maxd = g_songs[s].disc;
        }
    }
    g_detail_multidisc = (maxd > 1);
    /* Sum the runtime once, here — detail_render used to re-add every track's
     * duration on every paint just to print one meta line. */
    g_detail_total_s = 0;
    for (int i = 0; i < g_browse_n; i++) g_detail_total_s += g_track_dur[i];
    g_det_view_n = 0;
    int prev = -1;
    for (int i = 0; i < g_browse_n && g_det_view_n < DET_VIEW_MAX - 1; i++) {
        if (g_browse[i].is_dir) continue;
        int d = g_track_disc[i];
        if (g_detail_multidisc && d > 0 && d != prev) {
            g_det_view[g_det_view_n++] = (int16_t)(-(d + 1));   /* Disc header */
            prev = d;
        }
        g_det_view[g_det_view_n++] = (int16_t)i;
    }
}

/* ---------------------------------------------------------------------------
 * Presenting: damage-only pushes + partial list repaints
 *
 * lcd_present_fb streams all 38,400 32-bit words to the BCM with IRQs MASKED —
 * the audio DMA is single-shot and re-kicked inside its own ISR, so only the
 * 16-frame I2S FIFO (~363 us) covers a delayed interrupt. Pushing a whole frame
 * to move a selection bar one row is what forced the old fixed 150 ms repaint
 * throttle while playing. So: renderers report what they touched (console.c's
 * damage rect), we present only that, and a plain selection move repaints two
 * rows instead of clearing and redrawing the panel.
 * ------------------------------------------------------------------------- */

/* Measured cost of the last present, used to pace the next one. Seeded to a
 * full-frame-ish value so the first push is paced conservatively. */
/* Declared up with the stats line; seeded to a deliberate over-estimate so
 * the first present is throttled conservatively before it is measured. */
static uint32_t g_present_cost_us = 30000u;

/*
 * Scroll diagnostics, reported on a "core: ui" UART line every 5 s in which
 * something was painted (see ui_stats_emit). These exist because "the list
 * lags" has four candidate mechanisms — the render, the present, the cover
 * reads issued under the wheel, and the playing-time throttle — and the only
 * number the log carried was the last present's cost. Every counter is a
 * store or a compare on a path that already reads the timer; none is on the
 * audio path.
 */
static uint32_t g_ui_render_us;        /* CPU time of the last list paint, no present */
static uint32_t g_ui_render_max_us;    /* worst paint in the window                    */
static uint32_t g_ui_present_max_us;   /* worst present in the window                  */
static uint32_t g_ui_full, g_ui_partial;   /* presents in the window, by path        */
static uint32_t g_ui_art_reads;        /* cover loads issued (each a disk read)        */
static uint32_t g_ui_art_max_us;       /* longest single pump: a spin-up lands here    */
static uint32_t g_ui_art_held;         /* passes that refused to wake a parked platter */

static uint32_t present_gap_us(void);  /* defined just below, with the throttle */

static void ui_stats_emit(void)
{
    if (g_ui_full + g_ui_partial == 0 && g_ui_art_reads == 0 && g_ui_art_held == 0) {
        return;                            /* nothing painted: keep the log quiet */
    }
    const player_stats_t *ps = player_stats();
    uart_puts("core: ui full ");        uart_dec((int)g_ui_full);
    uart_puts(" partial ");             uart_dec((int)g_ui_partial);
    uart_puts(" render_us ");           uart_dec((int)g_ui_render_us);
    uart_puts(" max ");                 uart_dec((int)g_ui_render_max_us);
    uart_puts(" present_us ");          uart_dec((int)g_present_cost_us);
    uart_puts(" max ");                 uart_dec((int)g_ui_present_max_us);
    uart_puts(" gap_us ");              uart_dec((int)present_gap_us());
    uart_puts(" art reads ");           uart_dec((int)g_ui_art_reads);
    uart_puts(" max_us ");              uart_dec((int)g_ui_art_max_us);
    uart_puts(" held ");                uart_dec((int)g_ui_art_held);
    uart_puts(" parked ");              uart_dec(ata_is_parked());
    uart_puts(" decode_us_per_kframe "); uart_dec(ps ? (int)ps->decode_us_per_kframe : -1);
    uart_puts(" ring_low_pct ");        uart_dec((int)player_buf_pct());
    uart_putc('\n');
    g_ui_full = g_ui_partial = 0;
    g_ui_render_max_us = g_ui_present_max_us = 0;
    g_ui_art_reads = g_ui_art_max_us = g_ui_art_held = 0;
}

/* Present whatever has been drawn since the last console_damage_reset(), then
 * clear the damage. A full-screen damage rect (any console_clear) goes out via
 * the full-frame fast path, exactly as before. */
static void ui_present_damage(void)
{
    int x, y, w, h;
    if (console_damage_get(&x, &y, &w, &h)) {
        uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
        if (x <= 0 && y <= 0 && w >= LCD_WIDTH && h >= LCD_HEIGHT) {
            lcd_present_fb(console_framebuffer());
        } else {
            lcd_present_rect(console_framebuffer(), x, y, w, h);
        }
        g_present_cost_us = mmio_read32(USEC_TIMER_ADDR) - t0;
    }
    console_damage_reset();
}

/* How long to wait between repaints WHILE PLAYING. Self-tuning: space repaints
 * at ~4x what the last present actually cost. A cheap partial present paces
 * fast (a responsive wheel), a full-frame push still backs off to about the
 * old fixed 150 ms — but only when it really is a full frame.
 *
 * The 4x was chosen when the pixel push ran with IRQs MASKED end to end, to
 * keep that masked time under ~1/4 of the loop. lcd.c now releases the I-bit
 * between panel rows, so the DMA re-kick deadline no longer bears on this; what
 * the gap still buys is CPU for the decoder. player_pump decodes exactly one
 * 1024-frame step per pass (~23 ms of audio), so a pass that also renders and
 * presents yields less audio than it consumes, and the gap is what keeps such
 * passes the minority. Whether 4x is the right share is a question for the
 * "core: ui" line (render_us, present_us, decode_us_per_kframe, ring_low_pct):
 * on the album list every detent scrolls the window (ui_scroll_window anchors
 * the selection a third of the way down), so every scroll present is a FULL
 * frame and this multiplier is the frame rate. Do not lower it on a guess. */
static uint32_t present_gap_us(void)
{
    uint32_t gap = g_present_cost_us * 4u;
    if (gap < 20000u)  gap = 20000u;      /* <=50 fps: no point going faster    */
    if (gap > 150000u) gap = 150000u;     /* the old worst-case throttle        */
    return gap;
}

/* A list screen described generically, so one routine can repaint the rows that
 * changed without knowing which screen it is. `row` draws list row r showing
 * item `idx` (it reads that screen's own selection to decide highlighting). */
typedef struct {
    const char *title;                 /* header title (for the overlap check) */
    int         count, visible, rh, y0, sel;
    void      (*row)(int r, int idx);
    char        right[16];             /* header "n / m" value ("" = none)     */
} list_view_t;

/* Describe the screen on top of the stack, or return 0 if it isn't a list that
 * can be partially repainted (Settings/Charging/Now Playing render elsewhere,
 * empty lists draw a placeholder instead of rows). */
static int list_view_current(list_view_t *v)
{
    v->right[0] = '\0';
    v->rh       = ROW_H;
    v->y0       = LIST_Y0;
    v->visible  = LIST_ROWS;
    switch (scr_cur()) {
    case SCR_MENU:
        g_menu_items = g_main_menu;
        g_menu_sel   = g_main_sel;
        v->title = "Core";
        v->count = main_menu_count();
        v->sel   = g_main_sel;
        v->row   = menu_row_draw;
        break;
    case SCR_MUSIC:
        g_menu_items = g_music_menu;
        g_menu_sel   = g_music_sel;
        v->title = "Music";
        v->count = MU_COUNT;
        v->sel   = g_music_sel;
        v->row   = menu_row_draw;
        break;
    case SCR_ARTISTS:
        v->title = "Artists";
        v->count = g_artists_n;
        v->sel   = g_artist_sel;
        v->row   = artists_row_draw;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_SONGS:
        v->title   = g_songview_artist[0] ? g_songview_artist : "Songs";
        v->count   = g_songview_n;
        v->sel     = g_song_sel;
        v->row     = songs_row_draw;
        v->rh      = ROW_H2;
        v->visible = LIST_ROWS2;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_GENRES:
        v->title = "Genres";
        v->count = g_genres_n;
        v->sel   = g_genre_sel;
        v->row   = genres_row_draw;
        fmt_count(v->right, v->sel + 1, v->count);
        break;
    case SCR_QUEUE:
        v->title = "Now Playing";
        v->count = player_queue_len();
        v->sel   = g_queue_sel;
        v->row   = queue_row_draw;
        fmt_count(v->right, player_queue_current() + 1, v->count);
        break;
    case SCR_BROWSER:
        if (g_dir_depth == 0) {
            v->title   = g_artist_filter[0] ? g_artist_filter : "Albums";
            v->count   = albumlist_count();
            v->sel     = g_br_sel;
            v->row     = albumlist_row_draw;
            v->rh      = ROW_H2;
            v->visible = LIST_ROWS2;
            fmt_count(v->right, v->sel + 1, v->count);
        } else {
            /* The tracklist scrolls over the DISPLAY view (tracks interleaved
             * with "Disc N" headers), so the row space is view indices. */
            v->title   = "Albums";
            v->count   = g_det_view_n;
            v->sel     = detail_sel_view(g_det_sel);
            v->row     = detail_row_draw;
            v->y0      = DET_LIST_Y0;
            v->visible = DET_ROWS;
            fmt_count(v->right, g_det_sel + 1, g_browse_n > 0 ? g_browse_n : 1);
        }
        break;
    default:
        return 0;
    }
    return v->count > 0;
}

/* What the last paint showed, so the next one can tell a plain selection move
 * from a change that needs the whole panel. `chrome` fingerprints the shared
 * furniture (status strip name, battery, lock, playing row) — if any of it
 * moved, a two-row repaint would leave the screen stale. */
static struct {
    int      valid, scr, depth, sel, top, count, right_w;
    uint32_t chrome, epoch;
    uint32_t rows;      /* bit r: row r's content changed under a still view
                         * (a cover chip arrived) — repaint it, nothing else */
} g_lp;

static uint32_t chrome_key(void)
{
    uint32_t k = player_active() ? 1u : 0u;
    k = k * 31u + (player_paused() ? 1u : 0u);
    k = k * 31u + (uint32_t)(player_queue_current() + 1);
    k = k * 31u + (uint32_t)(g_locked ? 1 : 0);
    k = k * 31u + (uint32_t)(g_bat_pct + 1);
    return k;
}

/* Record what we just painted (called after every paint of a list screen). */
static void list_paint_note(void)
{
    list_view_t v;
    if (!list_view_current(&v)) {
        g_lp.valid = 0;
        return;
    }
    g_lp.valid   = 1;
    g_lp.scr     = (int)scr_cur();
    g_lp.depth   = g_dir_depth;
    g_lp.sel     = v.sel;
    g_lp.count   = v.count;
    g_lp.top     = ui_scroll_window(v.sel, v.count, v.visible);
    g_lp.right_w = v.right[0] ? text_width(v.right, FONT_SMALL) : 0;
    g_lp.chrome  = chrome_key();
    g_lp.epoch   = g_list_epoch;
    g_lp.rows    = 0;                     /* a full paint drew every row       */
}

/* Repaint the current screen by redrawing ONLY what changed under a still
 * view: the two rows a selection move touched (plus the header's "n / m" value
 * and the scrollbar), and any row whose cover chip arrived since the last
 * paint (g_lp.rows). Returns 0 when that isn't provably sufficient (different
 * screen, scrolled window, changed contents or chrome) — the caller then does
 * the full render.
 *
 * The chip case used to fall through to the full render: with the selection
 * unchanged there was "nothing" for the partial path to do, so each cover that
 * landed on the album list cleared and re-pushed the whole panel — six times
 * over for a fresh window, each one re-arming the playing-time throttle at the
 * full-frame cost, in the middle of the scroll the covers were loading for. */
static int list_repaint_partial(void)
{
    list_view_t v;
    if (!g_lp.valid || !list_view_current(&v)) return 0;
    if (g_lp.scr != (int)scr_cur() || g_lp.depth != g_dir_depth) return 0;
    if (g_lp.epoch != g_list_epoch)                              return 0;
    if (g_lp.count != v.count || g_lp.chrome != chrome_key())    return 0;
    int moved = (v.sel != g_lp.sel);
    if (!moved && g_lp.rows == 0)                                return 0;
    int top = ui_scroll_window(v.sel, v.count, v.visible);
    if (top != g_lp.top) return 0;        /* window scrolled: every row moved   */

    uint32_t rows = g_lp.rows;
    if (moved) {
        int r_old = g_lp.sel - top, r_new = v.sel - top;
        if (r_old < 0 || r_old >= v.visible || r_new < 0 || r_new >= v.visible) {
            return 0;                      /* off-window selection: play it safe */
        }

        /* Header value: clear only as wide as the old/new text. If the title
         * reaches into that box, the clear would eat it — fall back to a full
         * paint. */
        int rw = v.right[0] ? text_width(v.right, FONT_SMALL) : 0;
        int cw = ((rw > g_lp.right_w) ? rw : g_lp.right_w) + 4;
        if (cw > 4) {
            int cx = LCD_WIDTH - 12 - cw;
            if (12 + 20 + text_width(v.title, FONT_HEADER) > cx) return 0;
            console_fill_rect(cx, HDR_BASE - 12, cw, 16, LINEN_SURFACE);
            if (rw) {
                ui_text(LCD_WIDTH - 12 - rw, HDR_BASE - 1, v.right, FONT_SMALL,
                        LINEN_MUTED2);
            }
        }
        rows |= (1u << r_old) | (1u << r_new);
    }

    /* The rows: clear each band (the new content may be narrower) + redraw. */
    for (int r = 0; r < v.visible; r++) {
        if (!(rows & (1u << r))) continue;
        console_fill_rect(0, v.y0 + r * v.rh, LCD_WIDTH, v.rh, LINEN_SURFACE);
        int idx = top + r;
        if (idx < v.count) v.row(r, idx);
    }
    if (moved) {
        ui_scrollbar(v.y0, top, v.visible, v.count);
    }
    g_lp.rows = 0;
    return 1;
}

/* Cover loads under a moving wheel (the album-list pump in run_ui; see the
 * block comment there). A detent within SETTLE means the list is scrolling:
 * one disk read per pass, not six. A parked platter is only woken for covers
 * after QUIET of wheel silence — long enough that a deliberate detent-by-
 * detent scroll (~300 ms apart) never trips a spin-up mid-gesture. */
#define CHIP_WHEEL_SETTLE_US   150000u
#define CHIP_SPINUP_QUIET_US   500000u

/* The selected row's initial on the alphabetised lists (songs / artists /
 * albums), read straight off the already-sorted arrays; 0 on screens where an
 * A-Z cue would mean nothing. */
/*
 * The A-Z locator letter for a row, or 0 on screens that have no alphabetical
 * order to locate WITHIN.
 *
 * SONGS ONLY, deliberately. Letter stepping is worth its cost on the one list
 * that is thousands of entries long and sorted by title; on a fixed six-row
 * menu it is meaningless, and on Artists/Albums the lists are short enough that
 * row acceleration already gets you there. Returning 0 here is also what stops
 * wheel_move() from trying to letter-step a list that cannot be letter-stepped
 * — see the guard there.
 */
static char list_initial_at(int idx)
{
    if (scr_cur() != SCR_SONGS) {
        return 0;
    }
    if (idx >= 0 && idx < g_songview_n) {
        return initial_of(g_songs[g_songview[idx]].title);
    }
    return 0;
}

static char list_sel_initial(void)
{
    return (scr_cur() == SCR_SONGS) ? list_initial_at(g_song_sel) : 0;
}

/* The wheel's clock (the ui/wheel.h seam): the free-running USEC_TIMER. */
static uint32_t wheel_clock(void)
{
    return mmio_read32(USEC_TIMER_ADDR);
}

/* The letter itself, on a centred plate over the flying list. Its presence is
 * exactly the signal that the wheel is stepping LETTERS rather than rows —
 * there is deliberately no state where the plate is up and the wheel is still
 * grinding through individual songs. */
static void az_overlay_render(char ch)
{
    const int PW = 66, PH = 66;
    int px = (LCD_WIDTH - PW) / 2, py = (LCD_HEIGHT - PH) / 2;
    fill_round_rect_aa(px - 1, py - 1, PW + 2, PH + 2, 13, LINEN_BORDER);
    fill_round_rect_aa(px, py, PW, PH, 12, LINEN_PLATE);
    char s[2] = { ch, '\0' };
    int w = text_width(s, FONT_TITLE);
    ui_text(px + (PW - w) / 2, py + PH / 2 + 8, s, FONT_TITLE, LINEN_INK);

#if AZ_SHOW_TPS
    {
        char n[12];
        u32_to_dec(n, wheel_tps());
        int nw = text_width(n, FONT_SMALL);
        ui_text(px + (PW - nw) / 2, py + PH - 4, n, FONT_SMALL, LINEN_MUTED_D);
    }
#endif
}

/* Busy-wait `us` microseconds (PWM already off) — for the silent gap in the
 * multi-burst clicker profiles. Short enough not to underrun the decode buffer. */
static void ui_spin_us(uint32_t us)
{
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us) { /* spin */ }
}

/* Piezo navigation click. g_settings.clicker selects the sound profile (0 = Off;
 * 1..7 = Tick / Click / Pop / Blip / Tock / Double / Chirp). Single-tone
 * profiles are one (Hz, µs) burst; Double is two bursts with a gap; Chirp is a
 * rising three-tone sweep. Order matches settings.c CLICK_L. */
static void ui_click(void)
{
    switch (g_settings.clicker) {
        case 1: piezo_click_ex(3000, 3000); break;   /* Tick  — crisp mid tick   */
        case 2: piezo_click_ex(4500, 2000); break;   /* Click — higher, shorter  */
        case 3: piezo_click_ex(1800, 5000); break;   /* Pop   — lower, fuller    */
        case 4: piezo_click_ex(6000, 1500); break;   /* Blip  — high, ultra-short*/
        case 5: piezo_click_ex(1000, 4000); break;   /* Tock  — low, woody       */
        case 6:                                       /* Double — two quick taps  */
            piezo_click_ex(4200, 1500);
            ui_spin_us(22000);
            piezo_click_ex(4200, 1500);
            break;
        case 7:                                       /* Chirp — rising sweep     */
            piezo_click_ex(3000, 1100);
            piezo_click_ex(4000, 1100);
            piezo_click_ex(5200, 1200);
            break;
        default: break;                               /* 0 = Off                  */
    }
}

/* ---------------------------------------------------------------------------
 * Resume playback position across a power cycle.
 *
 * WHAT IS STORED, AND WHY IT IS A NAME
 *
 * settings_t carries three fields (resume_hash / resume_secs / resume_total)
 * that ride along in the CORECFG.DAT record. The locator is the folded
 * name_hash() of the track's ext-trimmed FILENAME as the queue displays it
 * (the on-disk stem, which is also lib_song_t.stem_hash once a record has
 * bound to its file) — not a queue index, not a song index, not a cluster. Every one of those is a statement about the
 * library as it happened to be laid out when we saved: re-import the music,
 * rebuild CORELIB.IDX, add one album, and index 412 is a different song while
 * cluster 918233 may be somebody else's file. The filename is the only handle
 * that means the same thing on the other side of a rebuild, which is exactly
 * why the library already binds records to files by this same hash.
 *
 * A name is not unique, though — "01 Intro.flac" lives in half the albums on a
 * real library — so resume_total (the decoder's track length) is kept as a
 * cross-check and the restore declines an ambiguous match outright. Resuming
 * nothing is a shrug; resuming the WRONG track is a bug.
 *
 * WHEN IT IS CAPTURED (the write budget)
 *
 * Never per second, and never per pass. The position is snapshotted only at
 * moments where it actually changed in a way worth a kilobyte of disk:
 *
 *   - the track changed (player_open_seq bumped) — the important half is
 *     WHICH track, and that is the only moment it moves;
 *   - pause/unpause — the "I'm putting this down" moment;
 *   - playback started or stopped;
 *   - otherwise at most once per RESUME_SAMPLE_US while a track plays, so a
 *     battery pull mid-album loses minutes, not the album;
 *   - forced on the way into suspend / PMU standby, before the drive parks.
 *
 * Each of those calls settings_touch(), so it inherits the existing debounce
 * AND the "don't spin the platters up for 1 KB" guard in settings_commit() —
 * a capture during playback rides out on the next anti-skip refill rather than
 * costing its own spin-up. Continuous playback therefore costs roughly one
 * write per track (~20/hour); an idle or paused device costs none.
 * ------------------------------------------------------------------------- */

/* Longest a single track may play before we re-stamp the position. 5 minutes:
 * a full-album track rarely outlives it, so in practice the track-change edge
 * is what fires and this is only the backstop for long mixes and podcasts. */
#define RESUME_SAMPLE_US   300000000u          /* 300 s */

/* Below this, don't bother restoring a position — the user is at the top of
 * the track for any purpose they'd notice, and it saves a seek. */
#define RESUME_MIN_SECS    10u

/* How far the library's indexed duration may differ from the decoder's before
 * we stop believing the two describe the same file. Both are truncated to
 * whole seconds from different sources, so they disagree by a second on
 * perfectly good matches. */
#define RESUME_DUR_SLOP    2u

static uint32_t g_resume_open_seq;      /* player_open_seq() when last captured */
static uint32_t g_resume_last_us;       /* when we last considered capturing     */
static uint32_t g_resume_end_seq;       /* last seen player_end_seq()            */
static int      g_resume_was_paused;    /* pause state when last captured       */

/*
 * Snapshot the playing track + position into g_settings (debounced onto disk
 * by settings_commit). Cheap and idempotent: re-capturing the same track at
 * the same second does not mark anything dirty, so the callers below can be
 * liberal about when they call it.
 */
static void resume_capture(void)
{
    if (!g_settings.resume_on_startup) {
        /* Switched off: don't merely stop recording — drop what is already
         * stored. Otherwise turning it back on next month resumes whatever the
         * user was listening to before they turned it off, which reads as the
         * device remembering something it was told to forget. */
        if (g_settings.resume_hash != 0) {
            g_settings.resume_hash  = 0;
            g_settings.resume_secs  = 0;
            g_settings.resume_total = 0;
            settings_touch();
        }
        return;
    }
    if (!player_active()) {
        return;                        /* nothing loaded — keep the last one */
    }
    const char *nm = player_track_name();
    if (nm == 0 || nm[0] == '\0') {
        return;
    }

    uint32_t h = name_hash(nm);
    uint32_t e = player_elapsed_s();
    if (h == g_settings.resume_hash && e == g_settings.resume_secs) {
        return;                        /* nothing moved — no write to schedule */
    }
    g_settings.resume_hash  = h;
    g_settings.resume_secs  = e;
    g_settings.resume_total = player_total_s();
    settings_touch();
}

/*
 * Find the library song whose on-disk stem folds to `hash`. Returns a
 * g_songs index, or -1 when there is no safe answer.
 *
 * "Safe" is the whole point. A name match that the duration also confirms is
 * taken immediately. A name match with no confirmation is taken only when the
 * name is UNIQUE across the library — otherwise we would be picking one of
 * several "01 Intro.flac" at random, and a coin-flip is not a resume.
 *
 * The name compared is stem_hash — the hash of the name the file HAS, taken
 * from its directory entry when the record bound, which is the same string
 * the queue shows and resume_capture hashed. It used to be name_hash(file)
 * over the index's copy of the filename, and for a filename past 63 bytes
 * that copy is a truncated string that hashes like nothing on the disk: the
 * track played, the position was saved, and the restore never found it. A
 * record that never bound keeps that provisional hash — exact for a short
 * name, matching nothing for a long one — so it can still count as a
 * same-named twin below, but can never be the song that is opened.
 *
 * One linear pass over g_songs at boot: ~6000 integer compares, well under a
 * millisecond, against a library load that already took seconds.
 */
static int resume_find_song(uint32_t hash, uint32_t total_s)
{
    int best = -1, n_named = 0;

    for (int i = 0; i < g_songs_n; i++) {
        if (hash == 0 || g_songs[i].stem_hash != hash) {
            continue;
        }
        n_named++;
        if (!g_songs[i].file_clus) {
            continue;                  /* indexed but not on the disk any more */
        }
        uint32_t d = g_songs[i].duration_s;
        if (d != 0 && total_s != 0 &&
            d + RESUME_DUR_SLOP >= total_s && total_s + RESUME_DUR_SLOP >= d) {
            return i;                  /* name AND length agree — this is it */
        }
        if (best < 0) {
            best = i;
        }
    }
    return (n_named == 1) ? best : -1;
}

/*
 * Re-open the saved track at the saved position, PAUSED, at boot.
 *
 * Three things this deliberately does NOT do:
 *
 *   - it does not play. Coming back on and having music start on its own is
 *     hostile, so the transport is left paused with the position already set;
 *     the user presses PLAY. The codec is muted across the open because
 *     player_open_current() unconditionally starts the DAC off a primed ring —
 *     without the mute there is a click between the open and the pause;
 *   - it does not guess. Any failure at any step — setting off, no locator, no
 *     unambiguous song, the album folder no longer lists the file, the open
 *     falling through to a different track — leaves the device exactly as if
 *     nothing had been saved;
 *   - it does not seek an MP3. dr_mp3 has no seek table and scans from the
 *     start, which for a podcast resumed at 50 minutes is a multi-second
 *     freeze on the boot path. FLAC seeks through its SEEKTABLE in O(log n),
 *     so it gets the position and MP3 gets the track cued at 0:00.
 */
static void resume_restore(fat32_t *fs)
{
    if (!g_settings.resume_on_startup || g_settings.resume_hash == 0) {
        return;
    }
    int si = resume_find_song(g_settings.resume_hash, g_settings.resume_total);
    if (si < 0) {
        return;                        /* deleted, renamed, or ambiguous */
    }

    /*
     * Rebuild the queue as the track's ALBUM — the context Next/Prev should
     * walk, and the one context we can reconstruct honestly. The real queue
     * might have been a shuffle of the whole library or a genre view; none of
     * that survives a power cut, and inventing it would be a worse lie than
     * the album the track actually lives in.
     *
     * browse_collect() only lists FILES at depth 1 (depth 0 is the album
     * list), so borrow the depth for the read and hand it straight back.
     */
    int saved_depth = g_dir_depth;
    g_dir_depth = 1;
    uint32_t rt0 = boot_ms_now();
    browse_load(fs, g_songs[si].dir_clus);
    g_boot_res_dir_ms = boot_ms_now() - rt0;
    g_dir_depth = saved_depth;

    /* The row is the file the song bound to: same cluster. Not the name — the
     * row's name and the song's are only the same string BECAUSE the song
     * bound, and the cluster is the binding itself. */
    int idx = -1;
    for (int i = 0; i < g_browse_n; i++) {
        if (!g_browse[i].is_dir && g_browse[i].clus == g_songs[si].file_clus) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        g_browse_n = 0;                /* the folder no longer holds the file */
        return;
    }

    /* Mute BEFORE the open: audio_bringup() re-latches whatever gain the HAL
     * currently holds, so a 0 here means the DAC comes up silent and the pause
     * lands before a single audible sample. Restored at the bottom. */
    hal_volume_set(0);
    uint32_t ot0 = boot_ms_now();
    player_play_queue(g_browse, g_browse_n, idx, g_art_clus, g_art_size);
    player_pause();
    g_boot_res_open_ms = boot_ms_now() - ot0;

    /* player_play_queue() skips forward over a track it cannot open. That is
     * right for a user pressing SELECT and wrong for a silent restore — being
     * handed a different song than the one you left is precisely the failure
     * this whole path is written to avoid. */
    if (!player_active() || player_queue_current() != idx) {
        player_stop();
        hal_volume_set(g_volume);
        return;
    }

    if (g_browse[idx].fmt == 0 && g_settings.resume_secs >= RESUME_MIN_SECS) {
        /* Clamped to the track length inside player_seek_to, and a refusal is
         * simply "resumed at 0:00" — never a reason to abandon the track. */
        uint32_t st0 = boot_ms_now();
        (void)player_seek_to(g_settings.resume_secs);
        g_boot_res_seek_ms = boot_ms_now() - st0;
    }
    hal_volume_set(g_volume);

    /* Put the cursor on Now Playing: it is the row this whole feature exists
     * to make one click away, and it only appears while a track is loaded. */
    g_main_sel = MM_NOWPLAYING;
}

/* Render whatever screen is on top of the stack into the framebuffer (no
 * present) — used to paint context behind the lock/unlock plate. */
static void paint_current_screen(void)
{
    g_mq.active = 0;                       /* a fresh paint re-registers any marquee */
    switch (scr_cur()) {
    case SCR_MENU:    main_menu_render();  break;
    case SCR_MUSIC:   music_menu_render(); break;
    case SCR_ARTISTS: artists_render(g_artist_sel); break;
    case SCR_SONGS:   songs_render(g_song_sel);     break;
    case SCR_GENRES:  genres_render(g_genre_sel);   break;
    case SCR_BROWSER: browse_render(g_dir_depth ? g_det_sel : g_br_sel); break;
    case SCR_QUEUE:   queue_render(g_queue_sel); break;
    case SCR_SETTINGS: settings_render_cur(); break;
    case SCR_NOWPLAYING:
        nowplaying_render(player_track_name(), player_elapsed_s(),
                          player_total_s(), player_buf_pct());
        break;
    case SCR_BATTERY:
        screen_battery_render(BATTWARN_DISKSAFE);
        break;
    case SCR_CHARGING:
        screen_charging_render(g_bat_pct, power_is_charging(), power_is_external());
        break;
    }
}

/* Quiesce and enter PMU deep-sleep standby (triggered by holding PLAY). Stops
 * audio + the decode/disk feed, blanks the panel + backlight, then hands off to
 * the PMU. Never returns — a button press wakes the device by re-running the
 * boot path (a cold boot of the firmware, not a resume). */
_Noreturn static void enter_standby(void)
{
    /* BEFORE player_stop(): once the transport is torn down there is no track
     * name and no elapsed clock left to record. */
    resume_capture();
    player_stop();
    settings_commit(1);                   /* persist before the PMU cuts power */
    console_clear(0x0000);                /* blank BEFORE the power cut so no */
    lcd_present_fb(console_framebuffer()); /* stale colour lingers on the panel */
    cpu_wait_ms(80);                      /* let the BCM push the black frame  */
    backlight_set(0);
    power_standby();                      /* PMU cuts power — does not return */
    for (;;) {
    }
}

/*
 * Suspend: the seamless "off". Keeps the CPU + RAM alive so wake RESUMES the
 * running firmware instantly (no cold boot, so no ipl2 menu) — but actually
 * quiesces the power-hungry parts: audio paused, the hard drive spun DOWN (ATA
 * standby), backlight off, panel blanked to black. Any button wakes it: spin
 * the drive back up, restore the screen, resume playback. Holding the trigger
 * PLAY past ~5s escalates to a true PMU power-down (everything off, but wakes
 * via a cold boot). `play_down_us` is when the hold began, for that escalation.
 *
 * Caveat vs a true power-down: the LCD controller + CPU stay powered (the panel
 * is dark, not electrically off), so it draws more than deep-sleep — fine for
 * short off/on, which is what this is for.
 */
static void suspend_to_ram(uint32_t play_down_us)
{
    wheel_event_t drain;
    int was_playing = player_active() && !player_paused();
    if (was_playing) {
        player_pause();                   /* silence + stop feeding the disk */
    }
    /* Last chance to persist: after this the drive is parked and the user may
     * never wake the device again (battery pull, dead cell). Forced, so a
     * change made 1 s ago is not lost to the debounce. The position goes in
     * the same write — the pause above froze the elapsed clock, so this is the
     * exact second the user stopped listening. */
    resume_capture();
    settings_commit(1);
    cpu_unboost();                        /* the boost refcount is >=1 here (we are
                                           * entered from BL_FULL), so without this
                                           * the "sleeping" device holds the 80 MHz
                                           * operating point for the whole suspend */
    ata_standby();                        /* spin the platters down (quiet, low-power) */
    /* Clear to black BEFORE cutting the backlight, so the transflective panel
     * doesn't faintly ghost the last UI in ambient light while asleep. Wake
     * repaints the real screen while the backlight is still off (below), so the
     * black is never seen as a flash. */
    console_clear(0x0000);
    lcd_present_fb(console_framebuffer());
    backlight_set(0);

    /* Wait for the trigger PLAY hold to release (so it can't instantly wake us).
     * Held past ~5s total => a real power-down instead. */
    while (clickwheel_buttons() & WHEEL_BTN_PLAY) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - play_down_us) > 5000000u) {
            enter_standby();              /* true off (PMU) — does not return */
        }
        /* See the wake loop below for why this drain is load-bearing. */
        while (clickwheel_get_event(&drain)) { }
        cpu_wait_ms(20);
    }
    while (clickwheel_get_event(&drain)) { }   /* drop the trigger's latched events */

    /* Low-power idle until any button is pressed. The 100 Hz tick keeps sampling
     * the wheel into the latch through each cpu_wait, so a press is seen fast. */
    while (clickwheel_buttons() == 0) {
        /*
         * DRAINING HERE IS WHAT MAKES THE DEVICE WAKE AT ALL.
         *
         * clickwheel_buttons() returns a CACHED value that only the 100 Hz tick
         * sampler refreshes. When the Hold switch goes off, the sampler gates
         * the OPTO block's clock back on but deliberately defers the reset/
         * config sequence out of ISR context by setting s_need_bringup — and
         * then bails on every subsequent tick until something clears it. The
         * only thing that clears it is clickwheel_get_event().
         *
         * So a user who suspends and then flips Hold on and off again (the
         * standard reflex after "turning it off") left the cached button state
         * pinned at 0 forever: this loop span on a value that could never
         * change, dark and unresponsive, with a hardware reset the only escape.
         * The normal main loop never hit this because it drains every pass.
         *
         * Draining does not clear the cached state, so the loop condition is
         * unaffected — it only lets the deferred bring-up actually run.
         */
        while (clickwheel_get_event(&drain)) { }
        /*
         * Pump while suspended, so the codec actually powers down.
         *
         * The pause above stops the DMA but leaves the WM8758 fully powered —
         * PLL, VMID, DACs, headphone amps — and the thing that shuts it off is
         * the persistent-pause timeout inside player_pump(). This loop never
         * called pump, so a device "asleep" via Play-hold held the codec live
         * for the entire suspend, which is the most expensive way to do
         * nothing that this firmware is capable of.
         *
         * Safe to call here: pump's paused branch does the codec-off check and
         * returns immediately. No decode, no disk, no ring work — which
         * matters, because the drive is parked by this point and waking it
         * would defeat the whole exercise.
         */
        player_pump();
        cpu_wait_ms(30);
    }
    /* Swallow the wake press so it isn't also acted on as navigation. */
    while (clickwheel_buttons() != 0) {
        cpu_wait_ms(20);
    }
    while (clickwheel_get_event(&drain)) { }

    /* Re-boost BEFORE the drive and the repaint: cpu_boost/cpu_unboost are
     * refcounted, so this pairs with the unboost on the way in and keeps the
     * count balanced (an unmatched unboost would drive it negative the next
     * time the idle path unboosts). It also puts the ATA read and the render
     * back at 80 MHz, which is where their timing was calibrated. */
    cpu_boost();
    ata_wakeup();                         /* spin the drive back up before any read */
    paint_current_screen();               /* render the real screen while dark... */
    lcd_present_fb(console_framebuffer());
    backlight_set(g_settings.backlight_bright);  /* ...then light up straight to it */
    if (was_playing) {
        player_resume();
    }
}

/*
 * Panel sleep at idle is DISABLED: it leaves the screen solid WHITE until a
 * reboot, which is the worst possible failure on a device whose only debug
 * channel is that screen.
 *
 * Mechanism, from docs/hw/02-lcd.md: on the first LCD_UPDATE after LCD_SLEEP the
 * BCM re-runs its internal LCD panel init and is allowed up to 500 ms for it
 * ("After waking from sleep, the first update can take up to 500 ms ... because
 * the BCM is doing internal LCD panel init"). Our commit handshake budgets
 * BCM_IDLE_SPIN_LIMIT (~2 ms) and RE-KICKS LCD_UPDATE 16 times inside that
 * window, while every later present streams a fresh 150 KB frame straight into
 * the in-progress init — so the init never completes and the BCM latches. The
 * doc names the symptom outright: "If we wake the backlight before the first
 * update completes, the user sees a 500 ms white flash." We light the backlight
 * at the input site, ~400 lines before lcd_wake() runs, so it is white, and
 * there is no bcm_init() anywhere in the tree to recover with — hence the
 * reboot.
 *
 * The backlight LED is by far the larger draw and is already off in this state;
 * suspend_to_ram() makes the same trade deliberately ("the panel is dark, not
 * electrically off"). Do NOT re-enable this without (a) a real bcm_init()
 * bootstrap to recover a failed wake, (b) a wall-clock absorb window on the
 * first post-wake commit with the re-kick SUPPRESSED, and (c) deferring
 * backlight-on until that first present has retired.
 */
#define PANEL_SLEEP_AT_IDLE  0

/*
 * The UI: one event loop that pumps the background player every pass and
 * dispatches input to the current screen (Main menu / Music menu / Browser /
 * Now Playing) on a stack. MENU pops the screen WITHOUT stopping playback, so a
 * song keeps going while you navigate. Never returns.
 */
_Noreturn static void run_ui(fat32_t *fs)
{
    clickwheel_init();
    player_init(fs);
    charger_set_max_current(500);     /* LTC4066 HPWR: 100 mA cap until asserted */
    chip_placeholder_init();
    artcache_init();                  /* ways must start at key -1; .bss gives 0,
                                       * which is album 0's real index */
    library_ensure(fs);                   /* preload the index at boot (drive is
                                           * spinning) so Songs/Albums/Artists/
                                           * Genres open INSTANTLY, like Apple —
                                           * not a multi-second stall on first use */
    battery_refresh(1);                   /* prime the status-strip gauge         */
    g_volume = hal_volume_get();          /* reflect the codec's default gain      */
    g_dir_depth = 0;
    g_browse_n  = 0;
    g_br_sel = g_br_accum = 0;
    g_main_sel  = 0;
    g_music_sel = MU_ALBUMS;
    g_menu_accum = 0;
    g_artist_filter[0] = '\0';
    g_artist_sel = g_artist_accum = 0;
    /*
     * Settings: defaults FIRST, then let the saved record overwrite them.
     * config_load() only touches g_settings when it finds a record whose
     * magic, version and CRC all check out, so any failure — file absent,
     * torn write, corrupt slot, unresolvable cluster — leaves the full
     * default set in place. Defaults are the floor, never skipped.
     */
    settings_defaults(&g_settings);
    int cfg_ok = config_load(fs, &g_settings);
    uart_puts("core: cfg load ");
    uart_put_hex32((uint32_t)cfg_ok);
    uart_puts(" writable ");
    uart_put_hex32((uint32_t)config_writable());
    uart_puts(" seq ");
    uart_put_hex32(config_seq());
    /* The address a save WOULD target, printed before any write can happen.
     * The first on-device bring-up must check this against the LBA
     * tools/make_config.py --verify computes on the host (see the procedure
     * at the top of kernel/config.c) — a mismatch here is the failure mode
     * that destroys a music library. */
    uint32_t cfg_lba0 = 0, cfg_lba1 = 0;
    (void)config_probe_lba(0, &cfg_lba0);
    (void)config_probe_lba(1, &cfg_lba1);
    uart_puts(" lba ");
    uart_put_hex32(cfg_lba0);
    uart_putc('/');
    uart_put_hex32(cfg_lba1);
    uart_putc('\n');
    settings_apply();                     /* push shuffle/repeat/volume out       */
    /* Pick up where the user left off — after settings_apply (it needs the
     * saved volume, which the restore mutes across the open and puts back) and
     * after library_ensure (the locator is resolved against the library). Comes
     * up PAUSED; nothing here starts audio. A no-op when Resume is off, when
     * nothing was saved, or when the saved track can't be resolved. */
    uint32_t resume_t0 = boot_ms_now();
    resume_restore(fs);
    g_boot_resume_ms   = boot_ms_now() - resume_t0;
    /* Everything the user waits for is now done; the next thing that happens is
     * the first UI paint. Stamped here rather than after the present so the
     * number means "time until the device was ready", not "+1 frame". */
    g_boot_total_ms    = boot_ms_now();
    g_cfg_dirty = 0;                      /* loading is not a change to save back */
    g_scr_n = 0;
    scr_push(SCR_MENU);

    int      dirty = 1;
    /* Backlight relight deferred until after the first post-wake present has
     * retired. Lighting the LED over a panel that is still running its BCM init
     * is precisely what the user sees as a white screen (02-lcd.md:490), so when
     * the panel was slept we raise this instead of calling backlight_set() at
     * the input site. Inert while PANEL_SLEEP_AT_IDLE is 0 (panel_slept never
     * sets), but it is what makes that flag safe to flip. */
    int      bl_relight = 0;
    uint32_t np_last = 0xFFFFFFFFu;
    int      np_first = 1;
    int      np_vol_prev = 0;            /* volume overlay was up last NP paint  */
    uint32_t last_present = 0;           /* rate-limit UI presents while playing */
    uint32_t last_bars = 0;              /* rate-limit the now-playing bar anim  */
    uint32_t last_chip = 0;              /* rate-limit album-cover chip loads    */
    uint32_t last_uistat = 0;            /* 5 s cadence of the "core: ui" line   */
    uint32_t last_mq = 0;                /* rate-limit the marquee scroll        */
    int      hold_prev = clickwheel_hold() ? 1 : 0;  /* seed hold-edge detect    */
    int      ext_prev  = power_is_external() ? 1 : 0; /* seed plug-in edge detect */
    int      lock_flashing = 0;          /* a lock/unlock plate is on screen     */
    char     az_prev = 0;                /* A-Z locator letter on screen         */
    int      toast_prev = 0;             /* low-battery toast on screen          */
    int      play_held = 0;              /* PLAY currently down (long-press off)  */
    uint32_t play_down_us = 0;           /* when PLAY went down                   */
    g_locked = hold_prev;

    /* Backlight inactivity: full -> dim -> off. Any input wakes to full; a press
     * that wakes from fully-OFF is swallowed (it just lights the screen, the way
     * a real iPod's first touch does). Playback keeps running the whole time. */
    enum { BL_OFF, BL_DIM, BL_FULL };
    int      bl_state   = BL_FULL;
    int      cpu_idled  = 0;              /* core dropped to 30 MHz for deep idle   */
    int      panel_slept = 0;            /* LCD panel put to sleep at backlight-off */
    uint32_t last_input = mmio_read32(USEC_TIMER_ADDR);

    /* Seeded from the LIVE transport, not from zero: a successful resume_restore
     * has already left a track loaded, and a `was_active` of 0 would read the
     * first pass as a play->stop edge (closing the codec under a track we just
     * restored) and as a fresh capture of a position we only just loaded. */
    int was_active = player_active();     /* detect the active->idle edge          */
    int last_qidx  = was_active ? player_queue_current() : -1;
    g_resume_open_seq   = player_open_seq();
    g_resume_was_paused = player_paused();
    /* Seeded AFTER resume_restore: a restore that had to skip a broken track
     * bumps the end counter, and that must not read as "the queue finished". */
    g_resume_end_seq    = player_end_seq();
    g_resume_last_us    = mmio_read32(USEC_TIMER_ADDR);
    for (;;) {
        /* Time the pump: when it returns in a few microseconds the decode step
         * produced nothing (PCM ring full) and the disk buffer isn't filling —
         * i.e. this pass had no work. Used at the bottom of the loop to halt. */
        uint32_t pump_t0 = mmio_read32(USEC_TIMER_ADDR);
        player_pump();
        uint32_t pump_us = mmio_read32(USEC_TIMER_ADDR) - pump_t0;

        /* TRACK-CHANGE EDGE — unconditional, above every screen-specific branch.
         * Auto-advance moves the queue with no button event, and several screens
         * paint the playing track (the status strip's name on ALL of them, the
         * tracklist's animated bars, the queue view's bars, the main menu's "Now
         * Playing" row). The bar animators are partial painters that only ever
         * repaint the row they find playing, so without a full repaint here the
         * FINISHED track's row keeps a frozen 3-bar glyph forever (one more each
         * time a track ends) and the status strip keeps its name. Covers the
         * play->stop transition too. */
        int now_active = player_active();
        int now_qidx   = now_active ? player_queue_current() : -1;
        if (now_active != was_active || now_qidx != last_qidx) {
            dirty = 1;
        }
        last_qidx = now_qidx;

        /* RESUME POSITION — the edges worth a kilobyte of disk (see the block
         * comment above resume_capture): the track changed, pause flipped,
         * playback started/stopped, or RESUME_SAMPLE_US has gone by. The
         * timestamp is stamped HERE and not inside resume_capture, so a pass
         * where nothing had moved can't re-arm the interval every iteration. */
        {
            uint32_t oseq  = player_open_seq();
            int      paus  = player_paused();
            uint32_t nowu  = mmio_read32(USEC_TIMER_ADDR);
            if (now_active != was_active || oseq != g_resume_open_seq ||
                paus != g_resume_was_paused ||
                (uint32_t)(nowu - g_resume_last_us) >= RESUME_SAMPLE_US) {
                resume_capture();
                g_resume_open_seq   = oseq;
                g_resume_was_paused = paus;
                g_resume_last_us    = nowu;
            }
        }

        /* Queue finished (last track of the album/playlist/song list ended and
         * Repeat is off): no screen may keep showing a dead player. Truncate the
         * stack at the FIRST player view (Now Playing / Queue — the queue view is
         * just as dead), keeping everything the user navigated through below it;
         * an empty stack falls back to the main menu. Testing only the TOP screen
         * left a dead Now Playing behind a Queue view, and left every other screen
         * un-repainted. */
        if (was_active && !now_active) {
            /*
             * If the queue ended BY ITSELF, drop the saved resume position.
             * resume_capture() deliberately keeps the last locator when
             * nothing is loaded ("nothing loaded — keep the last one"), which
             * is right for a stop but wrong here: the listener heard the track
             * to the end, so restoring it on the next boot re-presents a
             * finished song cued at 0:00 (device, 2026-07-27).
             *
             * Gated on player_end_seq() rather than on this transition alone,
             * so a manual stop still keeps its position.
             */
            uint32_t eseq = player_end_seq();
            if (eseq != g_resume_end_seq) {
                g_resume_end_seq = eseq;
                if (g_settings.resume_hash != 0) {
                    g_settings.resume_hash  = 0;
                    g_settings.resume_secs  = 0;
                    g_settings.resume_total = 0;
                    settings_touch();
                }
            }
            /* Playback truly ended (stop / queue exhausted / skip past the ends —
             * NOT an inter-track advance, which never reads inactive here since
             * advance+open complete inside one player_pump). Power the codec down
             * and gate the audio clocks; the next track's hal_audio_init does a
             * full pop-suppressed bring-up, so resume is clean. */
            hal_audio_close();
            for (int i = 0; i < g_scr_n; i++) {
                if (g_scr[i] == SCR_NOWPLAYING || g_scr[i] == SCR_QUEUE) {
                    g_scr_n = i;
                    break;
                }
            }
            if (g_scr_n == 0) {
                g_scr[0]   = SCR_MENU;
                g_scr_n    = 1;
                g_main_sel = 0;
            }
            dirty = 1;
        }
        was_active = now_active;
        if (battery_refresh(0) && scr_cur() == SCR_CHARGING) {
            dirty = 1;                    /* refresh the % on the charging screen  */
        }

        /*
         * Pause when the headphones are pulled out.
         *
         * Deliberately one-directional: re-inserting does NOT resume. The
         * insertion switch closes before the audio contacts seat, so resuming
         * on that edge would start playing into a half-made connection, at
         * whatever the volume happened to be, while the user still has hold of
         * the plug. Every reference player waits for Play, and keeping it
         * one-way means there is no "was this pause the jack's or the user's?"
         * flag to get wrong later.
         *
         * hal_headphones_present() returns -1 until the detect line's polarity
         * is confirmed on the device (hal/hw/headphone.h), and the >= 0 guard
         * is what makes that inert rather than a pause storm. Sitting outside
         * the input-swallowing branch is also deliberate: a yank in the pocket
         * is the canonical case, and Hold must not swallow it.
         */
        int hp = hal_headphones_present();
        if (hp == 0 && g_hp_last == 1 && player_active() && !player_paused()) {
            player_pause();
        }
        if (hp >= 0) {
            g_hp_last = hp;
        }

        /* Charging screen: pop up on a plug-IN edge (not if already powered at
         * boot, so it won't spuriously appear), auto-dismiss when unplugged. Any
         * button also dismisses it (handled in the input switch below). */
        int ext = power_is_external() ? 1 : 0;
        if (ext && !ext_prev && scr_cur() != SCR_CHARGING) {
            scr_push_modal(SCR_CHARGING);
            dirty = 1;
        } else if (!ext && scr_cur() == SCR_CHARGING) {
            scr_pop();
            dirty = 1;
        }
        ext_prev = ext;

        /* The low-battery modal. Raised once per OK->DISKSAFE crossing and
         * held until a button dismisses it; the charging screen outranks it,
         * because a plugged-in device has already answered the warning. This
         * is idempotent per pass, so it self-heals if anything else truncates
         * the screen stack underneath it. */
        if (battwarn_screen() == BATTWARN_DISKSAFE &&
            scr_cur() != SCR_BATTERY && scr_cur() != SCR_CHARGING) {
            scr_push_modal(SCR_BATTERY);
            dirty = 1;
        } else if (scr_cur() == SCR_BATTERY &&
                   battwarn_screen() != BATTWARN_DISKSAFE) {
            scr_pop();
            dirty = 1;
        }

        /* Long-press PLAY (~2s) sleeps the device (suspend: drive spun down,
         * screen dark, but CPU+RAM alive so wake is INSTANT and skips ipl2).
         * Holding on to ~5s escalates to a true PMU power-down. LIVE button
         * state tracks a continuous hold; a short PLAY tap is play/pause.
         * Locked out while the hold switch is on. */
        if (!g_locked && (clickwheel_buttons() & WHEEL_BTN_PLAY)) {
            uint32_t nowp = mmio_read32(USEC_TIMER_ADDR);
            if (!play_held) {
                play_held = 1;
                play_down_us = nowp;
            } else if ((uint32_t)(nowp - play_down_us) > 2000000u) {
                suspend_to_ram(play_down_us);   /* returns on wake */
                play_held  = 0;
                last_input = mmio_read32(USEC_TIMER_ADDR);
                bl_state   = BL_FULL;           /* backlight restored on resume */
                dirty      = 1;                 /* repaint the current screen */
            }
        } else {
            play_held = 0;
        }

        /* Hold-switch edge (a cheap GPIO read, independent of the wheel block
         * which is gated off while held): flash the lock/unlock plate and toggle
         * the input lock. Playback is untouched. */
        int held = clickwheel_hold() ? 1 : 0;
        if (held != hold_prev) {
            hold_prev = held;
            g_locked  = held;
            /* Force the plate to REPAINT for the new state. Without this, a second
             * edge (e.g. on->off within the 1 s window) leaves lock_flashing set
             * from the first edge, so the render guard (!lock_flashing) suppresses
             * the new plate and the unlock modal never shows. */
            lock_flashing = 0;
            ui_window_arm(&g_lock_flash);
            last_input = mmio_read32(USEC_TIMER_ADDR);   /* wake the backlight    */
            if (bl_state != BL_FULL) {
                if (panel_slept) bl_relight = 1;         /* after the present     */
                else             backlight_set(g_settings.backlight_bright);
                bl_state = BL_FULL;
                wheel_accel_reset();      /* don't resume a pre-sleep gesture */
            }
            dirty = 1;
        }

        /* Input is sampled on the 100 Hz tick and latched (clickwheel_service),
         * so a tap that lands while this loop is blocked in a disk read isn't
         * lost — we just drain the latch here. */
        wheel_event_t ev;
        if (!g_locked && clickwheel_get_event(&ev)) {
            last_input = mmio_read32(USEC_TIMER_ADDR);
            if (bl_state != BL_FULL) {
                int was_off = (bl_state == BL_OFF);
                if (panel_slept) bl_relight = 1;         /* after the present     */
                else             backlight_set(g_settings.backlight_bright);
                bl_state = BL_FULL;
                wheel_accel_reset();      /* don't resume a pre-sleep gesture */
                dirty = 1;                    /* repaint anything drawn while off */
                if (was_off) {                /* swallow the wake press */
                    ev.buttons = 0;
                    ev.wheel_delta = 0;
                }
            }
            /* Menu click on a button press (one per down-edge; not on the
             * swallowed backlight-wake press above). Wheel clicks are emitted by
             * wheel_move itself, only when the cursor actually advances a row —
             * so a sub-threshold tick that doesn't move can't click. */
            if (ev.buttons) {
                ui_click();
            }
            /* Charging screen: ANY press just dismisses it, and the press is
             * CONSUMED here — above the global transport block, which otherwise
             * saw the same event and paused/skipped the music you were only
             * trying to get the modal off the screen for. Same swallow pattern as
             * the backlight wake above. */
            /* Any input dismisses a live toast (not consumed — the press was
             * meant for whatever is underneath) and marks the modal seen. */
            battwarn_input(mmio_read32(USEC_TIMER_ADDR));
            if ((scr_cur() == SCR_CHARGING || scr_cur() == SCR_BATTERY) &&
                ev.buttons) {
                scr_pop();
                dirty = 1;
                ev.buttons     = 0;
                ev.wheel_delta = 0;
            }
            /* Transport buttons are global (work from any screen while playing),
             * like a real iPod: PLAY toggles pause, RIGHT/LEFT skip track. */
            if ((ev.buttons & WHEEL_BTN_PLAY) && player_active()) {
                player_toggle_pause();
                dirty = 1;
            }
            if ((ev.buttons & WHEEL_BTN_RIGHT) && player_active()) {
                player_next();
                hal_volume_set(g_volume);         /* re-apply over codec re-init */
                dirty = 1;
            }
            if ((ev.buttons & WHEEL_BTN_LEFT) && player_active()) {
                player_prev();
                hal_volume_set(g_volume);
                dirty = 1;
            }
            switch (scr_cur()) {
            case SCR_MENU:
                if (ev.wheel_delta) {
                    g_main_sel = wheel_move(g_main_sel, main_menu_count(),
                                            ev.wheel_delta, &g_menu_accum);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    if (g_main_sel == MM_MUSIC) {
                        g_music_sel  = MU_ALBUMS;
                        g_menu_accum = 0;
                        scr_push(SCR_MUSIC);
                    } else if (g_main_sel == MM_NOWPLAYING && player_active()) {
                        scr_push(SCR_NOWPLAYING);
                        np_first = 1;
                    } else if (g_main_sel == MM_SETTINGS) {
                        g_set_screen = SETTINGS_ROOT;
                        g_set_sel = g_set_root_sel = g_set_accum = 0;
                        g_set_editing = 0;
                        scr_push(SCR_SETTINGS);
                    }
                    /* other items are greyed: SELECT does nothing yet */
                    dirty = 1;
                }
                break;

            case SCR_MUSIC:
                if (ev.wheel_delta) {
                    g_music_sel = wheel_move(g_music_sel, MU_COUNT,
                                             ev.wheel_delta, &g_menu_accum);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    if (g_music_sel == MU_ALBUMS) {
                        library_ensure(fs);            /* index -> g_albums      */
                        g_dir_depth = 0;
                        g_artist_filter[0] = '\0';     /* all albums             */
                        albumview_build(0);
                        albumlist_queue_chips();       /* start loading covers   */
                        g_br_sel = g_br_accum = 0;
                        scr_push(SCR_BROWSER);
                    } else if (g_music_sel == MU_ARTISTS) {
                        library_ensure(fs);            /* index -> g_albums      */
                        g_artist_filter[0] = '\0';
                        build_artists();
                        g_artist_sel = g_artist_accum = 0;
                        scr_push(SCR_ARTISTS);
                    } else if (g_music_sel == MU_SONGS) {
                        library_ensure(fs);
                        songview_build(-1, 0);         /* all songs, all artists */
                        scr_push(SCR_SONGS);
                    } else if (g_music_sel == MU_SHUFFLE) {
                        shuffle_songs_play(fs);        /* random albums, shuffled */
                        if (player_active()) {
                            scr_push(SCR_NOWPLAYING);
                            np_first = 1;
                        }
                    } else if (g_music_sel == MU_GENRES) {
                        library_ensure(fs);
                        g_genre_sel = g_genre_accum = 0;
                        scr_push(SCR_GENRES);
                    }
                    /* other items are greyed: SELECT does nothing yet */
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to main menu */
                    g_menu_accum = 0;
                    dirty = 1;
                }
                break;

            case SCR_ARTISTS:
                if (ev.wheel_delta && g_artists_n > 0) {
                    g_artist_sel = wheel_move(g_artist_sel, g_artists_n,
                                              ev.wheel_delta, &g_artist_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_artists_n > 0) {
                    /* Filter the album list to the chosen artist's folders. */
                    int k = 0;
                    for (; g_artists[g_artist_sel].name[k] && k < NAME_MAX; k++) {
                        g_artist_filter[k] = g_artists[g_artist_sel].name[k];
                    }
                    g_artist_filter[k] = '\0';
                    g_dir_depth = 0;
                    albumview_build(g_artist_filter);
                    albumlist_queue_chips();
                    g_br_sel = g_br_accum = 0;
                    scr_push(SCR_BROWSER);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Music menu */
                    dirty = 1;
                }
                break;

            case SCR_SONGS:
                if (ev.wheel_delta && g_songview_n > 0) {
                    g_song_sel = wheel_move(g_song_sel, g_songview_n,
                                            ev.wheel_delta, &g_song_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_songview_n > 0) {
                    library_play_song(fs, g_song_sel);
                    scr_push(SCR_NOWPLAYING);
                    np_first = 1;
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back (Music or Genres) */
                    dirty = 1;
                }
                break;

            case SCR_GENRES:
                if (ev.wheel_delta && g_genres_n > 0) {
                    g_genre_sel = wheel_move(g_genre_sel, g_genres_n,
                                             ev.wheel_delta, &g_genre_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_genres_n > 0) {
                    songview_build(g_genre_sel, 0);     /* this genre's songs */
                    scr_push(SCR_SONGS);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Music menu */
                    dirty = 1;
                }
                break;

            case SCR_BROWSER: {
                /* Depth 0 scrolls the album list (g_br_sel); depth 1 the loaded
                 * tracklist (g_det_sel) — separate so backing out restores the
                 * album-list position. */
                int count = (g_dir_depth == 0) ? albumlist_count() : g_browse_n;
                int *sel  = (g_dir_depth == 0) ? &g_br_sel   : &g_det_sel;
                int *acc  = (g_dir_depth == 0) ? &g_br_accum : &g_det_accum;
                if (ev.wheel_delta && count > 0) {
                    *sel = wheel_move(*sel, count, ev.wheel_delta, acc);
                    dirty = 1;                    /* window derived at paint time  */
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && count > 0) {
                    if (g_dir_depth == 0 && albumlist_album_at(g_br_sel) < 0) {
                        /* "All Songs" for the artist we are filtered to: the
                         * whole discography in title order, so a track you
                         * remember but cannot place to an album is reachable. */
                        songview_build(-1, g_artist_filter);
                        scr_push(SCR_SONGS);
                    } else if (g_dir_depth == 0) {
                        /* Enter the selected album: load its tracklist + art. Keep
                         * g_br_sel (the album) so backing out lands back on it. */
                        lib_album_t *al = &g_albums[albumlist_album_at(g_br_sel)];
                        split_artist_album(al->folder,
                                           g_album_artist, g_album_title);
                        g_dir_depth = 1;
                        browse_load(fs, al->clus);
                        detail_load_meta(fs);
                        g_det_sel = g_det_accum = 0;
                    } else {
                        player_play_queue(g_browse, g_browse_n, g_det_sel,
                                          g_art_clus, g_art_size);
                        hal_volume_set(g_volume);  /* re-apply over codec re-init */
                        scr_push(SCR_NOWPLAYING);
                        np_first = 1;
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_dir_depth > 0) {              /* tracklist -> album list */
                        g_dir_depth = 0;
                        albumlist_queue_chips();        /* g_br_sel kept (the album) */
                    } else {                            /* album list -> Music menu */
                        scr_pop();
                    }
                    dirty = 1;
                }
                break;
            }

            case SCR_NOWPLAYING:
                /* Scrubbing: the wheel aims at a position instead of setting
                 * volume. Accelerates with the spin so a 70-minute track is
                 * crossable, but one deliberate detent is always SCRUB_STEP_S. */
                if (ev.wheel_delta && np_scrubbing() && player_active()) {
                    int vel   = wheel_accel_step(ev.wheel_delta);
                    int units = ev.wheel_delta / CW_WHEEL_SENSITIVITY;
                    if (units == 0) {
                        units = (ev.wheel_delta > 0) ? 1 : -1;
                    }
                    int32_t  step  = (int32_t)units * SCRUB_STEP_S * vel;
                    int32_t  t     = (int32_t)g_scrub_target_s + step;
                    uint32_t total = player_total_s();
                    if (t < 0) t = 0;
                    if (total > 0 && t > (int32_t)total) t = (int32_t)total;
                    g_scrub_target_s = (uint32_t)t;
                    g_scrub_last_us  = mmio_read32(USEC_TIMER_ADDR);
                    g_scrub_dirty    = 1;
                    np_last          = 0xFFFFFFFFu;   /* force a transport repaint */
                    dirty            = 1;
                } else if (ev.wheel_delta) {             /* wheel = volume        */
                    /* The wheel reports raw position units (~CW_WHEEL_SENSITIVITY
                     * per detent), which is why volume used to leap ~10 at a time.
                     * Quantise to whole detents: a single slow detent nudges the
                     * volume by exactly ±1, and spinning faster (more detents per
                     * event) accelerates so you can still sweep the whole range. */
                    int units = ev.wheel_delta / CW_WHEEL_SENSITIVITY;
                    if (units == 0) {                    /* sub-detent motion: ±1  */
                        units = (ev.wheel_delta > 0) ? 1 : -1;
                    }
                    int sign = (units < 0) ? -1 : 1;
                    int mag  = (units < 0) ? -units : units;
                    int step = (mag <= 1) ? sign : sign * mag * 2;  /* 1->1, 2->4, 3->6 */
                    if (step >  12) step =  12;
                    if (step < -12) step = -12;
                    int prev_vol = g_volume;
                    g_volume += step;
                    if (g_volume < 0)   g_volume = 0;
                    if (g_volume > 100) g_volume = 100;
                    (void)prev_vol;   /* no click on volume — it's a slider, not nav */
                    hal_volume_set(g_volume);
                    g_settings.volume = g_volume;         /* keep Settings in sync */
                    settings_touch();     /* debounced: one write per volume sweep */
                    ui_window_arm(&g_vol_show);
                    dirty = 1;
                }
                /* SELECT is now press-length sensitive here: a tap toggles the
                 * scrubber (what SELECT does on Now Playing on a real iPod, and
                 * far and away the more frequent action), a hold opens the queue
                 * view. Only the down-edge is recorded — the decision is made in
                 * the per-pass block below, once the press length is known. */
                if ((ev.buttons & WHEEL_BTN_SELECT) && player_active()) {
                    g_sel_down_us = mmio_read32(USEC_TIMER_ADDR);
                    g_sel_pending = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back, keep playing */
                    dirty = 1;
                }
                break;

            case SCR_QUEUE:
                if (ev.wheel_delta && player_queue_len() > 0) {
                    g_queue_sel = wheel_move(g_queue_sel, player_queue_len(),
                                             ev.wheel_delta, &g_queue_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && player_queue_len() > 0) {
                    if (!player_queue_is_dir(g_queue_sel)) {
                        player_jump(g_queue_sel);          /* play the chosen track */
                        hal_volume_set(g_volume);          /* re-apply over re-init */
                        scr_pop();                          /* back to Now Playing  */
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();
                    dirty = 1;
                }
                break;

            case SCR_SETTINGS: {
                int scount = settings_count(g_set_screen);
                int slider = (settings_kind(g_set_screen, g_set_sel)
                              == SETTINGS_KIND_SLIDER);
                /* NB `slider` is re-read before the SELECT test below: one latched
                 * event can carry BOTH a wheel move and a button, and the move
                 * changes which row SELECT lands on. */
                if (ev.wheel_delta) {
                    if (g_set_editing && slider) {         /* adjust the value  */
                        int dd = ev.wheel_delta;
                        if (dd >  4) dd =  4;
                        if (dd < -4) dd = -4;
                        settings_adjust(g_set_screen, &g_settings, g_set_sel, dd);
                        settings_apply();                  /* live volume/etc.  */
                        settings_touch();                  /* debounced save    */
                        /* Brightness slider: light up to the new level as you
                         * turn, so the wheel drives the panel in real time. */
                        if (g_set_screen == SETTINGS_DISPLAY && g_set_sel == 1) {
                            backlight_set(g_settings.backlight_bright);
                            bl_state = BL_FULL;
                        }
                    } else if (scount > 0) {               /* move selection    */
                        g_set_sel = wheel_move(g_set_sel, scount,
                                               ev.wheel_delta, &g_set_accum);
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    slider = (settings_kind(g_set_screen, g_set_sel)
                              == SETTINGS_KIND_SLIDER);    /* the CURRENT row   */
                    if (slider) {
                        g_set_editing = !g_set_editing;    /* enter/exit edit   */
                    } else {
                        int act = settings_activate(g_set_screen, &g_settings,
                                                    g_set_sel);
                        int target = -1;
                        switch (act) {
                        case SETTINGS_ENTER_PLAYBACK: target = SETTINGS_PLAYBACK; break;
                        case SETTINGS_ENTER_SOUND:    target = SETTINGS_SOUND;    break;
                        case SETTINGS_ENTER_DISPLAY:  target = SETTINGS_DISPLAY;  break;
                        case SETTINGS_ENTER_ABOUT:    target = SETTINGS_ABOUT;    break;
                        case SETTINGS_ENTER_THEME:    target = SETTINGS_THEME;    break;
                        case SETTINGS_ENTER_CLICKER:  target = SETTINGS_CLICKER;  break;
                        case SETTINGS_ENTER_DIAG:     target = SETTINGS_DIAG;     break;
                        default: break;
                        }
                        if (target >= 0) {                 /* descend a screen  */
                            g_set_root_sel = g_set_sel;
                            g_set_screen = target;
                            g_set_sel = g_set_accum = 0;
                            g_set_editing = 0;
                        } else if (act == SETTINGS_ACTION_DISKMODE) {
                            /* Reboot into the ROM's USB mass-storage mode. We
                             * implement no USB ourselves, so this is the only
                             * way onto a host from inside our own firmware —
                             * the alternative is catching the ROM's Select+Play
                             * window at boot, which is timing-sensitive and easy
                             * to miss. Stop playback and blank the panel first
                             * so we never reset mid-DMA or leave a half-drawn
                             * screen; power_enter_disk_mode never returns. */
                            player_stop();
                            hal_audio_close();
                            console_clear(LINEN_SURFACE);
                            ui_text_centered(LCD_HEIGHT / 2 - 8, "Disk Mode",
                                             FONT_TITLE, LINEN_INK);
                            lcd_present_fb(console_framebuffer());
                            power_enter_disk_mode();
                        } else if (act == SETTINGS_ACTION_RESET) {
                            settings_defaults(&g_settings);
                            settings_apply();
                            settings_touch();              /* persist the reset */
                        } else if (act == SETTINGS_ACTION_NONE) {
                            /* Only a row that actually CHANGED the record gets
                             * persisted. SELECT on About/Diagnostics, or on the
                             * theme already selected, used to mark the config
                             * dirty and spin the drive up three seconds later to
                             * write a byte-identical record. */
                            settings_apply();
                            settings_touch();
                        }
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_set_editing) {
                        g_set_editing = 0;                 /* exit edit, stay   */
                    } else if (g_set_screen != SETTINGS_ROOT) {
                        g_set_screen = SETTINGS_ROOT;      /* back to root      */
                        g_set_sel = g_set_root_sel;
                        g_set_accum = 0;
                    } else {
                        scr_pop();                          /* leave Settings    */
                        /* Resume may have just been switched OFF, and dropping
                         * the stored locator is part of honouring that. Doing
                         * it here folds the clear into the commit below instead
                         * of costing a second write when the interval next
                         * comes round. */
                        resume_capture();
                        /* The natural "I'm done" moment: commit now rather than
                         * waiting out the debounce, so the record is on the
                         * platter before the user can reach for the Hold switch. */
                        settings_commit(1);
                    }
                    dirty = 1;
                }
                break;
            }

            case SCR_BATTERY:
            case SCR_CHARGING:
                break;                        /* dismissal is handled above */
            }
        }

        /* Idle-timeout backlight: dim, then off — timeout from Settings (0 =
         * never), brightness from Settings. Playback keeps running. */
        uint32_t dim_us, off_us;
        if (g_settings.backlight_secs <= 0) {
            dim_us = off_us = 0xFFFFFFFFu;      /* never dim/off               */
        } else {
            dim_us = (uint32_t)g_settings.backlight_secs * 1000000u;
            off_us = dim_us + 15u * 1000000u;
        }
        int dim_level = g_settings.backlight_bright / 4;
        if (dim_level < 1) dim_level = 1;
        uint32_t idle = mmio_read32(USEC_TIMER_ADDR) - last_input;
        if (bl_state == BL_FULL && idle > dim_us) {
            backlight_set(dim_level);
            bl_state = BL_DIM;
        } else if (bl_state == BL_DIM && idle > off_us) {
            backlight_set(0);
            bl_state = BL_OFF;
            if (PANEL_SLEEP_AT_IDLE) {    /* see the note above run_ui() */
                lcd_sleep();              /* blank the panel too, not just the LED */
                panel_slept = 1;
            }
        }

        /* Idle CPU-clock scaling: once the screen has timed fully off AND nothing
         * is playing, drop the core 80->30 MHz — only the disk/decode path needs
         * 80. Restore the instant there's life again: any wake back to BL_FULL, or
         * playback starting. Rides the same refcounted boost the boot path took, so
         * it stays balanced (idle unboost 1->0, wake boost 0->1) and re-boosts
         * before the render/decode work later in this same iteration.
         *
         * "Playing" means the DAC is running — player_playing(), NOT
         * player_active(). Active stays set across a pause (the UI needs it to,
         * every transport control is gated on it), so keying this on active
         * meant a PAUSED device with the screen off held 80 MHz indefinitely:
         * the single largest idle-power miss in the firmware, since pause-and-
         * pocket is the common way to stop listening. A paused pump does no
         * decode and feeds no DMA, so 30 MHz is plenty; the resume itself
         * always arrives through a wake press that has already re-lit the
         * screen (and so re-boosted here) one pass earlier. */
        if (cpu_idled && (bl_state != BL_OFF || player_playing())) {
            cpu_boost();
            cpu_idled = 0;
        } else if (!cpu_idled && bl_state == BL_OFF && !player_playing()) {
            cpu_unboost();
            cpu_idled = 1;
        }

        /* Disk spin-down at idle: once nothing needs the drive AND the user has
         * been idle a while, park the platters — the single largest continuous
         * draw on a spinning-HDD unit. "Nothing needs it" = not actively playing;
         * PAUSED counts as idle (resume plays from the ~73 s RAM anti-skip buffer
         * and the drive spins up lazily on the next fill, so no gap). The next
         * real read self-recovers — ata_wait_drq tolerates the multi-second
         * spin-up — so no explicit wake is wired here. Skipped while actively
         * playing: player.c already parks the drive between bursts and owns that
         * cadence. ata_is_parked() is the shared truth, so we never re-issue
         * STANDBY on an already-parked drive. On an iFlash/SSD mod this is a
         * harmless no-op. */
        /* Debounced settings write. Deliberately placed BEFORE the spin-down
         * check below: if a save is due we want it to land while the platters
         * are still turning, rather than parking the drive and immediately
         * spinning it back up for a 1 KB write. Nothing happens here unless a
         * setting actually changed and the user has since gone quiet. */
        settings_commit(0);

        const uint32_t disk_idle_us = 20000000u;   /* 20 s: saves ~100 mA, no thrash */
        if (!player_playing() && !ata_is_parked() && idle > disk_idle_us) {
            ata_standby();
        }

        /*
         * SELECT press-length arbitration (Now Playing). Decided here rather
         * than at the down-edge because the length is only known once the button
         * is released or the hold threshold passes. Held past SEL_HOLD_US opens
         * the queue immediately (so the gesture confirms itself under your
         * thumb); released before it toggles the scrubber.
         */
        if (g_sel_pending && (scr_cur() != SCR_NOWPLAYING || g_locked)) {
            /* The press no longer belongs to Now Playing: MENU popped it, the
             * track ended and truncated the stack, or Hold engaged mid-press.
             * Hold is the nasty one — it zeroes clickwheel_buttons(), which
             * reads below as a release and would scrub_enter() while locked;
             * MENU-then-release used to toggle the scrubber on whatever
             * screen was current, and the next Now Playing wheel turn then
             * seeked instead of changing the volume. Drop it, and the
             * scrubber with it. */
            g_sel_pending = 0;
            if (np_scrubbing()) {
                scrub_exit();
                np_last = 0xFFFFFFFFu;
            }
        }
        if (g_sel_pending) {
            uint32_t sel_held = mmio_read32(USEC_TIMER_ADDR) - g_sel_down_us;
            int      down     = (clickwheel_buttons() & WHEEL_BTN_SELECT) != 0;
            if (down && sel_held >= SEL_HOLD_US) {
                g_sel_pending = 0;
                scrub_exit();
                g_queue_sel   = player_queue_current();
                g_queue_accum = 0;
                scr_push(SCR_QUEUE);
                dirty = 1;
            } else if (!down) {
                g_sel_pending = 0;
                if (np_scrubbing()) scrub_exit();
                else                scrub_enter();
                np_last = 0xFFFFFFFFu;          /* repaint the transport band */
                dirty   = 1;
            }
        }

        /*
         * Deferred seek. The target is committed once the wheel has been quiet
         * for SCRUB_COMMIT_US, so dragging across a track costs ONE seek rather
         * than one per detent — a seek stops the DAC, re-primes the ring, and on
         * a backward jump rewinds the file. Scrub mode then lapses on its own,
         * so the wheel cannot be left silently controlling position.
         */
        if (np_scrubbing()) {
            uint32_t quiet = mmio_read32(USEC_TIMER_ADDR) - g_scrub_last_us;
            if (g_scrub_dirty && quiet >= SCRUB_COMMIT_US) {
                g_scrub_dirty = 0;
                if (player_seek_to(g_scrub_target_s) != 0) {
                    g_scrub_target_s = player_elapsed_s();   /* refused: snap back */
                }
                np_last = 0xFFFFFFFFu;
                dirty   = 1;
            } else if (!g_scrub_dirty && quiet >= SCRUB_EXIT_US) {
                scrub_exit();
                np_last = 0xFFFFFFFFu;
                dirty   = 1;
            }
            if (!player_active()) {
                scrub_exit();                    /* track ended under the scrubber */
            }
        }

        /* Panel wake: the instant we leave the fully-off state, re-enable the LCD
         * panel BEFORE any present happens this iteration (the lock-plate flash or
         * the render block below). One central check covers every wake path, so no
         * individual input site needs patching. lcd_wake() only restores the
         * panel-enable bits — the following present re-lights and repaints. */
        if (panel_slept && bl_state != BL_OFF) {
            lcd_wake();
            panel_slept = 0;
            dirty = 1;                    /* never resume onto a stale panel */
        }

        /* Lock/unlock plate takes over the screen for ~1s on a Hold edge. Paint
         * the context + plate once, hold it, then repaint underneath when it
         * fades. Suppresses the normal render while up. */
        uint32_t now_us = mmio_read32(USEC_TIMER_ADDR);
        if (ui_window_up(&g_lock_flash, LOCK_FLASH_US, now_us)) {
            if (!lock_flashing && bl_state != BL_OFF) {
                paint_current_screen();
                lock_plate_render(g_locked);
                lcd_present_fb(console_framebuffer());
                dirty = 0;
            }
            lock_flashing = 1;
            /* Only throttle when idle; keep audio paced while playing. Halt the
             * core (self-waking ~10 ms, one tick) instead of a busy-spin. Same
             * gate as the main halt below: a PAUSED player has no DMA to pace. */
            if (!player_playing()) {
                cpu_wait_ms(10);
            }
            continue;                     /* skip the normal render this pass      */
        }
        if (lock_flashing) {              /* plate just faded: repaint underneath  */
            lock_flashing = 0;
            dirty = 1;
        }

        /* A-Z locator: while the wheel is being spun fast, show the selected
         * row's initial so you can aim at a letter instead of reading rows that
         * are flying past. Its appearance/disappearance is itself a repaint. */
        char az_letter = wheel_accelerating() ? list_sel_initial() : 0;
        /* Suppressed with the backlight: a toast nobody can see is only a
         * repaint. It reappears on the next sample if the cell is still low. */
        int  toast     = (bl_state != BL_OFF) &&
                         battwarn_toast_up(mmio_read32(USEC_TIMER_ADDR));

        /* Render the current screen (skipped entirely when the screen is off —
         * saves the IRQ-masked present while music plays dark). Now Playing
         * repaints ~1/s for the clock; menus/browser only on change. */
        if (bl_state == BL_OFF) {
            /* nothing to draw */
        } else if (scr_cur() == SCR_NOWPLAYING) {
            uint32_t nowv    = mmio_read32(USEC_TIMER_ADDR);
            uint32_t elapsed = player_elapsed_s();
            int vol_active = ui_window_up(&g_vol_show, VOL_SHOW_US, nowv);
            int expiring   = np_vol_prev && !vol_active;   /* overlay just faded */

            /* A FULL repaint is needed only on a real change (dirty — which the
             * loop's track-change edge sets for us) or to erase the fading volume
             * overlay, which straddles the static art band. Everything else is
             * just the clock ticking: that redraws ONLY the transport strip
             * (elapsed/remaining/progress) and presents only its band, instead of
             * re-rendering the whole screen — art, metadata and two anti-aliased
             * bars — once a second and throwing all of it above y=128 away. */
            int want_full = dirty || expiring || toast != toast_prev;
            if (want_full) {
                if (np_first || !player_active() ||
                    (uint32_t)(nowv - last_present) >= present_gap_us()) {
                    g_mq.active = 0;
                    console_damage_reset();
                    nowplaying_render(player_track_name(), elapsed,
                                      player_total_s(), player_buf_pct());
                    if (toast) {
                        screen_battery_toast_render();
                        battwarn_toast_shown(nowv);   /* its 4 s start here */
                        g_mq.active = 0;      /* see the list branch below */
                    }
                    toast_prev = toast;
                    ui_present_damage();
                    np_first = 0;
                    np_last  = elapsed;
                    np_vol_prev  = vol_active;
                    last_present = nowv;
                    player_note_presented();
                    dirty = 0;
                    g_lp.valid = 0;             /* not a list screen */
                }
                /* else: throttled — keep dirty set, present in the next window */
            } else if (elapsed != np_last) {
                /* Transport only: leave the marquee registered (its own tick
                 * keeps the title scrolling) and don't touch the art/metadata. */
                console_damage_reset();
                nowplaying_transport_render(elapsed, player_total_s());
                ui_present_damage();
                np_last = elapsed;
                np_vol_prev = vol_active;
                player_note_presented();
            }
        } else if (dirty || az_letter != az_prev || toast != toast_prev) {
            /* Repaints are paced by what the LAST present actually cost (see
             * present_gap_us), not by a fixed 150 ms: a selection move now
             * repaints two rows and pushes only those, so it can run several
             * times faster without keeping IRQs masked long enough to starve the
             * audio DMA ISR. Idle → present immediately. `dirty` stays set until
             * we present, so the latest scroll position is what lands. */
            uint32_t now = mmio_read32(USEC_TIMER_ADDR);
            if (!player_active() ||
                (uint32_t)(now - last_present) >= present_gap_us()) {
                g_mq.active = 0;
                console_damage_reset();
                /* Selection moved and nothing else did? Repaint the two rows
                 * (plus the header count + scrollbar) instead of clearing and
                 * redrawing the panel. Anything else falls through to the full
                 * render, which console_clear marks as whole-screen damage. The
                 * A-Z plate covers arbitrary rows, so while it is up (or in the
                 * frame that clears it) the full render is the honest option. */
                int partial = !az_letter && !az_prev && !toast && !toast_prev &&
                              list_repaint_partial();
                if (!partial) {
                    switch (scr_cur()) {
                    case SCR_MENU:    main_menu_render();            break;
                    case SCR_MUSIC:   music_menu_render();           break;
                    case SCR_ARTISTS: artists_render(g_artist_sel);  break;
                    case SCR_SONGS:   songs_render(g_song_sel);      break;
                    case SCR_GENRES:  genres_render(g_genre_sel);    break;
                    case SCR_BROWSER: browse_render(g_dir_depth ? g_det_sel : g_br_sel);   break;
                    case SCR_QUEUE:   queue_render(g_queue_sel);     break;
                    case SCR_SETTINGS: settings_render_cur();        break;
                    case SCR_BATTERY:
                        screen_battery_render(BATTWARN_DISKSAFE);
                        break;
                    case SCR_CHARGING:
                        screen_charging_render(g_bat_pct, power_is_charging(),
                                               power_is_external());
                        break;
                    default: break;                 /* NOWPLAYING handled above */
                    }
                }
                if (az_letter) az_overlay_render(az_letter);
                if (toast) {
                    screen_battery_toast_render();
                    battwarn_toast_shown(now);        /* its 4 s start here */
                }
                /* The plate owns the screen. The repaint above re-registered
                 * the selected row with the marquee, whose tick would then
                 * punch a hole straight through whatever is covering it. */
                if (az_letter || toast) g_mq.active = 0;
                /* Render and present timed apart: `now` was read before the
                 * paint, so the delta here is the CPU the renderer took. */
                g_ui_render_us = mmio_read32(USEC_TIMER_ADDR) - now;
                if (g_ui_render_us > g_ui_render_max_us) g_ui_render_max_us = g_ui_render_us;
                ui_present_damage();
                if (g_present_cost_us > g_ui_present_max_us) g_ui_present_max_us = g_present_cost_us;
                if (partial) g_ui_partial++; else g_ui_full++;
                list_paint_note();
                az_prev = az_letter;
                toast_prev = toast;
                dirty = 0;
                last_present = now;
            }
        }

        /* Deferred post-wake relight: the panel has now had a real frame pushed
         * to it (and the hardened commit path blocked for the BCM's panel init),
         * so it is safe to put light behind it. See bl_relight's declaration. */
        if (bl_relight && !dirty) {
            backlight_set(g_settings.backlight_bright);
            bl_relight = 0;
        }

        /* Animate the now-playing 3-bar indicator in the album detail: redraw
         * only its ~10px box and partial-present just that, ~9fps. A tiny pixel
         * push (unlike a full repaint) so it can't starve the audio DMA. */
        if (bl_state != BL_OFF && scr_cur() == SCR_BROWSER && g_dir_depth > 0
            && player_active()) {
            uint32_t nowb = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowb - last_bars) >= 110000u) {
                int sv = 0;
                for (int i = 0; i < g_det_view_n; i++)
                    if (g_det_view[i] == (int16_t)g_det_sel) { sv = i; break; }
                int top = ui_scroll_window(sv, g_det_view_n, DET_ROWS);
                const char *pn = player_track_name();
                for (int r = 0; r < DET_ROWS; r++) {
                    int vi = top + r;
                    if (vi >= g_det_view_n) break;
                    int16_t v = g_det_view[vi];
                    if (v < 0) continue;
                    const browse_entry_t *e = &g_browse[v];
                    if (e->is_dir || !name_eq_ci(e->name, pn)) continue;
                    int ry = DET_LIST_Y0 + r * ROW_H;
                    int is_sel = (v == g_det_sel);
                    console_fill_rect(15, ry + 6, NP_BARS_W, NP_BARS_H,
                                      is_sel ? LINEN_SEL_BG : LINEN_SURFACE);
                    nowplaying_bars(15, ry + 6, is_sel ? LINEN_SEL_FG : LINEN_INK, nowb);
                    lcd_present_rect(console_framebuffer(),
                                     15, ry + 6, NP_BARS_W + 1, NP_BARS_H);
                    break;
                }
                last_bars = nowb;
            }
        }

        /* Same animated bars on the queue view (the currently-playing row). */
        if (bl_state != BL_OFF && scr_cur() == SCR_QUEUE && player_active()) {
            uint32_t nowb = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowb - last_bars) >= 110000u) {
                int n = player_queue_len();
                int cur = player_queue_current();
                int top = ui_scroll_window(g_queue_sel, n, LIST_ROWS);
                int r = cur - top;
                if (cur < n && r >= 0 && r < LIST_ROWS) {
                    int ry = LIST_Y0 + r * ROW_H;
                    int bx = LCD_WIDTH - 22, by = ry + 7;
                    int is_sel = (cur == g_queue_sel);
                    console_fill_rect(bx, by, NP_BARS_W, NP_BARS_H,
                                      is_sel ? LINEN_SEL_BG : LINEN_SURFACE);
                    nowplaying_bars(bx, by, is_sel ? LINEN_SEL_FG : LINEN_INK, nowb);
                    lcd_present_rect(console_framebuffer(),
                                     bx, by, NP_BARS_W + 1, NP_BARS_H);
                }
                last_bars = nowb;
            }
        }

        /* Scroll the marquee target (a selected long row, or the now-playing
         * title) in place — redraw just its band + partial-present it, ~16fps. */
        if (bl_state != BL_OFF && g_mq.active) {
            uint32_t nowm = mmio_read32(USEC_TIMER_ADDR);
            /* Phase clock (g_mq.t0) + target-change detection live in mq_set, so
             * this just paces the redraw at the shared live offset. */
            if ((uint32_t)(nowm - last_mq) >= 33000u) {   /* ~30fps               */
                draw_marquee(g_mq.x, g_mq.y, g_mq.w, g_mq.text, g_mq.tw, g_mq.font,
                             g_mq.ink, g_mq.bg, nowm - g_mq.t0, g_mq.cy0, g_mq.cy1);
                int ptop = g_mq.y - text_ascent(g_mq.font);
                int pbot = g_mq.y + text_descent(g_mq.font);
                if (ptop < 0) ptop = 0;
                lcd_present_rect(console_framebuffer(),
                                 g_mq.x, ptop, g_mq.w, pbot - ptop);
                last_mq = nowm;
            }
        }

        /* Load album covers while on the album list. Idle → blast several per
         * pass (covers fill near-instantly). While a song plays we still spread
         * the I/O so it can't starve decode, but a folder.thm is only ~1KB and
         * the anti-skip diskbuf is 8MB (~73s), so a small BATCH every ~60ms
         * (~50 covers/s) is safe and stops the old one-line-at-a-time trickle.
         *
         * Every load is a SYNCHRONOUS disk read on this loop, so two things are
         * held back while the wheel is actually turning:
         *
         *   - A PARKED platter is not woken for a cover. player.c parks the
         *     drive between refill bursts (most of a track) and the idle timer
         *     parks it after 20 s, so the first cover read after either is a
         *     spin-up: the loop is frozen for the 1-3 s it takes (ATA_SPINUP_US
         *     allows 4), the wheel's motion piles into the latch, and the list
         *     jumps when it comes back. Covers wait for CHIP_SPINUP_QUIET_US of
         *     wheel silence, so that stall lands on a still screen — a wait, not
         *     a jerk — and a slow deliberate scroll never triggers it at all.
         *   - With the platter up, a scroll in progress gets ONE read per pass.
         *     Six reads is ~60 ms of seek+PIO stacked onto the pass that also
         *     has to paint the new window; one bounds the hitch and the rest of
         *     the window fills the moment the wheel settles.
         *
         * The pump is told the window (artcache_pump_for), so whichever reads
         * do happen go to the rows on screen, top first — never to rows that
         * already scrolled past. */
        if (scr_cur() == SCR_BROWSER && g_dir_depth == 0) {
            uint32_t nowc = mmio_read32(USEC_TIMER_ADDR);
            int idle_now  = !player_active();
            uint32_t since_wheel = nowc - wheel_last_us();
            int moving = since_wheel < CHIP_WHEEL_SETTLE_US;
            int budget;
            if (ata_is_parked() && since_wheel < CHIP_SPINUP_QUIET_US) {
                budget = 0;
                g_ui_art_held++;
            } else if (moving) {
                budget = 1;
            } else {
                budget = idle_now ? 6 : 3;
            }
            if (budget > 0 && (idle_now || (uint32_t)(nowc - last_chip) >= 60000u)) {
                /* A pumped slot is only worth a repaint if it is actually ON
                 * SCREEN: pumping a whole library's covers used to set dirty for
                 * every one of them, forcing ~21 consecutive full-frame repaints
                 * for rows nobody was looking at. Snapshot the visible rows'
                 * chips, pump, and repaint only the rows whose chip appeared —
                 * through g_lp.rows, so a landed cover costs one row band, not a
                 * cleared panel and a full-frame push (see list_repaint_partial). */
                const uint16_t *before[LIST_ROWS2];
                int want[LIST_ROWS2];
                int vtop = ui_scroll_window(g_br_sel, albumlist_count(), LIST_ROWS2);
                /* PEEK, not get: this loop is only observing. artcache_get
                 * claims a way and re-stamps the LRU, so using it here (12
                 * times a pass, always in row order) is what pinned the top
                 * row to the lowest stamp and starved it. The render's own
                 * artcache_get calls are what should drive residency. */
                /* Map row -> global album index: the cache is keyed by album,
                 * and a row is not an album when an artist filter puts the
                 * synthetic "All Songs" row at 0. */
                for (int r = 0; r < LIST_ROWS2; r++) {
                    want[r]   = albumlist_album_at(vtop + r);
                    before[r] = (want[r] >= 0) ? artcache_peek(want[r]) : 0;
                }
                uint32_t t_art = mmio_read32(USEC_TIMER_ADDR);
                int loaded = 0;
                while (loaded < budget && artcache_pump_for(fs, want, LIST_ROWS2)) {
                    loaded++;
                }
                t_art = mmio_read32(USEC_TIMER_ADDR) - t_art;
                g_ui_art_reads += (uint32_t)loaded;
                if (loaded && t_art > g_ui_art_max_us) g_ui_art_max_us = t_art;
                for (int r = 0; r < LIST_ROWS2; r++) {
                    const uint16_t *now_px = (want[r] >= 0) ? artcache_peek(want[r]) : 0;
                    if (now_px != before[r]) {
                        g_lp.rows |= 1u << r;
                        dirty = 1;
                    }
                }
                last_chip = nowc;
            }
        }

        /* The scroll diagnostics line, on the same 5 s cadence as the battery
         * and audio lines but only when something was painted. */
        {
            uint32_t nows = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nows - last_uistat) >= 5000000u) {
                ui_stats_emit();
                last_uistat = nows;
            }
        }

        /* While PLAYING, a pass that did nothing — no decode work, no repaint —
         * used to free-spin the core at 80 MHz, re-polling the wheel (which masks
         * and unmasks IRQs) thousands of times a second. Halt instead. The halt
         * is 200 us, NOT milliseconds: the countdown is not documented to wake on
         * an interrupt, and the audio DMA ISR has only the ~363 us I2S FIFO of
         * slack — 200 us stays inside that budget while still parking the core
         * for the overwhelming majority of an idle pass. Input is latched by the
         * tick ISR and every animation here ticks at >= 33 ms, so nothing is lost.
         *
         * Gated on player_playing(), not player_active(). The 200 us figure is a
         * DMA-feeding budget, and a PAUSED player has no DMA to feed — but
         * active() stays 1 across a pause, so a paused device used to take this
         * branch and spin the loop ~5,000 times a second (each pass a
         * player_pump call, several USEC_TIMER reads, GPIO reads and an event
         * drain) for as long as it sat paused. Paused is idle: it belongs in
         * the 10 ms halt below, fifty times fewer wakeups. */
        if (player_playing() && !dirty && pump_us < 200u) {
            cpu_wait_us(200);
        }

        /* Only throttle when idle; while playing, player_pump's decode_step
         * paces the loop and the wheel stays responsive. Halt the core until the
         * next tick (self-waking ~10 ms) instead of a busy-spin: input is latched
         * by the 100 Hz timer ISR, so this costs no responsiveness. "Idle"
         * includes paused (see above); the paused pump's only job — the codec
         * power-down timeout — is seconds long, so 100 Hz is ample for it. */
        if (!player_playing()) {
            cpu_wait_ms(10);
        }

        /*
         * Supervisor stack guard. crt0 paints the region below _stack_limit, so
         * this is an O(1) look at the low words — cheap enough to run every idle
         * pass. A breach means a deep UI call chain has already written past the
         * bottom of the stack, which is silent memory corruption; panic() at
         * least turns it into a readable screen with the faulting PC instead of
         * a mystery freeze or garbled state hours later.
         */
        if (stack_guard_breached()) {
            panic(PANIC_STACK, 0);
        }
    }
}

/* Per-task stack for the idle task (carved from .bss). Only reached if the LCD
 * is unpowered or the disk won't mount — the browser above never returns. */
#define TASK_STACK_SIZE 1024
static uint8_t idle_stack[TASK_STACK_SIZE];

/* Disk scratch (uint16_t for the 2-byte alignment ata_read_sectors needs). */
static uint16_t mbr_sector[1024];

/*
 * Idle task: never exits. Sleep the CPU for a short countdown, then yield.
 * The scheduler is only started on the no-LCD / mount-failure fallback path.
 */
_Noreturn static void idle_task(void) {
    uart_puts("core: idle task entered\n");
    for (;;) {
        cpu_wait_ms(10);
        sched_yield();
    }
}

_Noreturn void kernel_main(void) {
    /* Free-running 1 MHz microsecond counter (USEC_TIMER, 01-soc-pp5022.md
     * "Timers"), sampled at the very top so later milestones can report
     * elapsed-since-boot in real time, INDEPENDENT of the 100 Hz tick. */
    uint32_t boot_us0 = mmio_read32(USEC_TIMER_ADDR);

    uart_init();

    uart_puts("core: kernel alive (iPod 5G/5.5G, PP5022)\n");

    /*
     * Hand the shared list chrome its scrolling-title implementation.
     *
     * ui_list_row() draws an overflowing title through this hook. The marquee
     * needs USEC_TIMER, and a time source is exactly what ui/chrome.c must not
     * reach for if it is to build on the host — so the dependency is injected
     * here instead. Registered once, before anything can paint; leave it unset
     * (as the host tests do) and titles are plainly clipped.
     */
    ui_set_scroll_text(mq_text);

    /*
     * The wheel's acceleration state machine (ui/wheel.c) is portable for the
     * same reason and by the same means: its clock, its "what letter is row N
     * on this screen" source and its navigation click are injected here rather
     * than reached for. Unset, none of them can misbehave — no clock reads 0,
     * no letter source means nothing letter-steps, no click is silent.
     */
    wheel_set_clock(wheel_clock);
    wheel_set_initial_at(list_initial_at);
    wheel_set_click(ui_click);

    /* Hex-path self-test: if this doesn't read 1234ABCD on the terminal,
     * distrust every register dump that follows. */
    uart_puts("core: uart self-test ");
    uart_put_hex32(0x1234ABCD);
    uart_putc('\n');

    uart_puts("core: PROCESSOR_ID ");
    uart_put_hex32(PROCESSOR_ID);
    uart_putc('\n');

    /* Come off the 24 MHz boot clock up to CPUFREQ_NORMAL (30 MHz) before the
     * rest of bring-up so it runs at a sane speed. */
    uart_puts("core: clock init -> 30 MHz\n");
    clock_init();
    uart_puts("core: cpu freq ");
    uart_put_hex32(cpu_frequency());
    uart_putc('\n');

    /* Turn on the unified cache now — decode is far too slow with it off.
     * Write-back, so the audio DMA path flushes (cache_commit) before the DMA
     * reads a freshly-filled buffer. */
    uart_puts("core: cache init\n");
    cache_init();

    /* 100 Hz system tick + core IRQ unmask BEFORE audio: continuous playback is
     * DMA-driven and needs its completion interrupt delivered, and sleep_ms
     * spins on the IRQ-fed tick. */
    uart_puts("core: timer init @ 100 Hz, enabling IRQs\n");
    timer_init();
    arch_irq_enable();

    /* Boot clock starts here — the earliest point with a running timer. */
    g_boot_t0_us = mmio_read32(USEC_TIMER_ADDR);

    /* (A tick self-test used to sleep 100 ms here to prove the IRQ-fed tick
     * advanced. It did its job during bring-up; on a cold boot it is 100 ms of
     * a 10 s startup spent proving something the scheduler exercises anyway.) */

    /*
     * Backlight FIRST — before the LCD probe, deliberately.
     *
     * It used to live inside the lcd_init() block, which made it invisible
     * whether a dark panel meant "our code never ran" or "our code ran and
     * lcd_init() failed". Chainloaded from ipodloader2 that never mattered:
     * the loader draws its own menu, so it has already lit the backlight and
     * brought the BCM up before handing off. Booting DIRECTLY from the
     * firmware partition, nobody has — and a cold-BCM lcd_init() failure
     * skipped the backlight, the disk and the UI in one go, which is
     * indistinguishable from a crash in crt0 (2026-07-26: black screen, cause
     * undiagnosable for exactly this reason).
     *
     * Nothing here depends on the LCD: backlight_init() is GPIO B/D/L only
     * (charge pump, step line, LED enable) and its delay loops self-scale to
     * the live core clock via cpu_frequency(), whose 30 MHz calibration point
     * is precisely where we are now — before cpu_boost(), not after.
     *
     * Cost: the panel is lit for the duration of lcd_init() (up to ~500 ms of
     * BCM bring-up) before boot_splash() paints, so a cold boot can show a
     * brief bright field. That is the price of being able to see a boot at
     * all, and it is one line to put back.
     */
    backlight_init();

    /* LCD probe gates the whole disk/audio/UI stack: nonzero BCM power => real
     * hardware. The clicky emulator has no BCM (lcd_init() false there), so
     * this block is skipped, keeping the emulator smoke green; the register
     * grammar is proven host-side by the mock-bus trace tests. */
    uint32_t lcd_t0 = boot_ms_now();
    int      lcd_ok = lcd_init();
    g_boot_lcd_ms   = boot_ms_now() - lcd_t0;
    if (lcd_ok) {
        /* Boost to 80 MHz for the whole disk/decode path. The UI runs here too;
         * we never drop back because the browser never returns (fine for
         * bring-up — the device is on a cable during testing). */
        cpu_boost();

        /* Bring up the I2C control bus now (not just at first-song hal_audio_init)
         * so the status strip can read the PCF50605 battery gauge from the menu.
         * Bounded/idempotent — hal_audio_init re-inits it harmlessly per track. */
        i2c_init();
        battery_init();
        hal_volume_init();               /* codec output gain -> safe default      */
        piezo_init();                    /* PWM click for menu navigation          */

        /* Paint the boot splash immediately, so the panel shows CORE branding
         * instead of the chainloader's leftover framebuffer (a blue field with
         * a green stripe) while the disk spins up and the volume mounts. */
        boot_splash();

        /* Read the MBR, find the FAT32 data partition (type 0B/0C), mount it. */
        uint8_t *mbr = (uint8_t *)mbr_sector;
        uint32_t sig = 0, fat_lba = 0;
        int      mnt = -1;
        fat32_t  fs;
        /* Everything from here to the end of fat32_mount is "the disk": the
         * IDE reset, the first read (which is what actually spins the platters
         * up, and is allowed 4 s for it — see ATA_SPINUP_US), and the mount's
         * own reads. Timed as one phase because that is the unit we could move
         * or overlap; splitting it finer would not change what we can do. */
        uint32_t disk_t0 = boot_ms_now();
        if (ata_init() == 0 && ata_read_sectors(0, 1, mbr) == 0) {
            sig = (uint32_t)mbr[510] | ((uint32_t)mbr[511] << 8);
            for (int p = 0; p < 4; p++) {
                const uint8_t *e = &mbr[0x1BE + 16 * p];
                if (e[4] == 0x0B || e[4] == 0x0C) {
                    fat_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                              ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                    break;
                }
            }
        }
        if (sig == 0xAA55u && fat_lba != 0) {
            /* The MBR start-LBA may be in native 512-byte units OR (stock 80 GB,
             * 2048-byte FAT sectors) in 2048-byte units. Try as-is, then x4;
             * fat32_mount validates the boot signature + BPB. */
            mnt = fat32_mount(&fs, player_disk_read, 0, fat_lba);
            if (mnt != 0) {
                mnt = fat32_mount(&fs, player_disk_read, 0, fat_lba * 4u);
            }
        }

        g_boot_disk_ms = boot_ms_now() - disk_t0;

        uint32_t btus = mmio_read32(USEC_TIMER_ADDR) - boot_us0;
        uart_puts("core: mount rc ");
        uart_put_hex32((uint32_t)mnt);
        uart_puts(" BTUS ");
        uart_put_hex32(btus);
        uart_putc('\n');

        if (mnt == 0) {
            /* Capacity / free (MB) for the About screen (64-bit to avoid the
             * clusters*bytes overflow on a big volume). */
            g_total_mb = (uint32_t)(((uint64_t)fs.total_clus * fs.clus_bytes) >> 20);
            g_free_mb  = (fs.free_clus == 0xFFFFFFFFu) ? 0xFFFFFFFFu
                       : (uint32_t)(((uint64_t)fs.free_clus * fs.clus_bytes) >> 20);
            uart_puts("core: entering browser\n");
            run_ui(&fs);                       /* never returns */
        }

        /* Mount failed: show a diagnostic error screen and fall through to the
         * idle scheduler so the device isn't a black brick. */
        console_clear(CON_RED);
        console_str  (2, 3, "MOUNT FAILED", CON_WHITE, CON_RED);
        console_str  (2, 6, "SIG",  CON_WHITE, CON_RED);
        console_hex32(8, 6, sig,               CON_WHITE, CON_RED);
        console_str  (2, 8, "MNT",  CON_WHITE, CON_RED);
        console_hex32(8, 8, (uint32_t)mnt,     CON_WHITE, CON_RED);
        console_str  (2, 10, "PART", CON_WHITE, CON_RED);
        console_hex32(8, 10, fat_lba,          CON_WHITE, CON_RED);
        lcd_present_fb(console_framebuffer());
        cpu_unboost();
    } else {
        uart_puts("core: lcd bcm NOT powered, skipping disk + UI\n");
    }

    /* Fallback only (no LCD or mount failure): idle the core under the
     * cooperative scheduler. sched_start never returns. */
    uart_puts("core: sched init (idle fallback)\n");
    sched_init();
    if (sched_add_task(idle_task, idle_stack, sizeof idle_stack, "idle") < 0) {
        uart_puts("core: FATAL sched_add_task failed\n");
        for (;;) {
        }
    }
    sched_start();
}
