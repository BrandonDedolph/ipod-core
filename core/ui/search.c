/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/search.c — Music > Search: the character ring, the scan, the rows.
 * See search.h for why the scan is a plain linear walk and why main.c hands
 * this file the library through two function pointers instead of headers.
 */

#include "search.h"

#include "chrome.h"
#include "text.h"
#include "wheel.h"                 /* WHEEL_CLICKS_PER_ITEM / WHEEL_MAX_DELTA */
#include "../library/fold.h"
#include "../kernel/console.h"
#include "../hal/hal.h"            /* LCD_WIDTH / LCD_HEIGHT */

/* The longest name any source can hand over is a NAME_MAX display name, and
 * the fold never grows a string, so this bounds it with room to spare; a
 * truncated name simply stops matching past its tail. */
#define SEARCH_FOLD_MAX 128

/* ---------- small local helpers ---------------------------------------- */

/* Unsigned decimal into `dst` (needs 11 bytes), returns the length. Local so
 * this file needs nothing from library/names.h (and through it player.h). */
static int dec(char *dst, unsigned v)
{
    char nb[10];
    int n = 0;
    do { nb[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    for (int i = 0; i < n; i++) dst[i] = nb[n - 1 - i];
    dst[n] = '\0';
    return n;
}

/* Append `s` at `n` in a buffer of `cap` bytes (incl. NUL); returns the new
 * length. */
static int cat(char *dst, int n, int cap, const char *s)
{
    while (*s && n < cap - 1) dst[n++] = *s++;
    dst[n] = '\0';
    return n;
}

/*
 * First index in `h` where `n` occurs, or -1. The first-byte skip is what
 * keeps a 6000-title scan honest: most titles never get past it, so the inner
 * compare runs a handful of times per name rather than once per position.
 */
static int substr_at(const unsigned char *h, int hn,
                     const unsigned char *n, int nn)
{
    if (nn <= 0 || nn > hn) {
        return -1;
    }
    unsigned char c0 = n[0];
    for (int i = 0; i <= hn - nn; i++) {
        if (h[i] != c0) continue;
        int k = 1;
        while (k < nn && h[i + k] == n[k]) k++;
        if (k == nn) return i;
    }
    return -1;
}

/* The character a ring cell types. Lower case: that is what the fold emits,
 * so the query is already in the matched alphabet. */
static char ring_char(int cell)
{
    if (cell < 26) return (char)('a' + cell);
    if (cell < 36) return (char)('0' + cell - 26);
    return ' ';                                  /* SEARCH_CELL_SPACE */
}

/* ...and the character it SHOWS, which is upper case. */
static char ring_glyph(int cell)
{
    if (cell < 26) return (char)('A' + cell);
    return (char)('0' + cell - 26);
}

static const char *ring_word(int cell)
{
    if (cell == SEARCH_CELL_SPACE) return "SPACE";
    if (cell == SEARCH_CELL_DEL)   return "DEL";
    return "DONE";
}

static int ring_cell_w(int cell)
{
    return (cell >= SEARCH_CELL_SPACE) ? SEARCH_RING_WORD_W : SEARCH_RING_CELL_W;
}

/* ---------- the model --------------------------------------------------- */

void search_reset(search_t *s)
{
    s->query[0]  = '\0';
    s->qlen      = 0;
    s->mode      = SEARCH_PICK;
    s->cell      = 0;                            /* on 'A' */
    s->sel       = 0;
    s->accum     = 0;
    s->ring_accum = 0;
    s->nhit      = 0;
    s->total     = 0;
}

/*
 * A leading space and a double space are dropped rather than typed. Neither
 * can ever change which names match — the fold keeps spaces, so " sun" and
 * "sun  flower" simply match nothing — and a query that silently stops
 * matching because of an invisible character is the worst failure this screen
 * has available. Returns 1 when the query changed.
 */
static int query_append(search_t *s, char ch)
{
    if (s->qlen >= SEARCH_QUERY_MAX) {
        return 0;
    }
    if (ch == ' ' && (s->qlen == 0 || s->query[s->qlen - 1] == ' ')) {
        return 0;
    }
    s->query[s->qlen++] = ch;
    s->query[s->qlen]   = '\0';
    return 1;
}

static int query_backspace(search_t *s)
{
    if (s->qlen == 0) {
        return 0;
    }
    s->query[--s->qlen] = '\0';
    return 1;
}

int search_ring_move(search_t *s, int8_t delta)
{
    /* The same detent arithmetic every list uses, minus the acceleration: the
     * ring is 39 cells around, so there is nothing to accelerate across, and a
     * ring that moved four cells a detent would be unaimable. */
    int d = delta;
    if (d >  WHEEL_MAX_DELTA) d =  WHEEL_MAX_DELTA;
    if (d < -WHEEL_MAX_DELTA) d = -WHEEL_MAX_DELTA;
    s->ring_accum += d;
    int move = s->ring_accum / WHEEL_CLICKS_PER_ITEM;
    s->ring_accum -= move * WHEEL_CLICKS_PER_ITEM;
    if (move == 0) {
        return 0;
    }
    int cell = ((int)s->cell + move) % SEARCH_RING_N;
    if (cell < 0) cell += SEARCH_RING_N;
    s->cell = (uint8_t)cell;
    return move;
}

int search_key(search_t *s, int key)
{
    if (s->mode == SEARCH_RESULTS) {
        switch (key) {
        case SEARCH_KEY_SELECT:
            return (s->nhit > 0) ? SEARCH_ACT_OPEN : SEARCH_ACT_NONE;
        case SEARCH_KEY_MENU:
            /* Back to the ring with the query and the hits intact: "not quite
             * it, one more letter" is the common case, and retyping four
             * characters on a wheel is not a thing to ask twice. */
            s->mode = SEARCH_PICK;
            return SEARCH_ACT_TO_PICK;
        default:
            return SEARCH_ACT_NONE;   /* RIGHT/LEFT keep their global meaning */
        }
    }

    switch (key) {
    case SEARCH_KEY_MENU:
        return SEARCH_ACT_POP;
    case SEARCH_KEY_RIGHT:                       /* the space bar */
        return query_append(s, ' ') ? SEARCH_ACT_RESCAN : SEARCH_ACT_NONE;
    case SEARCH_KEY_LEFT:                        /* backspace */
        return query_backspace(s) ? SEARCH_ACT_RESCAN : SEARCH_ACT_NONE;
    case SEARCH_KEY_SELECT:
        if (s->cell == SEARCH_CELL_DEL) {
            return query_backspace(s) ? SEARCH_ACT_RESCAN : SEARCH_ACT_NONE;
        }
        if (s->cell == SEARCH_CELL_DONE) {
            /* DONE with nothing to show would hand the wheel to an empty
             * list, which reads as the button being broken. */
            if (s->nhit == 0) return SEARCH_ACT_NONE;
            s->mode  = SEARCH_RESULTS;
            s->sel   = 0;
            s->accum = 0;
            return SEARCH_ACT_TO_RESULTS;
        }
        return query_append(s, ring_char(s->cell)) ? SEARCH_ACT_RESCAN
                                                   : SEARCH_ACT_NONE;
    default:
        return SEARCH_ACT_NONE;
    }
}

/* Record one match. Prefix hits go straight into hit[]; substring hits wait in
 * sub[] until the scan ends, so a whole rank is collected before the next rank
 * is considered for the cap. `total` counts everything, cap or no cap — the
 * footer's whole job is to say how much was not shown. */
static void hit_add(search_t *s, int *nsub, int type, int i, int rank)
{
    s->total++;
    search_hit_t h;
    h.idx  = (uint16_t)i;
    h.type = (uint8_t)type;
    h.rank = (uint8_t)rank;
    if (rank == SEARCH_RANK_PREFIX) {
        if (s->nhit < SEARCH_MAX_HITS) s->hit[s->nhit++] = h;
    } else {
        if (*nsub < SEARCH_MAX_HITS) s->sub[(*nsub)++] = h;
    }
}

void search_scan(search_t *s, const search_source_t *src)
{
    s->nhit  = 0;
    s->total = 0;
    s->sel   = 0;
    s->accum = 0;
    int nsub = 0;
    if (s->qlen == 0 || !src || !src->name) {
        return;                       /* nothing typed: the hint, not a list */
    }

    unsigned char q[SEARCH_QUERY_MAX + 1];
    int qn = fold_ascii(s->query, q, (int)sizeof q);
    if (qn == 0) {
        return;
    }

    for (int t = 0; t < SEARCH_T_COUNT; t++) {
        int n = src->count[t];
        for (int i = 0; i < n; i++) {
            const char *nm = src->name(t, i);
            if (!nm) continue;
            unsigned char f[SEARCH_FOLD_MAX];
            int fn = fold_ascii(nm, f, (int)sizeof f);
            int at = substr_at(f, fn, q, qn);
            if (at < 0) continue;
            int rank = (at == 0) ? SEARCH_RANK_PREFIX : SEARCH_RANK_SUBSTR;
            if (rank != SEARCH_RANK_PREFIX && src->artist_key_of) {
                /* A match at the start of the SORT KEY is a prefix match too,
                 * or "kid" would rank The Kid LAROI below every song with the
                 * word in the middle of its title — on a list the artist sits
                 * at the top of, under K. Folded separately rather than by
                 * offset arithmetic: the fold DROPS characters (apostrophes),
                 * so a byte offset into the name is not an offset into the
                 * folded name. This runs only for a name that already matched
                 * somewhere, so it is bounded by the hit count, not by the
                 * library. */
                const char *k = src->artist_key_of(t, i);
                if (k && k != nm) {
                    unsigned char kf[SEARCH_FOLD_MAX];
                    int kn = fold_ascii(k, kf, (int)sizeof kf);
                    if (substr_at(kf, kn, q, qn) == 0) rank = SEARCH_RANK_PREFIX;
                }
            }
            hit_add(s, &nsub, t, i, rank);
        }
    }

    /* Substring hits after the prefix ones, as far as the cap allows. */
    for (int i = 0; i < nsub && s->nhit < SEARCH_MAX_HITS; i++) {
        s->hit[s->nhit++] = s->sub[i];
    }
}

int search_hold_rows(const search_t *s, int *addable, int *song_idx)
{
    int rows = 0, add = 0, si = -1;
    if (s->mode == SEARCH_RESULTS && s->nhit > 0 &&
        s->sel >= 0 && s->sel < s->nhit) {
        rows = s->nhit;
        if (s->hit[s->sel].type == SEARCH_T_SONG) {
            add = 1;
            si  = (int)s->hit[s->sel].idx;
        }
    }
    if (addable)  *addable  = add;
    if (song_idx) *song_idx = si;
    return rows;
}

int search_play_rows(const search_t *s, int *is_track)
{
    int rows  = 0;
    int track = 0;
    if (s->mode == SEARCH_RESULTS && s->nhit > 0 &&
        s->sel >= 0 && s->sel < s->nhit) {
        rows  = s->nhit;
        track = (s->hit[s->sel].type == SEARCH_T_SONG);
    }
    if (is_track) *is_track = track;
    return rows;
}

/* ---------- the screen -------------------------------------------------- */

/* One ring cell at `x`. Cells that would cross either edge of the strip are
 * not drawn at all: a half glyph reads as a rendering fault, and the right
 * edge is the scrollbar's column, which nothing but the scrollbar may touch. */
static void ring_cell_render(int cell, int x, int cursor)
{
    int w = ring_cell_w(cell);
    if (x < 0 || x + w > UI_SB_X) {
        return;
    }
    uint16_t ink = LINEN_INK;
    if (cursor) {
        ui_round_rect(x + 1, SEARCH_RING_Y + 1, w - 2, SEARCH_RING_H - 2, 4,
                      LINEN_INK);
        ink = LINEN_SURFACE;
    }
    if (cell >= SEARCH_CELL_SPACE) {
        const char *word = ring_word(cell);
        int ww = text_width(word, FONT_SMALL);
        ui_text(x + (w - ww) / 2, SEARCH_RING_Y + 15, word, FONT_SMALL,
                cursor ? ink : LINEN_MUTED2);
    } else {
        char g[2] = { ring_glyph(cell), '\0' };
        int gw = text_width(g, FONT_HEADER);
        ui_text(x + (w - gw) / 2, SEARCH_RING_Y + 16, g, FONT_HEADER, ink);
    }
}

/* The strip: SEARCH_RING_WINDOW cells centred on the cursor, wrapping both
 * ways, so the alphabet runs past a cursor that never leaves the middle. */
static void ring_render(const search_t *s)
{
    int cur  = s->cell;
    int half = SEARCH_RING_WINDOW / 2;
    int cx   = LCD_WIDTH / 2 - ring_cell_w(cur) / 2;

    int x = cx;
    for (int k = 1; k <= half; k++) {
        int cell = (cur - k + SEARCH_RING_N) % SEARCH_RING_N;
        x -= ring_cell_w(cell);
        ring_cell_render(cell, x, 0);
    }
    ring_cell_render(cur, cx, 1);
    x = cx + ring_cell_w(cur);
    for (int k = 1; k <= half; k++) {
        int cell = (cur + k) % SEARCH_RING_N;
        ring_cell_render(cell, x, 0);
        x += ring_cell_w(cell);
    }
}

/* Returns the pen after the query text, or 0 when the plate shows its hint. */
static int plate_render(const search_t *s)
{
    ui_round_rect(SEARCH_PLATE_X, SEARCH_PLATE_Y, SEARCH_PLATE_W,
                  SEARCH_PLATE_H, 4, LINEN_PLATE);
    if (s->qlen == 0) {
        ui_text(SEARCH_QUERY_X, SEARCH_QUERY_BASE, "Type with the wheel",
                FONT_SMALL, LINEN_MUTED2);
        return 0;
    }
    /* Upper case as typed: the query is stored lower case because that is the
     * alphabet the fold matches in, but a lower-case line at bold 13 inside a
     * plate reads as a placeholder rather than as what you entered. */
    char up[SEARCH_QUERY_MAX + 1];
    int n = 0;
    for (; n < s->qlen; n++) {
        char ch = s->query[n];
        up[n] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    up[n] = '\0';
    int pen = ui_text(SEARCH_QUERY_X, SEARCH_QUERY_BASE, up, FONT_HEADER,
                      s->mode == SEARCH_PICK ? LINEN_INK : LINEN_MUTED);
    if (s->mode == SEARCH_PICK) {
        console_fill_rect(pen + 2, SEARCH_QUERY_BASE - 12, 1, 15, LINEN_INK);
        return pen + 3;
    }
    return pen;
}

/*
 * "200 of 1234 - keep typing" — only when the cap actually hid something, and
 * saying so is the whole point: a list that silently stops at 200 is a list
 * that lies about the library.
 *
 * In PICK it sits under the preview, which the layout stops short of the
 * panel edge for it. In RESULTS the rows reach the bottom, so it goes on the
 * right of the query plate, which in that mode carries no caret and is only a
 * record of what was searched — but only if the query has left it room, since
 * what was typed matters more than the count. `pen` is where the query text
 * ended (0 when the plate is showing its hint).
 */
static void footer_render(const search_t *s, int pen)
{
    if (s->total <= s->nhit) {
        return;
    }
    char t[48];
    int n = dec(t, (unsigned)s->nhit);
    n = cat(t, n, (int)sizeof t, " of ");
    n += dec(t + n, (unsigned)s->total);
    n = cat(t, n, (int)sizeof t, " " UI_GLYPH_MIDDOT " keep typing");
    int w = text_width(t, FONT_SMALL);
    int x = LCD_WIDTH - 12 - w;
    if (s->mode == SEARCH_RESULTS) {
        if (x < pen + 8) return;                 /* the query owns the plate */
        ui_text(x, SEARCH_QUERY_BASE, t, FONT_SMALL, LINEN_MUTED2);
        return;
    }
    ui_text(x, SEARCH_FOOTER_BASE, t, FONT_SMALL, LINEN_MUTED2);
}

static void rows_render(const search_t *s, search_row_fn row_fill)
{
    int results = (s->mode == SEARCH_RESULTS);
    int y0      = results ? SEARCH_RESULTS_Y0   : SEARCH_PICK_Y0;
    int visible = results ? SEARCH_RESULTS_ROWS : SEARCH_PICK_ROWS;

    if (s->nhit == 0) {
        /* An empty query is not a failed search: the plate says what to do. */
        if (s->qlen > 0) {
            ui_text(14, 126, "No matches", FONT_ROW, LINEN_MUTED);
        }
        return;
    }

    int top = results ? ui_scroll_window(s->sel, s->nhit, visible) : 0;
    for (int r = 0; r < visible; r++) {
        int i = top + r;
        if (i >= s->nhit) break;
        char title[SEARCH_ROW_MAX], sub[SEARCH_ROW_MAX], right[SEARCH_RIGHT_MAX];
        int greyed = 0;
        title[0] = sub[0] = right[0] = '\0';
        if (row_fill) {
            row_fill(&s->hit[i], title, sub, right, &greyed);
        }
        ui_list_row(y0, r, title, sub[0] ? sub : 0, right, 0 /*chevron*/,
                    results && i == s->sel, greyed, 0 /*chip*/,
                    1 /*title_priority*/, ROW_H2);
    }
    if (results) {
        ui_scrollbar(y0, top, visible, s->nhit);
    }
}

void search_render(const search_t *s, search_row_fn row_fill)
{
    char right[16];
    right[0] = '\0';
    if (s->mode == SEARCH_RESULTS && s->nhit > 0) {
        int n = dec(right, (unsigned)(s->sel + 1));
        n = cat(right, n, (int)sizeof right, " / ");
        (void)dec(right + n, (unsigned)s->nhit);
    } else if (s->nhit > 0) {
        int n = dec(right, (unsigned)s->nhit);
        (void)cat(right, n, (int)sizeof right, s->nhit == 1 ? " hit" : " hits");
    }
    ui_header("Search", right, 1 /*back chevron*/);

    int pen = plate_render(s);
    if (s->mode == SEARCH_PICK) {
        ring_render(s);
        console_fill_rect(SEARCH_PLATE_X, SEARCH_RULE_Y,
                          LCD_WIDTH - 2 * SEARCH_PLATE_X, 1, LINEN_BORDER);
    }
    rows_render(s, row_fill);
    footer_render(s, pen);
}
