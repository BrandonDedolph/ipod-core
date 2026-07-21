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
        browse_entry_t *b = &g_browse[g_browse_n++];
        copy_display_name(b->name, e->name, 0);   /* keep folder name as-is */
        b->clus   = e->first_clus;
        b->size   = 0;
        b->fmt    = 0;
        b->is_dir = 1;
        return 0;
    }

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

/* Cached battery/power readout. The gauge read is an I2C transaction (slow, and
 * shares the codec bus), so sample it a few seconds apart — NOT every present —
 * and hold the last value. mv/pct < 0 means the read failed / not yet sampled. */
static int      g_bat_mv  = -1;
static int      g_bat_pct = -1;
static int      g_bat_ext = 0;               /* external power present            */
static uint32_t g_bat_last_us;

static void battery_refresh(int force)
{
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    if (!force && (uint32_t)(now - g_bat_last_us) < 5000000u) {
        return;
    }
    g_bat_last_us = now;
    g_bat_mv  = battery_millivolts();
    g_bat_pct = battery_percent();
    g_bat_ext = power_is_external();
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

    /* Raw mV to the left of the glyph (calibration aid). */
    if (g_bat_mv > 0) {
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

/* One list row (menus.jsx Row). Selected = filled ink bar + light bold text;
 * greyed = muted (inactive menu item). Optional sub-line, right value, chevron,
 * and a leading 22x22 art chip (RGB565, or NULL). */
static void list_row(int r, const char *text, const char *sub, const char *right,
                     int chevron, int selected, int greyed, const uint16_t *chip)
{
    int ry = LIST_Y0 + r * ROW_H;
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

/* Slim right-edge scrollbar (menus.jsx Scrollbar); no-op when everything fits. */
static void scrollbar_render(int top, int visible, int total)
{
    if (total <= visible) {
        return;
    }
    int track_y = LIST_Y0;
    int track_h = LCD_HEIGHT - LIST_Y0 - 4;
    console_fill_rect(LCD_WIDTH - 4, track_y, 3, track_h, LINEN_SB_TRK);
    int thumb_h = (visible * track_h) / total;
    if (thumb_h < 16) thumb_h = 16;
    int denom = total - visible;
    if (denom < 1) denom = 1;
    int thumb_y = track_y + (top * (track_h - thumb_h)) / denom;
    console_fill_rect(LCD_WIDTH - 4, thumb_y, 3, thumb_h, LINEN_SB_THMB);
}

static void browse_render(int sel, int top)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();

    char right[12];
    if (g_browse_n > 0) {
        /* "sel+1 / n" position counter (design ScreenHeader right slot). */
        int a = sel + 1, b = g_browse_n, i = 0;
        char nb[6]; int t = 0, v = a;
        do { nb[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 5);
        while (t > 0) right[i++] = nb[--t];
        right[i++] = ' '; right[i++] = '/'; right[i++] = ' ';
        t = 0; v = b;
        do { nb[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 5);
        while (t > 0) right[i++] = nb[--t];
        right[i] = '\0';
    } else {
        right[0] = '\0';
    }
    header_render(g_dir_depth > 0 ? "Folder" : "Albums", right, 1);

    if (g_browse_n == 0) {
        ui_text(14, LIST_Y0 + 20, "Empty folder", FONT_ROW, LINEN_MUTED);
        return;
    }
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_browse_n) break;
        const browse_entry_t *e = &g_browse[idx];
        /* Folders get a chevron; tracks are bare (no per-track duration yet). */
        list_row(r, e->name, 0, 0, e->is_dir, idx == sel, 0, 0);
    }
    scrollbar_render(top, LIST_ROWS, g_browse_n);
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
    { "Artists",    0 },
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
typedef enum { SCR_MENU, SCR_MUSIC, SCR_BROWSER, SCR_NOWPLAYING } screen_t;
#define SCR_STACK_MAX 8
static screen_t g_scr[SCR_STACK_MAX];
static int      g_scr_n;

static void      scr_push(screen_t s) { if (g_scr_n < SCR_STACK_MAX) g_scr[g_scr_n++] = s; }
static void      scr_pop(void)        { if (g_scr_n > 1) g_scr_n--; }
static screen_t  scr_cur(void)        { return g_scr[g_scr_n - 1]; }

/* Browser view state (was local to the old browse loop). */
static int g_br_sel, g_br_top, g_br_accum;

/* (Re)enumerate the directory at cluster `dir_clus` into g_browse. */
static void browse_load(fat32_t *fs, uint32_t dir_clus)
{
    g_cur_dir  = dir_clus;
    g_browse_n = 0;
    g_art_clus = 0;                      /* re-captured by browse_collect below */
    g_art_size = 0;
    fat32_readdir(fs, dir_clus, browse_collect, 0);
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
    battery_refresh(1);                   /* prime the status-strip gauge         */
    g_dir_depth = 0;
    g_browse_n  = 0;
    g_br_sel = g_br_top = g_br_accum = 0;
    g_main_sel  = 0;
    g_music_sel = MU_ALBUMS;
    g_menu_accum = 0;
    g_scr_n = 0;
    scr_push(SCR_MENU);

    int      dirty = 1;
    uint32_t np_last = 0xFFFFFFFFu;
    int      np_first = 1;
    uint32_t last_present = 0;           /* rate-limit UI presents while playing */

    /* Backlight inactivity: full -> dim -> off. Any input wakes to full; a press
     * that wakes from fully-OFF is swallowed (it just lights the screen, the way
     * a real iPod's first touch does). Playback keeps running the whole time. */
    enum { BL_OFF, BL_DIM, BL_FULL };
    int      bl_state   = BL_FULL;
    uint32_t last_input = mmio_read32(USEC_TIMER_ADDR);
    const uint32_t BL_DIM_US = 15u * 1000000u;
    const uint32_t BL_OFF_US = 30u * 1000000u;

    for (;;) {
        player_pump();
        battery_refresh(0);               /* self-throttled ~5s gauge resample     */

        wheel_event_t ev;
        if (clickwheel_poll(&ev)) {
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
                        browse_load(fs, fs->root_clus);
                        g_br_sel = g_br_top = g_br_accum = 0;
                        scr_push(SCR_BROWSER);
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

            case SCR_BROWSER:
                if (ev.wheel_delta && g_browse_n > 0) {
                    g_br_sel = wheel_move(g_br_sel, g_browse_n,
                                          ev.wheel_delta, &g_br_accum);
                    if (g_br_sel < g_br_top)               g_br_top = g_br_sel;
                    if (g_br_sel >= g_br_top + LIST_ROWS)  g_br_top = g_br_sel - (LIST_ROWS - 1);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_browse_n > 0) {
                    if (g_browse[g_br_sel].is_dir) {
                        if (g_dir_depth < DIR_STACK_MAX) {
                            g_dir_stack[g_dir_depth++] = g_cur_dir;
                            browse_load(fs, g_browse[g_br_sel].clus);
                            g_br_sel = g_br_top = g_br_accum = 0;
                        }
                    } else {
                        player_play_queue(g_browse, g_browse_n, g_br_sel,
                                          g_art_clus, g_art_size);
                        scr_push(SCR_NOWPLAYING);
                        np_first = 1;
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_dir_depth > 0) {              /* up a directory */
                        browse_load(fs, g_dir_stack[--g_dir_depth]);
                        g_br_sel = g_br_top = g_br_accum = 0;
                    } else {                            /* back to the music menu */
                        scr_pop();
                    }
                    dirty = 1;
                }
                break;

            case SCR_NOWPLAYING:
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back, keep playing */
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

        /* Render the current screen (skipped entirely when the screen is off —
         * saves the IRQ-masked present while music plays dark). Now Playing
         * repaints ~1/s for the clock; menus/browser only on change. */
        if (bl_state == BL_OFF) {
            /* nothing to draw */
        } else if (scr_cur() == SCR_NOWPLAYING) {
            uint32_t elapsed = player_elapsed_s();
            if (dirty || elapsed != np_last) {
                nowplaying_render(player_track_name(), elapsed,
                                  player_total_s(), player_buf_pct());
                if (np_first || dirty) {
                    lcd_present_fb(console_framebuffer());
                    np_first = 0;
                } else {
                    lcd_present_rect(console_framebuffer(),
                                     0, NP_ANIM_Y, LCD_WIDTH, NP_ANIM_H);
                }
                np_last = elapsed;
                player_note_presented();
                dirty = 0;
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
                case SCR_BROWSER: browse_render(g_br_sel, g_br_top); break;
                default: break;                 /* NOWPLAYING handled above */
                }
                lcd_present_fb(console_framebuffer());
                dirty = 0;
                last_present = now;
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
