/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/chrome.h — the shared list-and-panel chrome of the Cabinet/Linen UI.
 *
 * WHY THIS FILE EXISTS
 *
 * Two things were wrong before it.
 *
 * First, every one of these primitives lived as a `static` inside
 * kernel/main.c, which the host build cannot compile at all (meson.build only
 * builds kernel/ for target == 'hw', because main.c reaches into the hw headers and
 * USEC_TIMER). So the entire interactive surface of the firmware — lists,
 * headers, selection bars, scroll windows — could only be verified by flashing
 * the device and looking at it. Nothing here needs hardware: it draws into an
 * RGB565 back buffer through console.h and measures glyphs through text.h,
 * both of which already build and test on the host.
 *
 * Second, ui/screen_settings.c carried a hand-maintained SECOND COPY of the
 * same kit — its own header, selection bar, scrollbar, scroll window and
 * rounded rect, with its own layout constants — and said so in a comment
 * admitting the two had to be "kept in step" by hand. They had already
 * drifted: main.c's rounded rect memoizes a bitwise integer sqrt, while the
 * settings copy kept the per-scanline multiply loop that replaced it, and the
 * taller two-line row geometry (ROW_H2/LIST_ROWS2) landed only in main.c, so
 * Settings still scrolled with the old 24px feel.
 *
 * One definition, compiled into both the device image and the host tests.
 *
 * DEPENDENCIES: console.h (framebuffer + damage tracking), text.h (glyph
 * measurement and drawing), palette.h (the Linen colours), hal.h for
 * LCD_WIDTH/LCD_HEIGHT only. No hw/, no MMIO, no globals from main.c.
 */

#ifndef CORE_UI_CHROME_H
#define CORE_UI_CHROME_H

#include <stdint.h>

#include "text.h"
#include "palette.h"

/* ---------- Theme tokens --------------------------------------------
 * The tokens keep their LINEN_ names but resolve to the live, theme-selected
 * palette (palette.h): g_pal[] is swapped as a block by theme_set() when
 * Settings -> Theme changes (Linen / Onyx dark). The names are historical —
 * under Onyx these carry the dark values. SEL_BG/SEL_FG stay derived from
 * INK/SURFACE so the inverted selection bar reads right in both.
 */
#define LINEN_SURFACE g_pal[PAL_SURFACE]
#define LINEN_INK     g_pal[PAL_INK]
#define LINEN_MUTED   g_pal[PAL_MUTED]
#define LINEN_ACCENT  g_pal[PAL_ACCENT]
#define LINEN_BORDER  g_pal[PAL_BORDER]
#define LINEN_SEL_BG  g_pal[PAL_INK]     /* selection bar = ink (inverts w/ theme)*/
#define LINEN_SEL_FG  g_pal[PAL_SURFACE] /* selection text = surface              */
#define LINEN_MUTED2  g_pal[PAL_MUTED2]
#define LINEN_MUTED_D g_pal[PAL_MUTED_D]
#define LINEN_SEL_SUB g_pal[PAL_SEL_SUB]
#define LINEN_CHEVRON g_pal[PAL_CHEVRON]
#define LINEN_SB_TRK  g_pal[PAL_SB_TRK]
#define LINEN_SB_THMB g_pal[PAL_SB_THMB]
#define LINEN_PLATE   g_pal[PAL_PLATE]   /* raised plate (volume overlay)         */
#define LINEN_TRK     g_pal[PAL_TRK]     /* slider / meter track                  */

/* ---------- Nunito faces --------------------------------------------- */
#define FONT_HEADER   text_font_bold_13()
#define FONT_ROW      text_font_regular_12()
#define FONT_TITLE    text_font_bold_18()
#define FONT_SUB      text_font_regular_11()
#define FONT_SMALL    text_font_regular_9()

/* ---------- Vertical layout (menus.jsx) -----------------------------
 * A status strip up top, a titled header with a divider, then the
 * scrolling list. These were duplicated verbatim in screen_settings.c.
 */
#define STATUS_Y0  0      /* status strip band (battery/track)          */
#define STATUS_H   15     /* strip height                                */
#define HDR_BASE   30     /* header title text baseline                  */
#define HDR_DIV_Y  38     /* header divider row                          */
#define LIST_Y0    42     /* first list row top                          */
#define ROW_H      24     /* px per single-line list row                 */
#define LIST_ROWS  8      /* visible rows: (240-42)/24 ~= 8              */

/*
 * Two-line rows (album/song list: title + artist sub) get a taller row so the
 * bold title (bold_13, 19px ink) and the artist line (regular_9, 14px) don't
 * vertically overlap — 32px clears both so the marquee is cleanly title-only
 * (no artist repaint, no clipping of descenders). Fewer fit on screen.
 */
#define ROW_H2     32
#define LIST_ROWS2 6      /* (240-42)/32 ~= 6                            */

/* ---------- Text ----------------------------------------------------
 * Thin wrappers over text.c with the panel dimensions baked in, plus the
 * damage reporting a partial present depends on. Drawing text through
 * text_draw() directly and forgetting the damage call is how pixels get
 * silently dropped from a damage-only present.
 */

/* Draw a NUL-terminated string; `y` is the text BASELINE. Returns the pen
 * position after the last glyph (i.e. x + advance). */
int  ui_text(int x, int y, const char *s, const text_font_t *font, uint16_t ink);

/* As ui_text, clipped horizontally to [cx0, cx1). Glyphs outside are not
 * drawn and are not reported as damage. */
int  ui_text_clip(int x, int y, const char *s, const text_font_t *font,
                  uint16_t ink, int cx0, int cx1);

/* Centre a string horizontally at baseline `y`. */
void ui_text_centered(int y, const char *s, const text_font_t *font,
                      uint16_t ink);

/* ---------- Shapes --------------------------------------------------- */

/*
 * Bitwise integer square root — ~4 iterations for the radii used here, and no
 * multiply in the loop. Public because the anti-aliased modal corners and the
 * circular art masks in main.c need the same primitive; the form it replaced
 * stepped r up one at a time WITH a multiply, per scanline, on every list
 * repaint.
 */
int ui_isqrt(int v);

/* Largest corner radius the inset/coverage caches are sized for. */
#define UI_RR_MAX_R 16

/*
 * Filled rounded rectangle. The design's selection bars (r=4) and plates
 * (r=6) are rounded, not square. r < 1 degenerates to a plain fill; r is
 * clamped to half the smaller side and to an internal maximum.
 */
void ui_round_rect(int x, int y, int w, int h, int r, uint16_t c);

/* ---------- List furniture ------------------------------------------- */

/*
 * The titled header: optional back chevron, title on the left, an optional
 * right-hand value (a "3 / 12" count, typically), and the divider rule.
 */
void ui_header(const char *title, const char *right, int back);

/*
 * One list row at list origin `y0`, row index `r`, row height `rh`.
 *
 * Selected rows get a filled rounded ink bar and light bold text; greyed rows
 * are muted (an inactive menu item). `sub` adds a second line, `right` a
 * right-aligned value, `chevron` a trailing disclosure arrow, and `chip` a
 * leading 22x22 RGB565 art thumbnail (NULL for none). `title_priority` gives
 * the title the space the right value would otherwise take.
 */
void ui_list_row(int y0, int r, const char *text, const char *sub,
                 const char *right, int chevron, int selected, int greyed,
                 const uint16_t *chip, int title_priority, int rh);

/* The right-edge scrollbar. Draws nothing when everything fits. */
void ui_scrollbar(int y0, int top, int visible, int total);

/*
 * How a row title that overflows its column gets drawn.
 *
 * The device scrolls it (a marquee), which needs a TIME SOURCE — and a time
 * source is the one thing in this file that cannot be portable. Rather than
 * reach for USEC_TIMER and drag the whole hw layer in behind it, ui_list_row
 * calls out through this hook: main.c registers the marquee, and anything
 * without a clock (the host tests, a screenshot renderer) leaves it unset and
 * gets a plainly clipped title instead.
 *
 * Unset is the default and is always safe: NULL means "clip it".
 */
typedef void (*ui_scroll_text_fn)(int x, int y, int w, const char *text,
                                  const text_font_t *font, uint16_t ink,
                                  uint16_t bg, int cy0, int cy1);
void ui_set_scroll_text(ui_scroll_text_fn fn);

/*
 * Windowed-list scroll origin (menus.jsx useScrollWindow): keep the selection
 * about 1/3 from the top, clamped to the ends. PURE — the browser derives the
 * visible window from the selection each paint, so there is no separate `top`
 * state that can fall out of sync with the selection.
 */
int  ui_scroll_window(int sel, int total, int visible);

#endif /* CORE_UI_CHROME_H */
