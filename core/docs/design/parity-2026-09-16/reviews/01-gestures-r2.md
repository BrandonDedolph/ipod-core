# Review: feat/gestures (round 2)

Worktree `/home/brando/Projects/ipod_theme/.claude/worktrees/gestures`, rebased onto `main`@9c39b54
(sleep timer + headphone merged), HEAD 8fa6ea7, 8 commits (round-1's four rewritten as
50ff6f1/0c7343f/74c7ead/cf8ec00, then c13a2ea, d609b4f, 7440ac9, 8fa6ea7). Round-1 review:
`reviews/01-gestures-r1.md`.

## What I ran (from the worktree's `core/`, clean `build-sim`)

- `make sim && meson test -C build-sim` — **61/61 OK** (`keyhold`, `gesture`, `sleeptimer`,
  `jackwatch` all present), exit 0 (`scratchpad/sim2.log`).
- `make hw && make verify-hw` — exit 0, no warnings, every verify check PASS (`scratchpad/hw2.log`).
  gcc 16 only, as before.
- My harness re-run against the round-2 `gesture.c`/`keyhold.c` (`scratchpad/rv/cases2.c`),
  results quoted below.

## Round-1 findings, re-verified against the code

| # | Status | How verified |
|---|---|---|
| F1 track change under an aim | **fixed** | `gesture.c:60-73` `seekhold_cancel()`; `main.c:6157-6162` calls it on the `player_open_seq()` edge (or an active edge). Harness B: RW aim at 238/240, cancel at the open edge → `CANCEL`, 50 more feeds → NONE, release → NONE. Suite pins the same plus "next press is a normal skip". |
| `g_open_seq` claim | **verified** | `player.c:1263` is the tail of `player_open_current()` (starts `:1208`), `:1512` the tail of `pending_commit()` (the gapless hand-over, `:1463`). These are the only two `g_open_seq++` in the file. `seek_reopen_current()` (`:2067-2082`) goes through `track_open()`, not `player_open_current()`, so neither a committed seek nor its failure/reopen path bumps it. `player_prev()`'s restart and repeat-one's re-open both go through `player_open_current()` → bump → cancel, which a queue-index compare would miss (the header at `player.h:163-168` says exactly this). `was_active` is updated at `main.c:6228`, after the edge block, so the `now_active != was_active` half compares against the previous pass as intended. |
| F2 unmoved release seeks | **fixed** | `gesture.c:150` sets `moved` only when the target actually changed; `:170-178` returns CANCEL otherwise. Harness A: 610 ms release → `CANCEL`. Harness H: cancelled while pinned → CANCEL, `active` 0. Suite covers both the zero-tick and the pinned-from-birth cases; the moved-then-pinned release still COMMITs (existing clamp case), which is right. |
| F3 tap inside a blocked pass | **fixed for the case named**; one narrower residual remains (N1) | `gesture.c:75-80` `seekhold_missed_tap()`; `main.c:6697-6713` hands over a drained RIGHT/LEFT down-edge on the player screens when the live state is already up. Harness: claimed → next feed `SKIP`; across a cancel → still `SKIP` (deliberate, documented at `gesture.h:117-119`); with `allowed` 0 → dropped and does not come back. One `player_next`/`player_prev` site kept (`main.c:6504`). |
| N1 keyhold.h WHY | fixed | `keyhold.h:12-22` now names all four buttons and the no-single-threshold rule. |
| N2 "twenty times a second" / "17 s" | fixed | `gesture.h:23` gone; the ramp comment now says "about 24 s from a standing start" (`:63`), which is the number I derived. |
| N3 guide Play cell | fixed | `USER_GUIDE.md:20`: "Tap on … Tap elsewhere … Hold two seconds anywhere". |
| N4 `allowed` lost then regained | fixed | `gesture.c:112-120`: the `!allowed` branch now kills the press via `seekhold_cancel()` (latch cleared + arbiter reset, no grace). Harness C: aims 0, release NONE. Suite pins it. |
| N5 PASS after teardown | fixed | `PLAY_TAP_CONSUMED` (`main.c:4073-4089`) for every builder that runs `player_queue_begin()` before it can fail; the PLAYLIST-row case is correctly left as PASS with a comment saying why (`playlist_play` refuses before `player_queue_begin`, `:3032`). |

## Rebase resolution (checked)

- `play_tap_start()` sits inside the shared suspend block's `KEYHOLD_TAP` case (`main.c:6336-6356`);
  `want_suspend`/`suspend_origin` bookkeeping from the sleep-timer branch is untouched around it.
- Feed order per pass: track-change/open-seq edge (`:6137-6162`) → jackwatch feed (`:6257-6280`) →
  PLAY feed + sleep-timer feed + the one suspend site (`:6325-6465`) → seek/menu feed block
  (`:6470-6530`) → Hold edge → drain. So the open-seq cancel always runs before the machines are
  fed in the same pass; a press still down after an auto-advance is fed with `allowed_at_down` 0 and
  stays silent (harness B).
- **Jackwatch PAUSE during a seek hold**: `player_pause()` keeps `player_active()` 1, so `allowed`
  stays 1, the aim keeps stepping from its origin, and the release commits into a paused player,
  which `player_seek_to` leaves paused. Correct and consistent with the wheel scrubber.
- **Sleep-timer FIRE during a seek hold** (7440ac9): I confirmed the premise the fix rests on —
  `suspend_to_ram()` ends with `while (clickwheel_buttons() != 0) cpu_wait_ms(20);` then drains the
  latch (`main.c:5810-5813`) — so on return every button is up and no event is pending. Without the
  resets at `:6428-6430` the first feed back would see `was_held && !down` and COMMIT the pre-nap
  target; with them the machines are idle and the wake press is invisible to them (it was drained).
  `menu_key` reset alongside is right for the same reason (a MENU wake would otherwise be a fresh
  press that could fire HOLD if held past 1 s — no: it is released before return; the reset is
  belt-and-braces and harmless). `play_key` needs nothing on the PLAY-triggered path (fired) and on
  the timer path PLAY is released and drained before return, so its next press is fresh.

## Findings

### N1 — nit (residual of F3): a press that straddles the drain and releases before the next feed is still lost

`gesture.c:75-80` claims a missed tap only when the live state is already up at the drain;
`gesture.h:112-116` says a press still down "is a press the next feed will pick up normally". That
is only true if it is *still* down at the next feed. Between drain N and feed N+1 sits the whole
per-screen switch, the render and the present — after a skip that is a full Now Playing frame with
art, tens of ms. Harness "F3-residual": `seekhold_missed_tap(s, 1)` with the machine idle, then the
next feed 60 ms later sees the button up → `NONE`. Concretely: the second tap of a double-skip that
begins in the last few tens of ms of the first skip's blocking file open (the open runs inside the
feed block, before the drain) and lasts less than the post-skip repaint. Narrow — the tick must land
in the feed→drain window and the tap must be shorter than one pass — but it is the very scenario F3
was for, and the fix is two lines: on `is_down && !was_down` set an `edge_seen` flag; in
`seekhold_feed`, if `edge_seen` and the sample is up with `was_down` still 0, report SKIP; clear the
flag on any down sample. One suite case ("claimed while down, released before the next feed → SKIP").

### N2 — nit: guide line contradicts the code and the bench list

`docs/USER_GUIDE.md:117-118`: "Let go before it has moved anywhere, **or while it is sitting
against an end**, and nothing happens at all." A hold that walked to the end and is pinned there
has `moved == 1` and its release COMMITs to `total_s` (suite: "clamp: and the release still commits
the pinned target"; STATUS bench (g): "a hold to the end pins and ends the track normally"). Only a
hold that was pinned from its first tick (started at the end / at 0:00 for rewind) is silent. STATUS
says it right ("pinned against an end its whole life"); the guide should say "or if it never moved
because you were already at the end".

### N3 — nit: one frame of stale aim band on wake from a sleep-timer nap

`suspend_to_ram()` calls `paint_current_screen()` (`main.c:5822`) *before* it returns, and the
resets run *after* it returns (`:6428-6430`), so the wake frame is painted with `seekhold_active()`
still 1 and the band shows the pre-nap target for one present gap until the loop's own `dirty = 1`
repaint lands. Cosmetic. Moving `seekhold_reset(&g_ff/&g_rw)` and `keyhold_reset(&menu_key)` to
just before the `suspend_to_ram()` call is equally correct (nothing between the two points reads
them; `play_key` must stay where it is because suspend waits on PLAY) and removes the frame.

### N4 — nit, judgment call, no change requested: a tap in progress is killed by an auto-advance

Harness B3: RIGHT down 200 ms, the track auto-advances, release at 210 ms → NONE. The open-seq
cancel kills a not-yet-fired press as well as an aim, so a skip pressed in the last half-second of a
track is swallowed. Consistent with the header's "the press is dead" contract and arguably what the
user wanted (the track ended). Recording it so the bench does not mistake it for a lost tap.

## Cases re-confirmed correct in round 2 (beyond round 1's list)

- Cancel-on-`!allowed` every pass on non-player screens: `keyhold_reset` per pass is harmless; a
  list-born RIGHT press with the void in either drain order still ends silent and the next press on
  Now Playing skips (harness D).
- `pending_skip` is cleared when `allowed` drops and survives a cancel (harness G, both).
- Guide "Fast forward and rewind" paragraph otherwise matches (track-change drop, release-time
  skip, ramp numbers); STATUS bullet updated to 61 suites and gains bench (p)/(q); keyhold.h and
  gesture.h comments match the code; commit bodies explain the why, trailers present.
- Acceptance criteria from round 1 still all met on the rebased tree (single `player_next`/`prev`
  call site at `main.c:6504`; renderer goes through `np_aim_target`; `main.c` wiring only — the
  open-seq compare is an edge detect, not timing arithmetic).

SCORE: 9/10

---

# Round 3 (HEAD f5b6c5a: c7bc7e3, 48329c2, f5b6c5a on top of 8fa6ea7)

## What I ran (clean `build-sim`)

- `make sim && meson test -C build-sim` — **61/61 OK**, `gesture` and `keyhold` passing
  (`scratchpad/sim3.log`).
- `make hw && make verify-hw` — exit 0, no warnings, all verify checks PASS (`scratchpad/hw3.log`).
- Harness `scratchpad/rv/cases3.c` (nine cases, below) and `cases2.c` re-run against the round-3
  `gesture.c`; the round-2 "F3-residual" case now prints SKIP.

## Round-2 nits, re-verified against the code

| # | Status | How verified |
|---|---|---|
| r2-N1 straddling press lost | **fixed** | `gesture.c:82-91` latches `edge_seen` when the drain sees the button still down and the machine has no press; `:140-153` settles it at the next feed (down → the machine owns it and clears the latch; up → `pending_skip`, reported as SKIP in that same feed). `seekhold_cancel()` (`:70`) drops an edge in the air but keeps a settled tap; `seekhold_void()`/`seekhold_reset()` clear both. Harness: (1) straddling press → SKIP, then NONE; (2) edge in the air then a real 800 ms hold → 0 skips, 2 aims, COMMIT (no double count); (3) edge-tap then a normal tap → SKIP SKIP; (4) edge then cancel → NONE; (5) settled tap then cancel → SKIP; (6) edge then void → NONE; (7) edge, `allowed` gone at the next feed → NONE NONE (dropped, does not come back — the edge is settled at the top of the feed and the `!allowed` branch clears `pending_skip` right after); (8) press already being timed → drain ignored, one SKIP. The four new suite cases cover the same ground. |
| r2-N2 guide "sitting against an end" | **fixed** | `USER_GUIDE.md:117-122` now says a hold walked to the end lands at the end and the track finishes normally; only a press "only just longer than a tap" or a hold that "had nowhere to go because you were already at the end" is silent. Matches `moved` semantics and bench (g). |
| r2-N3 stale aim band on the wake frame | **fixed** | `main.c:6434-6438`: the three resets now sit immediately above `suspend_to_ram()`; the only statements between the reset block and the call are the resets themselves (read it: `:6418-6438`), so `paint_current_screen()` inside suspend sees `seekhold_active()` == 0. `play_key` correctly stays untouched (suspend waits on `WHEEL_BTN_PLAY`, `:5674`). |
| r2-N4 tap killed by an auto-advance | unchanged, as agreed | recorded only; the r3 header now states it explicitly (`gesture.h:110-113`). |

## Findings

### N1 — nit: one comment in `main.c` still describes the round-2 rule

`kernel/main.c:6712-6714`, in the drain's hand-over block: "A button still down is not a missed
tap — that press is simply one the next feed will pick up." That was the round-2 contract; since
c7bc7e3 a button still down is handed over too and *judged at the next feed* (`gesture.h:124-146`
says so). The commit touched `gesture.h` and `gesture.c` but not this caller comment. Replace the
last sentence with "A button still down is handed over as well; the next feed decides whether the
machine owns the press or it ended in the gap and was a tap."

### N2 — limit, no change requested: two complete taps inside one drain→feed gap fold into one skip

Harness (9): tap A's edge is handed over "in the air", A releases and tap B presses before the
next feed; that feed sees "down", treats it as A still held, and B's release yields one SKIP. The
tick latch is a bitmask, so two down-edges of the same button between two drains were always one
event; `main` would have counted A at drain N and B at drain N+1 only if B's edge landed after
drain N. Requires two full transitions within one pass gap (~50-100 ms after a skip) — below a
human double-tap interval. Recording it so nobody chases it as a regression.

## Verdict

All three round-2 nits are closed in the code (not just in the report), each with suite coverage or
a direct read; the harness confirms the straddling press now skips and none of the new latch paths
double-count or leak across a cancel, a void, a reset or a lost screen. Builds and all 61 suites are
green. What remains is one stale caller comment and a documented limit of the bitmask latch.

SCORE: 9/10

---

# Round 4 (HEAD 1e743a2, one comment-only commit on top of f5b6c5a)

Read the hunk (`kernel/main.c:6712-6716`): the drain's hand-over comment now says a button still
down is handed over too, the machine latches the edge and judges it at the next feed — still down,
the machine owns the press; already up, the whole press fell in the gap and was a tap. That is
exactly what `seekhold_missed_tap()` (`gesture.c:82-91`) and the top of `seekhold_feed()`
(`:140-153`) do, and it matches `gesture.h:124-146`. Re-ran `make hw && make verify-hw` on
1e743a2 myself: exit 0, no warnings, all verify checks PASS (`scratchpad/hw4.log`); the host
suite is unaffected by a `main.c` comment (61/61 at f5b6c5a, unchanged sources). Round-3 N1 is
closed; nothing else was open except the recorded bitmask-latch limit, which is not a defect.

SCORE: 10/10
