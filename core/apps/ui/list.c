/*
 * core/apps/ui/list.c — list view implementation.
 *
 * Rows are 22 px tall (room for 13px Nunito plus padding). Selector
 * is a soft-rounded terracotta band with cream text. Drill-in rows
 * get a right-edge chevron.
 */

#include "list.h"
#include "atlas.h"
#include "chrome.h"
#include "theme.h"
#include "../../hal/hal.h"

#include <stdint.h>

void list_view_init(list_view_t *v) {
    v->selected = 0;
    v->scroll_offset = 0;
}

/*
 * Chevron colors (faint, on each row's bg). Pre-composited approximations
 * of design's `opacity: sel ? 0.7 : 0.4` against the row bg, theme-aware.
 */
#define CHEV_UNSEL  theme_chev_unsel()
#define CHEV_SEL    theme_chev_sel()

/* Internal: shared draw routine. `provider` returns the i'th item
 * string. Both list_view_draw and list_view_draw_dyn delegate here.
 * `leading_at` is optional (NULL = no per-row leading visual). */
static void draw_internal(const list_view_t *v,
                          int count,
                          const char *(*provider)(int idx, void *user),
                          void *user,
                          list_leading_fn leading_at,
                          lcd_pixel_t fg, lcd_pixel_t bg,
                          lcd_pixel_t selector_fg);

/* Adapter: turn a (const char *const *items, int idx) pair into the
 * provider signature. */
static const char *array_provider(int idx, void *user) {
    const char *const *items = (const char *const *)user;
    return items[idx];
}

void list_view_draw(const list_view_t *v,
                    const char * const *items, int count,
                    lcd_pixel_t fg, lcd_pixel_t bg, lcd_pixel_t selector_fg) {
    draw_internal(v, count, array_provider, (void *)items,
                  NULL, fg, bg, selector_fg);
}

/* Wrapping the bare-fn callback in a small struct sidesteps the C
 * pedantic prohibition on converting an object pointer to a function
 * pointer (and back). */
typedef struct { const char *(*fn)(int); } bare_fn_holder_t;

static const char *bare_provider_glue(int idx, void *user) {
    return ((bare_fn_holder_t *)user)->fn(idx);
}

void list_view_draw_dyn(const list_view_t *v,
                        int count,
                        const char *(*item_at)(int idx),
                        lcd_pixel_t fg, lcd_pixel_t bg,
                        lcd_pixel_t selector_fg) {
    bare_fn_holder_t holder = { .fn = item_at };
    draw_internal(v, count, bare_provider_glue, &holder,
                  NULL, fg, bg, selector_fg);
}

void list_view_draw_dyn_leading(const list_view_t *v,
                                int count,
                                const char *(*item_at)(int idx),
                                list_leading_fn leading_at,
                                lcd_pixel_t fg, lcd_pixel_t bg,
                                lcd_pixel_t selector_fg) {
    bare_fn_holder_t holder = { .fn = item_at };
    draw_internal(v, count, bare_provider_glue, &holder,
                  leading_at, fg, bg, selector_fg);
}

static void draw_internal(const list_view_t *v,
                          int count,
                          const char *(*provider)(int idx, void *user),
                          void *user,
                          list_leading_fn leading_at,
                          lcd_pixel_t fg, lcd_pixel_t bg,
                          lcd_pixel_t selector_fg) {
    /* Clear the list region. */
    chrome_fill_rect(0, LIST_TOP_Y, LCD_WIDTH, LIST_VISIBLE_ROWS * LIST_ROW_H, bg);

    int first = v->scroll_offset;
    int last  = first + LIST_VISIBLE_ROWS;
    if (last > count) last = count;

    const atlas_t *body = &NUNITO_REGULAR_13;

    /* Indent the text column when the list reserves a leading slot;
     * keep the alignment whether or not a given row's leading_at fires,
     * so rows without art don't have their text jump leftward. */
    int leading_x = 14;
    int text_x    = leading_at ? (leading_x + LIST_LEADING_W + 8) : 14;

    for (int i = first; i < last; i++) {
        int row_idx = i - first;
        int row_top = LIST_TOP_Y + row_idx * LIST_ROW_H;
        /* Baseline near row bottom; for 27 px rows with 13 px font,
         * baseline at row_top + LIST_ROW_H - 8 = top+19 keeps the
         * glyph vertically centered with descenders fitting. */
        int baseline = row_top + LIST_ROW_H - 8;
        bool is_sel = (i == v->selected);
        const char *label = provider(i, user);

        if (is_sel) {
            /* Selector: 6 px side margin, 4 px corner radius — per
             * the design's `margin: "0 6px"; borderRadius: 4`. */
            chrome_rounded_rect(6, row_top,
                                LCD_WIDTH - 12, LIST_ROW_H,
                                4, selector_fg);
            atlas_render(body, text_x, baseline, label, bg);
        } else {
            atlas_render(body, text_x, baseline, label, fg);
        }

        /* Optional leading visual (e.g. album-art thumbnail), centered
         * vertically within the row. The callback owns the slot
         * entirely, including no-art fallbacks. */
        if (leading_at) {
            int lead_y = row_top + (LIST_ROW_H - LIST_LEADING_H) / 2;
            leading_at(i, leading_x, lead_y);
        }

        /*
         * Trailing chevron at right edge — thin angle bracket (›).
         * 7 px tall × 4 px wide; right-aligned with 14 px padding
         * matching the design's `padding: '7px 14px'`. Vertically
         * centered against the text x-height.
         */
        int chev_h = 7;
        int chev_w = chev_h / 2 + 1;
        int chev_x = LCD_WIDTH - 14 - chev_w;
        int chev_y = row_top + (LIST_ROW_H - chev_h) / 2;
        chrome_chevron(chev_x, chev_y, chev_h,
                       is_sel ? CHEV_SEL : CHEV_UNSEL);
    }
}

static void clamp_scroll(list_view_t *v, int count) {
    if (v->selected < v->scroll_offset) {
        v->scroll_offset = v->selected;
    }
    if (v->selected >= v->scroll_offset + LIST_VISIBLE_ROWS) {
        v->scroll_offset = v->selected - LIST_VISIBLE_ROWS + 1;
    }
    if (v->scroll_offset < 0) v->scroll_offset = 0;
    int max_offset = count - LIST_VISIBLE_ROWS;
    if (max_offset < 0) max_offset = 0;
    if (v->scroll_offset > max_offset) v->scroll_offset = max_offset;
}

bool list_view_handle_button(list_view_t *v, button_t btn, int count) {
    if (count <= 0) return false;
    switch (btn) {
        case BUTTON_SCROLL_FWD:
            if (v->selected < count - 1) v->selected++;
            clamp_scroll(v, count);
            return true;
        case BUTTON_SCROLL_BACK:
            if (v->selected > 0) v->selected--;
            clamp_scroll(v, count);
            return true;
        default:
            return false;
    }
}
