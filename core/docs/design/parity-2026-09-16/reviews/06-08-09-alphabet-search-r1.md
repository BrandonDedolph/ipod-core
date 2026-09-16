# Review r1 — feat/alphabet-search (plan 06/08/09: A-Z indicator, steering, Search)

Worktree: `/home/brando/Projects/ipod_theme/.claude/worktrees/alphabet-search`, rebased onto main
5fac8e4. Commits d4b3bcc (Stage A), 3f39638 (Stage B), 2cb2ed1 (Stage C). All `core/` paths below
are relative to the worktree's `core/`; line numbers are the branch's.

## What I ran (independently)

| Command | Result |
|---|---|
| `make sim && meson test -C build-sim` at HEAD | 66/66 OK (new: `letterindex`, `fold`, `search`; `names`, `name-hash`, `hw-name-hash-parity`, `resume`, `hw-resume-parity`, `wheel`, `gesture` all green) |
| `make hw && make verify-hw` at HEAD | clean; `-Werror` clean; size gate text 382,468 / bss 12,216,384 / data 428 |
| same two at d4b3bcc and 3f39638 (temporary worktrees) | 64/64 OK + hw clean at each — every commit stands alone |
| `docs/screens/render.py` (with the gitignored `art/` linked in) | all 42 stills/GIFs reproduce byte-for-byte except the six that carry the `git describe` stamp (boot, bootdetails, loading, loading_onyx, boot.gif, hero.gif); vs main, only music.png, letter.png, search.png, search_results.png and the stamped ones differ — mainmenu.png and every other untouched screen are byte-identical |

Nothing here ran on the device; every plate/ring/latency claim stays a bench item, as the plan says.

## Findings

### Blocker

**B1. `seekhold_void(&g_ff)` was deleted from the "RIGHT off the player screens" pre-dispatch, while the
comment, the commit message and STATUS all say it is still there.**
`kernel/main.c:7345-7382` (branch) vs main's `kernel/main.c:6823`. On main the block read
`seekhold_void(&g_ff); if (player_active()) { ...push Now Playing... }`. The branch rewrote the
comment (`"The void still applies — RIGHT is never a transport off the player screens..."`) and the
`if`, and the call is gone; `grep seekhold_void` on the branch finds only the modal/backlight paths
(7269/7270, 7305/7306). Commit 2cb2ed1's message: "RIGHT in the picker voids the seek machine at the
same point it skips the Now Playing jump" — it does not. STATUS repeats it.

Why it matters (read `main.c:7100-7135`): the seek machines are fed from the *live* GPIO
(`clickwheel_buttons()`) at 7123, BEFORE the drain reads the *tick-latched* event at ~7345. A RIGHT
press that begins after the feed's sample but before the drain in the same pass — trivially any press
during a long pass (a chip read, a spin-up, a present) — is pushed to Now Playing by the drain, and
on the NEXT pass the feed sees a down-edge with `seekable = 1` and owns it as a fresh press: release
under 500 ms → `SEEKHOLD_SKIP` → `player_next()` (the "skip from a list you were merely browsing"
this whole block exists to prevent); held → a seek. The void covered exactly that ordering, on every
list screen, and was device-proven. This is a regression on every list, not just Search.
Fix: restore the unconditional `seekhold_void(&g_ff);` before the `if` — it does not touch
`ev.buttons`, so the PICK space bar is unaffected (in PICK the press is safe either way because the
feed latches `allowed_at_down = 0` on a non-player screen). Verified by reading both trees and the
feed order; constructed the timeline above.

### Should-fix

**S1. Search never retries a failed playlist folder read.** `kernel/main.c:3231` sets
`g_playlists_scanned = 1` unconditionally, including when `lib playlist_scan` returned `n < 0`
(`g_playlists_err` set, `g_playlists_n = 0`). Music › Playlists re-reads on every entry and recovers
from a transient error; Search (7461) will show no playlist hits for the rest of the session. Set the
flag only when `n >= 0`, or say in the guide that a failed first read sticks. Read it.

### Nits

**N1. MENU from a Search-opened album leaves `g_dir_depth = 1` with no BROWSER on the stack.**
`kernel/main.c:7665-7669`: the `g_br_from_search` branch pops without `g_dir_depth = 0`, unlike the
sibling branch and `scr_pop_to_root` (which treats depth as state that "belongs to a screen the stack
no longer holds"). Every BROWSER entry re-sets depth, so I found no visible effect (checked all
readers: 619/622/643/1709/8334/8428 are BROWSER-guarded or re-set), but it is the one exit from
depth 1 that does not clean up. Constructed: Search → album hit → MENU → depth stays 1 on SEARCH.

**N2. Stray assignment.** `kernel/main.c` `search_play_hit`, playlist error path:
`g_br_from_search = 0;` before `scr_push(SCR_PLAYLIST)`. The flag is BROWSER's; nothing on the
PLAYLIST path reads it. Harmless, misleading.

**N3. `search_reset(&g_search)` is never called.** `kernel/main.c:449` relies on zero-initialised
`.bss` happening to equal the reset state (PICK, cell 0, empty). True today; one line in
`kernel_main` next to `wheel_set_letter_step` would make it not a coincidence.

**N4. Size claim vs current main.** STATUS (Stage C entry) says +7,752 B text / +2,120 B bss "against
main". Against the rebased main (5fac8e4, stamp strings identical in length) I measure text +7,728,
bss +2,120 (exact), and `.data` +40 B (388 → 428: `g_letters_for`'s initialiser and `g_search_src`)
which no entry mentions. The 24 B was presumably measured against the pre-rebase base. Stage A's own
"bss +456 B, text +1.2 KB" is exact (measured at d4b3bcc).

**N5. `docs/USER_GUIDE.md:42-43`** — one line of the new Controls paragraph runs past the file's wrap
width ("...about six screens. A name that starts with a digit,"). Cosmetic.

**N6. `docs/screens/hero.gif` / `boot*.png` will always drift with the stamp**, as the docs already
say; demo.gif reproduces exactly, hero.gif differs only in its boot frames. Fine — noting so nobody
reads the diff as a rendering change.

## The ten attention points

**(1) Genre remap in `library_finish`.** `genres_sort()` (`main.c:2659-2692`) runs before the count
loop (2702-2705), so `g_genre_count[]` is computed from the already-remapped `g_songs[i].genre` —
counts per genre are consistent by construction. Nothing captures a genre index before that point:
the index file carries the genre as a *string* per record (`genre_intern(m.genre)` at 2609, and the
scan path at 2532), interned before `library_finish`; the resume record (`ui/settings.h`) stores
kind + song locator, and `RESUME_KIND_GENRE` rebuilds via `songview_build(s->genre, 0)` at 5215 —
the song's own, remapped field; playlist binding is `g_pl_song[]` = song index; artcache is keyed by
album cluster; no static table indexes `g_genres`; `g_genre_sel` is a list selection reset on every
Genres entry (7447) and cannot be live during a load (`library_ensure` runs at boot before
`resume_restore` (6626 → 6676), and nothing sets `g_lib_scanned = 0` at runtime). The in-place
cycle apply is the same scatter-through-inverse the album sort uses; `g_sort_tmp` is free when the
inverse is built into it and the album block re-fills it afterwards. Genres are de-duplicated
case-insensitively (`name_eq_ci`) and `title_cmp` is case-insensitive, so no equal keys. Correct.
No host test (main.c-only) — the plan accepted that; the bench item is listed.

**(2) `g_album_key[]` permutation.** Consumers on the branch: the comparator (2632, pre-sort), the
fill (2715), the swap (2745-2751), `screen_initial_raw` (4846), `search_name_of` (5422) and
`search_row_fill` (5493). All three post-sort readers are new and all read `g_album_key[ai]` for
`g_albums[ai]`; the swap loop moves the key inside the same cycle as the album. Correct.

**(3) `letterindex` vs `title_cmp` byte order.** `title_cmp` (`names.c:240`) uppercases a–z on both
sides, so A–Z occupy 0x41–0x5A contiguously; everything below (space, punctuation, digits, an empty
title) sorts before A and everything above (`[ \ ] ^ _ \` { | } ~` and every UTF-8 lead byte)
sorts after Z. `initial_of` maps all of those to `#`, so a sorted list has at most a leading `#`
run, ≤26 letter runs and a trailing `#` run = 28 ≤ `LETTERIDX_MAX_RUNS` 40. A list whose first row
is a UTF-8 lead byte can only be an all-non-ASCII list (one run → invalid, no plate — correct); an
all-digit list is one run → invalid; 48 rows with 3 runs → invalid (`MIN_RUNS` 4, tested
`build("ABC", 200)` and the 47/48 boundary). `letter_at` returns the run's letter for rows inside a
run and `#` for both `#` runs; a step from Z forward lands on the trailing `#` head and back from it
on Z (tested §4). Gap rows (All Songs) belong to no run and are never a target (§5). The 6000-row
case walks 27 detents across and back (§6). `letteridx_step`'s backward rule matches
`list_letter_step`'s (head of own run first) — the two oracles are the same cases. One collation
wrinkle not in the plan: a title with leading spaces sorts first but `initial_of` skips the spaces,
so " Zebra" makes an extra Z run at the top; still ≤ 40, still correct letters, purely cosmetic.

**(4) Wheel latch timeline and the resets.** `wheel.c:69-89`: on `dt > WHEEL_IDLE_US` speed resets
(`vel = 1, tps = 0`); the latch survives iff `letters && dt < WHEEL_AZ_HOLD_LETTER`. Timeline:
fast spin latches at t₀; last detent at t₁; plate up while `now − t₁ < 1.2 s`. Resume at t₁+0.5 s:
`dt` 0.5 s > 0.2 s → vel 1, letters kept → `wheel_move`'s letter branch steps exactly `|move|` runs
(velocity is not used in that branch), so one detent = one run head; `wheel_accelerating()` is true
again from that detent → plate stays up showing `list_sel_initial()` = the run head's letter. Resume
at t₁+1.3 s: `dt ≥ 1.2 s` → letters cleared → one row at vel 1, and the plate was already down
(`accelerating` uses `< HOLD`, the latch uses `>= HOLD` — consistent boundary). That is what the
guide now says. Consequence worth stating for the bench: once latched, the ONLY ways back to rows
are a ≥1.2 s pause or a screen change — a slow continuous crawl at any gap < 1.2 s stays in letters
(before: 200 ms). This is the plan's row 3 and its flagged "one change a user could dislike".
Resets: `scr_push`/`scr_pop` (4018/4022) and `scr_pop_to_root` (4083) all call
`wheel_accel_reset()`. The Now Playing push from a list then MENU back: the list's gesture is
reset twice — but a press is a discrete event ≥ tens of ms after the last detent and velocity
already decays in 200 ms, so the only loss is a spin that was still going when RIGHT was pressed,
which is exactly the leak the reset is for. Modal pushes go through `scr_push_modal` → same reset;
harmless. Now Playing's own wheel is volume, not `wheel_move`. No transition needs to keep velocity.

**(5) `fold.c`.** The quote-dropping decision: `names`, `name-hash`, `hw-name-hash-parity` and
`build-index` suites are green, so `name_hash`'s byte pinning is untouched; the fold is a strict
superset unification (every pair `name_hash` calls equal folds equal — `fold_test` §4 asserts it
through `name_hash` itself, and the reverse direction for É/E). The table: I checked every 16-entry
row against the codepoint list and the two spliced non-letters; row sums are all 16, the
`_Static_assert` pins 192+1, and §2 asserts every entry is a–z or 0x80. Coverage vs the atlas: the
atlas is Latin-1 (U+00A0–U+00FF) + a few punctuation marks; the table covers all Latin-1 letters
(C0–FF minus ×/÷) plus Extended-A, which the atlas cannot draw but which folding cannot hurt. "its" →
"It's Over": `fold("It’s Over") = "its over"`, prefix hit — tested in both suites. Growth: `fold_cp`
returns one byte or 0 per codepoint (proved over the whole BMP in §5, exactly six drop), `fold_ascii`
stops at `max-1` and NUL-terminates, sentinels tested at max 0/1/3/16; the query buffer is
`SEARCH_QUERY_MAX+1` for a ≤24-char ASCII query and names get 128 ≥ `NAME_MAX+1`. Never grows.
The ß→"s" (not "ss") choice is the plan's; "strasse" will not find "Straße" — acceptable, documented
by the table comment.

**(6) `search.c`.** Ranking is deterministic: types in enum order, indices ascending, prefix straight
into `hit[]`, substring into `sub[]`, merged after; songs come by sorted position so they are in
title order. The cap: both arrays bounded at 200, `total` counts past both (tested 6000 prefix and
6000 substring). A query of only spaces cannot exist (leading space refused, backspace only removes
the tail); `qn == 0` is guarded anyway. Backspace on empty → `ACT_NONE` (tested). DONE with no hits
→ `ACT_NONE`, stays PICK (tested). MENU from RESULTS → PICK with query AND hits intact (no rescan;
tested). RIGHT/LEFT in PICK: the drain (7345-7382) excludes `SCR_SEARCH && PICK` from the Now
Playing jump so the press reaches the handler; LEFT is never consumed by the drain on any screen;
the feed at 7123 latches `seekable = 0` for a press that begins on Search, so a RIGHT hold in PICK
is one space and never a seek — see B1 for the ordering that IS broken, on other screens. `SCR_SEARCH`
in `play_tap_start`: PICK → `search_play_rows` = 0 → `GESTURE_PLAY_PAUSE` (transport); RESULTS →
count = nhit, `LIST_TRACK` for a song, `LIST_TITLE` otherwise → `GESTURE_PLAY_START` →
`search_play_hit`, which mirrors the per-list PLAY blocks (artist → `songview_build` +
`library_play_song(0)`, album → borrowed depth + `player_play_queue(..., 0, ...)` with the same
error path as BROWSER depth 0, playlist → `playlist_open` + `playlist_play(0)` with the same
push-the-screen-on-failure). Both `gesture_test` and `search_test` §9 pin the shapes. Now Playing
push guard in PICK: correct, and in RESULTS the global rule applies (tested `ACT_NONE`).
`g_br_from_search`: set only by the album hit (both SELECT and the PLAY error path), cleared by
every other BROWSER push (Albums 7426, Artists 7495, the artist hit), consumed by the BROWSER MENU
handler → `scr_pop()` lands on SEARCH in RESULTS mode; MENU from Now Playing is a plain `scr_pop`
(7747-7748) → the results. Playlists scanned once a session: correct because the disk cannot change
under a running firmware — a sync means USB disk mode, which is a reboot — but see S1 for the
failed-read case. Song hit without `detail_load_meta` (deviation): `browse_load` itself recaptures
`g_art_clus/size` via `browse_collect` (main.c:604/4197), which is what `player_play_queue`
consumes; `detail_load_meta` only fills the tracklist screen's per-row meta and hero decode, and
`g_album_title/artist` are read only by `detail_render` (1425-1427) — the path mirrors
`resume_open_album` exactly. Deviation is correct.

**(7) Scan cost.** Inner loop per name: `fold_ascii` (per byte: `mn_utf8_next` ASCII fast path = 3
compares + increment; `fold_cp` = 6 quote compares, 2 dash, 2 case, 1 `< 0x80`; a store) then
`substr_at` with a first-byte skip. `b_lto=true` (meson.build:22) so the calls inline. ~12–20
cycles/byte on ARM7TDMI is realistic, i.e. the plan's 10–15 is a little optimistic; the record
walk (`g_songs[g_song_sorted[i]].title`, 6000 random ~256 B records) adds ~2 cache misses per song,
negligible. At 80 MHz (the CPU is boosted whenever the backlight is on, `main.c:7972-7985`):
~150 KB typical → 35–60 ms, 320 KB worst → 80–130 ms. So "25–55 ms" is the right order and could be
2× under; the UART/CORELOG line (`search_rescan`, and `uart_puts` IS captured into the event log,
`hal/hw/uart.c:197`) measures it, as the plan requires. The scan is synchronous on the main loop,
once per SELECT/RIGHT/LEFT, not per detent. It cannot starve audio: `player_pump` decodes into a
262,144-frame PCM ring (~5.9 s, `player/player.c:42`) refilled from the main loop, and the DMA ISR
re-kicks from the ring, so a 100–200 ms pass is covered many times over (the only exposure is the
pre-roll right after a track start, which is seconds of margin too). Fine to ship unmeasured.

**(8) `menu_render_list` scrolling.** `ui_scroll_window` returns 0 and `ui_scrollbar` draws nothing
when `total <= visible` (`chrome.c:337-362`), so the main menu (≤6 rows) renders identically —
confirmed by the byte-identical mainmenu.png and every other untouched still. `list_view_current`'s
MENU/MUSIC cases already pass `count/sel/visible`, so `list_repaint_partial` windows Music the same
way as any list. render.py's `screen_menu` mirrors the C exactly.

**(9) Sizes.** bss +2,120 B: exact. text +7,728 B against the rebased main (claim 7,752 — N4).
data +40 B unmentioned. Per-stage: Stage A +1,200 text / +456 bss, exact. Stage B +308 text / +8 bss.
All gates pass with wide margin.

**(10) Docs.** USER_GUIDE: the Controls paragraph, Music row count, Genres A→Z, and the full Search
section with the key table match the code (every row checked against the handler); "holding it
types one space, not a seek" is true in PICK. README bullets + a "Finding things" screens row;
STATUS: three dated UNFLASHED entries with bench lists (10 + 6 + 6 items), the "Search — not
implemented" line replaced, ":658" amended; PLAN.md item 4; design_reference/README; docs/screens
README's source-of-truth table; core/README status paragraph; wheel.h's design comment. All present.
`render.py` stills reproduce. The one doc line that is false is the STATUS/commit sentence about the
seek void (B1).

## Deviations from the plan (implementer's ten)

All justified: `end[]` is needed because gap rows make runs non-contiguous; `SEARCH_ACT_OPEN` is a
better name than `ACT`; quote-dropping is argued in fold.h and holds the invariant the plan wanted
(the fold is no longer 1:1 per codepoint, and `search.c` correctly re-folds the artist key rather
than using a byte offset — good catch); `SEARCH_PICK_Y0` 104 and the RESULTS footer beside the plate
are pixel-tested; whole-cell ring clipping is tested against `UI_SB_X`; no `detail_load_meta` on the
song path is correct (see 6); rescan on every Search entry is cheap and honest; the 52-artist
`letter.png` respects the thresholds; amending CHANGELOG/gestures into commit C keeps the history
clean; demo/hero GIFs are the Music-menu row and the stamp.

## Verdict

SCORE: 7/10

Stages A and B are exactly what the plan asked for and I could not break them: the run index is
right against `title_cmp`'s byte order in every case I constructed, the genre remap provably runs
before anything captures an index, the album keys now travel with their albums, and the latch
timeline matches the guide. Stage C is well built and thoroughly tested — the ranking, cap, fold
and painter are pinned by real oracles, every hit action mirrors the list it came from, and the
scan cannot hurt audio. But the branch cannot merge as-is: in rewriting the RIGHT pre-dispatch it
deleted the `seekhold_void(&g_ff)` call that made a list-screen RIGHT press safe from becoming a
skip or seek once Now Playing came up, while its own comment, commit message and STATUS say the
void is still there — a regression of device-proven behaviour on every list, and a one-line restore.
With that line back and S1 addressed, this is a 9.
