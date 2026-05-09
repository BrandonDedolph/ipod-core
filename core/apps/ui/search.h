/*
 * core/apps/ui/search.h — Search frame: on-screen keyboard + live
 * categorized results.
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
 *   |  ─── SONGS ───                  |   <- section header
 *   |  · Track title                  |   <- live filtered results
 *   |  ─── ALBUMS ───                 |
 *   |  · Album name                   |
 *   |  ─── ARTISTS ───                |
 *   |  · Artist name                  |
 *   +---------------------------------+
 *
 * Wheel cycles the keyboard cursor (or the results when focus shifts);
 * SELECT types the highlighted key, plays the highlighted song result,
 * or drills into the highlighted album/artist;
 * RIGHT moves focus from keyboard to results, LEFT moves it back;
 * MENU pops the frame.
 *
 * Match logic: case-insensitive ASCII substring against the field
 * native to each result type — title for songs, album-name for
 * albums, artist-name for artists. Songs additionally hit on artist
 * and album so typing an artist name surfaces their tracks even when
 * the title doesn't contain the word. Per-category caps keep the
 * result panel readable on a populous library.
 */

#ifndef CORE_APPS_UI_SEARCH_H
#define CORE_APPS_UI_SEARCH_H

#include "../../hal/hal.h"

#include <stdbool.h>

#define SEARCH_QUERY_MAX  32

/* Per-category caps. The result panel is small (~80 px); generous
 * caps would just hide deeper results behind scroll. Pick numbers
 * that keep the most relevant matches visible-on-arrival. */
#define SEARCH_MAX_SONGS   12
#define SEARCH_MAX_ALBUMS   6
#define SEARCH_MAX_ARTISTS  6

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
    SEARCH_RESULT_SONG   = 0,
    SEARCH_RESULT_ALBUM  = 1,
    SEARCH_RESULT_ARTIST = 2,
} search_result_type_t;

typedef struct {
    /* `idx` is interpreted by `type`: a tagcache song index for
     * SEARCH_RESULT_SONG, an album/artist uniq index for the
     * corresponding types. */
    search_result_type_t type;
    int                  idx;
} search_result_t;

typedef enum {
    SEARCH_ACT_NONE = 0,
    SEARCH_ACT_POP,
    SEARCH_ACT_PLAY,           /* play a song (out_idx = global song idx) */
    SEARCH_ACT_DRILL_ALBUM,    /* drill into Album (out_idx = album uniq idx) */
    SEARCH_ACT_DRILL_ARTIST,   /* drill into Artist (out_idx = artist uniq idx) */
} search_action_t;

typedef struct {
    char           query[SEARCH_QUERY_MAX];
    int            query_len;
    int            kb_cursor;        /* 0..SEARCH_KB_COUNT-1 */
    search_focus_t focus;
    int            result_selected;  /* row index within results[] */
    int            result_scroll;    /* topmost visible row index */
    /* Flat result list, ordered Songs → Albums → Artists. The widget
     * walks this in order at draw time and emits a section header
     * each time the type changes. */
    search_result_t results[SEARCH_MAX_SONGS + SEARCH_MAX_ALBUMS + SEARCH_MAX_ARTISTS];
    int            result_count;
} search_t;

/* Reset to a fresh empty-query state. */
void search_init(search_t *s);

/* Render the current frame into the LCD framebuffer. Caller calls
 * lcd_present afterward. */
void search_draw(const search_t *s);

/*
 * Feed a button event. The action codes return:
 *
 *   SEARCH_ACT_PLAY          out_idx = tagcache song idx (cabinet plays)
 *   SEARCH_ACT_DRILL_ALBUM   out_idx = album  uniq idx   (cabinet pushes Album drilldown)
 *   SEARCH_ACT_DRILL_ARTIST  out_idx = artist uniq idx   (cabinet pushes Artist drilldown)
 *   SEARCH_ACT_POP           caller pops the frame
 *   SEARCH_ACT_NONE          search frame stays as-is
 */
search_action_t search_handle_button(search_t *s, button_t btn,
                                     int *out_idx);

#endif /* CORE_APPS_UI_SEARCH_H */
