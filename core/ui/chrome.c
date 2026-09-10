/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/chrome.c — implementation of the shared list/panel chrome.
 *
 * Moved VERBATIM out of kernel/main.c (only the names changed, from file-local
 * statics to the ui_* prefix in chrome.h) so that the same code the device
 * runs is the code the host tests exercise. See chrome.h for why.
 */

#include "chrome.h"

#include "palette.h"
#include "text.h"
#include "artcache.h"        /* ARTCACHE_DIM: the list row's art chip size */
#include "../kernel/console.h"
#include "../hal/hal.h"      /* LCD_WIDTH, LCD_HEIGHT */

/*
 * Overflowing row titles scroll on the device and are clipped everywhere else
 * — see ui_set_scroll_text() in chrome.h for why the time source is injected
 * rather than reached for.
 */
static ui_scroll_text_fn g_scroll_text;

void ui_set_scroll_text(ui_scroll_text_fn fn)
{
    g_scroll_text = fn;
}

static void row_title(int x, int y, int w, const char *text,
                      const text_font_t *font, uint16_t ink, uint16_t bg,
                      int cy0, int cy1)
{
    if (g_scroll_text != NULL) {
        g_scroll_text(x, y, w, text, font, ink, bg, cy0, cy1);
        return;
    }
    ui_text_clip(x, y, text, font, ink, x, x + w);
}


/* The corner-inset cache below is the one piece of state in this file; it is a
 * pure memo of a pure function, so it needs no synchronisation. */

/* The Nunito renderer writes straight through console_fb(), which console.c's
 * damage tracker can't see — so every text draw reports its own ink box, or a
 * damage-only present would drop the glyphs. `y` is the baseline. */
static void ui_text_damage(int x, int y, int w, const text_font_t *font)
{
    int asc = text_ascent(font), desc = text_descent(font);
    console_damage_add(x, y - asc, w, asc + desc);
}

/* Draw a NUL-terminated string; thin wrapper over text_draw with the panel
 * dimensions baked in. `y` is the text baseline. Returns the advance. */
int ui_text(int x, int y, const char *s, const text_font_t *font,
                   uint16_t ink)
{
    int end = text_draw(console_fb(), LCD_WIDTH, LCD_HEIGHT, x, y, s, font, ink);
    ui_text_damage(x, y, end - x, font);
    return end;
}

/* text_draw_clip with the panel dimensions baked in + damage reporting. */
int ui_text_clip(int x, int y, const char *s, const text_font_t *font,
                        uint16_t ink, int cx0, int cx1)
{
    int end = text_draw_clip(console_fb(), LCD_WIDTH, LCD_HEIGHT, x, y, s, font,
                             ink, cx0, cx1);
    int x0 = (x > cx0) ? x : cx0, x1 = (end < cx1) ? end : cx1;
    ui_text_damage(x0, y, x1 - x0, font);
    return end;
}

/* Centre a string horizontally at baseline `y`. */
void ui_text_centered(int y, const char *s, const text_font_t *font,
                             uint16_t ink)
{
    int w = text_width(s, font);
    ui_text((LCD_WIDTH - w) / 2, y, s, font, ink);
}

/* U+2026 HORIZONTAL ELLIPSIS as UTF-8. The atlas carries it (glyphmap.h), and
 * at bold 13 it is 10 px against 11 for three periods — narrower, and one
 * glyph so the pair kerning cannot spread it. */
#define UI_ELLIPSIS "\xE2\x80\xA6"

int ui_text_ellipsis_fit(char *buf, int buf_sz, const char *s,
                         const text_font_t *font, int max_w)
{
    const int room = buf_sz - 4;          /* prefix bytes, leaving "…" + NUL */
    if (max_w <= 0 || room <= 0) {
        buf[0] = '\0';
        return 0;
    }

    /* Copy what fits in the buffer. A string longer than the buffer cannot
     * fit on the panel anyway, so treating it as "too long" is exact. */
    int n = 0, whole = 1;
    while (s[n] != '\0') {
        if (n >= room) { whole = 0; break; }
        buf[n] = s[n];
        n++;
    }
    buf[n] = '\0';
    if (whole && text_width(buf, font) <= max_w) {
        return n;
    }

    /*
     * Shorten from the end, one CODEPOINT at a time, re-measuring the WHOLE
     * candidate string each step. Two things make the obvious shortcuts
     * wrong: the renderer is UTF-8 (a byte-wise cut leaves a stray
     * continuation byte that draws as a .notdef box), and text.c applies
     * pair kerning and rounds once at the end, so summing per-glyph widths
     * drifts from what text_width says — and the marquee already chains on
     * text_width being the truth. The measure includes the ellipsis so the
     * kern between the last letter and it is counted.
     */
    for (;;) {
        if (n == 0) {
            break;
        }
        n--;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) {
            n--;                        /* back over continuation bytes */
        }
        while (n > 0 && buf[n - 1] == ' ') {
            n--;                        /* "of the…", not "of the …" */
        }
        buf[n]     = UI_ELLIPSIS[0];
        buf[n + 1] = UI_ELLIPSIS[1];
        buf[n + 2] = UI_ELLIPSIS[2];
        buf[n + 3] = '\0';
        if (text_width(buf, font) <= max_w) {
            return n + 3;
        }
    }
    /* Not even the ellipsis fits. */
    buf[0] = '\0';
    return 0;
}

int ui_text_ellipsis(int x, int y, const char *s, const text_font_t *font,
                     uint16_t ink, int max_w)
{
    /* 160 bytes: at the smallest face that is ~4 panel widths of ASCII, and
     * a title that long is shortened to a few words regardless. */
    char buf[164];
    int n = ui_text_ellipsis_fit(buf, (int)sizeof buf, s, font, max_w);
    if (n == 0) {
        return x;
    }
    return ui_text(x, y, buf, font, ink);
}


/* Filled rounded rectangle (radius `r`) — the design's selection bars (r=4) and
 * plates (r=6) are rounded, not square; this replaces the hard console_fill_rect
 * at those spots. Each corner row is inset along a quarter-circle. */
int ui_isqrt(int v)
{
    /* Bitwise integer sqrt: ~4 iterations for the radii we use, no multiply in
     * the loop. The old form stepped r up one at a time WITH a multiply, and it
     * ran per scanline of every rounded rect — including the selection bar on
     * every single list repaint. */
    unsigned x = (unsigned)(v < 0 ? 0 : v), res = 0, bit = 1u << 16;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= res + bit) {
            x   -= res + bit;
            res  = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (int)res;
}

static int     g_rr_r = -1;
static uint8_t g_rr_inset[UI_RR_MAX_R];

static const uint8_t *rr_insets(int r)
{
    if (r != g_rr_r) {
        for (int k = 0; k < r; k++) {
            int dy = r - k;
            g_rr_inset[k] = (uint8_t)(r - ui_isqrt(r * r - dy * dy));
        }
        g_rr_r = r;
    }
    return g_rr_inset;
}

void ui_round_rect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r < 1) { console_fill_rect(x, y, w, h, c); return; }
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    if (r > UI_RR_MAX_R) r = UI_RR_MAX_R;
    const uint8_t *ins = rr_insets(r);
    for (int ry = 0; ry < h; ry++) {
        int inset = 0, k = -1;
        if (ry < r)            k = ry;
        else if (ry >= h - r)  k = h - 1 - ry;
        if (k >= 0) inset = ins[k];
        console_fill_rect(x + inset, y + ry, w - 2 * inset, 1, c);
    }
}

void ui_header(const char *title, const char *right, int back)
{
    int x = 12;
    if (back) {
        x = ui_text(x, HDR_BASE, UI_GLYPH_LAQUO, FONT_HEADER, LINEN_MUTED2) + 4;
    }
    /* Measure the right-hand value FIRST: the title's room is what is left
     * after it, less an 8 px gap. Real titles overflow — a Songs list
     * filtered by "The Presidents of the United States of America" is
     * ~300 px at bold 13 — and the title used to be drawn full width with
     * the count painted over its tail, which the alpha-blending renderer
     * turned into a smear rather than a cover. */
    int show_right = (right && right[0]);
    int right_w    = show_right ? text_width(right, FONT_SMALL) : 0;
    int title_max  = show_right ? (LCD_WIDTH - 12 - right_w - 8) - x
                                : (LCD_WIDTH - 12) - x;
    ui_text_ellipsis(x, HDR_BASE, title, FONT_HEADER, LINEN_INK, title_max);
    if (show_right) {
        ui_text(LCD_WIDTH - 12 - right_w, HDR_BASE - 1, right, FONT_SMALL,
                LINEN_MUTED2);
    }
    console_fill_rect(12, HDR_DIV_Y, LCD_WIDTH - 24, 1, LINEN_BORDER);
}

/* One list row (menus.jsx Row) at list origin `y0`. Selected = filled ink bar +
 * light bold text; greyed = muted (inactive menu item). Optional sub-line, right
 * value, chevron, and a leading 22x22 art chip (RGB565, or NULL). */
void ui_list_row(int y0, int r, const char *text, const char *sub,
                        const char *right, int chevron, int selected, int greyed,
                        const uint16_t *chip, int title_priority, int rh)
{
    int ry = y0 + r * rh;
    int rowmid = ry + rh / 2 + 3;         /* vertical centre for value/chevron   */
    uint16_t fg, subc, rightc, chevc;
    if (selected) {
        ui_round_rect(6, ry + 1, LCD_WIDTH - 16, rh - 2, 4, LINEN_SEL_BG);
        fg = LINEN_SEL_FG; subc = LINEN_SEL_SUB; rightc = LINEN_SEL_SUB;
        chevc = LINEN_SEL_SUB;
    } else {
        fg = greyed ? LINEN_MUTED : LINEN_INK;
        subc = LINEN_MUTED2; rightc = LINEN_MUTED_D; chevc = LINEN_CHEVRON;
    }

    int tx = 14;
    if (chip) {
        int cd = ARTCACHE_DIM;                 /* cached chip is cd x cd RGB565 */
        int cy = ry + (rh - cd) / 2;
        console_blit565(12, cy, cd, cd, chip);
        /* Round the chip's corners (~2px, menus.jsx Chip radius) by knocking the
         * outer corner pixels back to the row background. Written straight into
         * the framebuffer: these are 12 single pixels, and a 1x1 console_fill_rect
         * each ran the whole clamp preamble per pixel, per row, per frame. The
         * chip sits inside the blit above, so it is on-panel by construction —
         * bounds are still checked once for the row band. */
        uint16_t cbg = selected ? LINEN_SEL_BG : LINEN_SURFACE;
        if (cy >= 0 && cy + cd <= LCD_HEIGHT) {
            uint16_t *fb = console_fb();
            for (int dy = 0; dy < 2; dy++) {
                uint16_t *top = &fb[(cy + dy) * LCD_WIDTH + 12];
                uint16_t *bot = &fb[(cy + cd - 1 - dy) * LCD_WIDTH + 12];
                for (int dx = 0; dx < 2 - dy; dx++) {
                    top[dx] = top[cd - 1 - dx] = cbg;
                    bot[dx] = bot[cd - 1 - dx] = cbg;
                }
            }
        }
        tx = 12 + cd + 8;
    }
    const text_font_t *tf = selected ? FONT_HEADER : FONT_ROW;
    /* Where the title must stop (before the right value / chevron). Measure the
     * right-hand value ONCE — it used to be walked twice per row per frame. */
    int title_right;
    int show_right = (right && right[0]);
    int right_w    = show_right ? text_width(right, text_font_bold_12()) : 0;
    if (show_right) {
        int reserved = LCD_WIDTH - 16 - right_w - 6;
        /* title_priority: the title owns the row. Reserve the value column only
         * while the title still fits inside it; once it's too long the title
         * spans the full width (over where the value was) and the value drops —
         * so it clips at the row edge and marquees on select, instead of being
         * cramped into a short column. */
        if (title_priority && text_width(text, tf) > reserved - tx) {
            title_right = LCD_WIDTH - 16;
            show_right = 0;
        } else {
            title_right = reserved;
        }
    } else if (chevron) {
        title_right = LCD_WIDTH - 18 - 4;
    } else {
        title_right = LCD_WIDTH - 16;
    }
    int avail = title_right - tx;
    /* Two-line rows: title sits a little below the row top (there's room in the
     * 32px row) with the artist near the bottom; single-line rows centre it. */
    int base  = sub ? ry + 16 : rowmid;
    int sub_y = ry + rh - 4;                   /* artist baseline, near row bottom */
    /* Marquee vertical clip. Top = the selection bar's interior (ry+1), NOT
     * y-ascent: the font ascent (14) reaches 1px ABOVE the bar, and the clear
     * would paint a sel-coloured line there ("clips above"). Bottom = just above
     * the artist baseline's cap (sub_y-10) so the scroll tick's clear can't erase
     * the static artist line. Single-line rows clip to the bar bottom. */
    int mqy0 = ry + 1;
    int mqy1 = sub ? (sub_y - 10) : (ry + rh - 1);
    if (selected) {
        row_title(tx, base, avail, text, tf, fg, LINEN_SEL_BG, mqy0, mqy1);
    } else {
        ui_text_clip(tx, base, text, tf, fg, tx, tx + avail);
    }
    if (sub) {
        ui_text(tx, sub_y, sub, FONT_SMALL, subc);
    }
    if (show_right) {
        ui_text(LCD_WIDTH - 16 - right_w, rowmid, right, text_font_bold_12(),
                rightc);
    } else if (chevron) {
        ui_text(LCD_WIDTH - 18, rowmid, UI_GLYPH_RAQUO, FONT_ROW, chevc);
    }
}

void ui_scrollbar(int y0, int top, int visible, int total)
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
int ui_scroll_window(int sel, int total, int visible)
{
    if (total <= visible) return 0;
    int start = sel - visible / 3;
    if (start < 0) start = 0;
    if (start > total - visible) start = total - visible;
    return start;
}
