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
#include "../../hal/hal.h"

void list_view_init(list_view_t *v) {
    v->selected = 0;
    v->scroll_offset = 0;
}

void list_view_draw(const list_view_t *v,
                    const char * const *items, int count,
                    lcd_pixel_t fg, lcd_pixel_t bg, lcd_pixel_t selector_fg) {
    /* Clear the list region. */
    chrome_fill_rect(0, LIST_TOP_Y, LCD_WIDTH, LIST_VISIBLE_ROWS * LIST_ROW_H, bg);

    int first = v->scroll_offset;
    int last  = first + LIST_VISIBLE_ROWS;
    if (last > count) last = count;

    /* Use Nunito Regular 13px for body text. Baseline ~3 px above the
     * row bottom so descenders fit. */
    const atlas_t *body = &NUNITO_REGULAR_13;

    for (int i = first; i < last; i++) {
        int row_idx = i - first;
        int row_top = LIST_TOP_Y + row_idx * LIST_ROW_H;
        int baseline = row_top + LIST_ROW_H - 6;
        bool is_sel = (i == v->selected);

        if (is_sel) {
            /* Soft-rounded terracotta selector inset 4 px from each
             * side; text rendered in the cream "bg" color on top. */
            chrome_rounded_rect(4, row_top + 1,
                                LCD_WIDTH - 8, LIST_ROW_H - 2,
                                4, selector_fg);
            atlas_render(body, 12, baseline, items[i], bg);
        } else {
            atlas_render(body, 12, baseline, items[i], fg);
        }
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
