# Plan 01 — three iPod input gestures: PLAY on a list title, hold RIGHT/LEFT to seek, hold MENU to root

Repo `/home/brando/Projects/ipod_theme`, branch `main` at `dbce3b8`. All paths below are under `core/`
unless they start with `docs/` or `STATUS.md`. Line numbers are as of `dbce3b8`.

## Summary

Three gestures the original iPod 5G has and this firmware lacks, all decided from LIVE button state
by `ui/keyhold.c` (the existing tap-vs-hold arbiter), with the new logic in a host-testable module
`ui/gesture.c` and thin wiring in `kernel/main.c`:

1. **PLAY tap on a list title starts that list.** On Albums (an album row or the artist's All Songs
   row), Artists, Genres, Playlists, Songs, and inside an album or playlist tracklist, a PLAY *tap*
   builds the queue the row names, starts it from its first track (or from the highlighted track
   inside a tracklist), and pushes Now Playing exactly as SELECT would. A player that is already
   active or paused is replaced (the manual's rule: Play on a list title plays that list). Everywhere
   else — Now Playing, the queue view, Settings, the main and Music menus (except the Shuffle Songs
   row), modals — PLAY tap stays pause/resume.
2. **Hold RIGHT / LEFT seeks within the track** on Now Playing and the queue view only, at a rate
   that ramps (5 s per 250 ms tick, then 15, 30, 60). The hold *aims* the way the wheel scrubber
   does (the transport band shows the target and a signed delta); the one seek is committed on
   release through `player_seek_to()`. A tap still skips (now decided on release, like PLAY). On every
   other screen RIGHT hold does nothing more than the tap (push Now Playing) and LEFT nothing.
3. **Hold MENU (1 s) pops the screen stack to the main menu** from anywhere. MENU tap stays back one
   screen, still acted on at the down-edge, so a hold reads as "back, then home".

`keyhold.c` gains one primitive (`keyhold_void`: this press produces neither tap nor hold) and two
accessors; it already supports any number of instances with independent thresholds. Nothing here
can be tested on the device in this job; the bench list is in Acceptance criteria.

## Current behaviour (with file:line refs)

**Button model.** Buttons arrive two ways: latched DOWN-EDGE events from `clickwheel_get_event()`
(`hal/hw/clickwheel.h:119`, drained at `kernel/main.c:5905`) and LIVE state from
`clickwheel_buttons()` (`clickwheel.h:127`, sampled at 100 Hz). `ui/keyhold.c` turns the live state
into one action per press (`KEYHOLD_TAP` on release under `hold_us`, `KEYHOLD_HOLD` on the first
pass at it, silent afterwards) and has `keyhold_swallow_tap()` for a press whose down-edge was
consumed elsewhere (the tap is dropped, the hold survives). `hold_us` is a parameter of every
`keyhold_feed()` call and the state is per-instance (`keyhold.h:35-41`), so several buttons with
different thresholds are already supported — nothing in keyhold assumes one hold length. There is
exactly one instance today, `play_key`, a local of `run_ui()` (`main.c:5641`).

**PLAY.** `main.c:5840-5871`: `play_key` is fed `!g_locked && (clickwheel_buttons() & WHEEL_BTN_PLAY)`
with `PLAY_HOLD_US` (2 s, `main.c:663`). `KEYHOLD_TAP` → `player_toggle_pause()` only `if
(player_active())` — on a list with nothing loaded a tap does nothing. `KEYHOLD_HOLD` →
`suspend_to_ram(keyhold_down_us(&play_key))`, which itself waits for the PLAY release
(`main.c:5230`). The Hold-switch edge resets it (`main.c:5896`); a press that woke the backlight
from OFF (`main.c:5966-5972`) or dismissed the charging/battery modal (`main.c:5994-6002`) has its
tap swallowed. The per-screen `switch (scr_cur())` never looks at `WHEEL_BTN_PLAY`.

**RIGHT / LEFT.** `main.c:6012-6046`, on the DOWN-EDGE event: on `SCR_NOWPLAYING`/`SCR_QUEUE`
(`on_player`) RIGHT → `player_next()`, LEFT → `player_prev()`, each followed by
`hal_volume_set(g_volume)` and `dirty = 1`. On every other screen RIGHT pushes `SCR_NOWPLAYING` if
it is not already on the stack (sets `np_first = 1`, clears `ev.wheel_delta`) and is masked out of
`ev.buttons`; LEFT does nothing. No hold semantics exist for either.

**MENU.** Handled per screen on the down-edge event: `scr_pop()` at `main.c:6123, 6150, 6169, 6186,
6203, 6222, 6339, 6359`; the browser first drops from tracklist to album list (`6267-6275`);
Settings first leaves edit mode, then its sub-screen, then pops with `resume_capture()` +
`settings_commit(CFG_COMMIT_SOFT)` (`6471-6503`). No hold semantics.

**SELECT on Now Playing** is the other press-length site, hand-rolled (not keyhold): the down-edge
sets `g_sel_pending` (`main.c:6335-6338`); the per-pass block at `6604-6636` opens the queue at
`SEL_HOLD_US` (450 ms, `main.c:661`) or toggles the scrubber on release. The guard at `6608` drops
a pending press whose screen changed or Hold engaged — the "pending-SELECT drop".

**Scrub mode (the existing seek path).** `scrub_enter/exit` (`main.c:674-686`), the wheel moves
`g_scrub_target_s` (`6283-6300`), `nowplaying_transport_render()` (`3395-3445`) shows the target on
the left, `±delta` on the right and a playhead while `np_scrubbing()`, and the deferred commit at
`6648-6663` calls `player_seek_to(g_scrub_target_s)` once the wheel has been quiet
`SCRUB_COMMIT_US` (400 ms), snapping back on refusal; scrub lapses after `SCRUB_EXIT_US` (4 s).
Every target move forces a transport-band-only repaint with `np_last = 0xFFFFFFFFu` (`6300`,
consumed at `6826-6845`). `player_seek_to()` (`player/player.c:2084`) stops the DAC, seeks, re-primes
the ring and leaves a paused player paused at the new position; it returns -1 when inactive, the
codec cannot seek, or a hand-over is in flight (`player/player.h:183-191`).

**Screen stack.** `screen_t` and `g_scr[SCR_STACK_MAX]` at `main.c:3628-3658`: `scr_push` /
`scr_pop` (never below depth 1) / `scr_cur` / `scr_push_modal`; both push and pop bump
`g_list_epoch`, which invalidates the partial-repaint cache (`main.c:405-413`, `4097-4110`). Root is
always `SCR_MENU` (`main.c:5623`). Modals (`SCR_BATTERY`, `SCR_CHARGING`) sit on top and consume
every press (`5994-6002`).

**Queue builders** (all `static` in `main.c`, all set `g_queue_kind`/`g_queue_seed` for resume, none
un-mute — the caller re-applies `hal_volume_set(g_volume)`):
- album tracklist: `player_play_queue(g_browse, g_browse_n, g_det_sel, g_art_clus, g_art_size)`
  with `g_queue_kind = RESUME_KIND_ALBUM` (`6256-6264`); the tracklist is loaded by
  `browse_load(fs, al->clus)` + `detail_load_meta(fs)` (`6247-6253`), and `browse_collect()` lists
  files only while `g_dir_depth != 0` (`main.c:552-600`; `resume_open_album` at `4462` borrows the
  depth the same way);
- Songs / an artist's All Songs / a genre: `songview_build(genre, artist)` (`2572`) then
  `library_play_song(fs, idx)` (`2603`, returns the queue index or -1; kind from
  `resume_kind_of_view`, `kernel/resume_ctx.h:27`);
- Shuffle Songs: `shuffle_songs_play(fs)` (`2712`);
- playlists: `playlist_open(fs, pi)` (`2974`, parse + resolve; missing files are skipped and counted
  in `g_pl_stats`, an unreadable file leaves `g_pl_err`) then `playlist_play(start)` (`3001`,
  returns -1 when `g_pl_tracks_n == 0`).
- the artist row on Artists copies the name into `g_artist_filter` and calls `albumview_build`
  (`6136-6148`); the All Songs row is `albumlist_album_at(row) < 0` (`1354-1361`) and SELECT on it
  does `songview_build(-1, g_artist_filter)` (`6240-6245`).

**Now Playing entry** everywhere is the same three lines: `hal_volume_set(g_volume);
scr_push(SCR_NOWPLAYING); np_first = 1;` (`np_first` is a `run_ui` local, `main.c:5627`).

**Tests.** `tests/ui/keyhold_test.c` (suite `keyhold`, `tests/meson.build:190-196`) feeds
`(is_down, clock)` samples and asserts the action per sample; `tests/ui/wheel_test.c` injects a
clock. `tests/player/player_queue_test.c` runs the real `player.c` against `player_test_stubs.c`
(seek is stubbed: `stub_set_seek_max`, `stub_last_seek_frame`). The hw build links each ui module
as its own static library (`meson.build:268-282`, `link_with` at `meson.build:377`).

**Docs.** `docs/USER_GUIDE.md` Controls table (lines 13-22) and the Now Playing section; `STATUS.md`
top section carries per-change bullets ending in **UNFLASHED** with a bench list.

## Design

### Constants (in `ui/gesture.h` unless noted)

| Name | Value | Why |
|---|---|---|
| `PLAY_HOLD_US` (exists, `main.c:663`) | 2 000 000 | unchanged |
| `GESTURE_SEEK_HOLD_US` | 500 000 | below this a RIGHT/LEFT press is a skip; SELECT's 450 ms is the neighbour |
| `GESTURE_SEEK_TICK_US` | 250 000 | one aim step per tick while held |
| `GESTURE_SEEK_RAMP` | `{ {2 000 000, 5}, {5 000 000, 15}, {10 000 000, 30}, {0, 60} }` | seconds per tick by time since the hold fired: 20×, 60×, 120×, 240× real time |
| `GESTURE_MENU_HOLD_US` | 1 000 000 | long enough not to be a hesitant tap, clearly shorter than PLAY's 2 s |

### 1. PLAY tap on a list title

Decided in the existing `KEYHOLD_TAP` case (`main.c:5846`). The tap is a release, so the screen and
row at release are the ones acted on (a screen cannot change under a press except through Hold or a
modal, both of which void it). New order in that case:

```
case KEYHOLD_TAP:
    if (play_tap_start(fs)) { np_first = 1; dirty = 1; }   /* started a list: NP is pushed inside */
    else if (player_active()) { player_toggle_pause(); dirty = 1; }
    break;
```

`play_tap_start(fs)` is a new `static int` in `main.c` (next to the builders, after
`playlist_play`) that maps the current screen and row through the policy in `ui/gesture.c`
(`gesture_play_tap()`, see Files) and then calls the existing builder. Exact semantics per screen:

| Screen / row | Action on PLAY tap | Notes |
|---|---|---|
| `SCR_BROWSER`, depth 0, album row (`albumlist_album_at(g_br_sel) >= 0`) | load the album's tracklist exactly as SELECT does (`split_artist_album`, `g_dir_depth = 1`, `browse_load`, `detail_load_meta`, `g_det_sel = g_det_accum = 0`); if `g_browse_err == 0 && g_browse_n > 0`: `g_queue_kind = RESUME_KIND_ALBUM; g_queue_seed = 0; player_play_queue(g_browse, g_browse_n, 0, g_art_clus, g_art_size)`, then **restore `g_dir_depth = 0`** and enter NP | MENU from Now Playing lands on the album row you pressed, not inside the album (iPod behaviour). `player_play_queue` copies the entries, so the browser's buffers may be reused later. On failure (`g_browse_err` or empty) leave depth 1 so the tracklist screen shows "could not be read" exactly as SELECT would, and return 0 **without** toggling pause. |
| `SCR_BROWSER`, depth 0, All Songs row (`albumlist_album_at < 0`) | `songview_build(-1, g_artist_filter); library_play_song(fs, 0) >= 0` → enter NP | Queue kind `RESUME_KIND_ARTIST` via `songview_build`. `SCR_SONGS` is never beneath the browser, so overwriting the song view is safe. |
| `SCR_BROWSER`, depth 1 (a tracklist row) | identical to SELECT (`6256-6264`): play the album from `g_det_sel` | "Play on a highlighted song plays it". |
| `SCR_ARTISTS` row | copy the name into `g_artist_filter` as SELECT does (`6138-6143`), `songview_build(-1, g_artist_filter); library_play_song(fs, 0) >= 0` → enter NP | Do **not** call `albumview_build`/push the browser. `g_artist_filter` is reset by the next Albums/Artists entry (`6092, 6097`). |
| `SCR_GENRES` row | `songview_build(g_genre_sel, 0); library_play_song(fs, 0) >= 0` → enter NP | kind `RESUME_KIND_GENRE`. |
| `SCR_SONGS` row | identical to SELECT (`6162-6167`) | plays the view from that song. |
| `SCR_PLAYLISTS` row | `playlist_open(fs, g_pl_sel)`; if `playlist_play(0) >= 0` → enter NP; **else** `scr_push(SCR_PLAYLIST)` and return 1 | A playlist whose files are all missing or whose file is unreadable shows the tracklist screen with its existing reason line (`main.c:3095-3096`: "Could not read playlist" / "No tracks found on disk") — what SELECT would have shown — instead of silently pausing. |
| `SCR_PLAYLIST` row | identical to SELECT (`6215-6220`) | plays the playlist from that row; entries already exclude missing files (`playlist_resolve` skips and counts them). |
| `SCR_MUSIC`, `g_music_sel == MU_SHUFFLE` | `shuffle_songs_play(fs)`; enter NP if `player_active()` | the one menu row that names a queue. |
| `SCR_MENU`, other `SCR_MUSIC` rows, `SCR_NOWPLAYING`, `SCR_QUEUE`, `SCR_SETTINGS` | return 0 → pause/resume as today | |
| any list with count 0 | return 0 → pause/resume as today | "Play works everywhere" stays true. |
| `SCR_BATTERY`, `SCR_CHARGING` | unreachable: the dismissing press is tap-swallowed (`5998-6000`) | |

"Enter NP" = `hal_volume_set(g_volume); scr_push(SCR_NOWPLAYING);` inside `play_tap_start`
(return 1), and the caller sets `np_first = 1; dirty = 1`. Factor the three lines used at
`6038-6039, 6064-6065, 6108-6110, 6164-6166, 6217-6219, 6262-6263` only if you are touching them
anyway; the plan does not require it.

When a player is already active or paused, a START replaces the queue (the builders call
`player_queue_begin`/`player_play_queue`, which stop the old track). PLAY on the album that is
already playing restarts it from track 1 — the iPod does the same.

Edge cases:
- **Hold switch**: `play_key` is fed `down = 0` while `g_locked` and reset on the edge (`5896`); no
  tap can fire.
- **Backlight-wake press**: tap swallowed at `5968` → no start, no pause. Unchanged.
- **Charging / battery modal**: the dismissing PLAY press is tap-swallowed at `5998` → nothing.
  Unchanged.
- **Unlock banner up**: the PLAY down-edge event disarms it (`5937-5948`); the release then starts
  the list. Fine.
- **Load bar**: `library_play_song`/`shuffle_songs_build` draw the "LOADING SONGS" bar over the list
  for large views, exactly as they do for SELECT.
- **Resume**: every builder sets `g_queue_kind`; nothing new to persist.

### 2. Hold RIGHT / LEFT to seek

A new machine `seekhold_t` in `ui/gesture.c`, one instance per direction (`g_ff` for RIGHT with
`dir = +1`, `g_rw` for LEFT with `dir = -1`), fed once per pass right after the PLAY block:

```
seekhold_action_t seekhold_feed(seekhold_t *s, int is_down, uint32_t now_us, int allowed,
                                uint32_t elapsed_s, uint32_t total_s);
```

- `is_down` = `!g_locked && (clickwheel_buttons() & WHEEL_BTN_RIGHT/LEFT)`.
- `allowed` = `(scr_cur() == SCR_NOWPLAYING || scr_cur() == SCR_QUEUE) && player_active()`.
  **Latched at the down-edge**: a press that began with `allowed == 0` (RIGHT on a list, which the
  event branch turns into the Now Playing push) stays void for its whole life even though
  `allowed` becomes 1 one pass later. A press that began allowed and loses it (the track ended,
  MENU popped the screen) is cancelled.
- Returns one of: `SEEKHOLD_NONE`; `SEEKHOLD_SKIP` (released before `GESTURE_SEEK_HOLD_US`) →
  caller does `player_next()`/`player_prev()` + `hal_volume_set(g_volume)` + `dirty = 1`;
  `SEEKHOLD_AIM` (the target moved this pass) → caller forces the transport band
  (`np_last = 0xFFFFFFFFu`); `SEEKHOLD_COMMIT` (released after a hold) → caller
  `player_seek_to(seekhold_target(s))` and forces the band; `SEEKHOLD_CANCEL` (allowed dropped, or
  Hold engaged: caller resets) → caller forces the band so the live position comes back.
- Aim: on `KEYHOLD_HOLD` the target starts at `elapsed_s`, the machine records the hold origin and
  reports `SEEKHOLD_AIM` at once (the band flips to the aim display with its playhead immediately);
  every `GESTURE_SEEK_TICK_US` after that it adds `dir * step` where `step` comes from
  `GESTURE_SEEK_RAMP` indexed by `now - hold_origin`; clamped to `[0, total_s]` (`total_s == 0`
  means unknown length: no upper clamp; `player_seek_to` clamps to the last frame). Ticks are
  computed from elapsed time since the origin, not counted per pass, so a slow pass (a disk read)
  cannot lose steps.
- No track crossing: FF pins at `total_s` and RW at 0. A commit at `total_s` lands on the last frame
  and the track then ends and auto-advances through the normal path.
- Audio during the hold: **silent aiming, one seek on release** — the same trade the scrubber makes
  (`main.c:642-652`: a seek stops the DAC, re-primes, may rewind the file). A periodic audible
  commit while held is a follow-up gated on measuring the post-fix FLAC seek cost on the device
  (`STATUS.md` "not yet confirmed: post-fix seek timing").
- Paused player: aims and commits like the scrubber; `player_seek_to` keeps it paused at the new
  position (`player.h:189-190`).
- Refused commit (`player_seek_to != 0`): drop the target; the band repaints the live position
  (mirrors `6655-6657`).
- Display: `nowplaying_transport_render()` generalises its `scrub` local to an aim: a new
  `static int np_aim_target(uint32_t *target)` in `main.c` returns 1 with `g_scrub_target_s` while
  `np_scrubbing()`, else 1 with `seekhold_target()` while `seekhold_active(&g_ff)` /
  `(&g_rw)`, else 0. Nothing else in the renderer changes: same target-on-the-left, `±delta` on the
  right, playhead. Do **not** route FF through `g_np_scrub`: the deferred-commit block at
  `6648-6663` would then commit after 400 ms of "quiet" and, worse, run `scrub_exit()` + a forced
  band repaint every pass once quiet ≥ 4 s.
- Scrubber interaction: when a seek hold fires while `np_scrubbing()`, call `scrub_exit()` first
  (the wheel goes back to volume, the hold owns the aim). The wheel is ignored by the machine.
- Queue view: seeks work (the spec's two screens) but the queue view has no transport band, so
  the hold is invisible until release; the status strip does not show time. State this in the
  guide ("on the queue view you see the result when you let go").
- Tap latency: a skip now happens at release, not at the down-edge (up to 500 ms later if the user
  is slow). This is how the iPod behaves and how PLAY already behaves here.

Wiring changes at the down-edge event block (`6012-6046`):
- On `on_player`: **delete** the `player_next()`/`player_prev()` calls; the RIGHT/LEFT down-edges
  fall through and are ignored by the per-screen switch (nothing in it reads them).
- On other screens: keep the Now Playing push exactly as is, and add `seekhold_void(&g_ff)` beside
  it (covers the race where the drain runs before the 100 Hz sample has shown the press to
  `seekhold_feed`; the latched-`allowed` rule covers the other order).
- Wake press (`5966-5972`): add `seekhold_void(&g_ff); seekhold_void(&g_rw);` next to the PLAY
  swallow — the press that lights the screen is not acted on, so neither a skip nor a seek.
- Modal dismissal (`5994-6002`): same two voids.
- Hold-switch edge (`5896`): `seekhold_reset(&g_ff); seekhold_reset(&g_rw);` beside
  `keyhold_reset(&play_key)`.
- Root jump (feature 3) and any `scr_pop` off Now Playing: nothing to do — `allowed` drops and the
  machine cancels on the next feed.

### 3. Hold MENU to the main menu

A plain `keyhold_t menu_key` fed each pass with `!g_locked && (clickwheel_buttons() &
WHEEL_BTN_MENU)` and `GESTURE_MENU_HOLD_US`. Only `KEYHOLD_HOLD` is acted on; `KEYHOLD_TAP` is
ignored because the tap already happened at the down-edge in the per-screen switch.

`KEYHOLD_HOLD` → `scr_pop_to_root()`, a new helper beside `scr_pop` (`main.c:3647`):

```
static void scr_pop_to_root(void) {
    if (g_scr_n <= 1) return;               /* already home: nothing, not even a repaint */
    if (scr_cur() == SCR_SETTINGS) settings_leave();   /* the MENU-exit bookkeeping, see below */
    g_scr_n = 1; g_list_epoch++;
    g_dir_depth = 0; g_menu_accum = 0;      /* browser depth and root wheel remainder */
    g_sel_pending = 0; if (np_scrubbing()) scrub_exit();
}
```

`settings_leave()` is the body of the final `else` of the Settings MENU case (`6479-6502`:
`scr_pop(); resume_capture(); settings_commit(CFG_COMMIT_SOFT);`) minus the pop, plus
`g_set_editing = 0`, extracted so both the tap and the jump run the same exit; the tap branch calls
`scr_pop(); settings_leave();`. Then `dirty = 1`, and the main menu renders (the cursor keeps
`g_main_sel`; `main_menu_render` clamps it if Now Playing vanished, `3609-3613`).

Sequencing: MENU tap acts at the down-edge (back one), so a hold reads as back-one at the press,
then home at 1 s. This is deliberate: it keeps the ten per-screen MENU sites and their zero-latency
back untouched, and the second transition is what the user asked for. From Now Playing that means
list → main menu, with `g_sel_pending`/scrub already dropped by the guard at `6608` (screen changed)
and again by the helper for clarity.

Edge cases:
- **Hold switch**: fed 0 while locked; `keyhold_reset(&menu_key)` on the edge (`5896`). A hold
  interrupted by the switch does nothing.
- **Backlight-wake press**: the wake press is not acted on (guide, Power). `keyhold_void(&menu_key)`
  beside the PLAY swallow at `5968`: no back-one (already, `ev.buttons = 0`) and no home. PLAY keeps
  its documented exception (hold from dark still powers off).
- **Charging / battery modal**: the press dismisses the modal and is consumed; `keyhold_void(&menu_key)`
  at `5998` so a long press does not also jump home from under the modal.
- **Hold banner (unlock, up for 1 s)**: the MENU down-edge event disarms it (`5937-5948`) and pops
  one screen; the hold then jumps home. Locked banner: nothing reaches keyhold.
- **Already at root**: no-op, no repaint.
- **Settings mid-edit**: down-edge leaves edit mode (`6472-6473`); the hold leaves Settings through
  `settings_leave()` so the SOFT commit and `resume_capture()` happen exactly as a tap exit.
- **Pending SELECT**: dropped by the helper (and by the `6608` guard).
- **Concurrent RIGHT hold**: `allowed` drops with the screen; the seek machine cancels.
- **Stack invariants**: `g_scr_n = 1` leaves `g_scr[0] == SCR_MENU` (`5623`); modals are never
  under the root so nothing is lost.

### keyhold additions (`ui/keyhold.h/.c`)

- `void keyhold_void(keyhold_t *k)`: this press — the one being timed, or the next one if none is
  (same `KEYHOLD_SWALLOW_GRACE` rule as `keyhold_swallow_tap`) — reports neither TAP nor HOLD.
  Implemented as `no_tap = 1; no_hold = 1;` with a new `uint8_t no_hold` field, cleared on release
  with `no_tap`; `keyhold_feed` skips the `fired = 1; return KEYHOLD_HOLD` branch while `no_hold`.
- `int keyhold_held(const keyhold_t *k)`: 1 while the press is down and HOLD has been reported
  (`down && fired`). Used by `seekhold` and by `np_aim_target`.
- Existing behaviour and the `KEYHOLD_NONE` after-hold release are unchanged, so `play_key`'s switch
  needs no new case.

## Files to change

| File | Change |
|---|---|
| `ui/keyhold.h`, `ui/keyhold.c` | add `no_hold` to `keyhold_t`, `keyhold_void()`, `keyhold_held()`; update the header comment's "WHY" to say PLAY, RIGHT, LEFT and MENU are all arbitrated here. `keyhold_reset` clears the new field. |
| `ui/gesture.h`, `ui/gesture.c` (new) | the constants table above; `seekhold_t { keyhold_t key; int8_t dir; uint8_t active; uint8_t allowed_at_down; uint32_t hold_origin_us; uint32_t ticks; uint32_t target_s; }` with `seekhold_reset/void/feed/target/active`; `gesture_seek_step(uint32_t held_us)` (the ramp lookup, exported so the test pins the exact numbers); the PLAY-tap policy `gesture_play_tap(gesture_ctx_t ctx, int count) -> GESTURE_PLAY_START / GESTURE_PLAY_PAUSE` over `enum { GESTURE_CTX_MENU, GESTURE_CTX_MUSIC_SHUFFLE, GESTURE_CTX_MUSIC_OTHER, GESTURE_CTX_LIST_TITLE, GESTURE_CTX_LIST_TRACK, GESTURE_CTX_PLAYER, GESTURE_CTX_SETTINGS, GESTURE_CTX_MODAL }` (START for SHUFFLE, LIST_TITLE and LIST_TRACK with `count > 0`; PAUSE otherwise). Freestanding C11, `stdint.h` only, no clock of its own (wrap-safe unsigned differences, like keyhold). |
| `meson.build` | `gesture_hw_lib = static_library('core_gesture_hw', sources: ['ui/gesture.c'], c_args: ['-DCORE_FREESTANDING'])` after `keyhold_hw_lib` (`:278-282`); add it to `link_with` at `:377`. |
| `tests/meson.build` | `gesture_test` executable (`ui/gesture_test.c`, `../ui/gesture.c`, `../ui/keyhold.c`), `test('gesture', ...)` after `keyhold` (`:196`); extend the keyhold comment (`:182-189`). |
| `tests/ui/keyhold_test.c` | new cases (below). |
| `tests/ui/gesture_test.c` (new) | the suite below. |
| `kernel/main.c` | thin wiring only: (a) `#include "../ui/gesture.h"` at `:54`; (b) `static int np_aim_target(uint32_t *t)` beside `np_scrubbing` (`:668`), and `nowplaying_transport_render` (`:3404-3405`) uses it for `scrub`/`shown`; (c) `scr_pop_to_root()` beside `scr_pop` (`:3647`) and `settings_leave()` beside it; (d) `static int play_tap_start(fat32_t *fs)` after `playlist_play` (`:3030`); (e) in `run_ui`: declare `g_ff`, `g_rw` as file-scope statics next to the scrub state (`:653-657`, the renderer needs them) and `menu_key` beside `play_key` (`:5641`); the PLAY `KEYHOLD_TAP` case (`:5846-5852`); a seek/menu feed block right after the PLAY block (`:5871`); resets at the Hold edge (`:5896`); voids at the wake swallow (`:5968`) and the modal dismissal (`:5998`); delete the two skip calls in the `on_player` branch (`:6018-6027`) and add `seekhold_void(&g_ff)` beside the push (`:6038`); Settings MENU exit uses `settings_leave()` (`:6479-6502`). Net: ~120 lines added, ~15 removed, no new screen. |
| `docs/USER_GUIDE.md` | Controls table rows Play / Menu / Right / Left; the paragraph under the table; a "Fast forward and rewind" paragraph in Now Playing; the queue paragraph. |
| `STATUS.md` | one bullet in the top section, **UNFLASHED**, with the bench list from Acceptance criteria. |
| `CHANGELOG.md` | nothing now; three lines under the next version heading at tag time. |

Not touched: `player/`, `ui/wheel.c`, `ui/chrome.c`, the screen renderers, `docs/screens/render.py`
(no new pixels: the FF band is the scrub band), the settings record.

## Tests to add (host suites)

All run by `make sim && meson test -C build-sim`; suite count 58 → 59.

**`keyhold` (extend `tests/ui/keyhold_test.c`)**
- `void: a voided press reports neither tap nor hold` (void after the down-edge; feed past 3× `hold_us`; release silent).
- `void before the press: claims the next press, tap and hold` (void while idle, then press/hold/release: all NONE).
- `void lapses: after the grace feeds a fresh press taps` (like the stale-swallow case).
- `void clears on release: the press after a voided one holds` (HOLD fires on the second press).
- `held(): 0 before the threshold, 1 from the HOLD pass until release, 0 after`.
- `two instances, two thresholds: a 500 ms and a 2 s key fed the same samples fire at their own thresholds and neither disturbs the other` (pins the "no single hold length" claim).

**`gesture` (new `tests/ui/gesture_test.c`, suite `gesture`)** — every case is a sequence of
`(is_down, now, allowed, elapsed, total)` samples at 10 ms, assertions are the action and the exact
target in seconds.
- `ramp table: gesture_seek_step` returns 5 / 15 / 30 / 60 at 0, 1 999 999, 2 000 000, 4 999 999, 5 000 000, 9 999 999, 10 000 000 µs and at the 32-bit wrap.
- `tap: released at 499 ms is SKIP, exactly once` and `tap: down-edge and the passes before it are NONE`.
- `hold: first pass at 500 ms is AIM with target == elapsed (the band flips, no step yet), 250 ms later AIM with target elapsed+5, then +10, +15 …` for FF from elapsed 60 of 600; the same for RW (`dir = -1`) landing 55, 50, …
- `ramp: at 2 s of hold the step becomes 15, at 5 s 30, at 10 s 60` (exact targets after 3 s, 6 s, 12 s of hold from 0 of 3600).
- `clamp: FF pins at total and keeps reporting NONE, not AIM, once pinned`; `clamp: RW pins at 0`; `unknown length (total 0): no upper clamp`.
- `commit: release after a hold is COMMIT and seekhold_target is the last aim; the next feeds are NONE`.
- `slow pass: a 900 ms gap between feeds yields the ticks that elapsed (3 steps), not one`.
- `not allowed at the down-edge: a press born on a list never skips or aims, even when allowed becomes 1 on the next pass` (the RIGHT-pushes-Now-Playing case).
- `void: seekhold_void before the press (drain-first race) and after it: nothing on release`.
- `cancel: allowed drops mid-hold (track ended / MENU popped) is CANCEL once, release silent, target no longer active`.
- `Hold switch: seekhold_reset mid-hold, release silent`.
- `wrap: a hold across 2^32 µs aims correctly`.
- `play-tap policy: every (ctx, count) pair` — START for `MUSIC_SHUFFLE`, `LIST_TITLE`/`LIST_TRACK` with count ≥ 1; PAUSE for those with count 0 and for `MENU`, `MUSIC_OTHER`, `PLAYER`, `SETTINGS`, `MODAL`.

**`player-queue` (existing, `tests/player/player_queue_test.c`)** — one case, only because the
commit path relies on it: `seek while paused stays paused at the target` (`player_pause();
player_seek_to(30)`; `player_paused() == 1`, `stub_last_seek_frame == 30*44100`). Skip it if an
equivalent assertion already exists in `player_clock_test.c` (check `grep -n seek_to
tests/player/*.c` first).

The stack helper, `play_tap_start` and the wiring are `static` in `main.c` and are not host-built
(`core/README.md`, "`make sim` is the test build, not a simulator"); they are covered by the bench
list and by `make hw && make verify-hw` (`-Werror`).

## Docs to update

`docs/USER_GUIDE.md`, Controls table:

| Control | On lists and menus | On Now Playing |
|---|---|---|
| Select | Opens the row. On a track, plays it | Tap: scrub mode. Hold: the queue |
| Menu | Back one screen. Hold one second: the main menu | Back to the screen you came from. Hold: the main menu |
| Play | Tap on an album, artist, genre, playlist or song: plays it from the top. Elsewhere, tap: pause or resume. Hold two seconds: sleep. Hold longer: power off | Tap: pause or resume. Hold: sleep, then power off |
| Right | Jumps to Now Playing if a track is loaded | Tap: next track. Hold: fast forward |
| Left | Nothing but the click | Tap: previous track. Hold: rewind |

Paragraph after the table: replace "Left and Right skip tracks only on Now Playing and the queue.
Play works everywhere." with a version that says Play on a list title starts that list, that
Left/Right skip and seek only on Now Playing and the queue, and that holding Menu anywhere goes home.
Now Playing section: a **Fast forward and rewind** paragraph — hold Right or Left; the left time and
the bar show where you are heading and the right time the signed distance, faster the longer you
hold (5 s steps, then 15, 30, 60 per quarter second); let go and it seeks there; it stops at the ends
of the track. The queue paragraph: "Left and Right skip here as well, and a hold seeks; you see the
result when you let go." Power section: no change (the wake press is still not acted on).

`STATUS.md`: a bullet at the top of the 2026-09-13 section, dated, ending **UNFLASHED**, with the
bench list below verbatim.

## Acceptance criteria

Implementer and reviewer tick every line.

Host:
- [ ] `make sim && meson test -C build-sim` green, 59 suites; `gesture` and the extended `keyhold` cases present and passing; no XFAIL added.
- [ ] `make hw && make verify-hw` clean under `-Werror` (gcc 16 and the 14.2 CI pin); `check_size.sh` within budget.
- [ ] `ui/gesture.c` and `ui/keyhold.c` include only `stdint.h`/their own headers; no clock reads, no `main.c` globals.
- [ ] `kernel/main.c` diff is wiring only: no new state machine, no timing arithmetic, no ramp numbers in it.
- [ ] `git grep player_next kernel/main.c` shows exactly one call site (the SKIP case), same for `player_prev`.
- [ ] Every existing MENU `scr_pop()` site is unchanged except the Settings exit, which now calls `settings_leave()`.
- [ ] `nowplaying_transport_render` has no `g_ff`/`g_rw` field access; it goes through `np_aim_target`.
- [ ] USER_GUIDE table and paragraphs updated as above; STATUS bullet added with **UNFLASHED**.

Bench (device, not possible in this job — record results in STATUS when flashed):
- [ ] Albums: PLAY tap on an album row → Now Playing on track 1; MENU → the same album row (album list, not the tracklist).
- [ ] Artists → PLAY on an artist → its All Songs queue, track 1, `TRACK 1 OF n`; resume kind after a reboot is the artist's songs.
- [ ] Artist's album list: PLAY on the All Songs row → same queue; PLAY on an album row → that album.
- [ ] Genres: PLAY on a genre → its songs from the first. Playlists: PLAY on a playlist → from its first present track; a playlist whose files are all missing shows the tracklist screen with "No tracks found on disk" and the music keeps playing.
- [ ] Inside an album tracklist and inside a playlist: PLAY on a track = SELECT on it.
- [ ] Music menu: PLAY on Shuffle Songs deals and plays; PLAY on Artists/Albums/… pauses or resumes.
- [ ] Main menu, Settings, Now Playing, queue view: PLAY tap still pauses/resumes; hold still sleeps at 2 s and powers off past 5 s.
- [ ] PLAY tap on a list while paused starts the new list playing; while playing replaces the queue.
- [ ] From a dark backlight, one PLAY press on a list only lights the screen.
- [ ] Now Playing: RIGHT tap → next track (on release); LEFT tap → previous; RIGHT hold → the band shows the target advancing 5 s a quarter-second, faster after 2 s / 5 s / 10 s; release → one seek lands there; a hold to the end of the track pins at the end and releasing there ends the track normally; LEFT hold → to 0 and stays.
- [ ] While paused: hold RIGHT, release → position moves, still paused; PLAY resumes there.
- [ ] Scrubber on (SELECT tap) then hold RIGHT: wheel returns to volume, the hold owns the aim.
- [ ] Queue view: RIGHT/LEFT tap skip; a hold seeks on release (visible after MENU back to Now Playing).
- [ ] Albums list: RIGHT tap → Now Playing (as before); RIGHT held for 3 s from the list → Now Playing and **no** skip, no seek; LEFT hold on a list → nothing.
- [ ] MENU hold from inside Album → tracklist → Now Playing: one pop at the press, main menu at 1 s; from Settings → Sound mid-edit: edit closed, then main menu, and the changed value survives a power-off (the SOFT commit ran).
- [ ] MENU hold at the main menu: nothing. MENU hold with Hold switched on mid-press: nothing. MENU press from a dark screen: lights only, even if held.
- [ ] Charging screen: any of MENU/RIGHT/LEFT dismisses it and, held, does nothing more; PLAY held from it still sleeps.
- [ ] Hold on, then RIGHT/LEFT/MENU: banner only.

## Risks

- **Skip latency** moves from the down-edge to release (≤ 500 ms). Same as PLAY today and the iPod;
  a user who presses RIGHT slowly will notice. Mitigation: none needed; documented.
- **Seek cost on the device** is unmeasured post-fix (`STATUS.md`: FLAC seek timing not
  re-measured; files carry no SEEKTABLE). One seek per release bounds it; a long RW that rewinds
  the file is the worst case, the same as the scrubber's.
- **MENU hold's two-step transition** (back-one at press, home at 1 s) could read as a flicker
  from Now Playing. Acceptable; the alternative (tap on release) touches ten MENU sites and adds
  latency to every back.
- **Race ordering** between the live-state feed (before the drain) and the down-edge drain: covered
  both ways (latched `allowed`, `seekhold_void` with grace) and pinned by two host cases; a third
  ordering (a press so short it lives entirely between two feeds) is a skip lost, same as PLAY's tap
  today at 100 Hz sampling — the tick sampler latches down-edges, not live state.
- **`g_dir_depth` juggling** in the album START (set to 1 for `browse_load`, restored to 0) is the
  pattern `resume_open_album` already uses; the reviewer should check both exits restore it.
- **Stack budget**: no new screen and no deeper path; `SCR_STACK_MAX` (12) untouched.
- **`main.c` conflicts** with parallel branches (below); the seek machine's file-scope statics sit
  in the scrub block, the busiest neighbourhood.
- Device-only surprises (a BCM present during a seek while the band repaints every 250 ms) are
  paced by `present_gap_us` exactly as the scrubber's per-detent repaints; nothing new there.

## Conflict surface

Shared files/functions this plan edits, for scheduling against other branches:

- `kernel/main.c`
  - includes (`:54`); the scrub state block (`:640-686`: new `g_ff`, `g_rw`, `np_aim_target`);
  - `nowplaying_transport_render` (`:3395-3410`, two lines);
  - screen stack helpers (`:3644-3658`: `scr_pop_to_root`, `settings_leave`);
  - a new function after `playlist_play` (`:3030`, `play_tap_start`);
  - `run_ui` locals (`:5641-5642`), the PLAY keyhold block (`:5840-5871`) and the block right
    after it (new feeds), the Hold-edge block (`:5893-5911`), the wake swallow (`:5961-5972`), the
    modal dismissal (`:5994-6002`), the RIGHT/LEFT transport block (`:6012-6046`), the Settings MENU
    exit (`:6471-6503`), the pending-SELECT guard (`:6604-6618`, read-only unless you add the
    explicit reset). Not touched: the per-screen SELECT/MENU cases (`:6051-6470` apart from Settings'
    exit), the render step, suspend/standby, the boot path.
- `ui/keyhold.h/.c` (struct field + two functions; any branch adding a keyhold user must rebase).
- `meson.build:268-282, 377`; `tests/meson.build:182-196`.
- `docs/USER_GUIDE.md` Controls table and Now Playing section; `STATUS.md` top section.
- Player API is consumed, not changed: `player_seek_to`, `player_next/prev`, `player_play_queue`,
  the builders. A branch changing `player_seek_to`'s contract (e.g. audible periodic seeks) lands
  after this one.
