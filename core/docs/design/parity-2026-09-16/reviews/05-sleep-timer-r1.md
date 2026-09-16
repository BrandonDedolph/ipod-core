# Review: feat/sleep-timer (r1)

Worktree: `/home/brando/Projects/ipod_theme/.claude/worktrees/sleep-timer`, five commits on top of main dbce3b8
(8ecd92b, 2c6fe9b, 4baa62b, 8b43341, 2089bcd). Reviewed against `plans/05-sleep-timer.md`.

## Independently verified

- `make sim && meson test -C build-sim`: **59/59 OK, 0 fail**. `meson test --list | wc -l` = 59, so the three
  quoted suite counts (README.md, core/README.md, STATUS.md) are right.
- `make hw && make verify-hw`: clean under `-Werror`, exit 0 (layout/size/header checks, `build_index` selftest all PASS).
- `./build-sim/tests/sleeptimer_test`: 0 failures, 0 xfail, 0 xpass; the 72 000-feed two-wrap run is in it and passes.
- `docs/screens/render.py` regenerated into a scratch dir with the (gitignored) art directory in place: `playback.png`
  is byte-identical to the committed still; every other committed still is byte-identical except `boot.png`,
  `bootdetails.png`, `loading.png`, `loading_onyx.png`, `boot.gif`, `hero.gif`, which embed `git describe --dirty`
  (`BUILD_ID`) and differ on any commit — and the branch correctly did not commit those. Criterion 6 met.
- Read in full: `ui/sleeptimer.{h,c}`, `tests/ui/sleeptimer_test.c`, the diffs to `settings.{c,h}`, `config.c`,
  both test files, `main.c`, all four docs, `render.py`; plus the untouched callers the wiring depends on
  (`suspend_to_ram`, `enter_standby`, `battery_refresh`'s SHUTOFF edge, the idle CPU-scaling block, `chrome_key`
  users, the Hold-banner branch, `clock.c` boost refcount).

## The specific concerns from the task

**Wrap-safe accumulator, 100 ms cadence with jitter.** Correct. Delta is unsigned between consecutive feeds, the
remainder is carried in `acc_us` (< 60 s invariant held by the `while`), `elapsed_min` saturates at 0xFFFF, FIRE
resets in the same call. Test 4 starts 30 s before the wrap at 100 ms feeds and asserts every boundary; test 5's
7/13/1 s cycle (period 21 s, not a divisor of 60) and the 61×59 s case pin the no-truncation property; test 3
feeds 5-minute wrapped steps after FIRE and asserts silence. A coarse feed that crosses two boundaries at once
returns one TICK with remaining dropping by 2 — the caller only sets `dirty`, so that is fine. Feed cadence: the
feed sits above every `continue` in `run_ui` (the only one is the Hold-banner halt at main.c:6908, well below), so
it runs on every pass including banner passes. Contract (≤ 71 min between feeds) holds.

**Disarm invariant on every path that leaves the loop.**
- PLAY-hold suspend and timer suspend: both go through the one shared block (main.c:5999-6022), which
  `sleeptimer_reset` + zeroes the row before `suspend_to_ram`. Correct.
- Timer/hold escalation to PMU standby inside `suspend_to_ram`: already disarmed by the shared block. Correct.
- Disk mode (main.c:6604 `power_enter_disk_mode`): not disarmed, never returns → reboot → `.bss`/`config_decode`
  zero both sides. Correct in effect.
- Low-battery SHUTOFF edge from the main loop (`battery_refresh` → `enter_standby`, main.c:972): not disarmed;
  normally never returns. If the PMU refuses, the device runs on with the timer still armed and the row still
  showing its value — arguably the right behaviour (the device is still up), but it means the header's "every
  path that stops the loop for longer … disarms the timer first" is not literally true (nit 4 below).
- Reset Settings: `settings_defaults` zeroes the field, `sleep_timer_apply` disarms. Correct.
- Boot: `settings_defaults` + `config_decode` both write 0; `sleep_timer_apply` is not called from boot. Correct.
  I also checked there is no runtime `config_load`/wholesale `g_settings =` (only main.c:5630-5631 at boot), so
  the "two sources of truth" cannot drift through a reload.

**Fires while Hold is on.** `down = !g_locked && PLAY` only gates the keyhold; the feed is unconditional. Suspend
enters with no button down; the wake loop spins on `clickwheel_buttons()==0` with the drain that clears
`s_need_bringup`, so Hold-off-then-press wakes it — matches the guide's "Waking it with Hold on needs Hold off
first". Device-unverified, as STATUS says.

**Fires under the charging or low-battery modal, mid-scrub, or with nothing playing.** All take exactly the path a
PLAY hold takes from the same state (PLAY hold is also decided from live buttons above the modal handling):
suspend, wake, `paint_current_screen` repaints the modal; mid-scrub leaves `g_np_scrub` set and the existing
quiet-timeout block (main.c:6812-6825) commits or exits it on the next passes, same as a PLAY hold mid-scrub
today; nothing playing → `was_playing` 0, plain suspend. Parity, no new behaviour. Exception: the CPU-idle case
below, which a PLAY hold can never reach.

**Token vs the Hold banner and `chrome_key`.** `chrome_key()` gains `remaining_min`, and `g_lp.chrome` is the
only partial-paint gate for lists (Settings has no partial path; Now Playing's clock tick repaints only the
transport band and TICK forces a full frame via `dirty`). The banner branch redraws the token in `sub` so the 1 s
banner does not blink it off. Position differs by 14 px from the locked strip (see nit 2). Name clip is
`min(LCD_WIDTH-70, tok_x-8)` in both painters; with the timer off `tok_x` is 264/278 so the clip is unchanged at
250 — verified arithmetically. `render.py`'s `status_strip` mirrors the same formula.

**`config_decode` zeroing.** Present (config.c:454-458), pinned by the 0x5A-prefill test; encode byte-identity
pinned; `settings_eq` comment says why the field is excluded. Payload stays 44 / version 2.

**Guide vs code.** Every sentence I checked matches: six presses back to Off, arm-at-once, pause-then-sleep,
wake paused, Hold-off-then-press, 30-minute battery escalation, not remembered across restart, "any sleep or a
Reset turns it off", the strip/Now Playing token wording, and the "except the sleep timer" carve-out on disk
saves. STATUS's "three strip paint sites" is accurate (strip, Now Playing cluster, banner row).

**Stated deviation (45 → 15, not 45 → Off → 15).** Accepted. The design paragraph says an unknown value indexes
to 0 (= Off) and the step then lands on index 1; that is also exactly how `bl_index`/`bl_step` behave for an
unknown backlight timeout. The test asserts the behaviour the code has and says why. `nowplaying_sleep.png` was
optional in the plan.

## Findings

### 1. should-fix — CPU idle boost refcount leaks when the timer fires from a dark, idled device
`core/kernel/main.c:5999-6022` (shared suspend block re-seed) with `:6723-6729` (idle CPU scaling) and
`core/kernel/clock.c:182-194`.

Constructed case, the plan's own "a paused or idle device still sleeps" and a very ordinary night: arm 30 min,
album ends (or you pause) at minute 25, backlight times off. The loop's idle block sees `BL_OFF && !player_playing()`
→ `cpu_unboost()` (g_boost 1→0, 30 MHz), `cpu_idled = 1`. At minute 30 the timer FIREs → `suspend_to_ram()` →
its `cpu_unboost()` is a no-op (guarded, g_boost stays 0) → wake → its `cpu_boost()` makes g_boost 1. The re-seed
after the block sets `bl_state = BL_FULL`, `panel_slept = 0`, `dirty = 1` — but leaves `cpu_idled = 1`. Same
pass, main.c:6723: `cpu_idled && bl_state != BL_OFF` → `cpu_boost()` again → **g_boost = 2**, `cpu_idled = 0`.
From then on every idle `cpu_unboost()` takes it 2→1 and the core never drops to 30 MHz at idle again until a
reboot; every further timer-from-idle cycle keeps the leak at exactly one. The PLAY-hold path can never reach this
state: the press that starts the hold lights the backlight and re-boosts a pass earlier, so `suspend_to_ram`'s
"the boost refcount is >= 1 here (we are entered from BL_FULL)" premise held until this branch added the first
entry from BL_OFF. The plan's "Wake bookkeeping already re-seeds … nothing else is needed" (§7) was wrong on this
point; the implementer followed it.

Correct: the re-seed after the shared block also does `cpu_idled = 0;` (suspend_to_ram's wake `cpu_boost()` has
already re-established the one boost the loop believes it holds) — with a comment mirroring the `panel_slept`
one. Alternatively boost before the FIRE path's suspend, but the re-seed is the one-place fix. Untestable on the
host today (the idle block is loop-local in main.c); the bench list should gain "arm, let the album end, screen
off, timer fires, wake: `core: ui` line / About must still show the idle 30 MHz drop afterwards".

Verified by reading `clock.c` (`g_boost++ == 0` / guarded `--g_boost`), the idle block, the re-seed, and the
`suspend_to_ram` unboost/boost pair.

### 2. nit — Hold-banner token position differs from the locked strip
`core/kernel/main.c:4944-4950`. The banner row draws the token at `bx-6` (the unlocked position) although the
banner is up precisely when Hold is on; when the banner fades the strip repaints with the padlock and the token
14 px further left, so the token hops once per Hold flip. Cosmetic, one second, and the plan asked for "no
padlock" in that row without specifying the token's x; mentioned only so the device night knows it is expected.
Read it.

### 3. nit — same-pass PLAY-hold and FIRE overwrite the escalation origin
`core/kernel/main.c:5988-5989`. If `KEYHOLD_HOLD` and `SLEEPTIMER_FIRE` land on the same pass (a 2 s hold whose
threshold pass coincides with the minute boundary), `suspend_origin` is overwritten with `nowp`, so the 5 s
escalation is timed from now rather than the down-edge: the held PLAY reaches PMU standby ~2 s later than the
documented gesture. Harmless; correct is to keep the earlier of the two stamps, or simply not overwrite when
already set. Constructed the case; did not run it.

### 4. nit — header overstates the disarm rule
`core/ui/sleeptimer.h:28-31` ("every path that stops the loop for longer — suspend, PMU standby, disk mode —
disarms the timer first") and the STATUS/plan wording "PMU standby … leaves the timer Off". Disk mode and the
main-loop SHUTOFF standby do not disarm; they never return (and a refused SHUTOFF standby runs on with the timer
still armed, which is fine). Correct wording: "either disarms first or never returns to the loop". Read it.

### 5. nit — commit history vs the plan's risk note
The plan asked for the keyhold/suspend refactor to land as its own commit before the token/paint changes
(main.c is the merge hot spot); 2c6fe9b carries both. Messages themselves are good and explain the why.

### 6. nit — timer firing during the 1 s Hold banner
If FIRE lands inside the `LOCK_FLASH_US` window, `g_lock_flash` is not disarmed before the suspend; after a long
sleep `ui_window_up` sees a wrapped delta and may show the banner for up to one more second on wake. Pre-existing
pattern (`ui_window_t` is documented to disarm on expiry), a sub-second window of exposure, and the wake repaint
hides it. Constructed, not run; not worth code unless the shared block grows a "forget transient windows" step.

## Acceptance criteria

1. Row, cycle, no disk write: met (action code + no `settings_touch`; device side of "CFG seq does not move" is
   correctly in the bench list).
2. Token on every strip and Now Playing, name shortens, padlock/battery fixed: met by reading; panel look is
   device-only and STATUS says so.
3. Expiry behaviour: met on the host side (pause-then-suspend, `was_playing` 0 → wake paused); device-only parts
   deferred explicitly. Finding 1 is a side effect on this path from the idle state.
4. Any suspend/standby/disk mode/Reset leaves it Off: met (see disarm analysis; wording nit 4).
5. Exact across the wrap, never 0: met, tested.
6. Suites, `-Werror`, `verify-hw`, render.py: met, all run here.
7. Docs + STATUS UNFLASHED with bench list: met.

SCORE: 7/10

Verdict: the module, tests, config codec, settings model, docs and gallery are all correct and I could not break
the arithmetic or the disarm invariant on any path the plan lists; builds and all 59 suites are green here. What
keeps it off main as-is is one real bug the plan did not foresee: the timer is the first thing that can enter
`suspend_to_ram` from the dark-and-idle state, and the shared block's re-seed does not reset `cpu_idled`, so a
timer that fires after the music has stopped (album shorter than the timer — the common night) leaks one boost
reference and permanently defeats the 30 MHz idle drop until reboot. One line (`cpu_idled = 0` in the re-seed)
plus a bench-list item fixes it; the remaining findings are wording and cosmetic nits.
