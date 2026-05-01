/*
 * core/apps/ui/now_playing.c — NP screen renderer.
 *
 * Layout matches Theme 1 ("Linen") in design_handoff_rockbox_theme/
 * themes.jsx Theme1NowPlaying. Status bar with shuffle/repeat icons
 * + battery; flex row art/text; star rating + format badge below
 * the album line; "Up next" mini-row near the bottom; progress bar +
 * time labels at the very bottom.
 *
 * Album art is a diagonal-stripe placeholder until ID3 + JPEG decode
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
#define COL_STAR_MUTED  lcd_rgb(0xCC, 0xC4, 0xB7)   /* unfilled stars */

/* Album art stripes — subtle warm tans matching oklch(0.78/0.74). */
#define COL_STRIPE_A    lcd_rgb(0xCD, 0xB8, 0xA6)
#define COL_STRIPE_B    lcd_rgb(0xC0, 0xAB, 0x99)

/* ---------- Snapshot --------------------------------------------- */

void now_playing_load(now_playing_t *np, const audio_engine_t *engine) {
    /* Hardcoded for the FLAC fixture demo. ID3 + tagcache integration
     * will replace these with real reads. */
    snprintf(np->title,         NP_TITLE_MAX,  "Test Sine 440 Hz");
    snprintf(np->artist,        NP_ARTIST_MAX, "Cabinet Demo");
    snprintf(np->album,         NP_TITLE_MAX,  "Codec Vectors");
    snprintf(np->format,        NP_FORMAT_MAX, "FLAC");
    snprintf(np->format_detail, NP_FORMAT_MAX, "%u kHz",
             engine->sample_rate / 1000);
    snprintf(np->up_next,       NP_NEXT_MAX,   "(end of test playlist)");
    np->stars = 4;

    np->total_frames = (uint32_t)engine->decoder.total_frames;
    np->sample_rate  = engine->sample_rate;
    np->loaded       = true;
}

/* ---------- Helpers ---------------------------------------------- */

/*
 * NP status bar per themes.jsx Theme1NowPlaying (line 178+):
 *   padding 8/12, fontSize 11, fontWeight 600, color ink
 *   left:  "Now Playing"
 *   right: shuffle + repeat + battery (gap: 6)
 */
static void np_status_bar(int battery_pct) {
    atlas_render(&NUNITO_BOLD_11, 12, 17, "Now Playing", COL_INK);

    /* Right side: shuffle (~10 wide) + repeat (~10 wide) + battery
     * (32 wide), each separated by ~6 px gap, anchored to right edge
     * with 12 px outer padding. Layout right-to-left: */
    int x = LCD_WIDTH - 12;
    /* Battery */
    x -= 32;
    chrome_battery(x, 8, battery_pct, COL_INK);
    x -= 6;
    /* Repeat icon */
    x -= 10;
    chrome_repeat(x, 8, COL_INK);
    x -= 6;
    /* Shuffle icon */
    x -= 10;
    chrome_shuffle(x, 8, COL_INK);
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

/*
 * Star rating row — N filled stars + (5-N) outlined stars, 8 px each,
 * 1 px gap between. Returns the rightmost x for chained layout.
 */
static int draw_stars(int x, int y, int filled_count) {
    if (filled_count < 0) filled_count = 0;
    if (filled_count > 5) filled_count = 5;
    for (int i = 0; i < 5; i++) {
        chrome_star(x, y, i < filled_count,
                    i < filled_count ? COL_INK : COL_STAR_MUTED);
        x += 9;     /* 8 px star + 1 px gap */
    }
    return x;
}

/*
 * Format badge — "FLAC" in 1px-outlined rounded rect, then a space
 * and "44 kHz" in lighter ink. Returns the rightmost x.
 */
static int draw_format_badge(int x, int y_baseline, const char *label,
                             const char *detail) {
    /* Outlined pill around `label`. We use Bold-9 (closest atlas to
     * the design's 8px Bold-800). Add 4px horizontal padding inside
     * the box. */
    int label_w = atlas_text_width(&NUNITO_BOLD_9, label);
    int box_w   = label_w + 8;       /* 4 px padding each side */
    int box_h   = 12;                /* roughly font ascent + 2px */
    int box_x   = x;
    int box_y   = y_baseline - 9;    /* center vertically against the row */
    chrome_outline_rect(box_x, box_y, box_w, box_h, 2, COL_INK);
    atlas_render(&NUNITO_BOLD_9, box_x + 4, y_baseline - 1, label, COL_INK);

    /* "44 kHz" detail to the right of the box, gap 4 px. */
    int detail_x = box_x + box_w + 4;
    atlas_render(&NUNITO_REGULAR_9, detail_x, y_baseline - 1, detail, COL_INK_MUTED);
    int detail_w = atlas_text_width(&NUNITO_REGULAR_9, detail);
    return detail_x + detail_w;
}

/* ---------- Draw --------------------------------------------------- */

void now_playing_draw(const now_playing_t *np, const audio_engine_t *engine) {
    lcd_fill(COL_BG);

    /* Battery is stubbed at 87% until the power HAL lands. */
    np_status_bar(87);

    /*
     * Content area: padding 18 px sides; art on the left at 84x84,
     * text stack on the right starting at art_x + art_w + 14.
     */
    int pad_x  = 18;
    int art_w  = 84, art_h = 84;
    int art_x  = pad_x;
    int art_y  = 30;
    int text_x = art_x + art_w + 14;   /* 116 */

    /* Album art placeholder. */
    chrome_diagonal_stripes(art_x, art_y, art_w, art_h,
                            6, 4, COL_STRIPE_A, COL_STRIPE_B);
    chrome_fill_rect(art_x + 1, art_y + 1, art_w - 2, 1,
                     lcd_rgb(0xA0, 0x88, 0x70));

    /*
     * Text stack:
     *   y=42  "TRACK 1 OF 1"  — Bold-9 muted
     *   y=64  Title           — Bold-17 ink
     *   y=80  Artist          — Regular-13 ink-deep
     *   y=94  Album           — Regular-11 ink-muted
     *   y=108 Stars + Badge row
     */
    if (np->loaded) {
        atlas_render(&NUNITO_BOLD_9,     text_x, 42, "TRACK 1 OF 1",
                     COL_INK_MUTED);
        atlas_render(&NUNITO_BOLD_17,    text_x, 64, np->title,
                     COL_INK);
        atlas_render(&NUNITO_REGULAR_13, text_x, 80, np->artist,
                     COL_INK_DEEP);
        atlas_render(&NUNITO_REGULAR_11, text_x, 94, np->album,
                     COL_INK_MUTED);

        /* Stars + format badge in a row. */
        int sx = draw_stars(text_x, 102, np->stars);
        sx += 6;        /* gap */
        draw_format_badge(sx, 110, np->format, np->format_detail);
    }

    /*
     * "Up next" mini-row at y=192 (per design `bottom: 42, left: 18,
     * right: 18`). Label + thin separator + next-track text.
     */
    if (np->loaded && np->up_next[0]) {
        int up_y = 192;
        atlas_render(&NUNITO_BOLD_9, pad_x, up_y, "UP NEXT", COL_INK_MUTED);
        int sep_x = pad_x + atlas_text_width(&NUNITO_BOLD_9, "UP NEXT") + 6;
        chrome_fill_rect(sep_x, up_y - 7, 1, 8, COL_TRACK_FAINT);
        atlas_render(&NUNITO_REGULAR_11, sep_x + 6, up_y, np->up_next,
                     COL_INK_DEEP);
    }

    /* ---- Progress bar + time labels (bottom band). ---- */

    uint32_t played   = (uint32_t)engine->read_idx;
    uint32_t total    = np->total_frames;
    uint32_t played_s = (np->sample_rate > 0) ? played / np->sample_rate : 0;
    uint32_t remain_s = (total > played && np->sample_rate > 0)
                        ? (total - played) / np->sample_rate : 0;

    int bar_x = pad_x, bar_y = 226;
    int bar_w = LCD_WIDTH - 2 * pad_x;
    int bar_h = 3;
    chrome_fill_rect(bar_x, bar_y, bar_w, bar_h, COL_TRACK_FAINT);
    if (total > 0 && played > 0) {
        if (played > total) played = total;
        int fill_w = (int)((uint64_t)played * (uint64_t)bar_w / (uint64_t)total);
        chrome_fill_rect(bar_x, bar_y, fill_w, bar_h, COL_INK);
    }

    char left[16], right[16];
    format_time(played_s, left, sizeof(left));
    if (total > 0) {
        char tmp[16];
        format_time(remain_s, tmp, sizeof(tmp));
        snprintf(right, sizeof(right), "-%s", tmp);
    } else {
        snprintf(right, sizeof(right), "--:--");
    }
    int label_y = 219;
    atlas_render(&NUNITO_BOLD_9, bar_x, label_y, left, COL_INK_DEEP);
    int rw = atlas_text_width(&NUNITO_BOLD_9, right);
    atlas_render(&NUNITO_BOLD_9, bar_x + bar_w - rw, label_y, right, COL_INK_DEEP);
}
