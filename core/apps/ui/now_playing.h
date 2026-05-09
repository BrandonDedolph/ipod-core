/*
 * core/apps/ui/now_playing.h — Now Playing screen.
 *
 * The iconic iPod NP screen. Layout:
 *
 *   +---------------------------------+
 *   |          status bar             |
 *   |                                 |
 *   |        [ album art ]            |
 *   |                                 |
 *   |        Track title              |
 *   |        Artist                   |
 *   |        Album • format           |
 *   |                                 |
 *   |  0:01            ▓▓▓▓░░░  0:03  |
 *   +---------------------------------+
 *
 * Album art is a stub rectangle until ID3 + JPEG decode lands.
 * Title / artist / format come from a snapshot taken at play() time
 * — the audio engine's metadata is captured into now_playing_t so
 * the NP screen keeps rendering even after EOS.
 *
 * The scrubber animates in real time from the audio engine's
 * `read_idx` (frames played out the DAC) and `decoder.total_frames`.
 * If total_frames is 0 (some lossy codecs don't expose it without a
 * stream scan), the scrubber stays at 0 and shows "--:--" for total.
 */

#ifndef CORE_APPS_UI_NOW_PLAYING_H
#define CORE_APPS_UI_NOW_PLAYING_H

#include "art_cache.h"
#include "../audio/engine.h"
#include "../../hal/hal.h"

#include <stdbool.h>
#include <stdint.h>

#define NP_TITLE_MAX  64
#define NP_ARTIST_MAX 64
#define NP_FORMAT_MAX 32

#define NP_NEXT_MAX   80

/* Path field is wider than title/artist because filesystem paths
 * (esp. nested album folders) often blow past 64 chars; the track-
 * info page truncates with an ellipsis past this. */
#define NP_PATH_MAX   192

/*
 * The four NP pages, cycled through with center button (SELECT) per
 * the design (system-screens.jsx). Order matches the JSX:
 *   0 = default (title/art/scrubber/up-next)
 *   1 = big art (full-screen on dark backdrop)
 *   2 = peak meter (synthesized levels per channel)
 *   3 = track info (key-value rows)
 */
typedef enum {
    NP_PAGE_DEFAULT     = 0,
    NP_PAGE_BIG_ART     = 1,
    NP_PAGE_PEAK_METER  = 2,
    NP_PAGE_TRACK_INFO  = 3,
    NP_PAGE_COUNT       = 4,
} np_page_t;

/*
 * Album art lives in the shared art_cache (apps/ui/art_cache.h), keyed
 * by song_idx. The NP screen carries only the index; renderers ask the
 * cache for pixel buffers at draw time. Cache miss / decode failure /
 * no-embedded-art all surface as a NULL pixel pointer, and renderers
 * fall back to the diagonal-stripe placeholder on those rows. Moving
 * the buffers out of now_playing_t saves ~78 KB per cabinet_t and lets
 * future list-row thumbnails read from the same cache.
 */
#define NP_ART_W      ART_CACHE_SMALL_W
#define NP_ART_H      ART_CACHE_SMALL_H
#define NP_ART_BIG_W  ART_CACHE_BIG_W
#define NP_ART_BIG_H  ART_CACHE_BIG_H

typedef struct {
    char     title[NP_TITLE_MAX];
    char     artist[NP_ARTIST_MAX];
    char     album[NP_TITLE_MAX];          /* the album text under artist */
    char     format[NP_FORMAT_MAX];        /* "FLAC" / "MP3" / etc — short */
    char     format_detail[NP_FORMAT_MAX]; /* "44.1 kHz" / "192 kbps" / etc */
    char     path[NP_PATH_MAX];            /* filesystem path, for track-info page */
    char     up_next[NP_NEXT_MAX];         /* next-track title */
    uint32_t total_frames;     /* 0 if unknown */
    uint32_t sample_rate;
    bool     loaded;
    np_page_t page;

    /* Tagcache song index for the loaded track; -1 if the screen was
     * loaded from a non-tagcache source (e.g. the FLAC fixture demo).
     * Renderers feed this to art_cache_get to fetch decoded pixels. */
    int      song_idx;
} now_playing_t;

/*
 * Cycle to the next page (called when the user presses SELECT while
 * the NP frame is active).
 */
void now_playing_advance_page(now_playing_t *np);

/*
 * Snapshot the engine's current metadata into `np`. Strings are
 * hardcoded for now (the FLAC fixture has no ID3); future PRs that
 * land tagcache + ID3 parsing will fill these from real metadata.
 *
 * Sets song_idx = -1; the caller can override it after this call when
 * the track came from the tagcache (cabinet's play path does), which
 * lets the renderers pull decoded art from art_cache. Without an
 * override the art rects stay at their stripe placeholders.
 *
 * Call right after audio_engine_play() returns success.
 */
void now_playing_load(now_playing_t *np, const audio_engine_t *engine);

/*
 * Render the NP screen into the LCD framebuffer. `engine` is read
 * non-destructively to compute the scrubber position and time
 * elapsed; the audio engine continues playing meanwhile.
 *
 * Caller calls lcd_present() afterward.
 */
void now_playing_draw(const now_playing_t *np, const audio_engine_t *engine);

#endif /* CORE_APPS_UI_NOW_PLAYING_H */
