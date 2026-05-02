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
#include "../db/tagcache.h"
#include "../../codecs/dr_flac/flac.h"
#include "../../codecs/dr_mp3/mp3.h"
#include "../../hal/hal.h"

#include <ctype.h>
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

static const char *const empty_items[] = {"(empty)"};
static const int empty_actions[] = { ACT_NOOP };

/*
 * A menu either has static items (const arrays + count) OR a dynamic
 * provider (count_fn + item_fn from the tagcache). Selecting any
 * dynamic-menu row is a no-op for now; album/song drilldown lands
 * with playlist routing in a later PR.
 */
typedef struct {
    const char         *title;
    const char *const  *items;          /* NULL for dynamic menus */
    const int          *actions;
    int                 count;
    int               (*count_fn)(void);
    const char       *(*item_fn)(int idx);
} menu_t;

static const menu_t MENUS[M_COUNT] = {
    [M_MAIN]   = {"iPod",  main_items,  main_actions,  6},
    [M_MUSIC]  = {"Music", music_items, music_actions, 6},

    /* Tagcache-backed flat lists. */
    [M_MUSIC_ARTISTS] = {
        .title = "Artists",
        .count_fn = tagcache_artist_count,
        .item_fn  = tagcache_artist_name,
    },
    [M_MUSIC_ALBUMS] = {
        .title = "Albums",
        .count_fn = tagcache_album_count,
        .item_fn  = tagcache_album_name,
    },
    [M_MUSIC_SONGS] = {
        .title = "Songs",
        .count_fn = tagcache_song_count,
        .item_fn  = tagcache_song_title,
    },
    [M_MUSIC_GENRES] = {
        .title = "Genres",
        .count_fn = tagcache_genre_count,
        .item_fn  = tagcache_genre_name,
    },

    [M_PLAYLISTS]  = {"Playlists",  empty_items, empty_actions, 1},
    [M_PODCASTS]   = {"Podcasts",   empty_items, empty_actions, 1},
    [M_AUDIOBOOKS] = {"Audiobooks", empty_items, empty_actions, 1},
    [M_SETTINGS]   = {"Settings",   empty_items, empty_actions, 1},
};

/* Helpers that paper over the static-vs-dynamic distinction. */
static int menu_count(const menu_t *m) {
    return m->items ? m->count : m->count_fn();
}
static const char *menu_item(const menu_t *m, int idx) {
    if (m->items) return m->items[idx];
    return m->item_fn(idx);
}
static int menu_action(const menu_t *m, int idx) {
    if (m->actions) return m->actions[idx];
    return ACT_NOOP;
}

/* ---------- Internal helpers -------------------------------------- */

#define FIXTURE_PATH "tests/codec-vectors/sine_440hz_1s_44k_s16_stereo.flac"

static long load_file(const char *path, void **out_buf) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return -1; }
    void *buf = malloc((size_t)n);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    *out_buf = buf;
    return n;
}

/*
 * Pick a decoder by file extension. Returns NULL for unknown formats.
 * `format_label` (e.g. "FLAC", "MP3") is written into `*out_label` for
 * the NP screen badge.
 */
static const decoder_ops_t *decoder_for_path(const char *path,
                                             const char **out_label) {
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    char ext[8];
    size_t ext_len = strlen(dot + 1);
    if (ext_len >= sizeof(ext)) return NULL;
    for (size_t i = 0; i < ext_len; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[ext_len] = 0;
    if (strcmp(ext, "flac") == 0) {
        if (out_label) *out_label = "FLAC";
        return flac_decoder_ops();
    }
    if (strcmp(ext, "mp3") == 0) {
        if (out_label) *out_label = "MP3";
        return mp3_decoder_ops();
    }
    return NULL;
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

/*
 * Menu status bar per themes.jsx MainMenu (line 467+):
 *   padding "9px 14px 8px" + borderBottom 1px rgba(26,23,20,0.08)
 *   "iPod" Bold-13 ink upper-left; Battery upper-right.
 */
static void draw_status_bar(const char *title) {
    /* Title left, Bold-13 ink. Baseline at y=22 (9 padding + ~13 ascender). */
    atlas_render(&NUNITO_BOLD_13, 14, 22, title, COL_INK);
    /* Battery right. Vertically aligned with the title's cap height. */
    int bat_x = LCD_WIDTH - 14 - 31;
    chrome_battery(bat_x, 12, 78, COL_INK);
    /* Border line at y=30 — pre-composited 8% ink on cream. */
    chrome_fill_rect(0, 30, LCD_WIDTH, 1,
                     lcd_rgb(0xE2, 0xDE, 0xDA));
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

    long n = load_file(FIXTURE_PATH, &c->fixture_bytes);
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
    /* Per themes.jsx:464, selBg = ink, selFg = cream.
     * Terracotta (COL_ACCENT) is reserved for progress / play
     * indicators, NOT the menu selector. */
    if (m->items) {
        list_view_draw(&v, m->items, m->count,
                       COL_INK, COL_CREAM, COL_INK);
    } else {
        list_view_draw_dyn(&v, m->count_fn(), m->item_fn,
                           COL_INK, COL_CREAM, COL_INK);
    }
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

    /* Now Playing frame: SELECT cycles the info pages; wheel and
     * LEFT/RIGHT are no-ops for now (prev/next-track + scrub land
     * with proper playlists). */
    if (current_kind(c) == FRAME_NOW_PLAYING) {
        if (btn == BUTTON_SELECT) {
            now_playing_advance_page(&c->np);
            log_printf("cabinet: NP page -> %d", c->np.page);
        }
        return;
    }

    /* Menu frame: list-view scroll first, then SELECT for action. */
    int mid = current_menu(c);
    const menu_t *m = &MENUS[mid];
    int count = menu_count(m);

    list_view_t v = current_view(c);
    if (list_view_handle_button(&v, btn, count)) {
        store_view(c, &v);
        return;
    }

    if (btn == BUTTON_SELECT) {
        int idx = c->list_state[c->depth - 1].selected;
        if (idx < 0 || idx >= count) return;

        /* Songs menu: if a library is loaded and this row maps to a
         * real file, play it. Otherwise fall through to the generic
         * action dispatch (which logs a stub). */
        if (mid == M_MUSIC_SONGS) {
            const char *path = tagcache_song_path(idx);
            if (path) {
                const char *label = NULL;
                const decoder_ops_t *ops = decoder_for_path(path, &label);
                if (!ops) {
                    log_printf("cabinet: %s — unknown format", path);
                    return;
                }
                void *bytes = NULL;
                long len = load_file(path, &bytes);
                if (len <= 0) {
                    log_printf("cabinet: read failed %s", path);
                    return;
                }
                int rc = audio_engine_play(c->engine, ops, bytes, (size_t)len);
                free(bytes);    /* engine took its own copy */
                if (rc != 0) {
                    log_printf("cabinet: play failed %s rc=%d", path, rc);
                    return;
                }
                now_playing_load(&c->np, c->engine);
                /* Pull title/artist/album from the tagcache (already
                 * parsed during library_load). tagcache_song_title
                 * always returns a non-NULL string (TITLE tag or
                 * filename fallback); artist/album may be NULL when
                 * the file had no such tag. */
                snprintf(c->np.title, NP_TITLE_MAX, "%s",
                         tagcache_song_title(idx));
                const char *artist = tagcache_song_artist(idx);
                const char *album  = tagcache_song_album(idx);
                if (artist) snprintf(c->np.artist, NP_ARTIST_MAX, "%s", artist);
                else c->np.artist[0] = 0;
                if (album)  snprintf(c->np.album,  NP_TITLE_MAX,  "%s", album);
                else c->np.album[0]  = 0;
                snprintf(c->np.format, NP_FORMAT_MAX, "%s", label);
                snprintf(c->np.format_detail, NP_FORMAT_MAX, "%u kHz",
                         c->engine->sample_rate / 1000);
                push_now_playing(c);
                log_printf("cabinet: now playing %s", path);
                return;
            }
            /* No path mapping: fall through to the stub log. */
        }

        int act = menu_action(m, idx);
        if (act == ACT_NOOP) {
            log_printf("cabinet: %s -> %s (stub)",
                       m->title, menu_item(m, idx));
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
