/*
 * core/apps/ui/now_playing.c — NP screen renderer.
 */

#include "now_playing.h"
#include "atlas.h"
#include "chrome.h"
#include "../audio/engine.h"
#include "../../hal/hal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------- Linen palette ---------------------------------------- */

#define COL_INK      lcd_rgb(0x1A, 0x17, 0x14)
#define COL_CREAM    lcd_rgb(0xF4, 0xF1, 0xEC)
#define COL_ACCENT   lcd_rgb(0xC4, 0x5A, 0x3A)
#define COL_FAINT    lcd_rgb(0xC8, 0xC0, 0xB4)   /* art placeholder + bar bg */
#define COL_INK_SOFT lcd_rgb(0x60, 0x55, 0x4A)   /* artist / format text */

/* ---------- Snapshot --------------------------------------------- */

void now_playing_load(now_playing_t *np, const audio_engine_t *engine) {
    /* Hardcoded for the FLAC fixture demo. When tagcache + ID3 land,
     * this becomes a real metadata read. */
    snprintf(np->title,  NP_TITLE_MAX,  "Test Sine 440 Hz");
    snprintf(np->artist, NP_ARTIST_MAX, "Cabinet Demo");
    snprintf(np->format, NP_FORMAT_MAX, "FLAC %u kHz",
             engine->sample_rate / 1000);

    np->total_frames = (uint32_t)engine->decoder.total_frames;
    np->sample_rate  = engine->sample_rate;
    np->loaded       = true;
}

/* ---------- Helpers ---------------------------------------------- */

static void status_bar(const char *title) {
    chrome_fill_rect(0, 0, LCD_WIDTH, 20, COL_INK);
    const atlas_t *t = &NUNITO_BOLD_17;
    int w = atlas_text_width(t, title);
    int x = (LCD_WIDTH - w) / 2;
    atlas_render(t, x, 16, title, COL_CREAM);
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

/* Center a string of the given atlas at horizontal `cx`, baseline `y`. */
static void draw_centered(const atlas_t *a, int cx, int y_baseline,
                          const char *s, lcd_pixel_t fg) {
    int w = atlas_text_width(a, s);
    atlas_render(a, cx - w / 2, y_baseline, s, fg);
}

/* ---------- Draw --------------------------------------------------- */

void now_playing_draw(const now_playing_t *np, const audio_engine_t *engine) {
    /* Background. */
    lcd_fill(COL_CREAM);

    /* Status bar — title is the track name. */
    status_bar(np->loaded ? np->title : "Now Playing");

    /*
     * Layout (within the 220 px below the status bar):
     *   y =  24..123  album art (100x100, centered horizontally)
     *   y = 132..147  artist (regular 13)
     *   y = 152..167  album/format (regular 13, soft ink)
     *   y = 198..200  progress bar (3 px tall, full inset)
     *   y = 210..226  time labels (regular 13, ink)
     */

    /* --- Album art placeholder. Centered 100x100 stub rect. --- */
    int art_w = 100, art_h = 100;
    int art_x = (LCD_WIDTH - art_w) / 2;
    int art_y = 28;
    chrome_rounded_rect(art_x, art_y, art_w, art_h, 6, COL_FAINT);
    /* Inset accent bar in the center to make it obvious this is a
     * placeholder, not real art. */
    chrome_fill_rect(art_x + 36, art_y + 46, 28, 8, COL_ACCENT);

    /* --- Artist (the title is in the status bar; make sense to put
     * artist + album/format here so we don't repeat). --- */
    if (np->loaded) {
        draw_centered(&NUNITO_REGULAR_13, LCD_WIDTH / 2, 142,
                      np->artist, COL_INK);
        draw_centered(&NUNITO_REGULAR_13, LCD_WIDTH / 2, 158,
                      np->format, COL_INK_SOFT);
    }

    /* --- Progress bar + time labels. --- */
    uint32_t played = (uint32_t)engine->read_idx;
    uint32_t total  = np->total_frames;
    uint32_t played_s = (np->sample_rate > 0) ? played / np->sample_rate : 0;
    uint32_t total_s  = (np->sample_rate > 0 && total > 0)
                        ? total / np->sample_rate : 0;

    /* Bar geometry: full-width with 24 px side margins, 3 px tall. */
    int bar_x = 24, bar_y = 198, bar_w = LCD_WIDTH - 48, bar_h = 3;
    chrome_fill_rect(bar_x, bar_y, bar_w, bar_h, COL_FAINT);

    int fill_w = 0;
    if (total > 0 && played > 0) {
        if (played > total) played = total;
        /* Avoid uint overflow on huge tracks. */
        fill_w = (int)((uint64_t)played * (uint64_t)bar_w / (uint64_t)total);
    }
    if (fill_w > 0) {
        chrome_fill_rect(bar_x, bar_y, fill_w, bar_h, COL_ACCENT);
    }

    /* Time labels — elapsed left, total right. */
    char left[16], right[16];
    format_time(played_s, left, sizeof(left));
    if (total > 0) {
        format_time(total_s, right, sizeof(right));
    } else {
        snprintf(right, sizeof(right), "--:--");
    }
    int label_y = 220;
    atlas_render(&NUNITO_REGULAR_13, bar_x, label_y, left, COL_INK);
    int rw = atlas_text_width(&NUNITO_REGULAR_13, right);
    atlas_render(&NUNITO_REGULAR_13,
                 bar_x + bar_w - rw, label_y, right, COL_INK);
}
