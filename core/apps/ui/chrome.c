/*
 * core/apps/ui/chrome.c — chrome drawing primitives.
 */

#include "chrome.h"
#include "atlas.h"
#include "../../hal/hal.h"

#include <stdint.h>

/* Math without <math.h>: integer sqrt for corner-distance. */
static int isqrt32(int v) {
    if (v <= 0) return 0;
    int r = 0;
    int b = 1 << 14;   /* enough for v up to ~2^29 */
    while (b > v) b >>= 2;
    while (b > 0) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

static inline void unpack565(lcd_pixel_t p, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = (uint8_t)(((p >> 11) & 0x1F) << 3);
    *g = (uint8_t)(((p >> 5)  & 0x3F) << 2);
    *b = (uint8_t)(((p)       & 0x1F) << 3);
}

static inline lcd_pixel_t blend(lcd_pixel_t fg, lcd_pixel_t bg, uint8_t alpha) {
    if (alpha == 0)   return bg;
    if (alpha == 255) return fg;
    uint8_t fr, fgg, fbb; unpack565(fg, &fr, &fgg, &fbb);
    uint8_t br, bgg, bbb; unpack565(bg, &br, &bgg, &bbb);
    uint8_t inv = (uint8_t)(255 - alpha);
    uint8_t r  = (uint8_t)(((uint16_t)fr  * alpha + (uint16_t)br  * inv) / 255);
    uint8_t g  = (uint8_t)(((uint16_t)fgg * alpha + (uint16_t)bgg * inv) / 255);
    uint8_t b  = (uint8_t)(((uint16_t)fbb * alpha + (uint16_t)bbb * inv) / 255);
    return lcd_rgb(r, g, b);
}

static inline void put_pixel(int x, int y, lcd_pixel_t color, uint8_t alpha) {
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) return;
    lcd_pixel_t *fb = lcd_framebuffer();
    fb[y * LCD_WIDTH + x] = blend(color, fb[y * LCD_WIDTH + x], alpha);
}

void chrome_fill_rect(int x, int y, int w, int h, lcd_pixel_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH  - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    lcd_pixel_t *fb = lcd_framebuffer();
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            fb[row * LCD_WIDTH + col] = color;
        }
    }
}

/*
 * Per-pixel coverage for the four corners of a rounded rect.
 *
 * For each pixel inside the corner's r×r quadrant box, compute the
 * fixed-point distance to the corner center (which sits r pixels
 * from each edge of the rect). Coverage transitions from 1.0 at
 * dist <= r-0.5 to 0.0 at dist >= r+0.5, linear through r±0.5.
 *
 * Implemented in 8.8 fixed point: dist_q8 = isqrt(dx*dx + dy*dy) <<
 * 8 / approx; we just compare to (r << 8).
 */
static uint8_t corner_alpha(int dx, int dy, int radius) {
    /* dx, dy are integer pixel distances from the *inside* of the
     * corner — small values are "inside the rect," large values are
     * "outside." Square distance: */
    int d2 = dx * dx + dy * dy;
    int r2_outer = (radius + 1) * (radius + 1);
    int r2_inner = (radius) * (radius);

    if (d2 >= r2_outer) return 0;
    if (d2 <= r2_inner) return 255;

    /* On the edge ring: linear coverage. dist = sqrt(d2). */
    int dist  = isqrt32(d2 << 8);   /* 8.8 fixed-point sqrt approx */
    int r_q8  = radius << 8;
    /* coverage = 1 - (dist - (r-0.5)) for dist in [r-0.5, r+0.5].
     * We use [r .. r+1] in integer terms: alpha = 255 * (r+1 - dist/256). */
    int frac  = ((r_q8 + 256) - dist);   /* in 8.8 */
    if (frac <= 0)   return 0;
    if (frac >= 256) return 255;
    return (uint8_t)frac;
}

void chrome_rounded_rect(int x, int y, int w, int h, int radius,
                         lcd_pixel_t color) {
    if (radius < 1) { chrome_fill_rect(x, y, w, h, color); return; }
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 1) { chrome_fill_rect(x, y, w, h, color); return; }

    /* Center band — full-width rectangle minus the corner squares. */
    chrome_fill_rect(x, y + radius, w, h - 2 * radius, color);

    /* Top + bottom strips, minus the corner squares. */
    chrome_fill_rect(x + radius, y,             w - 2 * radius, radius, color);
    chrome_fill_rect(x + radius, y + h - radius, w - 2 * radius, radius, color);

    /* Four corners with AA. For each corner, iterate over a radius x
     * radius square and apply the coverage function. */
    for (int j = 0; j < radius; j++) {
        for (int i = 0; i < radius; i++) {
            /* Distance from the corner center is (radius-1-i, radius-1-j)
             * for the inside of the corner. */
            int dx = radius - i;   /* horizontal distance from rect edge inward */
            int dy = radius - j;
            uint8_t a = corner_alpha(dx, dy, radius);
            put_pixel(x + i,             y + j,             color, a);  /* TL */
            put_pixel(x + w - 1 - i,     y + j,             color, a);  /* TR */
            put_pixel(x + i,             y + h - 1 - j,     color, a);  /* BL */
            put_pixel(x + w - 1 - i,     y + h - 1 - j,     color, a);  /* BR */
        }
    }
}

void chrome_diagonal_stripes(int x, int y, int w, int h,
                             int stripe_w, int radius,
                             lcd_pixel_t color_a, lcd_pixel_t color_b) {
    if (stripe_w < 1) stripe_w = 1;
    int r2 = radius * radius;

    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= LCD_HEIGHT) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= LCD_WIDTH) continue;

            /* Corner cull for rounded rects. */
            if (radius > 0) {
                int dx = 0, dy = 0;
                if (i < radius) dx = radius - i - 1;
                else if (i >= w - radius) dx = i - (w - radius);
                if (j < radius) dy = radius - j - 1;
                else if (j >= h - radius) dy = j - (h - radius);
                if (dx > 0 && dy > 0 && dx * dx + dy * dy >= r2) continue;
            }

            /* Diagonal lines: (i + j) // stripe_w alternates. */
            int band = (i + j) / stripe_w;
            lcd_pixel_t color = (band & 1) ? color_b : color_a;
            lcd_framebuffer()[py * LCD_WIDTH + px] = color;
        }
    }
}

/*
 * Alpha-blend a rectangle of `color` into the framebuffer at `alpha`
 * (0..255). Used by chrome_battery for the soft-fill band.
 * Clipping handled.
 */
static void chrome_alpha_rect(int x, int y, int w, int h,
                              lcd_pixel_t color, uint8_t alpha) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH  - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            put_pixel(col, row, color, alpha);
        }
    }
}

void chrome_battery(int x, int y, int level_pct, lcd_pixel_t color) {
    /*
     * 32x11 body + 2x5 nub at right. Matches the SVG in
     * design_handoff_rockbox_theme/themes.jsx Battery() — viewBox
     * 0..32, body rect 0.5..28.5 / 0.5..10.5 with rx=1.5, nub at
     * x=29 y=3 size 2x5, inner soft fill rect at x=2 y=2 width
     * 25*level height 7 opacity 0.18, "NN%" text centered at x=15.
     */

    if (level_pct < 0)   level_pct = 0;
    if (level_pct > 100) level_pct = 100;

    /* Soft inner fill band, ~18% alpha = 46/255. Drawn first so the
     * outline strokes over it cleanly. Width scales with level: design
     * formula is 25 * level, where level is 0..1. */
    int fill_w = (level_pct * 25) / 100;
    if (fill_w > 0) {
        chrome_alpha_rect(x + 2, y + 2, fill_w, 7, color, 46);
    }

    /*
     * Outline (1 px stroke) with rounded corners matching the design's
     * rx=1.5. We drop the corner squares entirely and fade the two
     * adjacent edge pixels at each corner with ~80% alpha — that gives
     * about 1.5 px of visible rounding, equivalent to the SVG.
     */
    chrome_fill_rect(x + 2, y,      25, 1, color);    /* top */
    chrome_fill_rect(x + 2, y + 10, 25, 1, color);    /* bottom */
    chrome_fill_rect(x,     y + 2,  1,  7, color);    /* left */
    chrome_fill_rect(x + 28,y + 2,  1,  7, color);    /* right */
    /* Corner softening — partial alpha on the pixel adjacent to each
     * inset edge. The actual corner pixel stays empty (no drawing). */
    put_pixel(x + 1,  y,       color, 200);
    put_pixel(x,      y + 1,   color, 200);
    put_pixel(x + 27, y,       color, 200);
    put_pixel(x + 28, y + 1,   color, 200);
    put_pixel(x + 1,  y + 10,  color, 200);
    put_pixel(x,      y + 9,   color, 200);
    put_pixel(x + 27, y + 10,  color, 200);
    put_pixel(x + 28, y + 9,   color, 200);

    /* Nub: 2x5 solid rect. */
    chrome_fill_rect(x + 29, y + 3, 2, 5, color);

    /* Percent text inside the body, centered horizontally at x=15
     * within the 28-wide body. Bold-9 is the closest atlas to the
     * design's 7px tabular bold. */
    char buf[8];
    int n = level_pct;
    if (n >= 100) { buf[0]='1'; buf[1]='0'; buf[2]='0'; buf[3]='%'; buf[4]=0; }
    else if (n >= 10) { buf[0]='0'+(n/10); buf[1]='0'+(n%10); buf[2]='%'; buf[3]=0; }
    else { buf[0]='0'+n; buf[1]='%'; buf[2]=0; }

    int tw = atlas_text_width(&NUNITO_BOLD_9, buf);
    int tx = x + (28 - tw) / 2 + 1;   /* +1 to center on the visual midline */
    int baseline = y + 9;
    atlas_render(&NUNITO_BOLD_9, tx, baseline, buf, color);
}

void chrome_line(int x0, int y0, int x1, int y1, lcd_pixel_t color) {
    /* Standard Bresenham. Clipping is per-pixel via put_pixel. */
    int dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        put_pixel(x0, y0, color, 255);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void chrome_blit_alpha(int x, int y, int w, int h,
                       const uint8_t *alpha, lcd_pixel_t color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            uint8_t a = alpha[j * w + i];
            if (a == 0) continue;
            put_pixel(x + i, y + j, color, a);
        }
    }
}

void chrome_outline_rect(int x, int y, int w, int h, int radius,
                         lcd_pixel_t color) {
    if (w <= 0 || h <= 0) return;
    if (radius < 1) {
        chrome_fill_rect(x,         y,         w, 1, color);
        chrome_fill_rect(x,         y + h - 1, w, 1, color);
        chrome_fill_rect(x,         y,         1, h, color);
        chrome_fill_rect(x + w - 1, y,         1, h, color);
        return;
    }
    /* Top + bottom (skipping corner squares) */
    chrome_fill_rect(x + radius, y,         w - 2 * radius, 1, color);
    chrome_fill_rect(x + radius, y + h - 1, w - 2 * radius, 1, color);
    /* Left + right */
    chrome_fill_rect(x,         y + radius, 1, h - 2 * radius, color);
    chrome_fill_rect(x + w - 1, y + radius, 1, h - 2 * radius, color);
    /* Corner softening — partial-alpha inset pixels. */
    for (int j = 0; j < radius; j++) {
        for (int i = 0; i < radius; i++) {
            int dx = radius - i;
            int dy = radius - j;
            int d2 = dx * dx + dy * dy;
            int r2 = radius * radius;
            /* On the outline ring (within ±0.5 of radius) — coverage ~70%.
             * Skip pixels well outside (corner cull) and well inside. */
            if (d2 >= r2 + radius || d2 < r2 - radius) continue;
            put_pixel(x + i,             y + j,             color, 180);
            put_pixel(x + w - 1 - i,     y + j,             color, 180);
            put_pixel(x + i,             y + h - 1 - j,     color, 180);
            put_pixel(x + w - 1 - i,     y + h - 1 - j,     color, 180);
        }
    }
}

/*
 * Shuffle / Repeat icons — pre-rasterized 12×10 alpha masks generated
 * from the design SVGs (themes.jsx ShuffleIcon / RepeatIcon) by
 * tools/icon_gen.sh: render at 8× via librsvg, downsample with a box
 * filter, dump the alpha channel.
 *
 * Hand-drawn line art at this size doesn't reproduce 1.4 px strokes
 * cleanly — diagonals stair-step and arrowhead Vs read as scattered
 * pixels. Pre-rasterizing means the icons get proper anti-aliased
 * coverage, the same way the type atlases do. Edit the SVG source +
 * regenerate, don't hand-tweak the byte arrays.
 */
static const uint8_t SHUFFLE_ALPHA[12 * 10] = {
      0,   0,   0,   0,   0,   0,   0,  23,  51,   0,   0,   0,
    133, 208, 208, 191,  13,   0,   0,  85, 249,  95,   0,   0,
     65, 117, 117, 220, 135,   0, 123, 185, 241, 254, 103,   0,
      0,   0,   0,  76, 248,  85, 252, 150, 241, 246,  74,   0,
      0,   0,   0,   0, 183, 233, 128,  98, 234,  57,   0,   0,
      0,   0,   0,  19, 198, 252,  69,  49, 111,   0,   0,   0,
      0,   0,   0, 147, 207, 144, 212,  70, 248, 141,   2,   0,
    149, 232, 232, 252,  66,  16, 234, 236, 247, 255, 129,   0,
     49,  94,  94,  76,   0,   0,  49, 109, 242, 228,  47,   0,
      0,   0,   0,   0,   0,   0,   0,  92, 208,  29,   0,   0,
};

static const uint8_t REPEAT_ALPHA[12 * 10] = {
      0,   0,   0,   0,   0,   0,   0, 202, 208,  44,   0,   0,
      0,   2, 191, 208, 208, 208, 208, 223, 255, 247,  57,   0,
      0,   0,  99, 117, 117, 117, 117, 195, 255, 255,  28,   0,
      0,   0,   0,   0,   0,   0,   0, 181, 158, 255,   7,   0,
      0,   0,  26,   0,   0,   0,   0,   0,  37, 251,   5,   0,
      0,   5, 251,  37,   0,   0,   0,   0,   0,  26,   0,   0,
      0,   7, 255, 158, 181,   0,   0,   0,   0,   0,   0,   0,
      0,  28, 255, 255, 195, 117, 117, 117, 117,  99,   0,   0,
      0,  57, 246, 255, 223, 208, 208, 208, 208, 191,   2,   0,
      0,   0,  44, 208, 202,   0,   0,   0,   0,   0,   0,   0,
};

void chrome_shuffle(int x, int y, lcd_pixel_t color) {
    chrome_blit_alpha(x, y, 12, 10, SHUFFLE_ALPHA, color);
}

void chrome_repeat(int x, int y, lcd_pixel_t color) {
    chrome_blit_alpha(x, y, 12, 10, REPEAT_ALPHA, color);
}

void chrome_chevron(int x, int y, int size, lcd_pixel_t color) {
    /*
     * Thin right-pointing angle bracket (›) — two 1-px diagonals
     * meeting at a point on the right. Matches the Unicode ›
     * (U+203A) used in the design, NOT a filled triangle.
     *
     * `size` is the height. Width is computed as (size/2 + 1) so the
     * shape stays narrower than tall — matching how Nunito renders ›
     * at small font sizes.
     *
     * For size=7, w=4:
     *     X...     row 0: col 0
     *     .X..     row 1: col 1
     *     ..X.     row 2: col 2
     *     ...X     row 3: col 3 (apex, at y_mid)
     *     ..X.     row 4: col 2
     *     .X..     row 5: col 1
     *     X...     row 6: col 0
     */
    if (size < 4) size = 4;
    int h   = size;
    int w   = h / 2 + 1;
    int mid = h / 2;

    for (int r = 0; r < h; r++) {
        int dist_from_mid = (r >= mid) ? (r - mid) : (mid - r);
        int col = (w - 1) - dist_from_mid;
        if (col < 0) continue;
        put_pixel(x + col, y + r, color, 255);
        /* Light AA — partial-alpha pixel one column toward the apex
         * to soften the staircase between rows. */
        if (col + 1 < w && r != mid) {
            put_pixel(x + col + 1, y + r, color, 90);
        }
    }
}
