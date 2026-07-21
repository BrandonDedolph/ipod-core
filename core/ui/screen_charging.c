/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/screen_charging.c — full-screen "charging" battery view.
 *
 * Draws design_reference/system-screens.jsx ChargingScreen with the same
 * primitives the menu UI uses (console_fill_rect for shapes, the Nunito
 * text renderer for type). Pure integer math into the RGB565 console
 * framebuffer; no hardware access, no present (see screen_charging.h).
 *
 * Layout (320x240): a 150x68 battery outline centred horizontally in the
 * upper-middle with a nub on the right and a pct-proportional inner fill;
 * a lightning bolt cut into the fill (in the background colour) while
 * charging; a big percent number (bold 17) centred below with a smaller
 * muted "%"; then a one-line status in bold 11.
 */

#include "screen_charging.h"

#include "text.h"
#include "../kernel/console.h"
#include "../hal/hal.h"          /* LCD_WIDTH / LCD_HEIGHT */

/* ---------------------------------------------------------------------------
 * Palette (system-screens.jsx ChargingScreen tokens -> RGB565)
 * ------------------------------------------------------------------------- */
#define CHG_BG      0x0861u      /* #0e0d0c near-black background            */
#define CHG_OUTLINE 0x5A89u      /* #5a5048 battery outline + nub            */
#define CHG_FILL    0xEF3Bu      /* #e8e4dd light fill (normal, not low)     */
#define CHG_GREEN   0x3E4Du      /* charging fill (oklch(0.78 0.16 145))     */
#define CHG_RED     0xDA46u      /* low-battery fill (oklch(0.65 0.18 30))   */
#define CHG_TEXT    0xEF3Bu      /* #e8e4dd big percent digits               */
#define CHG_UNIT    0xACF2u      /* #a89e92 muted "%" unit                   */
#define CHG_MUTED   0x7B8Du      /* #7a736a muted status / "not charging"    */

/* Battery geometry. Centred horizontally: body 150 wide -> the panel centre
 * (160) is the battery centre, so the lightning bolt lands dead-centre. */
#define BATT_W   150             /* outline width                            */
#define BATT_H    68             /* outline height                           */
#define BATT_X    ((LCD_WIDTH - BATT_W) / 2)   /* 85                          */
#define BATT_Y    56             /* outline top (upper-middle)               */
#define BATT_T     3             /* outline stroke thickness                 */
#define BATT_INSET 8             /* gap from outline to the fill             */
#define NUB_W      6
#define NUB_H     24

#define PCT_BASE  168            /* big percent baseline                     */
#define STAT_BASE 196            /* status line baseline                     */

/* Clamp a raw charge reading to a drawable 0..100 percent. */
static int clamp_pct(int pct)
{
    if (pct < 0)   return 0;
    if (pct > 100) return 100;
    return pct;
}

/* Inner fill width for `pct` over an interior of `inner_w` pixels. Mirrors the
 * design's Math.max(8, ...): always show a small stub so an empty/near-empty
 * pack still reads as a battery rather than a bare outline. */
static int fill_width(int pct, int inner_w)
{
    int fw = (inner_w * clamp_pct(pct)) / 100;
    if (fw < 8) fw = 8;
    if (fw > inner_w) fw = inner_w;
    return fw;
}

/* Render a small unsigned value (0..999) as decimal into `dst`; returns the
 * digit count. `dst` needs >= 4 bytes. */
static int u_to_str(char *dst, int v)
{
    if (v < 0) v = 0;
    char tmp[4];
    int t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 3);
    int i = 0;
    while (t > 0) dst[i++] = tmp[--t];
    dst[i] = '\0';
    return i;
}

/* Draw a NUL-terminated string; thin wrapper over text_draw with the panel
 * dimensions baked in (mirrors kernel/main.c's ui_text). `y` is the baseline. */
static int chg_text(int x, int y, const char *s, const text_font_t *font,
                    uint16_t ink)
{
    return text_draw(console_fb(), LCD_WIDTH, LCD_HEIGHT, x, y, s, font, ink);
}

/* Rectangular battery outline (four strokes) with the outer corner pixels
 * knocked back to the background — a cheap hint of rounded corners. */
static void draw_battery_outline(int x, int y, int w, int h, int t, uint16_t c)
{
    console_fill_rect(x,             y,             w, t, c);   /* top    */
    console_fill_rect(x,             y + h - t,     w, t, c);   /* bottom */
    console_fill_rect(x,             y,             t, h, c);   /* left   */
    console_fill_rect(x + w - t,     y,             t, h, c);   /* right  */
    /* Soften the four outer corners. */
    console_fill_rect(x,             y,             1, 1, CHG_BG);
    console_fill_rect(x + w - 1,     y,             1, 1, CHG_BG);
    console_fill_rect(x,             y + h - 1,     1, 1, CHG_BG);
    console_fill_rect(x + w - 1,     y + h - 1,     1, 1, CHG_BG);
}

/* Lightning bolt cut into the fill (drawn in the background colour, as the
 * design does): two thick strokes that both slant down-left, with the x
 * snapping back to the right at the midpoint — the classic zig-zag "⚡".
 * `cx` is the horizontal centre, [y0, y0+h) the vertical span. */
static void draw_bolt(int cx, int y0, int h, uint16_t c)
{
    const int th = 7;                 /* stroke thickness                     */
    const int swing = 12;             /* left/right travel over one stroke    */
    int half = h / 2;
    if (half < 1) half = 1;
    int low = h - half;
    if (low < 1) low = 1;

    for (int dy = 0; dy < half; dy++) {
        int x = cx + swing / 2 - (swing * dy) / half;   /* +6 -> -6 */
        console_fill_rect(x, y0 + dy, th, 1, c);
    }
    for (int dy = 0; dy < low; dy++) {
        int x = cx + swing / 2 - (swing * dy) / low;    /* snaps back to +6 */
        console_fill_rect(x, y0 + half + dy, th, 1, c);
    }
}

void screen_charging_render(int pct, int charging, int external)
{
    pct = clamp_pct(pct);

    console_clear(CHG_BG);

    /* Fill colour: green while charging, red when low (and not charging),
     * else the light "full" tone (system-screens.jsx). */
    uint16_t fill = charging ? CHG_GREEN : (pct < 20 ? CHG_RED : CHG_FILL);

    draw_battery_outline(BATT_X, BATT_Y, BATT_W, BATT_H, BATT_T, CHG_OUTLINE);

    /* Terminal nub on the right, vertically centred. */
    console_fill_rect(BATT_X + BATT_W, BATT_Y + (BATT_H - NUB_H) / 2,
                      NUB_W, NUB_H, CHG_OUTLINE);

    /* Proportional inner fill. */
    int ix = BATT_X + BATT_INSET;
    int iy = BATT_Y + BATT_INSET;
    int iw = BATT_W - 2 * BATT_INSET;
    int ih = BATT_H - 2 * BATT_INSET;
    int fw = fill_width(pct, iw);
    console_fill_rect(ix, iy, fw, ih, fill);

    /* Lightning bolt cut into the fill while charging. */
    if (charging) {
        draw_bolt(LCD_WIDTH / 2, iy + 4, ih - 8, CHG_BG);
    }

    /* Big percent number, centred, with a smaller muted "%". */
    char num[4];
    u_to_str(num, pct);
    const text_font_t *big  = text_font_bold_17();
    const text_font_t *unit = text_font_bold_13();
    int wn = text_width(num, big);
    int wu = text_width("%", unit);
    int total = wn + 2 + wu;
    int nx = (LCD_WIDTH - total) / 2;
    chg_text(nx, PCT_BASE, num, big, CHG_TEXT);
    chg_text(nx + wn + 2, PCT_BASE, "%", unit, CHG_UNIT);

    /* Status line: honest, no fake time estimate. */
    const char *status;
    uint16_t status_ink;
    if (charging) {
        status = "CHARGING";
        status_ink = CHG_GREEN;
    } else if (!external) {
        status = "CONNECT CABLE";
        status_ink = CHG_MUTED;
    } else {
        status = "NOT CHARGING";
        status_ink = CHG_MUTED;
    }
    const text_font_t *sfont = text_font_bold_11();
    int ws = text_width(status, sfont);
    chg_text((LCD_WIDTH - ws) / 2, STAT_BASE, status, sfont, status_ink);
}
