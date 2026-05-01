/*
 * core/apps/ui/now_playing.c — NP screen renderer.
 *
 * Layout matches Theme 1 ("Linen") in design_handoff_rockbox_theme/
 * themes.jsx — flex row: 84x84 album art on the left, text stack on
 * the right. Status bar carries "Now Playing" + battery glyph.
 *
 * The art is a diagonal-stripe placeholder until ID3 + JPEG decode
 * lands. Title / artist / album hardcoded for the FLAC fixture
 * pending tagcache integration.
 */

#include "now_playing.h"
#include "atlas.h"
#include "chrome.h"
#include "../audio/engine.h"
#include "../../hal/hal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------- Linen palette (per themes.jsx Theme 1) -------------- */

#define COL_BG          lcd_rgb(0xF4, 0xF1, 0xEC)   /* paper */
#define COL_INK         lcd_rgb(0x1A, 0x17, 0x14)   /* primary text */
#define COL_INK_DEEP    lcd_rgb(0x5A, 0x50, 0x48)   /* artist / time */
#define COL_INK_MUTED   lcd_rgb(0x9A, 0x8E, 0x80)   /* labels / album */
#define COL_TRACK_FAINT lcd_rgb(0xD8, 0xD2, 0xC8)   /* progress-bar bg */

/*
 * Album art stripe: two close-luminance warm tans, per the JSX
 * `repeating-linear-gradient(135deg, oklch(0.78 0.04 30) 0 6px,
 *                                    oklch(0.74 0.04 30) 6px 12px)`.
 * OKLCH(0.78, 0.04, 30) ≈ #CDB8A6, OKLCH(0.74, 0.04, 30) ≈ #C0AB99 —
 * subtle ~5% luminance difference, not the high-contrast tans I had.
 */
#define COL_STRIPE_A    lcd_rgb(0xCD, 0xB8, 0xA6)
#define COL_STRIPE_B    lcd_rgb(0xC0, 0xAB, 0x99)

/* ---------- Snapshot --------------------------------------------- */

void now_playing_load(now_playing_t *np, const audio_engine_t *engine) {
    snprintf(np->title,  NP_TITLE_MAX,  "Test Sine 440 Hz");
    snprintf(np->artist, NP_ARTIST_MAX, "Cabinet Demo");
    snprintf(np->format, NP_FORMAT_MAX, "FLAC %u kHz",
             engine->sample_rate / 1000);

    np->total_frames = (uint32_t)engine->decoder.total_frames;
    np->sample_rate  = engine->sample_rate;
    np->loaded       = true;
}

/* ---------- Helpers ---------------------------------------------- */

/* Status bar specific to the NP screen — "Now Playing" left, simple
 * battery indicator + clock text right. */
/*
 * NP status bar per design_handoff_rockbox_theme/themes.jsx
 * Theme1NowPlaying (line 178+): cream background (no band), padding
 * 8 px top + 12 px sides, "Now Playing" title-case Bold-11 ink left,
 * battery glyph right. Shuffle/repeat icons are still TODO.
 */
static void np_status_bar(int battery_pct) {
    /* Background already filled cream by the caller. */
    atlas_render(&NUNITO_BOLD_11, 12, 17, "Now Playing", COL_INK);
    int bat_x = LCD_WIDTH - 12 - 31;
    chrome_battery(bat_x, 8, battery_pct, COL_INK);
}

static void format_time(uint32_t total_seconds, char *buf, size_t buflen) {
    if (total_seconds >= 3600) {
        snprintf(buf, buflen, "%u:%02u:%02u",
                 total_seconds / 3600,
                 (total_seconds / 60) % 60,
                 total_seconds % 60);
    } else {
        snprintf(buf, buflen, "%u:%02u",
                 total_seconds / 60,
                 total_seconds % 60);
    }
}

/* ---------- Draw --------------------------------------------------- */

void now_playing_draw(const now_playing_t *np, const audio_engine_t *engine) {
    /* Background. */
    lcd_fill(COL_BG);

    /* Status bar — battery is a stub (always 87%) until power HAL lands. */
    np_status_bar(87);

    /*
     * Content area below status bar (y >= 18). Padding 18 px sides;
     * art on the left at 84x84, text stack on the right starting
     * at art_x + art_w + gap.
     */
    int pad_x  = 18;
    int art_w  = 84, art_h = 84;
    int art_x  = pad_x;
    int art_y  = 26;
    int gap    = 14;
    int text_x = art_x + art_w + gap;       /* = 116 */

    /* Album art placeholder: rounded diagonal stripes. */
    chrome_diagonal_stripes(art_x, art_y, art_w, art_h,
                            6, 4, COL_STRIPE_A, COL_STRIPE_B);
    /* Thin inset border to crisp the edge. */
    chrome_fill_rect(art_x + 1, art_y + 1,         art_w - 2, 1,
                     lcd_rgb(0xA0, 0x88, 0x70));

    /*
     * Text stack:
     *   "TRACK 1 OF 1" — Bold-9 muted (track counter)  baseline ~38
     *   Title          — Bold-17 ink                   baseline ~58
     *   Artist         — Regular-13 ink-deep           baseline ~78
     *   Album          — Regular-11 ink-muted          baseline ~94
     */
    if (np->loaded) {
        atlas_render(&NUNITO_BOLD_9,    text_x, 38,
                     "TRACK 1 OF 1",   COL_INK_MUTED);
        atlas_render(&NUNITO_BOLD_17,   text_x, 60,
                     np->title,        COL_INK);
        atlas_render(&NUNITO_REGULAR_13, text_x, 80,
                     np->artist,       COL_INK_DEEP);
        atlas_render(&NUNITO_REGULAR_11, text_x, 96,
                     np->format,       COL_INK_MUTED);
    }

    /* ---- Progress bar + time labels (bottom band). ---- */

    uint32_t played   = (uint32_t)engine->read_idx;
    uint32_t total    = np->total_frames;
    uint32_t played_s = (np->sample_rate > 0) ? played / np->sample_rate : 0;
    uint32_t remain_s = (total > played && np->sample_rate > 0)
                        ? (total - played) / np->sample_rate : 0;

    /* Bar: full-width minus 18 px sides, 3 px tall; ink fill on faint track. */
    int bar_x = pad_x, bar_y = 222;
    int bar_w = LCD_WIDTH - 2 * pad_x;
    int bar_h = 3;
    chrome_fill_rect(bar_x, bar_y, bar_w, bar_h, COL_TRACK_FAINT);
    if (total > 0 && played > 0) {
        if (played > total) played = total;
        int fill_w = (int)((uint64_t)played * (uint64_t)bar_w / (uint64_t)total);
        chrome_fill_rect(bar_x, bar_y, fill_w, bar_h, COL_INK);
    }

    /* Time labels above the bar — current left, remaining right
     * (with leading "-" per the design spec). */
    char left[16], right[16];
    format_time(played_s, left, sizeof(left));
    if (total > 0) {
        char tmp[16];
        format_time(remain_s, tmp, sizeof(tmp));
        snprintf(right, sizeof(right), "-%s", tmp);
    } else {
        snprintf(right, sizeof(right), "--:--");
    }
    int label_y = 215;
    atlas_render(&NUNITO_BOLD_9, bar_x, label_y, left, COL_INK_DEEP);
    int rw = atlas_text_width(&NUNITO_BOLD_9, right);
    atlas_render(&NUNITO_BOLD_9, bar_x + bar_w - rw, label_y, right, COL_INK_DEEP);
}
