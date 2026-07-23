/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/text.c — freestanding antialiased text rendering.
 *
 * This is the freestanding sibling of core/apps/ui/atlas.c: the same
 * gamma-correct alpha blend and glyph layout, but libc-free and drawing
 * into a caller-supplied RGB565 framebuffer with explicit bounds (so it
 * needs neither lcd_framebuffer() nor the compile-time LCD_WIDTH/HEIGHT).
 * It links into core.elf on the device and into the host text_test.
 *
 * The atlas data (glyph metrics + concatenated alpha-coverage rows) lives
 * in the generated core/apps/ui/atlas/ headers; including them here
 * makes the const atlas_t handles resolve at link time — no init step, no
 * TTF parsing, no allocation. Everything is .rodata.
 */

#include "text.h"

/* atlas_glyph_t / atlas_t and the generated atlas handles. The generated
 * headers each pull in ../atlas.h (which only needs stdint + hal.h — both
 * freestanding-safe), then define their static coverage data + a
 * const atlas_t. This TU is the sole definition site for those atlas_t
 * symbols in both the hw link (atlas.c is sim-only) and the host text_test
 * link (which links text.c but not atlas.c), so there is no clash. */
#include "atlas.h"
#include "atlas/glyphmap.h"        /* ATLAS_CPMAP[]: codepoint -> glyph index */
#include "atlas/nunito_regular_9.h"
#include "atlas/nunito_regular_11.h"
#include "atlas/nunito_regular_13.h"
#include "atlas/nunito_bold_9.h"
#include "atlas/nunito_bold_11.h"
#include "atlas/nunito_bold_13.h"
#include "atlas/nunito_bold_17.h"

/* text_font_t is just a thin, opaque wrapper over an atlas_t so the public
 * header need not expose the atlas layout. */
struct text_font {
    const atlas_t *a;
};

static const struct text_font FONT_REGULAR_9  = { &NUNITO_REGULAR_9  };
static const struct text_font FONT_REGULAR_11 = { &NUNITO_REGULAR_11 };
static const struct text_font FONT_REGULAR_13 = { &NUNITO_REGULAR_13 };
static const struct text_font FONT_BOLD_9     = { &NUNITO_BOLD_9     };
static const struct text_font FONT_BOLD_11    = { &NUNITO_BOLD_11    };
static const struct text_font FONT_BOLD_13    = { &NUNITO_BOLD_13    };
static const struct text_font FONT_BOLD_17    = { &NUNITO_BOLD_17    };

const text_font_t *text_font_regular_9(void)  { return &FONT_REGULAR_9;  }
const text_font_t *text_font_regular_11(void) { return &FONT_REGULAR_11; }
const text_font_t *text_font_regular_13(void) { return &FONT_REGULAR_13; }
const text_font_t *text_font_bold_9(void)     { return &FONT_BOLD_9;     }
const text_font_t *text_font_bold_11(void)    { return &FONT_BOLD_11;    }
const text_font_t *text_font_bold_13(void)    { return &FONT_BOLD_13;    }
const text_font_t *text_font_bold_17(void)    { return &FONT_BOLD_17;    }

/* ---------- Gamma LUTs ---------------------------------------------- *
 * Glyph alpha is linear-light coverage, but RGB565 channels are sRGB-
 * encoded. Blending in sRGB space yields anemic, grey-fringed text. So:
 * decode each channel to linear, mix by coverage there, re-encode to
 * sRGB on store. gamma=2.2 approximation, pre-baked so the hot path
 * never touches <math.h> (matters on the FPU-less ARM7). Tables are the
 * same ones atlas.c uses:
 *   srgb5_to_linear[i]  = round(((i/31) ** 2.2) * 255)
 *   srgb6_to_linear[i]  = round(((i/63) ** 2.2) * 255)
 *   linear_to_srgb5[i]  = round(((i/255) ** (1/2.2)) * 31)
 *   linear_to_srgb6[i]  = round(((i/255) ** (1/2.2)) * 63)
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

/* Decode one UTF-8 sequence at *p, advance *p past it, and return the codepoint
 * (or -1 at the terminating NUL). Malformed bytes yield U+FFFD and advance one
 * byte so a bad string can't stall. UI strings are UTF-8; ASCII is the 1-byte
 * fast path (the common case). */
static int utf8_next(const unsigned char **p) {
    unsigned char c = **p;
    if (c == 0) return -1;
    if (c < 0x80) { (*p)++; return c; }
    int n, cp;
    if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { (*p)++; return 0xFFFD; }               /* stray continuation/lead */
    const unsigned char *q = *p + 1;
    for (int i = 0; i < n; i++) {
        if ((q[i] & 0xC0) != 0x80) { (*p)++; return 0xFFFD; }   /* truncated */
        cp = (cp << 6) | (q[i] & 0x3F);
    }
    *p += n + 1;
    return cp;
}

/* Map a Unicode codepoint to an atlas glyph index, or -1 for "no glyph"
 * (rendered as space-width). ASCII 0x20..0x7E map linearly; the three legacy
 * private control bytes 0x01/0x02/0x03 alias ‹ › · (UI_GLYPH_* predate UTF-8);
 * everything else is looked up in the generated, codepoint-sorted ATLAS_CPMAP
 * (tools/atlas_gen.py EXTRAS -> atlas/glyphmap.h) by binary search. */
static int glyph_index(int cp) {
    if (cp >= 0x20 && cp <= 0x7E) return cp - 0x20;
    switch (cp) {                                 /* legacy private aliases */
        case 0x01: cp = 0x2039; break;            /* ‹ */
        case 0x02: cp = 0x203A; break;            /* › */
        case 0x03: cp = 0x00B7; break;            /* · */
        default: break;
    }
    int lo = 0, hi = ATLAS_CPMAP_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1, mc = ATLAS_CPMAP[mid].cp;
        if      (cp < mc) hi = mid - 1;
        else if (cp > mc) lo = mid + 1;
        else return ATLAS_CPMAP[mid].idx;
    }
    return -1;
}

/* ---------- Measurement / metrics ---------------------------------- */

int text_width(const char *s, const text_font_t *font) {
    if (!font || !s) return 0;
    const atlas_t *a = font->a;
    const unsigned char *p = (const unsigned char *)s;
    int w = 0, cp;
    while ((cp = utf8_next(&p)) >= 0) {
        int gi = glyph_index(cp);
        if (gi < 0) {
            w += a->glyphs[0].advance;   /* space width */
            continue;
        }
        w += a->glyphs[gi].advance;
    }
    return w;
}

int text_line_height(const text_font_t *font) {
    if (!font) return 0;
    return font->a->line_height;
}

int text_descent(const text_font_t *font) {
    if (!font) return 0;
    return font->a->descent;
}

int text_ascent(const text_font_t *font) {
    if (!font) return 0;
    return font->a->ascent;
}

/* ---------- Rendering ---------------------------------------------- */

static int text_draw_c(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                       const char *s, const text_font_t *font, uint16_t ink,
                       int cx0, int cx1, int cy0, int cy1) {
    if (!fb || !font || !s || fb_w <= 0 || fb_h <= 0) {
        return x;
    }
    if (cy0 < 0) cy0 = 0;
    if (cy1 > fb_h) cy1 = fb_h;
    const atlas_t *a = font->a;

    /* Ink in raw RGB565 bit-fields, pre-decoded to linear light. */
    uint8_t ir5 = (uint8_t)((ink >> 11) & 0x1F);
    uint8_t ig6 = (uint8_t)((ink >> 5)  & 0x3F);
    uint8_t ib5 = (uint8_t)( ink        & 0x1F);
    uint8_t ir_lin = srgb5_to_linear[ir5];
    uint8_t ig_lin = srgb6_to_linear[ig6];
    uint8_t ib_lin = srgb5_to_linear[ib5];

    const unsigned char *p = (const unsigned char *)s;
    int cp;
    while ((cp = utf8_next(&p)) >= 0) {
        int gi = glyph_index(cp);
        if (gi < 0) {
            x += a->glyphs[0].advance;   /* treat as space-width */
            continue;
        }
        const atlas_glyph_t *gly = &a->glyphs[gi];
        const uint8_t *src = a->data + gly->data_offset;

        /* offset_y is "px below the ascender line" (PIL bbox convention).
         * Convert to framebuffer y: from the baseline, walk up by ascent
         * to the ascender, then down by offset_y to this glyph's top. */
        int gx = x + gly->offset_x;
        int gy = y - a->ascent + gly->offset_y;

        for (int j = 0; j < gly->h; j++) {
            int py = gy + j;
            if (py < 0 || py >= fb_h || py < cy0 || py >= cy1) continue;
            for (int i = 0; i < gly->w; i++) {
                int px = gx + i;
                if (px < cx0 || px >= cx1) continue;
                uint8_t alpha = src[j * gly->w + i];
                if (alpha == 0) continue;

                uint16_t *dst = &fb[py * fb_w + px];

                if (alpha == 255) {
                    *dst = ink;
                    continue;
                }

                uint16_t bg = *dst;
                uint8_t br_lin = srgb5_to_linear[(bg >> 11) & 0x1F];
                uint8_t bg_lin = srgb6_to_linear[(bg >> 5)  & 0x3F];
                uint8_t bb_lin = srgb5_to_linear[ bg        & 0x1F];

                /* Exact floor(x/255) for x in [0,65025] via add-shift — avoids
                 * three soft-divides (no HW divide on ARM7) per blended pixel,
                 * the hottest UI inner loop. Bit-identical to `/255`. */
                uint8_t inv = (uint8_t)(255 - alpha);
                unsigned xr = (unsigned)ir_lin * alpha + (unsigned)br_lin * inv;
                unsigned xg = (unsigned)ig_lin * alpha + (unsigned)bg_lin * inv;
                unsigned xb = (unsigned)ib_lin * alpha + (unsigned)bb_lin * inv;
                uint8_t r_lin = (uint8_t)((xr + 1 + (xr >> 8)) >> 8);
                uint8_t g_lin = (uint8_t)((xg + 1 + (xg >> 8)) >> 8);
                uint8_t b_lin = (uint8_t)((xb + 1 + (xb >> 8)) >> 8);

                uint8_t r5 = linear_to_srgb5[r_lin];
                uint8_t g6 = linear_to_srgb6[g_lin];
                uint8_t b5 = linear_to_srgb5[b_lin];
                *dst = (uint16_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | (uint16_t)b5);
            }
        }
        x += gly->advance;
    }
    return x;
}

int text_draw(uint16_t *fb, int fb_w, int fb_h, int x, int y,
              const char *s, const text_font_t *font, uint16_t ink) {
    return text_draw_c(fb, fb_w, fb_h, x, y, s, font, ink, 0, fb_w, 0, fb_h);
}

/* Like text_draw but writes only pixels with clip_x0 <= px < clip_x1 — the
 * horizontal window a marquee scrolls its text through. */
int text_draw_clip(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                   const char *s, const text_font_t *font, uint16_t ink,
                   int clip_x0, int clip_x1) {
    if (clip_x0 < 0)     clip_x0 = 0;
    if (clip_x1 > fb_w)  clip_x1 = fb_w;
    return text_draw_c(fb, fb_w, fb_h, x, y, s, font, ink, clip_x0, clip_x1, 0, fb_h);
}

/* text_draw_clip with an additional VERTICAL window [clip_y0, clip_y1) — used by
 * the marquee to keep a scrolling title inside its list row (so tall glyph tops
 * can't paint above the selection bar into the row above). */
int text_draw_clip_v(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                     const char *s, const text_font_t *font, uint16_t ink,
                     int clip_x0, int clip_x1, int clip_y0, int clip_y1) {
    if (clip_x0 < 0)     clip_x0 = 0;
    if (clip_x1 > fb_w)  clip_x1 = fb_w;
    return text_draw_c(fb, fb_w, fb_h, x, y, s, font, ink,
                       clip_x0, clip_x1, clip_y0, clip_y1);
}
