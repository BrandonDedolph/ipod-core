/*
 * core/apps/ui/atlas.c — atlas-based text rendering with alpha blending.
 *
 * Pulls in the auto-generated atlas .h files and provides the
 * runtime API: atlas_text_width and atlas_render. Each generated .h
 * defines its atlas as a const struct in .rodata; we just expose them
 * via the externs in atlas.h.
 */

#include "atlas.h"
#include "../../hal/hal.h"

#include <stdint.h>

/* Auto-generated atlas data. Each .h defines:
 *   - static const uint8_t  <SYM>_DATA[]
 *   - static const atlas_glyph_t <SYM>_GLYPHS[95]
 *   - const atlas_t <SYM>
 * So including them here makes the `<SYM>` consts available to the
 * rest of the firmware via extern in atlas.h. */
#include "atlas/nunito_regular_13.h"
#include "atlas/nunito_bold_17.h"

/* ---------- Measurement -------------------------------------------- */

int atlas_text_width(const atlas_t *a, const char *s) {
    if (!a || !s) return 0;
    int w = 0;
    for (; *s; s++) {
        int cp = (unsigned char)*s;
        if (cp < 0x20 || cp > 0x7E) continue;
        w += a->glyphs[cp - 0x20].advance;
    }
    return w;
}

/* ---------- Rendering ---------------------------------------------- */

/* Decode an RGB565 pixel into 8-bit RGB components. */
static inline void unpack565(lcd_pixel_t p, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = (uint8_t)(((p >> 11) & 0x1F) << 3);
    *g = (uint8_t)(((p >> 5)  & 0x3F) << 2);
    *b = (uint8_t)(((p)       & 0x1F) << 3);
}

void atlas_render(const atlas_t *a, int x, int y_baseline,
                  const char *s, lcd_pixel_t fg) {
    if (!a || !s) return;
    lcd_pixel_t *fb = lcd_framebuffer();

    uint8_t fr, fgreen, fb_; unpack565(fg, &fr, &fgreen, &fb_);

    for (; *s; s++) {
        int cp = (unsigned char)*s;
        if (cp < 0x20 || cp > 0x7E) {
            x += a->glyphs[0].advance;  /* treat as space-width */
            continue;
        }
        const atlas_glyph_t *gly = &a->glyphs[cp - 0x20];
        const uint8_t *src = a->data + gly->data_offset;

        /* offset_y is "px below the ascender line" (PIL's bbox convention).
         * Convert to lcd y: take the baseline, walk up by ascent to get
         * the ascender, then down by offset_y to the top of this glyph. */
        int gx = x + gly->offset_x;
        int gy = y_baseline - a->ascent + gly->offset_y;

        for (int j = 0; j < gly->h; j++) {
            int py = gy + j;
            if (py < 0 || py >= LCD_HEIGHT) continue;
            for (int i = 0; i < gly->w; i++) {
                int px = gx + i;
                if (px < 0 || px >= LCD_WIDTH) continue;
                uint8_t alpha = src[j * gly->w + i];
                if (alpha == 0) continue;

                lcd_pixel_t bg = fb[py * LCD_WIDTH + px];
                if (alpha == 255) {
                    fb[py * LCD_WIDTH + px] = fg;
                    continue;
                }
                uint8_t br, bgreen, bb; unpack565(bg, &br, &bgreen, &bb);
                uint8_t inv = (uint8_t)(255 - alpha);
                uint8_t r  = (uint8_t)(((uint16_t)fr     * alpha + (uint16_t)br     * inv) / 255);
                uint8_t gc = (uint8_t)(((uint16_t)fgreen * alpha + (uint16_t)bgreen * inv) / 255);
                uint8_t b_ = (uint8_t)(((uint16_t)fb_    * alpha + (uint16_t)bb     * inv) / 255);
                fb[py * LCD_WIDTH + px] = lcd_rgb(r, gc, b_);
            }
        }
        x += gly->advance;
    }
}
