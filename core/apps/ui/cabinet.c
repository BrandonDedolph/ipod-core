/*
 * core/apps/ui/cabinet.c — Cabinet shell implementation.
 *
 * Menu structure is a static table indexed by "menu id". A frame on
 * the stack is one menu id. Drilling in pushes a child menu id;
 * MENU button pops.
 *
 * Real Cabinet (the 1,900-line plugin) does live tagcache lookups
 * for artists/albums/songs at frame-push time. We stub that out for
 * now: the Music sub-menu's children render fixed placeholder text.
 * Wiring up real tagcache is its own PR.
 */

#include "cabinet.h"
#include "atlas.h"
#include "chrome.h"
#include "list.h"
#include "../audio/engine.h"
#include "../../codecs/dr_flac/flac.h"
#include "../../hal/hal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Linen palette ----------------------------------------- */

#define COL_INK    lcd_rgb(0x1A, 0x17, 0x14)
#define COL_CREAM  lcd_rgb(0xF4, 0xF1, 0xEC)
#define COL_ACCENT lcd_rgb(0xC4, 0x5A, 0x3A)

/* ---------- Menu definitions -------------------------------------- */

/*
 * Each menu is an id; the table maps id -> { title, items[], count,
 * action(menu_id, idx) -> next_menu_id_or_action }.
 *
 * Action codes:
 *   action >= 0  : push that menu id
 *   ACT_PLAY     : start playback of the FLAC fixture
 *   ACT_NOOP     : do nothing (placeholder)
 */

#define ACT_NOOP -1
#define ACT_PLAY -2

enum menu_id {
    M_MAIN = 0,
    M_MUSIC,
    M_MUSIC_ARTISTS,
    M_MUSIC_ALBUMS,
    M_MUSIC_SONGS,
    M_MUSIC_GENRES,
    M_PLAYLISTS,
    M_PODCASTS,
    M_AUDIOBOOKS,
    M_SETTINGS,
    /* Add new menus here, then append entries to MENUS below. */
    M_COUNT
};

/* For each row of each menu, what does selecting it do? An int
 * action code per item. */

static const char *const main_items[] = {
    "Music", "Playlists", "Podcasts", "Audiobooks", "Settings", "Now Playing"
};
static const int main_actions[] = {
    M_MUSIC, M_PLAYLISTS, M_PODCASTS, M_AUDIOBOOKS, M_SETTINGS, ACT_PLAY
};

static const char *const music_items[] = {
    "Artists", "Albums", "Songs", "Genres", "Composers", "Audiobooks"
};
static const int music_actions[] = {
    M_MUSIC_ARTISTS, M_MUSIC_ALBUMS, M_MUSIC_SONGS, M_MUSIC_GENRES, ACT_NOOP, ACT_NOOP
};

/* Stub child lists — no tagcache yet, just placeholder text. */
static const char *const stub_three[] = {
    "(no library indexed)", "Run: core release tagcache <dir>", "(stub)"
};
static const int stub_three_actions[] = { ACT_NOOP, ACT_NOOP, ACT_NOOP };

static const char *const empty_items[] = {"(empty)"};
static const int empty_actions[] = { ACT_NOOP };

typedef struct {
    const char         *title;
    const char *const  *items;
    const int          *actions;
    int                 count;
} menu_t;

static const menu_t MENUS[M_COUNT] = {
    [M_MAIN]            = {"iPod",       main_items,    main_actions,    6},
    [M_MUSIC]           = {"Music",      music_items,   music_actions,   6},
    [M_MUSIC_ARTISTS]   = {"Artists",    stub_three,    stub_three_actions, 3},
    [M_MUSIC_ALBUMS]    = {"Albums",     stub_three,    stub_three_actions, 3},
    [M_MUSIC_SONGS]     = {"Songs",      stub_three,    stub_three_actions, 3},
    [M_MUSIC_GENRES]    = {"Genres",     stub_three,    stub_three_actions, 3},
    [M_PLAYLISTS]       = {"Playlists",  empty_items,   empty_actions,   1},
    [M_PODCASTS]        = {"Podcasts",   empty_items,   empty_actions,   1},
    [M_AUDIOBOOKS]      = {"Audiobooks", empty_items,   empty_actions,   1},
    [M_SETTINGS]        = {"Settings",   empty_items,   empty_actions,   1},
};

/* ---------- Internal helpers -------------------------------------- */

#define FIXTURE_PATH "tests/codec-vectors/sine_440hz_1s_44k_s16_stereo.flac"

static long load_fixture(const char *path, void **out_buf) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    void *buf = malloc((size_t)n);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    *out_buf = buf;
    return n;
}

static int current_menu(const cabinet_t *c) {
    return c->stack[c->depth - 1];
}

/* Convert the cabinet's per-frame state slot to a list_view_t for
 * the current frame. */
static list_view_t current_view(const cabinet_t *c) {
    list_view_t v = {
        .selected      = c->list_state[c->depth - 1].selected,
        .scroll_offset = c->list_state[c->depth - 1].scroll_offset,
    };
    return v;
}

static void store_view(cabinet_t *c, const list_view_t *v) {
    c->list_state[c->depth - 1].selected      = v->selected;
    c->list_state[c->depth - 1].scroll_offset = v->scroll_offset;
}

/* Status bar — full-width ink band + centered title in Bold-17. */
static void draw_status_bar(const char *title) {
    chrome_fill_rect(0, 0, LCD_WIDTH, 20, COL_INK);
    const atlas_t *t = &NUNITO_BOLD_17;
    int w = atlas_text_width(t, title);
    int x = (LCD_WIDTH - w) / 2;
    /* Baseline near the bottom of the 20px band so descenders fit. */
    atlas_render(t, x, 16, title, COL_CREAM);
}

/* ---------- Public API -------------------------------------------- */

void cabinet_init(cabinet_t *c, audio_engine_t *engine) {
    memset(c, 0, sizeof(*c));
    c->engine = engine;
    c->stack[0] = M_MAIN;
    c->depth = 1;

    if (load_fixture(FIXTURE_PATH, &c->fixture_bytes) > 0) {
        /* Cache the size in our struct so we don't re-stat. Reuse
         * the long return value via a re-stat — simpler than smuggling
         * it back out. Actually let's just save it directly. */
    }
    /* Cleaner: re-call load to get the length. */
    if (c->fixture_bytes) {
        free(c->fixture_bytes);
        c->fixture_bytes = NULL;
    }
    long n = load_fixture(FIXTURE_PATH, &c->fixture_bytes);
    if (n > 0) c->fixture_len = (size_t)n;
}

void cabinet_draw(cabinet_t *c) {
    int mid = current_menu(c);
    const menu_t *m = &MENUS[mid];

    /* Background first. */
    lcd_fill(COL_CREAM);

    draw_status_bar(m->title);

    list_view_t v = current_view(c);
    list_view_draw(&v, m->items, m->count, COL_INK, COL_CREAM, COL_ACCENT);
}

static void push_frame(cabinet_t *c, int menu_id) {
    if (c->depth >= 8) return;          /* stack guard */
    c->stack[c->depth] = menu_id;
    c->list_state[c->depth].selected = 0;
    c->list_state[c->depth].scroll_offset = 0;
    c->depth++;
}

static void pop_frame(cabinet_t *c) {
    if (c->depth > 1) c->depth--;
}

void cabinet_handle_button(cabinet_t *c, button_t btn) {
    int mid = current_menu(c);
    const menu_t *m = &MENUS[mid];

    /* Wheel scroll: hand to the list view first. */
    list_view_t v = current_view(c);
    if (list_view_handle_button(&v, btn, m->count)) {
        store_view(c, &v);
        return;
    }

    switch (btn) {
        case BUTTON_SELECT: {
            int idx = c->list_state[c->depth - 1].selected;
            if (idx < 0 || idx >= m->count) return;
            int act = m->actions[idx];
            if (act == ACT_NOOP) {
                log_printf("cabinet: %s -> %s (stub)",
                           m->title, m->items[idx]);
            } else if (act == ACT_PLAY) {
                if (c->fixture_bytes && c->fixture_len > 0) {
                    int rc = audio_engine_play(c->engine, flac_decoder_ops(),
                                               c->fixture_bytes, c->fixture_len);
                    log_printf("cabinet: play fixture rc=%d", rc);
                } else {
                    log_printf("cabinet: no fixture; can't play");
                }
            } else if (act >= 0 && act < M_COUNT) {
                push_frame(c, act);
            }
            break;
        }
        case BUTTON_MENU:
            pop_frame(c);
            break;
        case BUTTON_PLAY:
            /* Toggle pause/resume on the engine if a track is loaded. */
            if (audio_engine_is_playing(c->engine)) {
                audio_engine_pause(c->engine);
                log_printf("cabinet: pause");
            } else if (c->engine->has_decoder) {
                audio_engine_resume(c->engine);
                log_printf("cabinet: resume");
            }
            break;
        default:
            break;
    }
}
