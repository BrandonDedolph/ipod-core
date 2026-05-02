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

#include "../audio/engine.h"
#include "../../hal/hal.h"

#include <stdbool.h>
#include <stdint.h>

#define NP_TITLE_MAX  64
#define NP_ARTIST_MAX 64
#define NP_FORMAT_MAX 32

#define NP_NEXT_MAX   80

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
 * Album-art buffers. Two sizes lined up with the two pages that show
 * art:
 *   - default page (NP_PAGE_DEFAULT): 84 × 84 in the inline art rect
 *   - big-art page (NP_PAGE_BIG_ART): 180 × 180 centered on dark bg
 *
 * Both live inline in now_playing_t (no heap). 14 112 + 64 800 =
 * 78 912 bytes total — fits comfortably in the static cabinet_t. When
 * a song has embedded art we decode + nearest-neighbor scale into both
 * buffers at load time; if neither decode succeeds the renderers fall
 * back to their respective stripe placeholders.
 */
#define NP_ART_W      84
#define NP_ART_H      84
#define NP_ART_BIG_W  180
#define NP_ART_BIG_H  180

typedef struct {
    char     title[NP_TITLE_MAX];
    char     artist[NP_ARTIST_MAX];
    char     album[NP_TITLE_MAX];          /* the album text under artist */
    char     format[NP_FORMAT_MAX];        /* "FLAC" / "MP3" / etc — short */
    char     format_detail[NP_FORMAT_MAX]; /* "44.1 kHz" / "192 kbps" / etc */
    char     up_next[NP_NEXT_MAX];         /* next-track title */
    int      stars;                        /* 0..5 */
    uint32_t total_frames;     /* 0 if unknown */
    uint32_t sample_rate;
    bool     loaded;
    np_page_t page;

    /* Decoded album art for the default + big-art pages. art_loaded
     * is set true after a successful decode of *both* buffers; if
     * either fails, both pages fall back to stripes. */
    uint16_t art_pixels    [NP_ART_W     * NP_ART_H];
    uint16_t art_pixels_big[NP_ART_BIG_W * NP_ART_BIG_H];
    bool     art_loaded;
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
 * Also clears the album-art buffer (art_loaded = false). Caller
 * follows up with now_playing_set_art_jpeg if the song has embedded
 * art.
 *
 * Call right after audio_engine_play() returns success.
 */
void now_playing_load(now_playing_t *np, const audio_engine_t *engine);

/*
 * Decode `jpeg_bytes` into the NP album-art buffer (NP_ART_W ×
 * NP_ART_H RGB565). Sets art_loaded=true on success; on failure
 * leaves art_loaded as-is (the caller's pre-set value, typically
 * false from now_playing_load) and returns -1.
 *
 * Pass NULL to clear the buffer (art_loaded = false). The default
 * page falls back to the diagonal-stripe placeholder when not loaded.
 */
int now_playing_set_art_jpeg(now_playing_t *np,
                             const void *jpeg_bytes, size_t jpeg_len);

/*
 * Render the NP screen into the LCD framebuffer. `engine` is read
 * non-destructively to compute the scrubber position and time
 * elapsed; the audio engine continues playing meanwhile.
 *
 * Caller calls lcd_present() afterward.
 */
void now_playing_draw(const now_playing_t *np, const audio_engine_t *engine);

#endif /* CORE_APPS_UI_NOW_PLAYING_H */
