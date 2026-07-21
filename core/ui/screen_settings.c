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
 * The jsx Settings screens carry no status strip (unlike the browser), so this
 * leaves the top band clear and places the header at main.c's header baseline —
 * the header stays put when you cross from the main menu into Settings.
 *
 * Pure rendering: integer-only, no libc/libm/malloc, no hardware access, no
 * present. The caller hands the framebuffer to lcd_present_fb().
 */

#include "settings.h"

#include "text.h"
#include "../kernel/console.h"
#include "../hal/hal.h"                        /* LCD_WIDTH / LCD_HEIGHT */

/* ---------------------------------------------------------------------------
 * Linen palette (kernel/main.c tokens -> RGB565) + theme swatch tones
 * ------------------------------------------------------------------------- */
#define S_SURFACE 0xF79Du      /* #f4f1ec surface                             */
#define S_INK     0x18A2u      /* #1a1714 ink / selection bar                 */
#define S_MUTED   0x7B8Du      /* #7a7068 secondary text                      */
#define S_MUTED2  0x9C70u      /* #9a8e80 lighter muted (subs / header right) */
#define S_MUTED_D 0x5A89u      /* #5a5048 deep muted (right values / keys)    */
#define S_ACCENT  0xC348u      /* terracotta accent (Ink theme ink hint)      */
#define S_BORDER  0xE71Bu      /* subtle divider line                         */
#define S_SEL_BG  S_INK        /* selection bar = dark ink                    */
#define S_SEL_FG  S_SURFACE    /* selection text = surface                    */
#define S_SEL_SUB 0xB595u      /* sub / right text ON a selected row          */
#define S_CHEVRON 0xB575u      /* > chevron, rgba(ink,0.3) on surface         */
#define S_TRK     0xE73Cu      /* slider track, rgba(ink,0.06)                */
#define S_PILL_OFF 0xCED6u     /* toggle pill OFF fill (light grey)           */

/* Nunito faces (see ui/text.h). */
#define F_HEADER text_font_bold_13()
#define F_ROW    text_font_regular_13()
#define F_SUB    text_font_regular_11()
#define F_SMALL  text_font_regular_9()

/* Geometry — identical to kernel/main.c so screens line up across the UI. */
#define HDR_BASE   30
#define HDR_DIV_Y  38
#define LIST_Y0    42
#define ROW_H      24
#define TH_ROW_H   40                          /* taller theme-picker rows    */

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

/* Titled header with an optional back chevron and right-aligned value, plus the
 * divider (menus.jsx ScreenHeader). */
static void header_render(const char *title, const char *right, int back)
{
    int x = 12;
    if (back) {
        x = st_text(x, HDR_BASE, "<", F_HEADER, S_MUTED2) + 4;
    }
    st_text(x, HDR_BASE, title, F_HEADER, S_INK);
    if (right && right[0]) {
        st_text_right(12, HDR_BASE - 1, right, F_SMALL, S_MUTED2);
    }
    console_fill_rect(12, HDR_DIV_Y, LCD_WIDTH - 24, 1, S_BORDER);
}

/* The dark-ink selection bar behind row `r` (menus.jsx selected Row). */
static void sel_bar(int y0, int rowh, int r)
{
    int ry = y0 + r * rowh;
    console_fill_rect(6, ry + 1, LCD_WIDTH - 16, rowh - 2, S_SEL_BG);
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
    console_fill_rect(px, py, pw, ph, track);
    int kx = on ? px + pw - 2 - 9 : px + 2;
    console_fill_rect(kx, py + 2, 9, 9, knob);
}

/* Slider fill bar for a value fraction num/den (menus.jsx SettingsSound bar):
 * a full-width track with a proportional fill, colours flipping when selected. */
static void draw_slider(int ry, int selected, int num, int den)
{
    int bx = 14, bw = LCD_WIDTH - 16 - bx, by = ry + 17, bh = 3;
    uint16_t trackc = selected ? S_SEL_SUB : S_TRK;
    uint16_t fillc  = selected ? S_SEL_FG  : S_INK;
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
static void list_render(int screen, const settings_t *s, int sel)
{
    int n = settings_count(screen);
    for (int r = 0; r < n; r++) {
        int ry = LIST_Y0 + r * ROW_H;
        int is_sel = (r == sel);
        if (is_sel) {
            sel_bar(LIST_Y0, ROW_H, r);
        }

        int kind = settings_kind(screen, r);
        char buf[24];
        int is_toggle = 0, on = 0, num = 0, den = 0;
        settings_value(screen, s, r, buf, &is_toggle, &on, &num, &den);

        uint16_t fg     = is_sel ? S_SEL_FG  : S_INK;
        uint16_t rightc = is_sel ? S_SEL_SUB : S_MUTED_D;
        uint16_t chevc  = is_sel ? S_SEL_SUB : S_CHEVRON;

        if (kind == SETTINGS_KIND_SLIDER) {
            /* label + right value lifted to make room for the bar below. */
            st_text(14, ry + 11, settings_label(screen, r),
                    is_sel ? F_HEADER : F_ROW, fg);
            if (buf[0]) {
                st_text_right(16, ry + 11, buf, F_SUB, rightc);
            }
            draw_slider(ry, is_sel, num, den);
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
            st_text(LCD_WIDTH - 18, ry + 15, ">", F_ROW, chevc);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Theme picker (system-screens.jsx ThemePicker): swatch + name + sub, with a
 * "CURRENT" tag on the active theme.
 * ------------------------------------------------------------------------- */
static const uint16_t TH_SWATCH[4] = { S_SURFACE, 0xFBDEu, 0x0861u, 0xEB5Cu };
static const uint16_t TH_INK[4]    = { S_INK, S_INK, S_ACCENT, S_INK };
static const char *const TH_SUB[4] = {
    "Warm light - text-forward",
    "Minimal - big art",
    "True dark - terracotta",
    "Floating card surface",
};

static void theme_render(const settings_t *s, int sel)
{
    for (int r = 0; r < 4; r++) {
        int ry = LIST_Y0 + r * TH_ROW_H;
        int is_sel = (r == sel);
        if (is_sel) {
            sel_bar(LIST_Y0, TH_ROW_H, r);
        }

        /* Swatch tile with a border + a small "text" hint bar. */
        int sw = 26, sx = 14, sy = ry + (TH_ROW_H - sw) / 2;
        console_fill_rect(sx - 1, sy - 1, sw + 2, sw + 2,
                          is_sel ? S_SEL_SUB : S_BORDER);
        console_fill_rect(sx, sy, sw, sw, TH_SWATCH[r]);
        console_fill_rect(sx + 6, sy + 10, 14, 3, TH_INK[r]);

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
        settings_about_render(-1, -1, 0, 0, 0);
        return;
    }

    console_clear(S_SURFACE);

    const char *title;
    const char *right = "";
    switch (screen) {
    case SETTINGS_PLAYBACK: title = "Playback"; break;
    case SETTINGS_SOUND:    title = "Sound";    break;
    case SETTINGS_DISPLAY:  title = "Display";  break;
    case SETTINGS_THEME:    title = "Theme"; right = "4 themes"; break;
    default:                title = "Settings"; break;
    }
    header_render(title, right, 1);

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

/* One About row: muted key on the left, bold value on the right, divider under.
 * Advances and returns the next row's top y. */
static int about_row(int y, const char *key, const char *val)
{
    st_text(16, y + 11, key, F_SUB, S_MUTED_D);
    st_text_right(16, y + 11, val, F_HEADER, S_INK);
    console_fill_rect(16, y + 22, LCD_WIDTH - 32, 1, S_BORDER);
    return y + 24;
}

void settings_about_render(int battery_pct, int battery_mv,
                           uint32_t free_mb, uint32_t total_mb, int n_artists)
{
    console_clear(S_SURFACE);
    header_render("About", "", 1);

    char v[24];
    int y = 54;

    y = about_row(y, "Model", "iPod 5.5G");
    y = about_row(y, "Firmware", "core");

    /* Battery: "NN% NNNN mV" (dashes if not yet sampled). */
    if (battery_pct < 0) {
        su_copy(v, "--");
    } else {
        int i = su_to_str(v, (unsigned)battery_pct);
        v[i++] = '%';
        if (battery_mv > 0) {
            v[i++] = ' ';
            i += su_to_str(v + i, (unsigned)battery_mv);
            v[i++] = ' '; v[i++] = 'm'; v[i++] = 'V';
        }
        v[i] = '\0';
    }
    y = about_row(y, "Battery", v);

    fmt_gb(v, total_mb);
    y = about_row(y, "Capacity", v);
    fmt_gb(v, free_mb);
    y = about_row(y, "Free", v);

    su_to_str(v, (unsigned)(n_artists < 0 ? 0 : n_artists));
    y = about_row(y, "Artists", v);
    (void)y;
}
