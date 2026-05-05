/*
 * core/apps/ui/theme.c — Light/Dark palette implementation.
 *
 * Pre-composited tints (separator, chev_*, etc) are derived from the
 * fg/bg pair at theme_set() time so callers can grab them in O(1).
 * Light is the canonical Linen palette; Dark inverts fg/bg and
 * synthesizes matching tints.
 */

#include "theme.h"

static theme_mode_t g_mode = THEME_LIGHT;

/* Palette holder filled by theme_set(). */
typedef struct {
    lcd_pixel_t fg, bg;
    lcd_pixel_t fg_deep, fg_muted;
    lcd_pixel_t separator;
    lcd_pixel_t chev_unsel, chev_sel;
    lcd_pixel_t track_faint;
    lcd_pixel_t star_muted;
    lcd_pixel_t stripe_a, stripe_b;
    lcd_pixel_t accent;
} palette_t;

static palette_t g_pal;

static void apply_light(void) {
    g_pal.fg            = lcd_rgb(0x1A, 0x17, 0x14);
    g_pal.bg            = lcd_rgb(0xF4, 0xF1, 0xEC);
    g_pal.fg_deep       = lcd_rgb(0x5A, 0x50, 0x48);
    g_pal.fg_muted      = lcd_rgb(0x9A, 0x8E, 0x80);
    g_pal.separator     = lcd_rgb(0xE2, 0xDE, 0xDA);   /* 8% ink on cream */
    g_pal.chev_unsel    = lcd_rgb(0xB0, 0xA8, 0x9E);   /* 0.4 ink on cream */
    g_pal.chev_sel      = lcd_rgb(0xA0, 0x9C, 0x97);   /* 0.7 cream on ink */
    g_pal.track_faint   = lcd_rgb(0xD8, 0xD2, 0xC8);
    g_pal.star_muted    = lcd_rgb(0xCC, 0xC4, 0xB7);
    g_pal.stripe_a      = lcd_rgb(0xCD, 0xB8, 0xA6);
    g_pal.stripe_b      = lcd_rgb(0xC0, 0xAB, 0x99);
    g_pal.accent        = lcd_rgb(0xC4, 0x5A, 0x3A);
}

static void apply_dark(void) {
    /* Dark base: nearly-black warm bg, warm-cream text. The big-art
     * page already uses these for its overlay (matches the design
     * handoff's dark variants). Reusing them keeps the palette
     * coherent. */
    g_pal.fg            = lcd_rgb(0xE8, 0xE4, 0xDD);
    g_pal.bg            = lcd_rgb(0x0E, 0x0D, 0x0C);
    g_pal.fg_deep       = lcd_rgb(0xC0, 0xB6, 0xA8);   /* lighter than fg_muted */
    g_pal.fg_muted      = lcd_rgb(0xA8, 0x9E, 0x92);
    g_pal.separator     = lcd_rgb(0x22, 0x20, 0x1D);   /* 8% cream on near-black */
    g_pal.chev_unsel    = lcd_rgb(0x65, 0x60, 0x55);   /* 0.4 cream on near-black bg */
    g_pal.chev_sel      = lcd_rgb(0x4F, 0x4B, 0x44);   /* 0.7 near-black on cream fg */
    g_pal.track_faint   = lcd_rgb(0x2A, 0x26, 0x21);
    g_pal.star_muted    = lcd_rgb(0x3C, 0x36, 0x30);
    /* Stripes stay earthy but a touch dimmer so they read on near-black. */
    g_pal.stripe_a      = lcd_rgb(0x6E, 0x5C, 0x4C);
    g_pal.stripe_b      = lcd_rgb(0x82, 0x6E, 0x5A);
    g_pal.accent        = lcd_rgb(0xC4, 0x5A, 0x3A);
}

void theme_set(theme_mode_t mode) {
    g_mode = mode;
    if (mode == THEME_DARK) apply_dark();
    else                    apply_light();
}

theme_mode_t theme_get(void) { return g_mode; }

const char *theme_label(theme_mode_t mode) {
    return (mode == THEME_DARK) ? "Dark" : "Light";
}

/* Lazy init: first read populates the palette if theme_set() wasn't
 * called explicitly. Keeps callers from having to thread an init step. */
static void ensure_init(void) {
    if (g_pal.bg == 0 && g_pal.fg == 0) apply_light();
}

lcd_pixel_t theme_fg(void)            { ensure_init(); return g_pal.fg; }
lcd_pixel_t theme_bg(void)            { ensure_init(); return g_pal.bg; }
lcd_pixel_t theme_fg_deep(void)       { ensure_init(); return g_pal.fg_deep; }
lcd_pixel_t theme_fg_muted(void)      { ensure_init(); return g_pal.fg_muted; }
lcd_pixel_t theme_separator(void)     { ensure_init(); return g_pal.separator; }
lcd_pixel_t theme_chev_unsel(void)    { ensure_init(); return g_pal.chev_unsel; }
lcd_pixel_t theme_chev_sel(void)      { ensure_init(); return g_pal.chev_sel; }
lcd_pixel_t theme_track_faint(void)   { ensure_init(); return g_pal.track_faint; }
lcd_pixel_t theme_star_muted(void)    { ensure_init(); return g_pal.star_muted; }
lcd_pixel_t theme_stripe_a(void)      { ensure_init(); return g_pal.stripe_a; }
lcd_pixel_t theme_stripe_b(void)      { ensure_init(); return g_pal.stripe_b; }
lcd_pixel_t theme_accent(void)        { ensure_init(); return g_pal.accent; }
