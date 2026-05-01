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
     * Outline (1 px stroke) with soft corners. The design's rx=1.5
     * rounding at this resolution is essentially "skip the four
     * absolute-corner pixels" — that's enough to read as rounded
     * without doing real AA curves at 11 px tall.
     */
    chrome_fill_rect(x + 1, y,      27, 1,  color);    /* top */
    chrome_fill_rect(x + 1, y + 10, 27, 1,  color);    /* bottom */
    chrome_fill_rect(x,     y + 1,  1,  9,  color);    /* left */
    chrome_fill_rect(x + 28,y + 1,  1,  9,  color);    /* right */
    /* Half-pixel softening at each corner via low-alpha pixel. */
    put_pixel(x,      y,      color, 128);
    put_pixel(x + 28, y,      color, 128);
    put_pixel(x,      y + 10, color, 128);
    put_pixel(x + 28, y + 10, color, 128);

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

void chrome_chevron(int x, int y, int size, lcd_pixel_t color) {
    /*
     * Right-pointing triangle from (x, y) to (x+size, y+size).
     * Apex on the right edge at vertical center; base on the left.
     * Per-row width:  row r in [0..size-1] -> width = size - 2*|r - size/2|
     * Anti-aliased on the slanting edges via fractional coverage.
     */
    int half = size / 2;
    for (int r = 0; r < size; r++) {
        int dist_from_mid = (r >= half) ? (r - half) : (half - 1 - r);
        int row_w = size - 2 * dist_from_mid;
        if (row_w <= 0) continue;
        for (int c = 0; c < row_w; c++) {
            put_pixel(x + c, y + r, color, 255);
        }
        /* AA: one pixel beyond the edge gets ~50% coverage to soften. */
        if (row_w < size) {
            put_pixel(x + row_w, y + r, color, 128);
        }
    }
}
