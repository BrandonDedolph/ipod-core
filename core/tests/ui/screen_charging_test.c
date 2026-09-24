/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/screen_charging_test.c — Hold on the charging screen, on the host.
 *
 * The bug this pins: sliding Hold while the charging screen was up ran the
 * ordinary Hold banner over it — a linen strip across the top of the dark
 * field with the STATUS STRIP's little battery token blinking in for a
 * second, above a screen that is already one big battery. main.c now hosts
 * no banner on the dark modals (banner_hosted) and paints the corner padlock
 * instead. main.c's loop cannot run here, so what the framebuffer proves is
 * the rendering half of that contract, on the SAME source the ARM build
 * links:
 *
 *   - the charging screen's top band is bare field — there is no strip and no
 *     battery token up there for a banner to "belong" to;
 *   - the corner padlock lands in the design's top-right token box
 *     (system-screens.jsx ChargingScreen: top 8, right 12) in the muted tone,
 *     is exactly the strip glyph's 72 pixels, and changes NOTHING else — on
 *     the charging field and on its low-battery sibling, which shares the
 *     treatment;
 *   - the padlock survives the caption (screen_charging_note clears its own
 *     band only), since main.c draws it last.
 *
 * The framebuffer is the oracle, as in screen_battery_test.c: a sentinel
 * colour first, pixels read back after.
 */

#include <stdio.h>
#include <string.h>

#include "screen_charging.h"
#include "screen_battery.h"
#include "palette.h"
#include "text.h"               /* UI_GLYPH_MIDDOT */
#include "console.h"
#include "hal.h"

#include "../xfail.h"

#define FB_PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define SENTINEL  0xF81Fu        /* magenta: in no palette this UI uses */

/* The corner box, as screen_charging.c lays it out. */
#define LOCK_W   8
#define LOCK_H   10
#define LOCK_X   (LCD_WIDTH - 12 - LOCK_W)
#define LOCK_Y   8
/* body 8x6 + two 2x5 posts + a 6x2 arch, less the arch's overlap with the
 * posts' top two rows and the posts' overlap with the body's top row:
 * 48 + 10 + 10 + 12 - 8 - 4. */
#define LOCK_PIXELS 68

/* Count pixels equal to `c` inside the box (x,y,w,h). */
static int count_in(int x, int y, int w, int h, uint16_t c)
{
    const uint16_t *fb = console_framebuffer();
    int n = 0;
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            if (yy < 0 || xx < 0 || yy >= LCD_HEIGHT || xx >= LCD_WIDTH) continue;
            if (fb[yy * LCD_WIDTH + xx] == c) n++;
        }
    }
    return n;
}

/* Pixels that are NOT `bg` inside a band — "is there anything here". */
static int count_not(int x, int y, int w, int h, uint16_t bg)
{
    const uint16_t *fb = console_framebuffer();
    int n = 0;
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            if (yy < 0 || xx < 0 || yy >= LCD_HEIGHT || xx >= LCD_WIDTH) continue;
            if (fb[yy * LCD_WIDTH + xx] != bg) n++;
        }
    }
    return n;
}

/* Pixels that differ between `ref` and the framebuffer OUTSIDE the box. */
static int diff_outside(const uint16_t *ref, int x, int y, int w, int h)
{
    const uint16_t *fb = console_framebuffer();
    int n = 0;
    for (int yy = 0; yy < LCD_HEIGHT; yy++) {
        for (int xx = 0; xx < LCD_WIDTH; xx++) {
            int inside = (xx >= x && xx < x + w && yy >= y && yy < y + h);
            if (!inside && fb[yy * LCD_WIDTH + xx] != ref[yy * LCD_WIDTH + xx]) n++;
        }
    }
    return n;
}

static uint16_t g_ref[FB_PIXELS];

/* Paint a field (charging or low-battery) unlocked into g_ref, then again
 * with the padlock, and check the padlock against the design box and the
 * reference. `what` names the field in the labels. */
static void check_lock_on(xfail_ctx *c, const char *what, void (*paint)(void))
{
    char label[96];

    console_clear(SENTINEL);
    paint();
    memcpy(g_ref, console_framebuffer(), sizeof g_ref);
    snprintf(label, sizeof label, "%s: paints the whole panel", what);
    xpect(c, label, count_in(0, 0, LCD_WIDTH, LCD_HEIGHT, SENTINEL) == 0);
    /* The top band — where the banner's 22 px strip + rule would go — is
     * bare field: no strip, no battery token, nothing for a banner to
     * "re-colour". */
    snprintf(label, sizeof label, "%s: the top 23 rows are bare field", what);
    xpect(c, label, count_not(0, 0, LCD_WIDTH, 23, CHG_BG) == 0);

    console_clear(SENTINEL);
    paint();
    screen_charging_lock_render();
    snprintf(label, sizeof label, "%s: the padlock is the muted tone, %d px, in its box",
             what, LOCK_PIXELS);
    xpect(c, label, count_in(LOCK_X, LOCK_Y, LOCK_W, LOCK_H, CHG_MUTED) == LOCK_PIXELS);
    snprintf(label, sizeof label, "%s: the box holds only padlock and field", what);
    xpect(c, label, count_not(LOCK_X, LOCK_Y, LOCK_W, LOCK_H, CHG_BG) == LOCK_PIXELS);
    snprintf(label, sizeof label, "%s: the padlock changes nothing outside its box", what);
    xpect(c, label, diff_outside(g_ref, LOCK_X, LOCK_Y, LOCK_W, LOCK_H) == 0);
    /* The design's corner: 12 px from the right edge, 8 from the top. */
    snprintf(label, sizeof label, "%s: the box sits at the design's corner", what);
    xpect(c, label, LOCK_X + LOCK_W == LCD_WIDTH - 12 && LOCK_Y == 8);
}

static void paint_charging(void)
{
    screen_charging_render(64, 1, 1);
    screen_charging_note("3978 mV " UI_GLYPH_MIDDOT " 500 mA");
}

static void paint_charging_full(void)
{
    screen_charging_render(100, 0, 1);      /* CHARGED: the light fill */
}

static void paint_low(void)
{
    screen_battery_render(BATTWARN_DISKSAFE);
}

int main(void)
{
    xfail_ctx c = { "screen_charging", 0, 0, 0 };
    theme_set(THEME_LINEN);                 /* screen_battery.c's toast palette */

    check_lock_on(&c, "charging", paint_charging);
    check_lock_on(&c, "charged", paint_charging_full);
    check_lock_on(&c, "low battery", paint_low);

    /* Order of painting in main.c: render, caption, then the padlock. The
     * caption band (rows 210..223) is nowhere near the corner, so the glyph
     * survives a caption repaint either way — pin that so a taller caption
     * band cannot quietly eat it. */
    console_clear(SENTINEL);
    screen_charging_render(42, 1, 1);
    screen_charging_lock_render();
    screen_charging_note("3912 mV " UI_GLYPH_MIDDOT " 500 mA " UI_GLYPH_MIDDOT
                         " +38 mV in 12 min");
    xpect(&c, "the caption does not clear the padlock",
          count_in(LOCK_X, LOCK_Y, LOCK_W, LOCK_H, CHG_MUTED) == LOCK_PIXELS);
    /* And the padlock is off the big battery entirely (it sits above it). */
    xpect(&c, "the padlock sits above the battery glyph",
          LOCK_Y + LOCK_H < 56);

    return xfail_done(&c);
}
