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
#include "../../codecs/stb_image/image.h"
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
    np->page  = NP_PAGE_DEFAULT;

    np->total_frames = (uint32_t)engine->decoder.total_frames;
    np->sample_rate  = engine->sample_rate;
    np->loaded       = true;
    np->art_loaded   = false;   /* caller will set art via now_playing_set_art_jpeg */
}

int now_playing_set_art_jpeg(now_playing_t *np,
                             const void *jpeg_bytes, size_t jpeg_len) {
    if (!np) return -1;
    if (!jpeg_bytes || jpeg_len == 0) {
        np->art_loaded = false;
        return -1;
    }
    int rc = image_jpeg_decode_rgb565(jpeg_bytes, jpeg_len,
                                      NP_ART_W, NP_ART_H,
                                      np->art_pixels);
    np->art_loaded = (rc == 0);
    return rc;
}

void now_playing_advance_page(now_playing_t *np) {
    np->page = (np->page + 1) % NP_PAGE_COUNT;
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

/* ---------- Draw: dispatch on page -------------------------------- */

static void draw_default(const now_playing_t *np, const audio_engine_t *engine);
static void draw_big_art(const now_playing_t *np, const audio_engine_t *engine);
static void draw_peak_meter(const now_playing_t *np, const audio_engine_t *engine);
static void draw_track_info(const now_playing_t *np, const audio_engine_t *engine);

void now_playing_draw(const now_playing_t *np, const audio_engine_t *engine) {
    switch (np->page) {
        case NP_PAGE_BIG_ART:    draw_big_art(np, engine);    break;
        case NP_PAGE_PEAK_METER: draw_peak_meter(np, engine); break;
        case NP_PAGE_TRACK_INFO: draw_track_info(np, engine); break;
        case NP_PAGE_DEFAULT:
        default:                 draw_default(np, engine);    break;
    }
}

/* ---------- Page 0: default --------------------------------------- */

static void draw_default(const now_playing_t *np, const audio_engine_t *engine) {
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

    /* Album art: real if decoded, diagonal-stripe placeholder if not. */
    if (np->art_loaded) {
        /* Direct framebuffer blit — art_pixels is already in RGB565
         * matching lcd_pixel_t. NP_ART_W/H matches the layout. */
        lcd_pixel_t *fb = lcd_framebuffer();
        for (int y = 0; y < art_h; y++) {
            for (int x = 0; x < art_w; x++) {
                fb[(art_y + y) * LCD_WIDTH + (art_x + x)] =
                    np->art_pixels[y * NP_ART_W + x];
            }
        }
    } else {
        chrome_diagonal_stripes(art_x, art_y, art_w, art_h,
                                6, 4, COL_STRIPE_A, COL_STRIPE_B);
        chrome_fill_rect(art_x + 1, art_y + 1, art_w - 2, 1,
                         lcd_rgb(0xA0, 0x88, 0x70));
    }

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

/* ---------- Page 1: big art on dark backdrop --------------------- */

#define COL_DARK_BG       lcd_rgb(0x0E, 0x0D, 0x0C)
#define COL_DARK_TEXT     lcd_rgb(0xE8, 0xE4, 0xDD)
#define COL_DARK_MUTED    lcd_rgb(0xA8, 0x9E, 0x92)
/* Stripes for the big art — same hue but slightly darker than page 0. */
#define COL_BIG_STRIPE_A  lcd_rgb(0xC8, 0xB2, 0x9F)
#define COL_BIG_STRIPE_B  lcd_rgb(0xB6, 0xA0, 0x8C)

static void draw_big_art(const now_playing_t *np, const audio_engine_t *engine) {
    (void)engine;

    /* Dark backdrop. */
    chrome_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COL_DARK_BG);

    /*
     * 180x180 art centered, per design — ((320-180)/2, (240-180)/2) =
     * (70, 30). Slightly different stripe colors so the art still
     * reads as warm-tan against the near-black bg.
     */
    int art_w = 180, art_h = 180;
    int art_x = (LCD_WIDTH  - art_w) / 2;
    int art_y = (LCD_HEIGHT - art_h) / 2;
    chrome_diagonal_stripes(art_x, art_y, art_w, art_h,
                            10, 6, COL_BIG_STRIPE_A, COL_BIG_STRIPE_B);

    /*
     * Top status row: "1 OF 3" left, battery right, both in cream
     * (per design line 318+). Use Bold-9 for the page label.
     */
    atlas_render(&NUNITO_BOLD_9, 10, 14, "1 OF 3", COL_DARK_TEXT);
    int bat_x = LCD_WIDTH - 10 - 31;
    chrome_battery(bat_x, 5, 87, COL_DARK_TEXT);

    /*
     * Bottom title gradient: design uses an alpha gradient. We
     * approximate with a solid 60%-dark band — enough for legibility,
     * cheaper than a true vertical-alpha gradient. Title + artist on top.
     */
    int band_h = 56;
    chrome_fill_rect(0, LCD_HEIGHT - band_h, LCD_WIDTH, band_h, COL_DARK_BG);
    /* The band overlaps the art; we just overwrite. The "soft fade"
     * is faked by drawing the art first; in a real gradient pass we
     * could alpha-blend. */
    if (np->loaded) {
        atlas_render(&NUNITO_BOLD_13,    14, 218, np->title, COL_DARK_TEXT);
        char sub[NP_TITLE_MAX + NP_ARTIST_MAX + 4];
        snprintf(sub, sizeof(sub), "%s  %s", np->artist, np->album);
        atlas_render(&NUNITO_REGULAR_11, 14, 232, sub, COL_DARK_MUTED);
    }
}

/* ---------- Page 2: peak meter ----------------------------------- */

#define PEAK_SEGS       24
#define PEAK_SEG_W      28
#define PEAK_SEG_H      4
#define PEAK_SEG_GAP    2

#define COL_PEAK_NORMAL lcd_rgb(0x1A, 0x17, 0x14)
#define COL_PEAK_AMBER  lcd_rgb(0xC0, 0x8C, 0x2A)
#define COL_PEAK_RED    lcd_rgb(0xC4, 0x50, 0x2A)
#define COL_PEAK_DIM    lcd_rgb(0xE6, 0xE2, 0xDB)   /* 8% ink on cream */

/* Synthesize a "level" 0..1 for a channel from engine state +
 * time, so the meter looks alive even though we don't yet pull
 * real DSP peak data. */
static float synth_level(const audio_engine_t *engine, int channel) {
    /* Base on read_idx so it tracks playback; phase-shift the right
     * channel so the two columns don't move identically. */
    uint32_t r = (uint32_t)engine->read_idx;
    uint32_t t = clock_ms();
    float phase = (float)((r / 100u + t / 50u + channel * 47u) % 100u) / 100.0f;
    /* Bias toward 0.55..0.85 so the meter looks active. */
    float level = 0.55f + 0.30f * (phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f);
    return level;
}

static void draw_meter_column(int cx, int top_y, float level,
                              const char *label) {
    /* Column label on top, in Bold-9 muted. */
    int lw = atlas_text_width(&NUNITO_BOLD_9, label);
    atlas_render(&NUNITO_BOLD_9, cx - lw / 2, top_y - 2, label, COL_INK_DEEP);

    /* 24 segments stacked top-down. Top of meter = top_y; each
     * segment is PEAK_SEG_H tall + PEAK_SEG_GAP px gap. Highest
     * segments are the loudest; lit if ratio <= level. */
    for (int i = 0; i < PEAK_SEGS; i++) {
        float ratio = (float)(PEAK_SEGS - i) / (float)PEAK_SEGS;
        bool lit = ratio <= level;
        lcd_pixel_t color =
            ratio > 0.85f ? COL_PEAK_RED   :
            ratio > 0.65f ? COL_PEAK_AMBER :
                            COL_PEAK_NORMAL;
        if (!lit) color = COL_PEAK_DIM;
        int seg_x = cx - PEAK_SEG_W / 2;
        int seg_y = top_y + 4 + i * (PEAK_SEG_H + PEAK_SEG_GAP);
        chrome_fill_rect(seg_x, seg_y, PEAK_SEG_W, PEAK_SEG_H, color);
    }

    /* dB label below. Approximate 20 * log10(level) without
     * pulling in <math.h> at this hot path — synthesize it from
     * a small lookup of the synthesized levels. */
    int db = -1;
    if      (level > 0.95f) db = 0;
    else if (level > 0.85f) db = -1;
    else if (level > 0.75f) db = -2;
    else if (level > 0.65f) db = -4;
    else if (level > 0.55f) db = -6;
    else                    db = -10;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d dB", db);
    int dw = atlas_text_width(&NUNITO_BOLD_9, buf);
    int db_y = top_y + 4 + PEAK_SEGS * (PEAK_SEG_H + PEAK_SEG_GAP) + 12;
    atlas_render(&NUNITO_BOLD_9, cx - dw / 2, db_y, buf, COL_INK_MUTED);
}

static void draw_peak_meter(const now_playing_t *np, const audio_engine_t *engine) {
    lcd_fill(COL_BG);

    /* Header: title left, "PAGE 2 OF 3" right, 1 px border below. */
    atlas_render(&NUNITO_BOLD_11, 12, 16, np->title, COL_INK);
    const char *page_label = "PAGE 2 OF 3";
    int pw = atlas_text_width(&NUNITO_BOLD_9, page_label);
    atlas_render(&NUNITO_BOLD_9, LCD_WIDTH - 12 - pw, 16, page_label, COL_INK_MUTED);
    chrome_fill_rect(0, 22, LCD_WIDTH, 1, lcd_rgb(0xE2, 0xDE, 0xDA));

    /* Two channels, centered with 22 px gap. */
    int meter_top = 28;
    int gap_between = 32;
    int total_w = PEAK_SEG_W * 2 + gap_between;
    int left_cx = (LCD_WIDTH - total_w) / 2 + PEAK_SEG_W / 2;
    int right_cx = left_cx + PEAK_SEG_W + gap_between;

    float L = synth_level(engine, 0);
    float R = synth_level(engine, 1);

    draw_meter_column(left_cx,  meter_top, L, "L");
    draw_meter_column(right_cx, meter_top, R, "R");

    /* Bottom labels: "PRE-AMP 0DB" left, "M:SS / M:SS" right. */
    uint32_t played = (uint32_t)engine->read_idx;
    uint32_t total  = np->total_frames;
    uint32_t played_s = (np->sample_rate > 0) ? played / np->sample_rate : 0;
    uint32_t total_s  = (total > 0 && np->sample_rate > 0)
                        ? total / np->sample_rate : 0;
    char left_buf[16], right_buf[24];
    snprintf(left_buf, sizeof(left_buf), "PRE-AMP 0DB");
    char a[16], b[16];
    format_time(played_s, a, sizeof(a));
    format_time(total_s,  b, sizeof(b));
    snprintf(right_buf, sizeof(right_buf), "%s / %s", a, b);
    atlas_render(&NUNITO_BOLD_9, 14, 232, left_buf, COL_INK_DEEP);
    int rw = atlas_text_width(&NUNITO_BOLD_9, right_buf);
    atlas_render(&NUNITO_BOLD_9, LCD_WIDTH - 14 - rw, 232, right_buf, COL_INK_DEEP);
}

/* ---------- Page 3: track info ----------------------------------- */

static void draw_track_info(const now_playing_t *np, const audio_engine_t *engine) {
    (void)engine;
    lcd_fill(COL_BG);

    /* Header: "Track Info" left, "PAGE 3 OF 3" right, border below. */
    atlas_render(&NUNITO_BOLD_11, 12, 16, "Track Info", COL_INK);
    const char *page_label = "PAGE 3 OF 3";
    int pw = atlas_text_width(&NUNITO_BOLD_9, page_label);
    atlas_render(&NUNITO_BOLD_9, LCD_WIDTH - 12 - pw, 16, page_label, COL_INK_MUTED);
    chrome_fill_rect(0, 22, LCD_WIDTH, 1, lcd_rgb(0xE2, 0xDE, 0xDA));

    /*
     * Key-value rows per design (line 388+). Keys uppercase tracking
     * Bold-9 muted at 78 px wide; values Regular-11 ink. ~11 rows
     * total but we only have a few real pieces of data — pad with the
     * stuff we do know for the FLAC fixture.
     */
    char track[16];
    snprintf(track, sizeof(track), "1 of 1");

    char length[16];
    if (np->total_frames > 0 && np->sample_rate > 0) {
        uint32_t total_s = np->total_frames / np->sample_rate;
        format_time(total_s, length, sizeof(length));
    } else {
        snprintf(length, sizeof(length), "--:--");
    }

    char rate[16];
    snprintf(rate, sizeof(rate), "%u kHz", np->sample_rate / 1000);

    char format_full[NP_FORMAT_MAX + NP_FORMAT_MAX + 4];
    snprintf(format_full, sizeof(format_full), "%s · %s",
             np->format, np->format_detail);

    static const char *const KEYS[] = {
        "TITLE", "ARTIST", "ALBUM", "TRACK", "FORMAT",
        "SAMPLE RATE", "LENGTH", "PATH",
    };
    const char *vals[] = {
        np->title, np->artist, np->album, track, format_full,
        rate, length,
        "tests/codec-vectors/sine_440hz_1s_44k_s16_stereo.flac",
    };
    int n = (int)(sizeof(KEYS) / sizeof(KEYS[0]));

    int row_top = 32;
    int row_h   = 16;
    int key_x   = 14;
    int val_x   = 14 + 86;     /* 78 + 8 px gap, matches design's 78 px column */

    for (int i = 0; i < n; i++) {
        int baseline = row_top + i * row_h + 11;
        atlas_render(&NUNITO_BOLD_9, key_x, baseline, KEYS[i], COL_INK_MUTED);
        atlas_render(&NUNITO_REGULAR_11, val_x, baseline, vals[i], COL_INK);
    }
}
