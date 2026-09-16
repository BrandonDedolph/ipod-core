# 05 — Sleep timer

Plan only. Repo at `main` dbce3b8, firmware in `core/`. Nothing here can be verified on the device in this job; the device-only items are called out as such.

## Summary

A Settings > Playback row **Sleep Timer** cycling Off / 15 / 30 / 60 / 90 / 120 min arms a RAM-only countdown. While armed, a small-caps `SLEEP <minutes left>` token sits in the right-hand cluster of the status strip on every list/Settings screen and next to SHUF / RPT on the Now Playing band, counting down once a minute. When it expires the player is paused first, and the device takes the existing `suspend_to_ram()` path — the same one a two-second PLAY hold takes: resume position force-committed, drive parked, panel dark, and on battery the 30-minute escalation to PMU standby. The wake press brings the device back **paused** (unlike a PLAY-hold wake, which resumes): the person fell asleep, and the next press is "where was I", not "play". The timer is disarmed after firing and by any suspend; it is never written to `CORECFG.DAT`, so the row reads Off after every boot.

The countdown is a new pure module `ui/sleeptimer.c` with a `feed(now_us)` API in the style of `ui/keyhold.c` and `screen_battery.c`'s `battwarn_*`, host-tested including a 120-minute run across two 32-bit microsecond wraps. `kernel/main.c` gets thin wiring only: one feed call, one action-code branch, one shared suspend block, and the token painted at the two existing token sites.

## Current behaviour (file:line)

Settings model — `core/ui/settings.c` / `.h`:
- `settings_t` is the persisted record plus the resume locator, which the header already describes as "runtime state, not a user preference … rides along in settings_t only because kernel/config.c's record is the one thing on this device that survives a power cut" (`settings.h:61-107`). That precedent is the hook for a runtime-only field.
- Playback is a three-row table `PLAY_L = { "Shuffle", "Repeat", "Resume" }` (`settings.c:134`); `settings_count(SETTINGS_PLAYBACK)` returns 3 (`settings.c:189`); every Playback row is `SETTINGS_KIND_SELECT` (`settings.c:241`); `settings_value` writes "On"/"Off"/"All"/"One" (`settings.c:283-291`); `settings_activate` toggles/cycles and returns `SETTINGS_ACTION_NONE` (`settings.c:353-365`). The root has nine rows already and scrolls (`ROOT_L`, `settings.c:127-130`); `settings_kind(SETTINGS_ROOT, …)` returns only SUBMENU or ACTION (`settings.c:235-238`).
- Discrete-option cycling precedent: Backlight's `BL_OPTS[6]` / `bl_index` / `bl_step` (`settings.c:96-119`), SELECT wraps through the list (`settings.c:367-373`).
- `settings_action_t` (`settings.h:164-182`): NONE = "record mutated, persist it", NOOP = "nothing to persist", appended last "so every value above keeps its number".
- Row renderer: `screen_settings.c:168-224` `list_render` draws a SELECT row's value right-aligned at x = LCD_WIDTH-16 in `F_SUB` (regular 11) (`screen_settings.c:213`), label at x = 14.

Main loop — `core/kernel/main.c`:
- `settings_activate` is dispatched at `main.c:6410-6467`: ENTER_* codes descend; `SETTINGS_ACTION_DISKMODE` and `_RESET` (`:6428-6458`); `SETTINGS_ACTION_NONE` → `settings_apply(); settings_touch();` (`:6459-6467`) — a touch is a disk write three seconds later (`settings_touch` `:3130`, `settings_commit` `:3154`). RESET goes `settings_defaults(&g_settings); settings_apply(); settings_touch();` (`:6455-6458`). `settings_apply()` (`:2871-2880`) pushes shuffle/repeat/volume/tone/theme.
- PLAY arbitration: `keyhold_feed(&play_key, down, nowp, PLAY_HOLD_US)` at `:5838-5874`; `KEYHOLD_HOLD` calls `suspend_to_ram(keyhold_down_us(&play_key))` and then re-seeds five loop locals: `last_input`, `last_present`, `bl_state = BL_FULL`, `panel_slept = 0`, `dirty = 1` (`:5854-5870`). `PLAY_HOLD_US` is 2 s (`:663`).
- `suspend_to_ram(play_down_us)` (`:5158-5430`): `was_playing = player_active() && !player_paused()` then `player_pause()` (`:5162-5165`); `resume_capture(); settings_commit(1);` (`:5171-5172`); waits out a held PLAY and escalates to `enter_standby()` when held > 5 s since `play_down_us` AND > 2.5 s since dark (`:5223-5242`); idle loop samples the battery and escalates to PMU standby after `SUSPEND_TO_STANDBY_US` (30 min, `:5054-5056`) off external power (`:5375-5383`); on wake it re-boosts, `ata_wakeup()`, `lcd_wake(); paint_current_screen(); lcd_present_fb(); backlight_set()`, and **resumes playback if `was_playing`** and the jack is not known empty (`:5418-5430`). `enter_standby()` is at `:4924-4986`.
- Clock: `USEC_TIMER_ADDR` is the free-running 1 MHz counter, wraps every ~71.6 min. The file's wrap rule and the `ui_window_t` pattern (start stamp + unsigned elapsed compare, disarm on expiry) are at `:91-120`. `battery_due()` / `battery_refresh()` use the same delta form on a 5 s cadence (`:760-770`). The backlight timeout is `idle = now - last_input` compared against `dim_us`/`off_us` (`:6514-6531`). `keyhold_feed` does the same (`ui/keyhold.c:580`), and `battwarn_toast_up` disarms on expiry so a wrap cannot resurrect it (`ui/screen_battery.c:427-444`).
- Loop cadence: every pass pumps the player (`:5667-5670`); the bottom of the loop halts ≤ 10 ms idle / 200 µs playing (`:6867-6900` region, also the banner's own halt `:6740-6748`). Nothing while awake blocks for minutes, so a feed-every-pass accumulator is always fed well inside one wrap.
- Status strip: `status_strip_render()` (`:1006-1067`) — track name at x 12 clipped to `[12, LCD_WIDTH-70)` in `FONT_SMALL` (regular 9); battery at `bx = LCD_WIDTH-12-24 = 284`; padlock at `bx-14 = 270` while `g_locked`. Called by every list painter and by `settings_render_cur()` (`:3294`). The strip is in `main.c`, **not** `ui/chrome.c` (chrome.c holds the header/rows/scrollbar primitives only).
- Now Playing band: `nowplaying_render()` (`:3450-3540`) draws "Now Playing"/"Paused" in bold 12 at x 12 (`:3457`), battery at `bx = 289` (`:3460-3461`), padlock at `bx-14`, and the SHUF / RPT / RPT1 tokens built into `char st[16]` and right-aligned at `rc = bx - (g_locked ? 18 : 6)` in `FONT_SMALL` (`:3466-3478`). This band is only repainted on a **full** repaint (`dirty`); the once-a-second clock tick repaints the transport band alone (`:6768-6840`).
- Hold banner's recoloured strip row duplicates the strip's name + battery (`top_banner_render`, `:4851-4857`), no padlock.
- Partial list repaints are gated on `chrome_key()` (`:4062-4070`: active, paused, queue index, locked, battery pct) — anything the strip shows must be in that key or a selection move could repaint two rows over a stale strip. The battery's 5 s sample sets `dirty` only when the drawn glyph changes (`:5760-5772`).
- Screens: `screen_t` at `:3627-3630`; `paint_current_screen()` at `:4698-4724`.

Config codec — `core/kernel/config.c`: `config_encode` writes every field explicitly by offset (`:296-352`), `config_decode` reads every field explicitly (`:354-446`), and `config_load` copies the decoded `tmp` wholesale into the caller's record (`cand = tmp`, `:693`) — so any new `settings_t` field that decode does not assign arrives in `g_settings` uninitialised.

Tests — `core/tests/ui/settings_test.c` (`count-play` == 3 at `:222`; Test 12 at `:264-300` asserts NONE ⇔ record changed, NOOP ⇔ byte-identical), `tests/ui/keyhold_test.c` (feed-sequence style, wrap case at `:145-154`), `tests/ui/screen_battery_test.c` (clock starts at 0xFFFFFF00 to straddle the wrap), `tests/kernel/config_test.c` (`settings_eq` compares fields explicitly `:225-245`; codec round-trip `:341`). Registration: `tests/meson.build:182-196` (keyhold), `:226-238` (settings), `:480-530` (config). Firmware link: `meson.build:274-282` (`keyhold_hw_lib`), `link_with` list `:377`.

Gallery — `docs/screens/render.py`: `status_strip(sc, left, pct, locked)` `:670-680`; `_now_playing_base(…, locked)` draws the SHUF token at `bx - (18 if locked else 6)` `:853-889`; `screen_clicker()` `:1752-1769` is the template for a small SELECT-row screen; `screen_settings()` `:1587`; outputs listed in `main()` `:2008-2040`. There is no Playback still today.

Docs — `docs/USER_GUIDE.md`: Controls table `:261-268`, status strip `:291-296`, Now Playing shuffle/repeat `:354-355`, Settings > Playback bullet `:375-377`, Power `:569-580`. `README.md:35-36` "Sleeps." bullet, `:148` "58 suites". `core/README.md:910-923` Sleep and power-off, `:824` suite count. `STATUS.md` top dated section `:6`, Testing `:745`.

## Design

### Where the row lives: Settings > Playback, row 3

Own root row rejected. The root table is nine rows and already scrolls; every root row is a SUBMENU or an ACTION (`settings_kind`, `settings.c:235-238`), and a value that cycles in place on the root would be the first SELECT-kind row there — `main.c:6404-6470` and `list_render` would both need a new root case. Theme and Clicker show their value on the root but still descend to a picker. Playback is where Shuffle / Repeat / Resume already cycle on SELECT, it has five spare visible rows, and the user guide already sends people there for the transport toggles. Apple put it at the Settings root; our root is fuller than theirs was. Row 3 of `PLAY_L`, label `"Sleep Timer"` (bold 13 selected = 74 px, regular 12 = 71 px; the value column starts at ≈ LCD_WIDTH-16-46 = 258 for "120 min" in regular 11, so no collision).

### The value: a runtime field in `settings_t`, never on disk

`settings.c` is pure over `settings_t`; the row's value must live there for `settings_value`/`settings_activate` to stay pure and host-tested. Add

```c
int  sleep_timer_min;    /* 0 (off) / 15 / 30 / 60 / 90 / 120 — RUNTIME ONLY:
                          * the chosen duration, never persisted (config.c
                          * neither encodes nor restores it); reads 0 at boot */
```

next to the resume locator with the same "rides along" justification. `settings_defaults` sets 0. `config_encode` ignores it; `config_decode` assigns `s->sleep_timer_min = 0` (one line, because `config_load` copies the whole decoded struct, `config.c:693`). The record's `length` stays 44, `CONFIG_VERSION` stays 2, `tools/make_config.py`, the Go codec and the `verify-hw` resume-parity check are untouched.

**Why not persist.** The countdown is relative to the moment it was armed, and a boot means the device was off, so a stored countdown is meaningless; only the *preference* ("last picked 30") could persist, and that buys one saved SELECT a night at the cost of a payload bump (44 → 45, old records still load) plus host-tool changes on both codecs. Arming happens right before sleep, typically with the drive parked, so the write would be a SOFT commit that lands at suspend's forced commit anyway — but it would also make every arm a disk write. Apple's own timer resets to Off after it fires. RAM-only, row reads Off after every boot. If it is ever wanted, the extension is `P_SLEEP_MIN` at payload offset 44 gated on `len >= 45`, and nothing in this design has to change shape.

### A new action code so arming is not a disk write

`settings_activate(SETTINGS_PLAYBACK, s, 3)` cycles `sleep_timer_min` through `SLEEP_OPTS[6] = { 0, 15, 30, 60, 90, 120 }` (same shape as `BL_OPTS`/`bl_step` with wrap; an unknown value indexes to 0 = Off) and returns a new `SETTINGS_ACTION_SLEEPTIMER`, appended after `SETTINGS_ACTION_NOOP` with a comment (nothing on disk stores these; NONE would make main.c `settings_touch()` and write a byte-identical record — the exact spurious write the NOOP work removed, `settings.h:156-163`). Semantics, documented in `settings.h`: "the record changed, but only its runtime part — apply, do not persist".

`settings_value(SETTINGS_PLAYBACK, s, 3)` writes `"Off"` or `"<n> min"` (a `u_to_str` + `scopy(" min")`, like Backlight's `" sec"`). The row shows the **chosen duration**, not the countdown; the countdown is in the strip. (A wheel-step through `settings_adjust` is not reachable on SELECT rows — the wheel moves the selection unless editing a slider, `main.c:6374-6400` — same as Backlight today, so six SELECTs cycle back to Off. Acceptable; note in the guide.)

### `ui/sleeptimer.c` / `.h` — the pure countdown

Freestanding, integer-only, no clock of its own, no `settings.h` dependency (main.c hands it minutes).

```c
typedef struct {
    uint8_t  armed;
    uint8_t  total_min;      /* the duration it was armed with, 0 = off      */
    uint16_t elapsed_min;    /* whole minutes accumulated since arm          */
    uint32_t last_us;        /* clock at the last feed                       */
    uint32_t acc_us;         /* sub-minute remainder, always < 60 000 000    */
} sleeptimer_t;

typedef enum { SLEEPTIMER_NONE = 0, SLEEPTIMER_TICK, SLEEPTIMER_FIRE } sleeptimer_event_t;

void sleeptimer_reset(sleeptimer_t *t);                              /* disarm */
void sleeptimer_arm(sleeptimer_t *t, int minutes, uint32_t now_us);  /* 0 disarms; restarts from now */
int  sleeptimer_armed(const sleeptimer_t *t);
int  sleeptimer_total_min(const sleeptimer_t *t);                    /* 0 when off */
int  sleeptimer_remaining_min(const sleeptimer_t *t);                /* total - elapsed; 0 when off; never 0 while armed */
sleeptimer_event_t sleeptimer_feed(sleeptimer_t *t, uint32_t now_us);
int  sleeptimer_token(const sleeptimer_t *t, char *buf, int buf_sz); /* "SLEEP 45", or "" and 0 when off */
```

`feed`: if not armed return NONE. `delta = (uint32_t)(now_us - last_us)` (wrap-correct, the `keyhold_feed` form); `last_us = now_us`; `acc_us += delta`; `while (acc_us >= 60000000u) { acc_us -= 60000000u; elapsed_min++; }`. If `elapsed_min >= total_min`: `sleeptimer_reset(t)` and return FIRE (one-shot, disarmed — the `battwarn_toast_up` rule: an expired thing cannot come back on the next wrap). Else return TICK when `remaining` changed during this feed, NONE otherwise. Contract: **feed at least once per 71 minutes** — the main loop feeds every pass (≤ 10 ms halts), and the timer is disarmed before any code path that stops the loop for longer (suspend, standby, disk mode). `remaining_min` at arm is `total` (shows 15, not 14, for the first minute — the original iPod's counting), then `total - elapsed_min`. Exactness with coarse feeds: 120 min of 5 s feeds, 100 ms feeds, or irregular feeds all accumulate to 120 minutes ± 0 because the remainder is carried, never truncated.

Token: `"SLEEP "` + decimal minutes; widths measured with the real atlas (`text_width`, regular 9): `SLEEP 120` = 48 px, `SLEEP 15` = 42, `SLEEP 1` = 36. Two-token cluster `SHUF RPT1 SLEEP 120` = 107 px.

### Wiring in `kernel/main.c` (thin)

1. `static sleeptimer_t g_sleep;` beside `g_settings` (`:547`). A local helper `sleep_timer_apply(void)`: `sleeptimer_arm(&g_sleep, g_settings.sleep_timer_min, now)` when the two disagree (`g_settings.sleep_timer_min != sleeptimer_total_min(&g_sleep)`), plus a `core: sleep timer: armed N min` / `off` line for the event log. Invariant stated at the helper: `g_settings.sleep_timer_min == sleeptimer_total_min(&g_sleep)` at every loop top — main.c owns keeping both sides in step, and the settings model only ever moves the left side.
2. Settings SELECT dispatch (`:6410-6467`): `else if (act == SETTINGS_ACTION_SLEEPTIMER) { sleep_timer_apply(); }` — no `settings_apply()`, no `settings_touch()`. RESET branch (`:6455-6458`) already zeroes the field via `settings_defaults`; add `sleep_timer_apply();` after it so Reset Settings disarms.
3. Feed once per pass, right after the PLAY keyhold block (`:5874`), from the same `nowp` stamp:
   ```c
   switch (sleeptimer_feed(&g_sleep, nowp)) {
   case SLEEPTIMER_TICK: dirty = 1; break;           /* the token's minute changed */
   case SLEEPTIMER_FIRE:
       g_settings.sleep_timer_min = 0;               /* feed disarmed it; keep the row honest */
       uart_puts("core: sleep timer: expired, sleeping\n");
       if (player_active() && !player_paused()) player_pause();  /* so suspend's wake stays paused */
       want_suspend = 1; suspend_origin = nowp;
       break;
   default: break;
   }
   ```
   and refactor the `KEYHOLD_HOLD` case (`:5854-5870`) into the same shape: it sets `want_suspend = 1; suspend_origin = keyhold_down_us(&play_key);`, and ONE block after both does `sleeptimer_reset(&g_sleep); g_settings.sleep_timer_min = 0; suspend_to_ram(suspend_origin);` followed by the existing five-local re-seed. That is the "disarmed by any suspend" rule in one place, and the FIRE site does not duplicate the bookkeeping. `suspend_to_ram` itself is **unchanged**: with the player already paused, `was_playing` is 0 and the wake does not resume (`:5162-5165`, `:5422-5430`); the release-wait loop exits at once because PLAY is not down; the escalation test `nowh - play_down_us > 5 s` is against a stamp taken this pass and PLAY is up, so it cannot escalate; the 30-minute battery escalation to PMU standby applies as today.
   Why pause before calling rather than a new parameter: no signature change to the hot function, and `resume_capture()` inside then records the paused position — exactly the "resume position is captured" requirement.
4. Token at the two paint sites, via one static `strip_sleep_token(int right_x, int y, uint16_t ink)` that draws `sleeptimer_token()` right-aligned in `FONT_SMALL` and returns the new right edge (or `right_x` untouched when off):
   - `status_strip_render` (`:1006`): compute `right = g_locked ? bx - 14 - 6 : bx - 6`; draw the token there; clip the name to `min(LCD_WIDTH-70, token_x - 8)`. Budget: battery 284, padlock 270, token `SLEEP 120` x ≥ 216 when locked / 230 unlocked, name clip 208 / 222 versus 250 today — a long track name loses 28–42 px only while the timer is armed.
   - `nowplaying_render` (`:3466-3478`): append `" SLEEP n"` to the `st[]` cluster (grow `st[16]` → `st[32]`; the cluster is one `text_width` so kerning is right). Worst case `SHUF RPT1 SLEEP 120` = 107 px right-aligned at `rc` = 283 (265 locked) starts at x 176 (158); "Now Playing" in bold 12 ends at 12 + 77 = 89. ≥ 69 px of air; no width problem on this band.
   - `top_banner_render`'s strip row (`:4851-4857`): same helper with the banner's `sub` colour, so the 1 s Hold banner does not blink the token off. Low priority; skip if the diff is already noisy.
5. `chrome_key()` (`:4062`): `k = k * 31u + (uint32_t)sleeptimer_remaining_min(&g_sleep);` — a partial two-row repaint must not leave a stale strip minute behind, the same reason `g_bat_pct` is in the key.
6. Repaint cadence: TICK sets `dirty` once a minute → a full frame on Now Playing (same cost class as the battery glyph's change every 5 s) and a full list paint (the partial path is defeated by the key). Nothing paints while `bl_state == BL_OFF`; the token is simply current at the next lit paint. No new partial-present rect.
7. Wake bookkeeping already re-seeds `last_input` etc. after the shared suspend block; nothing else is needed for the backlight.

Untouched on purpose: `suspend_to_ram`, `enter_standby`, `ui/chrome.c`, `ui/screen_settings.c` (the generic SELECT-row path renders the new row as-is), `cfg_commit.c`, the Go CLI, `tools/`.

Behavioural notes to write down in the header comments:
- Fires regardless of what the player is doing (a paused or idle device still sleeps — that is what the feature means) and regardless of screen, Hold switch included (a locked, pocketed device is the canonical case; wake then needs Hold off plus a press, as today's PLAY-hold suspend does).
- Not reset by input: it is a duration, not an idle timeout.
- The row's SELECT re-arms from now each time the value changes (cycling around to the same number is still a fresh arm).
- Any suspend (timer, PLAY hold), PMU standby, disk mode, or Reset Settings leaves the timer Off.

## Files to change

New:
- `core/ui/sleeptimer.h`, `core/ui/sleeptimer.c` — the module above (~120 lines with comments).
- `core/tests/ui/sleeptimer_test.c` — host suite (below).
- `docs/screens/playback.png` (new still; `nowplaying_sleep.png` optional), produced by `render.py`.

Edited:
- `core/ui/settings.h` — `sleep_timer_min` field (+ comment), `SETTINGS_ACTION_SLEEPTIMER` after NOOP, header prose listing the new row.
- `core/ui/settings.c` — `SLEEP_OPTS`/`sleep_index`/`sleep_step` next to `BL_OPTS` (`:96-119`); `PLAY_L` → 4 rows (`:134`); `settings_count` PLAYBACK → 4 (`:189`); `settings_defaults` (`:157`); `settings_value` case 3 (`:283-291`); `settings_activate` case 3 (`:353-365`).
- `core/kernel/config.c` — `config_decode` sets `s->sleep_timer_min = 0` (`:384` area, beside the v1 fields); one comment line in `config_encode` saying the field is deliberately absent.
- `core/kernel/main.c` — include, `g_sleep`, `sleep_timer_apply`, `strip_sleep_token`, the feed + shared-suspend block (`:5838-5874`), the SELECT dispatch branch and RESET (`:6410-6467`), `status_strip_render` (`:1006-1067`), `nowplaying_render` tokens (`:3466-3478`), `chrome_key` (`:4062`), optionally `top_banner_render` (`:4851`). Expect ~70 added lines.
- `core/meson.build` — `sleeptimer_hw_lib` (copy of `keyhold_hw_lib`, `:274-282`) and add to `link_with` (`:377`).
- `core/tests/meson.build` — `sleeptimer_test` after `keyhold_test` (`:196`), `suite: 'unit'`; `settings_test` and `config_test` sources unchanged.
- `core/tests/ui/settings_test.c` — see below.
- `core/tests/kernel/config_test.c` — see below.
- `docs/screens/render.py` — `status_strip(..., sleep=None)`, `_now_playing_base(..., sleep=None)`, new `screen_playback()` after `screen_clicker()` (`:1752`), `PLAY_L`/`SLEEP_OPTS` mirrored as constants with a comment naming `settings.c`, outputs added in `main()` (`:2026-2031`).
- `docs/USER_GUIDE.md`, `README.md`, `core/README.md`, `STATUS.md` — below.

## Tests to add

`tests/ui/sleeptimer_test.c` (xfail.h style, feed sequences like `keyhold_test.c`):
1. Reset/off: `armed` 0, `remaining` 0, `token` writes "" and returns 0, `feed` returns NONE forever.
2. Arm 15: `remaining` 15 immediately; 10 ms feeds for 59.99 s → still 15, all NONE; the feed that crosses 60 s → TICK and 14; no second TICK in the same minute.
3. Fires exactly once: arm 1 min, feed past 60 s → FIRE, then `armed` 0, `remaining` 0, further feeds NONE (the finger-off / clock-wrap rule).
4. 120 minutes across two wraps: clock starts at `0xFFFFFFFFu - 30 000 000u`, 100 ms feeds (72 000 feeds), assert TICK at every 60 s boundary and FIRE on the 120th minute, never earlier or later, and remaining is monotone 120 → 1.
5. Coarse and irregular feeds: arm 90, feed with 5 s steps, then alternating 7 s / 13 s / 1 s steps; total elapsed minutes equal the true sum (the remainder is carried, no truncation drift).
6. Re-arm restarts: arm 30, run 10 min, arm 60 → remaining 60; arm 0 → off.
7. Token formatting: "SLEEP 120", "SLEEP 9", "SLEEP 1"; buffer of 8 bytes gets "" (bounded).
8. Never shows 0 while armed: at every feed, `armed` implies `remaining >= 1`.

`tests/ui/settings_test.c`:
- `count-play` 3 → 4 (`:222`); label `settings_label(SETTINGS_PLAYBACK, 3) == "Sleep Timer"`; kind SELECT; default 0 and value text "Off".
- Cycle: six activates walk Off → 15 → 30 → 60 → 90 → 120 → Off, each returning `SETTINGS_ACTION_SLEEPTIMER` (never NONE), value text "15 min" … "120 min"; an out-of-table value (e.g. 45) steps to Off then 15.
- Test 12 grows a third class: SLEEPTIMER ⇒ the record changed (so main.c must not treat it as NOOP) — and a comment that it is the runtime part only.
- `settings_defaults` after a Reset gives `sleep_timer_min == 0`.

`tests/kernel/config_test.c`:
- Encode with `sleep_timer_min = 90` and with 0: the two records are byte-identical (the field is not on disk).
- Decode of any valid record yields `sleep_timer_min == 0` even when the `out` struct was pre-filled with 0x5A bytes (the `cand = tmp` hazard at `config.c:693`).
- `settings_eq` deliberately does NOT compare the field (so the existing round-trip cases stay green with `in.sleep_timer_min` set) — say so in its comment.

Verification: `make sim && meson test -C build-sim` (suite count +1: fix the number in README.md `:148`, core/README.md `:824`, STATUS.md Testing `:745` — confirm with `meson test -C build-sim --list | wc -l`), `make hw && make verify-hw` (`-Werror`, size and layout checks; `check_hw_consistency` is unaffected). `docs/screens/render.py` regenerated from a clean tree.

Device-only, cannot be tested in this job: the actual suspend from a timer (no button held at entry), the wake-stays-paused behaviour, the 30-minute escalation from a timer-initiated suspend, and the token's look on the panel. Say so in STATUS.md.

## Docs to update

- `docs/USER_GUIDE.md`
  - Settings > Playback bullet (`:375-377`): add "Sleep Timer: Off, 15, 30, 60, 90 or 120 minutes. Select cycles the value and starts the countdown at once. When it runs out playback pauses and the device sleeps as if you had held Play; the next press wakes it paused where you left off. It is not remembered across a restart, and any sleep or a Reset turns it off."
  - The status strip (`:291-296`): "…and `SLEEP` with the minutes left while the sleep timer is running."
  - Now Playing (`:354-355`): the SHUF / RPT sentence gains SLEEP.
  - Power (`:569-580`): a **Sleep timer** bullet after **Sleep**, including the battery escalation ("thirty minutes later, on battery, it powers itself off like any sleeping device").
  - Insert `screens/playback.png` beside the Settings table, or reference it in the Playback bullet.
- `README.md`: "Sleeps." bullet (`:35-36`) gains "or on a timer"; Settings blurb (`:95`) unchanged unless the still is added; suite count (`:148`).
- `core/README.md`: "Sleep and power-off" (`:910-923`) — one sentence on the timer taking the same path with the player paused; suite count (`:824`).
- `STATUS.md`: a `## 2026-09-16 — Sleep timer, UNFLASHED` entry at the top in the house style (what changed, the invariant, the bench list: arm 15 on Now Playing → token counts down → device dark at 0 → press wakes paused with the row reading Off; PLAY-hold while armed → wake shows Off; Reset Settings → Off; Hold on + timer → sleeps, wakes after Hold off + press). Bump the suite count in Testing.
- `CHANGELOG.md`: nothing until the next tag (it is per release); the STATUS entry is what `tools/release.py` will fold in.
- `docs/screens/README.md`: no rule change; the new still follows the existing "firmware is the source of truth" note.

## Acceptance criteria

1. Settings > Playback shows a fourth row, Sleep Timer, reading Off after every boot; SELECT cycles Off → 15 → 30 → 60 → 90 → 120 → Off min; no disk write results from arming (no `settings_touch`; `cfg_commit` pending flag unchanged — provable on the host by the action code and on the device by Boot Details' CFG seq not moving).
2. While armed, every list/Settings screen's strip and the Now Playing band show `SLEEP n`, `n` = minutes left, starting at the chosen value and dropping once a minute; the track name shortens to make room; the padlock and battery never move.
3. At expiry: playback pauses, the resume position is captured and force-committed, the device suspends exactly as a PLAY hold does; on battery it escalates to PMU standby after 30 minutes; a button wakes it **paused** on the same track and screen, with the row reading Off and no token.
4. A PLAY-hold suspend, PMU standby, disk mode, or Reset Settings while armed leaves the timer Off.
5. A 120-minute timer is exact to the minute across the 32-bit microsecond wrap (host test 4), and the token never shows 0.
6. Host suites all green (58 + 1); `make hw` clean under `-Werror`; `make verify-hw` clean; `render.py` regenerates without touching stills that did not change.
7. User guide, README, core/README and STATUS updated; STATUS marks the feature UNFLASHED with the bench list.

## Risks

- **`kernel/main.c` is the merge hot spot.** The keyhold/suspend block refactor (`:5838-5874`) touches the same lines the power-saving and hold-banner work keeps touching. Keep the refactor minimal (two flags + one block) and land it as its own commit before the token/paint changes.
- **Uninitialised runtime field at boot.** If the `config_decode` zeroing is forgotten, `config_load`'s `cand = tmp` copies stack garbage into `g_settings.sleep_timer_min` and `sleep_timer_apply` (if it runs at boot) would arm a random timer. The config_test case pins it; also do not call `sleep_timer_apply` from the boot path — the timer starts disarmed by `.bss`.
- **Two sources of truth.** `g_settings.sleep_timer_min` (what the row shows) and `g_sleep` (what runs) can drift if any new disarm site forgets one side. Every disarm goes through the shared suspend block or `sleep_timer_apply`; the FIRE branch zeroes the field explicitly. State the invariant at the helper.
- **Feed starvation.** The accumulator assumes a feed at least every ~71 min. All long blocking paths (suspend, standby, disk mode, boot library load) either disarm first or run before any arm is possible. A future long blocking path inside the loop (a multi-minute library rescan, say) would need a disarm or a feed — note it in `sleeptimer.h`.
- **Timer-initiated suspend is device-unverified.** Same `suspend_to_ram`, but entered with no button down and possibly with Hold on; the wake path's drain and swallow loops are documented to cope, and the PLAY-then-Hold case was fixed on the device, but this entry has never run there. If a fired timer is found to wake badly, the first bisect is to enter with a synthetic 2 s "hold" origin (`suspend_origin = nowp - PLAY_HOLD_US`) — it changes nothing in the loop but proves whether the origin matters.
- **Wake stays paused, unlike PLAY-hold.** Intentional, argued above; a reviewer may want parity. The alternative is one line (skip the `player_pause()` before the suspend) and is easy to flip after a device night.
- **`USEC_TIMER` under frequency scaling.** The plan assumes the 1 MHz counter is unaffected by the 30/80 MHz CPU boost (keyhold, backlight and battery cadence already rely on that while idled at 30 MHz). The PLL park is suspend-only and the timer is disarmed there.
- **Strip width on lists.** With a token, a locked device shows the name clipped at x 208 (was 250). If that reads cramped on the panel, the fallback is to drop the token from the list strip and keep it on Now Playing only — the helper makes that one call site.
- **Six presses to cycle back to Off.** Same as Backlight; MENU leaves the row armed. Documented; a wheel-step on SELECT rows would be a separate settings-UI change.

## Conflict surface

`core/kernel/main.c`:
- `run_ui` main loop: the PLAY keyhold block and its `KEYHOLD_HOLD` case (`:5838-5874`) → refactored into flags + one shared suspend block; the Settings SELECT dispatch (`:6410-6467`, new `else if` and the RESET branch); a new feed call after the keyhold block.
- `status_strip_render` (`:1006-1067`): right cluster and the name clip.
- `nowplaying_render` (`:3450-3540`): the `st[]` token cluster (`:3466-3478`).
- `chrome_key` (`:4062-4070`): one more term.
- `top_banner_render` (`:4825-4875`): strip row, optional.
- New statics near `g_settings` (`:547`) and near `draw_battery` (`:975`): `g_sleep`, `sleep_timer_apply`, `strip_sleep_token`.
- Not touched: `suspend_to_ram`, `enter_standby`, `settings_apply`, `resume_capture`, `paint_current_screen`, the backlight/idle/spin-down logic.

`core/ui/settings.c` / `.h`: `settings_t` (new field), `settings_action_t` (new code after NOOP), `PLAY_L`, `settings_count`, `settings_defaults`, `settings_value`, `settings_activate`; a new option table beside `BL_OPTS`. `settings_kind` unchanged (PLAYBACK is all SELECT). Anyone adding another Playback row at the same time collides on the index 3 cases.

`core/kernel/config.c`: one assignment in `config_decode`; anyone touching the v2 payload layout at the same time should know this field is deliberately not in it.

`core/ui/chrome.c`: no change (the strip is not there; the brief's pointer to `chrome.c status_strip_render` is stale — it is `main.c:1006`).

`core/ui/screen_settings.c`: no change.

Build/test registration: `core/meson.build` (`link_with`, `:377` — a one-line list every new hw lib edits), `core/tests/meson.build` (a new block after keyhold), `tests/ui/settings_test.c` (`count-play` and Test 12), `tests/kernel/config_test.c`.

Docs: `docs/screens/render.py` (`status_strip`, `_now_playing_base`, `main()`), `docs/USER_GUIDE.md` (Settings, strip, Now Playing, Power), `README.md`, `core/README.md`, `STATUS.md` (top section and Testing). Suite counts are quoted in three places and all three go stale together.
