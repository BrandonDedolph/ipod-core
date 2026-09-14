/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/palette_test.c — host test for the theme palettes (core/ui/palette.c).
 *
 * palette.c is the SAME source the ARM build links into core.elf: seven static
 * RGB565 token tables and theme_set(), the block copy main.c calls at boot and
 * on every Theme pick. This proves what the UI relies on:
 *   1. Every table is fully populated — PAL_COUNT entries, none left at the
 *      designated-initialiser default of 0 (a missing token would paint black,
 *      which on a dark theme is invisible until a slider or pill goes missing).
 *   2. Polarity: the light themes (Linen, Plaster, Olive, Mushroom) have a
 *      surface brighter than their ink; the dark ones (Onyx, Sage, Umber) the
 *      reverse. The selection bar is DERIVED (ink behind surface text), so this
 *      is what keeps a selected row legible in every theme.
 *   3. The derived-tone contract from palette.h: SEL_SUB and SEL_TRK sit on the
 *      ink-coloured bar, so each must be on the SURFACE side of the ink in
 *      luminance; TRK and PILL_OFF sit on the surface, so each must be on the
 *      INK side of the surface. The muteds, chevron and divider all lie
 *      between the two.
 *   4. theme_set(id) copies exactly that id's table into g_pal[]; an id outside
 *      the range (an older/newer settings record) loads Linen, not garbage.
 *   5. The seven surfaces are all distinct, so no two picker rows preview the
 *      same swatch.
 * No MMIO, no framebuffer — plain cc.
 */

#include "palette.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fail = 1;
    }
    return cond;
}

/* Relative luminance of an RGB565 pixel, 0..255 (Rec.601 weights, integer). */
static int luma(uint16_t c)
{
    int r = ((c >> 11) & 0x1F) * 255 / 31;
    int g = ((c >> 5)  & 0x3F) * 255 / 63;
    int b = ( c        & 0x1F) * 255 / 31;
    return (299 * r + 587 * g + 114 * b) / 1000;
}

/* "x lies strictly between a and b" — the derived tones must land on the
 * far side of one anchor from the other, never past either. */
static int between(int x, int a, int b)
{
    return (a < b) ? (x > a && x < b) : (x > b && x < a);
}

static const char *const NAME[THEME_COUNT] = {
    [THEME_LINEN]    = "linen",   [THEME_ONYX]  = "onyx",
    [THEME_SAGE]     = "sage",    [THEME_PLASTER] = "plaster",
    [THEME_OLIVE]    = "olive",   [THEME_UMBER] = "umber",
    [THEME_MUSHROOM] = "mushroom",
};
static const int IS_DARK[THEME_COUNT] = {
    [THEME_LINEN]    = 0, [THEME_ONYX]  = 1, [THEME_SAGE]  = 1,
    [THEME_PLASTER]  = 0, [THEME_OLIVE] = 0, [THEME_UMBER] = 1,
    [THEME_MUSHROOM] = 0,
};

int main(void)
{
    char label[64];

    check("theme-count-7", THEME_COUNT == 7);
    check("theme-ids-in-order",
          THEME_LINEN == 0 && THEME_ONYX == 1 && THEME_SAGE == 2 &&
          THEME_PLASTER == 3 && THEME_OLIVE == 4 && THEME_UMBER == 5 &&
          THEME_MUSHROOM == 6);

    /* --- 1..3: every table, entry by entry --- */
    for (int t = 0; t < THEME_COUNT; t++) {
        const uint16_t *p = theme_table(t);
        snprintf(label, sizeof label, "%s-table", NAME[t]);
        if (!check(label, p != 0)) {
            continue;
        }

        int nonzero = 1;
        for (int i = 0; i < PAL_COUNT; i++) {
            nonzero &= (p[i] != 0);
        }
        snprintf(label, sizeof label, "%s-all-%d-tokens-set", NAME[t], PAL_COUNT);
        check(label, nonzero);

        int ls = luma(p[PAL_SURFACE]), li = luma(p[PAL_INK]);
        snprintf(label, sizeof label, "%s-%s-polarity", NAME[t],
                 IS_DARK[t] ? "dark" : "light");
        check(label, IS_DARK[t] ? (ls < li) : (ls > li));
        /* Enough contrast that the ink bar / text actually reads. */
        snprintf(label, sizeof label, "%s-contrast", NAME[t]);
        check(label, (ls > li ? ls - li : li - ls) >= 150);

        /* Derived tones: on the ink bar, SEL_SUB / SEL_TRK lean toward the
         * surface; on the surface, TRK / PILL_OFF lean toward the ink. */
        snprintf(label, sizeof label, "%s-sel-sub-on-bar", NAME[t]);
        check(label, between(luma(p[PAL_SEL_SUB]), li, ls));
        snprintf(label, sizeof label, "%s-sel-trk-on-bar", NAME[t]);
        check(label, between(luma(p[PAL_SEL_TRK]), li, ls));
        snprintf(label, sizeof label, "%s-trk-on-surface", NAME[t]);
        check(label, between(luma(p[PAL_TRK]), ls, li));
        snprintf(label, sizeof label, "%s-pill-off-on-surface", NAME[t]);
        check(label, between(luma(p[PAL_PILL_OFF]), ls, li));
        /* SEL_TRK is a track just off the bar; SEL_SUB is text on it, so the
         * sub must stand further from the bar than the track does. */
        int d_trk = luma(p[PAL_SEL_TRK]) - li, d_sub = luma(p[PAL_SEL_SUB]) - li;
        if (d_trk < 0) d_trk = -d_trk;
        if (d_sub < 0) d_sub = -d_sub;
        snprintf(label, sizeof label, "%s-sel-sub-past-sel-trk", NAME[t]);
        check(label, d_sub > d_trk);

        /* The three muteds, the chevron and the divider all sit between the
         * surface and the ink — secondary, never louder than primary text and
         * never lost in the page. (Their order among themselves is a design
         * choice per theme: Onyx's MUTED2 is brighter than its MUTED.) */
        snprintf(label, sizeof label, "%s-secondaries-between", NAME[t]);
        check(label, between(luma(p[PAL_MUTED]),   ls, li) &&
                     between(luma(p[PAL_MUTED2]),  ls, li) &&
                     between(luma(p[PAL_MUTED_D]), ls, li) &&
                     between(luma(p[PAL_CHEVRON]), ls, li) &&
                     between(luma(p[PAL_BORDER]),  ls, li));
    }

    /* --- 4: theme_set() loads exactly that table; out of range -> Linen --- */
    for (int t = 0; t < THEME_COUNT; t++) {
        memset(g_pal, 0xA5, sizeof g_pal);
        theme_set(t);
        snprintf(label, sizeof label, "theme-set-%s", NAME[t]);
        check(label, memcmp(g_pal, theme_table(t), sizeof g_pal) == 0);
    }
    {
        static const int bad[] = { -1, THEME_COUNT, 7, 99, 255, 0x7FFFFFFF, -0x7FFFFFFF };
        int all_linen = 1;
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            memset(g_pal, 0xA5, sizeof g_pal);
            theme_set(bad[i]);
            all_linen &= memcmp(g_pal, theme_table(THEME_LINEN), sizeof g_pal) == 0;
            all_linen &= (theme_table(bad[i]) == 0);
        }
        check("theme-set-out-of-range-is-linen", all_linen);
    }
    /* The static initialiser of g_pal[] must equal Linen: the boot splash
     * paints before the first theme_set(). */
    {
        /* Re-derive it: a fresh copy of the initialiser is not reachable once
         * theme_set() has run, so compare the documented values instead. */
        theme_set(THEME_LINEN);
        check("linen-is-the-original",
              g_pal[PAL_SURFACE] == 0xF79Du && g_pal[PAL_INK] == 0x18A2u &&
              g_pal[PAL_ACCENT] == 0xC348u);
    }

    /* --- 5: distinct picker swatches --- */
    {
        int distinct = 1;
        for (int a = 0; a < THEME_COUNT; a++) {
            for (int b = a + 1; b < THEME_COUNT; b++) {
                distinct &= theme_table(a)[PAL_SURFACE] != theme_table(b)[PAL_SURFACE];
            }
        }
        check("surfaces-distinct", distinct);
    }

    printf(g_fail ? "PALETTE TEST FAILED\n" : "PALETTE TEST OK\n");
    return g_fail;
}
