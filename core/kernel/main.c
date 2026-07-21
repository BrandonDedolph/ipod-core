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
#include "hal.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "sched.h"
#include "timer.h"
#include "irq.h"
#include "clock.h"
#include "cache.h"
#include "console.h"
#include "../ui/text.h"
#include "../ui/thumb.h"
#include "../ui/artcache.h"
#include "../ui/screen_charging.h"
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

/* ---------------------------------------------------------------------------
 * Linen theme + Nunito faces
 * ------------------------------------------------------------------------- */

/* Linen theme palette (design_reference/README.md tokens -> RGB565).
 *   surface #f4f1ec, ink #1a1714, muted #7a7068, accent terracotta,
 *   border = 8% ink over surface. */
#define LINEN_SURFACE 0xF79Du            /* #f4f1ec warm off-white               */
#define LINEN_INK     0x18A2u            /* #1a1714 near-black text              */
#define LINEN_MUTED   0x7B8Du            /* #7a7068 secondary text               */
#define LINEN_ACCENT  0xC348u            /* terracotta accent (progress/low-batt)*/
#define LINEN_BORDER  0xE71Bu            /* subtle divider line                  */

/* Design-list tones (menus.jsx): the selection is a DARK ink bar with light
 * text (NOT a terracotta bar); sub-lines + right values use two muted greys. */
#define LINEN_SEL_BG  LINEN_INK          /* selection bar = dark ink             */
#define LINEN_SEL_FG  LINEN_SURFACE      /* selection text = surface             */
#define LINEN_MUTED2  0x9C70u            /* #9a8e80 lighter muted (subs/strip)   */
#define LINEN_MUTED_D 0x5A89u            /* #5a5048 deep muted (right values)    */
#define LINEN_SEL_SUB 0xB595u            /* sub/right text ON a selected row      */
#define LINEN_CHEVRON 0xB575u            /* › chevron, rgba(ink,0.3) on surface   */
#define LINEN_SB_TRK  0xE73Cu            /* scrollbar track, rgba(ink,0.06)       */
#define LINEN_SB_THMB 0xAD34u            /* scrollbar thumb,  rgba(ink,0.35)      */

/* Nunito faces (freestanding renderer, core/ui/text.h). */
#define FONT_HEADER   text_font_bold_13()
#define FONT_ROW      text_font_regular_13()
#define FONT_TITLE    text_font_bold_17()
#define FONT_SUB      text_font_regular_11()
#define FONT_SMALL    text_font_regular_9()

/* Draw a NUL-terminated string; thin wrapper over text_draw with the panel
 * dimensions baked in. `y` is the text baseline. Returns the advance. */
static int ui_text(int x, int y, const char *s, const text_font_t *font,
                   uint16_t ink)
{
    return text_draw(console_fb(), LCD_WIDTH, LCD_HEIGHT, x, y, s, font, ink);
}

/* Centre a string horizontally at baseline `y`. */
static void ui_text_centered(int y, const char *s, const text_font_t *font,
                             uint16_t ink)
{
    int w = text_width(s, font);
    ui_text((LCD_WIDTH - w) / 2, y, s, font, ink);
}

/* ---------------------------------------------------------------------------
 * File / album browser
 *
 * browse_entry_t + BROWSE_MAX/NAME_MAX are shared with the player (player.h):
 * a folder's worth of entries is copied into the player as its queue.
 * ------------------------------------------------------------------------- */

static browse_entry_t g_browse[BROWSE_MAX];
static int            g_browse_n;

/* Directory navigation: the cluster of the directory currently listed, plus a
 * stack of parent clusters so MENU can climb back up. Depth 0 == the browser
 * root (the album list). */
#define DIR_STACK_MAX 12
static uint32_t g_cur_dir;
static uint32_t g_dir_stack[DIR_STACK_MAX];
static int      g_dir_depth;

/* Album art (folder.art) location captured while enumerating the current
 * folder; handed to player_play_queue so the player owns/validates it. */
static uint32_t g_art_clus, g_art_size;

/* When set, the album list (depth 0) only shows folders whose "Artist - Album"
 * name has this artist prefix — the Artists → one-artist's-albums drill-down.
 * Empty means the plain "Albums" list (every folder). */
static char g_artist_filter[NAME_MAX + 1];

/* Case-insensitive ASCII match of a dirent name against a literal. */
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

/* Junk-filter for the album list: skip iPod/OS system folders and any dotfolder
 * (.Trashes, .Spotlight-V100, .fseventsd, …) so only music folders show. */
static int is_junk_dir(const char *name)
{
    if (name[0] == '.') {
        return 1;
    }
    static const char *const junk[] = {
        "iPod_Control", "Calendars", "Contacts", "Photos", "Recordings",
        "Notes", "System Volume Information", "$RECYCLE.BIN", "LOST.DIR",
        "Find My iPod",
    };
    for (unsigned i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        if (name_eq_ci(name, junk[i])) {
            return 1;
        }
    }
    return 0;
}

/* Copy `src` into `dst` (<= NAME_MAX), keeping only printable ASCII (the Nunito
 * atlas covers 0x20..0x7E; UTF-8 multibyte bytes in fancy filenames are dropped
 * rather than drawn as tofu). If `drop_ext`, trim a trailing ".ext" so the list
 * shows track titles, not filenames. */
static void copy_display_name(char *dst, const char *src, int drop_ext)
{
    int end = 0;
    while (src[end]) end++;
    if (drop_ext) {
        int dot = -1;
        for (int j = 0; src[j]; j++) {
            if (src[j] == '.') dot = j;
        }
        if (dot > 0) end = dot;              /* trim the extension */
    }
    int i = 0;
    for (int j = 0; j < end && i < NAME_MAX; j++) {
        char c = src[j];
        if (c >= 0x20 && c <= 0x7E) {
            dst[i++] = c;
        }
    }
    dst[i] = '\0';
}

/* Split a "Artist - Album" folder name on its first " - " separator (the loader
 * names album folders this way). No separator -> artist empty, album = whole
 * name. Both outputs NAME_MAX-bounded + printable-ASCII (copy_display_name). */
static void split_artist_album(const char *name, char *artist, char *album)
{
    int sep = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == ' ' && name[i + 1] == '-' && name[i + 2] == ' ') {
            sep = i;
            break;
        }
    }
    if (sep < 0) {
        artist[0] = '\0';
        copy_display_name(album, name, 0);
        return;
    }
    char tmp[NAME_MAX + 1];
    int n = (sep < NAME_MAX) ? sep : NAME_MAX;
    for (int i = 0; i < n; i++) tmp[i] = name[i];
    tmp[n] = '\0';
    copy_display_name(artist, tmp, 0);
    copy_display_name(album, name + sep + 3, 0);
}

/* MP3 playback is parked: dr_mp3's float synthesis can't hit real-time on this
 * FPU-less CPU (buffer starves -> stutter), and FLAC is lossless so there's no
 * quality reason to prefer it. The device is FLAC-only; a companion loader app
 * ensures music lands as FLAC. Flip to 1 to re-surface MP3 files (they'll open
 * but stutter) once a fixed-point/COP decoder exists. */
#define CORE_ENABLE_MP3 0

/* Classify by extension: 0 = FLAC (.fla/.flac), 1 = MP3 (.mp3), -1 = skip. */
static int classify_ext(const char *name)
{
    int dot = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') dot = i;
    }
    if (dot < 0) return -1;

    char ext[5];
    int n = 0;
    for (const char *e = name + dot + 1; *e && n < 4; e++) {
        char c = *e;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        ext[n++] = c;
    }
    ext[n] = '\0';

    if (n == 3 && ext[0] == 'F' && ext[1] == 'L' && ext[2] == 'A') return 0;
    if (n == 4 && ext[0] == 'F' && ext[1] == 'L' && ext[2] == 'A' && ext[3] == 'C') return 0;
#if CORE_ENABLE_MP3
    if (n == 3 && ext[0] == 'M' && ext[1] == 'P' && ext[2] == '3') return 1;
#endif
    return -1;
}

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
    return 0;
}

/* Vertical layout (menus.jsx): a status strip up top, a titled header with a
 * divider, then the scrolling list. */
#define STATUS_Y0  0                      /* status strip band (battery/track)    */
#define STATUS_H   15                     /* strip height                         */
#define HDR_BASE   30                     /* header title text baseline           */
#define HDR_DIV_Y  38                     /* header divider row                    */
#define LIST_Y0    42                     /* first list row top                   */
#define ROW_H      24                     /* px per list row (design: 22-24px)    */
#define LIST_ROWS  8                       /* visible rows: (240-42)/24 ~= 8       */

/* Wheel scroll feel. The driver reports the raw differenced detent count (up to
 * ~half a rotation per poll), so adding it straight to the selection flung the
 * list. Accumulate detents and advance one row per WHEEL_CLICKS_PER_ITEM,
 * clamping a single event so a fast flick can't teleport to the end. */
#define WHEEL_CLICKS_PER_ITEM 3          /* higher = less sensitive             */
#define WHEEL_MAX_DELTA       6          /* max raw detents honoured per event  */

/* ---------------------------------------------------------------------------
 * Design-matched list chrome (menus.jsx): status strip, header, rows, scrollbar
 * ------------------------------------------------------------------------- */

/* Hold-switch lock state. While g_locked, all wheel/button input is swallowed
 * (playback keeps running); a brief plate flashes on the engage/disengage edge,
 * and a small padlock stays in the status strip while held. */
static int      g_locked;
static uint32_t g_lock_flash_until;

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

/* Returns 1 if it actually resampled this call (so a live screen can repaint). */
static int battery_refresh(int force)
{
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    if (!force && (uint32_t)(now - g_bat_last_us) < 5000000u) {
        return 0;
    }
    g_bat_last_us = now;
    g_bat_mv  = battery_millivolts();
    g_bat_pct = battery_percent();
    g_bat_ext = power_is_external();
    return 1;
}

/* Small battery glyph: outline + nub + a fill proportional to `pct` (terracotta
 * when low, ink otherwise). Drawn at top-left (x,y), ~20px wide incl. the nub. */
static void draw_battery(int x, int y, int pct)
{
    const int w = 17, h = 9;
    console_fill_rect(x, y, w, 1, LINEN_MUTED2);           /* top    */
    console_fill_rect(x, y + h - 1, w, 1, LINEN_MUTED2);   /* bottom */
    console_fill_rect(x, y, 1, h, LINEN_MUTED2);           /* left   */
    console_fill_rect(x + w - 1, y, 1, h, LINEN_MUTED2);   /* right  */
    console_fill_rect(x + w, y + 3, 2, h - 6, LINEN_MUTED2); /* nub   */
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int fw = ((w - 4) * pct) / 100;
    if (fw > 0) {
        console_fill_rect(x + 2, y + 2, fw, h - 4,
                          pct <= 20 ? LINEN_ACCENT : LINEN_INK);
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
    const char *left = player_active() ? player_track_name() : "CORE";
    ui_text(12, STATUS_Y0 + 11, left, FONT_SMALL, LINEN_MUTED2);

    /* Clear a right-hand region so a long track name can't collide with it. */
    console_fill_rect(LCD_WIDTH - 70, STATUS_Y0, 70, STATUS_H, LINEN_SURFACE);

    int bx = LCD_WIDTH - 12 - 19;             /* battery block (17 + 2 nub)        */
    draw_battery(bx, STATUS_Y0 + 3, g_bat_pct);

    /* Persistent padlock while Hold is engaged (design keeps it in the strip). */
    if (g_locked) {
        draw_lock_glyph(bx - 14, STATUS_Y0 + 3, LINEN_INK);
    }

    /* Raw mV to the left of the glyph (calibration aid). Hidden while locked so
     * it doesn't collide with the padlock. */
    if (g_bat_mv > 0 && !g_locked) {
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
static void header_render(const char *title, const char *right, int back)
{
    int x = 12;
    if (back) {
        x = ui_text(x, HDR_BASE, "<", FONT_HEADER, LINEN_MUTED2) + 4;
    }
    ui_text(x, HDR_BASE, title, FONT_HEADER, LINEN_INK);
    if (right && right[0]) {
        int w = text_width(right, FONT_SMALL);
        ui_text(LCD_WIDTH - 12 - w, HDR_BASE - 1, right, FONT_SMALL, LINEN_MUTED2);
    }
    console_fill_rect(12, HDR_DIV_Y, LCD_WIDTH - 24, 1, LINEN_BORDER);
}

/* One list row (menus.jsx Row) at list origin `y0`. Selected = filled ink bar +
 * light bold text; greyed = muted (inactive menu item). Optional sub-line, right
 * value, chevron, and a leading 22x22 art chip (RGB565, or NULL). */
static void list_row_at(int y0, int r, const char *text, const char *sub,
                        const char *right, int chevron, int selected, int greyed,
                        const uint16_t *chip)
{
    int ry = y0 + r * ROW_H;
    uint16_t fg, subc, rightc, chevc;
    if (selected) {
        console_fill_rect(6, ry + 1, LCD_WIDTH - 16, ROW_H - 2, LINEN_SEL_BG);
        fg = LINEN_SEL_FG; subc = LINEN_SEL_SUB; rightc = LINEN_SEL_SUB;
        chevc = LINEN_SEL_SUB;
    } else {
        fg = greyed ? LINEN_MUTED : LINEN_INK;
        subc = LINEN_MUTED2; rightc = LINEN_MUTED_D; chevc = LINEN_CHEVRON;
    }

    int tx = 14;
    if (chip) {
        console_blit565(12, ry + (ROW_H - 22) / 2, 22, 22, chip);
        tx = 12 + 22 + 8;
    }
    int base = sub ? ry + 11 : ry + 15;       /* lift the title when a sub is shown */
    ui_text(tx, base, text, selected ? FONT_HEADER : FONT_ROW, fg);
    if (sub) {
        ui_text(tx, ry + 21, sub, FONT_SMALL, subc);
    }
    if (right && right[0]) {
        int w = text_width(right, FONT_SUB);
        ui_text(LCD_WIDTH - 16 - w, ry + 15, right, FONT_SUB, rightc);
    } else if (chevron) {
        ui_text(LCD_WIDTH - 18, ry + 15, ">", FONT_ROW, chevc);
    }
}

/* Convenience: a row in the default (full-height) list. */
static void list_row(int r, const char *text, const char *sub, const char *right,
                     int chevron, int selected, int greyed, const uint16_t *chip)
{
    list_row_at(LIST_Y0, r, text, sub, right, chevron, selected, greyed, chip);
}

/* Slim right-edge scrollbar (menus.jsx Scrollbar); no-op when everything fits.
 * `y0` is the list origin (differs between the full list and the detail view). */
static void scrollbar_render(int y0, int top, int visible, int total)
{
    if (total <= visible) {
        return;
    }
    int track_y = y0;
    int track_h = LCD_HEIGHT - y0 - 4;
    console_fill_rect(LCD_WIDTH - 4, track_y, 3, track_h, LINEN_SB_TRK);
    int thumb_h = (visible * track_h) / total;
    if (thumb_h < 16) thumb_h = 16;
    int denom = total - visible;
    if (denom < 1) denom = 1;
    int thumb_y = track_y + (top * (track_h - thumb_h)) / denom;
    console_fill_rect(LCD_WIDTH - 4, thumb_y, 3, thumb_h, LINEN_SB_THMB);
}

/* Windowed-list scroll origin (menus.jsx useScrollWindow): keep the selection
 * about 1/3 from the top, clamped to the ends. Pure function — the browser
 * derives the visible window from the selection each paint (no separate top
 * state to keep in sync). */
static int scroll_window(int sel, int total, int visible)
{
    if (total <= visible) return 0;
    int start = sel - visible / 3;
    if (start < 0) start = 0;
    if (start > total - visible) start = total - visible;
    return start;
}

/* Format "a / b" into dst (needs >= 12 bytes). */
static void fmt_count(char *dst, int a, int b)
{
    int i = 0, v, t; char nb[6];
    v = a; t = 0; do { nb[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 5);
    while (t > 0) dst[i++] = nb[--t];
    dst[i++] = ' '; dst[i++] = '/'; dst[i++] = ' ';
    v = b; t = 0; do { nb[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 5);
    while (t > 0) dst[i++] = nb[--t];
    dst[i] = '\0';
}

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

#define ART_RAW_MAX  (12 + 120 * 120 * 2)  /* a full 120x120 CoreArt file          */
static uint8_t  g_art_scratch[ART_RAW_MAX];      /* raw folder.art read buffer      */
static uint16_t g_detail_art[DET_ART * DET_ART]; /* downscaled 56x56 hero           */
static int      g_detail_art_ok;
static char     g_album_title[NAME_MAX + 1];     /* album part of "Artist - Album" */
static char     g_album_artist[NAME_MAX + 1];    /* artist part (empty if none)    */
static int      g_album_track_n;                  /* playable files in the folder   */

/* Read the current folder's folder.art (clus/size captured by browse_load) and
 * downscale it to the 56x56 hero. Leaves g_detail_art_ok=0 if absent/malformed.
 * Same CoreArt "CART" validation the player uses (player.c load_folder_art). */
static void detail_art_load(fat32_t *fs)
{
    g_detail_art_ok = 0;
    if (g_art_clus == 0 || g_art_size < 12 || g_art_size > sizeof g_art_scratch) {
        return;
    }
    int32_t n = fat32_read_file(fs, g_art_clus, g_art_scratch, g_art_size);
    if (n < 12) {
        return;
    }
    if (g_art_scratch[0] != 'C' || g_art_scratch[1] != 'A' ||
        g_art_scratch[2] != 'R' || g_art_scratch[3] != 'T') {
        return;
    }
    int w = g_art_scratch[6] | (g_art_scratch[7] << 8);
    int h = g_art_scratch[8] | (g_art_scratch[9] << 8);
    if (w <= 0 || h <= 0 || w > 120 || h > 120) {
        return;
    }
    if ((int32_t)(12 + w * h * 2) > n) {
        return;
    }
    thumb_downscale_rgb565((const uint16_t *)(g_art_scratch + 12), w, h,
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

static void detail_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    fmt_count(right, sel + 1, g_browse_n > 0 ? g_browse_n : 1);
    header_render("Albums", right, 1);

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
    char meta[24];
    { int i = 0, v = g_album_track_n, t = 0; char nb[6];
      do { nb[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 5);
      while (t > 0) meta[i++] = nb[--t];
      for (const char *p = " tracks"; *p; p++) meta[i++] = *p;
      meta[i] = '\0'; }
    ui_text(tx, DET_HERO_Y + 47, meta, FONT_SMALL, LINEN_MUTED2);

    console_fill_rect(12, DET_LIST_Y0 - 6, LCD_WIDTH - 24, 1, LINEN_BORDER);

    if (g_browse_n == 0) {
        ui_text(14, DET_LIST_Y0 + 14, "Empty folder", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = scroll_window(sel, g_browse_n, DET_ROWS);
    const char *playing = player_active() ? player_track_name() : 0;
    for (int r = 0; r < DET_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_browse_n) break;
        const browse_entry_t *e = &g_browse[idx];
        int is_sel = (idx == sel);
        list_row_at(DET_LIST_Y0, r, e->name, 0, 0, 0, is_sel, 0, 0);
        /* Mark the track that's currently playing with the animated bars. */
        if (playing && !e->is_dir && name_eq_ci(e->name, playing)) {
            int ry = DET_LIST_Y0 + r * ROW_H;
            nowplaying_bars(LCD_WIDTH - 22, ry + 7,
                            is_sel ? LINEN_SEL_FG : LINEN_INK,
                            mmio_read32(USEC_TIMER_ADDR));
        }
    }
    scrollbar_render(DET_LIST_Y0, top, DET_ROWS, g_browse_n);
}

/* A neutral tile shown in a row's chip slot until its real cover loads, so the
 * text doesn't shift right when the thumbnail pops in. Filled once at startup. */
static uint16_t g_chip_ph[ARTCACHE_DIM * ARTCACHE_DIM];

static void chip_placeholder_init(void)
{
    for (int i = 0; i < ARTCACHE_DIM * ARTCACHE_DIM; i++) {
        g_chip_ph[i] = LINEN_BORDER;
    }
}

/* Queue every album row's cover into the incremental thumbnail cache. Called
 * when the album list is (re)entered; artcache_pump loads them a few frames
 * apart off the audio path. */
static void albumlist_queue_chips(void)
{
    artcache_reset();
    for (int i = 0; i < g_browse_n && i < ARTCACHE_SLOTS; i++) {
        if (g_browse[i].is_dir) {
            artcache_queue(i, g_browse[i].clus);
        }
    }
}

/* Album LIST (browser depth 0): the folder list with the design chrome, each
 * album row carrying a 22x22 cover chip (or a placeholder until it loads). */
static void albumlist_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_browse_n > 0) {
        fmt_count(right, sel + 1, g_browse_n);
    } else {
        right[0] = '\0';
    }
    /* Header title = the artist when drilled in from Artists, else "Albums". */
    header_render(g_artist_filter[0] ? g_artist_filter : "Albums", right, 1);

    if (g_browse_n == 0) {
        ui_text(14, LIST_Y0 + 20, "Empty folder", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = scroll_window(sel, g_browse_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_browse_n) break;
        const browse_entry_t *e = &g_browse[idx];
        const uint16_t *chip = 0;
        if (e->is_dir) {                       /* albums get a cover chip           */
            chip = (idx < ARTCACHE_SLOTS) ? artcache_get(idx) : 0;
            if (!chip) chip = g_chip_ph;       /* reserve the space meanwhile       */
        }
        /* Show "Album" as the title and the artist as a sub-line (parsed from the
         * "Artist - Album" folder name). In an artist's own list the artist sub
         * is redundant, so drop it there. */
        char artist[NAME_MAX + 1], album[NAME_MAX + 1];
        split_artist_album(e->name, artist, album);
        const char *sub = (!g_artist_filter[0] && artist[0]) ? artist : 0;
        list_row(r, album, sub, 0, e->is_dir, idx == sel, 0, chip);
    }
    scrollbar_render(LIST_Y0, top, LIST_ROWS, g_browse_n);
}

/* ---------------------------------------------------------------------------
 * Artists (menus.jsx ArtistsList): a list of the unique artist prefixes parsed
 * from the "Artist - Album" folder names. Selecting one filters the album list
 * (g_artist_filter) to just that artist's albums.
 * ------------------------------------------------------------------------- */
#define ARTISTS_MAX 96
static char g_artists[ARTISTS_MAX][NAME_MAX + 1];
static int  g_artists_n;
static int  g_artist_sel, g_artist_accum;

/* Build the unique, de-duplicated artist list from the currently-loaded root
 * folder listing (g_browse must hold the UNFILTERED album list). */
static void build_artists(void)
{
    g_artists_n = 0;
    for (int i = 0; i < g_browse_n; i++) {
        if (!g_browse[i].is_dir) continue;
        char artist[NAME_MAX + 1], album[NAME_MAX + 1];
        split_artist_album(g_browse[i].name, artist, album);
        if (!artist[0]) continue;                 /* no "Artist - " prefix        */
        int found = 0;
        for (int j = 0; j < g_artists_n; j++) {
            if (name_eq_ci(g_artists[j], artist)) { found = 1; break; }
        }
        if (!found && g_artists_n < ARTISTS_MAX) {
            int k = 0;
            for (; artist[k] && k < NAME_MAX; k++) g_artists[g_artists_n][k] = artist[k];
            g_artists[g_artists_n][k] = '\0';
            g_artists_n++;
        }
    }
}

static void artists_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_artists_n > 0) fmt_count(right, sel + 1, g_artists_n);
    else                 right[0] = '\0';
    header_render("Artists", right, 1);

    if (g_artists_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No artists", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = scroll_window(sel, g_artists_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_artists_n) break;
        list_row(r, g_artists[idx], 0, 0, 1, idx == sel, 0, 0);
    }
    scrollbar_render(LIST_Y0, top, LIST_ROWS, g_artists_n);
}

static void browse_render(int sel)
{
    if (g_dir_depth > 0) {
        detail_render(sel);
    } else {
        albumlist_render(sel);
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

/* Format `s` seconds as "M:SS" (minutes uncapped). */
static void fmt_time(char *buf, uint32_t s)
{
    uint32_t m = s / 60, ss = s % 60;
    int i = 0;
    if (m >= 10) buf[i++] = (char)('0' + (m / 10) % 10);
    buf[i++] = (char)('0' + m % 10);
    buf[i++] = ':';
    buf[i++] = (char)('0' + ss / 10);
    buf[i++] = (char)('0' + ss % 10);
    buf[i]   = '\0';
}

/* Output volume (WM8758 codec gain, 0..100) + how long the on-screen overlay
 * stays up after the last wheel tick (volume-demo.jsx: show ~1.5 s then fade). */
static int      g_volume = 70;
static uint32_t g_vol_show_until;

/* Centered volume overlay plate (volume-demo.jsx VolumeOverlay): speaker glyph +
 * ink fill bar + big percent, on a light near-surface plate. */
static void volume_overlay_render(int vol)
{
    const int PX = 60, PY = 101, PW = 200, PH = 32;
    console_fill_rect(PX, PY, PW, PH, 0xFFDEu);            /* light plate         */
    console_fill_rect(PX, PY, PW, 1, LINEN_BORDER);        /* 1px border          */
    console_fill_rect(PX, PY + PH - 1, PW, 1, LINEN_BORDER);
    console_fill_rect(PX, PY, 1, PH, LINEN_BORDER);
    console_fill_rect(PX + PW - 1, PY, 1, PH, LINEN_BORDER);

    /* Minimal speaker glyph: a small box + a growing cone. */
    int sx = PX + 14, sy = PY + PH / 2;
    console_fill_rect(sx, sy - 3, 3, 6, LINEN_INK);
    for (int k = 0; k < 5; k++) {
        console_fill_rect(sx + 3 + k, sy - 1 - k, 1, 2 + 2 * k, LINEN_INK);
    }

    /* Fill bar. */
    int bx = PX + 34, by = PY + PH / 2 - 3, bw = PW - 34 - 42, bh = 6;
    console_fill_rect(bx, by, bw, bh, 0xDEFBu);            /* track rgba(ink,0.12) */
    int fw = bw * vol / 100;
    if (fw < 0) fw = 0;
    if (fw > bw) fw = bw;
    console_fill_rect(bx, by, fw, bh, LINEN_INK);

    /* Percent, right-aligned. */
    char p[5]; int i = 0, v = vol, t = 0; char nb[4];
    do { nb[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 3);
    while (t > 0) p[i++] = nb[--t];
    p[i] = '\0';
    int w = text_width(p, FONT_SUB);
    ui_text(PX + PW - 14 - w, PY + PH / 2 + 4, p, FONT_SUB, LINEN_INK);
}

/* Bigger padlock for the lock/unlock plate: a body + shackle. Closed = the
 * shackle sits on the body with both legs down; open = it's swung up-left so
 * only the left leg meets the body and the right leg floats above it (the gap
 * reads as unlatched). */
static void draw_lock_icon(int cx, int cy, int open, uint16_t c)
{
    console_fill_rect(cx - 9, cy, 18, 14, c);            /* body */
    if (open) {
        console_fill_rect(cx - 9, cy - 12, 2, 12, c);    /* left leg -> body   */
        console_fill_rect(cx - 9, cy - 13, 12, 2, c);    /* top arch           */
        console_fill_rect(cx + 1, cy - 13, 2, 7, c);     /* right leg (floats) */
    } else {
        console_fill_rect(cx - 6, cy - 8, 2, 8, c);      /* left leg  */
        console_fill_rect(cx + 4, cy - 8, 2, 8, c);      /* right leg */
        console_fill_rect(cx - 6, cy - 9, 12, 2, c);     /* top arch  */
    }
}

/* Centered lock/unlock plate (system-screens.jsx LockedScreen/UnlockedScreen):
 * LOCKED = dark plate + light closed lock; UNLOCKED = light plate + dark open
 * lock. Drawn over whatever screen is currently in the framebuffer. */
static void lock_plate_render(int locked)
{
    const int PX = 70, PY = 65, PW = 180, PH = 110;
    uint16_t plate = locked ? LINEN_INK : LINEN_SURFACE;
    uint16_t fg    = locked ? LINEN_SURFACE : LINEN_INK;
    console_fill_rect(PX, PY, PW, PH, plate);
    if (!locked) {                                    /* border on the light plate */
        console_fill_rect(PX, PY, PW, 1, LINEN_BORDER);
        console_fill_rect(PX, PY + PH - 1, PW, 1, LINEN_BORDER);
        console_fill_rect(PX, PY, 1, PH, LINEN_BORDER);
        console_fill_rect(PX + PW - 1, PY, 1, PH, LINEN_BORDER);
    }
    draw_lock_icon(PX + PW / 2, PY + 34, !locked, fg);
    const char *label = locked ? "LOCKED" : "UNLOCKED";
    int w = text_width(label, FONT_HEADER);
    ui_text(PX + (PW - w) / 2, PY + 84, label, FONT_HEADER, fg);
}

/* Now-playing (Linen): album art up top, track title + elapsed/total clock and
 * an accent progress bar below, with a small buffer-health readout. Art + track
 * metadata come from the player (the PLAYING folder's, not the browsed one). */
static void nowplaying_render(const char *name, uint32_t elapsed_s,
                              uint32_t total_s, uint32_t buf_pct)
{
    console_clear(LINEN_SURFACE);

    /* Pre-scaled album art (folder.art), centred near the top. */
    if (player_art_ok()) {
        int aw = player_art_w(), ah = player_art_h();
        int ax = (LCD_WIDTH - aw) / 2;
        console_blit565(ax, 8, aw, ah, player_art_pixels());
    }

    /* Track title (extension already trimmed at collect time). */
    ui_text(14, 150, name, FONT_TITLE, LINEN_INK);

    /* Clock: elapsed left, total right. */
    char te[12], tt[12];
    fmt_time(te, elapsed_s);
    fmt_time(tt, total_s);
    ui_text(14, 174, te, FONT_SUB, LINEN_MUTED);
    int wtt = text_width(tt, FONT_SUB);
    ui_text(LCD_WIDTH - 14 - wtt, 174, tt, FONT_SUB, LINEN_MUTED);

    /* Thin accent progress bar. */
    int bx = 14, by = 182, bw = LCD_WIDTH - 28, bh = 3;
    console_fill_rect(bx, by, bw, bh, LINEN_BORDER);
    int fw = (total_s > 0) ? (int)((elapsed_s * (uint32_t)bw) / total_s) : 0;
    if (fw > bw) fw = bw;
    console_fill_rect(bx, by, fw, bh, LINEN_ACCENT);

    /* Small buffer-health readout, bottom-left ("buf NN"), + stop hint right. */
    char bs[12];
    uint32_t p = buf_pct > 99u ? 99u : buf_pct;
    bs[0] = 'b'; bs[1] = 'u'; bs[2] = 'f'; bs[3] = ' ';
    bs[4] = (char)('0' + (p / 10) % 10);
    bs[5] = (char)('0' + p % 10);
    bs[6] = '\0';
    ui_text(14, 208, bs, FONT_SMALL, p < 20u ? LINEN_ACCENT : LINEN_MUTED);
    const char *hint = "menu: back";
    int wh = text_width(hint, FONT_SMALL);
    ui_text(LCD_WIDTH - 14 - wh, 208, hint, FONT_SMALL, LINEN_MUTED);

    /* Volume overlay rides on top for ~1.5 s after a wheel adjustment. */
    if (mmio_read32(USEC_TIMER_ADDR) < g_vol_show_until) {
        volume_overlay_render(g_volume);
    }
}

/* Now-playing animated strip: the album art (y 8..128) is static for a track,
 * so we full-present it once then partial-present only this lower band each
 * second — shrinking the IRQ-masked pixel push (relies on the BCM persisting
 * the un-repainted art region across a partial LCD_UPDATE). */
#define NP_ANIM_Y 128
#define NP_ANIM_H 112

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
    { "Settings",    0 },
    { "Now Playing", 1 },
};
static int g_main_sel;

/* Music sub-menu. ACTIVE: Albums (enters the junk-filtered folder browser). */
enum { MU_PLAYLISTS, MU_ARTISTS, MU_ALBUMS, MU_SONGS, MU_GENRES, MU_COMPOSERS,
       MU_AUDIOBOOKS, MU_COUNT };
static const menu_item_t g_music_menu[MU_COUNT] = {
    { "Playlists",  0 },
    { "Artists",    1 },
    { "Albums",     1 },
    { "Songs",      0 },
    { "Genres",     0 },
    { "Composers",  0 },
    { "Audiobooks", 0 },
};
static int g_music_sel;

/* Shared wheel accumulator for the menu screens. */
static int g_menu_accum;

/* Generic menu renderer over a {label, active} list. `back` draws the header
 * back chevron (off for the root menu). */
static void menu_render_list(const char *title, const menu_item_t *items,
                             int n, int sel, int back)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    header_render(title, "", back);
    for (int i = 0; i < n && i < LIST_ROWS; i++) {
        list_row(i, items[i].label, 0, 0, 1 /*chevron*/, i == sel,
                 !items[i].active /*greyed*/, 0);
    }
}

static void main_menu_render(void)
{
    g_main_menu[MM_NOWPLAYING].active = (uint8_t)player_active();
    menu_render_list("Core", g_main_menu, MM_COUNT, g_main_sel, 0);
}

static void music_menu_render(void)
{
    menu_render_list("Music", g_music_menu, MU_COUNT, g_music_sel, 1);
}

/* ---------------------------------------------------------------------------
 * Screen stack
 * ------------------------------------------------------------------------- */
typedef enum { SCR_MENU, SCR_MUSIC, SCR_ARTISTS, SCR_BROWSER, SCR_NOWPLAYING,
               SCR_CHARGING } screen_t;
#define SCR_STACK_MAX 8
static screen_t g_scr[SCR_STACK_MAX];
static int      g_scr_n;

static void      scr_push(screen_t s) { if (g_scr_n < SCR_STACK_MAX) g_scr[g_scr_n++] = s; }
static void      scr_pop(void)        { if (g_scr_n > 1) g_scr_n--; }
static screen_t  scr_cur(void)        { return g_scr[g_scr_n - 1]; }

/* Browser view state (was local to the old browse loop). */
static int g_br_sel, g_br_accum;

/* (Re)enumerate the directory at cluster `dir_clus` into g_browse. */
static void browse_load(fat32_t *fs, uint32_t dir_clus)
{
    g_cur_dir  = dir_clus;
    g_browse_n = 0;
    g_art_clus = 0;                      /* re-captured by browse_collect below */
    g_art_size = 0;
    fat32_readdir(fs, dir_clus, browse_collect, 0);
}

/* After entering an album folder: load its hero art + count its playable tracks
 * (for the detail view's "N tracks" line). */
static void detail_load_meta(fat32_t *fs)
{
    detail_art_load(fs);
    g_album_track_n = 0;
    for (int i = 0; i < g_browse_n; i++) {
        if (!g_browse[i].is_dir) g_album_track_n++;
    }
}

/* Apply a wheel event to a selection index in [0, count) with acceleration. */
static int wheel_move(int sel, int count, int8_t delta, int *accum)
{
    int wd = delta;
    if (wd >  WHEEL_MAX_DELTA) wd =  WHEEL_MAX_DELTA;
    if (wd < -WHEEL_MAX_DELTA) wd = -WHEEL_MAX_DELTA;
    *accum += wd;
    int move = *accum / WHEEL_CLICKS_PER_ITEM;
    *accum -= move * WHEEL_CLICKS_PER_ITEM;
    sel += move;
    if (sel < 0)          sel = 0;
    if (sel >= count)     sel = count - 1;
    return sel;
}

/* Render whatever screen is on top of the stack into the framebuffer (no
 * present) — used to paint context behind the lock/unlock plate. */
static void paint_current_screen(void)
{
    switch (scr_cur()) {
    case SCR_MENU:    main_menu_render();  break;
    case SCR_MUSIC:   music_menu_render(); break;
    case SCR_ARTISTS: artists_render(g_artist_sel); break;
    case SCR_BROWSER: browse_render(g_br_sel); break;
    case SCR_NOWPLAYING:
        nowplaying_render(player_track_name(), player_elapsed_s(),
                          player_total_s(), player_buf_pct());
        break;
    case SCR_CHARGING:
        screen_charging_render(g_bat_pct, power_is_charging(), power_is_external());
        break;
    }
}

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
    chip_placeholder_init();
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
    g_scr_n = 0;
    scr_push(SCR_MENU);

    int      dirty = 1;
    uint32_t np_last = 0xFFFFFFFFu;
    int      np_first = 1;
    int      np_vol_prev = 0;            /* volume overlay was up last NP paint  */
    uint32_t last_present = 0;           /* rate-limit UI presents while playing */
    uint32_t last_bars = 0;              /* rate-limit the now-playing bar anim  */
    uint32_t last_chip = 0;              /* rate-limit album-cover chip loads    */
    int      hold_prev = clickwheel_hold() ? 1 : 0;  /* seed hold-edge detect    */
    int      ext_prev  = power_is_external() ? 1 : 0; /* seed plug-in edge detect */
    int      lock_flashing = 0;          /* a lock/unlock plate is on screen     */
    g_locked = hold_prev;

    /* Backlight inactivity: full -> dim -> off. Any input wakes to full; a press
     * that wakes from fully-OFF is swallowed (it just lights the screen, the way
     * a real iPod's first touch does). Playback keeps running the whole time. */
    enum { BL_OFF, BL_DIM, BL_FULL };
    int      bl_state   = BL_FULL;
    uint32_t last_input = mmio_read32(USEC_TIMER_ADDR);
    const uint32_t BL_DIM_US = 15u * 1000000u;
    const uint32_t BL_OFF_US = 30u * 1000000u;

    const char *last_tn = 0;              /* re-apply volume on track change       */
    for (;;) {
        player_pump();
        if (battery_refresh(0) && scr_cur() == SCR_CHARGING) {
            dirty = 1;                    /* refresh the % on the charging screen  */
        }

        /* Charging screen: pop up on a plug-IN edge (not if already powered at
         * boot, so it won't spuriously appear), auto-dismiss when unplugged. Any
         * button also dismisses it (handled in the input switch below). */
        int ext = power_is_external() ? 1 : 0;
        if (ext && !ext_prev && scr_cur() != SCR_CHARGING) {
            scr_push(SCR_CHARGING);
            dirty = 1;
        } else if (!ext && scr_cur() == SCR_CHARGING) {
            scr_pop();
            dirty = 1;
        }
        ext_prev = ext;

        /* Auto-advance re-inits the codec (resetting its gain), so re-apply our
         * volume whenever the playing track changes. */
        const char *tn = player_active() ? player_track_name() : 0;
        if (tn && tn != last_tn) {
            hal_volume_set(g_volume);
        }
        last_tn = tn;

        /* Hold-switch edge (a cheap GPIO read, independent of the wheel block
         * which is gated off while held): flash the lock/unlock plate and toggle
         * the input lock. Playback is untouched. */
        int held = clickwheel_hold() ? 1 : 0;
        if (held != hold_prev) {
            hold_prev = held;
            g_locked  = held;
            g_lock_flash_until = mmio_read32(USEC_TIMER_ADDR) + 1000000u;
            last_input = mmio_read32(USEC_TIMER_ADDR);   /* wake the backlight    */
            if (bl_state != BL_FULL) {
                backlight_set(BACKLIGHT_MAX);
                bl_state = BL_FULL;
            }
            dirty = 1;
        }

        wheel_event_t ev;
        if (!g_locked && clickwheel_poll(&ev)) {
            last_input = mmio_read32(USEC_TIMER_ADDR);
            if (bl_state != BL_FULL) {
                int was_off = (bl_state == BL_OFF);
                backlight_set(BACKLIGHT_MAX);
                bl_state = BL_FULL;
                dirty = 1;                    /* repaint anything drawn while off */
                if (was_off) {                /* swallow the wake press */
                    ev.buttons = 0;
                    ev.wheel_delta = 0;
                }
            }
            switch (scr_cur()) {
            case SCR_MENU:
                if (ev.wheel_delta) {
                    g_main_sel = wheel_move(g_main_sel, MM_COUNT,
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
                        g_dir_depth = 0;
                        g_artist_filter[0] = '\0';     /* all albums             */
                        browse_load(fs, fs->root_clus);
                        albumlist_queue_chips();       /* start loading covers   */
                        g_br_sel = g_br_accum = 0;
                        scr_push(SCR_BROWSER);
                    } else if (g_music_sel == MU_ARTISTS) {
                        g_artist_filter[0] = '\0';     /* load ALL to derive list */
                        g_dir_depth = 0;
                        browse_load(fs, fs->root_clus);
                        build_artists();
                        g_artist_sel = g_artist_accum = 0;
                        scr_push(SCR_ARTISTS);
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
                    for (; g_artists[g_artist_sel][k] && k < NAME_MAX; k++) {
                        g_artist_filter[k] = g_artists[g_artist_sel][k];
                    }
                    g_artist_filter[k] = '\0';
                    g_dir_depth = 0;
                    browse_load(fs, fs->root_clus);
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

            case SCR_BROWSER:
                if (ev.wheel_delta && g_browse_n > 0) {
                    g_br_sel = wheel_move(g_br_sel, g_browse_n,
                                          ev.wheel_delta, &g_br_accum);
                    dirty = 1;                    /* window derived at paint time  */
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_browse_n > 0) {
                    if (g_browse[g_br_sel].is_dir) {
                        if (g_dir_depth < DIR_STACK_MAX) {
                            /* Capture the album/artist BEFORE browse_load
                             * overwrites g_browse, then load tracklist + art. */
                            split_artist_album(g_browse[g_br_sel].name,
                                               g_album_artist, g_album_title);
                            g_dir_stack[g_dir_depth++] = g_cur_dir;
                            browse_load(fs, g_browse[g_br_sel].clus);
                            detail_load_meta(fs);
                            g_br_sel = g_br_accum = 0;
                        }
                    } else {
                        player_play_queue(g_browse, g_browse_n, g_br_sel,
                                          g_art_clus, g_art_size);
                        hal_volume_set(g_volume);  /* re-apply over codec re-init */
                        scr_push(SCR_NOWPLAYING);
                        np_first = 1;
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_dir_depth > 0) {              /* up a directory */
                        browse_load(fs, g_dir_stack[--g_dir_depth]);
                        if (g_dir_depth > 0) {
                            detail_load_meta(fs);       /* re-enter parent album  */
                        } else {
                            albumlist_queue_chips();    /* back at the album list */
                        }
                        g_br_sel = g_br_accum = 0;
                    } else {                            /* back to the music menu */
                        scr_pop();
                    }
                    dirty = 1;
                }
                break;

            case SCR_NOWPLAYING:
                if (ev.wheel_delta) {                    /* wheel = volume        */
                    /* 1% per detent (clamp a fast flick so it can't teleport). */
                    int d = ev.wheel_delta;
                    if (d >  WHEEL_MAX_DELTA) d =  WHEEL_MAX_DELTA;
                    if (d < -WHEEL_MAX_DELTA) d = -WHEEL_MAX_DELTA;
                    g_volume += d;
                    if (g_volume < 0)   g_volume = 0;
                    if (g_volume > 100) g_volume = 100;
                    hal_volume_set(g_volume);
                    g_vol_show_until = mmio_read32(USEC_TIMER_ADDR) + 1500000u;
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back, keep playing */
                    dirty = 1;
                }
                break;

            case SCR_CHARGING:
                if (ev.buttons) {                       /* any press dismisses */
                    scr_pop();
                    dirty = 1;
                }
                break;
            }
        }

        /* Idle-timeout backlight: dim, then off. Playback keeps running. */
        uint32_t idle = mmio_read32(USEC_TIMER_ADDR) - last_input;
        if (bl_state == BL_FULL && idle > BL_DIM_US) {
            backlight_set(BACKLIGHT_MAX / 4);
            bl_state = BL_DIM;
        } else if (bl_state == BL_DIM && idle > BL_OFF_US) {
            backlight_set(0);
            bl_state = BL_OFF;
        }

        /* Lock/unlock plate takes over the screen for ~1s on a Hold edge. Paint
         * the context + plate once, hold it, then repaint underneath when it
         * fades. Suppresses the normal render while up. */
        uint32_t now_us = mmio_read32(USEC_TIMER_ADDR);
        if (now_us < g_lock_flash_until) {
            if (!lock_flashing && bl_state != BL_OFF) {
                paint_current_screen();
                lock_plate_render(g_locked);
                lcd_present_fb(console_framebuffer());
                dirty = 0;
            }
            lock_flashing = 1;
            /* Only throttle when idle; keep audio paced while playing. */
            if (!player_active()) {
                for (volatile uint32_t d = 0; d < (1u << 15); d++) { }
            }
            continue;                     /* skip the normal render this pass      */
        }
        if (lock_flashing) {              /* plate just faded: repaint underneath  */
            lock_flashing = 0;
            dirty = 1;
        }

        /* Render the current screen (skipped entirely when the screen is off —
         * saves the IRQ-masked present while music plays dark). Now Playing
         * repaints ~1/s for the clock; menus/browser only on change. */
        if (bl_state == BL_OFF) {
            /* nothing to draw */
        } else if (scr_cur() == SCR_NOWPLAYING) {
            uint32_t nowv    = mmio_read32(USEC_TIMER_ADDR);
            uint32_t elapsed = player_elapsed_s();
            int vol_active = nowv < g_vol_show_until;
            int expiring   = np_vol_prev && !vol_active;   /* overlay just faded */

            /* A FULL present is needed only on a real change (dirty) or to erase
             * the fading volume overlay (it straddles the static-art band, so a
             * partial present can't clear it). Cap those to ~6fps while playing:
             * back-to-back full-frame, IRQ-masked pixel pushes were starving the
             * audio DMA ISR (BUF dropping into the red on a volume sweep). The
             * once-a-second clock rides the cheap partial present, unthrottled. */
            int want_full = dirty || expiring;
            if (want_full) {
                if (np_first || (uint32_t)(nowv - last_present) >= 150000u) {
                    nowplaying_render(player_track_name(), elapsed,
                                      player_total_s(), player_buf_pct());
                    lcd_present_fb(console_framebuffer());
                    np_first = 0;
                    np_last  = elapsed;
                    np_vol_prev  = vol_active;
                    last_present = nowv;
                    player_note_presented();
                    dirty = 0;
                }
                /* else: throttled — keep dirty set, present in the next window */
            } else if (elapsed != np_last) {
                nowplaying_render(player_track_name(), elapsed,
                                  player_total_s(), player_buf_pct());
                lcd_present_rect(console_framebuffer(),
                                 0, NP_ANIM_Y, LCD_WIDTH, NP_ANIM_H);
                np_last = elapsed;
                np_vol_prev = vol_active;
                player_note_presented();
            }
        } else if (dirty) {
            /* While a song plays, cap menu/browser repaints (~6 fps) so
             * back-to-back full-frame presents during rapid scrolling don't keep
             * IRQs masked long enough to starve the audio DMA ISR. Idle → present
             * immediately for a snappy UI. `dirty` stays set until we present, so
             * the latest scroll position is what lands. */
            uint32_t now = mmio_read32(USEC_TIMER_ADDR);
            if (!player_active() || (now - last_present) >= 150000u) {
                switch (scr_cur()) {
                case SCR_MENU:    main_menu_render();            break;
                case SCR_MUSIC:   music_menu_render();           break;
                case SCR_ARTISTS: artists_render(g_artist_sel);  break;
                case SCR_BROWSER: browse_render(g_br_sel);       break;
                case SCR_CHARGING:
                    screen_charging_render(g_bat_pct, power_is_charging(),
                                           power_is_external());
                    break;
                default: break;                 /* NOWPLAYING handled above */
                }
                lcd_present_fb(console_framebuffer());
                dirty = 0;
                last_present = now;
            }
        }

        /* Animate the now-playing 3-bar indicator in the album detail: redraw
         * only its ~10px box and partial-present just that, ~9fps. A tiny pixel
         * push (unlike a full repaint) so it can't starve the audio DMA. */
        if (bl_state != BL_OFF && scr_cur() == SCR_BROWSER && g_dir_depth > 0
            && player_active()) {
            uint32_t nowb = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowb - last_bars) >= 110000u) {
                int top = scroll_window(g_br_sel, g_browse_n, DET_ROWS);
                const char *pn = player_track_name();
                for (int r = 0; r < DET_ROWS; r++) {
                    int idx = top + r;
                    if (idx >= g_browse_n) break;
                    const browse_entry_t *e = &g_browse[idx];
                    if (e->is_dir || !name_eq_ci(e->name, pn)) continue;
                    int ry = DET_LIST_Y0 + r * ROW_H;
                    int bx = LCD_WIDTH - 22, by = ry + 7;
                    int is_sel = (idx == g_br_sel);
                    console_fill_rect(bx, by, NP_BARS_W, NP_BARS_H,
                                      is_sel ? LINEN_SEL_BG : LINEN_SURFACE);
                    nowplaying_bars(bx, by, is_sel ? LINEN_SEL_FG : LINEN_INK, nowb);
                    lcd_present_rect(console_framebuffer(),
                                     bx, by, NP_BARS_W + 1, NP_BARS_H);
                    break;
                }
                last_bars = nowb;
            }
        }

        /* Load one album cover every ~120ms while on the album list (spreads the
         * chip I/O so it can't starve decode); repaint when one lands. */
        if (scr_cur() == SCR_BROWSER && g_dir_depth == 0) {
            uint32_t nowc = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowc - last_chip) >= 120000u) {
                if (artcache_pump(fs)) {
                    dirty = 1;
                }
                last_chip = nowc;
            }
        }

        /* Only throttle when idle; while playing, player_pump's decode_step
         * paces the loop and the wheel stays responsive. */
        if (!player_active()) {
            for (volatile uint32_t d = 0; d < (1u << 15); d++) {
            }
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

    uart_puts("core: tick ");
    uart_put_hex32(current_tick());
    uart_puts(" (pre-sleep)\n");
    sleep_ms(100);
    uart_puts("core: tick ");
    uart_put_hex32(current_tick());
    uart_puts(" (post-sleep, ~+10)\n");

    /* LCD probe gates the whole disk/audio/UI stack: nonzero BCM power => real
     * hardware. The clicky emulator has no BCM (lcd_init() false there), so
     * this block is skipped, keeping the emulator smoke green; the register
     * grammar is proven host-side by the mock-bus trace tests. */
    if (lcd_init()) {
        /* Boost to 80 MHz for the whole disk/decode path. The UI runs here too;
         * we never drop back because the browser never returns (fine for
         * bring-up — the device is on a cable during testing). */
        cpu_boost();

        /* Backlight to full (GPIO charge-pump dimmer) so the splash + UI are lit;
         * run_ui dims then turns it off after inactivity. */
        backlight_init();

        /* Bring up the I2C control bus now (not just at first-song hal_audio_init)
         * so the status strip can read the PCF50605 battery gauge from the menu.
         * Bounded/idempotent — hal_audio_init re-inits it harmlessly per track. */
        i2c_init();
        battery_init();
        hal_volume_init();               /* codec output gain -> safe default      */

        /* Paint the boot splash immediately, so the panel shows CORE branding
         * instead of the chainloader's leftover framebuffer (a blue field with
         * a green stripe) while the disk spins up and the volume mounts. */
        boot_splash();

        /* Read the MBR, find the FAT32 data partition (type 0B/0C), mount it. */
        uint8_t *mbr = (uint8_t *)mbr_sector;
        uint32_t sig = 0, fat_lba = 0;
        int      mnt = -1;
        fat32_t  fs;
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

        uint32_t btus = mmio_read32(USEC_TIMER_ADDR) - boot_us0;
        uart_puts("core: mount rc ");
        uart_put_hex32((uint32_t)mnt);
        uart_puts(" BTUS ");
        uart_put_hex32(btus);
        uart_putc('\n');

        if (mnt == 0) {
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
