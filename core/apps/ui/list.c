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

/*
 * Chevron colors (faint, on each row's bg). Hardcoded approximations
 * of design's `opacity: sel ? 0.7 : 0.4` against the row bg.
 * Pre-composited so atlas_render's alpha blender doesn't have to know.
 */
#define CHEV_UNSEL  lcd_rgb(0xB0, 0xA8, 0x9E)   /* 0.4 ink on cream */
#define CHEV_SEL    lcd_rgb(0xA0, 0x9C, 0x97)   /* 0.7 cream on ink */

void list_view_draw(const list_view_t *v,
                    const char * const *items, int count,
                    lcd_pixel_t fg, lcd_pixel_t bg, lcd_pixel_t selector_fg) {
    /* Clear the list region. */
    chrome_fill_rect(0, LIST_TOP_Y, LCD_WIDTH, LIST_VISIBLE_ROWS * LIST_ROW_H, bg);

    int first = v->scroll_offset;
    int last  = first + LIST_VISIBLE_ROWS;
    if (last > count) last = count;

    const atlas_t *body = &NUNITO_REGULAR_13;

    for (int i = first; i < last; i++) {
        int row_idx = i - first;
        int row_top = LIST_TOP_Y + row_idx * LIST_ROW_H;
        /* Baseline near row bottom; for 27 px rows with 13 px font,
         * baseline at row_top + LIST_ROW_H - 8 = top+19 keeps the
         * glyph vertically centered with descenders fitting. */
        int baseline = row_top + LIST_ROW_H - 8;
        bool is_sel = (i == v->selected);

        if (is_sel) {
            /* Selector: 6 px side margin, 4 px corner radius — per
             * the design's `margin: "0 6px"; borderRadius: 4`. */
            chrome_rounded_rect(6, row_top,
                                LCD_WIDTH - 12, LIST_ROW_H,
                                4, selector_fg);
            atlas_render(body, 14, baseline, items[i], bg);
        } else {
            atlas_render(body, 14, baseline, items[i], fg);
        }

        /* Trailing chevron at right edge. Sized ~6 px square,
         * vertically centered against the text x-height. */
        int chev_size = 6;
        int chev_x    = LCD_WIDTH - 14 - chev_size;
        int chev_y    = baseline - 9;   /* roughly centered with text */
        chrome_chevron(chev_x, chev_y, chev_size,
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
