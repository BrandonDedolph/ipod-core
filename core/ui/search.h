/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/search.h — Music > Search: the character ring, the scan, the rows.
 *
 * WHY THIS FILE EXISTS
 *
 * Browsing answers "what do I have"; search answers "where is that one
 * thing", and on a 6000-song library those are different questions. Nothing
 * else on the device can reach a track whose album you cannot remember
 * without scrolling past everything either side of it.
 *
 * It is a module rather than another block of kernel/main.c because almost
 * all of it is judgement that has to be pinned: what a keystroke does to the
 * query, which of two matches comes first, what "no matches" looks like,
 * and where every pixel of an entirely new screen goes. main.c contributes
 * exactly two things it alone knows — WHAT to scan (search_source_t, over the
 * library arrays) and how a hit reads as a row (search_row_fn) — and both are
 * injected, the same seam ui/wheel.c and ui/chrome.c already use.
 *
 * THE SCAN IS LINEAR AND THAT IS FINE. One keystroke folds and substring-
 * matches every artist, album, playlist name and song title: ~150 KB of text
 * typical, 320 KB worst, with an ASCII fast path and a first-byte-filtered
 * compare. At 80 MHz that is tens of milliseconds, once per SELECT — not per
 * detent — and it happens while the user's thumb is still on the wheel. A
 * cache of folded titles would cost 288 KB of .bss to save that; it is not
 * bought until a bench says the scan is felt. kernel/main.c times every scan
 * onto the UART (`core: search 3 chars 41 hits 27 ms`) so the bench can read
 * the real number instead of this paragraph's estimate.
 *
 * DEPENDENCIES: chrome.h/text.h/palette.h/console.h/hal.h to draw, wheel.h
 * for the detent arithmetic, library/fold.h to match. No hardware, no
 * globals from main.c — the same search.c the host suite compiles.
 */

#ifndef CORE_UI_SEARCH_H
#define CORE_UI_SEARCH_H

#include <stdint.h>

/* Long enough for "rearrange my world" and short enough that the query never
 * has to wrap or scroll inside its plate. */
#define SEARCH_QUERY_MAX 24

/*
 * How many hits are kept. Past a couple of hundred the list is not an answer
 * any more, it is the library again — the honest response is to say how many
 * there are and ask for another letter, which is what the footer does. The
 * cap also bounds the two hit arrays, which are the only memory this costs.
 */
#define SEARCH_MAX_HITS  200

/* What a hit IS. The order is also the order hits of equal rank appear in:
 * few-to-many, so two hundred song hits cannot bury the one artist. */
enum { SEARCH_T_ARTIST, SEARCH_T_ALBUM, SEARCH_T_PLAYLIST, SEARCH_T_SONG,
       SEARCH_T_COUNT };

/* Rank: a match at the start of the name (or at the start of an artist's
 * sort key, past "The ") beats a match anywhere inside it. */
enum { SEARCH_RANK_PREFIX, SEARCH_RANK_SUBSTR };

/* PICK types the query; RESULTS drives the hits. */
enum { SEARCH_PICK, SEARCH_RESULTS };

/* `idx` indexes the source list for `type` — and for SEARCH_T_SONG it is the
 * SORTED position, not the song record, so song hits arrive in title order
 * and main.c's lookup is one array step. */
typedef struct { uint16_t idx; uint8_t type; uint8_t rank; } search_hit_t;

/*
 * What the model scans. Injected so the host suite can hand it a table of
 * names instead of a library.
 *
 * `name(type, i)` returns the string that list is ORDERED by (which is also
 * the string the row shows). `artist_key_of(type, i)` returns the key it is
 * SORTED by: for artists the name past a leading "The "; for everything else
 * the name itself, which is how "this one has no separate key" is said —
 * returning the identical pointer costs the scan nothing. NULL is allowed and
 * means no list here has one.
 */
typedef struct {
    int count[SEARCH_T_COUNT];
    const char *(*name)(int type, int i);
    const char *(*artist_key_of)(int type, int i);
} search_source_t;

typedef struct {
    char         query[SEARCH_QUERY_MAX + 1];
    uint8_t      qlen, mode, cell;         /* cell: ring position           */
    int          sel, accum;               /* RESULTS selection + remainder */
    int          ring_accum;               /* PICK sub-detent remainder     */
    search_hit_t hit[SEARCH_MAX_HITS];     /* prefix hits, then substring   */
    search_hit_t sub[SEARCH_MAX_HITS];     /* substring hits, until merged  */
    int          nhit;                     /* rows in hit[]                 */
    int          total;                    /* matches, including past the cap */
} search_t;

/*
 * The ring: A-Z, 0-9, then three word cells. Upper case on screen, lower case
 * in the query — the fold is lower, and the plate upper-cases for legibility
 * at bold 13.
 */
#define SEARCH_RING_N 39
enum { SEARCH_CELL_SPACE = 36, SEARCH_CELL_DEL = 37, SEARCH_CELL_DONE = 38 };

/* Keys, as search_key() takes them. */
enum { SEARCH_KEY_SELECT, SEARCH_KEY_MENU, SEARCH_KEY_LEFT, SEARCH_KEY_RIGHT };

/* What the caller must do about a key. */
enum {
    SEARCH_ACT_NONE,         /* nothing changed                              */
    SEARCH_ACT_RESCAN,       /* the query changed: call search_scan()        */
    SEARCH_ACT_TO_RESULTS,   /* mode is now RESULTS                          */
    SEARCH_ACT_TO_PICK,      /* mode is now PICK                             */
    SEARCH_ACT_POP,          /* leave the screen                             */
    SEARCH_ACT_OPEN          /* act on s->hit[s->sel]                        */
};

/* Sizes search_row_fn writes into. `title`/`sub` hold a NAME_MAX name plus an
 * eyebrow; `right` a duration. */
#define SEARCH_ROW_MAX   96
#define SEARCH_RIGHT_MAX 16

/* Fill one row for `h`. `sub` and `right` may be left empty (""); `*greyed`
 * marks a hit that cannot be acted on (a song the disk no longer has). */
typedef void (*search_row_fn)(const search_hit_t *h, char *title, char *sub,
                              char *right, int *greyed);

/* ---------- Geometry ---------------------------------------------------
 * Public because docs/screens/render.py draws the gallery stills from these
 * numbers and the tests read pixels at them; the panel is 320x240.
 */
#define SEARCH_PLATE_X      12      /* the query plate                       */
#define SEARCH_PLATE_Y      44
#define SEARCH_PLATE_W      296
#define SEARCH_PLATE_H      26
#define SEARCH_QUERY_X      20      /* query text / hint, left edge          */
#define SEARCH_QUERY_BASE   62      /* ...its baseline                       */
#define SEARCH_RING_Y       76      /* the character ring's band             */
#define SEARCH_RING_H       22
#define SEARCH_RING_CELL_W  20      /* a letter or digit cell                */
#define SEARCH_RING_WORD_W  40      /* SPACE / DEL / DONE                    */
#define SEARCH_RING_WINDOW  15      /* cells drawn, centred on the cursor    */
#define SEARCH_RULE_Y       102     /* hairline under the ring               */
#define SEARCH_PICK_Y0      104     /* the results PREVIEW, in PICK          */
#define SEARCH_PICK_ROWS    4       /* ...104 + 4*32 = 232, clear of the     */
#define SEARCH_FOOTER_BASE  238     /* footer's band (231..238)              */
#define SEARCH_RESULTS_Y0   76      /* the results list, in RESULTS: 5 rows  */
#define SEARCH_RESULTS_ROWS 5       /* reach 236, so the footer moves into   */
                                    /* the query plate instead               */

/* ---------- The model --------------------------------------------------- */

/* Empty query, no hits, PICK mode, the cursor on 'A'. */
void search_reset(search_t *s);

/* A wheel event in PICK: moves the ring cursor, wrapping, one cell per detent
 * with no acceleration (the ring is 39 cells; there is nothing to accelerate
 * across). Returns the cells moved, so the caller can click. */
int  search_ring_move(search_t *s, int8_t delta);

/* A key. Returns one of SEARCH_ACT_*. */
int  search_key(search_t *s, int key);

/* Rerun the match over `src`. Fills hit[]/nhit/total; an empty query has no
 * hits. Leaves mode alone — the caller decides what an empty result means. */
void search_scan(search_t *s, const search_source_t *src);

/*
 * What a PLAY tap has under the cursor, for kernel/main.c's gesture policy
 * (ui/gesture.h). Returns the row count the arbiter should see — 0 in PICK,
 * because a text field has no row under the cursor at all and PLAY there is
 * the transport it is everywhere else — and sets `*is_track` when the
 * selected hit IS a track rather than something that NAMES a queue, which is
 * the same distinction Songs and Albums draw.
 */
int  search_play_rows(const search_t *s, int *is_track);

/*
 * Rows the ROW-SELECT arbiter should see, for the same reason. A hold on a
 * result adds it to On-The-Go, which only means anything for a hit that names
 * ONE TRACK — an artist, an album or a playlist hit is a title row, and
 * holding Select on those does nothing here exactly as it does nothing on
 * Artists or Playlists. 0 in PICK: the ring is a text field, its SELECT types
 * a character, and a press there must act on the down-edge like a key and
 * never wait for a release.
 *
 * `*addable` is set when the selected hit is a track; `*song_idx` is its
 * position in the caller's SORTED song order (search_hit_t.idx for a song
 * hit), or -1. Split out of the caller so the rule has one home and the host
 * can assert it — kernel/main.c is not host-built.
 */
int  search_hold_rows(const search_t *s, int *addable, int *song_idx);

/* ---------- The screen -------------------------------------------------- */

/* Draw the screen below the status strip (which the caller paints, along with
 * the surface clear). Touches nothing above the header. */
void search_render(const search_t *s, search_row_fn row_fill);

#endif /* CORE_UI_SEARCH_H */
