/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/palette.c — the theme palettes + the live-swap.
 *
 * Linen is the original warm-light theme (values lifted verbatim from the old
 * kernel/main.c LINEN_* and ui/screen_settings.c S_* #defines, so the light UI
 * is pixel-identical to before). Onyx is its warm-dark counterpart: the same
 * design language (terracotta accent, inverted high-contrast selection bar)
 * rendered on a warm charcoal surface with an off-white ink. The five that
 * followed (Sage, Plaster, Olive, Umber, Mushroom) each carry their own
 * owner-approved surface / ink / muted / accent set, and derive the rest the
 * same way Linen and Onyx do:
 *
 *   TRK       ink at 10 % over the surface (the resting slider track)
 *   SEL_SUB   ink at 30 % over the surface — on a light theme that is a light
 *             muted tone that reads on the dark ink bar; on a dark theme it is
 *             a dark muted tone that reads on the light ink bar
 *   SEL_TRK   surface at 16 % over the ink — a track just off the bar colour
 *   PILL_OFF  light themes: ink at 17 % over the surface (a shade deeper than
 *             TRK, as Linen's is); dark themes: the track itself, as Onyx's is
 *
 * Every entry carries its 24-bit source colour; the RGB565 is round-to-nearest
 * per channel.
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

/* Dark, muted green-grey ("Green Smoke" / "Pigeon" family) with a clay accent. */
static const uint16_t PAL_SAGE[PAL_COUNT] = {
    [PAL_SURFACE]  = 0x31A6u,   /* #2e3631 green-grey       */
    [PAL_INK]      = 0xEF3Bu,   /* #ebe8df warm off-white   */
    [PAL_MUTED]    = 0xAD74u,   /* #a9b0a6                  */
    [PAL_ACCENT]   = 0xC3CAu,   /* #c47a52 clay             */
    [PAL_BORDER]   = 0x3A28u,   /* #3d4640                  */
    [PAL_MUTED2]   = 0x7C2Fu,   /* #7f877e                  */
    [PAL_MUTED_D]  = 0xC657u,   /* #c4c9bf                  */
    [PAL_SEL_SUB]  = 0x6B4Cu,   /* #676b65 dark sub on light bar */
    [PAL_CHEVRON]  = 0x5B2Bu,   /* #5c655e                  */
    [PAL_SB_TRK]   = 0x3A07u,   /* #37403a                  */
    [PAL_SB_THMB]  = 0x6BCDu,   /* #6e786f                  */
    [PAL_PLATE]    = 0x3A07u,   /* #37403a raised plate     */
    [PAL_TRK]      = 0x4248u,   /* #414842 (ink 0.10)       */
    [PAL_SEL_TRK]  = 0xCE58u,   /* #cdccc3 light track on light bar */
    [PAL_PILL_OFF] = 0x4248u,   /* = TRK                    */
};

/* Light, pink-beige limewash ("Setting Plaster") with an oxblood accent. */
static const uint16_t PAL_PLASTER[PAL_COUNT] = {
    [PAL_SURFACE]  = 0xE6DAu,   /* #e9dcd2 pink-beige       */
    [PAL_INK]      = 0x3965u,   /* #3a2e2a warm dark brown  */
    [PAL_MUTED]    = 0x8B8Du,   /* #8a7168                  */
    [PAL_ACCENT]   = 0x8A06u,   /* #8f3f2f oxblood          */
    [PAL_BORDER]   = 0xDE57u,   /* #dccbbf                  */
    [PAL_MUTED2]   = 0xA491u,   /* #a89389                  */
    [PAL_MUTED_D]  = 0x5A48u,   /* #5c4a43                  */
    [PAL_SEL_SUB]  = 0xB553u,   /* #b4a8a0 light sub on ink bar */
    [PAL_CHEVRON]  = 0xB533u,   /* #b8a59a                  */
    [PAL_SB_TRK]   = 0xDE98u,   /* #e0d1c6                  */
    [PAL_SB_THMB]  = 0xB4F2u,   /* #b39c90                  */
    [PAL_PLATE]    = 0xEF3Bu,   /* #f1e7df raised plate     */
    [PAL_TRK]      = 0xD657u,   /* #d8cbc1 (ink 0.10)       */
    [PAL_SEL_TRK]  = 0x5248u,   /* #564a45 track on ink bar */
    [PAL_PILL_OFF] = 0xCDF6u,   /* #cbbeb5 (ink 0.17)       */
};

/* Light, greige-olive with a burnt-ochre accent. */
static const uint16_t PAL_OLIVE[PAL_COUNT] = {
    [PAL_SURFACE]  = 0xE71Au,   /* #e3e2d3 greige-olive     */
    [PAL_INK]      = 0x2964u,   /* #2b2d22 deep olive       */
    [PAL_MUTED]    = 0x6B8Cu,   /* #6f7260                  */
    [PAL_ACCENT]   = 0xABA6u,   /* #b0762f burnt ochre      */
    [PAL_BORDER]   = 0xD6B8u,   /* #d6d5c4                  */
    [PAL_MUTED2]   = 0x8C90u,   /* #8f9280                  */
    [PAL_MUTED_D]  = 0x4A67u,   /* #4a4d3d                  */
    [PAL_SEL_SUB]  = 0xAD53u,   /* #acac9e light sub on ink bar */
    [PAL_CHEVRON]  = 0xA532u,   /* #a6a693                  */
    [PAL_SB_TRK]   = 0xDED9u,   /* #dad9ca                  */
    [PAL_SB_THMB]  = 0xA511u,   /* #a3a38f                  */
    [PAL_PLATE]    = 0xEF5Bu,   /* #ecebdf raised plate     */
    [PAL_TRK]      = 0xCE77u,   /* #d1d0c1 (ink 0.10)       */
    [PAL_SEL_TRK]  = 0x4A48u,   /* #484a3e track on ink bar */
    [PAL_PILL_OFF] = 0xC616u,   /* #c4c3b5 (ink 0.17)       */
};

/* Dark, espresso brown with a caramel accent. */
static const uint16_t PAL_UMBER[PAL_COUNT] = {
    [PAL_SURFACE]  = 0x2903u,   /* #2a211c espresso         */
    [PAL_INK]      = 0xEF1Au,   /* #ece1d6 warm cream       */
    [PAL_MUTED]    = 0xB512u,   /* #b3a094                  */
    [PAL_ACCENT]   = 0xCC49u,   /* #d08a4a caramel          */
    [PAL_BORDER]   = 0x3985u,   /* #3a2f29                  */
    [PAL_MUTED2]   = 0x8BCEu,   /* #8a7a70                  */
    [PAL_MUTED_D]  = 0xCDF6u,   /* #cdbfb4                  */
    [PAL_SEL_SUB]  = 0x62CAu,   /* #645b54 dark sub on light bar */
    [PAL_CHEVRON]  = 0x5A68u,   /* #5c4d44                  */
    [PAL_SB_TRK]   = 0x3144u,   /* #342a25                  */
    [PAL_SB_THMB]  = 0x7B2Bu,   /* #7a675c                  */
    [PAL_PLATE]    = 0x3165u,   /* #352b26 raised plate     */
    [PAL_TRK]      = 0x39A6u,   /* #3d342f (ink 0.10)       */
    [PAL_SEL_TRK]  = 0xCE16u,   /* #cdc2b8 light track on light bar */
    [PAL_PILL_OFF] = 0x39A6u,   /* = TRK                    */
};

/* Light, warm greige with a muted-rust accent. */
static const uint16_t PAL_MUSHROOM[PAL_COUNT] = {
    [PAL_SURFACE]  = 0xE6FAu,   /* #e6e0d8 warm greige      */
    [PAL_INK]      = 0x3144u,   /* #2e2925 warm near-black  */
    [PAL_MUTED]    = 0x7BADu,   /* #7d746c                  */
    [PAL_ACCENT]   = 0xA2C8u,   /* #a5583e muted rust       */
    [PAL_BORDER]   = 0xD678u,   /* #d8d0c6                  */
    [PAL_MUTED2]   = 0x9C91u,   /* #9b928a                  */
    [PAL_MUTED_D]  = 0x5289u,   /* #56504a                  */
    [PAL_SEL_SUB]  = 0xAD54u,   /* #afa9a2 light sub on ink bar */
    [PAL_CHEVRON]  = 0xB553u,   /* #b5aaa0                  */
    [PAL_SB_TRK]   = 0xDEB9u,   /* #ddd5cc                  */
    [PAL_SB_THMB]  = 0xAD12u,   /* #ada298                  */
    [PAL_PLATE]    = 0xEF5Cu,   /* #efeae4 raised plate     */
    [PAL_TRK]      = 0xD678u,   /* #d4cec6 (ink 0.10)       */
    [PAL_SEL_TRK]  = 0x4A28u,   /* #4b4642 track on ink bar */
    [PAL_PILL_OFF] = 0xC617u,   /* #c7c1ba (ink 0.17)       */
};

/* Indexed by THEME_* — the picker order and the stored id. */
static const uint16_t *const PAL_TABLES[THEME_COUNT] = {
    [THEME_LINEN]    = PAL_LINEN,
    [THEME_ONYX]     = PAL_ONYX,
    [THEME_SAGE]     = PAL_SAGE,
    [THEME_PLASTER]  = PAL_PLASTER,
    [THEME_OLIVE]    = PAL_OLIVE,
    [THEME_UMBER]    = PAL_UMBER,
    [THEME_MUSHROOM] = PAL_MUSHROOM,
};

uint16_t g_pal[PAL_COUNT] = {
    [PAL_SURFACE]  = 0xF79Du, [PAL_INK]      = 0x18A2u, [PAL_MUTED]  = 0x7B8Du,
    [PAL_ACCENT]   = 0xC348u, [PAL_BORDER]   = 0xE71Bu, [PAL_MUTED2] = 0x9C70u,
    [PAL_MUTED_D]  = 0x5A89u, [PAL_SEL_SUB]  = 0xB595u, [PAL_CHEVRON]= 0xB575u,
    [PAL_SB_TRK]   = 0xE73Cu, [PAL_SB_THMB]  = 0xAD34u, [PAL_PLATE]  = 0xF7BEu,
    [PAL_TRK]      = 0xDEDAu, [PAL_SEL_TRK]  = 0x41E7u, [PAL_PILL_OFF]= 0xCE58u,
};

const uint16_t *theme_table(int theme)
{
    if (theme < 0 || theme >= THEME_COUNT) {
        return 0;
    }
    return PAL_TABLES[theme];
}

void theme_set(int theme)
{
    const uint16_t *src = theme_table(theme);
    if (!src) {
        src = PAL_LINEN;               /* unknown id (older/newer record) */
    }
    for (int i = 0; i < PAL_COUNT; i++) {
        g_pal[i] = src[i];
    }
}
