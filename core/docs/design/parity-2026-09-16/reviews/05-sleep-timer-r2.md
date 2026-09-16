# Review: feat/sleep-timer (r2)

Worktree: `/home/brando/Projects/ipod_theme/.claude/worktrees/sleep-timer`, HEAD 44cd189, history rewritten into
four commits on main dbce3b8: 7e4ad27 (ui model + module + tests), f56b22f (shared suspend block, pure refactor),
45076a2 (wiring + chrome), 44cd189 (docs/gallery/STATUS). Round 1 is `05-sleep-timer-r1.md`; the old HEAD 2089bcd
is still in the object store, so every round-2 change was reviewed as `git diff 2089bcd HEAD` (63 lines in main.c,
9 in sleeptimer.h, 17 in STATUS.md — nothing else moved) plus the split refactor commit on its own.

## Independently verified

- Wiped `build-sim`/`build-hw`, then `make sim && meson test -C build-sim`: **59/59 OK**; `make hw && make verify-hw`:
  clean under `-Werror`, exit 0. `meson test --list | wc -l` = 59.
- Bisectability of the split: checked out 7e4ad27 and f56b22f into throwaway worktrees. 7e4ad27 alone builds sim and
  passes 59/59 (its stat shows it carries both meson registrations); f56b22f alone builds `make hw` clean. The
  refactor commit's diff is exactly the flag + origin + one block with the same five-local re-seed, and
  `suspend_origin`'s default `nowp` is dead until the next commit uses it — genuinely behaviour-preserving.
- Gallery: unchanged since r1 (`playback.png` and `render.py` identical), so the r1 byte-identity check stands.

## Each reported fix, against the code

1. **`cpu_idled = 0` in the shared re-seed** — `main.c:6052`. Present, after `suspend_to_ram` returns and before
   the idle block at `:6743`. Re-traced: enter from idled (g_boost 0) → suspend's `cpu_unboost()` no-op → wake
   `cpu_boost()` → 1 → re-seed `cpu_idled = 0`, `bl_state = BL_FULL` → idle block takes neither branch → next
   idle-off unboosts 1→0 and the core drops to 30 MHz. Balanced. Enter from non-idled (g_boost 1) → 0 → 1 →
   no-op clear. Balanced. The r1 leak is closed. STATUS gained the bench item for it (device-only, correctly
   labelled).
2. **`strip_cluster_left()`** — `main.c:1028-1032`, used by `status_strip_render` (`:1068`) and the banner row
   (`:4965`). Both now take `bx - (g_locked ? 20 : 6)`; the banner draws no padlock but leaves its 14 px, so the
   token no longer hops when the banner fades. With the timer off the returned x only feeds the `min()` clip and
   the strip is unchanged (250) — same arithmetic as r1. `render.py`'s `status_strip` already used this formula.
3. **Same-pass hold + FIRE origin** — `main.c:6011-6014`: `if (!want_suspend) suspend_origin = nowp;` then
   `want_suspend = 1`. The keyhold switch runs first, so a hold's down-edge stamp survives. Correct.
4. **`sleeptimer.h` feed contract** — `:25-33` now distinguishes "disarms first" (suspend and the standby it
   escalates into) from "never returns" (disk mode, low-battery shut-off, power-down), which is what the code does
   (r1 disarm analysis unchanged). STATUS says the same. The refused-PMU degenerate case returns to the loop with
   the timer still armed, but that path never blocks for more than a few seconds, so the contract is not at risk
   and the wording is fair.
5. **Refactor split** — done, see bisectability above; the message explains why it is its own commit.
6. **`g_lock_flash.armed = 0; g_vol_show.armed = 0;` before `suspend_to_ram`** — `main.c:6035-6036`.

## The new clears on the pre-existing PLAY-hold path: correct on wake?

Yes, and they fix a latent pre-existing hazard rather than introduce one. Read every consumer of both windows.

- `g_vol_show` (VOL_SHOW_US = 1.5 s, `:543`): armed only by a wheel move on Now Playing (`:6519`), read by
  `nowplaying_render` (`:3631`, draws the plate) and the render step (`:6977-6982`, `vol_active`/`vol_edge`).
  Before this change a PLAY hold with the plate still up (wheel turned while PLAY is down — possible, since the
  hold is decided from live buttons) entered suspend armed; the wake frame is painted by `suspend_to_ram`'s own
  `paint_current_screen()`, whose `ui_window_up` sees a wrapped delta after a long sleep and could draw the plate
  on the wake frame, then fade it. With the clear the wake frame never carries a plate. Bookkeeping after wake:
  `dirty = 1` forces `want_full`; the full-paint branch resets `np_vol_prev = vol_active (0)` and `np_vol_dirty =
  0` (`:7025-7026`), so a stale `np_vol_dirty` from the pre-suspend wheel move is consumed by that first frame and
  cannot trigger a spurious plate-only present later. Nothing else reads `g_vol_show`. Correct.
- `g_lock_flash` (LOCK_FLASH_US = 1 s): armed on the Hold edge (`:6097`, `:6124`), read by the banner branch
  (`:6927`) and the early-dismiss path (`:6134`, which also clears `.armed` — the same idiom). A PLAY hold cannot
  coincide with a banner (locked: `down` is gated off; unlock banner: 1 s < 2 s hold), so on that path the clear
  is a no-op. On the timer path a FIRE during the banner's 1 s (the keyhold/feed block sits above the banner's
  `continue`) now wakes with `armed = 0`; the loop-local `lock_flashing` is still 1, and the `if (lock_flashing)
  { lock_flashing = 0; dirty = 1; }` at `:6936` repaints underneath. Correct; closes r1 nit 6.

## Anything the fixes introduced

Looked for it and found nothing: the helper recomputes `bx` locally (the strip keeps its own `bx` for the
battery, same value); the clears happen before the wake paint; the origin guard depends on switch order, which
is fixed by source order; no new host-testable logic was added to main.c that should have gone in a module (all
four are one-line state re-seeds or a coordinate helper). Docs still match the code (guide unchanged since r1;
STATUS paragraph on the clears and the never-returns paths is accurate).

## Findings

### 1. nit — `g_sleep` header comment overstates the FIRE branch
`core/kernel/main.c:557-559`: "the two places that disarm — the shared suspend block and the FIRE branch — zero
both." The FIRE branch only disarms `g_sleep` (inside `sleeptimer_feed`); it is the shared block, on the same
pass, that zeroes the row — which the shared block's own comment (`:6021-6025`) says correctly. One clause:
"…the FIRE branch disarms the countdown and the shared block that follows it zeroes the row." Read it. Same
sentence as r1; not load-bearing, but it is the one comment a future reader will use to find the disarm sites.

No other findings. Round-1 findings 1-6 are all closed.

## Acceptance criteria

All seven met as in r1; criterion 3's idle-boost side effect is fixed and has its bench item; criterion 6
re-run from clean build directories. Device-only items (timer-initiated suspend, wake paused, 30-minute
escalation, the idle-boost path, token look) are explicitly listed in STATUS as unflashed.

SCORE: 9/10

Verdict: every round-2 fix is in the code exactly as reported, each was re-traced rather than trusted, the two
window clears on the PLAY-hold path are correct on wake and actually remove a latent stale-plate case, the
history now bisects (both intermediate commits build and test on their own), and clean rebuilds are green. I
would merge this to main. The one point withheld is for a single comment sentence at the `g_sleep` declaration
that still credits the FIRE branch with zeroing both sides when the shared block does the row; fix that clause
and this is a 10.

---

# r3 — 597287f

One commit on top of 44cd189 (`git diff 44cd189 HEAD --stat`: `core/kernel/main.c`, +3/-2, nothing else).
The hunk at `main.c:557-560` is comment-only and now reads: "The FIRE branch only disarms the countdown; it is
the shared suspend block, which every FIRE lands in, that zeroes the row as well, so both sides are 0 by the
next pass." That is exactly what the code does (`sleeptimer_feed` resets `g_sleep` on FIRE at `sleeptimer.c`;
the shared block at `main.c:6021-6027` resets it again and zeroes `g_settings.sleep_timer_min`; the FIRE case
unconditionally sets `want_suspend`, so every FIRE does reach that block). The commit message states the why.

Re-ran at 597287f: `make hw` exit 0, `make verify-hw` exit 0, `meson test -C build-sim` 59/59 OK.

No findings remain. Every round-1 and round-2 item is closed; every acceptance criterion is met or explicitly
deferred as device-only in STATUS's bench list; the history is four feature commits plus this one-line
correction, each with a message that explains why.

SCORE: 10/10

Verdict: I would merge this to main as-is. The module is correct across the wrap and tested for it, the wiring
is thin and its one genuinely new entry condition (suspend from the dark, idled state) is handled and
bench-listed, the disarm invariant holds on every path that leaves the loop, arming never touches the disk, the
docs match the code line for line, and builds and all 59 suites are green from clean directories.
