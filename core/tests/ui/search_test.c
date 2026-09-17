/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/search_test.c — Music > Search (core/ui/search.c) on the host.
 *
 * THE POINT OF THIS FILE. Search is the first screen on this device whose
 * whole value is a JUDGEMENT rather than a fact: which of two matches the
 * person meant. "Sunflower Bean" above "The Sun", the artist above two
 * hundred songs, the accented spelling reachable from an unaccented query,
 * and the cap admitting how much it hid — none of that is visible from a
 * screenshot, and all of it is one comparison away from being silently
 * wrong.
 *
 * The library is injected (search_source_t), so the fixture below is nineteen
 * names chosen for the cases that bite: a "The " artist filed under its third
 * word, an accented album, an apostrophe the 39-cell ring cannot type, an
 * album tagged in capitals, four songs whose titles START with "sun" and one
 * that merely contains it.
 *
 * §6-8 are the painter, in chrome_test.c's style: render into console_fb() and
 * read the pixels back, because the layout's claims — the status strip's band
 * is untouched, the scrollbar column is untouched in PICK, the footer appears
 * only when something was hidden — are exactly the ones a renderer breaks by
 * accident.
 */

#include <stdio.h>
#include <string.h>

#include "search.h"
#include "chrome.h"
#include "palette.h"
#include "text.h"
#include "console.h"
#include "hal.h"

#include "../xfail.h"

/* ---- the injected library ---------------------------------------------- */

static const char *const ARTISTS[] = {
    "Daniel Caesar",          /* 0 */
    "The Kid LAROI",          /* 1  — sorted under K, so "kid" is a PREFIX */
    "LANY",                   /* 2 */
    "Post Malone",            /* 3 */
    "Sunflower Bean",         /* 4 */
};
static const char *const ALBUMS[] = {
    "Sun Leads Me On",        /* 0 */
    "Hollywood's Bleeding",   /* 1 */
    "AUSTIN",                 /* 2 */
    "\xC3\x89lan",            /* 3  — Élan */
};
static const char *const PLAYLISTS[] = {
    "Sunday Morning",         /* 0 */
    "Road Trip",              /* 1 */
};
/* In TITLE order, which is the order the sorted view hands them over. */
static const char *const SONGS[] = {
    "Big Kid Energy",                                  /* 0 */
    "Circles",                                         /* 1 */
    "It\xE2\x80\x99s Over",                            /* 2  — smart quote */
    "Sun Leads Me On",                                 /* 3 */
    "Sunflower",                                       /* 4 */
    "Sunflower (Spider-Man: Into the Spider-Verse)",   /* 5 */
    "Sunny Afternoon",                                 /* 6 */
    "The Sun",                                         /* 7  — substring only */
};

static const char *name_of(int type, int i)
{
    switch (type) {
    case SEARCH_T_ARTIST:
        return (i >= 0 && i < (int)(sizeof ARTISTS / sizeof *ARTISTS))
                   ? ARTISTS[i] : 0;
    case SEARCH_T_ALBUM:
        return (i >= 0 && i < (int)(sizeof ALBUMS / sizeof *ALBUMS))
                   ? ALBUMS[i] : 0;
    case SEARCH_T_PLAYLIST:
        return (i >= 0 && i < (int)(sizeof PLAYLISTS / sizeof *PLAYLISTS))
                   ? PLAYLISTS[i] : 0;
    default:
        return (i >= 0 && i < (int)(sizeof SONGS / sizeof *SONGS))
                   ? SONGS[i] : 0;
    }
}

/* library/names.c artist_key(), reproduced here so the suite does not have to
 * link the library for four characters. Artists only, as main.c does. */
static const char *key_of(int type, int i)
{
    const char *s = name_of(type, i);
    if (!s || type != SEARCH_T_ARTIST) return s;
    if ((s[0] == 'T' || s[0] == 't') && (s[1] == 'h' || s[1] == 'H') &&
        (s[2] == 'e' || s[2] == 'E') && s[3] == ' ') {
        return s + 4;
    }
    return s;
}

static search_source_t SRC = {
    { (int)(sizeof ARTISTS   / sizeof *ARTISTS),
      (int)(sizeof ALBUMS    / sizeof *ALBUMS),
      (int)(sizeof PLAYLISTS / sizeof *PLAYLISTS),
      (int)(sizeof SONGS     / sizeof *SONGS) },
    name_of, key_of
};

/* A library with more of one thing than the cap can hold. */
static const char *big_name(int type, int i)
{
    (void)type; (void)i;
    return "Sunflower";                 /* every song, the same title */
}
static const char *big_name_mid(int type, int i)
{
    (void)type; (void)i;
    return "The Sunflower";             /* ...none of them a prefix match */
}

/* ---- driving the model -------------------------------------------------- */

static search_t S;

/* Type `q` through the ring: park the cursor on each character's cell and
 * SELECT it, exactly as a person would. */
static void type_query(const char *q)
{
    for (; *q; q++) {
        if (*q == ' ')                        S.cell = SEARCH_CELL_SPACE;
        else if (*q >= 'a' && *q <= 'z')      S.cell = (uint8_t)(*q - 'a');
        else                                  S.cell = (uint8_t)(36 - 10 + (*q - '0'));
        (void)search_key(&S, SEARCH_KEY_SELECT);
    }
}

static void fresh(const char *q)
{
    search_reset(&S);
    type_query(q);
    search_scan(&S, &SRC);
}

static int hit_is(int i, int type, int idx, int rank)
{
    if (i >= S.nhit) return 0;
    return S.hit[i].type == type && S.hit[i].idx == idx && S.hit[i].rank == rank;
}

/* ---- reading the framebuffer back --------------------------------------- */

static int count_in(int x, int y, int w, int h, uint16_t c)
{
    const uint16_t *fb = console_framebuffer();
    int n = 0;
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            if (yy < 0 || xx < 0 || yy >= LCD_HEIGHT || xx >= LCD_WIDTH) continue;
            if (fb[yy * LCD_WIDTH + xx] == c) n++;
        }
    }
    return n;
}

static int count_not(int x, int y, int w, int h, uint16_t c)
{
    return w * h - count_in(x, y, w, h, c);
}

static int count_outside(int x, int y, int w, int h, uint16_t c)
{
    const uint16_t *fb = console_framebuffer();
    int n = 0;
    for (int yy = 0; yy < LCD_HEIGHT; yy++) {
        for (int xx = 0; xx < LCD_WIDTH; xx++) {
            int inside = (xx >= x && xx < x + w && yy >= y && yy < y + h);
            if (!inside && fb[yy * LCD_WIDTH + xx] == c) n++;
        }
    }
    return n;
}

/* The rows the painter is given, so the pixels have something to be. */
static void row_fill(const search_hit_t *h, char *title, char *sub,
                     char *right, int *greyed)
{
    const char *nm = name_of(h->type, h->idx);
    int n = 0;
    for (; nm && nm[n] && n < SEARCH_ROW_MAX - 1; n++) title[n] = nm[n];
    title[n] = '\0';
    static const char *const EYEBROW[] = { "ARTIST", "ALBUM", "PLAYLIST", "SONG" };
    const char *e = EYEBROW[h->type];
    for (n = 0; e[n] && n < SEARCH_ROW_MAX - 1; n++) sub[n] = e[n];
    sub[n] = '\0';
    right[0] = '\0';
    *greyed  = 0;
}

static void paint(void)
{
    console_clear(LINEN_SURFACE);
    console_damage_reset();
    search_render(&S, row_fill);
}

int main(void)
{
    xfail_ctx c = { "search", 0, 0, 0 };

    /* ---- 1. the ring ----------------------------------------------------- */
    search_reset(&S);
    xpect(&c, "a fresh screen is PICK, empty, with the cursor on 'A'",
          S.mode == SEARCH_PICK && S.qlen == 0 && S.cell == 0 && S.nhit == 0);
    xpect(&c, "one detent is one cell", search_ring_move(&S, 4) == 1 && S.cell == 1);
    xpect(&c, "half a detent moves nothing and carries its remainder",
          search_ring_move(&S, 2) == 0 && S.cell == 1 && S.ring_accum == 2);
    xpect(&c, "...the other half completes the cell",
          search_ring_move(&S, 2) == 1 && S.cell == 2 && S.ring_accum == 0);
    S.cell = 0;
    xpect(&c, "backwards from 'A' wraps to DONE, the last cell",
          search_ring_move(&S, -4) == -1 && S.cell == SEARCH_CELL_DONE);
    xpect(&c, "...and forwards from DONE wraps back to 'A'",
          search_ring_move(&S, 4) == 1 && S.cell == 0);
    /* A flick is capped like every other list's, so the ring cannot be thrown
     * half way round the alphabet by one event. */
    S.cell = 0; S.ring_accum = 0;
    xpect(&c, "a flick is capped at two cells per event",
          search_ring_move(&S, 100) == 2 && S.cell == 2);

    /* ---- 2. what a key does to the query --------------------------------- */
    search_reset(&S);
    S.cell = 18;                                        /* 's' */
    xpect(&c, "SELECT on a letter types it, lower case, and asks for a rescan",
          search_key(&S, SEARCH_KEY_SELECT) == SEARCH_ACT_RESCAN &&
          S.qlen == 1 && S.query[0] == 's');
    S.cell = 30;                                        /* '4' */
    xpect(&c, "digits type too", search_key(&S, SEARCH_KEY_SELECT) == SEARCH_ACT_RESCAN &&
          S.query[1] == '4');
    xpect(&c, "LEFT is backspace",
          search_key(&S, SEARCH_KEY_LEFT) == SEARCH_ACT_RESCAN && S.qlen == 1);
    xpect(&c, "RIGHT is the space bar",
          search_key(&S, SEARCH_KEY_RIGHT) == SEARCH_ACT_RESCAN &&
          S.qlen == 2 && S.query[1] == ' ');
    xpect(&c, "a double space is not typed",
          search_key(&S, SEARCH_KEY_RIGHT) == SEARCH_ACT_NONE && S.qlen == 2);
    S.cell = SEARCH_CELL_SPACE;
    xpect(&c, "...however it is asked for",
          search_key(&S, SEARCH_KEY_SELECT) == SEARCH_ACT_NONE && S.qlen == 2);
    S.cell = SEARCH_CELL_DEL;
    xpect(&c, "DEL is backspace as well",
          search_key(&S, SEARCH_KEY_SELECT) == SEARCH_ACT_RESCAN && S.qlen == 1);
    search_reset(&S);
    xpect(&c, "a leading space is not typed",
          search_key(&S, SEARCH_KEY_RIGHT) == SEARCH_ACT_NONE && S.qlen == 0);
    S.cell = SEARCH_CELL_DEL;
    xpect(&c, "DEL on an empty query does nothing",
          search_key(&S, SEARCH_KEY_SELECT) == SEARCH_ACT_NONE && S.qlen == 0);
    xpect(&c, "LEFT on an empty query does nothing",
          search_key(&S, SEARCH_KEY_LEFT) == SEARCH_ACT_NONE && S.qlen == 0);
    /* The cap. Typing into a full query must be refused, not truncated into
     * a buffer that has no room for the NUL. */
    search_reset(&S);
    S.cell = 0;
    for (int i = 0; i < SEARCH_QUERY_MAX + 4; i++) (void)search_key(&S, SEARCH_KEY_SELECT);
    xpect(&c, "the query stops at SEARCH_QUERY_MAX and stays terminated",
          S.qlen == SEARCH_QUERY_MAX && S.query[SEARCH_QUERY_MAX] == '\0' &&
          search_key(&S, SEARCH_KEY_SELECT) == SEARCH_ACT_NONE);
    xpect(&c, "MENU in PICK leaves the screen",
          search_key(&S, SEARCH_KEY_MENU) == SEARCH_ACT_POP);

    /* ---- 3. the match ---------------------------------------------------- */
    search_reset(&S);
    search_scan(&S, &SRC);
    xpect(&c, "an empty query has no hits at all", S.nhit == 0 && S.total == 0);

    fresh("sun");
    /* Prefix hits first, in source order (artist, album, playlist, song), then
     * the one that only CONTAINS the word. This whole assertion is the
     * feature: it is what stops six songs burying the artist. */
    xpect(&c, "\"sun\": seven prefix hits, few-to-many, then one substring",
          S.nhit == 8 && S.total == 8 &&
          hit_is(0, SEARCH_T_ARTIST,   4, SEARCH_RANK_PREFIX) &&
          hit_is(1, SEARCH_T_ALBUM,    0, SEARCH_RANK_PREFIX) &&
          hit_is(2, SEARCH_T_PLAYLIST, 0, SEARCH_RANK_PREFIX) &&
          hit_is(3, SEARCH_T_SONG,     3, SEARCH_RANK_PREFIX) &&
          hit_is(4, SEARCH_T_SONG,     4, SEARCH_RANK_PREFIX) &&
          hit_is(5, SEARCH_T_SONG,     5, SEARCH_RANK_PREFIX) &&
          hit_is(6, SEARCH_T_SONG,     6, SEARCH_RANK_PREFIX) &&
          hit_is(7, SEARCH_T_SONG,     7, SEARCH_RANK_SUBSTR));
    /* The ring can only type lower case, so "case-blind" is a claim about the
     * NAMES: an album tagged in capitals has to be reachable. */
    fresh("austin");
    xpect(&c, "an upper-case name is reached by a lower-case query",
          S.nhit == 1 && hit_is(0, SEARCH_T_ALBUM, 2, SEARCH_RANK_PREFIX));
    fresh("sunflower ");
    xpect(&c, "a trailing space still matches what it should",
          S.nhit == 2 && hit_is(0, SEARCH_T_ARTIST, 4, SEARCH_RANK_PREFIX) &&
          hit_is(1, SEARCH_T_SONG, 5, SEARCH_RANK_PREFIX));

    /* The sort key, not the name: The Kid LAROI is filed under K, so a search
     * for "kid" must rank it as a prefix and not below a song. */
    fresh("kid");
    xpect(&c, "a match at the start of the SORT key is a prefix match",
          S.nhit == 2 &&
          hit_is(0, SEARCH_T_ARTIST, 1, SEARCH_RANK_PREFIX) &&
          hit_is(1, SEARCH_T_SONG,   0, SEARCH_RANK_SUBSTR));
    fresh("the kid");
    xpect(&c, "...and the name itself still matches, as a prefix too",
          S.nhit == 1 && hit_is(0, SEARCH_T_ARTIST, 1, SEARCH_RANK_PREFIX));

    /* The fold, from the query side. */
    fresh("elan");
    xpect(&c, "an unaccented query reaches an accented name",
          S.nhit == 1 && hit_is(0, SEARCH_T_ALBUM, 3, SEARCH_RANK_PREFIX));
    fresh("its over");
    xpect(&c, "a query crosses an apostrophe the ring cannot type",
          S.nhit == 1 && hit_is(0, SEARCH_T_SONG, 2, SEARCH_RANK_PREFIX));
    fresh("hollywoods");
    xpect(&c, "...in an album name as well",
          S.nhit == 1 && hit_is(0, SEARCH_T_ALBUM, 1, SEARCH_RANK_PREFIX));
    fresh("zzz");
    xpect(&c, "a query that matches nothing says so, and is not an error",
          S.nhit == 0 && S.total == 0);

    /* ---- 4. the cap ------------------------------------------------------ */
    {
        search_source_t big = { { 0, 0, 0, 6000 }, big_name, 0 };
        search_reset(&S);
        type_query("sun");
        search_scan(&S, &big);
        xpect(&c, "6000 prefix hits are capped at 200, and total says 6000",
              S.nhit == SEARCH_MAX_HITS && S.total == 6000 &&
              S.hit[SEARCH_MAX_HITS - 1].type == SEARCH_T_SONG);
        /* The same, with every hit in the OTHER rank: the overflow array has
         * to be bounded too, and the total has to keep counting past it. */
        search_source_t big2 = { { 0, 0, 0, 6000 }, big_name_mid, 0 };
        search_reset(&S);
        type_query("sun");
        search_scan(&S, &big2);
        xpect(&c, "6000 substring hits are capped the same way",
              S.nhit == SEARCH_MAX_HITS && S.total == 6000 &&
              S.hit[0].rank == SEARCH_RANK_SUBSTR);
    }

    /* ---- 5. PICK -> RESULTS and back ------------------------------------- */
    fresh("zzz");
    S.cell = SEARCH_CELL_DONE;
    xpect(&c, "DONE with no hits stays in PICK",
          search_key(&S, SEARCH_KEY_SELECT) == SEARCH_ACT_NONE &&
          S.mode == SEARCH_PICK);
    fresh("sun");
    S.cell = SEARCH_CELL_DONE;
    xpect(&c, "DONE with hits hands the wheel to the results, at the top",
          search_key(&S, SEARCH_KEY_SELECT) == SEARCH_ACT_TO_RESULTS &&
          S.mode == SEARCH_RESULTS && S.sel == 0);
    xpect(&c, "SELECT on a hit is the caller's to act on",
          search_key(&S, SEARCH_KEY_SELECT) == SEARCH_ACT_OPEN);
    xpect(&c, "RIGHT and LEFT keep their global meaning in RESULTS",
          search_key(&S, SEARCH_KEY_RIGHT) == SEARCH_ACT_NONE &&
          search_key(&S, SEARCH_KEY_LEFT)  == SEARCH_ACT_NONE &&
          S.qlen == 3);
    xpect(&c, "MENU in RESULTS returns to the ring with the query intact",
          search_key(&S, SEARCH_KEY_MENU) == SEARCH_ACT_TO_PICK &&
          S.mode == SEARCH_PICK && S.qlen == 3 && S.nhit == 8);
    /* A rescan resets the selection: the row that was under the bar is not the
     * same row once the hits change. */
    S.mode = SEARCH_RESULTS;
    S.sel  = 5;
    search_scan(&S, &SRC);
    xpect(&c, "a rescan puts the selection back at the top", S.sel == 0);

    /* ---- 6. the painter: PICK -------------------------------------------- */
    fresh("sun");
    paint();
    xpect(&c, "PICK: nothing is drawn in the status strip's band",
          count_not(0, 0, LCD_WIDTH, STATUS_H, LINEN_SURFACE) == 0);
    xpect(&c, "PICK: nothing is drawn in the scrollbar's column",
          count_not(UI_SB_X, 0, LCD_WIDTH - UI_SB_X, LCD_HEIGHT,
                    LINEN_SURFACE) == 0);
    xpect(&c, "PICK: the query plate is a plate, and only there",
          count_in(SEARCH_PLATE_X, SEARCH_PLATE_Y, SEARCH_PLATE_W,
                   SEARCH_PLATE_H, LINEN_PLATE) > SEARCH_PLATE_W * 4 &&
          count_outside(SEARCH_PLATE_X, SEARCH_PLATE_Y, SEARCH_PLATE_W,
                        SEARCH_PLATE_H, LINEN_PLATE) == 0);
    {
        /* The cursor cell: an ink pill, centred, with the glyph knocked out of
         * it in the surface colour. */
        int cw = SEARCH_RING_CELL_W;
        int cx = LCD_WIDTH / 2 - cw / 2;
        int px = cx + 1, pw = cw - 2;
        xpect(&c, "PICK: the cursor is an ink pill with a surface glyph in it",
              count_in(px, SEARCH_RING_Y + 1, pw, SEARCH_RING_H - 2,
                       LINEN_INK) > pw * 10 &&
              count_in(px, SEARCH_RING_Y + 1, pw, SEARCH_RING_H - 2,
                       LINEN_SURFACE) > 8);
        xpect(&c, "PICK: the cells either side are not pills",
              count_in(cx - cw + 1, SEARCH_RING_Y + 1, cw - 2,
                       SEARCH_RING_H - 2, LINEN_INK) < pw * 6 &&
              count_in(cx + cw + 1, SEARCH_RING_Y + 1, cw - 2,
                       SEARCH_RING_H - 2, LINEN_INK) < pw * 6);
    }
    xpect(&c, "PICK: the hairline is under the ring, inside the margins",
          count_in(SEARCH_PLATE_X, SEARCH_RULE_Y, SEARCH_PLATE_W, 1,
                   LINEN_BORDER) == SEARCH_PLATE_W &&
          count_in(0, SEARCH_RULE_Y, SEARCH_PLATE_X, 1, LINEN_BORDER) == 0);
    /* LINEN_SEL_BG is the ink, so "there is no bar" is a claim about AREA: a
     * filled bar is ~9000 pixels of it, a row of text a few hundred. */
    xpect(&c, "PICK: the preview draws rows and no selection bar",
          count_not(0, SEARCH_PICK_Y0, UI_SB_X, 32, LINEN_SURFACE) > 0 &&
          count_in(6, SEARCH_PICK_Y0 + 1, UI_SB_X - 12, 30, LINEN_SEL_BG) < 2000);
    xpect(&c, "PICK: eight hits fit, so no footer",
          count_not(140, 231, UI_SB_X - 140, 9, LINEN_SURFACE) == 0);

    /* Empty query: the hint, and no rows at all. */
    search_reset(&S);
    search_scan(&S, &SRC);
    paint();
    xpect(&c, "PICK: an empty query shows the hint and draws no rows",
          count_not(SEARCH_QUERY_X, SEARCH_QUERY_BASE - 8, 120, 10,
                    LINEN_PLATE) > 0 &&
          count_not(0, SEARCH_PICK_Y0, UI_SB_X, LCD_HEIGHT - SEARCH_PICK_Y0,
                    LINEN_SURFACE) == 0);

    /* A query with nothing behind it is a different state from no query. */
    fresh("zzz");
    paint();
    xpect(&c, "PICK: a query with no matches says so where the rows would be",
          count_not(0, 118, 120, 12, LINEN_SURFACE) > 0);

    /* ---- 7. the painter: RESULTS ----------------------------------------- */
    fresh("sun");
    S.mode = SEARCH_RESULTS;
    S.sel  = 0;
    paint();
    xpect(&c, "RESULTS: the status strip's band is still untouched",
          count_not(0, 0, LCD_WIDTH, STATUS_H, LINEN_SURFACE) == 0);
    xpect(&c, "RESULTS: the selected row has a bar",
          count_in(6, SEARCH_RESULTS_Y0 + 1, UI_SB_X - 12, 30,
                   LINEN_SEL_BG) > 1000);
    xpect(&c, "RESULTS: eight hits in five rows means a scrollbar",
          count_not(UI_SB_X, SEARCH_RESULTS_Y0, 3,
                    LCD_HEIGHT - SEARCH_RESULTS_Y0 - 4, LINEN_SURFACE) > 0);
    xpect(&c, "RESULTS: the ring and its hairline are gone",
          count_in(SEARCH_PLATE_X, SEARCH_RULE_Y, SEARCH_PLATE_W, 1,
                   LINEN_BORDER) == 0);
    /* The plate is now a record of what was searched, not a field being
     * edited: muted ink, and no caret — which is the only ink in it in PICK. */
    xpect(&c, "RESULTS: the query plate stays, muted, with no caret",
          count_outside(SEARCH_PLATE_X, SEARCH_PLATE_Y, SEARCH_PLATE_W,
                        SEARCH_PLATE_H, LINEN_PLATE) == 0 &&
          count_in(SEARCH_PLATE_X, SEARCH_PLATE_Y, SEARCH_PLATE_W,
                   SEARCH_PLATE_H, LINEN_MUTED) > 0 &&
          count_in(SEARCH_PLATE_X, SEARCH_PLATE_Y, SEARCH_PLATE_W,
                   SEARCH_PLATE_H, LINEN_INK) == 0);
    {
        /* The selection moves the bar and nothing else about the geometry. */
        S.sel = 1;
        paint();
        xpect(&c, "RESULTS: the bar follows the selection, one row down",
              count_in(6, SEARCH_RESULTS_Y0 + 1, UI_SB_X - 12, 30,
                       LINEN_SEL_BG) < 2000 &&
              count_in(6, SEARCH_RESULTS_Y0 + 33, UI_SB_X - 12, 30,
                       LINEN_SEL_BG) > 1000);
    }

    /* ---- 8. the footer appears exactly when something was hidden ---------- */
    {
        search_source_t big = { { 0, 0, 0, 6000 }, big_name, 0 };
        search_reset(&S);
        type_query("sun");
        search_scan(&S, &big);
        paint();                                   /* PICK */
        xpect(&c, "PICK: a capped scan draws the footer under the preview",
              count_not(140, 231, UI_SB_X - 140, 9, LINEN_SURFACE) > 0);
        xpect(&c, "...and it still does not reach the scrollbar's column",
              count_not(UI_SB_X, 0, LCD_WIDTH - UI_SB_X, LCD_HEIGHT,
                        LINEN_SURFACE) == 0);
        S.mode = SEARCH_RESULTS;
        S.sel  = 0;
        paint();
        /* In RESULTS the rows reach the bottom of the panel, so the footer
         * goes on the right of the plate — which is empty there when nothing
         * was hidden, and is the differential this asserts. */
        int with_footer = count_not(150, SEARCH_QUERY_BASE - 10, 150, 12,
                                    LINEN_PLATE);
        fresh("sun");
        S.mode = SEARCH_RESULTS;
        S.sel  = 0;
        paint();
        int no_footer = count_not(150, SEARCH_QUERY_BASE - 10, 150, 12,
                                  LINEN_PLATE);
        xpect(&c, "RESULTS: the footer moves into the plate, and only when "
                  "the cap hid something",
              with_footer > 0 && no_footer == 0);
    }

    /* ---- 9. what a PLAY tap has under the cursor -------------------------- */
    /* kernel/main.c hands this straight to ui/gesture.h's arbiter: the row
     * count it should see, and whether the row IS a track or NAMES a queue.
     * The picker answers 0 because a text field has no row under the cursor,
     * which is how "PLAY is still pause/resume here" is said. */
    {
        int is_track = 99;
        fresh("sun");
        xpect(&c, "PLAY in the picker has no row under the cursor",
              search_play_rows(&S, &is_track) == 0 && is_track == 0);
        S.mode = SEARCH_RESULTS;
        S.sel  = 0;                                   /* the artist hit    */
        xpect(&c, "PLAY on an artist result: a row that NAMES a queue",
              search_play_rows(&S, &is_track) == S.nhit && is_track == 0);
        S.sel = 1;                                    /* the album hit     */
        xpect(&c, "...on an album result, likewise",
              search_play_rows(&S, &is_track) == S.nhit && is_track == 0);
        S.sel = 2;                                    /* the playlist hit  */
        xpect(&c, "...and on a playlist result",
              search_play_rows(&S, &is_track) == S.nhit && is_track == 0);
        S.sel = 3;                                    /* the first song    */
        xpect(&c, "PLAY on a song result: a row that IS a track",
              search_play_rows(&S, &is_track) == S.nhit && is_track == 1);
        xpect(&c, "...and the answer survives a NULL out-parameter",
              search_play_rows(&S, 0) == S.nhit);
        fresh("zzz");
        S.mode = SEARCH_RESULTS;
        xpect(&c, "RESULTS with nothing in it has nothing for PLAY to start",
              search_play_rows(&S, &is_track) == 0 && is_track == 0);
    }

    /* ---- 10. what a HOLD has under the cursor ----------------------------- */
    /* The same seam for the row-select arbiter (kernel/main.c rowsel_*): a
     * hold on a result adds it to On-The-Go, which only means anything for a
     * hit that names ONE TRACK. A song hit hands back its position in the
     * SORTED song order, which is the one thing main.c has to turn into a
     * record. The PICKER answers 0 rows on purpose — its SELECT types a
     * character and must act on the DOWN-EDGE like a key, never wait for a
     * release — so a press there never enters the arbitration at all. */
    {
        int addable = 99, song = 99;
        fresh("sun");
        xpect(&c, "a hold in the picker has no row: SELECT stays a keystroke",
              search_hold_rows(&S, &addable, &song) == 0 &&
              addable == 0 && song == -1);
        S.mode = SEARCH_RESULTS;
        S.sel  = 0;                                   /* the artist hit    */
        xpect(&c, "an artist result is a TITLE row: nothing to add",
              search_hold_rows(&S, &addable, &song) == S.nhit &&
              addable == 0 && song == -1);
        S.sel = 1;                                    /* the album hit     */
        xpect(&c, "...an album result likewise",
              search_hold_rows(&S, &addable, &song) == S.nhit &&
              addable == 0 && song == -1);
        S.sel = 2;                                    /* the playlist hit  */
        xpect(&c, "...and a playlist result",
              search_hold_rows(&S, &addable, &song) == S.nhit &&
              addable == 0 && song == -1);
        S.sel = 3;                                    /* the first song    */
        xpect(&c, "a song result IS addable, and says which song",
              search_hold_rows(&S, &addable, &song) == S.nhit &&
              addable == 1 && song == (int)S.hit[3].idx);
        xpect(&c, "...and the row count survives NULL out-parameters",
              search_hold_rows(&S, 0, 0) == S.nhit);
        fresh("zzz");
        S.mode = SEARCH_RESULTS;
        xpect(&c, "RESULTS with nothing in it has nothing to add either",
              search_hold_rows(&S, &addable, &song) == 0 &&
              addable == 0 && song == -1);
    }

    return xfail_done(&c);
}
