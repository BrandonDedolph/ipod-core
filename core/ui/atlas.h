/*
 * core/apps/ui/atlas.h — pre-rasterized glyph atlas.
 *
 * One atlas per (font, size) combo. Generated at fixture-time by
 * tools/atlas_gen.sh; the C side just loads the resulting .h and
 * renders. No runtime TTF parsing, no FreeType dependency in the
 * firmware — exactly what PLAN.md prescribes.
 *
 * Atlases are first-class consts: they live in .rodata, the C
 * compiler resolves them at link time, no init step needed.
 *
 * Pixel format: alpha coverage. 0 = transparent, 255 = full fg.
 * atlas_render alpha-blends fg into whatever the framebuffer
 * already holds at each pixel.
 */

#ifndef CORE_APPS_UI_ATLAS_H
#define CORE_APPS_UI_ATLAS_H

#include "../hal/hal.h"

#include <stdint.h>

/*
 * Per-glyph layout. ASCII-only for now (95 printable code points,
 * 0x20..0x7E); the table is indexed by (codepoint - 0x20).
 *
 * data_offset  byte offset into atlas_t.data where this glyph's
 *              alpha rows begin. Rows are h × w bytes packed with
 *              no padding (width != stride). Bytes are 0..255.
 * w, h         glyph bitmap dimensions in pixels.
 * offset_x     x offset from pen position to top-left of bitmap.
 * offset_y     px BELOW the ascender line where this glyph's ink
 *              starts. PIL convention; positive means "this glyph's
 *              top is N px below the ascender." atlas_render converts
 *              to baseline-relative coordinates internally.
 * advance      pen advance after drawing this glyph, in pixels.
 */
typedef struct {
    uint16_t data_offset;
    uint8_t  w;
    uint8_t  h;
    int8_t   offset_x;
    int8_t   offset_y;
    uint8_t  advance;
} atlas_glyph_t;

typedef struct {
    const atlas_glyph_t *glyphs;          /* 95 entries */
    const uint8_t       *data;            /* concatenated glyph alpha rows */
    int8_t               ascent;          /* px above baseline */
    int8_t               descent;         /* px below baseline (positive) */
    int8_t               line_height;     /* recommended row spacing */
} atlas_t;

/* The atlases we ship (declared here, defined in generated .h files
 * pulled in by atlas.c). */
extern const atlas_t NUNITO_REGULAR_9;
extern const atlas_t NUNITO_REGULAR_11;
extern const atlas_t NUNITO_REGULAR_13;
extern const atlas_t NUNITO_BOLD_9;
extern const atlas_t NUNITO_BOLD_11;
extern const atlas_t NUNITO_BOLD_13;
extern const atlas_t NUNITO_BOLD_17;

/*
 * Measure a string in pixels using the given atlas.
 */
int atlas_text_width(const atlas_t *a, const char *s);

/*
 * Render `s` at (x, y) where y is the *baseline* — the bottom of
 * non-descender glyphs. This is the standard typography origin.
 *
 * Alpha-blends `fg` into existing framebuffer pixels using each
 * glyph's coverage values. Clips to the LCD bounds. Skips
 * non-printable / out-of-range chars silently.
 */
void atlas_render(const atlas_t *a, int x, int y_baseline,
                  const char *s, lcd_pixel_t fg);

#endif /* CORE_APPS_UI_ATLAS_H */
