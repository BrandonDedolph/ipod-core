/***************************************************************************
 *  Cabinet — custom UI plugin for Linen theme.
 *  Replaces Rockbox's hardcoded main menu with the design's curated list.
 *
 *  Iter 2: Main → Music → Artists → Albums (filtered by artist) →
 *          Songs (filtered by album) → tap to play (hands off to WPS).
 ***************************************************************************/
#include "plugin.h"

/* ===== Linen palette ===== */
#define COL_SURFACE  LCD_RGBPACK(0xF4, 0xF1, 0xEC)
#define COL_INK      LCD_RGBPACK(0x1A, 0x17, 0x14)
#define COL_INK_AA   LCD_RGBPACK(0x87, 0x84, 0x80)
#define COL_MUTED    LCD_RGBPACK(0x9A, 0x8E, 0x80)
#define COL_DEEP     LCD_RGBPACK(0x5A, 0x50, 0x48)
#define COL_BORDER   LCD_RGBPACK(0xE2, 0xE0, 0xDB)

/* ===== Layout ===== */
#define STATUS_H     16
#define HEADER_TOP   STATUS_H
#define HEADER_H     22
#define UNDERLINE_Y  (HEADER_TOP + HEADER_H - 1)
#define LIST_TOP     (UNDERLINE_Y + 5)
#define ROW_H        24
#define ROW_PAD_X    8
#define LIST_BOTTOM  (LCD_HEIGHT - 4)
#define VISIBLE_ROWS ((LIST_BOTTOM - LIST_TOP) / ROW_H)

/* ===== Battery icon (drawn directly with primitives) ===== */
static void draw_battery_glyph(int x, int y, int level_pct, bool charging)
{
    /* 18x9 outline + 4-step fill, mirroring the design's small battery
     * (showPct=false form). Caller sets fg before calling. */
    rb->lcd_drawrect(x, y, 14, 9);
    rb->lcd_fillrect(x + 14, y + 2, 2, 5);
    int fill_w = (level_pct * 11) / 100;
    if (fill_w > 0) rb->lcd_fillrect(x + 2, y + 2, fill_w, 5);
    if (charging) {
        /* Tiny lightning bolt overlaid on the fill. */
        rb->lcd_drawpixel(x + 7, y + 2);
        rb->lcd_drawpixel(x + 6, y + 3);
        rb->lcd_drawpixel(x + 7, y + 3);
        rb->lcd_drawpixel(x + 8, y + 3);
        rb->lcd_drawpixel(x + 7, y + 4);
        rb->lcd_drawpixel(x + 6, y + 5);
        rb->lcd_drawpixel(x + 7, y + 5);
    }
}

/* ===== Fonts ===== */
static int font_main = -1, font_status = -1, font_title = -1;

/* Shuffle/repeat bitmap cache (lazy-loaded from theme dir).
 * shuffle.bmp = 12×18 (2 cells of 12×9)
 * repeat.bmp  = 12×27 (3 cells of 12×9) */
#define ICON_W 12
#define ICON_H 9
static fb_data shuffle_pix[ICON_W * ICON_H * 2];
static fb_data repeat_pix[ICON_W * ICON_H * 3];
static struct bitmap shuffle_bm = { 0 };
static struct bitmap repeat_bm  = { 0 };
static bool shuffle_ok = false, repeat_ok = false;

static void load_status_icons(void) {
    shuffle_bm.data = (unsigned char *)shuffle_pix;
    shuffle_bm.width = ICON_W; shuffle_bm.height = ICON_H * 2;
    int r = rb->read_bmp_file("/.rockbox/wps/linen/shuffle.bmp",
                              &shuffle_bm, sizeof(shuffle_pix),
                              FORMAT_NATIVE, NULL);
    shuffle_ok = (r > 0);

    repeat_bm.data = (unsigned char *)repeat_pix;
    repeat_bm.width = ICON_W; repeat_bm.height = ICON_H * 3;
    r = rb->read_bmp_file("/.rockbox/wps/linen/repeat.bmp",
                          &repeat_bm, sizeof(repeat_pix),
                          FORMAT_NATIVE, NULL);
    repeat_ok = (r > 0);
}

static void draw_icon_cell(int x, int y, struct bitmap *bm, bool ok, int cell) {
    if (!ok) return;
    rb->lcd_bitmap_part((const fb_data *)bm->data, 0, cell * ICON_H, ICON_W,
                        x, y, ICON_W, ICON_H);
}

/* Truncate-with-ellipsis variant of lcd_putsxy. */
static void putsxy_ellipsis(int x, int y, const char *s, int max_w) {
    int w = 0, h = 0;
    rb->lcd_getstringsize((const unsigned char *)s, &w, &h);
    if (w <= max_w) { rb->lcd_putsxy(x, y, s); return; }
    char buf[128];
    rb->strlcpy(buf, s, sizeof(buf));
    int len = rb->strlen(buf);
    while (len > 0) {
        buf[len] = '\0';
        char tmp[132];
        rb->snprintf(tmp, sizeof(tmp), "%s...", buf);
        rb->lcd_getstringsize((const unsigned char *)tmp, &w, &h);
        if (w <= max_w) { rb->lcd_putsxy(x, y, tmp); return; }
        len--;
    }
    rb->lcd_putsxy(x, y, "...");
}

/* Marquee-scrolled draw of a string clipped to (x, y, max_w) using a
 * viewport. tick advances the offset; for strings shorter than max_w it
 * just renders normally. Caller provides fg/bg via current state. */
static void putsxy_marquee(int x, int y, const char *s, int max_w,
                           long tick, unsigned fg, unsigned bg)
{
    int w = 0, h = 0;
    rb->lcd_getstringsize((const unsigned char *)s, &w, &h);
    if (w <= max_w) { rb->lcd_putsxy(x, y, s); return; }

    /* Build "<text>    <text>" so it loops smoothly. */
    char loop[256];
    rb->snprintf(loop, sizeof(loop), "%s    %s", s, s);
    int loop_w = 0, loop_h = 0;
    rb->lcd_getstringsize((const unsigned char *)loop, &loop_w, &loop_h);
    int seg_w = (loop_w + 1) / 2;   /* one full "<text>    " segment */
    int offset = (int)((tick / 2) % seg_w);

    /* Clip via viewport. */
    static struct viewport vp;
    rb->memset(&vp, 0, sizeof(vp));
    vp.x = x; vp.y = y;
    vp.width = max_w; vp.height = h;
    vp.fg_pattern = fg;
    vp.bg_pattern = bg;
    struct viewport *prev = rb->lcd_set_viewport(&vp);
    rb->lcd_putsxy(-offset, 0, loop);
    rb->lcd_set_viewport(prev);
}

static void load_fonts(void) {
    font_main   = rb->font_load("/.rockbox/fonts/nunito-13.fnt");
    font_status = rb->font_load("/.rockbox/fonts/nunito-9.fnt");
    font_title  = rb->font_load("/.rockbox/fonts/nunito-13-bold.fnt");
}
static void unload_fonts(void) {
    if (font_main   >= 0) rb->font_unload(font_main);
    if (font_status >= 0) rb->font_unload(font_status);
    if (font_title  >= 0) rb->font_unload(font_title);
}
static int safe_font(int id) { return id >= 0 ? id : FONT_UI; }

/* ===== Frame stack ===== */
enum frame_kind {
    F_MAIN, F_MUSIC, F_ARTISTS, F_ALBUMS, F_SONGS, F_GENRES, F_COMPOSERS,
    F_PLAYLISTS, F_SETTINGS, F_ABOUT,
    F_PLAYING,
};
struct frame {
    enum frame_kind kind;
    int sel;
    int scroll;
    /* Tagcache filter seeks inherited from parent frames. -1 = no filter. */
    int parent_artist_seek;
    int parent_album_seek;
    int parent_genre_seek;
    int parent_composer_seek;
    /* Cached title shown in this frame's header, e.g. the artist or album name. */
    char header_title[96];
};
#define MAX_FRAMES 8
static struct frame stack[MAX_FRAMES];
static int depth = 0;

static struct frame *top(void) { return &stack[depth - 1]; }
static struct frame *push(enum frame_kind k) {
    if (depth >= MAX_FRAMES) return NULL;
    struct frame *f = &stack[depth];
    rb->memset(f, 0, sizeof(*f));
    f->kind = k;
    f->parent_artist_seek = -1;
    f->parent_album_seek = -1;
    f->parent_genre_seek = -1;
    f->parent_composer_seek = -1;
    /* Inherit parent's seek filters so child frames don't have to re-thread them. */
    if (depth > 0) {
        f->parent_artist_seek    = stack[depth - 1].parent_artist_seek;
        f->parent_album_seek     = stack[depth - 1].parent_album_seek;
        f->parent_genre_seek     = stack[depth - 1].parent_genre_seek;
        f->parent_composer_seek  = stack[depth - 1].parent_composer_seek;
    }
    depth++;
    return f;
}
static void pop(void) { if (depth > 1) depth--; }

/* ===== List storage ===== */
#define MAX_LIST 256
#define LIST_LABEL_LEN 96
#define LIST_PATH_LEN 256
static char list_buf[MAX_LIST][LIST_LABEL_LEN];
static char list_path[MAX_LIST][LIST_PATH_LEN];   /* only populated for songs */
static int  list_seek[MAX_LIST];                  /* tagcache result_seek per row */
static int  list_track_num[MAX_LIST];             /* track number for songs */
static int  list_length_ms[MAX_LIST];             /* track length for songs */
static int list_count = 0;

/* Static menus */
static const char *MAIN_ITEMS[] = {
    "Music", "Playlists", "Podcasts", "Audiobooks", "Settings", "Now Playing"
};
#define MAIN_LEN ((int)(sizeof(MAIN_ITEMS) / sizeof(MAIN_ITEMS[0])))

static const char *MUSIC_ITEMS[] = {
    "Artists", "Albums", "Songs", "Genres", "Composers"
};
#define MUSIC_LEN ((int)(sizeof(MUSIC_ITEMS) / sizeof(MUSIC_ITEMS[0])))

/* ===== Tagcache loaders ===== */
static char uniqbuf[8192];

static void load_artists(void) {
    list_count = 0;
    struct tagcache_search tcs;
    if (!rb->tagcache_search(&tcs, tag_artist)) return;
    rb->tagcache_search_set_uniqbuf(&tcs, uniqbuf, sizeof(uniqbuf));
    char buf[LIST_LABEL_LEN];
    while (list_count < MAX_LIST &&
           rb->tagcache_get_next(&tcs, buf, sizeof(buf))) {
        if (buf[0] == '\0') continue;
        rb->strlcpy(list_buf[list_count], buf, LIST_LABEL_LEN);
        list_seek[list_count] = tcs.result_seek;
        list_count++;
    }
    rb->tagcache_search_finish(&tcs);
}

/* Albums for a given artist seek. */
static void load_albums_for_artist(int artist_seek) {
    list_count = 0;
    struct tagcache_search tcs;
    if (!rb->tagcache_search(&tcs, tag_album)) return;
    rb->tagcache_search_set_uniqbuf(&tcs, uniqbuf, sizeof(uniqbuf));
    if (artist_seek >= 0)
        rb->tagcache_search_add_filter(&tcs, tag_artist, artist_seek);

    char buf[LIST_LABEL_LEN];
    while (list_count < MAX_LIST &&
           rb->tagcache_get_next(&tcs, buf, sizeof(buf))) {
        if (buf[0] == '\0') continue;
        rb->strlcpy(list_buf[list_count], buf, LIST_LABEL_LEN);
        list_seek[list_count] = tcs.result_seek;
        list_track_num[list_count] =
            (int)rb->tagcache_get_numeric(&tcs, tag_year);
        list_path[list_count][0] = '\0';
        list_count++;
    }
    rb->tagcache_search_finish(&tcs);
}

/* Fetch one sample track filename for the given (artist, album) filters.
 * We need this because tagcache_retrieve(tag_filename) on a uniqbuf-
 * iterated tag_album entry doesn't return the related track's path. */
static bool fetch_sample_filename(int artist_seek, int album_seek,
                                  char *out, int out_size) {
    struct tagcache_search tcs;
    if (!rb->tagcache_search(&tcs, tag_filename)) return false;
    if (artist_seek >= 0)
        rb->tagcache_search_add_filter(&tcs, tag_artist, artist_seek);
    if (album_seek >= 0)
        rb->tagcache_search_add_filter(&tcs, tag_album, album_seek);
    bool got = false;
    if (rb->tagcache_get_next(&tcs, out, out_size) && out[0])
        got = true;
    rb->tagcache_search_finish(&tcs);
    return got;
}

/* FNV-ish 32-bit hash → hue 0..359 for procedural album chip color. */
static int album_hue(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return (int)(h % 360);
}

/* Convert HSV (h 0-359, s 0-100, v 0-100) → RGB565. */
static unsigned hsv_to_rgb565(int h, int s, int v) {
    int c = (v * s) / 100;
    int hp = h / 60;
    int hh = h % 60;
    int xv = (c * (60 - (hh > 30 ? hh - 30 : 30 - hh) * 2)) / 60;
    int r1, g1, b1;
    switch (hp) {
        case 0:  r1 = c;  g1 = xv; b1 = 0;  break;
        case 1:  r1 = xv; g1 = c;  b1 = 0;  break;
        case 2:  r1 = 0;  g1 = c;  b1 = xv; break;
        case 3:  r1 = 0;  g1 = xv; b1 = c;  break;
        case 4:  r1 = xv; g1 = 0;  b1 = c;  break;
        default: r1 = c;  g1 = 0;  b1 = xv; break;
    }
    int m = v - c;
    int r = (r1 + m) * 255 / 100;
    int g = (g1 + m) * 255 / 100;
    int b = (b1 + m) * 255 / 100;
    if (r < 0)   r = 0;
    if (r > 255) r = 255;
    if (g < 0)   g = 0;
    if (g > 255) g = 255;
    if (b < 0)   b = 0;
    if (b > 255) b = 255;
    return LCD_RGBPACK(r, g, b);
}

/* Album-detail thumbnail cache (56×56, separate from the 84×84 NP art). */
#define THUMB_W 56
#define THUMB_H 56
#define THUMB_BUF_BYTES (64 * 1024)
static unsigned char thumb_buf[THUMB_BUF_BYTES] __attribute__((aligned(4)));
static char    thumb_cache_path[MAX_PATH] = "";
static bool    thumb_cache_valid = false;
static struct bitmap thumb_bm;

/* Tiny per-row album-list thumbnail (22×22). Only the currently-selected
 * row holds a real thumb; others use the procedural color chip. */
#define ROWTHUMB_W 22
#define ROWTHUMB_H 22
#define ROWTHUMB_BUF_BYTES (32 * 1024)
static unsigned char rowthumb_buf[ROWTHUMB_BUF_BYTES] __attribute__((aligned(4)));
static char    rowthumb_path[MAX_PATH] = "";
static bool    rowthumb_valid = false;
static struct bitmap rowthumb_bm;

static void update_rowthumb_for_path(const char *path)
{
    if (!path || path[0] == '\0') {
        rowthumb_valid = false;
        rowthumb_path[0] = '\0';
        return;
    }
    if (rb->strcmp(rowthumb_path, path) == 0)
        return;
    rb->strlcpy(rowthumb_path, path, sizeof(rowthumb_path));
    rowthumb_valid = false;

    rowthumb_bm.data = rowthumb_buf;
    rowthumb_bm.width = ROWTHUMB_W;
    rowthumb_bm.height = ROWTHUMB_H;

    int fd = rb->open(path, O_RDONLY);
    if (fd < 0) return;
    static struct mp3entry id3;
    rb->memset(&id3, 0, sizeof(id3));
    int format = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT;
    int ret = -1;
    if (rb->get_metadata(&id3, fd, path) &&
        id3.has_embedded_albumart && id3.albumart.size > 0) {
        rb->lseek(fd, id3.albumart.pos, SEEK_SET);
        if (id3.albumart.type == AA_TYPE_JPG)
            ret = rb->read_jpeg_fd(fd, &rowthumb_bm,
                                   ROWTHUMB_BUF_BYTES, format, NULL);
        else if (id3.albumart.type == AA_TYPE_BMP)
            ret = rb->read_bmp_fd(fd, &rowthumb_bm,
                                  ROWTHUMB_BUF_BYTES, format, NULL);
    }
    rb->close(fd);
    rowthumb_valid = (ret > 0);
}

/* Read embedded album art for an arbitrary file path into thumb cache.
 * Uses get_metadata() to find APIC offset; works for any track, not just
 * the currently-playing one. */
static void update_thumb_for_path(const char *path)
{
    if (!path || path[0] == '\0') {
        thumb_cache_valid = false;
        thumb_cache_path[0] = '\0';
        return;
    }
    if (rb->strcmp(thumb_cache_path, path) == 0)
        return;
    rb->strlcpy(thumb_cache_path, path, sizeof(thumb_cache_path));
    thumb_cache_valid = false;

    thumb_bm.data = thumb_buf;
    thumb_bm.width = THUMB_W;
    thumb_bm.height = THUMB_H;
    int format = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT;
    int ret = -1;

    int fd = rb->open(path, O_RDONLY);
    if (fd < 0) return;

    static struct mp3entry id3;
    rb->memset(&id3, 0, sizeof(id3));
    if (rb->get_metadata(&id3, fd, path) &&
        id3.has_embedded_albumart && id3.albumart.size > 0) {
        rb->lseek(fd, id3.albumart.pos, SEEK_SET);
        if (id3.albumart.type == AA_TYPE_JPG)
            ret = rb->read_jpeg_fd(fd, &thumb_bm, THUMB_BUF_BYTES,
                                   format, NULL);
        else if (id3.albumart.type == AA_TYPE_BMP)
            ret = rb->read_bmp_fd(fd, &thumb_bm, THUMB_BUF_BYTES,
                                  format, NULL);
    }
    rb->close(fd);
    thumb_cache_valid = (ret > 0);
}

/* Insertion-sort the current song list by track number. */
static void sort_songs_by_track_num(void) {
    for (int i = 1; i < list_count; i++) {
        int n = list_track_num[i], len = list_length_ms[i];
        char title[LIST_LABEL_LEN], path[LIST_PATH_LEN];
        rb->strlcpy(title, list_buf[i], LIST_LABEL_LEN);
        rb->strlcpy(path, list_path[i], LIST_PATH_LEN);
        int j = i - 1;
        while (j >= 0 && list_track_num[j] > n) {
            list_track_num[j + 1] = list_track_num[j];
            list_length_ms[j + 1] = list_length_ms[j];
            rb->strlcpy(list_buf[j + 1], list_buf[j], LIST_LABEL_LEN);
            rb->strlcpy(list_path[j + 1], list_path[j], LIST_PATH_LEN);
            j--;
        }
        list_track_num[j + 1] = n;
        list_length_ms[j + 1] = len;
        rb->strlcpy(list_buf[j + 1], title, LIST_LABEL_LEN);
        rb->strlcpy(list_path[j + 1], path, LIST_PATH_LEN);
    }
}

/* Songs filtered by any combination of (artist, album, genre, composer). */
static void load_songs_filtered(int artist_seek, int album_seek,
                                int genre_seek, int composer_seek) {
    list_count = 0;
    struct tagcache_search tcs;
    if (!rb->tagcache_search(&tcs, tag_title)) return;
    if (artist_seek >= 0)
        rb->tagcache_search_add_filter(&tcs, tag_artist, artist_seek);
    if (album_seek >= 0)
        rb->tagcache_search_add_filter(&tcs, tag_album, album_seek);
    if (genre_seek >= 0)
        rb->tagcache_search_add_filter(&tcs, tag_genre, genre_seek);
    if (composer_seek >= 0)
        rb->tagcache_search_add_filter(&tcs, tag_composer, composer_seek);

    char buf[LIST_LABEL_LEN];
    while (list_count < MAX_LIST &&
           rb->tagcache_get_next(&tcs, buf, sizeof(buf))) {
        if (buf[0] == '\0') continue;
        rb->strlcpy(list_buf[list_count], buf, LIST_LABEL_LEN);
        rb->tagcache_retrieve(&tcs, tcs.idx_id, tag_filename,
                              list_path[list_count], LIST_PATH_LEN);
        list_track_num[list_count] = (int)rb->tagcache_get_numeric(&tcs, tag_tracknumber);
        list_length_ms[list_count] = (int)rb->tagcache_get_numeric(&tcs, tag_length);
        list_count++;
    }
    rb->tagcache_search_finish(&tcs);
    if (album_seek >= 0)  /* only sort by track # when filtering by album */
        sort_songs_by_track_num();
}

/* Backwards-compatible thin wrapper. */
static void load_songs_for_album(int artist_seek, int album_seek) {
    load_songs_filtered(artist_seek, album_seek, -1, -1);
}

/* Scan a directory for .m3u / .m3u8 playlist files. */
static void load_playlists(void) {
    list_count = 0;
    static const char *dirs[] = { "/Playlists", "/.rockbox/playlists", "/" };
    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0])
                           && list_count < MAX_LIST; d++) {
        DIR *dp = rb->opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *de;
        while ((de = rb->readdir(dp)) && list_count < MAX_LIST) {
            const char *name = de->d_name;
            int len = rb->strlen(name);
            bool is_m3u = (len >= 4 &&
                           rb->strcasecmp(name + len - 4, ".m3u") == 0)
                       || (len >= 5 &&
                           rb->strcasecmp(name + len - 5, ".m3u8") == 0);
            if (!is_m3u) continue;
            /* Trim extension for display. */
            char display[LIST_LABEL_LEN];
            rb->strlcpy(display, name, sizeof(display));
            char *dot = rb->strrchr(display, '.');
            if (dot) *dot = '\0';
            rb->strlcpy(list_buf[list_count], display, LIST_LABEL_LEN);
            /* Store full path in list_path for opening. */
            rb->snprintf(list_path[list_count], LIST_PATH_LEN,
                         "%s/%s", dirs[d], name);
            list_count++;
        }
        rb->closedir(dp);
    }
}

/* Open a playlist file and queue all listed tracks for playback. */
static int play_m3u(const char *path) {
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0) return -1;
    rb->playlist_create(NULL, NULL);
    char ch, line[LIST_PATH_LEN];
    int li = 0;
    int added = 0;
    while (rb->read(fd, &ch, 1) == 1) {
        if (ch == '\n' || ch == '\r') {
            if (li > 0) {
                line[li] = '\0';
                if (line[0] != '#' && line[0] != '\0') {
                    rb->playlist_insert_track(NULL, line,
                                              PLAYLIST_INSERT_LAST,
                                              false, true);
                    added++;
                }
                li = 0;
            }
        } else if (li < (int)sizeof(line) - 1) {
            line[li++] = ch;
        }
    }
    if (li > 0) {
        line[li] = '\0';
        if (line[0] != '#' && line[0] != '\0') {
            rb->playlist_insert_track(NULL, line,
                                      PLAYLIST_INSERT_LAST, false, true);
            added++;
        }
    }
    rb->close(fd);
    if (added == 0) return -1;
    rb->playlist_sync(NULL);
    rb->playlist_start(0, 0, 0);
    return 0;
}

static void load_tag_list(int tag) {
    list_count = 0;
    struct tagcache_search tcs;
    if (!rb->tagcache_search(&tcs, tag)) return;
    rb->tagcache_search_set_uniqbuf(&tcs, uniqbuf, sizeof(uniqbuf));
    char buf[LIST_LABEL_LEN];
    while (list_count < MAX_LIST &&
           rb->tagcache_get_next(&tcs, buf, sizeof(buf))) {
        if (buf[0] == '\0') continue;
        rb->strlcpy(list_buf[list_count], buf, LIST_LABEL_LEN);
        list_seek[list_count] = tcs.result_seek;
        list_count++;
    }
    rb->tagcache_search_finish(&tcs);
}

/* ===== Playback ===== */
static int play_track_list(int start_index)
{
    if (list_count == 0 || start_index < 0 || start_index >= list_count)
        return -1;
    rb->playlist_create(NULL, NULL);
    for (int i = 0; i < list_count; i++) {
        if (list_path[i][0] == '\0') continue;
        rb->playlist_insert_track(NULL, list_path[i],
                                  PLAYLIST_INSERT_LAST, false, true);
    }
    rb->playlist_sync(NULL);
    rb->playlist_start(start_index, 0, 0);
    return 0;
}

/* ===== Now Playing screen ===== */
static void draw_status_strip(void);   /* forward decl */
static void rounded_fillrect(int x, int y, int w, int h,
                             int r, unsigned fg, unsigned bg, unsigned aa);

/* Album-art cache. We only re-decode when the track changes.
 * The buffer holds both the scaled output (max 84×84×2 ≈ 14 KB) and the
 * JPEG decoder's working memory. 128 KB is comfortably more than enough. */
#define ART_W 84
#define ART_H 84
#define ART_BUF_BYTES (128 * 1024)
static unsigned char art_buf[ART_BUF_BYTES] __attribute__((aligned(4)));
#define art_pixels ((fb_data *)art_buf)

static char    art_cache_path[MAX_PATH] = "";
static bool    art_cache_valid = false;
static struct bitmap art_bm;

/* Diagnostic flags exposed for on-screen rendering. */
static int dbg_embed = 0;
static int dbg_type = 0;
static int dbg_size = 0;
static int dbg_ret = 0;

/* Volume overlay state — shown for ~1 s after wheel scroll on NP. */
static long vol_overlay_until = 0;   /* tick (HZ) when overlay should hide */
static int  vol_overlay_pct = 0;

/* NP info-page cycling (Select button on NP). */
static int np_page = 0;   /* 0=default, 1=big art, 2=track info */

/* Hold switch state — for the LOCKED / UNLOCKED flash plates. */
static bool prev_hold = false;
static long lock_overlay_until = 0;   /* tick when LOCKED/UNLOCKED plate hides */
static bool lock_overlay_locked = false;

static void update_album_art(const struct mp3entry *id3)
{
    if (!id3 || id3->path[0] == '\0') {
        art_cache_valid = false;
        art_cache_path[0] = '\0';
        return;
    }
    if (rb->strcmp(art_cache_path, id3->path) == 0)
        return;  /* same track, nothing to do */

    rb->strlcpy(art_cache_path, id3->path, sizeof(art_cache_path));
    art_cache_valid = false;

    art_bm.data = (unsigned char *)art_pixels;
    art_bm.width = ART_W;
    art_bm.height = ART_H;

    int format = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT;
    int ret = -1;
    int debug_path = 0;
    int debug_ret = 0;
    dbg_embed = id3->has_embedded_albumart ? 1 : 0;
    dbg_type = id3->albumart.type;
    dbg_size = id3->albumart.size;

    /* Prefer embedded art (iTunes-style ID3 APIC). */
    if (id3->has_embedded_albumart && id3->albumart.size > 0) {
        debug_path = 1;
        int fd = rb->open(id3->path, O_RDONLY);
        if (fd >= 0) {
            rb->lseek(fd, id3->albumart.pos, SEEK_SET);
            switch (id3->albumart.type) {
            case AA_TYPE_JPG:
                ret = rb->read_jpeg_fd(fd, &art_bm, ART_BUF_BYTES,
                                       format, NULL);
                break;
            case AA_TYPE_BMP:
                ret = rb->read_bmp_fd(fd, &art_bm, ART_BUF_BYTES,
                                      format, NULL);
                break;
            default:
                /* PNG / unknown — no plugin-side decoder */
                ret = -1;
            }
            rb->close(fd);
            debug_ret = ret;
        }
    }

    /* Fall back to sidecar files (cover.jpg, folder.jpg, etc.). */
    if (ret <= 0) {
        char artpath[MAX_PATH];
        if (rb->search_albumart_files(id3, "", artpath, sizeof(artpath))) {
            int len = rb->strlen(artpath);
            if (len >= 4 && (rb->strcasecmp(artpath + len - 4, ".jpg") == 0 ||
                             (len >= 5 &&
                              rb->strcasecmp(artpath + len - 5, ".jpeg") == 0)))
                ret = rb->read_jpeg_file(artpath, &art_bm, ART_BUF_BYTES,
                                         format, NULL);
            else
                ret = rb->read_bmp_file(artpath, &art_bm, ART_BUF_BYTES,
                                        format, NULL);
        }
    }

    art_cache_valid = (ret > 0);
    dbg_ret = ret;
    (void)debug_path; (void)debug_ret;
}

static void fmt_time(char *buf, size_t n, unsigned long ms) {
    unsigned long s = ms / 1000;
    rb->snprintf(buf, n, "%lu:%02lu", s / 60, s % 60);
}

static void draw_albumart_placeholder(int x, int y, int size) {
    /* Striped 135° pattern at ~85% lightness — matches design's AlbumArt
     * placeholder used when no embedded art. */
    unsigned light = LCD_RGBPACK(0xC8, 0xBC, 0xAA);
    unsigned dark  = LCD_RGBPACK(0xBC, 0xAF, 0x9D);
    rb->lcd_set_foreground(light);
    rb->lcd_fillrect(x, y, size, size);
    rb->lcd_set_foreground(dark);
    /* Diagonal stripes 6 px apart. */
    for (int d = -size; d < 2 * size; d += 12) {
        for (int t = 0; t < 6; t++) {
            for (int i = 0; i < size; i++) {
                int px = x + i, py = y + (d + t - i);
                if (py >= y && py < y + size)
                    rb->lcd_drawpixel(px, py);
            }
        }
    }
    /* Inner 1-px frame. */
    rb->lcd_set_foreground(LCD_RGBPACK(0xA0, 0x94, 0x84));
    rb->lcd_drawrect(x + 4, y + 4, size - 8, size - 8);
}

static void draw_np_big_art(void)
{
    rb->lcd_set_background(COL_INK);
    rb->lcd_set_foreground(COL_SURFACE);
    /* True dark backdrop matches design's NowPlayingBigArt component. */
    rb->lcd_set_foreground(LCD_RGBPACK(0x0E, 0x0D, 0x0C));
    rb->lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);

    struct mp3entry *id3 = rb->audio_current_track();
    update_album_art(id3);
    if (art_cache_valid && art_bm.width > 0 && art_bm.height > 0) {
        int ax = (LCD_WIDTH - art_bm.width) / 2;
        int ay = (LCD_HEIGHT - art_bm.height) / 2 - 8;
        rb->lcd_bitmap((const fb_data *)art_bm.data, ax, ay,
                       art_bm.width, art_bm.height);
    }

    /* Bottom title overlay (matches design's gradient strip) */
    rb->lcd_setfont(safe_font(font_title));
    rb->lcd_set_background(LCD_RGBPACK(0x0E, 0x0D, 0x0C));
    rb->lcd_set_foreground(LCD_RGBPACK(0xE8, 0xE4, 0xDD));
    if (id3 && id3->title)
        putsxy_ellipsis(14, LCD_HEIGHT - 32, id3->title, LCD_WIDTH - 28);
    rb->lcd_setfont(safe_font(font_main));
    rb->lcd_set_foreground(LCD_RGBPACK(0xA8, 0x9E, 0x92));
    if (id3 && id3->artist) {
        char sub[128];
        if (id3->album)
            rb->snprintf(sub, sizeof(sub), "%s · %s", id3->artist, id3->album);
        else
            rb->strlcpy(sub, id3->artist, sizeof(sub));
        putsxy_ellipsis(14, LCD_HEIGHT - 16, sub, LCD_WIDTH - 28);
    }

    /* Top status: page indicator */
    rb->lcd_setfont(safe_font(font_status));
    rb->lcd_set_foreground(LCD_RGBPACK(0xA8, 0x9E, 0x92));
    rb->lcd_putsxy(14, 6, "1 OF 3");
    rb->lcd_set_background(COL_SURFACE);
    rb->lcd_update();
}

static void draw_np_track_info(void)
{
    rb->lcd_set_background(COL_SURFACE);
    rb->lcd_set_foreground(COL_INK);
    rb->lcd_clear_display();
    draw_status_strip();

    struct mp3entry *id3 = rb->audio_current_track();
    rb->lcd_setfont(safe_font(font_title));
    rb->lcd_set_foreground(COL_INK);
    rb->lcd_putsxy(14, HEADER_TOP + 4, "Track Info");
    rb->lcd_setfont(safe_font(font_status));
    rb->lcd_set_foreground(COL_MUTED);
    rb->lcd_putsxy(LCD_WIDTH - 60, HEADER_TOP + 7, "PAGE 3 OF 3");
    rb->lcd_set_foreground(COL_BORDER);
    rb->lcd_hline(0, LCD_WIDTH - 1, UNDERLINE_Y);

    /* Key/value rows starting at y=44, ~13 px per row. */
    struct {
        const char *k;
        const char *v;
        char buf[64];
    } rows[10];
    int n = 0;
    if (id3) {
        rows[n].k = "TITLE";  rows[n].v = id3->title  ? id3->title  : ""; n++;
        rows[n].k = "ARTIST"; rows[n].v = id3->artist ? id3->artist : ""; n++;
        rows[n].k = "ALBUM";  rows[n].v = id3->album  ? id3->album  : ""; n++;
        if (id3->year) {
            rows[n].k = "YEAR";
            rb->snprintf(rows[n].buf, sizeof(rows[n].buf), "%d", id3->year);
            rows[n].v = rows[n].buf; n++;
        }
        if (id3->genre_string) {
            rows[n].k = "GENRE"; rows[n].v = id3->genre_string; n++;
        }
        rows[n].k = "BITRATE";
        rb->snprintf(rows[n].buf, sizeof(rows[n].buf), "%d kbps", id3->bitrate);
        rows[n].v = rows[n].buf; n++;
        if (id3->frequency) {
            rows[n].k = "SAMPLE";
            rb->snprintf(rows[n].buf, sizeof(rows[n].buf), "%lu Hz",
                         (unsigned long)id3->frequency);
            rows[n].v = rows[n].buf; n++;
        }
        rows[n].k = "LENGTH";
        if (id3->length > 0) {
            unsigned long s = id3->length / 1000;
            rb->snprintf(rows[n].buf, sizeof(rows[n].buf), "%lu:%02lu",
                         s / 60, s % 60);
            rows[n].v = rows[n].buf;
        } else rows[n].v = "—";
        n++;
        rows[n].k = "PATH"; rows[n].v = id3->path; n++;
    }

    rb->lcd_setfont(safe_font(font_main));
    int y = LIST_TOP - 4;
    for (int i = 0; i < n; i++) {
        rb->lcd_set_foreground(COL_MUTED);
        rb->lcd_putsxy(14, y, rows[i].k);
        rb->lcd_set_foreground(COL_INK);
        putsxy_ellipsis(80, y, rows[i].v, LCD_WIDTH - 80 - 14);
        y += 13;
    }

    rb->lcd_update();
}

static void draw_now_playing(void)
{
    if (np_page == 1) { draw_np_big_art(); return; }
    if (np_page == 2) { draw_np_track_info(); return; }

    rb->lcd_set_background(COL_SURFACE);
    rb->lcd_set_foreground(COL_INK);
    rb->lcd_clear_display();

    draw_status_strip();

    /* "Now Playing" / "Paused" label at left of status row. */
    int astatus = rb->audio_status();
    bool playing = (astatus & AUDIO_STATUS_PLAY) && !(astatus & AUDIO_STATUS_PAUSE);
    rb->lcd_setfont(safe_font(font_status));
    rb->lcd_set_foreground(COL_INK);
    rb->lcd_putsxy(12, 4, playing ? "Now Playing" : "Paused");

    /* Shuffle/Repeat icons just left of battery. Use the 2- and 3-cell
     * bitmap strips loaded at plugin start. */
    int batt_x = LCD_WIDTH - 12 - 16;
    int rep_x = batt_x - ICON_W - 4;
    int shuf_x = rep_x - ICON_W - 4;
    int rmode = rb->global_settings->repeat_mode;
    /* repeat.bmp cells: 0=off (faded), 1=all, 2=one */
    int rep_cell = (rmode == 2) ? 2 : (rmode != 0 ? 1 : 0);
    draw_icon_cell(rep_x, 4, &repeat_bm, repeat_ok, rep_cell);
    /* shuffle.bmp cells: 0=off (faded), 1=on */
    int shuf_cell = rb->global_settings->playlist_shuffle ? 1 : 0;
    draw_icon_cell(shuf_x, 4, &shuffle_bm, shuffle_ok, shuf_cell);

    struct mp3entry *id3 = rb->audio_current_track();
    update_album_art(id3);

    /* Album art at (18, 35) 84x84. */
    if (art_cache_valid && art_bm.width > 0 && art_bm.height > 0) {
        int ax = 18 + (ART_W - art_bm.width) / 2;
        int ay = 35 + (ART_H - art_bm.height) / 2;
        rb->lcd_bitmap((const fb_data *)art_bm.data, ax, ay,
                       art_bm.width, art_bm.height);
    } else {
        draw_albumart_placeholder(18, 35, 84);
    }

    /* "TRACK n OF total" caps at (116, 37). */
    rb->lcd_setfont(safe_font(font_status));
    rb->lcd_set_foreground(COL_MUTED);
    char meta[40];
    int pos = rb->playlist_get_display_index();
    int total = rb->playlist_amount();
    rb->snprintf(meta, sizeof(meta), "TRACK %d OF %d", pos, total);
    rb->lcd_putsxy(116, 37, meta);

    /* Title 17 px Bold at (116, 49) — marquee-scrolled if too long. */
    rb->lcd_setfont(safe_font(font_title));
    rb->lcd_set_foreground(COL_INK);
    const char *title = (id3 && id3->title) ? id3->title : "—";
    putsxy_marquee(116, 49, title, LCD_WIDTH - 116 - 12,
                   *rb->current_tick, COL_INK, COL_SURFACE);

    /* Artist (deep muted) at (116, 68). */
    rb->lcd_setfont(safe_font(font_main));
    rb->lcd_set_foreground(COL_DEEP);
    const char *artist = (id3 && id3->artist) ? id3->artist : "";
    rb->lcd_putsxy(116, 68, artist);

    /* Album (light muted) at (116, 82). */
    rb->lcd_set_foreground(COL_MUTED);
    const char *album = (id3 && id3->album) ? id3->album : "";
    rb->lcd_putsxy(116, 82, album);

    /* Format badge — small box at (170, 98) with codec extension + bitrate. */
    if (id3) {
        rb->lcd_setfont(safe_font(font_status));
        /* Pull extension as codec hint (uppercase). */
        char codec[8] = "MP3";
        const char *dot = rb->strrchr(id3->path, '.');
        if (dot) {
            int n = 0;
            for (const char *p = dot + 1; *p && n < (int)sizeof(codec) - 1; p++, n++)
                codec[n] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
            codec[n] = '\0';
        }
        rb->lcd_set_foreground(COL_INK);
        rb->lcd_drawrect(170, 98, 30, 11);
        int cw = 0, ch = 0;
        rb->lcd_getstringsize((const unsigned char *)codec, &cw, &ch);
        rb->lcd_putsxy(170 + (30 - cw) / 2, 99, codec);

        char br[16];
        rb->snprintf(br, sizeof(br), "%d kbps", id3->bitrate);
        rb->lcd_set_foreground(COL_DEEP);
        rb->lcd_putsxy(204, 99, br);
    }

    /* Stars rating at (116, 113) — 5 stars × 8 px, drawn as polygons.
     * id3->rating is 0..10, mapped to 0..5 stars. */
    if (id3) {
        int stars = id3->rating / 2;
        for (int i = 0; i < 5; i++) {
            int sx = 116 + i * 9;
            unsigned col = (i < stars) ? COL_INK : COL_BORDER;
            rb->lcd_set_foreground(col);
            /* Simple 5-point star, ~7×7 px */
            int pts[][2] = {
                {sx + 3, 113}, {sx + 4, 116}, {sx + 7, 116},
                {sx + 4, 118}, {sx + 5, 121}, {sx + 3, 119},
                {sx + 1, 121}, {sx + 2, 118}, {sx + 0, 116},
                {sx + 3, 116},
            };
            for (size_t j = 0; j < sizeof(pts) / sizeof(pts[0]); j++)
                rb->lcd_drawpixel(pts[j][0], pts[j][1]);
        }
    }

    /* Up Next strip at y=187. */
    rb->lcd_setfont(safe_font(font_status));
    rb->lcd_set_foreground(COL_MUTED);
    rb->lcd_putsxy(18, 187, "UP NEXT");
    rb->lcd_set_foreground(COL_BORDER);
    rb->lcd_drawline(64, 189, 64, 196);
    struct mp3entry *next = rb->audio_next_track();
    if (next && next->title) {
        rb->lcd_setfont(safe_font(font_main));
        rb->lcd_set_foreground(COL_DEEP);
        rb->lcd_putsxy(72, 187, next->title);
    }

    /* Time row + progress bar at y=210/223. */
    rb->lcd_setfont(safe_font(font_main));
    rb->lcd_set_foreground(COL_DEEP);
    char tbuf[16];
    unsigned long elapsed = id3 ? id3->elapsed : 0;
    unsigned long length  = id3 ? id3->length  : 0;
    fmt_time(tbuf, sizeof(tbuf), elapsed);
    rb->lcd_putsxy(18, 210, tbuf);
    fmt_time(tbuf, sizeof(tbuf), length);
    int tw = 0, th = 0;
    rb->lcd_getstringsize((const unsigned char *)tbuf, &tw, &th);
    rb->lcd_putsxy(LCD_WIDTH - 18 - tw, 210, tbuf);

    /* Progress bar (3 px) — bg track + fg fill. */
    rb->lcd_set_foreground(LCD_RGBPACK(0xDE, 0xDA, 0xD3));
    rb->lcd_fillrect(18, 223, 284, 3);
    if (length > 0 && elapsed > 0) {
        int fill = (int)((elapsed * 284) / length);
        if (fill > 284) fill = 284;
        rb->lcd_set_foreground(COL_INK);
        rb->lcd_fillrect(18, 223, fill, 3);
    }

    /* Volume overlay — centered transient pill that appears after wheel
     * scroll, matches design's VolumeDemo. */
    if (vol_overlay_until > *rb->current_tick) {
        int ow = 200, oh = 56;
        int ox = (LCD_WIDTH - ow) / 2;
        int oy = (LCD_HEIGHT - oh) / 2;
        /* Slight shadow */
        rb->lcd_set_foreground(LCD_RGBPACK(0xCB, 0xC4, 0xB8));
        rb->lcd_fillrect(ox + 2, oy + 3, ow, oh);
        /* Plate */
        rounded_fillrect(ox, oy, ow, oh, 6,
                         LCD_RGBPACK(0x1A, 0x17, 0x14),
                         COL_SURFACE,
                         LCD_RGBPACK(0x87, 0x84, 0x80));
        /* "VOLUME" caps label */
        rb->lcd_setfont(safe_font(font_status));
        rb->lcd_set_foreground(LCD_RGBPACK(0xA8, 0x9E, 0x92));
        rb->lcd_set_background(LCD_RGBPACK(0x1A, 0x17, 0x14));
        rb->lcd_putsxy(ox + 14, oy + 10, "VOLUME");
        /* Big % number */
        rb->lcd_setfont(safe_font(font_title));
        rb->lcd_set_foreground(COL_SURFACE);
        char vbuf[8];
        rb->snprintf(vbuf, sizeof(vbuf), "%d%%", vol_overlay_pct);
        int vw = 0, vh = 0;
        rb->lcd_getstringsize((const unsigned char *)vbuf, &vw, &vh);
        rb->lcd_putsxy(ox + ow - 14 - vw, oy + 8, vbuf);
        /* Track + fill */
        int track_x = ox + 14, track_y = oy + 32;
        int track_w = ow - 28;
        rb->lcd_set_foreground(LCD_RGBPACK(0x4A, 0x44, 0x3D));
        rb->lcd_fillrect(track_x, track_y, track_w, 4);
        int fill_w = (vol_overlay_pct * track_w) / 100;
        rb->lcd_set_foreground(COL_SURFACE);
        rb->lcd_fillrect(track_x, track_y, fill_w, 4);
        rb->lcd_set_background(COL_SURFACE);
    }

    rb->lcd_update();
}

/* ===== Drawing ===== */
static void rounded_fillrect(int x, int y, int w, int h,
                             int r, unsigned fg, unsigned bg, unsigned aa)
{
    rb->lcd_set_foreground(fg);
    rb->lcd_fillrect(x, y, w, h);
    int r2 = 4 * r * r;
    int rin2 = 4 * (r - 1) * (r - 1);
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int a = 2 * (r - dx) - 1;
            int b = 2 * (r - dy) - 1;
            int d2 = a * a + b * b;
            unsigned color;
            if (d2 > r2)        color = bg;
            else if (d2 > rin2) color = aa;
            else                continue;
            rb->lcd_set_foreground(color);
            rb->lcd_drawpixel(x + dx,         y + dy);
            rb->lcd_drawpixel(x + w - 1 - dx, y + dy);
            rb->lcd_drawpixel(x + dx,         y + h - 1 - dy);
            rb->lcd_drawpixel(x + w - 1 - dx, y + h - 1 - dy);
        }
    }
}

static void draw_lock_glyph(int x, int y) {
    /* 8×11 closed padlock (matches design's StatusStrip lock SVG). */
    rb->lcd_drawrect(x, y + 4, 8, 6);
    rb->lcd_drawline(x + 1, y + 4, x + 1, y + 2);
    rb->lcd_drawline(x + 6, y + 4, x + 6, y + 2);
    rb->lcd_drawline(x + 1, y + 2, x + 6, y + 2);
}

static void draw_status_strip(void) {
    rb->lcd_setfont(safe_font(font_status));
    rb->lcd_set_foreground(COL_MUTED);
    rb->lcd_set_background(COL_SURFACE);

    /* Left: time as "h:MM AM/PM" caps. Skip on platforms without RTC. */
    struct tm *t = rb->get_time();
    if (t) {
        char buf[16];
        int h = t->tm_hour;
        const char *ampm = "AM";
        if (h == 0) { h = 12; }
        else if (h == 12) { ampm = "PM"; }
        else if (h > 12) { h -= 12; ampm = "PM"; }
        rb->snprintf(buf, sizeof(buf), "%d:%02d %s", h, t->tm_min, ampm);
        rb->lcd_putsxy(12, 4, buf);
    }

    /* Right: battery icon + percent text. */
    int pct = rb->battery_level();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char pctbuf[8];
    rb->snprintf(pctbuf, sizeof(pctbuf), "%d", pct);
    int pw = 0, ph = 0;
    rb->lcd_getstringsize((const unsigned char *)pctbuf, &pw, &ph);
    int batt_x = LCD_WIDTH - 12 - 16;
    rb->lcd_set_foreground(COL_DEEP);
    draw_battery_glyph(batt_x, 4, pct, false);
    rb->lcd_putsxy(batt_x - 4 - pw, 4, pctbuf);

    /* Persistent lock glyph just left of percent when hold is engaged. */
    if (rb->button_hold()) {
        rb->lcd_set_foreground(COL_INK);
        draw_lock_glyph(batt_x - 4 - pw - 12, 3);
    }

    rb->lcd_set_foreground(COL_INK);
}

static void draw_lock_overlay(void) {
    if (lock_overlay_until <= *rb->current_tick) return;
    int pw = 180, ph = 110;
    int px = (LCD_WIDTH - pw) / 2;
    int py = (LCD_HEIGHT - ph) / 2;
    if (lock_overlay_locked) {
        rounded_fillrect(px, py, pw, ph, 6,
                         LCD_RGBPACK(0x1A, 0x17, 0x14),
                         COL_SURFACE,
                         LCD_RGBPACK(0x87, 0x84, 0x80));
        /* Big closed padlock 28x32 centered, then "LOCKED" caps below. */
        int gx = px + (pw - 28) / 2, gy = py + 18;
        rb->lcd_set_foreground(COL_SURFACE);
        rb->lcd_fillrect(gx + 5, gy + 14, 18, 14);
        rb->lcd_drawline(gx + 9, gy + 14, gx + 9, gy + 9);
        rb->lcd_drawline(gx + 18, gy + 14, gx + 18, gy + 9);
        rb->lcd_drawline(gx + 9, gy + 9, gx + 18, gy + 9);
        rb->lcd_drawline(gx + 9, gy + 8, gx + 9, gy + 7);
        rb->lcd_drawline(gx + 18, gy + 8, gx + 18, gy + 7);
        rb->lcd_drawline(gx + 10, gy + 6, gx + 17, gy + 6);
        rb->lcd_setfont(safe_font(font_title));
        rb->lcd_set_background(LCD_RGBPACK(0x1A, 0x17, 0x14));
        rb->lcd_set_foreground(COL_SURFACE);
        int tw = 0, th = 0;
        rb->lcd_getstringsize((const unsigned char *)"LOCKED", &tw, &th);
        rb->lcd_putsxy(px + (pw - tw) / 2, py + ph - 32, "LOCKED");
    } else {
        rounded_fillrect(px, py, pw, ph, 6,
                         COL_SURFACE,
                         LCD_RGBPACK(0x1A, 0x17, 0x14),
                         LCD_RGBPACK(0x87, 0x84, 0x80));
        rb->lcd_set_foreground(COL_INK);
        int gx = px + (pw - 28) / 2, gy = py + 18;
        rb->lcd_fillrect(gx + 4, gy + 13, 18, 14);
        /* Open shackle leaning right */
        rb->lcd_drawline(gx + 8, gy + 13, gx + 8, gy + 8);
        rb->lcd_drawline(gx + 8, gy + 8, gx + 14, gy + 6);
        rb->lcd_setfont(safe_font(font_title));
        rb->lcd_set_background(COL_SURFACE);
        rb->lcd_set_foreground(COL_INK);
        int tw = 0, th = 0;
        rb->lcd_getstringsize((const unsigned char *)"UNLOCKED", &tw, &th);
        rb->lcd_putsxy(px + (pw - tw) / 2, py + ph - 32, "UNLOCKED");
    }
    rb->lcd_set_background(COL_SURFACE);
}

/* Detect hold switch toggle and trigger flash plate. */
static void poll_hold_switch(void) {
    bool now = rb->button_hold();
    if (now != prev_hold) {
        lock_overlay_locked = now;
        lock_overlay_until = *rb->current_tick + HZ;   /* 1 second */
        prev_hold = now;
    }
}

static void draw_header(const char *title, const char *right) {
    rb->lcd_setfont(safe_font(font_title));
    rb->lcd_set_foreground(COL_INK);
    rb->lcd_set_background(COL_SURFACE);
    rb->lcd_putsxy(14, HEADER_TOP + 4, title);
    if (right && right[0]) {
        rb->lcd_setfont(safe_font(font_status));
        rb->lcd_set_foreground(COL_MUTED);
        int rw = 0, rh = 0;
        rb->lcd_getstringsize((const unsigned char *)right, &rw, &rh);
        rb->lcd_putsxy(LCD_WIDTH - 14 - rw, HEADER_TOP + 7, right);
    }
    rb->lcd_set_foreground(COL_BORDER);
    rb->lcd_hline(0, LCD_WIDTH - 1, UNDERLINE_Y);
    rb->lcd_set_foreground(COL_INK);
}

static void draw_row(int row_idx, const char *label, bool sel) {
    int y = LIST_TOP + row_idx * ROW_H;
    rb->lcd_setfont(safe_font(font_main));
    int title_x = 6 + ROW_PAD_X;
    int title_max = LCD_WIDTH - title_x - 24;   /* leave room for chevron */
    if (sel) {
        rounded_fillrect(6, y, LCD_WIDTH - 12, ROW_H - 4,
                         4, COL_INK, COL_SURFACE, COL_INK_AA);
        rb->lcd_set_foreground(COL_SURFACE);
        rb->lcd_set_background(COL_INK);
        /* Selected long titles marquee-scroll. */
        putsxy_marquee(title_x, y + 4, label, title_max,
                       *rb->current_tick, COL_SURFACE, COL_INK);
        rb->lcd_putsxy(LCD_WIDTH - 22, y + 4, ">");
        rb->lcd_set_background(COL_SURFACE);
    } else {
        rb->lcd_set_foreground(COL_INK);
        rb->lcd_set_background(COL_SURFACE);
        putsxy_ellipsis(title_x, y + 4, label, title_max);
        rb->lcd_set_foreground(COL_MUTED);
        rb->lcd_putsxy(LCD_WIDTH - 22, y + 4, ">");
    }
}

/* Song row with track number prefix and duration on right.
 * Currently unused — F_SONGS rendering inlines this style with the album
 * hero. Kept for possible future flat-songs view. */
__attribute__((unused))
static void draw_song_row(int row_idx, int track_idx, bool sel) {
    int y = LIST_TOP + row_idx * ROW_H;
    rb->lcd_setfont(safe_font(font_main));

    char trk[8];
    rb->snprintf(trk, sizeof(trk), "%02d", list_track_num[track_idx]);

    char dur[12] = "";
    int len_ms = list_length_ms[track_idx];
    if (len_ms > 0) {
        int s = len_ms / 1000;
        rb->snprintf(dur, sizeof(dur), "%d:%02d", s / 60, s % 60);
    }
    int dw = 0, dh = 0;
    if (dur[0])
        rb->lcd_getstringsize((const unsigned char *)dur, &dw, &dh);

    if (sel) {
        rounded_fillrect(6, y, LCD_WIDTH - 12, ROW_H - 4,
                         4, COL_INK, COL_SURFACE, COL_INK_AA);
        rb->lcd_set_background(COL_INK);
        rb->lcd_set_foreground(COL_SURFACE);
        rb->lcd_putsxy(6 + ROW_PAD_X, y + 4, trk);
        rb->lcd_putsxy(6 + ROW_PAD_X + 22, y + 4, list_buf[track_idx]);
        if (dw) rb->lcd_putsxy(LCD_WIDTH - 18 - dw, y + 4, dur);
        rb->lcd_set_background(COL_SURFACE);
    } else {
        rb->lcd_set_background(COL_SURFACE);
        rb->lcd_set_foreground(COL_MUTED);
        rb->lcd_putsxy(6 + ROW_PAD_X, y + 4, trk);
        rb->lcd_set_foreground(COL_INK);
        rb->lcd_putsxy(6 + ROW_PAD_X + 22, y + 4, list_buf[track_idx]);
        rb->lcd_set_foreground(COL_MUTED);
        if (dw) rb->lcd_putsxy(LCD_WIDTH - 18 - dw, y + 4, dur);
    }
}

static void draw_list(const char *title, const char *right_meta,
                      const char *const *items, int count, int sel, int scroll)
{
    rb->lcd_set_background(COL_SURFACE);
    rb->lcd_set_foreground(COL_INK);
    rb->lcd_clear_display();
    draw_status_strip();
    draw_header(title, right_meta);

    int end = scroll + VISIBLE_ROWS;
    if (end > count) end = count;
    for (int i = scroll; i < end; i++) {
        const char *label = items ? items[i] : list_buf[i];
        draw_row(i - scroll, label, i == sel);
    }
    if (count == 0) {
        rb->lcd_setfont(safe_font(font_main));
        rb->lcd_set_foreground(COL_INK);
        rb->lcd_putsxy(14, LIST_TOP + 12, "Nothing here yet.");
        rb->lcd_setfont(safe_font(font_status));
        rb->lcd_set_foreground(COL_MUTED);
        rb->lcd_putsxy(14, LIST_TOP + 32,
                       "Initialize the Database from");
        rb->lcd_putsxy(14, LIST_TOP + 46,
                       "Rockbox's main menu, then return.");
    }
    rb->lcd_update();
}

static void draw_current(void) {
    struct frame *f = top();
    char meta[40] = "";
    switch (f->kind) {
    case F_MAIN:
        draw_list("iPod", "", MAIN_ITEMS, MAIN_LEN, f->sel, f->scroll);
        break;
    case F_MUSIC:
        draw_list("Music", "", MUSIC_ITEMS, MUSIC_LEN, f->sel, f->scroll);
        break;
    case F_ARTISTS:
        rb->snprintf(meta, sizeof(meta), "%d", list_count);
        draw_list("Artists", meta, NULL, list_count, f->sel, f->scroll);
        break;
    case F_ALBUMS: {
        rb->lcd_set_background(COL_SURFACE);
        rb->lcd_set_foreground(COL_INK);
        rb->lcd_clear_display();
        draw_status_strip();
        rb->snprintf(meta, sizeof(meta), "%d", list_count);
        draw_header(f->header_title[0] ? f->header_title : "Albums", meta);
        const int row_h = 28;
        int avail = LCD_HEIGHT - LIST_TOP - 4;
        int visible = avail / row_h;
        int end = f->scroll + visible;
        if (end > list_count) end = list_count;
        for (int i = f->scroll; i < end; i++) {
            int rowy = LIST_TOP + (i - f->scroll) * row_h;
            bool sel = (i == f->sel);
            rb->lcd_setfont(safe_font(font_main));
            if (sel) {
                rounded_fillrect(6, rowy, LCD_WIDTH - 12, row_h - 4,
                                 4, COL_INK, COL_SURFACE, COL_INK_AA);
                rb->lcd_set_background(COL_INK);
            } else {
                rb->lcd_set_background(COL_SURFACE);
            }
            /* Selected row: try to load real album art; others get the
             * procedural color chip. Loading is gated on path equality so
             * we don't decode every redraw. */
            int chip_x = 14, chip_y = rowy + 2;
            bool drew_thumb = false;
            if (sel) {
                /* Lazy-fill list_path[i] on demand. */
                if (list_path[i][0] == '\0') {
                    fetch_sample_filename(f->parent_artist_seek,
                                          list_seek[i],
                                          list_path[i], LIST_PATH_LEN);
                }
                if (list_path[i][0])
                    update_rowthumb_for_path(list_path[i]);
                if (rowthumb_valid && rowthumb_bm.width > 0) {
                    int tx = chip_x + (ROWTHUMB_W - rowthumb_bm.width) / 2;
                    int ty = chip_y + (ROWTHUMB_H - rowthumb_bm.height) / 2;
                    rb->lcd_bitmap((const fb_data *)rowthumb_bm.data, tx, ty,
                                   rowthumb_bm.width, rowthumb_bm.height);
                    drew_thumb = true;
                }
            }
            if (!drew_thumb) {
                int hue = album_hue(list_buf[i]);
                rb->lcd_set_foreground(hsv_to_rgb565(hue, 28, 78));
                rb->lcd_fillrect(chip_x, chip_y, 22, 22);
                rb->lcd_set_foreground(hsv_to_rgb565(hue, 35, 60));
                rb->lcd_drawrect(chip_x, chip_y, 22, 22);
            }

            int title_x = chip_x + 22 + 10;
            int title_max = LCD_WIDTH - title_x - 24;
            rb->lcd_set_foreground(sel ? COL_SURFACE : COL_INK);
            putsxy_ellipsis(title_x, rowy + 2, list_buf[i], title_max);

            /* Sub-text: year (if non-zero). */
            if (list_track_num[i] > 0) {
                char ystr[12];
                rb->snprintf(ystr, sizeof(ystr), "%d", list_track_num[i]);
                rb->lcd_setfont(safe_font(font_status));
                rb->lcd_set_foreground(sel ? LCD_RGBPACK(0xC8, 0xC0, 0xB6) : COL_MUTED);
                rb->lcd_putsxy(title_x, rowy + 16, ystr);
            }
            /* Right chevron */
            rb->lcd_setfont(safe_font(font_main));
            rb->lcd_set_foreground(sel ? LCD_RGBPACK(0xB8, 0xB0, 0xA4) : COL_MUTED);
            rb->lcd_putsxy(LCD_WIDTH - 22, rowy + 6, ">");
            rb->lcd_set_background(COL_SURFACE);
        }
        if (list_count == 0) {
            rb->lcd_setfont(safe_font(font_main));
            rb->lcd_set_foreground(COL_MUTED);
            rb->lcd_putsxy(14, LIST_TOP + 8, "(empty)");
        }
        rb->lcd_update();
        break;
    }
    case F_SONGS: {
        rb->lcd_set_background(COL_SURFACE);
        rb->lcd_set_foreground(COL_INK);
        rb->lcd_clear_display();
        draw_status_strip();

        bool show_hero = (f->parent_album_seek >= 0);
        int LIST_Y;

        if (show_hero) {
            /* Top header bar: "‹ Albums" + "n / total" — collection-detail.jsx */
            rb->lcd_setfont(safe_font(font_title));
            rb->lcd_set_foreground(COL_MUTED);
            rb->lcd_putsxy(12, HEADER_TOP + 4, "<");
            rb->lcd_set_foreground(COL_INK);
            rb->lcd_putsxy(22, HEADER_TOP + 4, "Albums");

            rb->lcd_setfont(safe_font(font_status));
            rb->lcd_set_foreground(COL_MUTED);
            char hdr_right[16];
            rb->snprintf(hdr_right, sizeof(hdr_right), "%d / %d",
                         f->sel + 1, list_count);
            int rw = 0, rh = 0;
            rb->lcd_getstringsize((const unsigned char *)hdr_right, &rw, &rh);
            rb->lcd_putsxy(LCD_WIDTH - 14 - rw, HEADER_TOP + 7, hdr_right);
            rb->lcd_set_foreground(COL_BORDER);
            rb->lcd_hline(0, LCD_WIDTH - 1, UNDERLINE_Y);

            /* Album hero: 56×56 thumb + title + artist + meta */
            const int HERO_Y = UNDERLINE_Y + 6;
            if (list_count > 0)
                update_thumb_for_path(list_path[0]);
            if (thumb_cache_valid && thumb_bm.width > 0) {
                int tx = 14 + (THUMB_W - thumb_bm.width) / 2;
                int ty = HERO_Y + (THUMB_H - thumb_bm.height) / 2;
                rb->lcd_bitmap((const fb_data *)thumb_bm.data, tx, ty,
                               thumb_bm.width, thumb_bm.height);
            } else {
                unsigned light = LCD_RGBPACK(0xC8, 0xBC, 0xAA);
                rb->lcd_set_foreground(light);
                rb->lcd_fillrect(14, HERO_Y, THUMB_W, THUMB_H);
            }

            rb->lcd_setfont(safe_font(font_title));
            rb->lcd_set_foreground(COL_INK);
            rb->lcd_set_background(COL_SURFACE);
            putsxy_ellipsis(14 + THUMB_W + 12, HERO_Y + 2,
                            f->header_title[0] ? f->header_title : "Album",
                            LCD_WIDTH - (14 + THUMB_W + 12) - 14);

            rb->lcd_setfont(safe_font(font_main));
            rb->lcd_set_foreground(COL_DEEP);
            if (depth >= 2) {
                const struct frame *parent = &stack[depth - 2];
                if (parent->kind == F_ALBUMS && parent->header_title[0])
                    putsxy_ellipsis(14 + THUMB_W + 12, HERO_Y + 22,
                                    parent->header_title,
                                    LCD_WIDTH - (14 + THUMB_W + 12) - 14);
            }

            unsigned long total_ms = 0;
            for (int i = 0; i < list_count; i++) total_ms += list_length_ms[i];
            unsigned long total_min = total_ms / 60000;
            char meta_line[40];
            if (total_min >= 60)
                rb->snprintf(meta_line, sizeof(meta_line),
                             "%d tracks  ·  %luh %lum",
                             list_count, total_min / 60, total_min % 60);
            else
                rb->snprintf(meta_line, sizeof(meta_line),
                             "%d tracks  ·  %lum",
                             list_count, total_min);
            rb->lcd_setfont(safe_font(font_status));
            rb->lcd_set_foreground(COL_MUTED);
            rb->lcd_putsxy(14 + THUMB_W + 12, HERO_Y + 38, meta_line);

            LIST_Y = HERO_Y + THUMB_H + 6;
            rb->lcd_set_foreground(COL_BORDER);
            rb->lcd_hline(0, LCD_WIDTH - 1, LIST_Y - 3);
        } else {
            /* Flat songs list (e.g. "All Songs" with no album filter). */
            char meta_str[16];
            rb->snprintf(meta_str, sizeof(meta_str), "%d", list_count);
            draw_header(f->header_title[0] ? f->header_title : "Songs", meta_str);
            LIST_Y = LIST_TOP;
        }

        const int song_row_h = 20;
        const int song_bar_h = 18;       /* selector bar fits text+descender */
        const int song_text_y = 3;       /* text Y offset within row */
        int avail = LCD_HEIGHT - LIST_Y - 4;
        int visible = avail / song_row_h;
        int end = f->scroll + visible;
        if (end > list_count) end = list_count;

        /* Currently-playing path for the "now playing" dot indicator. */
        struct mp3entry *cur = rb->audio_current_track();
        const char *cur_path = (cur && cur->path[0]) ? cur->path : NULL;

        for (int i = f->scroll; i < end; i++) {
            int rowy = LIST_Y + (i - f->scroll) * song_row_h;
            bool sel = (i == f->sel);
            bool is_playing = cur_path && (rb->strcmp(list_path[i], cur_path) == 0);
            char trk[8] = "", dur[12] = "";
            if (show_hero && !is_playing)
                rb->snprintf(trk, sizeof(trk), "%2d", list_track_num[i]);
            int len_ms = list_length_ms[i];
            if (len_ms > 0) {
                int s = len_ms / 1000;
                rb->snprintf(dur, sizeof(dur), "%d:%02d", s / 60, s % 60);
            }
            /* Set font BEFORE measuring — otherwise the first row measures
             * against whatever font was last used (e.g. font_status from
             * the album hero), giving a smaller width and shifting the
             * duration text too far right on row 0. */
            rb->lcd_setfont(safe_font(font_main));
            int dw = 0, dh = 0;
            if (dur[0])
                rb->lcd_getstringsize((const unsigned char *)dur, &dw, &dh);
            int title_x = show_hero ? 34 : 14;
            int title_max_w = LCD_WIDTH - title_x - (dw ? dw + 24 : 14);
            if (sel) {
                rounded_fillrect(6, rowy, LCD_WIDTH - 12, song_bar_h,
                                 3, COL_INK, COL_SURFACE, COL_INK_AA);
                rb->lcd_set_background(COL_INK);
                rb->lcd_set_foreground(COL_SURFACE);
                if (trk[0]) rb->lcd_putsxy(14, rowy + song_text_y, trk);
                putsxy_ellipsis(title_x, rowy + song_text_y, list_buf[i],
                                title_max_w);
                if (dw) rb->lcd_putsxy(LCD_WIDTH - 18 - dw, rowy + song_text_y, dur);
                rb->lcd_set_background(COL_SURFACE);
            } else {
                rb->lcd_set_background(COL_SURFACE);
                rb->lcd_set_foreground(COL_MUTED);
                if (trk[0]) rb->lcd_putsxy(14, rowy + song_text_y, trk);
                rb->lcd_set_foreground(COL_INK);
                putsxy_ellipsis(title_x, rowy + song_text_y, list_buf[i],
                                title_max_w);
                rb->lcd_set_foreground(COL_MUTED);
                if (dw) rb->lcd_putsxy(LCD_WIDTH - 18 - dw, rowy + song_text_y, dur);
            }

            /* Now-playing 3-bar dot replaces the track number when this row
             * matches the currently-playing track. */
            if (is_playing && show_hero) {
                rb->lcd_set_foreground(sel ? COL_SURFACE : COL_INK);
                rb->lcd_fillrect(14, rowy + 9, 2, 6);
                rb->lcd_fillrect(17, rowy + 6, 2, 9);
                rb->lcd_fillrect(20, rowy + 10, 2, 5);
            }
        }
        if (list_count == 0) {
            rb->lcd_setfont(safe_font(font_main));
            rb->lcd_set_foreground(COL_MUTED);
            rb->lcd_putsxy(14, LIST_Y + 8, "(empty)");
        }
        rb->lcd_update();
        break;
    }
    case F_GENRES:
        rb->snprintf(meta, sizeof(meta), "%d", list_count);
        draw_list("Genres", meta, NULL, list_count, f->sel, f->scroll);
        break;
    case F_COMPOSERS:
        rb->snprintf(meta, sizeof(meta), "%d", list_count);
        draw_list("Composers", meta, NULL, list_count, f->sel, f->scroll);
        break;
    case F_PLAYLISTS:
        rb->snprintf(meta, sizeof(meta), "%d", list_count);
        draw_list("Playlists", meta, NULL, list_count, f->sel, f->scroll);
        break;
    case F_SETTINGS: {
        rb->lcd_set_background(COL_SURFACE);
        rb->lcd_set_foreground(COL_INK);
        rb->lcd_clear_display();
        draw_status_strip();
        draw_header("Settings", "");
        rb->lcd_setfont(safe_font(font_main));

        /* Settings rows: { label, current value text } */
        const char *items[3];
        char rep_v[8];
        items[0] = rb->global_settings->playlist_shuffle ? "On" : "Off";
        int rmode = rb->global_settings->repeat_mode;
        rb->snprintf(rep_v, sizeof(rep_v),
                     rmode == 2 ? "One" : (rmode == 1 ? "All" : "Off"));
        items[1] = rep_v;
        items[2] = NULL;  /* About is a drill row */

        const char *labels[3] = {"Shuffle", "Repeat", "About"};
        for (int i = 0; i < 3; i++) {
            int rowy = LIST_TOP + i * ROW_H;
            bool sel = (i == f->sel);
            if (sel) {
                rounded_fillrect(6, rowy, LCD_WIDTH - 12, ROW_H - 4,
                                 4, COL_INK, COL_SURFACE, COL_INK_AA);
                rb->lcd_set_background(COL_INK);
                rb->lcd_set_foreground(COL_SURFACE);
            } else {
                rb->lcd_set_background(COL_SURFACE);
                rb->lcd_set_foreground(COL_INK);
            }
            rb->lcd_putsxy(14, rowy + 4, labels[i]);
            if (items[i]) {
                int vw = 0, vh = 0;
                rb->lcd_getstringsize((const unsigned char *)items[i],
                                      &vw, &vh);
                if (!sel) rb->lcd_set_foreground(COL_DEEP);
                rb->lcd_putsxy(LCD_WIDTH - 14 - vw, rowy + 4, items[i]);
            } else {
                /* About: drill chevron */
                if (!sel) rb->lcd_set_foreground(COL_MUTED);
                rb->lcd_putsxy(LCD_WIDTH - 22, rowy + 4, ">");
            }
            rb->lcd_set_background(COL_SURFACE);
        }
        rb->lcd_update();
        break;
    }
    case F_ABOUT: {
        rb->lcd_set_background(COL_SURFACE);
        rb->lcd_set_foreground(COL_INK);
        rb->lcd_clear_display();
        draw_status_strip();
        draw_header("About", "");
        rb->lcd_setfont(safe_font(font_main));
        struct {
            const char *k, *v;
            char buf[32];
        } rows[8];
        int n = 0;
        rows[n++] = (typeof(rows[0])){.k = "Model",     .v = "iPod Video 5G"};
        rows[n++] = (typeof(rows[0])){.k = "Theme",     .v = "Linen"};
        rows[n++] = (typeof(rows[0])){.k = "Plugin",    .v = "Cabinet"};
        rows[n++] = (typeof(rows[0])){.k = "Resolution",.v = "320x240"};
        rows[n].k = "Battery";
        rb->snprintf(rows[n].buf, sizeof(rows[n].buf), "%d%%",
                     rb->battery_level());
        rows[n].v = rows[n].buf; n++;
        struct tagcache_stat *ts = rb->tagcache_get_stat();
        if (ts) {
            rows[n].k = "Songs";
            rb->snprintf(rows[n].buf, sizeof(rows[n].buf), "%d",
                         (int)ts->total_entries);
            rows[n].v = rows[n].buf; n++;
        }
        rows[n++] = (typeof(rows[0])){.k = "Firmware", .v = "Rockbox"};
        int y = LIST_TOP + 4;
        for (int i = 0; i < n; i++) {
            rb->lcd_set_foreground(COL_MUTED);
            rb->lcd_putsxy(14, y, rows[i].k);
            rb->lcd_set_foreground(COL_INK);
            rb->lcd_putsxy(120, y, rows[i].v);
            rb->lcd_set_foreground(COL_BORDER);
            rb->lcd_hline(14, LCD_WIDTH - 14, y + 16);
            y += 18;
        }
        rb->lcd_update();
        break;
    }
    case F_PLAYING:
        draw_now_playing();
        break;
    }
}

/* ===== Input handling ===== */
static void scroll_into_view(struct frame *f) {
    int wanted_top = f->sel - VISIBLE_ROWS / 3;
    if (wanted_top < 0) wanted_top = 0;
    if (f->sel < f->scroll) f->scroll = wanted_top;
    else if (f->sel >= f->scroll + VISIBLE_ROWS) f->scroll = wanted_top;
}

static int frame_count(struct frame *f) {
    switch (f->kind) {
    case F_MAIN:    return MAIN_LEN;
    case F_MUSIC:   return MUSIC_LEN;
    case F_ARTISTS:
    case F_ALBUMS:
    case F_SONGS:
    case F_GENRES:
    case F_COMPOSERS:
    case F_PLAYLISTS: return list_count;
    case F_SETTINGS:  return 3;
    case F_ABOUT:     return 0;
    case F_PLAYING:   return 0;
    }
    return 0;
}

/* Returns true if caller should quit the plugin (e.g. handed off to WPS). */
static bool handle_select(void) {
    struct frame *f = top();
    switch (f->kind) {
    case F_MAIN:
        if (f->sel == 0) {                     /* Music */
            push(F_MUSIC);
        } else if (f->sel == 1) {              /* Playlists */
            push(F_PLAYLISTS);
            load_playlists();
            top()->sel = 0;
        } else if (f->sel == 4) {              /* Settings */
            push(F_SETTINGS);
            top()->sel = 0;
        } else if (f->sel == 5) {              /* Now Playing */
            if (rb->audio_status() & AUDIO_STATUS_PLAY)
                push(F_PLAYING);
            else
                rb->splash(HZ, "No track playing");
        }
        /* Podcasts (2), Audiobooks (3) — Rockbox doesn't natively
         * distinguish these; would need a folder convention. */
        break;
    case F_MUSIC:
        if (f->sel == 0) {                     /* Artists */
            push(F_ARTISTS);
            load_artists();
            top()->sel = 0;
        } else if (f->sel == 1) {              /* Albums (all) */
            struct frame *child = push(F_ALBUMS);
            if (child) {
                child->parent_artist_seek = -1;
                rb->strlcpy(child->header_title, "Albums",
                            sizeof(child->header_title));
            }
            load_albums_for_artist(-1);
            top()->sel = 0;
        } else if (f->sel == 2) {              /* Songs (all) */
            struct frame *child = push(F_SONGS);
            if (child) {
                child->parent_artist_seek = -1;
                child->parent_album_seek  = -1;
                rb->strlcpy(child->header_title, "Songs",
                            sizeof(child->header_title));
            }
            load_songs_for_album(-1, -1);
            top()->sel = 0;
        } else if (f->sel == 3) {              /* Genres */
            push(F_GENRES);
            load_tag_list(tag_genre);
            top()->sel = 0;
        } else if (f->sel == 4) {              /* Composers */
            push(F_COMPOSERS);
            load_tag_list(tag_composer);
            top()->sel = 0;
        }
        break;
    case F_ARTISTS: {
        int artist_seek = list_seek[f->sel];
        char artist_name[LIST_LABEL_LEN];
        rb->strlcpy(artist_name, list_buf[f->sel], sizeof(artist_name));
        struct frame *child = push(F_ALBUMS);
        if (child) {
            child->parent_artist_seek = artist_seek;
            rb->strlcpy(child->header_title, artist_name, sizeof(child->header_title));
        }
        load_albums_for_artist(artist_seek);
        top()->sel = 0;
        break;
    }
    case F_ALBUMS: {
        int album_seek = list_seek[f->sel];
        char album_name[LIST_LABEL_LEN];
        rb->strlcpy(album_name, list_buf[f->sel], sizeof(album_name));
        struct frame *child = push(F_SONGS);
        if (child) {
            child->parent_album_seek = album_seek;
            rb->strlcpy(child->header_title, album_name, sizeof(child->header_title));
        }
        load_songs_for_album(top()->parent_artist_seek, album_seek);
        top()->sel = 0;
        break;
    }
    case F_GENRES: {
        int genre_seek = list_seek[f->sel];
        char gname[LIST_LABEL_LEN];
        rb->strlcpy(gname, list_buf[f->sel], sizeof(gname));
        struct frame *child = push(F_SONGS);
        if (child) {
            child->parent_genre_seek = genre_seek;
            rb->strlcpy(child->header_title, gname, sizeof(child->header_title));
        }
        load_songs_filtered(-1, -1, genre_seek, -1);
        top()->sel = 0;
        break;
    }
    case F_COMPOSERS: {
        int comp_seek = list_seek[f->sel];
        char cname[LIST_LABEL_LEN];
        rb->strlcpy(cname, list_buf[f->sel], sizeof(cname));
        struct frame *child = push(F_SONGS);
        if (child) {
            child->parent_composer_seek = comp_seek;
            rb->strlcpy(child->header_title, cname, sizeof(child->header_title));
        }
        load_songs_filtered(-1, -1, -1, comp_seek);
        top()->sel = 0;
        break;
    }
    case F_PLAYLISTS:
        if (f->sel >= 0 && f->sel < list_count) {
            if (play_m3u(list_path[f->sel]) == 0)
                push(F_PLAYING);
        }
        break;
    case F_SETTINGS:
        if (f->sel == 0) {
            /* Toggle Shuffle */
            rb->global_settings->playlist_shuffle =
                !rb->global_settings->playlist_shuffle;
        } else if (f->sel == 1) {
            /* Cycle Repeat: 0=off, 1=all, 2=one (skip 3=shuffle, 4=ab) */
            int r = rb->global_settings->repeat_mode;
            r = (r + 1) % 3;
            rb->global_settings->repeat_mode = r;
        } else if (f->sel == 2) {
            push(F_ABOUT);
            top()->sel = 0;
        }
        break;
    case F_ABOUT:
        /* No drill — info-only screen. */
        break;
    case F_SONGS:
        /* Build playlist, start playback, and push our own Now Playing
         * frame instead of handing off to .wps. */
        if (play_track_list(f->sel) == 0)
            push(F_PLAYING);
        break;
    case F_PLAYING:
        /* Cycle through info pages. Use BUTTON_PLAY for pause/resume. */
        np_page = (np_page + 1) % 3;
        break;
    }
    return false;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    load_fonts();
    load_status_icons();
    push(F_MAIN);

    bool exit_plugin = false;
    while (!exit_plugin) {
        poll_hold_switch();
        draw_current();
        draw_lock_overlay();
        if (lock_overlay_until > *rb->current_tick)
            rb->lcd_update();
        struct frame *f = top();
        /* Periodic redraw for: Now Playing (progress ticks), transient
         * overlays (volume/lock fade), and any list view (marquee scroll). */
        bool overlay_active = (lock_overlay_until > *rb->current_tick) ||
                              (vol_overlay_until > *rb->current_tick);
        bool needs_redraw = (f->kind == F_PLAYING) || overlay_active ||
                            (f->kind != F_MAIN && f->kind != F_MUSIC &&
                             f->kind != F_ABOUT);
        int btn = needs_redraw
                  ? rb->button_get_w_tmo(HZ / 4)
                  : rb->button_get(true);
        f = top();
        int n = frame_count(f);
        switch (btn) {
        case BUTTON_NONE:
            /* Timeout — just redraw next loop. */
            break;
        case BUTTON_SCROLL_BACK:
        case BUTTON_SCROLL_BACK | BUTTON_REPEAT:
            if (f->kind == F_PLAYING) {
                int vmin = rb->sound_min(SOUND_VOLUME);
                int vmax = rb->sound_max(SOUND_VOLUME);
                int vol  = rb->global_status->volume;
                vol -= 2;
                if (vol < vmin) vol = vmin;
                rb->global_status->volume = vol;
                rb->sound_set(SOUND_VOLUME, vol);
                vol_overlay_pct = (vol - vmin) * 100 / (vmax - vmin);
                vol_overlay_until = *rb->current_tick + HZ;
            } else if (n > 0) {
                /* Held wheel = jump by 8 for fast browsing of long lists. */
                int step = (btn & BUTTON_REPEAT) ? 8 : 1;
                f->sel = (f->sel - step + n) % n;
                scroll_into_view(f);
            }
            break;
        case BUTTON_SCROLL_FWD:
        case BUTTON_SCROLL_FWD | BUTTON_REPEAT:
            if (f->kind == F_PLAYING) {
                int vmin = rb->sound_min(SOUND_VOLUME);
                int vmax = rb->sound_max(SOUND_VOLUME);
                int vol  = rb->global_status->volume;
                vol += 2;
                if (vol > vmax) vol = vmax;
                rb->global_status->volume = vol;
                rb->sound_set(SOUND_VOLUME, vol);
                vol_overlay_pct = (vol - vmin) * 100 / (vmax - vmin);
                vol_overlay_until = *rb->current_tick + HZ;
            } else if (n > 0) {
                int step = (btn & BUTTON_REPEAT) ? 8 : 1;
                f->sel = (f->sel + step) % n;
                scroll_into_view(f);
            }
            break;
        case BUTTON_LEFT:
            if (f->kind == F_PLAYING) {
                rb->audio_prev();
            } else if (n > 0) {
                f->sel = (f->sel - 1 + n) % n;
                scroll_into_view(f);
            }
            break;
        case BUTTON_RIGHT:
            if (f->kind == F_PLAYING) {
                rb->audio_next();
            } else if (n > 0) {
                f->sel = (f->sel + 1) % n;
                scroll_into_view(f);
            }
            break;
        case BUTTON_PLAY:
            if (rb->audio_status() & AUDIO_STATUS_PAUSE) rb->audio_resume();
            else if (rb->audio_status() & AUDIO_STATUS_PLAY) rb->audio_pause();
            break;
        case BUTTON_SELECT:
            handle_select();
            break;
        case BUTTON_MENU:
            if (depth > 1) pop();
            else exit_plugin = true;
            break;
        default:
            break;
        }
    }

    unload_fonts();
    rb->lcd_setfont(FONT_UI);
    return PLUGIN_OK;
}
