/*
 * core/apps/ui/theme.h — Light/Dark palette switch.
 *
 * Two modes share the same chrome geometry, terracotta accent, and
 * embedded album art. Everything else (text, page bg, dividers,
 * placeholder stripes) reads through the accessors below so a single
 * theme_set() flips every screen.
 *
 * Album art bytes are theme-agnostic (the JPEG decoder produces fixed
 * pixel values); only the no-art *placeholder stripes* shift per-mode.
 *
 * Single-process, single-thread — there is no lock.
 */

#ifndef CORE_APPS_UI_THEME_H
#define CORE_APPS_UI_THEME_H

#include "../../hal/hal.h"

typedef enum {
    THEME_LIGHT = 0,
    THEME_DARK  = 1,
} theme_mode_t;

void          theme_set(theme_mode_t mode);
theme_mode_t  theme_get(void);
const char   *theme_label(theme_mode_t mode);   /* "Light" / "Dark" */

/* Primary text + page bg. Flip in dark mode. */
lcd_pixel_t theme_fg(void);
lcd_pixel_t theme_bg(void);

/* NP secondary / tertiary text tiers. Flip in dark mode. */
lcd_pixel_t theme_fg_deep(void);
lcd_pixel_t theme_fg_muted(void);

/* Pre-composited helpers — derived once per mode at theme_set() time. */
lcd_pixel_t theme_separator(void);   /* 8% fg on bg — status-bar divider */
lcd_pixel_t theme_chev_unsel(void);  /* 0.4 fg on bg */
lcd_pixel_t theme_chev_sel(void);    /* 0.7 bg on fg */
lcd_pixel_t theme_track_faint(void); /* progress-bar bg, peak-meter dim */
lcd_pixel_t theme_star_muted(void);  /* unfilled rating stars */
lcd_pixel_t theme_stripe_a(void);    /* placeholder stripe colors — */
lcd_pixel_t theme_stripe_b(void);    /* art-area no-art fallback */

/* Accent (terracotta) is identical in both modes. */
lcd_pixel_t theme_accent(void);

#endif /* CORE_APPS_UI_THEME_H */
