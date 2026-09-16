# Review: feat/clock (plan 14-clock) — round 1

Worktree `/home/brando/Projects/ipod_theme/.claude/worktrees/clock`, HEAD `ce6ffa6`, nine commits on
top of `main 5fac8e4`, 65 files. Reviewed against `scratchpad/plans/14-clock.md`.

## What I ran (fresh build dirs, not the implementer's output)

| Command | Result |
|---|---|
| `rm -rf build-sim && make sim && meson test -C build-sim` | 67/67 OK, no warnings; `datetime`, `timesync`, `settime`, `hw-rtc` present, `config`/`settings` extended |
| `rm -rf build-hw && make hw && make verify-hw` | both rc 0 under `-Werror`, `build_index: PASS` |
| `cd core/cli && go vet ./... && go test -count=1 ./...` | vet clean, every package ok |
| `go test -count=1 -v -run 'Stamp\|ConfigTime\|Eject\|Clock'` | `TestStampMatchesMakeConfig` **ran and PASSED** (not skipped); the other stamp/doctor/eject/sync tests pass |
| Python cross-check of `kernel/datetime.c` (own harness, `scratchpad/dt_check.py`) | 4 286 epochs — every Feb 28 / Feb 29 / Mar 1 / Dec 31 / Jan 1 boundary 2000–2099 at 00:00:00, 23:59:59 and the second before, the plan's hand vectors, 3 000 random — vs `datetime.fromtimestamp(..., UTC)`: **0 mismatches** in date, weekday, round trip, 12 h and 24 h formatting |
| `docs/screens/render.py` in the worktree (`CORE_FONTS_DIR` + a temporary symlink to the untracked `art/`) | every still byte-identical to the committed PNGs except `boot.png`, `boot.gif`, `bootdetails.png`, `hero.gif`, `loading.png`, `loading_onyx.png`, which embed `git describe` (`render.py:64-73`) and so change with HEAD, not with this branch. `settings.png` and the three new stills regenerate identically. Acceptance 7 holds. |

## Findings

No blockers. Five should-fix, a handful of nits.

### Should-fix

**S1 — `core/ui/screen_settings.c:315-323`: the Date & Time list screen is titled "Settings".**
`settings_render()`'s title switch has no `SETTINGS_DATETIME` case, so it falls to `default: title =
"Settings"`. `docs/screens/render.py:1683` (`header(sc, "Date & Time", back=True)`) and the committed
`datetime.png` show "Date & Time", as does the plan (§4 "`Date & Time` title in `settings_render`").
The gallery's rule is that a still is a claim about what the device draws; this one is false. One
`case`. Read, not run (the sim cannot be driven headlessly here).

**S2 — `core/kernel/main.c:7688-7702` (Reset Settings) drops `utc_off_min` until the next boot, and
`ui/settings.h:180-182` says otherwise.**
The reset restores `host_epoch`/`host_off_min` but not `utc_off_min`, and `settings_defaults()` zeroes
it. Nothing in the running session copies the host offset back (only the boot timesync block does), so
from the Reset until the next boot the strip, the header and the Date & Time row show **UTC**. Worse:
a manual Set Date & Time in that window computes `utc = local - 0`, writes local-as-UTC to the chip, and
the next boot's STALE action then copies the host offset back and the clock jumps by the zone. The
`settings.h` comment "they are the only two of the six a Reset returns to their defaults" is wrong —
four are (`time_24h`, `time_in_title`, `utc_off_min`, `applied_epoch`); `docs/USER_GUIDE.md` "The clock
keeps running ... kept across a reset" is true of the RTC but not of what is displayed. The
implementer's own rationale ("the host's fields are not a preference") applies to `utc_off_min` too:
preserve it across the reset, and make the header comment count correctly. Constructed by reading the
reset arm and `settings_defaults()`.

**S3 — `core/kernel/main.c` boot block (~6345): the FORCE commit's battery gate is never evaluated.**
The block calls `settings_commit(CFG_COMMIT_FORCE)` before `battery_refresh(1)` at `main.c:6398`.
`bat_level` is statically `BATTERY_LEVEL_OK` (`hal/hw/battery.c:333`) and `battery_init()` takes no
sample (`battery.c`, `battery_policy_reset()` only), so `battery_disk_writes_allowed()` is trivially 1
at that point and the comment "FORCE is not a promise: the gate can still refuse (a cell below the
disk-safe line)" describes a protection that does not exist for this write. Consequence today: the
`clock_mark_unsaved` re-arm path can only be reached by a write *failure* (`config_save` rc < 0), never
by a refusal. The 1 KB write happens while the platters are already up from the mount, so the actual
risk is small, but either sample first or say plainly that this write is pre-gauge. Read; confirmed by
grepping every `battery_refresh(`/`battery_sample(` caller.

**S4 — `core/hal/hw/rtc.h:97` `INT1_ALARM 0x80` and the `06-power.md` row "`INT1`/`INT1M` bit 7 =
alarm" are probably wrong.** I could not reach the PCF50606 datasheet from this session (nxp.com,
openmoko wiki and datasheet mirrors all refused), so this is from my recollection of the public map and
carries that caveat: INT1 is ONKEYR `0x01`, ONKEYF `0x02`, ONKEY1S `0x04`, EXTONR `0x08`, EXTONF `0x10`,
SECOND `0x20`, **ALARM `0x40`**, bit 7 unused. Nothing executes it (the alarm is tabled, not
implemented), but the table's "medium" confidence should read "to confirm", and the define should not
ship a value I believe is off by one bit. On the rest of the table my recollection **agrees** with the
branch: RTCSC..RTCYR at `0x0A..0x10`, alarm at `0x11..0x17`, BCD, 24-hour hours, binary weekday;
OOCC1 GOSTDBY `0x01`, RTCWAK **`0x10`**, CHGWAK `0x20`, EXTONWAK as a two-bit field at `0x40/0x80` — so
the doc's old "RTCWAK 0x80" would actually set EXTONWAK-low, and the branch was right to flag the
CONFLICT and write nothing there. One more clause worth adding to the cross-check argument: in the
datasheet's naming ADCC1 is `0x2E` and ADCC2 (`0x2F`) is the mux+start register; the doc's "`ADCC1
0x2F`" is the datasheet's *address* for the register the firmware actually pokes, under the wrong name.
The address cross-check holds by function, not by name; the doc should not claim "sits at its datasheet
address" for that row without saying so.

**S5 — `design_reference/README.md:36-40`: the new paragraph is inside the file table.** It was
inserted between the `ipod-frame.jsx` and `volume-demo.jsx` rows, so the last two rows now render as a
header-less table fragment. Move it below the table. Read.

### Nits

**N1 — `main.c clock_resync()` on the wake path with `rc < 0`.** The "-1 carry" is right for the
30-minute cadence, but at `clock_resync("wake")` it folds `now - last_us` — the very delta the block
above says cannot be trusted across a park — so if the first read after a wake gets no answer the clock
is wrong (by up to the suspend length) for at most `RTC_RETRY_S` = 60 s, then the retry adopts the chip
and logs a DRIFT. Bounded and rare; a wake read failure could simply reset, or the comment could state
the bound.

**N2 — the wake line will normally say `drift`.** If the µs counter stalls in the park, every wake
resync reports `drift` ≈ the suspend length. `clock_resync()`'s comment calls a drift "one of the two
clocks is not what we think it is"; bench step 5 in `STATUS.md` should say a drift equal to the sleep
is the expected shape. DEVICE item; comment only.

**N3 — `main.c:6649` "the band-only present the battery gauge already uses does the rest".** The
minute edge sets `dirty = 1`, which is a full paint and present; the band-only present at `main.c:7966+`
belongs to the Hold banner. Harmless (one present a minute on idle screens), but the comment overstates.

**N4 — plan tests not written:** `cli/sync_test.go` for the plan/report lines, and the app's
fake-backend Eject test. The shared `internal/eject.Eject` wrapper makes the second moot; the
`sync.go` printing is untested.

**N5 — editor edge at the zone boundary.** `hal_rtc_set` returns `-3` for 2001-01-01 00:xx local with a
positive offset (UTC lands in 2000) and 2099-12-31 23:xx with a negative one; the editor returns to
Date & Time showing the old value with only a UART line. Acceptable; the guide's "the year stops at
2001 and 2099" is the whole story for offset 0 only.

**N6 — `06-power.md:343`** keeps "Source: `firmware/export/pcf5060x.h`" (a Rockbox header) as the
citation for the register block the branch extends. Pre-existing cleanroom smell, out of scope, but
the branch now leans on that line for its cross-check argument.

## Coverage of the ten asks, with evidence

1. **`hal/hw/rtc.c`.** Register table vs datasheet: see S4 for what I can and cannot confirm; every
   address in `rtc.c` and the `06-power.md` table carries a confidence note, and `rtc.h` restates the
   provenance. BCD: `bcd_to_int` rejects any nibble > 9 (`rtc.c:56-64`); `rtc_decode` then requires a
   real calendar date via `datetime_valid` and year ≠ 00. Torn read: `rtc_read_once` reads SC+3, DT+2,
   SC again, and only `last < first` (both valid BCD) counts as a tear; one retry, a second tear is
   accepted as-is (comment says why). The trace test pins the exact pointer-read grammar, the retry,
   `0x4A`, year 00, 31 Feb and the NACK shape `{0x0A,0x0E,0x0A}`+stale bytes → unset with exactly one
   pass, and the wedged bus → `-1` with zero DATA latches. No write on a read path: `rtc_read_once`
   calls only `i2c_read` (the register-pointer byte is inherent to the read primitive); the trace
   asserts no `expect_write_reg` in `rtc_read`. Same path as the battery: `battery.c:238/248` and
   `rtc.c` both use `i2c_send`/`i2c_read` on `PMU_ADDR 0x08`; the trace test builds its framing with
   `battery_trace_test.c`'s helpers.
2. **`datetime.c`.** Exhaustive 36 525-day loop passes; my independent Python check (above) agrees on
   every leap day and year boundary 2000–2099, the 2038 roll, and 12/24 h formatting including 00:05 →
   "12:05 AM" and 13:00 → "1:00 PM". `datetime_local` refuses out-of-range offsets and shifts that leave
   the range. Buffer rules (`DATETIME_TIME_MAX 9`, `DATE_MAX 32`) are enforced and tested.
3. **`wallclock.c`.** Consecutive-sample unsigned subtraction; `whole_seconds()` never forms
   `frac + delta` in 32 bits; the test crosses two counter wraps in a two-hour feed, carries a
   333 333 µs remainder exactly, and folds a 4 000 s delta. Re-anchor after wake: `clock_resync("wake")`
   at `main.c:6125` after `suspend_lowpower_leave()` (I²C clocks back) and `ata_wakeup()`; PMU standby
   is a cold boot so it re-runs the boot block. The -1 carry: `main.c clock_resync()` ticks instead of
   resyncing when `hal_rtc_get` returns -1 and the clock is valid, and retries in `RTC_RETRY_S`; the
   module itself resets on `rtc_valid == 0` (tested) and the boundary is documented in both places.
   Drift bound: `WALLCLOCK_DRIFT_S 5` logged, chip adopted; `STOPPED` when the chip reads the anchor's
   second after ≥ 2 s — tested both ways. See N1/N2.
4. **`timesync.c`.** Rules 1–6 in the plan's order; each has a test row. "Applied stamp" prevents a
   re-apply (`one_stamp_three_boots`: SET, NONE, NONE, RTC never rewritten). Unset RTC → SET (rule 5
   cannot fire). Stamp older than the RTC by ≥ 600 s → STALE, by < 600 s → SET. Different zone: the
   epoch is UTC, the offset is copied on SET and STALE (`dst_offset`). All-zero record → NONE. 48-byte
   record → `config_decode` zeroes the block (`config_test` "a 48-byte record reads as never stamped";
   Go `TestDecodeConfigTimeAbsent`) → NONE.
5. **The record.** Payload 48..63 exactly as the plan's table (`config.c` `P_HOST_EPOCH`..`P_TIME_PAD2`,
   `make_config.py TIME_FIELDS`, `clock.go pHostEpoch..pUTCOffMin`); `CFG_PAYLOAD_V2T = V2S + 16 = 64`
   (07-18 had already landed at 48, so the literal-vs-chain concern is moot). Decode gated on
   `len >= 64` and tested at 63. `CONFIG_VERSION 2u` unchanged (`config.h:110`; the diff touches only
   comments). Encode clamps offsets both ways, epochs verbatim. `--emit` and `EncodeConfigSlot` still
   write length 48 and the C fixture test asserts "no stamp". `make_config.py --stamp` + `config.go`
   moved in one commit (`99718d3`) with the goldens untouched; the Go stamper writes the *other* slot
   with `seq+1`, tail verbatim (`TestStampConfigTimePreservesTheDevicesBytes` checks the resume locator,
   queue context, sound tail and the firmware half of the block), CRC over `[0,1020)`, file size and a
   marker at byte 4096 preserved (no-truncate proof), `OpenWriteThroughExisting` is `O_RDWR` /
   `OPEN_EXISTING|FILE_FLAG_WRITE_THROUGH`. Python and Go agree byte for byte (parity test ran).
6. **Boot apply.** Traced `main.c:6285-6380`: one `rtc_read_raw` + pure `rtc_decode` (three I²C
   transactions, logged bytes = decided bytes, pinned by `test_decode_is_pure`), `wallclock_anchor`,
   `timesync_decide(config_writable(), ...)`, `hal_rtc_set` on SET (failure → act = NONE, mark
   untouched), `timesync_apply` → `settings_touch(); settings_commit(FORCE)` exactly once, before
   `theme_set`/`library_ensure`. Refused/failed FORCE: `clock_mark_unsaved = g_cfg_commit.dirty`, and
   after `cfg_commit_clear()` at `main.c:6450` a `settings_touch()` re-arms it for the idle path
   (`cfg_commit_result` keeps `dirty` on rc < 0). DISKSAFE gate: see S3 — not actually evaluated at
   this point. Parked drive: not parked at boot (the mount just spun it); FORCE would `WRITE_WAKE`
   anyway. Sleep timer / gesture / jackwatch: all reach the same `suspend_to_ram` wake block, which
   re-anchors before the first paint; no interaction with the boot block.
7. **Settings.** Root indices: `settings_kind` 8/9, `settings_activate` 5..9, `settings_test`
   (labels asserted by index), `render.py ROOT_L`, `settings.png` regenerated; `main.c` indexes root
   rows only through action enums (grepped: no numeric root index). Date & Time rows: SUBMENU with an
   injected value (`settings_set_now`, "Not set" for invalid or < 2000), two SELECT toggles whose SELECT
   returns NONE → `settings_apply(); resume_capture(); settings_touch()` in the existing branch, so
   12/24 h and Time in Title persist via the debounced save and the SOFT commit on leaving Settings.
   `settings_adjust` steps them but main.c routes only sliders to it — same as Backlight today, so not a
   regression; the test's "the wheel steps" claim is about the module. Key map: wheel → `settime_adjust`
   (±4 clamp inside the model), SELECT → `settime_next`, last plate → `settime_civil` → local→UTC with
   the stored offset → `hal_rtc_set` → re-anchor, back to Date & Time; MENU → back with nothing
   written (verified: the arm has no `hal_rtc_set` on that path; `settime_test` proves done-exactly-once
   and day ≤ mdays under any field order). The I²C write from the UI thread is safe: every I²C user
   (`hal_volume_set`, `battery_refresh` incl. the suspend loop, `power_standby`, `rtc.c`) is called
   from `main.c`'s loop; `i2c.c` masks IRQs only around DEV_EN/DEV_RS RMW; the timer ISR and the audio
   DMA path touch no I²C — cooperative single-writer, no lock needed, and the WM8758 volume writes share
   that same discipline. Plates: every colour is a `g_pal` token (`S_SEL_BG/S_SEL_FG/S_BORDER/S_SURFACE/
   S_INK`), so the seven themes invert for free; `settime.png` matches `settime_render` geometry
   (52/34 widths, 10/22/8 gaps, r 6, y 100/34). Title bug: S1.
8. **Strip/header clock.** `strip_left_text()` gives the track when `player_active()`, else the clock
   when `time_in_title`, else "" — shared by `status_strip_render` and `top_banner_render`. Width: the
   clock is ≤ ~38 px in a 238 px clip that the SLEEP token shortens further (`clip_r = tok_x - 8`);
   padlock and battery are untouched; SHUF·ALB lives on Now Playing, which has no strip. Header:
   `menu_render_list(title, right, ...)` with `NULL` for Music; `ui_header` measures `right` first.
   Minute edge: one `wallclock_minute` compare per pass, gated to the Date & Time screen (always) and
   to `time_in_title && (!player_active() || SCR_MENU)`; `g_clock_min` is zeroed on wake and after a set;
   a `dirty` with the panel asleep is swallowed by the existing `bl_state != BL_OFF` gate. Buffer:
   `char clock[DATETIME_TIME_MAX]` on each caller's stack; the header's copy is formatted before
   `menu_render_list` paints the strip, and the comment explains the aliasing it replaces.
9. **Go.** `syncer/execute.go` stamps after `EnsureLog`, before the index, and never in a dry run
   (`Execute` returns at `o.DryRun` before it; `TestSyncStampsTheClock` covers both). `installer.go`
   stamps after `EnsureConfig`, after the dry-run return. `internal/eject.Eject` stamps first for
   every OS via the shared wrapper (`ejectVolume` per platform), skips non-directories with one line,
   never fails the eject; `app/backend.go:650` calls that wrapper, so the app gets it. `zoneOffsetMinutes`
   clamps to −720..840 and the report prints the clamped value (`TestStampClampsAnImpossibleZone`).
   `doctor` prints never-stamped / pending / applied (tested via `runCore`). vet and tests clean.
10. **Docs.** `06-power.md`: a "Real-time clock" section with the per-register confidence column,
    BCD/reset value/read+write order/alarm/RTCWAK conflict/bench a–e; the "State across sleep" bullet;
    the register table row. `09-i2c.md`: the two controller properties (4-byte cap → tear check; NACK
    invisible → per-reader gate). `07-usb.md`: the WRITE_BUFFER note. `USER_GUIDE.md`: Date & Time
    bullet, strip paragraph, the core-app clock paragraph, the troubleshooting line. `STATUS.md`: an
    UNFLASHED entry with a nine-step bench and the "Alarm" not-done item. README/core/README suite
    count 67 (correct). `settings-persistence.md`: the 64-byte table and the host-write argument.
    Stills: identical except the build-id-bearing ones (see the run table). Doc defects: S1 (still vs
    device), S2 (settings.h + guide wording), S4 (INT1 row), S5 (table split), N2/N3 (comments).

## Commit history

Nine commits, each buildable in the order it lands (HAL+datetime → pure modules → editor → record+UI
wiring → docs → parity test → single-read boot → usb note → audit fixes). Subjects are imperative and
in the log's voice; bodies explain why, including the width budget and the cannot-loop argument. The
last commit bundles five wiring fixes and one Go fix; that is defensible as "what an audit found" but
the five would each have been easier to bisect alone.

## Acceptance criteria

1 ✓ (67 suites, hw clean, go clean). 2 ✓ (12/24/44/48/64 decode; `--emit` still 48 and parity holds).
3 ✓ (Go tests pin size, other slot, verbatim tail, marker past the slots). 4 ✓. 5 ✓ (plus my Python
check). 6 ✓ (MENU writes nothing; one `rtc_write` sequence per commit; day ≤ mdays). 7 ✓ (gallery
diff empty except `settings.png` and build-id stills). 8 ✓ (three transactions at boot; the forced
save only on a new stamp; no per-frame I²C). 9 DEVICE — correctly deferred, bench listed.

SCORE: 7/10

Verdict: the hard parts are right and independently verifiable — the calendar maths agrees with
Python on every boundary of the century, the RTC driver's grammar and every "wrong answer" shape are
pinned on the mock bus, the once-per-stamp state machine does what the plan's table says in the
plan's order, the record grows without moving a golden, and the host's slot write is provably
non-truncating and byte-preserving on both the Go and Python sides. What keeps it from an 8 or higher
is a handful of real, cheap mismatches between what the code does and what the branch says it does:
the Date & Time screen is titled "Settings" while its committed still says otherwise, Reset Settings
shows UTC until the next boot while the header comment and the guide say the clock survives, the boot
FORCE commit's battery gate is described but never evaluated, one alarm bit in the tabled map is very
likely wrong and its confidence note does not say so, and a README table was split in two. None of
them is deep; all of them are the kind of stale line the rules say blocks a 10, and S2 is a
user-visible wrong clock for a session.
