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

typedef struct {
    char     title[NP_TITLE_MAX];
    char     artist[NP_ARTIST_MAX];
    char     format[NP_FORMAT_MAX];
    uint32_t total_frames;     /* 0 if unknown */
    uint32_t sample_rate;
    bool     loaded;
} now_playing_t;

/*
 * Snapshot the engine's current metadata into `np`. Strings are
 * hardcoded for now (the FLAC fixture has no ID3); future PRs that
 * land tagcache + ID3 parsing will fill these from real metadata.
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
