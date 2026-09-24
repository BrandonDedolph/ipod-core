/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/screen_settings.c — Settings screens rendered in the Linen system.
 *
 * Draws the data-driven model in settings.c (settings_count / _label / _kind /
 * _value) into the 320x240 RGB565 console framebuffer, matching
 * design_reference/menus.jsx (SettingsMenu / SettingsPlayback / SettingsSound /
 * SettingsAbout) and system-screens.jsx (ThemePicker). Uses the same list
 * chrome, palette, fonts and geometry as kernel/main.c (LIST_Y0 42, ROW_H 24,
 * dark-ink selection bar with light text), replicated locally as tiny helpers
 * so this stays a self-contained module (main.c is not edited).
 *
 * These painters leave the top band (rows 0..STATUS_H-1) clear — the jsx
 * Settings screens carry no status strip of their own — and place the header at
 * main.c's header baseline, so the header stays put when you cross from the
 * main menu into Settings. The firmware paints the ordinary status strip (track
 * name, battery, Hold padlock) over that clear band afterwards, in main.c's
 * settings_render_cur(), so Settings carries the same strip as the lists.
 *
 * Pure rendering: integer-only, no libc/libm/malloc, no hardware access, no
 * present. The caller hands the framebuffer to lcd_present_fb().
 */

#include "settings.h"
#include "settime.h"                          /* the Date & Time editor model */
#include "screen_charging.h"                   /* CHG_FULL_PCT, shared with the modal */
#include "chrome.h"
#include "palette.h"                          /* live theme palette (g_pal[]) */

#include "text.h"
#include "../kernel/console.h"
#include "../hal/hal.h"                        /* LCD_WIDTH / LCD_HEIGHT */

/* ---------------------------------------------------------------------------
 * Palette tokens — now resolve to the live, theme-selected g_pal[] (shared with
 * kernel/main.c via ui/palette.h) so the Settings screens follow Linen / Onyx
 * like the rest of the UI. SEL_BG/SEL_FG stay derived from INK/SURFACE.
 * ------------------------------------------------------------------------- */
#define S_SURFACE  g_pal[PAL_SURFACE]
#define S_INK      g_pal[PAL_INK]
#define S_MUTED    g_pal[PAL_MUTED]
#define S_MUTED2   g_pal[PAL_MUTED2]
#define S_MUTED_D  g_pal[PAL_MUTED_D]
#define S_ACCENT   g_pal[PAL_ACCENT]
#define S_BORDER   g_pal[PAL_BORDER]
#define S_SEL_BG   g_pal[PAL_INK]      /* selection bar = ink (inverts w/ theme) */
#define S_SEL_FG   g_pal[PAL_SURFACE]  /* selection text = surface               */
#define S_SEL_SUB  g_pal[PAL_SEL_SUB]
#define S_CHEVRON  g_pal[PAL_CHEVRON]
#define S_TRK      g_pal[PAL_TRK]      /* slider track                           */
#define S_SB_TRK   g_pal[PAL_SB_TRK]   /* scrollbar track                        */
#define S_SB_THMB  g_pal[PAL_SB_THMB]  /* scrollbar thumb                        */
/* Warning red. Same literal as kernel/main.c's BATT_LOW_RED — deliberately a
 * fixed colour and not a palette slot: it must stay legible as a WARNING in
 * every theme, which is exactly what a themed palette entry would not. */
#define S_WARN     0xE125u
#define S_SEL_TRK  g_pal[PAL_SEL_TRK]  /* slider track on a selected row         */
#define S_PILL_OFF g_pal[PAL_PILL_OFF] /* toggle pill OFF fill                   */
#define S_PLATE_C  g_pal[PAL_PLATE]    /* raised plate (About's gauge cards)     */

/* Nunito faces (see ui/text.h). */
#define F_BIG    text_font_bold_18()
#define F_HEADER text_font_bold_13()
#define F_ROW    text_font_regular_12()
#define F_SUB    text_font_regular_11()
#define F_SMALL  text_font_regular_9()

/* Geometry comes from ui/chrome.h — the single definition every screen shares.
 * These were duplicated here as a "matched copy", and the copy had already
 * fallen behind: the taller two-line row work (ROW_H2/LIST_ROWS2) landed only
 * in main.c, so Settings kept scrolling with the old feel. */
#define TH_ROW_H   39                          /* five rows fill the page: (240-42)/39 = 5 */

/* ---------------------------------------------------------------------------
 * Tiny draw helpers (matched copies of main.c's, kept local on purpose)
 * ------------------------------------------------------------------------- */

/* Draw a NUL-terminated string at baseline `y`; returns the advance. */
static int st_text(int x, int y, const char *s, const text_font_t *font,
                   uint16_t ink)
{
    return text_draw(console_fb(), LCD_WIDTH, LCD_HEIGHT, x, y, s, font, ink);
}

/* Right-align `s` so it ends at x = LCD_WIDTH - `pad`. */
static void st_text_right(int pad, int y, const char *s,
                          const text_font_t *font, uint16_t ink)
{
    int w = text_width(s, font);
    st_text(LCD_WIDTH - pad - w, y, s, font, ink);
}

/* The dark-ink selection bar behind row `r` (menus.jsx selected Row, radius 4). */
static void sel_bar(int y0, int rowh, int r)
{
    int ry = y0 + r * rowh;
    ui_round_rect(6, ry + 1, LCD_WIDTH - 16, rowh - 2, 4, S_SEL_BG);
}

/* Toggle pill (menus.jsx SettingsPlayback toggle): a 22x12 track with a 9x9
 * knob that sits right when on, left when off. Colours flip on a selected row. */
static void draw_toggle(int ry, int selected, int on)
{
    const int pw = 22, ph = 12;
    int px = LCD_WIDTH - 16 - pw;
    int py = ry + (ROW_H - ph) / 2;

    uint16_t track = on ? (selected ? S_SEL_FG : S_INK)
                        : (selected ? S_SEL_SUB : S_PILL_OFF);
    uint16_t knob  = on ? (selected ? S_INK : S_SURFACE)
                        : (selected ? S_SURFACE : S_SURFACE);
    ui_round_rect(px, py, pw, ph, ph / 2, track);      /* rounded pill end-caps */
    int kx = on ? px + pw - 2 - 9 : px + 2;
    ui_round_rect(kx, py + 2, 9, 9, 4, knob);          /* round knob            */
}

/* Slider fill bar for a value fraction num/den (menus.jsx SettingsSound bar):
 * a full-width track with a proportional fill, colours flipping when selected.
 * A locked row (settings_row_locked — Bass/Treble under an EQ preset) keeps
 * its bar, because the number it shows is real, but draws it in the muted
 * tone the label and value use so it reads as state rather than as a control. */
static void draw_slider(int ry, int selected, int locked, int num, int den)
{
    int bx = 14, bw = LCD_WIDTH - 16 - bx, by = ry + 17, bh = 3;
    uint16_t trackc = selected ? S_SEL_TRK : S_TRK;
    uint16_t fillc  = locked   ? (selected ? S_SEL_SUB : S_MUTED2)
                               : (selected ? S_SEL_FG  : S_INK);
    console_fill_rect(bx, by, bw, bh, trackc);
    if (den > 0) {
        int fw = bw * num / den;
        if (fw < 0) fw = 0;
        if (fw > bw) fw = bw;
        console_fill_rect(bx, by, fw, bh, fillc);
    }
}

/* ---------------------------------------------------------------------------
 * Generic list screens (Root / Playback / Sound / Display)
 * ------------------------------------------------------------------------- */
/*
 * Windowed-list scroll origin and scrollbar. Deliberately the same geometry
 * and the same palette entries as the browser's in kernel/main.c (which owns
 * the originals and cannot be called from here — this module is freestanding
 * and has no dependency on main.c). Keep the two in step: a scrollbar that
 * sits 1px off, or a window that keeps the selection somewhere else, reads as
 * a different list widget on the same device.
 */
#define LIST_ROWS  8                   /* visible rows: (240-42)/24 ~= 8 */

static int st_scroll_window(int sel, int total, int visible)
{
    if (total <= visible) return 0;
    int start = sel - visible / 3;     /* selection sits ~1/3 down */
    if (start < 0) start = 0;
    if (start > total - visible) start = total - visible;
    return start;
}

static void st_scrollbar(int y0, int top, int visible, int total)
{
    if (total <= visible) {
        return;                        /* everything fits: no bar at all */
    }
    int track_y = y0;
    int track_h = LCD_HEIGHT - y0 - 4;
    console_fill_rect(LCD_WIDTH - 4, track_y, 3, track_h, S_SB_TRK);
    int thumb_h = (visible * track_h) / total;
    if (thumb_h < 16) thumb_h = 16;
    int denom = total - visible;
    if (denom < 1) denom = 1;
    int thumb_y = track_y + (top * (track_h - thumb_h)) / denom;
    console_fill_rect(LCD_WIDTH - 4, thumb_y, 3, thumb_h, S_SB_THMB);
}

static void list_render(int screen, const settings_t *s, int sel)
{
    int n   = settings_count(screen);
    /* Every row is ROW_H tall whatever its kind — a slider just puts its bar
     * at ry+17 inside that same row — so the window maths is row-count based
     * and needs no per-kind arithmetic. Sound is the tallest screen at six
     * rows and still fits in the eight-row window. */
    int top = st_scroll_window(sel, n, LIST_ROWS);
    int end = top + LIST_ROWS;
    if (end > n) end = n;
    st_scrollbar(LIST_Y0, top, LIST_ROWS, n);
    for (int r = top; r < end; r++) {
        int vr = r - top;                      /* visible row index */
        int ry = LIST_Y0 + vr * ROW_H;
        int is_sel = (r == sel);
        if (is_sel) {
            sel_bar(LIST_Y0, ROW_H, vr);
        }

        int kind = settings_kind(screen, r);
        char buf[24];
        int is_toggle = 0, on = 0, num = 0, den = 0;
        settings_value(screen, s, r, buf, &is_toggle, &on, &num, &den);

        /* A locked row is drawn, and drawn greyed: it is explaining state
         * (an EQ preset owns this shelf), which is exactly what a row the
         * user cannot move has to do to earn its place on the screen. */
        int locked = settings_row_locked(screen, s, r);

        uint16_t fg     = is_sel ? S_SEL_FG  : S_INK;
        uint16_t rightc = is_sel ? S_SEL_SUB : S_MUTED_D;
        uint16_t chevc  = is_sel ? S_SEL_SUB : S_CHEVRON;
        if (locked) {
            fg = rightc = is_sel ? S_SEL_SUB : S_MUTED2;
        }

        if (kind == SETTINGS_KIND_SLIDER) {
            /* label + right value lifted to make room for the bar below. */
            st_text(14, ry + 11, settings_label(screen, r),
                    is_sel ? F_HEADER : F_ROW, fg);
            if (buf[0]) {
                st_text_right(16, ry + 11, buf, F_SUB, rightc);
            }
            draw_slider(ry, is_sel, locked, num, den);
            continue;
        }

        /* Non-slider rows: label centred in the row height. */
        st_text(14, ry + 15, settings_label(screen, r),
                is_sel ? F_HEADER : F_ROW, fg);

        if (is_toggle) {
            draw_toggle(ry, is_sel, on);
        } else if (buf[0]) {
            st_text_right(16, ry + 15, buf, F_SUB, rightc);
        } else {
            /* submenu / action with no value -> chevron. */
            st_text(LCD_WIDTH - 18, ry + 15, UI_GLYPH_RAQUO, F_ROW, chevc);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Theme picker (system-screens.jsx ThemePicker): swatch + name + sub, with a
 * "CURRENT" tag on the active theme.
 * ------------------------------------------------------------------------- */
/* Preview tones are each theme's OWN colours — read from that theme's source
 * table (palette.h theme_table), NOT the live g_pal[] — so a row previews the
 * theme it would switch to and can never drift from what theme_set() loads.
 * Only the one-line blurbs live here, indexed by the THEME_* id. */
static const char *const TH_SUB[THEME_COUNT] = {
    [THEME_LINEN]    = "Warm light - text-forward",
    [THEME_ONYX]     = "Warm dark - terracotta",
    [THEME_SAGE]     = "Dark green-grey - clay",
    [THEME_PLASTER]  = "Pink-beige limewash - oxblood",
    [THEME_OLIVE]    = "Greige-olive - burnt ochre",
    [THEME_UMBER]    = "Espresso - caramel",
    [THEME_MUSHROOM] = "Warm greige - muted rust",
};

/* Seven 39 px rows do not fit under the header ((240-42)/39 = 5), so the
 * picker scrolls through the same window + scrollbar as every other list. */
#define TH_ROWS    ((LCD_HEIGHT - LIST_Y0) / TH_ROW_H)

/* The header's right-hand count. A literal (no snprintf here), pinned to the
 * id list so adding a theme without updating it fails to build. */
#define TH_COUNT_STR "7 themes"
_Static_assert(THEME_COUNT == 7, "update TH_COUNT_STR and TH_SUB[]");

static void theme_render(const settings_t *s, int sel)
{
    int n   = settings_count(SETTINGS_THEME);
    int top = st_scroll_window(sel, n, TH_ROWS);
    st_scrollbar(LIST_Y0, top, TH_ROWS, n);
    for (int vr = 0; vr < TH_ROWS; vr++) {
        int r = top + vr;
        if (r >= n) {
            break;
        }
        int ry = LIST_Y0 + vr * TH_ROW_H;
        int is_sel = (r == sel);
        if (is_sel) {
            sel_bar(LIST_Y0, TH_ROW_H, vr);
        }

        /* Swatch tile with a border + a small "text" hint bar. */
        int sw = 26, sx = 14, sy = ry + (TH_ROW_H - sw) / 2;
        console_fill_rect(sx - 1, sy - 1, sw + 2, sw + 2,
                          is_sel ? S_SEL_SUB : S_BORDER);
        const uint16_t *tp = theme_table(r);   /* r < THEME_COUNT: never NULL */
        console_fill_rect(sx, sy, sw, sw, tp[PAL_SURFACE]);
        console_fill_rect(sx + 6, sy + 10, 14, 3, tp[PAL_INK]);
        console_fill_rect(sx + 6, sy + 16, 4, 4, tp[PAL_ACCENT]);

        int tx = sx + sw + 10;
        uint16_t fg   = is_sel ? S_SEL_FG  : S_INK;
        uint16_t subc = is_sel ? S_SEL_SUB : S_MUTED;
        st_text(tx, ry + 17, settings_theme_name(r), F_HEADER, fg);
        st_text(tx, ry + 31, TH_SUB[r], F_SMALL, subc);

        if (r == s->theme) {
            st_text_right(14, ry + 20, "CURRENT", F_SMALL,
                          is_sel ? S_SEL_FG : S_MUTED2);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public renderers
 * ------------------------------------------------------------------------- */
void settings_render(int screen, const settings_t *s, int sel)
{
    if (screen == SETTINGS_ABOUT) {
        /* main.c should call settings_about_render() with live values; this
         * placeholder path keeps settings_render total over every screen. */
        settings_about_render(-1, -1, -1, 0, 0xFFFFFFFFu, 0, 0, 0, 0, ABOUT_LOG_OFF,
                              0, "v0.0.0", NULL);
        return;
    }
    if (screen == SETTINGS_BATTERY) {
        settings_battery_render(s, sel, NULL);       /* same idea: dashes */
        return;
    }

    console_clear(S_SURFACE);

    /* The title is settings.c's (settings_title): a switch here had no case
     * for Date & Time and silently titled it "Settings". Only the Theme
     * picker's right-hand count is the renderer's own. */
    const char *right = (screen == SETTINGS_THEME) ? TH_COUNT_STR : "";
    ui_header(settings_title(screen), right, 1);

    if (screen == SETTINGS_THEME) {
        theme_render(s, sel);
    } else {
        list_render(screen, s, sel);
    }
}

/* ---------------------------------------------------------------------------
 * About (SettingsAbout): device info key/value rows from live values.
 * ------------------------------------------------------------------------- */

/* Local string copy + decimal writer (a separate TU from settings.c). */
static void su_copy(char *d, const char *s)
{
    while (*s) *d++ = *s++;
    *d = '\0';
}
static int su_to_str(char *d, unsigned v)
{
    char tmp[10];
    int t = 0;
    do { tmp[t++] = (char)('0' + v % 10u); v /= 10u; } while (v && t < 10);
    int i = 0;
    while (t > 0) d[i++] = tmp[--t];
    d[i] = '\0';
    return i;
}

/* Format whole megabytes as "W.F GB" (one decimal). */
static void fmt_gb(char *d, uint32_t mb)
{
    uint32_t whole = mb / 1024u;
    uint32_t frac  = (mb % 1024u) * 10u / 1024u;
    int i = su_to_str(d, whole);
    d[i++] = '.';
    d[i++] = (char)('0' + frac);
    d[i++] = ' ';
    d[i++] = 'G';
    d[i++] = 'B';
    d[i]   = '\0';
}

/* Append " word" to the NUL-terminated string in `d`. */
static void su_append(char *d, const char *w)
{
    int i = 0; while (d[i]) i++;
    su_copy(d + i, w);
}

/*
 * About — a small device dashboard.
 *
 *   ‹ About
 *   iPod 5.5G                                  [Core]
 *      4127          318           142
 *      SONGS        ALBUMS        ARTISTS
 *   ┌ STORAGE ──────────┐  ┌ BATTERY ──────────┐
 *   │ 21.0 GB free       │  │ 73%                │
 *   │ ▓▓▓▓▓▓▓▓▓░░░       │  │ [▓▓▓▓▓▓▓░░]▏       │
 *   │ 55.3 of 76.3 GB    │  │ 3912 mV            │
 *   └────────────────────┘  └────────────────────┘
 *          ADC 2731 · LOG 6 on · JACK 1 n4
 *
 * The first cut stacked STORAGE and BATTERY as two identical full-width
 * accent bars with their diagnostics jammed against the labels, and the
 * device name floated alone in the middle of the page. Now the name and the
 * firmware chip share one row, the two gauges sit side by side on raised
 * plates so each has room for its own caption, and every raw diagnostic
 * (ADC code, event log) lives on ONE muted footer line where it cannot be
 * mistaken for a design element — but stays on this screen, because "LOG
 * off" here is how the owner knows a night's crash will come with no log.
 */
#define AB_CARD_Y   142          /* both plates: top edge                     */
#define AB_CARD_H   80           /* ...and height (ends at 222)               */
#define AB_CARD_W   140          /* 16 | 140 | 8 | 140 | 16 = 320             */
#define AB_CARD_PAD 10           /* inset for the plate's text and bars       */

/* One raised plate with its small-caps label; returns the inner x. */
static int about_card(int x, const char *label)
{
    ui_round_rect(x, AB_CARD_Y, AB_CARD_W, AB_CARD_H, 6, g_pal[PAL_PLATE]);
    st_text(x + AB_CARD_PAD, AB_CARD_Y + 18, label, F_SMALL, S_MUTED);
    return x + AB_CARD_PAD;
}

void settings_about_render(int battery_pct, int battery_mv, int battery_raw,
                           uint32_t total_mb, uint32_t free_mb,
                           int n_songs, int n_albums, int n_artists,
                           uint32_t log_seq, int log_state, int lib_truncated,
                           const char *version, const about_jack_t *jack)
{
    console_clear(S_SURFACE);
    ui_header(settings_title(SETTINGS_ABOUT), "", 1);

    /* v holds the footer, which is the longest string this screen builds:
     * three tokens, the last of which can carry a pin-configuration tail. */
    char v[80], w[24];

    /* --- device row: name left, firmware chip right, one baseline ---
     * The chip is the release version: "Core v0.1.0", from the nearest git
     * tag (CORE_VERSION, passed in — this file is host-built and cannot see
     * the generated header). It is sized from text_width, so a longer version
     * simply widens it leftwards; "iPod 5.5G" ends at x=101 and even a chip
     * reading "Core v0.10.12" starts at x=211, so the row has ~110 px spare.
     * The build id (tag + commits + hash + -dirty) is the LONG string and
     * lives on the boot screen and in Boot Details, not here. */
    st_text(16, 66, "iPod 5.5G", F_BIG, S_INK);
    {
        char chip[32];
        su_copy(chip, "Core");
        if (version && version[0]) {
            int i = 4;
            chip[i++] = ' ';
            for (int j = 0; version[j] && i < (int)sizeof chip - 1; j++) {
                chip[i++] = version[j];
            }
            chip[i] = '\0';
        }
        int cw = text_width(chip, F_SUB);
        int chw = cw + 16, chx = LCD_WIDTH - 16 - chw, chy = 52;
        ui_round_rect(chx, chy, chw, 16, 8, S_INK);
        st_text(chx + 8, chy + 12, chip, F_SUB, S_SURFACE);
    }
    /* A capped load is the one thing the stat columns below cannot show on
     * their own: a library that hit LIB_MAX_* just looks smaller. Warning
     * red, deliberately outside the palette, in the slot under the name. */
    if (lib_truncated) {
        ui_text_centered(84, "Library too large " UI_GLYPH_MIDDOT
                             " some items not shown", F_SMALL, S_WARN);
    }

    /* --- three stat columns: Songs / Albums / Artists --- */
    const char *lbl[3] = { "SONGS", "ALBUMS", "ARTISTS" };
    int         val[3] = { n_songs, n_albums, n_artists };
    int colw = LCD_WIDTH / 3;
    for (int i = 0; i < 3; i++) {
        int cx = colw * i + colw / 2;
        su_to_str(v, (unsigned)(val[i] < 0 ? 0 : val[i]));
        st_text(cx - text_width(v, F_BIG) / 2, 108, v, F_BIG, S_INK);
        st_text(cx - text_width(lbl[i], F_SMALL) / 2, 124, lbl[i], F_SMALL, S_MUTED);
        if (i) console_fill_rect(colw * i, 94, 1, 36, S_BORDER);   /* column rule */
    }

    /* --- STORAGE plate: free space big, used-fraction bar, capacity caption --- */
    {
        int ix = about_card(16, "STORAGE");
        int bw = AB_CARD_W - 2 * AB_CARD_PAD;
        if (free_mb != 0xFFFFFFFFu) {
            fmt_gb(v, free_mb);
            int pen = st_text(ix, AB_CARD_Y + 44, v, F_BIG, S_INK);
            st_text(pen + 5, AB_CARD_Y + 44, "free", F_SUB, S_MUTED_D);
        } else {
            st_text(ix, AB_CARD_Y + 44, "--", F_BIG, S_MUTED);
        }
        int by = AB_CARD_Y + 52, bh = 6;
        ui_round_rect(ix, by, bw, bh, 3, S_TRK);
        if (total_mb > 0 && free_mb != 0xFFFFFFFFu) {
            uint32_t used = (total_mb > free_mb) ? total_mb - free_mb : 0;
            int fw = (int)(((unsigned long long)used * bw) / total_mb);
            if (fw < bh && used > 0) fw = bh;
            if (fw > bw) fw = bw;
            ui_round_rect(ix, by, fw, bh, 3, S_INK);
            fmt_gb(v, used);                 /* "55.3 GB" ...                */
            { int i = 0; while (v[i] && v[i] != ' ') i++; v[i] = '\0'; }   /* ..."55.3" */
            su_append(v, " of ");
            fmt_gb(w, total_mb);
            su_append(v, w);
            st_text(ix, AB_CARD_Y + 72, v, F_SMALL, S_MUTED_D);
        }
    }

    /* --- BATTERY plate: percent big, a battery pictogram, millivolts caption --- */
    {
        int ix = about_card(164, "BATTERY");
        if (battery_pct >= 0) {
            su_to_str(v, (unsigned)battery_pct); su_append(v, "%");
            st_text(ix, AB_CARD_Y + 44, v, F_BIG, S_INK);
        } else {
            st_text(ix, AB_CARD_Y + 44, "--", F_BIG, S_MUTED);
        }
        int gx = ix, gy = AB_CARD_Y + 50, gw = AB_CARD_W - 2 * AB_CARD_PAD - 4, gh = 10;
        ui_round_rect(gx, gy, gw, gh, 3, S_TRK);                   /* shell   */
        console_fill_rect(gx + gw, gy + 3, 3, gh - 6, S_TRK);      /* + nub   */
        if (battery_pct >= 0) {
            int pct = battery_pct > 100 ? 100 : battery_pct;
            int fw  = (gw - 4) * pct / 100;
            if (fw < 2 && pct > 0) fw = 2;
            ui_round_rect(gx + 2, gy + 2, fw, gh - 4, 2, S_INK);
        }
        /*
         * Millivolts, per battery.h's own instruction to "display raw
         * millivolts and sanity-check the ~3300..4200 mV range before
         * trusting battery_percent()". The percent above is interpolated off
         * a curve transcribed from a 2005 cell; on a replacement cell it is
         * a guess until this number is checked against a meter.
         */
        if (battery_mv > 0) {
            su_to_str(v, (unsigned)battery_mv); su_append(v, " mV");
            st_text(ix, AB_CARD_Y + 72, v, F_SMALL, S_MUTED_D);
        }
    }

    /*
     * Diagnostics footer: the raw ADC code behind the millivolts, the event
     * log, and the headphone jack. "LOG off" is the one that matters —
     * CORELOG.BIN is missing or did not validate. The number is the next
     * block's sequence: it climbing across sessions is how you know flushes
     * are landing.
     *
     * JACK is the pin probe (settings.h): the raw level, then the debounced
     * one as well once the line is trusted (so a bench can watch the two
     * agree), then "n<count>" — how many times the raw level has moved since
     * power-on, which is what separates "this pin follows the plug" from
     * "this pin flaps on its own". "en=0"/"oe=1" appear only when the boot
     * ROM did not leave A7 as a plain GPIO input, i.e. when a level that
     * never changes means nothing.
     */
    {
        char adc[20], jk[40];

        adc[0] = '\0';
        if (battery_raw >= 0) {
            su_copy(adc, "ADC ");
            su_to_str(adc + 4, (unsigned)battery_raw);
            su_append(adc, " " UI_GLYPH_MIDDOT " ");
        }
        jk[0] = '\0';
        if (jack) {
            su_copy(jk, " " UI_GLYPH_MIDDOT " JACK ");
            su_append(jk, jack->raw ? "1" : "0");
            if (jack->debounced >= 0) {
                su_append(jk, jack->debounced ? "/1" : "/0");
            }
            su_append(jk, " n");
            su_to_str(w, jack->edges);
            su_append(jk, w);
            if (!(jack->pin_cfg & ABOUT_JACK_PIN_ENABLED)) {
                su_append(jk, " en=0");
            }
            if (jack->pin_cfg & ABOUT_JACK_PIN_OUTPUT) {
                su_append(jk, " oe=1");
            }
        }

        if (log_state == ABOUT_LOG_OFF) {
            su_copy(w, "LOG off");
        } else {
            char n[12];
            su_copy(w, "LOG ");
            su_to_str(n, (unsigned)log_seq);
            su_append(w, n);
            su_append(w, log_state == ABOUT_LOG_ERR ? " err" : " on");
        }

        su_copy(v, adc);
        su_append(v, w);
        su_append(v, jk);
        /* ui_text_centered neither clips nor ellipsises, so a footer wider
         * than the row would run off both ends. The ADC code is the least
         * useful of the three (the millivolts above it are the number anyone
         * reads), so it is the one that goes. */
        if (text_width(v, F_SMALL) > LCD_WIDTH - 32) {
            su_copy(v, w);
            su_append(v, jk);
        }
        ui_text_centered(236, v, F_SMALL, S_MUTED);
    }
}


/* ---------------------------------------------------------------------------
 * Boot Details (SETTINGS_DIAG)
 * ------------------------------------------------------------------------- */

/*
 * "This phase did not run", distinct from "this phase ran and took no
 * measurable time". The first version conflated them — both rendered "--" —
 * which made the screen actively misleading the moment it mattered: after the
 * FLAC seek fix, a seek that had become instant was indistinguishable from a
 * seek that had been skipped.
 */
#define MS_NA  0xFFFFFFFFu

/*
 * Sub-second times are the interesting ones now, so render them in ms rather
 * than rounding a 40 ms seek to "0.0s". Over a second, one decimal is plenty.
 */
static void fmt_ms(char *d, uint32_t ms)
{
    if (ms == MS_NA) { su_copy(d, "--"); return; }
    if (ms < 1000u) {
        int i = su_to_str(d, ms);
        su_copy(d + i, "ms");
        return;
    }
    int i = su_to_str(d, ms / 1000u);
    d[i++] = '.';
    d[i++] = (char)('0' + (ms / 100u) % 10u);
    d[i++] = 's';
    d[i]   = '\0';
}

/* Draw `s` so it ends at x = right. */
static void st_text_at_right(int right, int y, const char *s,
                             const text_font_t *font, uint16_t ink)
{
    st_text(right - text_width(s, font), y, s, font, ink);
}

/*
 * Phase colours. Fixed literals rather than palette slots, on purpose: this is
 * a diagnostic chart, and its segments have to stay distinguishable from each
 * other in any theme — which is exactly what themed tokens would not
 * guarantee. Ordered to match the legend.
 */
#define C_LCD   0x3BDBu    /* blue      */
#define C_DISK  0x3DADu    /* green     */
#define C_LIB   0xE546u    /* amber     */
#define C_RES   0xE125u    /* red       */
#define C_OTHER 0x94B2u    /* grey      */

void settings_diag_render(uint32_t total_ms, uint32_t lcd_ms, uint32_t disk_ms,
                          uint32_t lib_ms, uint32_t resume_ms,
                          uint32_t res_dir_ms, uint32_t res_open_ms,
                          uint32_t res_seek_ms,
                          uint32_t decode_us_kframe, uint32_t decode_rate,
                          uint32_t underruns,
                          int cfg_writable, uint32_t cfg_seq,
                          uint32_t lba0, uint32_t lba1,
                          uint32_t log_hdr_lba, uint32_t log_next_lba,
                          const char *build_id)
{
    char v[48];
    console_clear(S_SURFACE);
    /* The full build stamp goes in the header's right-hand slot. Everything
     * below is claimed — the phase bar, a two-column legend, the resume
     * split, the underrun warning and two LBA rows run to y=230 of a 240 px
     * panel — so this is the only free text row on the page, and it costs no
     * pixels from the bars. ui_header measures the right value first and
     * ellipsises the title into what is left: "Boot Details" is 80 px at
     * F_HEADER, the longest plausible stamp ("v0.10.12-123-g1234567-dirty")
     * 145 px at F_SMALL, and 24 + 80 + 8 + 145 + 12 = 269 < 320, so neither
     * is clipped. */
    ui_header(settings_title(SETTINGS_DIAG),
              (build_id && build_id[0]) ? build_id : "", 1);

    /* --- headline: label left, total right, on one line --- */
    st_text(16, 60, "COLD BOOT", F_SMALL, S_MUTED);
    fmt_ms(v, total_ms);
    st_text_right(16, 63, v, F_BIG, S_INK);

    /*
     * --- stacked proportional bar ---
     * Where the time went, at a glance. Widths come from the SAME numbers as
     * the legend below, so the picture cannot disagree with the figures. The
     * remainder segment is drawn last and simply takes what is left, which is
     * why unattributed time shows up as grey rather than silently rescaling
     * the others.
     */
    {
        int bx = 16, by = 74, bw = LCD_WIDTH - 32, bh = 10;
        ui_round_rect(bx, by, bw, bh, 5, S_TRK);
        if (total_ms > 0) {
            uint32_t seg[4] = { lcd_ms, disk_ms, lib_ms, resume_ms };
            uint16_t col[4] = { C_LCD, C_DISK, C_LIB, C_RES };
            int x = bx;
            for (int i = 0; i < 4; i++) {
                int w = (int)(((uint64_t)seg[i] * (uint32_t)bw) / total_ms);
                if (w <= 0) continue;                  /* too small to see */
                if (x + w > bx + bw) w = bx + bw - x;
                console_fill_rect(x, by, w, bh, col[i]);
                x += w;
            }
            if (x < bx + bw) {
                console_fill_rect(x, by, bx + bw - x, bh, C_OTHER);
            }
        }
    }

    /*
     * --- legend, two columns ---
     * Six entries in two columns of three rather than six stacked rows: the
     * panel is 240 px tall and the single-column version ran into the config
     * block at the bottom.
     */
    {
        static const char *const NM[6] = { "LCD", "DISK", "LIBRARY",
                                           "RESUME", "OTHER", "DECODE" };
        uint16_t cl[6] = { C_LCD, C_DISK, C_LIB, C_RES, C_OTHER, 0 };
        uint32_t known = lcd_ms + disk_ms + lib_ms + resume_ms;
        uint32_t val[5] = { lcd_ms, disk_ms, lib_ms, resume_ms,
                            (total_ms > known) ? total_ms - known : 0 };
        int colx[2] = { 16, 168 };
        int colw    = 136;
        for (int i = 0; i < 6; i++) {
            int cx = colx[i / 3];
            int y  = 104 + (i % 3) * 18;
            console_fill_rect(cx, y - 7, 6, 6, cl[i] ? cl[i] : S_MUTED2);
            st_text(cx + 11, y, NM[i], F_SMALL, S_MUTED);
            if (i < 5) {
                fmt_ms(v, val[i]);
                st_text_at_right(cx + colw, y, v, F_SUB, S_INK);
            } else {
                /*
                 * Decode headroom against real time: 1000 frames must decode
                 * in under 1e9/rate microseconds (22676 at 44.1 kHz, 20833 at
                 * 48). The divisor is the STREAM's rate, not a constant —
                 * hard-coding 44.1 kHz under-reported a 48 kHz track by 8 %,
                 * and MP3 arrives at rates FLAC albums rarely use.
                 *
                 * This is THE number for MP3: pvmp3 is fixed-point and costs
                 * about 1.7x the FLAC decoder on the same music, and whether
                 * that fits on an 80 MHz ARM7TDMI has never been measured on
                 * the device. Red inside 20 % of the budget: past that a slow
                 * disk refill becomes an audible underrun, not a near miss.
                 */
                uint32_t budget = (decode_rate > 0u) ? 1000000000u / decode_rate
                                                     : 22676u;
                if (decode_us_kframe == 0) {
                    st_text_at_right(cx + colw, y, "--", F_SUB, S_MUTED_D);
                } else {
                    int pct = (int)((decode_us_kframe * 100u) / budget);
                    su_to_str(v, (unsigned)pct);
                    su_append(v, "%");
                    st_text_at_right(cx + colw, y, v, F_SUB,
                                     (pct >= 80) ? S_WARN : S_INK);
                }
            }
        }
    }

    console_fill_rect(16, 152, LCD_WIDTH - 32, 1, S_BORDER);

    /* --- the resume split, as one line: it is a breakdown OF the RESUME
     * entry above, not three more phases, so it is indented and dimmer. --- */
    {
        static const char *const SUB[3] = { "dir", "open", "seek" };
        uint32_t sv[3] = { res_dir_ms, res_open_ms, res_seek_ms };
        int x = 16;
        st_text(x, 168, "RESUME", F_SMALL, S_MUTED2);
        x += text_width("RESUME", F_SMALL) + 10;
        for (int i = 0; i < 3; i++) {
            st_text(x, 168, SUB[i], F_SMALL, S_MUTED2);
            x += text_width(SUB[i], F_SMALL) + 4;
            fmt_ms(v, sv[i]);
            st_text(x, 168, v, F_SMALL, S_INK);
            x += text_width(v, F_SMALL) + 12;
        }
    }

    if (underruns) {
        su_to_str(v, underruns);
        su_append(v, " audio underrun");
        st_text(16, 186, v, F_SMALL, S_WARN);
    }

    console_fill_rect(16, 196, LCD_WIDTH - 32, 1, S_BORDER);

    /*
     * --- settings-file locator ---
     * The two absolute LBAs config_save() writes to. This is the ONLY way to
     * run the mandatory pre-write safety check on this device:
     * tools/make_config.py says to boot, read the LBA the firmware reports,
     * and confirm it matches the host's independently computed address BEFORE
     * anything is written — "a mismatch there is the bug that overwrites
     * somebody's music library". That procedure assumes a serial cable, and
     * there is none here, so the number has to reach the panel. Read-only.
     */
    /* Two rows, label left, "a / b" right: CONFIG's slot LBAs (with the seq
     * folded into the label), then the event log's header / next-flush LBAs.
     * A pair the host tool prints in the same order. */
    if (!cfg_writable) {
        st_text(16, 214, "CONFIG", F_SMALL, S_MUTED);
        st_text_right(16, 214, "not writable", F_SUB, S_MUTED_D);
    } else {
        su_copy(v, "CONFIG seq ");
        su_to_str(v + 11, cfg_seq);
        st_text(16, 214, v, F_SMALL, S_MUTED);
        int i = su_to_str(v, lba0);
        su_copy(v + i, " / ");
        su_to_str(v + i + 3, lba1);
        st_text_right(16, 214, v, F_SMALL, S_MUTED_D);
    }
    st_text(16, 230, "LOG", F_SMALL, S_MUTED);
    if (log_hdr_lba || log_next_lba) {
        int i = su_to_str(v, log_hdr_lba);
        su_copy(v + i, " / ");
        su_to_str(v + i + 3, log_next_lba);
        st_text_right(16, 230, v, F_SMALL, S_MUTED_D);
    } else {
        st_text_right(16, 230, "off", F_SUB, S_MUTED_D);
    }
}

/* ---------------------------------------------------------------------------
 * Set Date & Time (plans/14-clock.md's widget; there is no jsx for it).
 *
 * A row of plates, one per field, drawn in the list's own language: the
 * selected plate is the selection bar's inversion (filled ink, surface text),
 * the others are surface with a 1 px border, exactly as a selected row inverts
 * against an unselected one. Everything is a palette token, so the seven
 * themes get it for free.
 *
 *   y  72   the date being edited, spelled out            FONT_SUB / MUTED2
 *     100   the plates, 34 px tall, r=6                   FONT_TITLE
 *     150   the captions under them                       FONT_SMALL / MUTED
 *     224   what the buttons do                           FONT_SMALL / MUTED2
 *
 * Widths: year 52, the rest 34; gaps 10, 22 between the date and the time, and
 * 8 for the colon — 282 px in 12-hour mode, 238 in 24-hour, centred either
 * way. The band above the header is left clear like every other Settings
 * screen; the firmware paints the status strip over it.
 * ------------------------------------------------------------------------- */

#define ST_PLATE_Y 100
#define ST_PLATE_H 34
#define ST_PLATE_R 6
#define ST_CAP_Y   150                         /* caption baseline            */
#define ST_SUM_Y   72                          /* summary line baseline       */
#define ST_HINT_Y  224                         /* button hint baseline        */

/* Plate width, and the gap BEFORE each plate (the first has none). */
static const uint8_t ST_W[ST_FIELDS]   = { 52, 34, 34, 34, 34, 34 };
static const uint8_t ST_GAP[ST_FIELDS] = {  0, 10, 10, 22,  8, 10 };

void settime_render(const settime_t *t)
{
    console_clear(S_SURFACE);
    ui_header(settings_title(SETTINGS_SETTIME), "", 1);

    int nf = settime_field_count(t);

    /* The date in words, so the plates never have to be read as a date. */
    datetime_t civil;
    settime_civil(t, &civil);
    char line[DATETIME_DATE_MAX];
    if (datetime_fmt_date(line, (int)sizeof(line), &civil) > 0) {
        int w = text_width(line, F_SUB);
        st_text((LCD_WIDTH - w) / 2, ST_SUM_Y, line, F_SUB, S_MUTED2);
    }

    int total = 0;
    for (int f = 0; f < nf; f++) {
        total += (int)ST_GAP[f] + (int)ST_W[f];
    }

    /* Vertically centre the plate text on its ink box, not on the line box:
     * the caption sits close underneath and leading would push the digits up
     * off centre. */
    int asc  = text_ascent(F_BIG);
    int desc = text_descent(F_BIG);
    int base = ST_PLATE_Y + (ST_PLATE_H - (asc + desc)) / 2 + asc;

    int x = (LCD_WIDTH - total) / 2;
    for (int f = 0; f < nf; f++) {
        x += (int)ST_GAP[f];
        int w = (int)ST_W[f];
        int is_sel = (f == t->field);

        if (is_sel) {
            ui_round_rect(x, ST_PLATE_Y, w, ST_PLATE_H, ST_PLATE_R, S_SEL_BG);
        } else {
            /* Border then an inset fill: the same two-call outline the volume
             * plate uses, and the only way to get a 1 px rounded rule. */
            ui_round_rect(x, ST_PLATE_Y, w, ST_PLATE_H, ST_PLATE_R, S_BORDER);
            ui_round_rect(x + 1, ST_PLATE_Y + 1, w - 2, ST_PLATE_H - 2,
                          ST_PLATE_R - 1, S_SURFACE);
        }

        char buf[SETTIME_FIELD_MAX];
        if (settime_field_text(t, f, buf, (int)sizeof(buf)) > 0) {
            int tw = text_width(buf, F_BIG);
            st_text(x + (w - tw) / 2, base, buf, F_BIG,
                    is_sel ? S_SEL_FG : S_INK);
        }

        const char *cap = settime_field_label(f);
        if (cap[0] != '\0') {
            int cw = text_width(cap, F_SMALL);
            st_text(x + (w - cw) / 2, ST_CAP_Y, cap, F_SMALL, S_MUTED);
        }

        /* The colon lives in the 8 px gap that precedes the minute plate. */
        if (f == ST_MIN) {
            int gap = (int)ST_GAP[ST_MIN];
            int cw  = text_width(":", F_BIG);
            st_text(x - gap + (gap - cw) / 2, base, ":", F_BIG, S_INK);
        }

        x += w;
    }

    const char *hint = "Wheel changes " UI_GLYPH_MIDDOT
                       " Select next " UI_GLYPH_MIDDOT " Menu cancels";
    int hw = text_width(hint, F_SMALL);
    st_text((LCD_WIDTH - hw) / 2, ST_HINT_Y, hint, F_SMALL, S_MUTED2);
}


/* ---------------------------------------------------------------------------
 * Battery (SETTINGS_BATTERY)
 * ------------------------------------------------------------------------- */

/*
 * The Charge Rate row over a small dashboard: what the charger has actually
 * been doing, which the charging screen's "CHARGING" cannot say.
 *
 *   ‹ Battery
 *   [ Charge Rate                              500 mA ]
 *   Charging                                  3912 mV
 *   USB · 500 mA · 47% · +38 mV in 12 min
 *   ┌ LAST HOUR                    +21 mV / 10 min ┐
 *   │ 3940                                   ╱─    │
 *   │ 3860 ───────────────────────╱──╱             │
 *   └──────────────────────────────────────────────┘
 *        CHRG 1 · 75% on · HPWR 1 · SUSP 0 · ADC 651
 *
 * WHY A TREND AND NOT A CURRENT. The iPod has no current sense; the only
 * measurement is the terminal voltage every 5 s. The LTC4066's CHRG pin says
 * the charger is TRYING, and the 2026-09-19 event log has it asserted for
 * three hours of playback during which the cell moved 11 mV — so the pin
 * alone would have read "charging" through every hour the cell was not. The
 * line is the answer: rising, flat or falling, over the last hour, with the
 * cable's edges left in (kernel/chargestat.h on why it is not reset).
 *
 * THE FOOTER is the pin probe, the same idea as About's JACK token: HPWR is
 * read back from the pad, so "500 mA" on the row and "HPWR 0" on the footer
 * together say the request never reached the charger — and "oe=0" says why
 * (the pin is not being driven). The one thing this page cannot do is
 * confirm the port DELIVERS 500 mA; the line above it does that, slowly.
 */
#define BP_HEAD_BASE   92          /* status word + millivolts baseline       */
#define BP_SUB_BASE    108         /* the token line under them               */
#define BP_PLATE_Y     116
#define BP_PLATE_H     92          /* ...to 208                               */
#define BP_PLATE_X     16
#define BP_PLATE_W     (LCD_WIDTH - 2 * BP_PLATE_X)
#define BP_LABEL_BASE  (BP_PLATE_Y + 18)
#define BP_CHART_X     62          /* the mV labels take 26..58               */
#define BP_CHART_W     (BP_PLATE_X + BP_PLATE_W - 10 - BP_CHART_X)   /* 232  */
#define BP_CHART_Y     (BP_PLATE_Y + 26)                             /* 142  */
#define BP_CHART_H     56                                            /* ..198*/
#define BP_FOOT_BASE   232
#define BP_MIN_SPAN_MV 40          /* the chart never zooms tighter than this */

/* "+38 mV" / "-12 mV" / "0 mV". */
static void fmt_smv(char *d, int mv)
{
    int i = 0;
    if (mv > 0) d[i++] = '+';
    if (mv < 0) { d[i++] = '-'; mv = -mv; }
    i += su_to_str(d + i, (unsigned)mv);
    su_copy(d + i, " mV");
}

/* "12 min" / "1 h 5 min" (never called under a minute: see the token line). */
static void fmt_span(char *d, uint32_t secs)
{
    uint32_t m = secs / 60u;
    if (m == 0) { su_copy(d, "<1 min"); return; }
    if (m < 60u) {
        int i = su_to_str(d, m);
        su_copy(d + i, " min");
        return;
    }
    int i = su_to_str(d, m / 60u);
    su_copy(d + i, " h ");
    i += 3;
    i += su_to_str(d + i, m % 60u);
    su_copy(d + i, " min");
}

/* 1-px line from (x0,y0) to (x1,y1), Bresenham over console_fill_rect. Two
 * pixels tall so the trace reads at arm's length on a 320x240 panel. */
static void bp_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        console_fill_rect(x0, y0, 1, 2, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* The hour's line. The time axis is fixed — the right edge is now, the left
 * edge an hour ago — so a device up for ten minutes shows ten minutes of
 * line at the right, not ten minutes stretched across the plate. */
static void bp_chart(const battery_page_t *live)
{
    int x0 = BP_CHART_X, y0 = BP_CHART_Y, w = BP_CHART_W, h = BP_CHART_H;
    console_fill_rect(x0, y0 + h - 1, w, 1, S_TRK);        /* the floor    */

    int n = (live && live->hist) ? live->hist_n : 0;
    if (n > BATTERY_HIST_MAX) n = BATTERY_HIST_MAX;
    int lo = 0, hi = 0, readings = 0;
    for (int i = 0; i < n; i++) {
        int v = live->hist[i];
        if (v == 0) continue;
        if (!readings || v < lo) lo = v;
        if (!readings || v > hi) hi = v;
        readings++;
    }
    if (readings < 2) {
        ui_text_centered(y0 + h / 2 + 3, "Collecting", F_SMALL, S_MUTED);
        return;
    }
    /* Scale: at least BP_MIN_SPAN_MV tall, centred on the readings, so a
     * flat hour draws flat in the middle rather than as noise filling the
     * plate; labelled top and bottom. */
    int span = hi - lo;
    if (span < BP_MIN_SPAN_MV) {
        int pad = (BP_MIN_SPAN_MV - span) / 2;
        lo -= pad;
        hi  = lo + BP_MIN_SPAN_MV;
        span = BP_MIN_SPAN_MV;
    }
    char v[12];
    su_to_str(v, (unsigned)hi);
    st_text(BP_CHART_X - 4 - text_width(v, F_SMALL), y0 + 8, v, F_SMALL, S_MUTED2);
    su_to_str(v, (unsigned)lo);
    st_text(BP_CHART_X - 4 - text_width(v, F_SMALL), y0 + h - 1, v, F_SMALL, S_MUTED2);

    int px = -1, py = -1, lx = -1, ly = -1;
    for (int i = 0; i < n; i++) {
        int mv = live->hist[i];
        /* Bin i of n sits at (i + 60 - n) of 60 along the fixed hour. */
        int x = x0 + ((i + BATTERY_HIST_MAX - n) * (w - 1)) / (BATTERY_HIST_MAX - 1);
        if (mv == 0) { px = -1; continue; }              /* a gap breaks the line */
        int y = y0 + (h - 2) - ((mv - lo) * (h - 2)) / span;
        if (px >= 0) {
            bp_line(px, py, x, y, S_INK);
        } else {
            console_fill_rect(x, y, 1, 2, S_INK);         /* a lone reading */
        }
        px = x; py = y; lx = x; ly = y;
    }
    if (lx >= 0) {
        console_fill_rect(lx - 1, ly - 1, 3, 4, S_INK);   /* now: a heavier dot */
    }
}

void settings_battery_render(const settings_t *s, int sel,
                             const battery_page_t *live)
{
    char v[96], w[24];

    console_clear(S_SURFACE);
    ui_header(settings_title(SETTINGS_BATTERY), "", 1);
    list_render(SETTINGS_BATTERY, s, sel);            /* the Charge Rate row */

    /* --- headline: the status word left, millivolts right --- */
    const char *status = "--";
    if (live && (live->pct >= 0 || live->mv >= 0)) {
        if (live->source == 0)                         status = "On battery";
        else if (live->charging)                       status = "Charging";
        else if (live->pct >= CHG_FULL_PCT)            status = "Charged";
        else                                           status = "Not charging";
    }
    st_text(16, BP_HEAD_BASE, status, F_BIG, S_INK);
    if (live && live->mv >= 0) {
        su_to_str(v, (unsigned)live->mv); su_append(v, " mV");
        st_text_right(16, BP_HEAD_BASE, v, F_BIG, S_INK);
    } else {
        st_text_right(16, BP_HEAD_BASE, "--", F_BIG, S_MUTED);
    }

    /* --- the token line: supply · budget · percent · the session --- */
    v[0] = '\0';
    if (live) {
        if (live->source == 0) {
            su_copy(v, "BATTERY");
        } else {
            su_copy(v, (live->source & BATTERY_SRC_USB) ? "USB" : "DOCK");
            if ((live->source & (BATTERY_SRC_USB | BATTERY_SRC_MAIN)) ==
                (BATTERY_SRC_USB | BATTERY_SRC_MAIN)) {
                su_append(v, "+DOCK");
            }
            su_append(v, " " UI_GLYPH_MIDDOT " ");
            su_to_str(w, (unsigned)live->rate_ma); su_append(v, w); su_append(v, " mA");
        }
        if (live->pct >= 0) {
            su_append(v, " " UI_GLYPH_MIDDOT " ");
            su_to_str(w, (unsigned)live->pct); su_append(v, w); su_append(v, "%");
        }
        /* The session's drift, once it has had a minute to mean something:
         * "+0 mV in <1 min" is not information. */
        if (live->session_mv0 >= 0 && live->session_s >= 60u) {
            su_append(v, " " UI_GLYPH_MIDDOT " ");
            fmt_smv(w, live->session_dmv); su_append(v, w);
            su_append(v, " in ");
            fmt_span(w, live->session_s); su_append(v, w);
        }
    }
    if (v[0]) {
        st_text(16, BP_SUB_BASE, v, F_SMALL, S_MUTED_D);
    }

    /* --- the plate: the last hour, and the last ten minutes as a number --- */
    ui_round_rect(BP_PLATE_X, BP_PLATE_Y, BP_PLATE_W, BP_PLATE_H, 6, S_PLATE_C);
    st_text(BP_PLATE_X + 10, BP_LABEL_BASE, "LAST HOUR", F_SMALL, S_MUTED);
    if (live && live->trend_ok) {
        fmt_smv(v, live->trend_mv);
    } else {
        su_copy(v, "--");
    }
    su_append(v, " / ");
    su_to_str(w, (unsigned)BATTERY_TREND_MIN); su_append(v, w); su_append(v, " min");
    st_text_right(BP_PLATE_X + 10, BP_LABEL_BASE, v, F_SMALL, S_MUTED_D);
    bp_chart(live);

    /* --- the footer: the charger's pins, as the SoC reads them back --- */
    if (live) {
        char adc[16];
        su_copy(v, "CHRG ");
        su_append(v, live->charging ? "1" : "0");
        if (live->session_mv0 >= 0) {
            su_append(v, " " UI_GLYPH_MIDDOT " ");
            su_to_str(w, (unsigned)live->session_chg); su_append(v, w);
            su_append(v, "% on");
        }
        su_append(v, " " UI_GLYPH_MIDDOT " HPWR ");
        su_append(v, live->hpwr < 0 ? "?" : live->hpwr ? "1" : "0");
        if (!(live->hpwr_cfg & BATTERY_PIN_GPIO)) su_append(v, " en=0");
        if (!(live->hpwr_cfg & BATTERY_PIN_OUT))  su_append(v, " oe=0");
        su_append(v, " " UI_GLYPH_MIDDOT " SUSP ");
        su_append(v, live->susp < 0 ? "?" : live->susp ? "1" : "0");
        if (!(live->susp_cfg & BATTERY_PIN_GPIO)) su_append(v, " en=0");
        if (!(live->susp_cfg & BATTERY_PIN_OUT))  su_append(v, " oe=0");
        adc[0] = '\0';
        if (live->adc >= 0) {
            su_copy(adc, " " UI_GLYPH_MIDDOT " ADC ");
            su_to_str(w, (unsigned)live->adc); su_append(adc, w);
        }
        /* The ADC code is the least useful token and the first to go when
         * the pin tails make the line too wide (same rule as About). */
        int fits = text_width(v, F_SMALL) + text_width(adc, F_SMALL) <= LCD_WIDTH - 32;
        if (fits) su_append(v, adc);
        ui_text_centered(BP_FOOT_BASE, v, F_SMALL, S_MUTED);
    }
}
