/*
 * core/apps/ui/cabinet.c — Cabinet shell implementation.
 *
 * Frame stack: each level is either a menu (FRAME_MENU, indexed into
 * the static MENUS table) or the Now Playing screen (FRAME_NOW_PLAYING,
 * uses cabinet_t.np). MENU pops; SELECT drills in or triggers an
 * action. PLAY toggles pause/resume globally regardless of frame.
 */

#include "cabinet.h"
#include "atlas.h"
#include "chrome.h"
#include "list.h"
#include "now_playing.h"
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
    M_COUNT
};

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

static frame_kind_t current_kind(const cabinet_t *c) {
    return c->stack_kind[c->depth - 1];
}

static int current_menu(const cabinet_t *c) {
    return c->stack_menu[c->depth - 1];
}

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

static void draw_status_bar(const char *title) {
    chrome_fill_rect(0, 0, LCD_WIDTH, 20, COL_INK);
    const atlas_t *t = &NUNITO_BOLD_17;
    int w = atlas_text_width(t, title);
    int x = (LCD_WIDTH - w) / 2;
    atlas_render(t, x, 16, title, COL_CREAM);
}

static void push_menu(cabinet_t *c, int menu_id) {
    if (c->depth >= 8) return;
    c->stack_kind[c->depth] = FRAME_MENU;
    c->stack_menu[c->depth] = menu_id;
    c->list_state[c->depth].selected = 0;
    c->list_state[c->depth].scroll_offset = 0;
    c->depth++;
}

static void push_now_playing(cabinet_t *c) {
    if (c->depth >= 8) return;
    c->stack_kind[c->depth] = FRAME_NOW_PLAYING;
    c->stack_menu[c->depth] = -1;
    c->depth++;
}

static void pop_frame(cabinet_t *c) {
    if (c->depth > 1) c->depth--;
}

/* ---------- Public API -------------------------------------------- */

void cabinet_init(cabinet_t *c, audio_engine_t *engine) {
    memset(c, 0, sizeof(*c));
    c->engine = engine;
    c->stack_kind[0] = FRAME_MENU;
    c->stack_menu[0] = M_MAIN;
    c->depth = 1;

    long n = load_fixture(FIXTURE_PATH, &c->fixture_bytes);
    if (n > 0) c->fixture_len = (size_t)n;
}

void cabinet_draw(cabinet_t *c) {
    if (current_kind(c) == FRAME_NOW_PLAYING) {
        now_playing_draw(&c->np, c->engine);
        return;
    }

    int mid = current_menu(c);
    const menu_t *m = &MENUS[mid];

    lcd_fill(COL_CREAM);
    draw_status_bar(m->title);

    list_view_t v = current_view(c);
    list_view_draw(&v, m->items, m->count, COL_INK, COL_CREAM, COL_ACCENT);
}

void cabinet_handle_button(cabinet_t *c, button_t btn) {
    /* PLAY: global pause/resume — works on any frame as long as
     * something is loaded. */
    if (btn == BUTTON_PLAY) {
        if (audio_engine_is_playing(c->engine)) {
            audio_engine_pause(c->engine);
            log_printf("cabinet: pause");
        } else if (c->engine->has_decoder) {
            audio_engine_resume(c->engine);
            log_printf("cabinet: resume");
        }
        return;
    }

    /* MENU: pop, regardless of frame kind. */
    if (btn == BUTTON_MENU) {
        pop_frame(c);
        return;
    }

    /* Now Playing frame: only PLAY/MENU are meaningful. Wheel and
     * SELECT are no-ops here for now (next-track / scrub will land
     * with proper playlists). */
    if (current_kind(c) == FRAME_NOW_PLAYING) {
        return;
    }

    /* Menu frame: list-view scroll first, then SELECT for action. */
    int mid = current_menu(c);
    const menu_t *m = &MENUS[mid];

    list_view_t v = current_view(c);
    if (list_view_handle_button(&v, btn, m->count)) {
        store_view(c, &v);
        return;
    }

    if (btn == BUTTON_SELECT) {
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
                if (rc == 0) {
                    now_playing_load(&c->np, c->engine);
                    push_now_playing(c);
                    log_printf("cabinet: now playing");
                } else {
                    log_printf("cabinet: play failed rc=%d", rc);
                }
            } else {
                log_printf("cabinet: no fixture; can't play");
            }
        } else if (act >= 0 && act < M_COUNT) {
            push_menu(c, act);
        }
    }
}
