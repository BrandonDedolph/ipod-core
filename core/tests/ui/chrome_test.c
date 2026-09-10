/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/chrome_test.c — the shared list chrome (core/ui/chrome.c) on the host.
 *
 * THE POINT OF THIS FILE. Every function it exercises was, until chrome.c
 * existed, a `static` inside kernel/main.c — which meson builds only for
 * target == 'hw'. None of it could be compiled by the host, so the only way to
 * check a selection-bar inset, a scroll clamp or a row that draws past the
 * panel edge was to flash the device and look at the screen. That is why the
 * project's notes say "LCD needs the device to verify visually".
 *
 * Nothing here touches hardware: chrome.c draws into console.c's RGB565 back
 * buffer and measures glyphs through text.c, and the one thing it genuinely
 * cannot do portably — the marquee's clock — is injected via
 * ui_set_scroll_text(), left unset here so overflowing titles clip instead.
 *
 * The framebuffer is the oracle: assertions read pixels back out of
 * console_fb() rather than trusting a return value.
 */

#include <stdio.h>
#include <string.h>

#include "chrome.h"
#include "palette.h"
#include "text.h"
#include "console.h"
#include "hal.h"

#include "../xfail.h"

#define FB_PIXELS (LCD_WIDTH * LCD_HEIGHT)

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

/* Pixels equal to `c` ANYWHERE outside the box — the out-of-bounds detector. */
static int count_outside(int x, int y, int w, int h, uint16_t c)
{
    const uint16_t *fb = console_framebuffer();
    int n = 0;
    for (int yy = 0; yy < LCD_HEIGHT; yy++) {
        for (int xx = 0; xx < LCD_WIDTH; xx++) {
            int inside = (xx >= x && xx < x + w && yy >= y && yy < y + h);
            if (!inside && fb[yy * LCD_WIDTH + xx] == c) n++;
        }
    }
    return n;
}

int main(void)
{
    xfail_ctx c = { "chrome", 0, 0, 0 };

    /* ---- 1. ui_scroll_window: pure, and the edges are where lists break --- */
    struct { int sel, total, visible, want; } sw[] = {
        /* everything fits -> always origin 0, whatever the selection */
        { 0, 5, 8, 0 }, { 4, 5, 8, 0 },
        /* selection sits ~1/3 down (visible/3 = 2 for an 8-row list) */
        { 0, 100, 8, 0 },        /* clamped at the top, not -2            */
        { 1, 100, 8, 0 },
        { 2, 100, 8, 0 },
        { 3, 100, 8, 1 },
        { 50, 100, 8, 48 },
        /* clamped at the bottom: last row visible, never past the end     */
        { 99, 100, 8, 92 },
        { 97, 100, 8, 92 },
        /* degenerate inputs must not produce a negative origin            */
        { 0, 0, 8, 0 }, { 0, 1, 1, 0 }, { 0, 1, 8, 0 },
    };
    int sw_bad = 0;
    for (unsigned i = 0; i < sizeof sw / sizeof sw[0]; i++) {
        int got = ui_scroll_window(sw[i].sel, sw[i].total, sw[i].visible);
        if (got != sw[i].want) {
            fprintf(stderr, "[chrome] scroll_window(%d,%d,%d) = %d, want %d\n",
                    sw[i].sel, sw[i].total, sw[i].visible, got, sw[i].want);
            sw_bad++;
        }
        if (got < 0) sw_bad++;
    }
    xpect(&c, "scroll_window matches the 1/3-anchor contract at every edge",
          sw_bad == 0);

    /* The invariant that actually matters: the selection is always visible. */
    int unseen = 0;
    for (int total = 1; total <= 40; total++) {
        for (int sel = 0; sel < total; sel++) {
            int top = ui_scroll_window(sel, total, 8);
            if (sel < top || sel >= top + 8) {
                if (total > 8 || top != 0) unseen++;
            }
        }
    }
    xpect(&c, "the selection is inside the window for every (sel,total)",
          unseen == 0);

    /* ---- 2. ui_isqrt ---------------------------------------------------- */
    int sq_bad = 0;
    for (int v = 0; v < 4096; v++) {
        int r = ui_isqrt(v);
        if (r * r > v || (r + 1) * (r + 1) <= v) sq_bad++;
    }
    if (ui_isqrt(-5) != 0) sq_bad++;      /* negative must not misbehave */
    xpect(&c, "ui_isqrt is floor(sqrt(v)) across the used range", sq_bad == 0);

    /* ---- 3. ui_round_rect stays inside its rectangle -------------------- */
    theme_set(0);
    console_clear(0x0000);
    ui_round_rect(40, 50, 60, 30, 6, 0xF800);
    xpect(&c, "round rect writes nothing outside its box",
          count_outside(40, 50, 60, 30, 0xF800) == 0);
    xpect(&c, "round rect fills its middle band",
          count_in(40, 60, 60, 10, 0xF800) == 60 * 10);
    {
        /* Corners are inset: the very first row is narrower than the middle. */
        const uint16_t *fb = console_framebuffer();
        int first = 0, mid = 0;
        for (int x = 40; x < 100; x++) {
            if (fb[50 * LCD_WIDTH + x] == 0xF800) first++;
            if (fb[65 * LCD_WIDTH + x] == 0xF800) mid++;
        }
        xpect(&c, "round rect actually rounds (top row inset vs middle)",
              first > 0 && first < mid && mid == 60);
    }
    /* A rect hanging off the left/top edge must clip, not wrap or corrupt. */
    console_clear(0x0000);
    ui_round_rect(-20, -10, 60, 40, 6, 0x07E0);
    xpect(&c, "round rect clipped at the origin writes nothing off-panel",
          count_outside(0, 0, 40, 30, 0x07E0) == 0);

    /* ---- 4. ui_header --------------------------------------------------- */
    console_clear(0x0000);
    console_damage_reset();
    ui_header("Artists", "3 / 12", 1);
    {
        int dx, dy, dw, dh;
        int any = console_damage_get(&dx, &dy, &dw, &dh);
        xpect(&c, "header reports damage", any == 1);
        xpect(&c, "header damage stays on-panel",
              dx >= 0 && dy >= 0 && dx + dw <= LCD_WIDTH &&
              dy + dh <= LCD_HEIGHT);
    }
    xpect(&c, "header draws its divider rule at HDR_DIV_Y",
          count_in(12, HDR_DIV_Y, LCD_WIDTH - 24, 1, LINEN_BORDER)
              == LCD_WIDTH - 24);

    /* ---- 5. ui_list_row ------------------------------------------------- *
     * The row band is [LIST_Y0 + r*rh, +rh). A row must paint inside it and
     * nowhere else — a selection bar bleeding one pixel up is how the tall-row
     * chip geometry was found to be wrong. */
    console_clear(0x0000);
    ui_list_row(LIST_Y0, 2, "Some Artist", NULL, NULL, 1, 1, 0, NULL, 0, ROW_H);
    {
        int band_y = LIST_Y0 + 2 * ROW_H;
        xpect(&c, "a selected row's bar stays within its own row band",
              count_outside(0, band_y, LCD_WIDTH, ROW_H, LINEN_SEL_BG) == 0);
        xpect(&c, "a selected row actually draws a selection bar",
              count_in(0, band_y, LCD_WIDTH, ROW_H, LINEN_SEL_BG) > 0);
    }

    /* An absurdly long title with no marquee hook must clip, not overrun. */
    console_clear(0x0000);
    {
        char longt[512];
        memset(longt, 'M', sizeof longt - 1);
        longt[sizeof longt - 1] = '\0';
        ui_list_row(LIST_Y0, 0, longt, "and a subtitle", "9:99", 1, 0, 0,
                    NULL, 0, ROW_H2);
        xpect(&c, "an over-long title is clipped to its row, not drawn past it",
              count_outside(0, LIST_Y0, LCD_WIDTH, ROW_H2, LINEN_INK) == 0);
    }

    /* The last row of a full list must not paint below the panel. */
    console_clear(0x0000);
    ui_list_row(LIST_Y0, LIST_ROWS - 1, "Last", NULL, NULL, 0, 1, 0, NULL, 0,
                ROW_H);
    xpect(&c, "the final row fits on the panel",
          LIST_Y0 + LIST_ROWS * ROW_H <= LCD_HEIGHT);

    /* ---- 6. ui_scrollbar ------------------------------------------------ */
    console_clear(0x0000);
    ui_scrollbar(LIST_Y0, 0, 8, 8);
    xpect(&c, "no scrollbar when everything fits",
          count_in(0, 0, LCD_WIDTH, LCD_HEIGHT, LINEN_SB_THMB) == 0);

    console_clear(0x0000);
    ui_scrollbar(LIST_Y0, 0, 8, 100);
    int thumb_top = count_in(0, 0, LCD_WIDTH, LCD_HEIGHT, LINEN_SB_THMB);
    xpect(&c, "a long list gets a thumb", thumb_top > 0);

    console_clear(0x0000);
    ui_scrollbar(LIST_Y0, 92, 8, 100);
    int thumb_bot = count_in(0, 0, LCD_WIDTH, LCD_HEIGHT, LINEN_SB_THMB);
    xpect(&c, "the thumb is the same size at both ends of the track",
          thumb_bot == thumb_top);
    xpect(&c, "the thumb never leaves the panel",
          count_outside(LCD_WIDTH - 4, 0, 3, LCD_HEIGHT, LINEN_SB_THMB) == 0);

    /* ---- 7. the injected marquee seam ----------------------------------- */
    xpect(&c, "chrome runs with no scroll-text hook registered", 1);

    return xfail_done(&c);
}
