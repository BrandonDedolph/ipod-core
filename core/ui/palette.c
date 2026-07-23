/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/palette.c — the two theme palettes + the live-swap.
 *
 * Linen is the original warm-light theme (values lifted verbatim from the old
 * kernel/main.c LINEN_* and ui/screen_settings.c S_* #defines, so the light UI
 * is pixel-identical to before). Onyx is its warm-dark counterpart: the same
 * design language (terracotta accent, inverted high-contrast selection bar)
 * rendered on a warm charcoal surface with an off-white ink.
 *
 * Freestanding: designated-initialiser static tables + one block copy.
 */

#include "palette.h"

/* Warm-light (original). */
static const uint16_t PAL_LINEN[PAL_COUNT] = {
    [PAL_SURFACE]  = 0xF79Du,   /* #f4f1ec warm off-white   */
    [PAL_INK]      = 0x18A2u,   /* #1a1714 near-black       */
    [PAL_MUTED]    = 0x7B8Du,   /* #7a7068                  */
    [PAL_ACCENT]   = 0xC348u,   /* terracotta               */
    [PAL_BORDER]   = 0xE71Bu,
    [PAL_MUTED2]   = 0x9C70u,   /* #9a8e80                  */
    [PAL_MUTED_D]  = 0x5A89u,   /* #5a5048                  */
    [PAL_SEL_SUB]  = 0xB595u,
    [PAL_CHEVRON]  = 0xB575u,
    [PAL_SB_TRK]   = 0xE73Cu,
    [PAL_SB_THMB]  = 0xAD34u,
    [PAL_PLATE]    = 0xF7BEu,   /* warm cream plate         */
    [PAL_TRK]      = 0xDEDAu,   /* slider track (ink 0.10)  */
    [PAL_SEL_TRK]  = 0x41E7u,   /* track on a selected row  */
    [PAL_PILL_OFF] = 0xCE58u,
};

/* Warm-dark (Onyx). Surface/ink are the Linen ink/surface swapped and warmed;
 * the derived selection bar (ink behind surface text) therefore becomes a light
 * bar with dark text — the same inversion Linen has, just the other way up. */
static const uint16_t PAL_ONYX[PAL_COUNT] = {
    [PAL_SURFACE]  = 0x18C2u,   /* #1c1a17 warm charcoal    */
    [PAL_INK]      = 0xEF3Cu,   /* #ece7e0 warm off-white   */
    [PAL_MUTED]    = 0xACF2u,   /* #a89d90                  */
    [PAL_ACCENT]   = 0xC348u,   /* terracotta (unchanged)   */
    [PAL_BORDER]   = 0x3185u,   /* #35302b                  */
    [PAL_MUTED2]   = 0xB533u,   /* #b0a598                  */
    [PAL_MUTED_D]  = 0x8C0Eu,   /* #8a8075                  */
    [PAL_SEL_SUB]  = 0x5A89u,   /* dark sub text on light bar */
    [PAL_CHEVRON]  = 0x4A27u,   /* #4a443c                  */
    [PAL_SB_TRK]   = 0x2924u,   /* #2a2622                  */
    [PAL_SB_THMB]  = 0x6B0Bu,   /* #6a6258                  */
    [PAL_PLATE]    = 0x2944u,   /* raised dark plate        */
    [PAL_TRK]      = 0x39A5u,   /* #3a352f                  */
    [PAL_SEL_TRK]  = 0xCE16u,   /* light track on light bar */
    [PAL_PILL_OFF] = 0x39A5u,
};

uint16_t g_pal[PAL_COUNT] = {
    [PAL_SURFACE]  = 0xF79Du, [PAL_INK]      = 0x18A2u, [PAL_MUTED]  = 0x7B8Du,
    [PAL_ACCENT]   = 0xC348u, [PAL_BORDER]   = 0xE71Bu, [PAL_MUTED2] = 0x9C70u,
    [PAL_MUTED_D]  = 0x5A89u, [PAL_SEL_SUB]  = 0xB595u, [PAL_CHEVRON]= 0xB575u,
    [PAL_SB_TRK]   = 0xE73Cu, [PAL_SB_THMB]  = 0xAD34u, [PAL_PLATE]  = 0xF7BEu,
    [PAL_TRK]      = 0xDEDAu, [PAL_SEL_TRK]  = 0x41E7u, [PAL_PILL_OFF]= 0xCE58u,
};

void theme_set(int theme)
{
    const uint16_t *src = (theme == THEME_ONYX) ? PAL_ONYX : PAL_LINEN;
    for (int i = 0; i < PAL_COUNT; i++) {
        g_pal[i] = src[i];
    }
}
