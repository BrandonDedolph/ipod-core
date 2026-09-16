# Review: feat/gestures (round 1)

Worktree: `/home/brando/Projects/ipod_theme/.claude/worktrees/gestures`, branch `feat/gestures`,
4 commits on top of `main`@dbce3b8 (f8246d8, 4c2d373, 3d4b052, da62be5). Plan: `plans/01-gestures.md`.

## What I ran (from the worktree's `core/`)

- `make sim && meson test -C build-sim` — **59/59 OK**; `core:keyhold` and `core:gesture` present and
  passing, no XFAIL added (`scratchpad/sim.log`).
- `make hw && make verify-hw` — exit 0, no warnings under `-Werror`, all verify checks PASS
  (`scratchpad/hw.log`). Only gcc 16 is available here; the 14.2 CI pin is not exercised locally.
- No Go or Python changed.
- My own harness against the branch's `ui/gesture.c` + `ui/keyhold.c`
  (`scratchpad/rv/cases.c`) for the cases the suite does not construct; results quoted per finding.

## Acceptance criteria (host) — all met

| Criterion | Status |
|---|---|
| 59 suites green, `gesture` + extended `keyhold`, no XFAIL | met (ran it) |
| `make hw && make verify-hw` clean under `-Werror` | met (ran it; gcc 16 only) |
| `gesture.c`/`keyhold.c` include only `stdint.h` + own headers, no clock, no main.c globals | met (read them) |
| `main.c` wiring only: no state machine / timing arithmetic / ramp numbers | met (read the diff) |
| `git grep player_next kernel/main.c` == one call site, same for `player_prev` | met: both on `main.c:6160` |
| Every MENU `scr_pop()` unchanged except Settings' exit → `settings_leave()` | met (diff touches only the Settings `else`) |
| `nowplaying_transport_render` has no `g_ff`/`g_rw` access, goes through `np_aim_target` | met (`main.c:3432-3470`) |
| USER_GUIDE table + paragraphs, STATUS bullet **UNFLASHED** with bench list | met (see nits on wording) |

Bench list: correctly deferred as device-only; STATUS carries it verbatim-equivalent.

`settings_leave()` (`main.c:3709-3714`): byte-for-byte the old `else` minus `scr_pop()` plus
`g_set_editing = 0`, which is already 0 on that branch (it is the `!g_set_editing` arm), so the tap
exit is unchanged. Verified by reading the old hunk in the diff against the new body.

## Findings

### F1 — should-fix: a RIGHT/LEFT hold that spans an auto-advance seeks the *next* track to a target computed against the *previous* one

`ui/gesture.c:112-121` (the cancel branch) and `kernel/main.c:6140` (`seekable = on_player && player_active()`).

The plan (and `gesture.h:109-111`, and commit 4c2d373's message: "an aim that loses its screen or
its track is cancelled") promise that "a press that began allowed and loses it (**the track ended**,
MENU popped the screen) is cancelled". `allowed` only drops when `player_active()` goes to 0, i.e.
at the end of the *queue*. A track ending and auto-advancing keeps `player_active()` == 1, so the
machine keeps stepping its old `target_s` and commits it into the new track.

Constructed (harness case B, real `gesture.c`): LEFT held at 3:58 of a 4:00 track; the track ends
2 s into the hold and the next one starts (`elapsed` 0, `total` 300 in the feed). Release at 2.5 s:
`COMMIT target=203` → `player_seek_to(203)` lands 3:23 into a track the user has heard zero seconds
of. Case B2: RIGHT pinned at the old total 240, next track is 600 s long → the pin un-pins
(`240+5 > 240` no longer clamps) and the release seeks the new track to 245. Not exotic: rewinding
near the end of a track is exactly when the audio (still playing under a silent aim) reaches the
end during the hold, and a FF hold longer than the remaining audio is common.

Correct looks like: the seek machines' `allowed` must also drop on a track change. `run_ui` already
computes that edge every pass (`main.c:5920-5924`, `now_qidx != last_qidx`, and `player_open_seq()`
at `:5932`), so either feed `seekable && (open_seq == seq_seen_at_down)` or, simpler, call
`seekhold_reset(&g_ff); seekhold_reset(&g_rw); np_last = 0xFFFFFFFFu;` where `dirty = 1` is set for
the queue-index edge (the band is repainted in full there anyway). Add a `gesture` case that feeds
a changed `elapsed`/`total` under an aim *with* `allowed` dropped for one pass and asserts
CANCEL + silent release, and delete/rephrase the "its track" claim in `gesture.h` and the commit
message if the wiring rather than the machine ends up owning it. (Note: the plan's own `allowed`
formula is the source of the gap — the implementer followed it faithfully — but the behaviour the
plan promises, and the task asked me to check, is not delivered.)

### F2 — should-fix: a hold released before its first tick commits a seek to where it started

`ui/gesture.c:133` (target starts at `elapsed_s`) and `:138-140` (COMMIT on any post-hold release);
consumer `main.c:6172-6175`.

Harness case A: RIGHT down for 610 ms, released → `COMMIT target=60` with the live position at 60.
`player_seek_to(60)` stops the DAC, re-primes the ring and rewinds 0.6-0.75 s (audible hiccup and
a small backwards jump) for a gesture that moved nothing. Same while paused: the frozen position
snaps back to the whole second. The wheel scrubber guards precisely this with `g_scrub_dirty`
(`main.c:6966`: no move, no commit). It is also the case a slightly-slow "tap" (500-750 ms) hits,
so it is the most likely mis-press outcome on the device.

Correct looks like: COMMIT only if the aim moved (`ticks > 0`, or `target_s != elapsed-at-fire`),
else return `SEEKHOLD_CANCEL` so the band repaints live. Keep the existing "pinned at the end still
commits" case — that one *did* move. Add a `gesture` case: release between the fire and the first
tick → CANCEL, `player_seek_to` never called.

### F3 — should-fix (plan-accepted risk, but cheap to close): a RIGHT/LEFT tap that starts and ends inside one blocked pass is lost on the player screens

`main.c:6136-6156` (feed from live state only) vs the drain comment at `main.c:6233-6235`
("so a tap that lands while this loop is blocked in a disk read isn't lost — we just drain the
latch here"), which is no longer true for RIGHT/LEFT on Now Playing/queue.

Before this branch a skip was a latched down-edge (`clickwheel.h:108-118`: "no event is ever lost
to a busy main loop"). Now it is a release seen by the 100 Hz live sampler, and `clickwheel_buttons()`
is not latched. A tap whose press *and* release fall inside a pass that is blocked — e.g. the
second RIGHT of a double-skip, which lands while `player_next()` is opening the next file on a
drive the spin-down feature has parked (1-3 s per `STATUS.md`) — produces nothing. The plan calls
this "the same as PLAY's tap today"; PLAY is not double-tapped, RIGHT is, and main handled it.

Correct looks like: on the player screens, when the drain sees a RIGHT/LEFT down-edge event and the
corresponding machine has no press in flight (`was_down == 0`) and the live state already shows
the button up, treat it as a SKIP (a small `seekhold_missed_tap()` returning 1 once). That keeps
the latch's guarantee without touching the hold path. If deliberately left as-is, the drain
comment at `:6233` must say RIGHT/LEFT taps are the exception.

### N1 — nit: `ui/keyhold.h` "WHY THIS FILE EXISTS" was not updated as the plan's file table asks

`ui/keyhold.h:12-24` still says PLAY is the only such button. The plan (`Files to change`, keyhold
row) asks the WHY to say PLAY, RIGHT, LEFT and MENU are all arbitrated here. The `keyhold_void`
doc-comment and `tests/meson.build` do say it; the header's headline paragraph does not.

### N2 — nit: wrong number in `ui/gesture.h:23`

"a seek per tick would stop the DAC, re-prime the ring and rewind the file **twenty** times a
second" — a tick is 250 ms, so four times a second, which is what `main.c:692` and the commit
message say. (The "70-minute recording in 17 s of holding" line 60-63 is the last tier's rate only;
from a standing start it is ~24 s. Harmless but "in 17 s of holding" reads as a total.)

### N3 — nit: guide Play cell can be read as "hold-to-sleep only elsewhere"

`docs/USER_GUIDE.md:20`: "On an album, artist, genre, playlist or song: plays it. Elsewhere, tap:
pause or resume. Hold two seconds: sleep. Hold longer: power off". The plan's wording had "Tap on
an album…"; dropping "Tap" and then starting the hold sentence after "Elsewhere" lets a reader
think a hold on an album row does not sleep. It does (`play_key` is fed everywhere; only the TAP
case consults `play_tap_start`). "Tap on an album …: plays it. Tap elsewhere: pause or resume. Hold
two seconds anywhere: sleep …" matches the code. Otherwise the table and paragraphs match the code
line for line, including the queue-view "you see the result when you let go" and the release-time
skip.

### N4 — nit: `gesture.h:109-111` over-claims "cancelled" for a press that has not fired yet

Harness case C: press begins allowed, `allowed` drops before the 500 ms fire, then returns while
still down → the hold fires later and aims from the new position (`aims=4`, COMMIT). Unreachable in
practice (the player cannot become active again under a held RIGHT without a SELECT on a list
while the finger is still on RIGHT), so no behavioural fix needed; either say "an aim in flight is
cancelled" or `keyhold_void(&s->key)` in the `!allowed` branch so the sentence is exactly true.

### N5 — nit: `PLAY_TAP_PASS` after a builder has already torn the old queue down

`main.c:3970-3985, 3999-4001`: for Artists / Genres / All Songs / Songs, `library_play_song()`
returns -1 only after `player_queue_begin()` → `player_stop()` (`player.c:1590-1592`) when every
song in the view is unresolved on disk. The tap then returns PASS, `player_active()` is now 0, so
nothing toggles and the user is left on the list with the music silently stopped and no Now
Playing. Stale-index edge; SELECT on Songs has the same teardown (it pushes NP regardless). Not a
regression, but the PASS name ("PLAY still means pause/resume") is untrue for this exit; SHOWN or a
comment would do.

## Cases I checked that are correct (so they are not re-litigated)

- **RIGHT press → Hold switch engages**: Hold edge resets `g_ff/g_rw` (`:6213-6214`), `btn` is 0 while
  locked, `dirty = 1` repaints the band. Unlock with the finger still down starts a *new* press
  (harness E), identical to `play_key`'s existing behaviour.
- **End of queue / player inactive under a hold**: `allowed` → 0, CANCEL once, release silent
  (suite case + read).
- **RIGHT hold born on a list, screen becomes Now Playing before release**: latched
  `allowed_at_down == 0` keeps keyhold unfed; the drain's `seekhold_void` (`:6357`) covers the other
  ordering, and its 2-feed grace lapses without eating the next press (harness D: released → NONE,
  next press on NP → SKIP). Void is placed before `player_active()` so it also covers the
  inactive-player list press.
- **Elapsed moving under the aim**: the aim starts at `elapsed_s` on the fire pass (button is down,
  so `el/tot` are read on that pass, `:6145-6148`); delta shown is target minus live; RW target can
  never exceed live. Paused: frozen `elapsed`, `player_seek_to` keeps paused.
- **Repeat-one / FF pinned at total**: commit clamps to `limit-1`, track ends, normal path.
- **`np_aim_target` vs the scrub block**: the hold never sets `g_np_scrub`, so neither the 400 ms
  deferred commit nor the 4 s lapse touches it; an AIM while scrubbing runs `scrub_exit()` first
  (`:6168`), which drops an uncommitted wheel aim as the plan specified. A SELECT tap *during* a hold
  re-enters scrub for at most one tick; not worth code.
- **Borrowed `g_dir_depth`**: success restores 0 (`:4039`) so MENU from NP lands on the album row
  with `g_br_sel` kept; `browse_load` bumps `g_list_epoch` (`:3839`) so the list repaints in full;
  chips are cached in `artcache` (idempotent queue) so the album list is not chip-less afterwards.
  Failure (`g_browse_err` or empty) keeps depth 1 exactly as SELECT, returns SHOWN, and MENU then
  takes the tracklist→album-list branch (`:6595-6598`) back to the same row.
- **PLAY on a playlist whose files are all missing**: `playlist_play(0)` returns -1 *before*
  `player_queue_begin` (`:3032`), so the music keeps playing; `playlist_open` reset `g_plt_sel` and
  bumped the epoch; SCR_PLAYLIST pushed → reason line as SELECT would show.
- **PLAY on Shuffle Songs**: `g_songs_n` is populated at boot (`:5794 library_ensure`), the
  `player_active()` check after the deal matches SELECT's (`:6435-6440`). Playlists root row →
  `MUSIC_OTHER` → pause, as the plan says.
- **MENU hold from Settings sub-screen mid-edit**: tap leaves edit (`:6802`), hold hits
  `settings_leave()` via `scr_pop_to_root` (SOFT commit + `resume_capture`); slider edits are already
  applied/touched live (`:6714-6716`) so nothing is lost. From NP over a list: tap pops NP, hold
  pops the rest, `g_dir_depth = 0`, pending SELECT and scrub dropped. At root: true no-op.
- **`keyhold_void` grace**: same 2-feed grace as the swallow, cleared on release, both fields kept
  through the down-edge so a drain-first void claims the press; suite pins all four positions.
- **Clock wrap**: `now - hold_origin` and `now - down_us` are unsigned; per-tick age is saturated,
  not wrapped; suite pins a hold across 2^32 and I re-derived the 110/365/1355 ramp marks by hand.
- **Modal pushed mid-MENU-hold**: `scr_pop_to_root` drops the charging modal (edge-triggered push
  `:6034`, not re-pushed). Same outcome as a tap dismissing it; acceptable.
- Commit history: four commits in the right order, subjects in the log's style, bodies explain why,
  trailer present on each.

SCORE: 7/10
