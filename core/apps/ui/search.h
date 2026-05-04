/*
 * core/apps/ui/search.h — Search frame: on-screen keyboard + live results.
 *
 * A click-wheel search screen modeled on the iPod 5G's "Search":
 *   +---------------------------------+
 *   |          status bar             |
 *   |  query|                         |   <- typed query + caret
 *   |                                 |
 *   |  A B C D E F G                  |   <- 4×7 keyboard
 *   |  H I J K L M N                  |
 *   |  O P Q R S T U                  |
 *   |  V W X Y Z _ <                  |
 *   |                                 |
 *   |  Result row 1                   |   <- live filtered titles
 *   |  Result row 2                   |
 *   +---------------------------------+
 *
 * Wheel cycles the keyboard cursor (or the results when focus shifts);
 * SELECT types the highlighted key (or plays the highlighted result);
 * RIGHT moves focus from keyboard to results, LEFT moves it back;
 * MENU pops the frame.
 *
 * Match logic: case-insensitive ASCII substring against song titles.
 * The first SEARCH_RESULT_MAX matches are kept in `results[]` as global
 * tagcache song indexes; cabinet's play_global_song handles the rest.
 */

#ifndef CORE_APPS_UI_SEARCH_H
#define CORE_APPS_UI_SEARCH_H

#include "../../hal/hal.h"

#include <stdbool.h>

#define SEARCH_QUERY_MAX  32
#define SEARCH_RESULT_MAX 64

/* 26 letters + space + delete; arranged 4 rows × 7 cols. */
#define SEARCH_KB_COLS    7
#define SEARCH_KB_ROWS    4
#define SEARCH_KB_COUNT   (SEARCH_KB_COLS * SEARCH_KB_ROWS)
#define SEARCH_KEY_SPACE  26
#define SEARCH_KEY_DELETE 27

typedef enum {
    SEARCH_FOCUS_KB      = 0,
    SEARCH_FOCUS_RESULTS = 1,
} search_focus_t;

typedef enum {
    SEARCH_ACT_NONE = 0,
    SEARCH_ACT_POP,
    SEARCH_ACT_PLAY,
} search_action_t;

typedef struct {
    char           query[SEARCH_QUERY_MAX];
    int            query_len;
    int            kb_cursor;        /* 0..SEARCH_KB_COUNT-1 */
    search_focus_t focus;
    int            result_selected;  /* row index within results[] */
    int            result_scroll;    /* topmost visible row index */
    int            results[SEARCH_RESULT_MAX];  /* global song idxs */
    int            result_count;
} search_t;

/* Reset to a fresh empty-query state. */
void search_init(search_t *s);

/* Render the current frame into the LCD framebuffer. Caller calls
 * lcd_present afterward. */
void search_draw(const search_t *s);

/*
 * Feed a button event. On SEARCH_ACT_PLAY, *out_global_idx is the
 * tagcache song index the caller should play (and push NP for); on
 * SEARCH_ACT_POP, the caller pops the frame; SEARCH_ACT_NONE means
 * the search frame stays as-is.
 */
search_action_t search_handle_button(search_t *s, button_t btn,
                                     int *out_global_idx);

#endif /* CORE_APPS_UI_SEARCH_H */
