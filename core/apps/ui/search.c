/*
 * core/apps/ui/search.c — Search frame implementation.
 */

#include "search.h"
#include "atlas.h"
#include "chrome.h"
#include "../db/tagcache.h"
#include "../../hal/hal.h"

#include <stdint.h>
#include <string.h>

#define COL_INK    lcd_rgb(0x1A, 0x17, 0x14)
#define COL_CREAM  lcd_rgb(0xF4, 0xF1, 0xEC)
#define COL_FAINT  lcd_rgb(0xE2, 0xDE, 0xDA)   /* cream-on-cream divider */

/* Layout constants. The status bar (y=0..30) is owned by the caller;
 * everything below is search-frame territory. */
#define QUERY_TOP        31
#define QUERY_H          27
#define KB_TOP           (QUERY_TOP + QUERY_H)   /* 58 */
#define KB_H             72                      /* 4 rows × 18 px */
#define KB_LEFT_MARGIN   6
#define KB_CELL_W        ((LCD_WIDTH - 2 * KB_LEFT_MARGIN) / SEARCH_KB_COLS)
#define KB_CELL_H        (KB_H / SEARCH_KB_ROWS)
#define RESULTS_TOP      (KB_TOP + KB_H)         /* 130 */
#define RESULTS_H        (LCD_HEIGHT - RESULTS_TOP)
#define RESULT_ROW_H     22
#define RESULTS_VISIBLE  ((LCD_HEIGHT - RESULTS_TOP - 4) / RESULT_ROW_H)

static char key_char(int idx) {
    if (idx < 0 || idx >= SEARCH_KB_COUNT) return 0;
    if (idx < 26) return (char)('A' + idx);
    if (idx == SEARCH_KEY_SPACE) return ' ';
    return 0;   /* delete is special — no character */
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive ASCII substring match. Returns 1 on hit, 0 on miss.
 * Empty needle matches everything. */
static int substr_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    if (!*needle) return 1;
    for (const char *h = haystack; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && ascii_lower(*a) == ascii_lower(*b)) {
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

/* Walk every song in the loaded library; collect the first
 * SEARCH_RESULT_MAX whose title contains the query (case-insensitive
 * ASCII). When the library is empty this leaves result_count = 0
 * and the screen shows no results — matching the "no library loaded"
 * state. */
static void rebuild_results(search_t *s) {
    s->result_count = 0;
    s->result_selected = 0;
    s->result_scroll = 0;
    int total = tagcache_song_count();
    for (int i = 0; i < total && s->result_count < SEARCH_RESULT_MAX; i++) {
        const char *title = tagcache_song_title(i);
        if (substr_ci(title, s->query)) {
            s->results[s->result_count++] = i;
        }
    }
    if (s->focus == SEARCH_FOCUS_RESULTS && s->result_count == 0) {
        s->focus = SEARCH_FOCUS_KB;
    }
}

void search_init(search_t *s) {
    memset(s, 0, sizeof(*s));
    s->focus = SEARCH_FOCUS_KB;
    /* Empty query matches every song; populate the result list so the
     * user sees something below the keyboard immediately. */
    rebuild_results(s);
}

/* ---------- Drawing -------------------------------------------------- */

static void draw_query_row(const search_t *s) {
    chrome_fill_rect(0, QUERY_TOP, LCD_WIDTH, QUERY_H, COL_CREAM);
    /* Bottom 1 px divider. */
    chrome_fill_rect(0, QUERY_TOP + QUERY_H - 1, LCD_WIDTH, 1, COL_FAINT);

    int x = 14;
    int baseline = QUERY_TOP + QUERY_H - 9;
    if (s->query_len == 0) {
        atlas_render(&NUNITO_REGULAR_13, x, baseline,
                     "Search music", COL_FAINT);
    } else {
        atlas_render(&NUNITO_BOLD_13, x, baseline, s->query, COL_INK);
        x += atlas_text_width(&NUNITO_BOLD_13, s->query);
    }
    /* Caret: thin 1×11 ink rect just past the last character (or at the
     * placeholder's left edge when the query is empty). */
    int caret_x = (s->query_len == 0) ? 14 : x + 1;
    chrome_fill_rect(caret_x, baseline - 11, 1, 12, COL_INK);
}

static void draw_keyboard(const search_t *s) {
    chrome_fill_rect(0, KB_TOP, LCD_WIDTH, KB_H, COL_CREAM);

    bool kb_focused = (s->focus == SEARCH_FOCUS_KB);
    for (int row = 0; row < SEARCH_KB_ROWS; row++) {
        for (int col = 0; col < SEARCH_KB_COLS; col++) {
            int idx = row * SEARCH_KB_COLS + col;
            int cx  = KB_LEFT_MARGIN + col * KB_CELL_W;
            int cy  = KB_TOP + row * KB_CELL_H;
            bool sel = (idx == s->kb_cursor);

            /* Cell background: ink if this is the selected cell AND
             * keyboard has focus; faint outline if selected but focus
             * is on results (so user knows where they'll return); else
             * nothing (transparent on cream). */
            if (sel && kb_focused) {
                chrome_rounded_rect(cx + 2, cy + 1,
                                    KB_CELL_W - 4, KB_CELL_H - 2,
                                    3, COL_INK);
            } else if (sel && !kb_focused) {
                chrome_outline_rect(cx + 2, cy + 1,
                                    KB_CELL_W - 4, KB_CELL_H - 2,
                                    3, COL_FAINT);
            }

            lcd_pixel_t fg = (sel && kb_focused) ? COL_CREAM : COL_INK;
            int baseline = cy + KB_CELL_H - 5;

            if (idx == SEARCH_KEY_DELETE) {
                /* Render a chunky left-arrow as the delete key glyph.
                 * 9px tall, drawn from a few horizontal lines so it
                 * reads at a glance. Center it within the cell. */
                int gw = 9, gh = 9;
                int gx = cx + (KB_CELL_W - gw) / 2;
                int gy = cy + (KB_CELL_H - gh) / 2;
                /* Arrow shaft + head. */
                chrome_line(gx, gy + gh / 2, gx + gw - 1, gy + gh / 2, fg);
                chrome_line(gx + 3, gy,         gx, gy + gh / 2, fg);
                chrome_line(gx + 3, gy + gh - 1, gx, gy + gh / 2, fg);
            } else {
                char ch[2] = { 0, 0 };
                ch[0] = (idx == SEARCH_KEY_SPACE) ? '_' : key_char(idx);
                int tw = atlas_text_width(&NUNITO_BOLD_13, ch);
                int tx = cx + (KB_CELL_W - tw) / 2;
                atlas_render(&NUNITO_BOLD_13, tx, baseline, ch, fg);
            }
        }
    }
}

static void draw_results(const search_t *s) {
    chrome_fill_rect(0, RESULTS_TOP, LCD_WIDTH, RESULTS_H, COL_CREAM);
    /* Top divider. */
    chrome_fill_rect(0, RESULTS_TOP, LCD_WIDTH, 1, COL_FAINT);

    if (s->result_count == 0) {
        atlas_render(&NUNITO_REGULAR_11, 14, RESULTS_TOP + 18,
                     "No matches", COL_FAINT);
        return;
    }

    bool res_focused = (s->focus == SEARCH_FOCUS_RESULTS);
    int first = s->result_scroll;
    int last  = first + RESULTS_VISIBLE;
    if (last > s->result_count) last = s->result_count;

    for (int i = first; i < last; i++) {
        int row_idx = i - first;
        int row_top = RESULTS_TOP + 2 + row_idx * RESULT_ROW_H;
        bool sel = (i == s->result_selected);
        const char *title = tagcache_song_title(s->results[i]);
        int baseline = row_top + RESULT_ROW_H - 7;

        if (sel && res_focused) {
            chrome_rounded_rect(6, row_top, LCD_WIDTH - 12, RESULT_ROW_H,
                                4, COL_INK);
            atlas_render(&NUNITO_REGULAR_13, 14, baseline, title, COL_CREAM);
        } else {
            atlas_render(&NUNITO_REGULAR_13, 14, baseline, title, COL_INK);
        }
    }
}

void search_draw(const search_t *s) {
    /* Status bar is drawn by cabinet (consistent across all frames);
     * we own the rest. */
    draw_query_row(s);
    draw_keyboard(s);
    draw_results(s);
}

/* ---------- Input ---------------------------------------------------- */

static void clamp_result_scroll(search_t *s) {
    if (s->result_selected < s->result_scroll) {
        s->result_scroll = s->result_selected;
    }
    if (s->result_selected >= s->result_scroll + RESULTS_VISIBLE) {
        s->result_scroll = s->result_selected - RESULTS_VISIBLE + 1;
    }
    if (s->result_scroll < 0) s->result_scroll = 0;
    int max_off = s->result_count - RESULTS_VISIBLE;
    if (max_off < 0) max_off = 0;
    if (s->result_scroll > max_off) s->result_scroll = max_off;
}

static void apply_key(search_t *s, int idx) {
    if (idx == SEARCH_KEY_DELETE) {
        if (s->query_len > 0) {
            s->query[--s->query_len] = 0;
            rebuild_results(s);
        }
        return;
    }
    char c = key_char(idx);
    if (!c) return;
    if (s->query_len + 1 >= SEARCH_QUERY_MAX) return;   /* leave room for NUL */
    s->query[s->query_len++] = c;
    s->query[s->query_len]   = 0;
    rebuild_results(s);
}

search_action_t search_handle_button(search_t *s, button_t btn,
                                     int *out_global_idx) {
    if (out_global_idx) *out_global_idx = -1;

    if (btn == BUTTON_MENU) return SEARCH_ACT_POP;

    if (s->focus == SEARCH_FOCUS_KB) {
        switch (btn) {
            case BUTTON_SCROLL_FWD:
                s->kb_cursor = (s->kb_cursor + 1) % SEARCH_KB_COUNT;
                return SEARCH_ACT_NONE;
            case BUTTON_SCROLL_BACK:
                s->kb_cursor = (s->kb_cursor + SEARCH_KB_COUNT - 1) % SEARCH_KB_COUNT;
                return SEARCH_ACT_NONE;
            case BUTTON_SELECT:
                apply_key(s, s->kb_cursor);
                return SEARCH_ACT_NONE;
            case BUTTON_RIGHT:
                if (s->result_count > 0) s->focus = SEARCH_FOCUS_RESULTS;
                return SEARCH_ACT_NONE;
            default:
                return SEARCH_ACT_NONE;
        }
    }

    /* SEARCH_FOCUS_RESULTS */
    switch (btn) {
        case BUTTON_SCROLL_FWD:
            if (s->result_selected < s->result_count - 1) s->result_selected++;
            clamp_result_scroll(s);
            return SEARCH_ACT_NONE;
        case BUTTON_SCROLL_BACK:
            if (s->result_selected > 0) s->result_selected--;
            clamp_result_scroll(s);
            return SEARCH_ACT_NONE;
        case BUTTON_LEFT:
            s->focus = SEARCH_FOCUS_KB;
            return SEARCH_ACT_NONE;
        case BUTTON_SELECT:
            if (s->result_count > 0
                && s->result_selected >= 0
                && s->result_selected < s->result_count) {
                if (out_global_idx) {
                    *out_global_idx = s->results[s->result_selected];
                }
                return SEARCH_ACT_PLAY;
            }
            return SEARCH_ACT_NONE;
        default:
            return SEARCH_ACT_NONE;
    }
}
