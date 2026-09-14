/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/palette.h — the runtime UI palette (theme-selected RGB565 tokens).
 *
 * The Linen UI was originally a set of compile-time #define colors duplicated in
 * kernel/main.c (LINEN_*) and ui/screen_settings.c (S_*). To support a live theme
 * switch (Settings -> Theme) those constants become indices into a single
 * mutable palette, g_pal[], swapped as a block by theme_set(). Both files keep
 * their familiar token *names* — they're just #defined to g_pal[PAL_*] now — so
 * the switch is a data change, not a rename across ~100 call sites.
 *
 * Seven themes ship: Linen (warm light, the original), Onyx (warm dark), Sage
 * (dark green-grey), Plaster (light pink-beige), Olive (light greige-olive),
 * Umber (dark espresso) and Mushroom (light warm greige). Their ids are the
 * order of the Theme picker AND the value the settings record stores, so the
 * enum below is append-only.
 *
 * Freestanding: a plain uint16_t array + static source palettes. No libc.
 */
#ifndef CORE_UI_PALETTE_H
#define CORE_UI_PALETTE_H

#include <stdint.h>

/* Every independent color token used by the UI. SEL_BG / SEL_FG are NOT here:
 * they are deliberately derived from INK / SURFACE (a selected row is the ink
 * color barred behind surface-colored text), and that inversion is what makes
 * one set of tokens read correctly in both a light and a dark theme. */
enum {
    PAL_SURFACE,     /* page background                                   */
    PAL_INK,         /* primary text / selection bar                      */
    PAL_MUTED,       /* secondary text                                    */
    PAL_ACCENT,      /* terracotta accent (progress / low battery)        */
    PAL_BORDER,      /* divider lines                                     */
    PAL_MUTED2,      /* lighter muted (subs / status strip)               */
    PAL_MUTED_D,     /* deep muted (right-hand values)                    */
    PAL_SEL_SUB,     /* sub / right text ON a selected row                */
    PAL_CHEVRON,     /* › chevron on surface                              */
    PAL_SB_TRK,      /* scrollbar track                                   */
    PAL_SB_THMB,     /* scrollbar thumb                                   */
    PAL_PLATE,       /* raised plate (volume overlay)                     */
    PAL_TRK,         /* slider / meter track                              */
    PAL_SEL_TRK,     /* slider track on a selected row                    */
    PAL_PILL_OFF,    /* toggle pill OFF fill                              */
    PAL_COUNT
};

/* The live palette. Initialised to Linen so it is valid before the first
 * theme_set() (boot splash paints through it). */
extern uint16_t g_pal[PAL_COUNT];

/* Theme ids — the Theme picker order and the settings-record value. Keep in
 * sync with settings_theme_name() / THEME_L in ui/settings.c and the preview
 * swatches in ui/screen_settings.c. Append only: a stored id must keep
 * meaning the same theme across builds. */
enum {
    THEME_LINEN    = 0,      /* warm light (original)        */
    THEME_ONYX     = 1,      /* warm dark                    */
    THEME_SAGE     = 2,      /* dark, muted green-grey       */
    THEME_PLASTER  = 3,      /* light, pink-beige limewash   */
    THEME_OLIVE    = 4,      /* light, greige-olive          */
    THEME_UMBER    = 5,      /* dark, espresso brown         */
    THEME_MUSHROOM = 6,      /* light, warm greige           */
    THEME_COUNT
};

/* The source table for `theme`, or NULL when the id is not one of the above.
 * Exposed so the palette test can prove theme_set() copies the right table;
 * the UI never reads tables directly — it reads g_pal[]. */
const uint16_t *theme_table(int theme);

/* Swap the whole palette to `theme` (a THEME_* id; anything else falls back
 * to Linen). Cheap block copy — call it on boot and whenever the Theme
 * setting changes. */
void theme_set(int theme);

#endif /* CORE_UI_PALETTE_H */
