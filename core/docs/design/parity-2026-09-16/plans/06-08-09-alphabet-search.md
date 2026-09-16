# Plan 06/08/09 — alphabet indicator, alphabet steering, Search

One plan, three stages that build on each other. All paths below are under
`/home/brando/Projects/ipod_theme/core/` unless they start with `docs/`,
`README.md`, `STATUS.md`, `PLAN.md` or `design_reference/` (repo root).
Nothing here can be verified on the device in this job; every stage's
acceptance is host suites + `make hw && make verify-hw`, and every
user-visible change lands in STATUS.md as **UNFLASHED** with a bench list.

## Summary

- **Stage A — the letter on every long list.** A new host-testable module
  `ui/letterindex.c` turns "the sort key's initial for row i" into a per-list
  index of *runs* (`#`, `A`…`Z`, trailing `#`): first row per letter, letter
  for any row in O(log runs), a length threshold, and unsorted-list
  detection. `kernel/main.c`'s `list_initial_at()` stops being Songs-only and
  answers for Songs / a genre's songs / an artist's All Songs / Artists /
  Albums (all, and one artist's) / Playlists / Genres from that index. The
  existing 66×66 plate (`az_overlay_render`) is unchanged and simply appears
  wherever the index is valid. Two library fixes ride along because the
  index exposes them: `g_album_key[]` is not permuted with `g_albums[]` after
  the sort (stale keys), and Genres are listed in intern order, not A→Z.
- **Stage B — steering.** `ui/wheel.c` gets a second seam,
  `wheel_set_letter_step()`, so a letter detent lands on the index's next
  run head instead of walking `initial_at()` row by row, and letter mode
  now *survives* a pause shorter than the plate's hold (1.2 s): while the
  letter is up the wheel steps letters; lift for longer and the next detent
  is a row again. `scr_push`/`scr_pop` reset the gesture so a spin cannot
  leak into the next screen.
- **Stage C — Music › Search.** `ui/search.c` (model + painter, host-tested)
  and `library/fold.c` (case/diacritic/quote folding that is *consistent
  with* but separate from `name_hash`, which must not change). A wheel-driven
  character ring, Select enters, RIGHT = space, LEFT = backspace, DONE hands
  the wheel to the results; substring match over song titles, artist names,
  album titles and playlist names; prefix hits first, capped at 200 with a
  "more" footer; rows carry a type eyebrow; Select plays the song in its
  album / opens the artist / opens the album / opens the playlist; MENU comes
  back to Search. A full scan per keystroke is ~25–55 ms on this CPU, so no
  cache is needed. The Music menu grows to 9 rows, which means
  `menu_render_list` finally has to scroll.

RAM: bss is 12,177,704 B of a 14 MiB budget (2.5 MB free); the three stages
add ~2.5 KB of .bss and ~8–10 KB of .text.

## Current behaviour (file:line)

Wheel / locator:
- `ui/wheel.h:53-68` — `WHEEL_AZ_VEL 3` (letter mode latches at the speed the
  plate appears), `WHEEL_IDLE_US 200000` (a gap longer than this is a new
  gesture: velocity 1, letters off), `WHEEL_AZ_HOLD_LETTER 1200000` (plate
  stays up 1.2 s after the last detent).
- `ui/wheel.c:60-113` `wheel_accel_step`: `dt > WHEEL_IDLE_US` resets
  `g_wheel_letters = 0`. So today the plate can be up (1.2 s) while the wheel
  has already dropped back to rows (after 200 ms) — the next detent both
  moves one row and takes the plate down. Stage B changes this.
- `ui/wheel.c:147-179` `list_letter_step`: walks `initial_at()` row by row to
  the next/previous run head — O(rows in the letter) per step.
- `ui/wheel.c:183-249` `wheel_move`: `move != 0 && wheel_letter_mode() &&
  initial_at(sel) != 0` is the letter branch; `initial_at(sel)==0` is the
  guard that lets letterless screens fall through to rows.
- `kernel/main.c:4183-4192` `list_initial_at`: **`if (scr_cur() != SCR_SONGS)
  return 0;`** then `initial_of(g_songs[g_songview[idx]].title)`.
  `main.c:4194-4197` `list_sel_initial` (same gate). Registered at
  `main.c:7169` `wheel_set_initial_at(list_initial_at)`.
- `kernel/main.c:4209-4229` `az_overlay_render(ch)`: 66×66 rounded plate
  (border + `LINEN_PLATE`), the letter in `FONT_TITLE`, centred.
- `kernel/main.c:6757` `char az_letter = wheel_accelerating() ?
  list_sel_initial() : 0;`; `main.c:6862` the partial-repaint path is skipped
  while the plate is up or in the frame that clears it; `main.c:6899`
  `if (az_letter) az_overlay_render(az_letter);` after the list paints.
- `wheel_accel_reset()` is called only at `main.c:5909, 5934, 5964`
  (sleep/wake/backlight), never on a screen change, although `wheel.h:87`
  says "screen change".

List orders (what "initial" must mean per list):
- Songs / a genre / an artist's All Songs: `g_songview[]` is a filter over
  `g_song_sorted[]`, sorted by `title_cmp(title)` (`main.c:2461-2464,
  2474-2475, 2582-2600`). `title_cmp` (`library/names.c:200-210`) is
  byte-wise, ASCII case-folded: space/punctuation/digits (< 'A') sort before
  A, bytes > 'Z' (`[`, `_`, and every UTF-8 lead byte ≥ 0x80) sort AFTER Z.
  `initial_of` (`names.c:224-230`) skips leading spaces, uppercases, returns
  `#` for anything not A–Z. Consequence: `#` is not one contiguous run —
  "(Intro)", "1979", "#1" sit at the front, "Élan" at the back.
- Artists: `build_artists` (`main.c:1456-1505`) de-dups and insertion-sorts by
  `title_cmp(artist_key(name))` — `artist_key` (`names.c:216-222`) skips a
  leading "The ". The initial is therefore `initial_of(artist_key(name))`,
  not `initial_of(name)`: "The Kid LAROI" lives under K.
- Albums: `library_finish` (`main.c:2472-2510`) fills `g_album_key[i]` with the
  album part of `g_albums[i].folder`, sorts an order array by
  `title_cmp(g_album_key)`, and applies the permutation to `g_albums[]` in
  place — **but not to `g_album_key[]`**, which is never read again
  (`main.c:2457, 2466, 2487` are its only uses). The on-screen list
  (`g_albumview[]`, `albumview_build` `main.c:1508-1520`) is the sorted
  `g_albums` filtered by artist; when filtered, row 0 is the synthetic "All
  Songs" row (`albumlist_all_row` `main.c:1343`, `albumlist_album_at`
  `main.c:1354`).
- Playlists: `playlist_scan` insertion-sorts by `title_cmp(name)`
  (`library/playlist.c:131-140`) — initial is `initial_of(name)`.
- Genres: `genre_intern` (`main.c:1641-1655`) appends in first-seen order;
  nothing sorts `g_genres[]`. `genres_render` (`main.c:2792`) lists them in
  that order. So today Genres has no alphabetical order to locate within.
- Playlist tracks (`SCR_PLAYLIST`), an album's tracklist (`SCR_BROWSER` depth
  1), the queue, menus and Settings have no alphabetical order and must keep
  returning 0.

Screens and dispatch (where thin wiring lands):
- `main.c:3627-3630` `screen_t` enum; `main.c:3639-3661` stack (`SCR_STACK_MAX
  12`, `scr_push` bumps `g_list_epoch`).
- `main.c:3561-3573` `g_music_menu[MU_COUNT]` — 8 rows; `menu_render_list`
  (`main.c:3592-3601`) draws `i < n && i < LIST_ROWS` with NO scroll window
  and no scrollbar; `LIST_ROWS` is 8 (`ui/chrome.h:82`). A 9th row is
  invisible today.
- `main.c:3945-4040` `list_view_t` / `list_view_current` (partial repaint
  model, per screen); `main.c:4699-4725` `paint_current_screen`;
  `main.c:6869-6886` the full-render switch.
- `main.c:6003-6047` RIGHT/LEFT pre-dispatch: outside Now Playing/queue,
  RIGHT pushes Now Playing and is consumed; LEFT does nothing.
- `main.c:6052-6280` per-screen `switch (scr_cur())`: wheel → `wheel_move`,
  SELECT → action, MENU → `scr_pop`. Songs SELECT `main.c:6162-6168`
  (`library_play_song` → whole view as queue, `RESUME_KIND_SONGS`); Artists
  SELECT `6136-6149` (sets `g_artist_filter`, `albumview_build`,
  `albumlist_queue_chips`, push BROWSER); Browser SELECT `6239-6266` (depth 0
  → `browse_load` + `detail_load_meta`, depth 1 → `player_play_queue` with
  `RESUME_KIND_ALBUM`); Playlists SELECT `6198-6202` (`playlist_open`, push
  PLAYLIST); `playlists_load` `main.c:2922-2935` re-reads the folder on every
  Music › Playlists entry.
- SELECT-hold (queue view) is armed only on Now Playing (`main.c:6337`), so
  Search's SELECT is a plain press.
- Eyebrow style precedent: Now Playing's `TRACK   N     OF     M` in
  `FONT_SMALL` / `LINEN_MUTED2` with literal spaces for air (`main.c:3499-3510`).
  `UI_GLYPH_MIDDOT` exists (`ui/text.h:31`) for "SONG · Artist".
- Atlas charset (`ui/atlas/glyphmap.h`): printable ASCII + Latin-1 + `‘’“”–—…‹›`.
  No `⌫`, `←`, `␣`: the picker's space/backspace/done must be word cells.

Docs today: `docs/USER_GUIDE.md` Controls says "On the Songs list only,
spinning the wheel fast puts the selected row's first letter on screen";
`STATUS.md:658` "letter-stepping on a sustained fast spin (Songs only)";
`STATUS.md:706` and `PLAN.md:155` "Search — not implemented";
`design_reference/README.md` "What the firmware actually implements".
`docs/screens/render.py` has no plate and no search screen; its Music menu
list (`screen_music`, `MUSIC_MENU`) has 8 rows.

Size gate (`tests/scripts/check_size.sh`, `build-hw/core.size`): text 367,328 /
1,048,576; bss 12,177,704 / 14,680,064 (82%); image 12.5 MB / 30 MB.

## Design

### Stage A — alphabet indicator on every long list

**Semantics.**
- A list "has letters" when it is alphabetised by a key whose initial is
  meaningful AND it is long enough that aiming beats scrolling:
  `LETTERIDX_MIN_ROWS = 48` rows (six 8-row screens / eight 6-row screens)
  and at least `LETTERIDX_MIN_RUNS = 4` distinct runs. Rationale: at
  `WHEEL_VEL_MAX` 8 rows/detent a 48-row list is six detents end to end;
  below that a letter step (≤27 detents across the alphabet) is slower, not
  faster. Apple's 5G used ~100; ours is lower because our lists are 6–8 rows
  tall, not 9–11. Both are one `#define` in `letterindex.h` and pinned by
  the test, so retuning after a bench is a one-line change.
- "Initial" per screen is the initial of the key the list is *sorted by*:
  Songs/genre/All Songs → `initial_of(title)`; Artists →
  `initial_of(artist_key(name))`; Albums → `initial_of(album title)`;
  Playlists → `initial_of(name)`; Genres → `initial_of(genre)` once sorted.
  Rows that are not part of the order (the "All Songs" row) report 0 and
  are skipped by the index. Every other screen reports 0 (unchanged guard).
- The plate (`az_overlay_render`) is unchanged: same size, same face, same
  hold. It appears on a screen exactly when the index is valid for that
  screen and letter mode is latched.
- The index is *per screen instance*: rebuilt lazily when
  `(scr_cur(), g_dir_depth, g_list_epoch, count)` differs from what it was
  built for. `g_list_epoch` already bumps on every content rebuild and every
  push/pop, so this is one `uint32_t` compare per call.

**Data structures.** `ui/letterindex.h`:

```c
#define LETTERIDX_MAX_RUNS  40   /* #, A..Z, trailing #, + slack; more = "not sorted" */
#define LETTERIDX_MIN_ROWS  48
#define LETTERIDX_MIN_RUNS   4

typedef struct {
    uint8_t  valid;                       /* 0: this list has no letters   */
    uint8_t  n;                           /* runs                          */
    uint16_t count;                       /* rows the index was built over */
    char     letter[LETTERIDX_MAX_RUNS];  /* run initial ('#', 'A'..'Z')   */
    uint16_t first [LETTERIDX_MAX_RUNS];  /* first row of the run          */
} letteridx_t;                            /* 124 B */

typedef char (*letteridx_initial_fn)(int row);

/* Walk rows 0..count-1 once; a run starts where the initial changes. Rows
 * whose initial is 0 belong to no run (skipped, never a run head). Returns
 * ix->valid: 0 when count < MIN_ROWS, runs < MIN_RUNS, or runs would exceed
 * MAX_RUNS (an unsorted list — e.g. Genres before Stage A sorts them). */
int  letteridx_build(letteridx_t *ix, int count, letteridx_initial_fn at);
char letteridx_letter_at(const letteridx_t *ix, int row);  /* 0 if !valid or row in no run */
int  letteridx_run_of  (const letteridx_t *ix, int row);   /* -1 if none; binary search */
/* Head of the next/previous run from `sel`; the head of sel's own run when
 * stepping back from mid-run (matches list_letter_step); `sel` at either end. */
int  letteridx_step    (const letteridx_t *ix, int sel, int dir);
```

Runs, not first-row-per-letter: with `title_cmp`'s byte order, `#` occurs
both before A and after Z. A 27-slot "first row per letter" table would make
the trailing `#` group (every name starting with a non-ASCII letter)
unreachable by a letter step; runs keep both groups steppable, and the run
cap is what tells an unsorted list apart from a sorted one without a second
walk.

`kernel/main.c` wiring (thin):
- One `static letteridx_t g_letters; static struct { int scr, depth, count; uint32_t epoch; } g_letters_for;`
  and `static char screen_initial_raw(int row)` — the per-screen switch
  listed above (replaces the body of `list_initial_at`). `list_initial_at(idx)`
  becomes: `letters_ensure(); return letteridx_letter_at(&g_letters, idx);`.
  `list_sel_initial()` becomes `list_initial_at(<current sel>)` through the
  same switch as `list_view_current` (reuse `list_view_current(&v)`; `v.sel`,
  `v.count`).
- Albums: in `library_finish` swap `g_album_key[i]`/`g_album_key[j]` inside the
  existing cycle loop (`main.c:2497-2508`) so `g_album_key[ai]` is the key of
  `g_albums[ai]` after the sort. Then the album initial is
  `initial_of(g_album_key[albumlist_album_at(row)])`, O(1), and no per-row
  `split_artist_album`. (`g_album_key` is 66 KB of .bss already paid for.)
- Genres A→Z: after the song loop in `library_finish`, sort an order array
  over `g_genres[]` with `merge_sort_idx` + a `title_cmp` cmp (128 entries,
  `g_sort_tmp` scratch), permute `g_genres[]`/`g_genre_count[]` by the same
  cycle-apply, and remap every `g_songs[i].genre` through the inverse
  (`inv[old] = new`). `songview_build(genre, …)` and `resume_kind_of_view`
  take a genre *index*, so the remap must run before anything captures one;
  `library_finish` is that point. The resume record stores the song, not the
  genre index, so nothing on disk changes (check
  `tests/kernel/resume_test.c` still passes — it should, it seeds by song).
- `scr_push`/`scr_pop`: add `wheel_accel_reset()` (already promised by
  `wheel.h:87`). Without it Stage B's longer letter latch would carry a spin
  into the next screen.

**Screen layout.** Unchanged: the plate over the list. No new chrome.

**Key map.** Unchanged in A.

### Stage B — alphabet steering

**Semantics** (the manual, p.7, matched to what wheel.c already does):
1. Fast spin → letter mode latches at `WHEEL_AZ_VEL`, plate appears (as now).
2. While the plate is up, one detent = one run: forward lands on the *first
   row* of the next run (`#` → A → B … → Z → trailing `#`); backward goes to
   the head of the current run, then the head of the previous one.
3. Pausing or lifting for less than `WHEEL_AZ_HOLD_LETTER` (1.2 s — "just
   over a second", already the plate's hold and already in the guide) keeps
   letter mode: the next detent is still a letter. Pausing ≥ 1.2 s: the plate
   is gone and the next detent is one row at velocity 1.
4. Select while the plate is up acts on the selected row as usual (the row
   under the bar is the run head, which is what the user aimed at).
5. Screens without a valid index fall through to row acceleration exactly
   as today (`initial_at(sel) == 0` guard).

**Changes in `ui/wheel.c` / `wheel.h`.**
- `wheel_accel_step`: on `dt > WHEEL_IDLE_US`, if `g_wheel_letters &&
  dt < WHEEL_AZ_HOLD_LETTER` → `g_wheel_vel = 1; g_wheel_tps = 0;` keep
  `g_wheel_letters`, return 1; else the full reset as now. Document: the
  plate is the control surface, so the latch and the plate share one clock.
  `wheel_accelerating()` needs no change — it already tests letters + hold.
- New seam: `typedef int (*wheel_letter_step_fn)(int sel, int count, int dir);
  void wheel_set_letter_step(wheel_letter_step_fn fn);`. `wheel_move`'s letter
  branch calls it when set, else `list_letter_step` (the walk) — unset stays
  safe and the existing tests keep their oracle. `list_letter_step` stays
  public.
- `main.c`: `wheel_set_letter_step(list_letter_step_idx)` where
  `list_letter_step_idx` = `letters_ensure(); return letteridx_step(&g_letters, sel, dir);`.

**Data structures.** `letteridx_t` from A; no new state in wheel.c beyond the
seam pointer.

**Screen layout.** Unchanged; the plate letter now always names the run the
selection bar sits on, because the selection is a run head after every
letter detent.

**Key map.** Unchanged; only timing changes (row 3 above).

### Stage C — Music › Search

**Semantics.**
- Music menu gains `Search` after Genres (before the greyed Composers /
  Audiobooks). `SCR_SEARCH` is pushed from Music; `library_ensure(fs)` first
  (already loaded at boot); playlists are scanned once per session for the
  search (`playlists_load` if `!g_playlists_scanned`) — the disk is read-only
  while the firmware runs, so a session cache is exact; the first Search
  entry may spin the drive for the folder read, like Music › Playlists does.
- Two modes inside the screen. **PICK** (default): the wheel moves a cursor
  on a character ring; **RESULTS**: the wheel moves the selection bar on the
  hits. DONE (a ring cell) or a Select on DONE switches PICK → RESULTS when
  there is at least one hit; MENU in RESULTS returns to PICK with the query
  and hits intact; MENU in PICK pops to Music. The query is kept for the
  session (re-entering Search shows the last query and hits); cleared by a
  library reload (none exists at runtime today) — i.e. never, until reboot.
- Query: up to `SEARCH_QUERY_MAX 24` ASCII characters from the ring (A–Z
  entered as lower-case internally, shown upper-case in the plate as typed —
  see layout), digits, space. A leading space and a double space are
  ignored (they can never change a match). Backspace on an empty query does
  nothing. Every edit reruns the scan; an empty query has no hits and shows
  the hint text instead of rows.
- Matching: fold both sides through `fold_ascii()` (below); a hit is a
  substring of the folded name. Two ranks: prefix (match at 0 of the name,
  or at 0 of `artist_key(name)` for artists) then anywhere. Sources and
  order within a rank: Artists (`g_artists[].name`), Albums (`g_album_key[]`
  — the title; the artist is shown on the sub-line), Playlists
  (`g_playlists[].name`), Songs (`g_songs[g_song_sorted[i]].title`, so songs
  arrive in title order). Few-to-many so 200 song hits cannot bury the one
  artist. Cap `SEARCH_MAX_HITS 200` after ranking; `total` keeps counting so
  the footer can say `200 of 1 234 · keep typing`.
- Actions on Select in RESULTS:
  - **Song** → play it in its album: `browse_load(fs, dir_clus)`,
    `detail_load_meta(fs)`, find the row with `clus == file_clus`,
    `g_queue_kind = RESUME_KIND_ALBUM`, `player_play_queue(g_browse,
    g_browse_n, row, g_art_clus, g_art_size)`, `hal_volume_set`, push
    NOWPLAYING, `np_first = 1`. Stack: MENU, MUSIC, SEARCH, NOWPLAYING —
    MENU returns to the results. (Album context rather than the 6000-track
    Songs queue: it is what "play this one" means, resumes through the
    existing `RESUME_KIND_ALBUM` path, and skips the LOADING SONGS bar.)
    `g_browse` is free to reuse because no BROWSER screen is on the stack
    under Search. An unbound song (`file_clus == 0`, album unreadable) shows
    the row greyed and Select clicks without acting.
  - **Artist** → exactly the Artists SELECT block (`g_artist_filter`,
    `g_dir_depth = 0`, `albumview_build`, `albumlist_queue_chips`, `g_br_sel =
    0`, push BROWSER). MENU pops back to Search.
  - **Album** → `g_artist_filter[0] = 0; albumview_build(0); g_br_sel = ai;
    g_dir_depth = 1; browse_load(fs, g_albums[ai].clus); detail_load_meta;
    g_det_sel = 0; g_br_from_search = 1;` push BROWSER. In the BROWSER MENU
    handler, `if (g_dir_depth > 0 && g_br_from_search) { g_br_from_search = 0;
    scr_pop(); }` so MENU from the detail lands on Search, not on the full
    Albums list. `g_br_from_search` is cleared on every other BROWSER push.
  - **Playlist** → `playlist_open(fs, pi)`, push PLAYLIST. MENU pops to Search.
- Physical keys in PICK: RIGHT = space (consumed *before* the global "RIGHT
  pushes Now Playing" block: that block gains `&& !(scr_cur() == SCR_SEARCH &&
  search_mode_pick())`), LEFT = backspace (LEFT is otherwise unused off the
  player screens). In RESULTS the global rules apply (RIGHT → Now Playing).
- Search rows never have letters (`screen_initial_raw` returns 0 for
  `SCR_SEARCH`); the results wheel uses `wheel_move` like every list (row
  acceleration only).

**Cost estimate (the "is a linear scan fine" question).** Per keystroke the
scan decodes and folds every source string once and runs a first-byte-
filtered substring compare: songs ≤ 6000 × ≤47 title bytes (typical ~20),
artists ≤ 512 × ~15, albums ≤ 1024 × ~20, playlists ≤ 64 × ~15 → ~150 KB
typical, 320 KB worst. `fold_ascii` has an ASCII fast path (≈10–15 cycles
per byte on ARM7TDMI with the PP5022 caches warm; the UTF-8 decode only runs
for bytes ≥ 0x80) and the compare is a `memchr`-style first-byte skip, so
≈ 2–5 M cycles at 80 MHz = **25–55 ms per keystroke**, once per Select, not
per detent. That is under one repaint and needs no folded-name cache (which
would cost 288 KB of .bss for song titles alone). Optional, only if a bench
says otherwise: when a character is appended and the previous scan was
complete (`total <= SEARCH_MAX_HITS`), rescan only the previous hits.

**Folding** — `library/fold.h` (new, pure, host-tested):

```c
/* One folded byte per codepoint: A-Z -> a-z; U+2018/9 -> '; U+201C/D -> ";
 * U+2013/4 -> -; Latin-1 Supplement and Latin Extended-A letters -> their
 * base ASCII letter (É é È ë -> e, Ł -> l, Ø -> o, ß -> s, Æ -> a, Þ -> t);
 * everything else non-ASCII -> 0x80 (never matches an ASCII query). */
int  fold_cp(int cp);
/* Fold `s` into `out` (max bytes incl. NUL); returns the length. */
int  fold_ascii(const char *s, unsigned char *out, int max);
```

The case/quote/dash rules are `name_hash`'s (`names.c:126-147`) so a query
matches what the locator would consider the same name; the diacritic table
is *additional* and lives here, not in `name_hash`, whose bytes are pinned by
`tests/kernel/name_hash_vectors.h` and `tools/build_index.py` parity. The
Latin-1 + Extended-A table is 224 entries of one byte.

**Data structures** — `ui/search.h`:

```c
#define SEARCH_QUERY_MAX 24
#define SEARCH_MAX_HITS  200
enum { SEARCH_T_ARTIST, SEARCH_T_ALBUM, SEARCH_T_PLAYLIST, SEARCH_T_SONG };
enum { SEARCH_PICK, SEARCH_RESULTS };

typedef struct { uint16_t idx; uint8_t type; uint8_t rank; } search_hit_t;  /* 4 B */

/* What the model scans — injected so the host test can hand it arrays.
 * `name(type, i)` returns the sort key the list is ordered by. */
typedef struct {
    int count[4];
    const char *(*name)(int type, int i);
    const char *(*artist_key_of)(int type, int i);   /* artists: past "The "; else name */
} search_source_t;

typedef struct {
    char         query[SEARCH_QUERY_MAX + 1];
    uint8_t      qlen, mode, cell;         /* cell: ring position          */
    int          sel, accum;               /* RESULTS selection            */
    int          ring_accum;               /* PICK sub-detent remainder    */
    search_hit_t hit[SEARCH_MAX_HITS];     /* 800 B                        */
    search_hit_t sub[SEARCH_MAX_HITS];     /* substring-rank overflow, 800 B */
    int          nhit, total;
} search_t;                                /* ~1.7 KB .bss, one instance   */

/* The ring: "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" + SPACE + DEL + DONE = 39 cells. */
#define SEARCH_RING_N 39
enum { SEARCH_CELL_SPACE = 36, SEARCH_CELL_DEL = 37, SEARCH_CELL_DONE = 38 };

void search_reset(search_t *s);
/* Wheel in PICK: returns cells moved (for the click); wraps. */
int  search_ring_move(search_t *s, int8_t delta);
/* Keys. Returns SEARCH_ACT_NONE / RESCAN / TO_RESULTS / TO_PICK / POP /
 * ACT (Select on a hit; the hit is s->hit[s->sel]). */
int  search_key(search_t *s, int key);          /* SEARCH_KEY_SELECT/MENU/LEFT/RIGHT */
void search_scan(search_t *s, const search_source_t *src);
/* Painter (below the status strip; the caller paints the strip). `row_fill`
 * gives title/sub/right per hit — main.c knows the library, the painter
 * does not. */
typedef void (*search_row_fn)(const search_hit_t *h, char *title, char *sub,
                              char *right, int *greyed);
void search_render(const search_t *s, search_row_fn row_fill);
```

`main.c` supplies `search_source_t` over `g_artists / g_album_key /
g_playlists / g_songs[g_song_sorted[i]]` (song `idx` is the *sorted
position*, so hits stay in title order and `g_songview`-style lookups are
direct) and `search_row_fill` (song: title, `SONG · artist`, duration;
artist: name, `ARTIST`; album: `g_album_key`, `ALBUM · artist`; playlist:
name, `PLAYLIST`).

**Screen layout** (320×240; y in px; fonts from `chrome.h`). PICK mode:

```
y  0..14  ┃ Sunflower                                 ▮▮▮▯ ┃  status strip (unchanged)
y 30 base ┃ ‹ Search                                12 hits ┃  ui_header: title, right = "N hits" / "" when empty
y 38      ┃ ──────────────────────────────────────────────── ┃  divider
y 44..69  ┃ ┌──────────────────────────────────────────────┐ ┃  query plate: ui_round_rect(12,44,296,26,r4,LINEN_PLATE)
          ┃ │ SUNF▏                                        │ ┃  bold_13 ink, upper-case as typed, 1-px ink caret;
          ┃ └──────────────────────────────────────────────┘ ┃  empty: "Type with the wheel" in FONT_SMALL MUTED2
y 76..97  ┃   Q  R  S  T  U  V  W  X  Y  Z  0  1  2  3  4    ┃  ring strip: 15 cells × 20 px, centred on the cursor, wraps;
          ┃                 [W]                              ┃  cursor cell = ink pill 18×20 r4 with surface glyph (bold_12);
          ┃                                                  ┃  word cells SPACE / DEL / DONE in FONT_SMALL caps, 40 px wide
y 102     ┃ ──────────────────────────────────────────────── ┃  hairline (LINEN_BORDER)
y 106..   ┃ Sunflower                                  2:38  ┃  results preview: 4 rows × ROW_H2 (32) via ui_list_row
          ┃   SONG · Post Malone                             ┃  (y0 = 106; no selection bar in PICK)
          ┃ Sunday Morning                                   ┃
          ┃   PLAYLIST                                       ┃
          ┃ Sunflower (Spider-Man…)                    2:41  ┃
          ┃   SONG · Post Malone, Swae Lee                   ┃
          ┃ Sun Leads Me On                                  ┃
          ┃   ALBUM · Half Moon Run                          ┃
y 234..238┃                          200 of 1 234 · keep typing┃  footer, FONT_SMALL MUTED2, right-aligned, only when total > nhit
```

RESULTS mode: the ring strip and the hairline are gone; the query plate
stays (muted ink, no caret) as the "what you searched" line; hits start at
y 76 with 5 rows × 32 (76..236), the selection bar, `n / m` in the header's
right slot, `ui_scrollbar(76, top, 5, nhit)`, and the same footer. MENU
returns to PICK. Empty state (no hits, non-empty query): `No matches` in
`FONT_ROW` `LINEN_MUTED` at (14, 126), as the other empty lists do.

Album hits do not get art chips in this stage: the chip pump
(`albumlist_queue_chips`) is tied to `g_albumview` and the BROWSER screen;
the eyebrow `ALBUM · artist` carries the type. Chips are a follow-up.

Repaint: Search is not in `list_view_current` (returns 0 → always the full
render). A keystroke changes the header count, plate, ring and rows anyway;
a RESULTS wheel move could use the partial path later by adding a
`SCR_SEARCH` case with `y0 = 76, rh = ROW_H2, visible = 5` — leave it out of
C, note it.

**Key map.**

| Key    | PICK                                   | RESULTS                         |
|--------|----------------------------------------|---------------------------------|
| Wheel  | moves the ring cursor (wraps, 1 cell/detent, click per cell, no acceleration) | moves the selection (`wheel_move`, rows only) |
| Select | on A–Z/0–9: append + rescan; SPACE: append ' '; DEL: backspace; DONE: → RESULTS if `nhit > 0` | act on the hit (song plays / artist / album / playlist open) |
| Right  | space (consumed; never Now Playing)    | Now Playing (global rule)       |
| Left   | backspace                              | nothing (global rule)           |
| Menu   | pop to Music (query kept)              | → PICK                          |
| Play   | global (tap pause / hold sleep)        | global                          |

## Files to change

New (host-testable, linked into `core.elf` as `static_library` targets in
`meson.build` next to `wheel_hw_lib`, `c_args: ['-DCORE_FREESTANDING']`):
- `ui/letterindex.h`, `ui/letterindex.c` — Stage A. Depends on nothing but
  `<stdint.h>`.
- `library/fold.h`, `library/fold.c` — Stage C. Uses `mn_utf8_next` from
  `names.h`; add to `library_hw_lib` sources.
- `ui/search.h`, `ui/search.c` — Stage C model + painter. Depends on
  `chrome.h`, `text.h`, `palette.h`, `console.h`, `hal.h` (LCD_WIDTH),
  `fold.h`, `wheel.h` (`WHEEL_CLICKS_PER_ITEM` for the ring arithmetic).
  Same include shape as `ui/screen_battery.c`.

Modified:
- `ui/wheel.h`, `ui/wheel.c` — Stage B: the latch-across-pause rule in
  `wheel_accel_step`; `wheel_set_letter_step` seam; header comment for the
  "screen change" reset now being true.
- `kernel/main.c` — thin, in this order:
  - A: `screen_initial_raw` + `letters_ensure` + `list_initial_at` /
    `list_sel_initial` rewrite (~4183); `g_album_key` swap in the cycle loop
    (~2497); genre sort + remap in `library_finish` (~2470); `wheel_accel_reset()`
    in `scr_push`/`scr_pop` (~3644).
  - B: `list_letter_step_idx` + `wheel_set_letter_step` registration (~7169).
  - C: `SCR_SEARCH` in the enum (~3627) and in `paint_current_screen`
    (~4702), the full-render switch (~6869), and a `default: return 0` is
    already there in `list_view_current`; `MU_SEARCH` in `g_music_menu`
    (~3561) and the Music SELECT block (~6086); `menu_render_list` gains
    `ui_scroll_window` + `ui_scrollbar` (~3592) and `list_view_current`'s
    MENU/MUSIC cases already pass `count/sel` so partial repaint keeps
    working; the RIGHT pre-dispatch exclusion (~6027); the `case SCR_SEARCH`
    handler (wheel/SELECT/MENU/LEFT/RIGHT → `search_key` → the four actions);
    `g_br_from_search` in the BROWSER MENU handler (~6267); `search_source_t`
    + `search_row_fill` statics; `g_playlists_scanned`.
- `meson.build` — three new hw static libraries (`letterindex`, `search`;
  `fold.c` into `library_hw_lib`) linked into `core.elf` like `wheel_hw_lib`.
- `tests/meson.build` — new suites (below).
- `docs/screens/render.py` — `az_plate(sc, ch)` + `screen_letter()` (Artists
  with the plate, `letter.png`); `screen_search()` / `screen_search_results()`
  (`search.png`, `search_results.png`); `MUSIC_MENU` gets `Search`; optional
  `gif_search()` (type S-U-N, DONE) for the README.
- Docs (below).

## Tests to add

- `tests/ui/letterindex_test.c` (new suite `letterindex`): build over
  `"AAABBCDDDD"`-style initial strings padded to ≥ 48 rows and over a real
  6000-row synthetic; `letter_at` for every row equals the source; run heads;
  `step` at both ends and from mid-run (the same oracle as wheel_test §6, so
  the two agree); rows with initial 0 (the All Songs row at 0, and one in
  the middle) belong to no run and are never a step target; a trailing `#`
  run after Z is a distinct run and reachable; `count < MIN_ROWS` → invalid;
  `< MIN_RUNS` (all one letter) → invalid; > `MAX_RUNS` (an unsorted
  "ABABAB…" list) → invalid, and the build stays inside the arrays
  (sentinels). Pins the two thresholds by value.
- `tests/ui/wheel_test.c` (update §4/§7/§8): "an idle gap starts a new
  gesture at one row per detent" splits into (a) a 250 ms gap in letter mode
  keeps letters, velocity resets to 1, plate still up; (b) a gap ≥
  `WHEEL_AZ_HOLD_LETTER` drops to rows; (c) a 250 ms gap *not* in letter mode
  is a plain reset (unchanged). New §9: with `wheel_set_letter_step` set to a
  counting stub, `wheel_move` in letter mode calls it and not the walk; unset
  → the walk (existing assertions).
- `tests/library/fold_test.c` (new suite `fold`): `fold_cp` table spot checks
  (É/é/È/ë→e, Ø→o, Ł→l, ß→s, ’→', –→-, A→a, non-Latin → 0x80); `fold_ascii`
  bounds with sentinels, truncation on a codepoint boundary, malformed UTF-8
  yields progress. Assert `fold_cp` agrees with `name_hash`'s three quote/dash
  rules by hashing a folded-vs-raw pair (documents the "consistent with"
  claim without touching the vectors).
- `tests/ui/search_test.c` (new suite `search`): a fake `search_source_t`
  with ~30 names across the four types incl. "The Kid LAROI", "Élan",
  "It’s Over"; query edits (append, cap at 24, leading/double space ignored,
  DEL on empty, RIGHT/LEFT mapping); scan: case-insensitive, diacritic
  ("elan" hits "Élan"), quote ("its" hits "It’s Over"), prefix rank before
  substring, type order inside a rank, songs in source order, `nhit` capped
  at 200 with `total` counting past it (a 6000-song source of the same
  title), empty query → 0; ring: wrap both ways, 4 ticks = 1 cell, remainder
  carried; mode transitions (DONE with 0 hits stays in PICK); painter: render
  PICK and RESULTS into `console_fb()` and assert the plate fill rect, the
  cursor pill, that nothing is drawn in rows 0..14 (the strip band) or in the
  scrollbar column in PICK, the selection bar in RESULTS, and the footer
  only when `total > nhit` — the pixel-oracle style of `chrome_test.c`.
- `tests/library/sort_test.c` — no change (genre sort reuses
  `merge_sort_idx`); the genre remap is main.c-only, covered by
  `tests/kernel/resume_test.c` staying green and by a bench item.
- `make verify-hw`: `check_size.sh` must stay under budget (expect +~2.5 KB
  bss, +~8–10 KB text); `check_hw_layout.sh` unaffected.

## Docs to update

- `docs/USER_GUIDE.md`: Controls — replace "On the Songs list only…" with the
  every-long-list rule, the 1-second latch, and that the letter is the sort
  key's ("The Kid LAROI is under K"); Browsing — a **Search** paragraph with
  the key map table above and the screens; note that Genres are now A→Z.
- `README.md`: the "Browses by…" bullet gains "and a Search"; a screens row
  with `search.png` / `search_results.png` / `letter.png`.
- `STATUS.md`: a dated entry per stage, each **UNFLASHED**, with the bench
  list (below); remove "Search — not implemented" (§"What's NOT done" 2) and
  amend `STATUS.md:658` "(Songs only)".
- `PLAN.md:155` item 4 → done/unflashed pointer.
- `design_reference/README.md` "What the firmware actually implements":
  Search exists (there is no jsx for it — say the layout is derived from
  `menus.jsx` Row/ScreenHeader and the Now Playing eyebrow).
- `docs/screens/README.md`: list the new stills and that `render.py` draws
  the plate/search from `ui/search.c`'s geometry constants (add them to the
  "source of truth" table).
- `core/README.md` Status paragraph: mention Search and the A–Z on every list.
- `ui/wheel.h` header comment (already the design doc for the wheel): the
  latch-across-pause rule and the letter-step seam.

## Acceptance criteria per stage

**Stage A** (`make sim && meson test -C build-sim` green incl. `letterindex`;
`make hw && make verify-hw` clean):
1. `list_initial_at` returns the sort-key initial on Songs, a genre's songs,
   an artist's All Songs, Artists (`artist_key`), Albums (all and filtered;
   the All Songs row → 0), Playlists, Genres; 0 on menus, Settings, queue,
   tracklists, playlist tracks, Search.
2. On any of those lists with ≥ 48 rows and ≥ 4 runs, a fast spin shows the
   plate; under the threshold it never appears (code-read + test on the
   module; device-only for the plate itself — say so).
3. `g_album_key[ai]` equals the title of `g_albums[ai]` after load (assert
   in a UART line at boot behind an existing debug switch, or by the bench:
   Albums plate letters match the rows).
4. Genres list A→Z; a genre's song list is the same set as before the sort
   (bench: counts per genre unchanged; About's genre count unchanged).
5. `scr_push`/`scr_pop` reset the gesture (wheel_test cannot see this — it is
   main.c — so a code-read item).
Bench (device, later): Artists/Albums/Playlists/Genres plate; All Songs row
never shows a letter; Onyx theme plate colours.

**Stage B** (`wheel` suite updated and green; `letterindex` step oracle
agrees with `list_letter_step`):
1. In letter mode a detent lands on a run head, forward and back, on every
   list from A; the ends stay put without a click.
2. A pause < 1.2 s keeps letters; ≥ 1.2 s returns to one row per detent.
3. A screen without letters in letter mode scrolls rows (unchanged test).
Bench: spin on Songs, stop 0.5 s, one detent → next letter; stop 2 s, one
detent → one row; Select on the plate opens the run head's row.

**Stage C** (`fold` + `search` suites green; verify-hw within budget):
1. Music menu shows 9 rows with a scroll window and scrollbar; Search opens
   with the wheel on `A`, an empty plate, the hint.
2. Typing "sun" gives prefix hits first (artist/album/playlist/song order),
   substring hits after, ≤ 200 shown, footer when more; case, diacritics and
   smart quotes fold; RIGHT = space, LEFT = backspace, DONE → RESULTS only
   with hits, MENU back and forth as in the key map.
3. Select on a song plays it in its album and MENU from Now Playing returns
   to the results; artist → its album list; album → its detail, MENU → back
   to Search (not the Albums list); playlist → its tracks.
4. Scan cost: instrument `search_scan` with USEC_TIMER under the existing
   `g_ui_render_us` style counters and print once per scan on the UART
   (`core: search 3 chars 1234 hits 41 ms`) so the bench can read it; the
   25–55 ms estimate is a claim until then.
5. The gallery has `search.png`, `search_results.png`, `letter.png`; README
   and the guide show them.
Bench: the four Select actions; the first Search entry's drive spin-up
(playlist scan) is acceptable; a 6000-song library's keystroke latency.

## Risks

- **RAM / size.** Budget headroom is 2.5 MB of bss and 680 KB of text; the
  stages add ≈ 124 B (`letteridx_t`) + ~1.7 KB (`search_t`, two 200-entry hit
  arrays) + 224 B (fold table, rodata) and ~8–10 KB of code. No new MB-scale
  buffer; `check_size.sh` remains the gate. Do **not** add a folded-title
  cache (288 KB) unless the bench shows the scan is felt.
- **Scan time on device.** Estimated 25–55 ms; if the PP5022 cache misses
  make it 2–3× worse it is still under 200 ms per Select. Mitigation is the
  incremental narrowing in the Design; keep it out until measured.
- **`title_cmp` byte order vs. `#`.** Non-ASCII-initial names sort after Z
  and show `#`; the run index makes them steppable but a user looking for
  "Élan" under E will not find it there. Fixing that means a collation
  change in `title_cmp` (fold through `fold_cp`) — a library-order change
  that touches every list and the host `build_index.py` track-number tests;
  out of scope, note it in the guide.
- **Genre sort remaps indices.** Anything caching a genre index across
  `library_finish` (there is none today: `songview_build` is called per
  entry, the resume record stores a song) would break silently. Grep
  `->genre`/`.genre` before merging.
- **Wheel semantic change** (letters survive a 250 ms gap) alters the feel
  of an *existing* device behaviour on Songs. It is what the guide already
  describes ("The letter stays for just over a second") and what the plate
  already promises; still, it is the one change in this plan that a user
  could dislike. Keep it a single `if` so the rollback is a one-liner.
- **Music menu overflow.** Nine rows in an 8-row list: `menu_render_list`
  must scroll or the last row is invisible. The partial-repaint model
  already handles windowed lists; the risk is the main-menu `Now Playing`
  row logic (`main_menu_count`) — untouched, but retest MENU/MUSIC partial
  repaints in `list_view_current`.
- **Playlist scan on Search entry** spins a parked drive once per session.
  Acceptable and documented; the alternative (search playlists only if the
  user has opened Playlists) is a UX surprise.
- **`g_browse` reuse for a song hit** is safe only while no BROWSER is on the
  stack under Search; Search is reachable from Music only, so that holds —
  assert it with a UART line if `g_scr` contains SCR_BROWSER at that point.
- **Nothing device-verified.** Plate placement over 4-row previews, ring
  legibility at bold_12, the 1.2 s feel and the scan latency are all bench
  items. Ship each stage UNFLASHED with the bench list in STATUS.md.

## Conflict surface

- `kernel/main.c` (7,344 lines, the merge hot spot): touched regions are the
  screen enum (3627), the Music menu table + `menu_render_list` (3561-3601),
  `library_finish` (2470-2510), the locator block (4183-4230), the stack
  helpers (3644-3648), the RIGHT pre-dispatch (6003-6047), the dispatch
  switch (Music 6086, Browser MENU 6267, a new `case SCR_SEARCH`), the two
  render switches (4702, 6869), and the registrations at 7169. The power/
  suspend work lives in the 5600-6000 band and `suspend_to_ram`; the Hold
  banner in 4825-4900; neither overlaps. Keep each stage a separate PR so
  the main.c diff stays under ~150 lines per PR.
- `ui/wheel.c` / `tests/ui/wheel_test.c`: last touched by 6f8e758 / ef8ee4f
  (the extraction); no other branch in flight touches them.
- `library/`: only additions (`fold.c`) and `library_hw_lib` sources; the
  `name_hash` parity check (`tests/scripts/check_name_hash_parity.py`) is
  unaffected because `name_hash` is not edited.
- `docs/screens/render.py`: additive (new screens + one list item); the
  version-stamp stills change with the tree's `git describe` as documented,
  so regenerate from the tagged commit.
- Host app / library manager plans (`core/cli`, `core/docs/design/*`) are
  disjoint.
