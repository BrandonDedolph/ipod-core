/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/tests/ui/text_test.c — host test for the freestanding text renderer.
 *
 * Renders a known string into a small RGB565 buffer over a known bg in a
 * known ink and asserts:
 *   (a) text_width > 0 and is additive (== sum of per-glyph substrings),
 *       and equals the pen advance returned by text_draw;
 *   (b) at least one interior pixel got blended toward the ink;
 *   (c) pixels fully outside glyph coverage are untouched;
 *   (d) drawing partly off every edge clips safely — no OOB write (checked
 *       with sentinel guard bytes bracketing the logical framebuffer).
 * Exit 0 on success, non-zero on the first failed check.
 */

#include "text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define W 48
#define H 32
#define PAD 64                    /* guard cells before and after the fb */
#define BG  0xFFFFu               /* white  */
#define INK 0x0000u               /* black  */
#define SENT 0xA55Au              /* sentinel guard value */

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); fails++; } \
} while (0)

int main(void) {
    const text_font_t *font = text_font_bold_17();
    CHECK(font != NULL, "font handle is NULL");

    /* ---- (a) width: positive, additive, matches draw advance ------- */
    int w_ag = text_width("Ag", font);
    int w_a  = text_width("A",  font);
    int w_g  = text_width("g",  font);
    CHECK(w_ag > 0, "text_width(\"Ag\") not positive");
    CHECK(w_a > 0 && w_g > 0, "single-glyph widths not positive");
    CHECK(w_ag == w_a + w_g, "text_width not additive over glyphs");
    CHECK(text_line_height(font) > 0, "line_height not positive");
    CHECK(text_ascent(font) > 0, "ascent not positive");

    /* ---- buffer with guard cells on both sides --------------------- */
    static uint16_t mem[PAD + W * H + PAD];
    for (size_t i = 0; i < sizeof(mem) / sizeof(mem[0]); i++) mem[i] = SENT;
    uint16_t *fb = &mem[PAD];
    for (int i = 0; i < W * H; i++) fb[i] = BG;

    /* Draw "Ag" with a comfortable margin from the edges. Baseline near
     * the bottom so the whole face fits. */
    int baseline = text_ascent(font) + 2;
    int pen = text_draw(fb, W, H, 4, baseline, "Ag", font, INK);
    CHECK(pen == 4 + w_ag, "text_draw advance != x + text_width");

    /* ---- (b) at least one pixel blended toward ink ----------------- */
    int changed = 0, toward_ink = 0;
    for (int i = 0; i < W * H; i++) {
        if (fb[i] != BG) {
            changed++;
            /* ink is black, bg white: any change darkens toward ink. */
            if (fb[i] < BG) toward_ink++;
        }
    }
    CHECK(changed > 0, "no pixels were drawn");
    CHECK(toward_ink > 0, "no pixel blended toward ink");

    /* An antialiased face must produce partial-coverage (grey) pixels,
     * not just fully-on ones — proves the gamma blend actually ran. */
    int grey = 0;
    for (int i = 0; i < W * H; i++) {
        if (fb[i] != BG && fb[i] != INK) grey++;
    }
    CHECK(grey > 0, "no antialiased (partial-coverage) pixels found");

    /* ---- (c) far corner untouched ---------------------------------- */
    CHECK(fb[(H - 1) * W + (W - 1)] == BG, "far corner unexpectedly modified");

    /* ---- (d) clip safety: draw partly off every edge --------------- */
    /* Re-fill, then hammer all four edges + a huge string. If any write
     * escaped [0,W)x[0,H) it would corrupt a guard cell. */
    for (int i = 0; i < W * H; i++) fb[i] = BG;
    text_draw(fb, W, H, -6, baseline, "Ag", font, INK);          /* off left  */
    text_draw(fb, W, H, W - 3, baseline, "Ag", font, INK);       /* off right */
    text_draw(fb, W, H, 4, -2, "Ag", font, INK);                 /* off top   */
    text_draw(fb, W, H, 4, H + 20, "Ag", font, INK);             /* off bottom*/
    text_draw(fb, W, H, -100, -100, "clipme", font, INK);        /* fully out */
    text_draw(fb, W, H, 0, baseline,
              "The quick brown fox jumps over 0123456789!",       /* overflow  */
              font, INK);

    int guard_ok = 1;
    for (int i = 0; i < PAD; i++) {
        if (mem[i] != SENT) guard_ok = 0;
        if (mem[PAD + W * H + i] != SENT) guard_ok = 0;
    }
    CHECK(guard_ok, "OOB write detected (guard cell clobbered)");

    if (fails) {
        fprintf(stderr, "text_test: %d check(s) failed\n", fails);
        return 1;
    }
    printf("text_test: OK (w_ag=%d, changed=%d, grey=%d)\n", w_ag, changed, grey);
    return 0;
}
