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

    /* ---- 4b. ui_header: a long title must not smear into the count ----- *
     * The title used to be drawn full width and the "n / m" value painted on
     * top; the renderer alpha-blends, so both showed through each other. On
     * the surface colour (as on the device) so a blended MUTED2 pixel cannot
     * coincidentally equal INK. The header band is [HDR_BASE-14, HDR_BASE+5)
     * for bold 13 (ascent 14, descent 5). */
    {
        char longt[301];
        memset(longt, 'M', 300);
        longt[300] = '\0';
        const char *cnt = "3 / 12";
        int rw = text_width(cnt, FONT_SMALL);
        int box_x = LCD_WIDTH - 12 - rw - 8;          /* count + its 8 px gap */
        int band_y = HDR_BASE - 14, band_h = 19;

        console_clear(LINEN_SURFACE);
        ui_header(longt, cnt, 1);
        xpect(&c, "long header title leaves the count column free of ink",
              count_in(box_x, band_y, LCD_WIDTH - box_x, band_h, LINEN_INK) == 0);
        /* Regular 9 is thin enough that no pixel reaches full coverage, so
         * "anything but the surface" is the honest test for the count. */
        xpect(&c, "...and the count is actually drawn there",
              (LCD_WIDTH - box_x) * band_h -
                  count_in(box_x, band_y, LCD_WIDTH - box_x, band_h, LINEN_SURFACE)
              > 0);
        /* The title runs right up to its limit (not over-trimmed): the last
         * ink column is inside the 24 px before the box — one 'M' (11 px) plus
         * the ellipsis (10 px) of slack at most. */
        {
            const uint16_t *fb = console_framebuffer();
            int last_ink = -1;
            for (int yy = band_y; yy < band_y + band_h; yy++) {
                for (int xx = 0; xx < LCD_WIDTH; xx++) {
                    if (fb[yy * LCD_WIDTH + xx] == LINEN_INK && xx > last_ink) {
                        last_ink = xx;
                    }
                }
            }
            xpect(&c, "the ellipsis lands just left of the count column",
                  last_ink >= box_x - 24 && last_ink < box_x);
        }
        /* No count: the title may use the full width to the 12 px margin. */
        console_clear(LINEN_SURFACE);
        ui_header(longt, NULL, 1);
        xpect(&c, "without a count the title stops at the right margin",
              count_in(LCD_WIDTH - 12, band_y, 12, band_h, LINEN_INK) == 0 &&
              count_in(LCD_WIDTH - 12 - 24, band_y, 24, band_h, LINEN_INK) > 0);
    }

    /* A SHORT title must render pixel-identically to the direct ui_text run
     * it always was — the ellipsis path must not touch a title that fits. */
    {
        static uint16_t snap[LCD_WIDTH * LCD_HEIGHT];
        console_clear(LINEN_SURFACE);
        ui_header("Artists", "3 / 12", 1);
        memcpy(snap, console_framebuffer(), sizeof snap);

        console_clear(LINEN_SURFACE);
        int x = ui_text(12, HDR_BASE, UI_GLYPH_LAQUO, FONT_HEADER, LINEN_MUTED2) + 4;
        ui_text(x, HDR_BASE, "Artists", FONT_HEADER, LINEN_INK);
        int rw = text_width("3 / 12", FONT_SMALL);
        ui_text(LCD_WIDTH - 12 - rw, HDR_BASE - 1, "3 / 12", FONT_SMALL, LINEN_MUTED2);
        console_fill_rect(12, HDR_DIV_Y, LCD_WIDTH - 24, 1, LINEN_BORDER);
        xpect(&c, "a short header title is pixel-identical to a plain ui_text",
              memcmp(snap, console_framebuffer(), sizeof snap) == 0);
    }

    /* ---- 4c. ui_text_ellipsis_fit: the measuring half, without pixels --- */
    {
        char buf[164];
        const char *ell = "\xE2\x80\xA6";
        /* Fits: returned verbatim, no ellipsis. */
        int n = ui_text_ellipsis_fit(buf, sizeof buf, "Artists", FONT_HEADER, 300);
        xpect(&c, "fit: a short string comes back verbatim",
              n == 7 && strcmp(buf, "Artists") == 0);
        /* Exactly at its width: still verbatim (<=, not <). */
        int w = text_width("Artists", FONT_HEADER);
        n = ui_text_ellipsis_fit(buf, sizeof buf, "Artists", FONT_HEADER, w);
        xpect(&c, "fit: a string exactly max_w wide is not shortened",
              n == 7 && strcmp(buf, "Artists") == 0);
        n = ui_text_ellipsis_fit(buf, sizeof buf, "Artists", FONT_HEADER, w - 1);
        xpect(&c, "fit: one pixel short and it IS shortened",
              n > 0 && n < 7 + 3 && strstr(buf, ell) != NULL);

        /* The real case: trailing space trimmed, width honoured. */
        const char *pres = "The Presidents of the United States of America";
        int bad = 0, over = 0, under = 0, spaces = 0;
        for (int mw = 12; mw <= 300; mw += 7) {
            n = ui_text_ellipsis_fit(buf, sizeof buf, pres, FONT_HEADER, mw);
            if (n <= 0) { bad++; continue; }
            if (text_width(buf, FONT_HEADER) > mw) over++;
            /* Not over-trimmed: adding back the widest glyph (~14 px) plus a
             * space would overflow, so the result is within 20 px of max. */
            if (mw >= 40 && text_width(buf, FONT_HEADER) < mw - 20) under++;
            /* Ellipsis present, and never preceded by a space. */
            if (n < 3 || memcmp(buf + n - 3, ell, 3) != 0) bad++;
            if (n >= 4 && buf[n - 4] == ' ') spaces++;
        }
        xpect(&c, "fit: every shortened width is <= max_w", over == 0);
        xpect(&c, "fit: no width is over-trimmed", under == 0);
        xpect(&c, "fit: the ellipsis is always present and never after a space",
              bad == 0 && spaces == 0);
        n = ui_text_ellipsis_fit(buf, sizeof buf, pres, FONT_HEADER, 150);
        xpect(&c, "fit: 'The Presidents of the…' style cut at a word",
              strncmp(buf, "The Presidents", 14) == 0 && strchr(buf, ' ') != NULL);

        /* UTF-8: "Björk " x 40 = 280 bytes (also longer than the buffer), so
         * the cut must land on a codepoint boundary at every width, and the
         * chosen prefix must parse as UTF-8 — a stray continuation byte would
         * draw a hollow .notdef box. */
        char bj[281];
        bj[0] = '\0';
        for (int i = 0; i < 40; i++) strcat(bj, "Bj\xC3\xB6rk ");
        /* "Never ends on a continuation byte" is the wrong test: a prefix
         * that ends in a complete "ö" (C3 B6) ends on one legitimately. The
         * property is that the cut never lands INSIDE a sequence — i.e. the
         * prefix parses: every lead byte has its continuation bytes, every
         * continuation byte has a lead, and a 2-byte lead is never the last
         * byte. That is what a byte-wise cut would break. */
        int malformed = 0, none = 0, cut_o = 0;
        for (int mw = 12; mw <= 300; mw += 5) {
            n = ui_text_ellipsis_fit(buf, sizeof buf, bj, FONT_HEADER, mw);
            if (n <= 0) { none++; continue; }
            int plen = n - 3;                       /* bytes before the … */
            for (int i = 0; i < plen; ) {
                unsigned char b = (unsigned char)buf[i];
                int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : 0;
                if (len == 0 || i + len > plen) { malformed++; break; }
                for (int k = 1; k < len; k++) {
                    if (((unsigned char)buf[i + k] & 0xC0) != 0x80) malformed++;
                }
                i += len;
            }
            /* A cut that split "ö" would leave a bare C3 as the last byte. */
            if (plen > 0 && (unsigned char)buf[plen - 1] == 0xC3) cut_o++;
        }
        xpect(&c, "fit: a UTF-8 title is never cut inside a sequence",
              malformed == 0 && cut_o == 0 && none == 0);
        /* And on the panel: nothing is drawn past x + max_w. */
        console_clear(LINEN_SURFACE);
        int end = ui_text_ellipsis(20, HDR_BASE, bj, FONT_HEADER, LINEN_INK, 200);
        xpect(&c, "ellipsis draw stays inside [x, x + max_w)",
              end <= 220 && count_in(220, HDR_BASE - 14, 100, 19, LINEN_INK) == 0 &&
              count_in(20, HDR_BASE - 14, 200, 19, LINEN_INK) > 0);

        /* Degenerate: nothing fits -> nothing drawn, pen unchanged. */
        console_clear(LINEN_SURFACE);
        end = ui_text_ellipsis(20, HDR_BASE, pres, FONT_HEADER, LINEN_INK, 3);
        xpect(&c, "ellipsis with no room draws nothing",
              end == 20 && count_in(0, 0, LCD_WIDTH, LCD_HEIGHT, LINEN_INK) == 0);
        n = ui_text_ellipsis_fit(buf, sizeof buf, "", FONT_HEADER, 100);
        xpect(&c, "fit: an empty string is empty", n == 0 && buf[0] == '\0');
    }

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
