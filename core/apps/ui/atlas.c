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
#include "atlas/nunito_regular_9.h"
#include "atlas/nunito_regular_11.h"
#include "atlas/nunito_regular_13.h"
#include "atlas/nunito_bold_9.h"
#include "atlas/nunito_bold_11.h"
#include "atlas/nunito_bold_13.h"
#include "atlas/nunito_bold_17.h"

/* ---------- Gamma LUTs --------------------------------------------- *
 * Glyph alpha is *coverage* (linear-light), but RGB565 channels are
 * sRGB-encoded. Naively mixing in sRGB space yields anemic, grey-fringed
 * text. Convert each channel to linear, blend by coverage there, then
 * re-encode to sRGB on store. We use a gamma=2.2 approximation —
 * cheap, accurate enough at 5/6-bit precision, and pre-baked to avoid
 * pulling <math.h> into a hot path that also runs on freestanding ARM.
 *
 * Tables generated with:
 *   srgb5_to_linear[i] = round(((i/31) ** 2.2) * 255)
 *   srgb6_to_linear[i] = round(((i/63) ** 2.2) * 255)
 *   linear_to_srgb5[i] = round(((i/255) ** (1/2.2)) * 31)
 *   linear_to_srgb6[i] = round(((i/255) ** (1/2.2)) * 63)
 */
static const uint8_t srgb5_to_linear[32] = {
      0,   0,   1,   1,   3,   5,   7,  10,  13,  17,  21,  26,  32,  38,  44,  52,
     60,  68,  77,  87,  97, 108, 120, 132, 145, 159, 173, 188, 204, 220, 237, 255,
};

static const uint8_t srgb6_to_linear[64] = {
      0,   0,   0,   0,   1,   1,   1,   2,   3,   4,   4,   5,   7,   8,   9,  11,
     13,  14,  16,  18,  20,  23,  25,  28,  31,  33,  36,  40,  43,  46,  50,  54,
     57,  61,  66,  70,  74,  79,  84,  89,  94,  99, 105, 110, 116, 122, 128, 134,
    140, 147, 153, 160, 167, 174, 182, 189, 197, 205, 213, 221, 229, 238, 246, 255,
};

static const uint8_t linear_to_srgb5[256] = {
      0,   2,   3,   4,   5,   5,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,
      9,   9,   9,  10,  10,  10,  10,  10,  11,  11,  11,  11,  11,  12,  12,  12,
     12,  12,  12,  13,  13,  13,  13,  13,  13,  14,  14,  14,  14,  14,  14,  14,
     15,  15,  15,  15,  15,  15,  15,  15,  16,  16,  16,  16,  16,  16,  16,  16,
     17,  17,  17,  17,  17,  17,  17,  17,  17,  18,  18,  18,  18,  18,  18,  18,
     18,  18,  19,  19,  19,  19,  19,  19,  19,  19,  19,  19,  20,  20,  20,  20,
     20,  20,  20,  20,  20,  20,  20,  21,  21,  21,  21,  21,  21,  21,  21,  21,
     21,  21,  21,  22,  22,  22,  22,  22,  22,  22,  22,  22,  22,  22,  23,  23,
     23,  23,  23,  23,  23,  23,  23,  23,  23,  23,  23,  24,  24,  24,  24,  24,
     24,  24,  24,  24,  24,  24,  24,  24,  25,  25,  25,  25,  25,  25,  25,  25,
     25,  25,  25,  25,  25,  25,  26,  26,  26,  26,  26,  26,  26,  26,  26,  26,
     26,  26,  26,  26,  26,  27,  27,  27,  27,  27,  27,  27,  27,  27,  27,  27,
     27,  27,  27,  27,  28,  28,  28,  28,  28,  28,  28,  28,  28,  28,  28,  28,
     28,  28,  28,  28,  29,  29,  29,  29,  29,  29,  29,  29,  29,  29,  29,  29,
     29,  29,  29,  29,  29,  30,  30,  30,  30,  30,  30,  30,  30,  30,  30,  30,
     30,  30,  30,  30,  30,  30,  30,  31,  31,  31,  31,  31,  31,  31,  31,  31,
};

static const uint8_t linear_to_srgb6[256] = {
      0,   5,   7,   8,  10,  11,  11,  12,  13,  14,  14,  15,  16,  16,  17,  17,
     18,  18,  19,  19,  20,  20,  21,  21,  22,  22,  22,  23,  23,  23,  24,  24,
     25,  25,  25,  26,  26,  26,  27,  27,  27,  27,  28,  28,  28,  29,  29,  29,
     29,  30,  30,  30,  31,  31,  31,  31,  32,  32,  32,  32,  33,  33,  33,  33,
     34,  34,  34,  34,  35,  35,  35,  35,  35,  36,  36,  36,  36,  37,  37,  37,
     37,  37,  38,  38,  38,  38,  38,  39,  39,  39,  39,  39,  40,  40,  40,  40,
     40,  41,  41,  41,  41,  41,  42,  42,  42,  42,  42,  42,  43,  43,  43,  43,
     43,  44,  44,  44,  44,  44,  44,  45,  45,  45,  45,  45,  45,  46,  46,  46,
     46,  46,  46,  47,  47,  47,  47,  47,  47,  48,  48,  48,  48,  48,  48,  48,
     49,  49,  49,  49,  49,  49,  49,  50,  50,  50,  50,  50,  50,  51,  51,  51,
     51,  51,  51,  51,  52,  52,  52,  52,  52,  52,  52,  53,  53,  53,  53,  53,
     53,  53,  54,  54,  54,  54,  54,  54,  54,  54,  55,  55,  55,  55,  55,  55,
     55,  56,  56,  56,  56,  56,  56,  56,  56,  57,  57,  57,  57,  57,  57,  57,
     57,  58,  58,  58,  58,  58,  58,  58,  58,  59,  59,  59,  59,  59,  59,  59,
     59,  60,  60,  60,  60,  60,  60,  60,  60,  60,  61,  61,  61,  61,  61,  61,
     61,  61,  62,  62,  62,  62,  62,  62,  62,  62,  62,  63,  63,  63,  63,  63,
};

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

void atlas_render(const atlas_t *a, int x, int y_baseline,
                  const char *s, lcd_pixel_t fg) {
    if (!a || !s) return;
    lcd_pixel_t *fb = lcd_framebuffer();

    /* Foreground in raw RGB565 bit-fields and pre-LUT'd to linear. */
    uint8_t fr5 = (uint8_t)((fg >> 11) & 0x1F);
    uint8_t fg6 = (uint8_t)((fg >> 5)  & 0x3F);
    uint8_t fb5 = (uint8_t)( fg        & 0x1F);
    uint8_t fr_lin = srgb5_to_linear[fr5];
    uint8_t fg_lin = srgb6_to_linear[fg6];
    uint8_t fb_lin = srgb5_to_linear[fb5];

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

                if (alpha == 255) {
                    fb[py * LCD_WIDTH + px] = fg;
                    continue;
                }

                lcd_pixel_t bg = fb[py * LCD_WIDTH + px];
                uint8_t br_lin = srgb5_to_linear[(bg >> 11) & 0x1F];
                uint8_t bg_lin = srgb6_to_linear[(bg >> 5)  & 0x3F];
                uint8_t bb_lin = srgb5_to_linear[ bg        & 0x1F];

                uint8_t inv = (uint8_t)(255 - alpha);
                uint8_t r_lin = (uint8_t)(((uint16_t)fr_lin * alpha + (uint16_t)br_lin * inv) / 255);
                uint8_t g_lin = (uint8_t)(((uint16_t)fg_lin * alpha + (uint16_t)bg_lin * inv) / 255);
                uint8_t b_lin = (uint8_t)(((uint16_t)fb_lin * alpha + (uint16_t)bb_lin * inv) / 255);

                uint8_t r5 = linear_to_srgb5[r_lin];
                uint8_t g6 = linear_to_srgb6[g_lin];
                uint8_t b5 = linear_to_srgb5[b_lin];
                fb[py * LCD_WIDTH + px] =
                    (lcd_pixel_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | (uint16_t)b5);
            }
        }
        x += gly->advance;
    }
}
