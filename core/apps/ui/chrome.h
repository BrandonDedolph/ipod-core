/*
 * core/apps/ui/chrome.h — drawing primitives for Linen-style chrome.
 *
 * Soft-rounded selector rectangle, chevron glyph, and rectangular
 * fill — these operate on the LCD framebuffer directly. All
 * coordinates are in pixels, top-left origin.
 *
 * Anti-aliasing on rounded corners uses per-pixel alpha-from-distance:
 * a corner pixel's coverage = clamp(radius - dist_from_corner_center, 0..1).
 * The per-pixel cost is fine for the sim and well within the iPod's
 * 80 MHz boost budget — selectors don't redraw every frame, only
 * when the selection moves.
 */

#ifndef CORE_APPS_UI_CHROME_H
#define CORE_APPS_UI_CHROME_H

#include "../../hal/hal.h"

/*
 * Solid-fill rectangle, no AA. Clips to the LCD bounds.
 */
void chrome_fill_rect(int x, int y, int w, int h, lcd_pixel_t color);

/*
 * Soft-rounded rectangle, AA on the four corners.
 *
 * radius is the corner radius in pixels; ~3-5 looks good at 320x240.
 * Interior is solid `color`; corner pixels alpha-blend into whatever
 * the framebuffer already holds.
 */
void chrome_rounded_rect(int x, int y, int w, int h, int radius,
                         lcd_pixel_t color);

/*
 * Right-pointing chevron glyph (▶) rendered at (x, y), where (x, y)
 * is the top-left of a `size`×`size` square. `size` ~= 6-8 looks right
 * next to 13px text. Anti-aliased.
 */
void chrome_chevron(int x, int y, int size, lcd_pixel_t color);

/*
 * Diagonal-stripe pattern fill — used as the album-art placeholder
 * when no real image is loaded. Fills the rect (x, y, w, h) with
 * stripes alternating between `color_a` and `color_b`, each
 * `stripe_w` pixels wide, running at 45 degrees.
 *
 * Optionally rounds the rect's corners (radius > 0). Caller-managed
 * border (use chrome_rounded_rect or similar with a thin outline if
 * needed).
 */
void chrome_diagonal_stripes(int x, int y, int w, int h,
                             int stripe_w, int radius,
                             lcd_pixel_t color_a, lcd_pixel_t color_b);

/*
 * Battery glyph: ~14px wide × 7px tall outline + nub on the right,
 * partial fill scaled to `level_pct` (0..100). Drawn at (x, y) =
 * top-left.
 */
void chrome_battery(int x, int y, int level_pct, lcd_pixel_t color);

#endif /* CORE_APPS_UI_CHROME_H */
