/*
 * core/apps/ui/cabinet.h — Cabinet shell.
 *
 * Replaces the Rockbox plugin's nested-loop navigation with a simple
 * stack of "frames," each one a list_view_t over a different set of
 * items. Pushing a frame goes deeper; popping returns to the parent.
 *
 * Today's scope (PR 11):
 *   - Main menu: Music / Playlists / Podcasts / Audiobooks / Settings / Now Playing
 *   - Music sub-menu (Artists / Albums / Songs / Genres) — children stub-render
 *   - Other top-level entries are stubs (do nothing on select)
 *   - Selecting Now Playing triggers the FLAC-fixture playback we
 *     already have in the audio engine
 *
 * Tagcache integration, real artist/album lists, and the actual Now
 * Playing screen come in subsequent PRs.
 */

#ifndef CORE_APPS_UI_CABINET_H
#define CORE_APPS_UI_CABINET_H

#include "../../hal/hal.h"
#include "../audio/engine.h"
#include "now_playing.h"
#include "search.h"

#include <stdbool.h>

/*
 * cabinet_t — the shell state. Held in BSS via a single static
 * instance in sim/main.c; not designed to be reentrant.
 */
typedef struct cabinet cabinet_t;

/* Initialize the shell. Doesn't touch the HAL. */
void cabinet_init(cabinet_t *c, audio_engine_t *engine);

/*
 * Draw the current frame into the framebuffer. Caller calls
 * lcd_present afterward.
 */
void cabinet_draw(cabinet_t *c);

/*
 * Feed a button event. Updates internal state, may trigger
 * playback, may pop frames, etc.
 */
void cabinet_handle_button(cabinet_t *c, button_t btn);

/*
 * Frame kind — what's on each level of the nav stack. Most frames
 * are list-view menus; the Now Playing screen is a different shape
 * so it gets its own kind.
 */
typedef enum {
    FRAME_MENU = 0,
    FRAME_NOW_PLAYING,
    FRAME_SEARCH,
} frame_kind_t;

/* Forward decl + storage size — exposed so callers can statically
 * allocate one. Definition is in cabinet.c. */
#define CABINET_MAX_DEPTH      8
#define CABINET_FRAME_TITLE_MAX 64

struct cabinet {
    /* Frame stack. Each frame has a kind + per-kind state. */
    frame_kind_t   stack_kind[CABINET_MAX_DEPTH];
    int            stack_menu[CABINET_MAX_DEPTH];     /* FRAME_MENU: menu id */
    int            depth;             /* number of frames on the stack */

    /* List view state per stack level, only meaningful for FRAME_MENU
     * frames. Pop restores scroll/selection. */
    struct {
        int selected;
        int scroll_offset;
    } list_state[CABINET_MAX_DEPTH];

    /* For filtered drilldown frames: the artist_idx or album_idx to
     * filter by; -1 for unfiltered frames. The dynamic title (e.g.
     * the artist's name) is stored alongside so the status bar shows
     * "Aphex Twin" rather than the menu's static label. */
    int            frame_filter[CABINET_MAX_DEPTH];
    char           frame_title [CABINET_MAX_DEPTH][CABINET_FRAME_TITLE_MAX];

    audio_engine_t *engine;           /* for triggering playback */
    now_playing_t   np;               /* current NP snapshot, if any */
    search_t        search;           /* search frame state */

    /* The bytes of the FLAC fixture, loaded once at init for the
     * Now Playing demo. NULL if not loaded. */
    void  *fixture_bytes;
    size_t fixture_len;
};

#endif /* CORE_APPS_UI_CABINET_H */
