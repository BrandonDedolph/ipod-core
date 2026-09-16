# Review: feat/shuffle-albums (r1)

Worktree: /home/brando/Projects/ipod_theme/.claude/worktrees/shuffle-albums, four commits on top of main 6d6504a
(132e4ca ui/config, aeeadaa player, 56b476f kernel wiring, 0102d61 docs). Plan: plans/03-shuffle-albums.md.

## What I ran (independently, from the worktree's core/)

| command | result |
|---|---|
| `make sim && meson test -C build-sim` | 63/63 OK (player-album is #54, 35 assertions) |
| `make hw && make verify-hw` | clean; layout OK, bss 12 214 264 B / 14 680 064 B (83%), build_index selftest PASS |
| `go test ./...` and `go test -count=1 ./internal/devicefs/` | ok (the defaults grep test sees SHUFFLE_OFF via cValue) |
| struct layout, host gcc and arm-none-eabi-gcc `-mcpu=arm7tdmi` | sizeof(browse_entry_t) = 92, album @86, order_key @88 on both |
| token widths with docs/screens/render.py's atlas metrics (FONT_SMALL = regular_9) | `SHUF·ALB RPT1 SLEEP 120` = 128 px, `SHUF RPT1 SLEEP 120` = 107, `SHUF·ALB RPT1` = 75 — exactly what the nowplaying_render comment claims |
| scratch harness linked against the suite's own player.o/stubs (scratchpad/album_extra.c) | all pass, see "Constructed cases" |

## Traces against the questions asked

**album_deal determinism in (queue, seed, keep, mode).** `shuffle_deal` (player.c:1033) collects playable indices in
queue order, records seed/keep, resets `g_groups`, returns for KEEP_QUEUE, then branches on `g_shuffle` to
`album_deal(seed, keep, n)`. Inside: copy to `g_sorted`, heapsort with `album_cmp` (album, order_key, index —
a total order, so the result is independent of heapsort's instability), group starts in sorted order,
`g_rng = seed` then Fisher–Yates over the starts, keep-pin swap, in-place expansion. No other state is read.
So the order is a pure function of (queue contents, seed, keep, g_shuffle). Verified by reading and by
harness E (same (seed, keep) replays the same walk with all-singleton groups) plus suite test 3.

**Boot resume rebuilds the same order with no record change.** `run_ui`: `settings_apply()` (main.c:6141)
pushes `g_settings.shuffle` → `player_set_shuffle(2)` on an empty queue (deals nothing: n = 0, g_order_n = 0),
then `resume_restore(fs)` (:6148) rebuilds the queue by kind — every builder fills `album`/`order_key` from the
same index — then, under `g_queue_kind == kind && (order_seed != 0 || keep == KEEP_QUEUE)` (:5170),
`player_reshuffle_with_seed(resume_order_seed, resume_order_keep)` → `shuffle_deal` under the already-pushed
mode → `album_deal`. Same queue, seed, keep, mode → same `g_order`. `resume_capture` (:4842) stores
`player_order_seed()/keep()` unchanged; `resume_ctx_store` (resume_ctx.h:78) is a field-compare, so the new
`resume_capture()` in the Settings SELECT handler (:7293) touches only when something moved. The skew the plan
worried about (mode changed, old pair still on disk) is closed: the handler captures right after
`settings_apply()` re-dealt. Side effect worth knowing (not a bug): toggling Resume OFF now clears the
locator on the SELECT press rather than at the next capture edge, which is what the guide already promises.

**In-place back-to-front expansion.** After processing groups G-1..g, `out = Σ_{h<g} len_h ≥ g` because
`album_run_len` never returns < 1 (`album == 0` → 1; otherwise the run includes q itself). `g_order[g]` is read
into `q` before the write to `[out, out+len)` with `out ≥ g`, so no unread start is clobbered. Groups of size 1:
`out == g` exactly, written after the read. Pinned album last in sorted order: its start (n − len) is found at
some `g ≥ 1` and swapped to `g_order[0]`; it is expanded last at `out = 0`. Harness A ran 40 seeds starting on
C1 (album 3, last in sorted order): every pass well-formed with album 3 first; A2 (start on C4, the LAST track
of the last-sorted album) plays the two other albums whole afterwards.

**Heapsort stability / cost inside prefetch_next.** The comparator's third key is the index, so equal
(album, order_key) pairs keep queue order deterministically without stability (suite test 9 covers the tie).
Cost: `album_sort` is heapsort O(n log n); group scan, keep search and expansion are O(n) in total (each
`album_run_len` call walks only its own run). Host measurement: 0.66 ms per 6000-entry deal (500 albums ×
12); on the ARM7 at 80 MHz that is tens of ms, worst case ×8 retries under Repeat All, which only happens with
G = 2 (chance 1/256) and then n is tiny. `prefetch_next` (player.c:1601) runs from `player_pump` on the main
loop, not from the DMA IRQ, and is called with the finished track's tail already in the ring; the `track_open`
it does next (metadata parse, possible spin-up) dwarfs the sort. No starvation risk beyond what exists today.

**Repeat All wrap rule.** `shuffle_next` (player.c:1093): ALBUMS branch breaks when `g_groups <= 1` (one-album
queue loops in order — suite test 4 asserts the wrap lands on track 1 with no swap) or when
`!same_album(g_order[0], from)`; the track-level `order_swap` fallback is skipped under ALBUMS. Two albums:
harness B ran 60 wraps, all started the other album at its track 1. Three albums: suite test 5. Two strays
(album 0): `same_album` degrades to index equality, harness D shows no fault. `g_groups` is fresh per deal
(set to 0 in `shuffle_deal`, set by `album_deal`), so the rule never reads a stale count.

**Repeat One / player_next / player_prev.** `auto_next_index` returns `from` under Repeat One (untouched);
`successor/predecessor` just walk `g_order`, so Next at a group's last track is the next group's first track
(suite test 11 through the pump) and Prev at a group's first track is the previous group's last (test 2 covers
Prev inside a group; boundary Prev is the same `shuffle_prev` as Songs, wrapping to the tail at position 0).

**Queue view and TRACK N OF M.** Unchanged in code, as the plan chose: queue order, playing row marked,
`player_queue_current()+1 / player_queue_len()`. See nit 3 on the guide's wording.

**KEEP_QUEUE honesty rule.** `shuffle_deal` returns before the mode branch for KEEP_QUEUE, so Shuffle Songs
stays a song walk under either setting (suite test 7); `nowplaying_render` prints `SHUF` when
`player_order_keep() == PLAYER_KEEP_QUEUE` even with the setting on Albums. Switching the setting to Albums
mid-Shuffle-Songs re-deals with keep = current index, after which both the walk and the token are album-wise —
consistent. Boot with a Shuffle Songs queue restores (0, KEEP_QUEUE) and shows SHUF.

**Alternating-album playlist.** Grouping is by album id through the sort, not adjacency — the fixture
(A3 B1 C4 A1 B3 C2 …) is exactly that shape and test 1 proves regroup + reorder. Within a group the order is
the index's (disc, track); rows the index never bound get 0xFFFFFFFF and sort last (test 9). The guide's
sentence only implies this; see nit 4.

**browse_entry_t 92 bytes, _Static_asserts.** Layout confirmed on both compilers (above). The three
`_Static_assert`s at main.c:67-72 tie `SHUFFLE_*` to `PLAYER_SHUFFLE_*` at the one seam where both headers
are visible. bss +36 KB matches the STATUS claim and the size gate output.

**Config byte 0.** Encode `clampi(s->shuffle, 0, 2)`; decode `<= 2 ? mode : SHUFFLE_OFF`. config_test now
pokes 7 → Off, loops 0/1/2 round trips, and checks encode of 9 writes 2; spicy() uses SHUFFLE_ALBUMS. The
theme-id precedent is followed (unknown → default, not nearest). v1/24-byte and 44-byte records go through
the unchanged §2b tests. Go/Python host mirrors changed only comments.

**Gestures' play_tap_start (main.c:4184).** It builds nothing itself: SCR_MUSIC → `shuffle_songs_play`;
ARTISTS/GENRES/SONGS/BROWSER-depth-0 All Songs → `library_play_song`; PLAYLISTS/PLAYLIST → `playlist_play`;
BROWSER → `browse_load` (which calls `browse_bind` at :4103) then `player_play_queue(g_browse…)`. So all
five fill sites the plan grepped (`browse_collect` dir/file, `queue_entry_from_song` ×2 callers,
`playlist_play`) are the only ones, and each writes both fields. `browse_bind`'s swap loop moves whole
`browse_entry_t`s, so `order_key` written before the sort travels with its row. Re-ran the plan's grep:
main.c:616, :636, :2810, :2863, :3222 — no site missed.

**SLEEP token width.** `st[32]`, worst string 23 chars + NUL; `sleeptimer_token` caps at "SLEEP 120";
measured 128 px against the ~194 px of air (Now Playing in bold_12 ends at x 89 by the renderer's metrics).

## Constructed cases (scratchpad/album_extra.c, linked against build-sim's player.o and stubs)

A pinned album last in sorted order ×40 seeds; A2 start on the last track of that album; B two albums,
Repeat All, 60 wraps; C one-entry queue wrap; D two strays wrap; E/E2 all-singleton determinism and singleton
keep; F 6000-entry deal timing. All pass.

## Findings

1. **nit** — `core/tests/ui/settings_test.c:9`: the file header still reads "2. activate() SELECT: toggles
   Shuffle, cycles Repeat …". Test 2 now cycles Shuffle Off → Songs → Albums → Off and asserts the three
   labels. Say "cycles Shuffle Off->Songs->Albums->Off". (Read it; the body of Test 2 in the diff.)

2. **nit** — commit 0102d61's body: "The README suite counts go 60 -> 61." The rebased diff changes both
   READMEs 62 → 63 (main's last commit already said 62), and 63 is what runs. The history should say what it
   does. (git log + git diff.)

3. **nit** — `docs/USER_GUIDE.md:129-130`: "The queue view lists what is playing and what is next, with the
   playing row marked." Under Songs or Albums shuffle the view lists the queue in list order, which is not
   "what is next" — the plan deliberately left the view in queue order and the album you are hearing sits
   scattered through it. Pre-existing for Songs, but this branch makes it far more visible (SHUF·ALB on a
   Songs list) and touched the paragraphs on either side without fixing it. Suggest: "lists the tracks of
   the list you played from, in that list's order, with the playing row marked; with shuffle on, what plays
   next is not the next row."

4. **nit** — `docs/USER_GUIDE.md:164-166`: "Albums keeps each album whole: the album you are on plays to its
   end in track order, then another album from that same list". For a Songs/artist/genre/playlist queue the
   code plays only the tracks of that album that are IN the list, gathered from wherever they sit in it, in
   the index's track order — it does not play the whole album from disk, and it does not need the tracks to
   be adjacent. The task asked whether the guide says the grouping is by album rather than by position; it
   does not quite. One clause fixes it: "the tracks of the album you are on that are in the list play in
   track order, wherever they sit in it, then another album's". (STATUS.md's "Leaving a Settings row that
   changed the record now also captures" is also slightly off — it is the SELECT press on the row — but that
   is a status note, not the guide.)

No blockers, no should-fix. The implementer's stated deviations are all fine: SHUFFLE_OFF in
settings_defaults with cValue taught (Go passes); `queue_entry_from_song` shared by the two lib_song_t
builders with playlist_play explained; dir rows using 0xFFFFFFFF (never played, comment says why); named seeds
in the different-seed test (the reasoning about only two orders with a pin is right for G = 3); config_test's
`recrc` hoisted to be reused; the Unreleased heading already existed on main. Every acceptance criterion
1-9 is met on the host; the bench list in STATUS.md is the correct device-only deferral.

SCORE: 9/10

---

# r2 — HEAD bcab677

Branch is now 132e4ca, aeeadaa, 56b476f, 9c534e4 (0102d61 reworded), bcab677. `git diff --stat 0102d61 9c534e4`
is empty — the reword touched only the message, which now says "62 -> 63" and "seven-line bench list".
`git diff 9c534e4 bcab677` touches STATUS.md, core/tests/ui/settings_test.c and docs/USER_GUIDE.md only; no
code change.

## Re-ran

| command | result |
|---|---|
| `make sim && meson test -C build-sim` | 63/63 OK (settings #17, player-album #54) |
| `make hw && make verify-hw` | clean; layout OK, bss 12 214 264 B / 14 680 064 B (83%), unchanged from r1 |

Go was not touched since r1 (config.go / settings_defaults_test.go identical), so the r1 `-count=1` run stands.

## The four nits

1. **settings_test.c:9 — closed.** Header now reads "cycles Shuffle Off->Songs->Albums->Off, cycles Repeat
   OFF->ALL->ONE->OFF, …", which is what Test 2 asserts (`shuffle-songs`, `shuffle-albums`, `shuffle-wrap-off`).

2. **Commit message — closed.** 9c534e4's body says "The README suite counts go 62 -> 63", matching the diff
   and the 63 that run. It also corrects "six-line" to "seven-line" for the bench list, which does have (a)-(g).

3. **Guide, queue view — closed and matches the code.** "The queue view lists the tracks of the list you
   played from, in that list's own order, with the playing row marked — so with shuffle on, the next row is
   not what plays next." `queue_render` lists `player_queue_name(i)` for i in queue order and marks
   `player_queue_current()`; under either shuffle mode the successor comes from `g_order`, not `i+1`. Correct
   for Off (next row IS next), Songs and Albums.

4. **Guide, Albums regrouping — closed and matches the code.** "Albums keeps each album together: the tracks
   of the album you are on that are in that list play in track order, wherever they sit in the list, then
   another album from the same list at random … It regroups the list, not the disk — tracks the list does not
   hold are not fetched to complete an album." That is exactly `album_deal`: the sort gathers by
   `browse_entry_t.album` over the queue's own entries (no library lookup in the player), orders each group by
   `order_key` = the index's (disc, track), and the group is pinned first when `keep` falls in it. "Playing one
   album from the browser … simply plays it in order" still matches the single-group case (test 4). The
   STATUS sentence now credits the capture to "A SELECT that changes a Settings row", which is the
   `SETTINGS_ACTION_NONE` branch at main.c:7293.

Nothing else changed; every r1 trace and constructed case still applies to this tree (player.c, main.c,
config.c, settings.c/h are byte-identical to r1).

SCORE: 10/10
