/*
 * core/apps/ui/list.h — vertical list view widget.
 *
 * The iPod's iconic UI — a column of left-aligned text rows with a
 * highlighted selector that the user moves with the click wheel.
 *
 * Stateless API: caller owns the list_view_t struct, the items
 * array (const char *[]), and the count. Widget renders into the
 * LCD framebuffer (caller calls lcd_present afterward). Buttons are
 * fed in one at a time; the widget updates `selected` and
 * `scroll_offset` and returns whether the input was consumed.
 */

#ifndef CORE_APPS_UI_LIST_H
#define CORE_APPS_UI_LIST_H

#include "../../hal/hal.h"

#include <stdbool.h>
#include <stdint.h>

/* Geometry per themes.jsx MainMenu (line 467+):
 *   Header: padding 9/14/8 + 13 px font + 1 px border = 31 px tall
 *   List padding: 6 px top
 *   Row: padding 7px top/bottom + 13 px font = 27 px each
 *
 * 7 visible rows × 27 = 189 px. Plus header 31 + list padding 6 + 6 =
 * 232 px. Leaves 8 px below — same proportion as the mockup. */
#define LIST_ROW_H        27
#define LIST_VISIBLE_ROWS 7
#define LIST_TOP_Y        37    /* header(31) + list-padding(6) */

typedef struct list_view {
    int selected;        /* 0..count-1; the highlighted row */
    int scroll_offset;   /* index of the topmost visible row */
} list_view_t;

/*
 * Reset state — call once when constructing or re-entering a list.
 */
void list_view_init(list_view_t *v);

/*
 * Draw the list into the LCD framebuffer.
 *
 * `items` is an array of `count` C strings; the widget shows up to
 * LIST_VISIBLE_ROWS at once and scrolls so `selected` is always
 * visible. Caller's job to lcd_present() afterward.
 */
void list_view_draw(const list_view_t *v,
                    const char * const *items, int count,
                    lcd_pixel_t fg, lcd_pixel_t bg, lcd_pixel_t selector_fg);

/*
 * Feed a button press. Returns true if the input was consumed (i.e.,
 * scroll up/down). SELECT/MENU pass through unconsumed for the caller
 * to act on.
 */
bool list_view_handle_button(list_view_t *v, button_t btn, int count);

#endif /* CORE_APPS_UI_LIST_H */
