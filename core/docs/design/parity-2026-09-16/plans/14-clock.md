# 14 — Clock, and Settings › Date & Time

Plan only. Repo `/home/brando/Projects/ipod_theme`, `main` at `dbce3b8`. Paths are under `core/`
unless they start with `docs/`, `tools/`, `design_reference/`, `README.md`, `STATUS.md` or
`CHANGELOG.md`. Line numbers are as of `dbce3b8`. Nothing here can be verified on the device in
this job; every device-only item is marked **DEVICE**.

## Summary

The iPod 5.5G's PMIC (NXP PCF50605, I²C device `0x08`) carries a real-time clock in its always-on
domain. Nothing in the firmware reads it. This plan adds, in five slices that each build and test
alone:

1. **`hal_rtc_get` / `hal_rtc_set`** — a PCF50605 RTC driver over the existing `i2c_send` /
   `i2c_read` path (`hal/hw/rtc.c`), BCD calendar registers `0x0A..0x10`, "unset" detection, a
   torn-read guard, and a golden-trace test on the mock bus. The register map is not in
   `docs/hw/06-power.md`; it is derived from the public PCF50606 datasheet, cross-checked against
   the six PMU registers the doc *does* list, and marked **to confirm** with a bench procedure.
2. **Host-set time** — `core sync`, `core eject`, `core install` and the desktop app cannot talk
   to the firmware (the iPod is in Apple's ROM disk mode on the cable; the iTunes `WRITE_BUFFER`
   RTC path in `docs/hw/07-usb.md:151` is Apple's USB stack, which we do not run), so the host
   stamps `CORECFG.DAT` instead: the current UTC epoch and the host's UTC offset go into new
   length-gated fields of the existing record, written into the *other* slot with `seq+1`, tail
   bytes preserved verbatim, no truncation. At boot the firmware compares that stamp with the
   stamp it last acted on and with the running RTC, sets the clock at most once per stamp, and
   records the decision in the same record before anything else can go wrong. Old records load;
   `CONFIG_VERSION` stays 2; `make_config.py --emit` and the Go golden stay byte-identical.
3. **Settings › Date & Time** — a new root row (`Playback, Sound, Theme, Display, Clicker,
   Date & Time, About, …`), a three-row sub-screen (Set Date & Time · Time Format · Time in Title)
   and a field editor (year / month / day / hour / minute [/ AM-PM]) in the list chrome's own
   language: the selected field is an ink plate with surface type, exactly the selection bar's
   inversion. Model in `ui/settime.c` (pure, tested); renderer in `ui/screen_settings.c`; `main.c`
   wiring is one `case`.
4. **Showing the time** — with Time in Title on: the status strip's left slot shows `10:42 AM`
   when nothing is playing (today blank, `main.c:1012`), and the main menu's header right slot
   (free today; `ui_header` measures it first) shows it always. While a track plays the strip keeps
   the track name — the 2026-09-14 decision that the strip is a now-playing readout stands, and
   the width budget says so (below). A minute-edge repaint costs one comparison per pass; the
   time itself comes from a software clock anchored to the RTC, so the I²C bus is touched at boot,
   after a wake, after a set and every 30 minutes — never per frame.
5. **Alarm foundation** — the alarm registers, the interrupt bit and the standby wake bit are
   tabled (with a conflict between the doc and the datasheet flagged), the HAL is shaped so
   `hal_rtc_alarm_set` is a later two-function addition, and no alarm UI is planned.

Integer-only civil↔epoch for 2000..2099 lives in `kernel/datetime.c` with a table test and an
exhaustive day loop.

## Current behaviour (file:line)

**PMU / I²C.**
- `hal/hw/i2c.c:244-289` `i2c_send(dev, bytes, len)`: ≤ 4 payload bytes, returns before the
  transaction completes (lazy completion); `:291-350` `i2c_read(dev, reg, buf, n)`: 1-byte
  pointer write, turn-around wait, n ≤ 4 result bytes; **cannot see a NACK** — an unanswered read
  hands back the DATA registers' last written bytes (`battery.c:698-727` explains the 1130 mV
  artefact this causes). `i2c_clock_suspend/resume` (`:129-172`) gate the block during suspend
  and every entry self-restores.
- `hal/hw/battery.c:618-641` PMU register constants live in the driver, not in `pp5022.h`
  (`PMU_ADDR 0x08`, `PMU_ADCC1 0x2F`, `PMU_ADCS1 0x30`); `battery_sample` (`:819-870`) is the
  template for a PMU transaction with a plausibility gate.
- `hal/hw/power.c:1182-1186` `PMU_OOCC1 0x08`, `GOSTDBY 0x01`, `CHGWAK 0x20`, `EXTONWAK 0x40`;
  `power_standby` (`:1245-1285`) clears IRAM and writes OOCC1. `docs/hw/06-power.md` "Standby /
  sleep" lists `RTCWAK 0x80` and states "RTC and PMIC config: preserved (always-on domain)".
- `docs/hw/06-power.md` "Other PCF50605 registers we touch": `OOCC1 0x08, DCDC1 0x1B, IOREGC
  0x23, MBCS1 0x2C, ADCC1 0x2F, ADCS1/2 0x30/0x31`. **No RTC register map anywhere in
  `docs/hw/`.** `docs/hw/09-i2c.md` documents the controller (4-byte payload cap, register-pointer
  reads).
- `hal/hal.h` has no time-of-day call; `clock_ms/clock_us` (`:100-115`) are monotonic since boot.
  `hal/sim/sim_hal.c` implements the contract for the host but is linked by no executable
  (`core/README.md` "make sim is the test build").
- `tests/hw_mmio/battery_trace_test.c:44-116` is the pattern for asserting a PMU transaction on the
  mock bus (`mmio_mock_set_read` for DATA0..3, `expect_send_open/close` helpers).
  `tests/meson.build:644-665` registers it; `hal/hw/meson.build` lists the driver sources.

**Settings record.**
- `kernel/config.c:126-209` layout: header 12 B, payload v1 `0..11` (one byte each), resume
  locator `12..23`, queue context `24..43`, `CFG_PAYLOAD_V2Q = 44`; `config_encode` (`:296-352`)
  writes length 44 and zero-fills the slot so the CRC covers the padding; `config_decode`
  (`:354-450`) gates each tail on the record's own `length`, clamps every field.
  `kernel/config.h:72-100` `CONFIG_VERSION 2` and the "append, never move" rule.
- `kernel/cfg_commit.c` gate: `CFG_COMMIT_IDLE` (debounced, never wakes a parked drive), `SOFT`,
  `FORCE` (wakes), `LAST`; `main.c:3154` `settings_commit(mode)`, `:3128-3133` `settings_touch()`.
- Host encoders: `cli/internal/devicefs/config.go:128-153` `EncodeConfigSlot` (length 44, tail
  zero), `:169-209` `DecodeConfigSlot` (v1 settings only; tail not modelled), `:216-236`
  `ConfigFileValid`, `:247-271` `EnsureConfig` (fresh file only when no valid slot), `:293-311`
  `writeFileThrough` → `OpenWriteThrough`, which **truncates** (`writethrough_other.go` `O_TRUNC`,
  `writethrough_windows.go` `CREATE_ALWAYS`). `tools/make_config.py:398-444` mirrors the layout
  (`PAYLOAD_LEN = 44`), `--emit` feeds `tests/kernel/config_test.c` through `tests/meson.build:
  514-530`, and `cli/internal/devicefs/config_test.go:80` asserts the Go slot is byte-identical to
  `--emit`.
- Call sites: `cli/internal/syncer/execute.go:255-273` (device files step, before the index),
  `syncer.go:441` (`p.Config = !configValid`), `cli/sync.go:201,241` (printing),
  `installer/installer.go:309-315`, `doctor/doctor.go:391-410` `checkConfig`, `cli/eject.go:25`
  → `internal/eject.Eject`, `app/backend.go:649` `Eject`, `app/actions.go:340` `startEject`.

**Boot order** (`main.c` `run_ui`, `:5518-5622`): `settings_defaults` → `config_load` (`:5530`)
→ UART `cfg load` line → `theme_set` → `boot_screen_render` → `library_ensure` (`:5565`) →
`battery_refresh(1)` → `evlog_mount` → `settings_apply` → `resume_restore` →
`cfg_commit_clear(&g_cfg_commit)` (`:5621`, "loading is not a change to save back"). The I²C bus
is up from `kernel_main` `:7251` (`i2c_init(); battery_init();`), before the disk. Suspend wake:
`:5404-5424` (`ata_wakeup`, `"core: suspend: wake"`, `lcd_wake`, paint, present, backlight). The
5 s battery cadence with its "repaint only if the glyph changed" rule: `:5760-5771`. PMU standby is
a cold boot (`hal/hw/power.h`), so wake-from-off re-runs the boot path.

**Settings screens.** `ui/settings.h:131-148` screen enum (`ROOT..DIAG`), `:164-182` action enum
(`NOOP` deliberately last); `ui/settings.c:437-440` `ROOT_L[9]`, `:495-508` counts, `:542-565`
kinds (root idx 7/8 = ACTION), `:646-704` `settings_activate`, `:706-746` `settings_adjust`.
Renderer `ui/screen_settings.c:168-224` `list_render` (24 px rows, `sel_bar`, right value in
`F_SUB`, chevron), `:289-323` `settings_render` titles. `main.c:6362-6510` the `SCR_SETTINGS`
input branch (wheel: slider edit or move; SELECT: `settings_activate` → `target` switch `:6413-
6421`; MENU: exit edit / back to root / pop + `settings_commit(CFG_COMMIT_SOFT)`).
`main.c:3237-3295` `settings_render_cur` dispatches About / Boot Details / generic and paints the
strip over the band.

**Status strip.** `main.c:1006-1046` `status_strip_render` (the brief's pointer to `ui/chrome.c`
is stale — chrome.c holds header/rows/scrollbar only): `left = player_active() ?
track_display(...) : ""` (`:1012`), drawn at `(12, STATUS_Y0+11)` in `FONT_SMALL`/`LINEN_MUTED2`
clipped to `[12, LCD_WIDTH-70)` = 238 px; battery at `bx = 284`; padlock at `bx-14`. The same
`left` rule is repeated in `top_banner_render` (`:4853`). `ui/chrome.c:200-223` `ui_header(title,
right, back)` measures `right` (`FONT_SMALL`, `LINEN_MUTED2`) first and ellipsises the title into
what is left. `main_menu_render` (`main.c:3612-3617`) → `menu_render_list("Core", …, back=0)`
(`:3592`), which calls `status_strip_render()` then `ui_header` with no right value. Gallery:
`docs/screens/render.py:670-680` `status_strip`, `:1574-1611` `screen_settings` with its own
`ROOT_L`, `:2008-2040` the still list.

**Design reference.** `design_reference/menus.jsx:21-39` `StatusStrip({ time = "10:42 AM", … })`
draws the time on the strip's LEFT — the design always intended it there. `system-screens.jsx:469`
puts a "tiny clock" top-right on the charging screen (not in this plan; noted as a follow-on).
There is no Date & Time editor in the jsx; the widget below is designed from the list tokens.

## Design

### 1. RTC HAL

**Contract** (`hal/hal.h`, new "Real-time clock" section):

```c
/* UTC seconds since 1970-01-01 (Unix epoch), uint32: valid for 2000..2099 here.
 * hal_rtc_get: 1 = *epoch written, the clock is running and holds a plausible
 * date; 0 = UNSET (never set, or reset by a drained cell: year register 00,
 * or fields that are not a date); -1 = the bus did not answer (*epoch untouched).
 * A caller must treat 0 and -1 alike: no time is known.
 * hal_rtc_set: writes the calendar registers, reads them back, returns 0 when
 * the read-back agrees within 1 s, -1 on a bus error, -2 on a read-back
 * mismatch (the write did not take; DEVICE item). Sub-second phase restarts
 * at the write. Refuses epochs outside 2001..2099 with -3.
 * Sim: the host's wall clock plus an offset hal_rtc_set changes;
 * CORE_SIM_RTC_UNSET=1 in the environment forces "unset". */
int hal_rtc_get(uint32_t *epoch);
int hal_rtc_set(uint32_t epoch);
```

`hal/hw/rtc.h` additionally exposes the raw layer the trace test and the bench line need:
`rtc_read_raw(uint8_t regs[7])` (BCD bytes SC..YR as read), `rtc_read(datetime_t*)`,
`rtc_write(const datetime_t*)`. Freestanding, `<stdint.h>` only, host-compiles under
`-DMMIO_MOCK` like `battery.c`.

**Register table** — PCF50605 at I²C `0x08`. Source: the public NXP PCF50606 datasheet's register
map (the 50605 is the same family; every register `docs/hw/06-power.md` lists — `OOCC1 0x08`,
`DCDC1 0x1B`, `IOREGC 0x23`, `MBCS1 0x2C`, `ADCC1 0x2F`, `ADCS1/2 0x30/0x31` — sits at the
datasheet's PCF50606 address, which is the cross-check that the RTC block is at the same place).
Cleanroom: derived from the datasheet and the doc, not from any GPL source; the doc must be
extended (Docs section) before the header can cite it.

| Addr | Name | Meaning | Encoding | Confidence |
|---|---|---|---|---|
| `0x0A` | `RTCSC` | seconds 00..59 | BCD, bits 6:4 tens, 3:0 units | high (map cross-check) |
| `0x0B` | `RTCMN` | minutes 00..59 | BCD | high |
| `0x0C` | `RTCHR` | hours 00..23 | BCD, 24-hour | high (24 h: medium — **confirm** no 12 h/AM-PM bit) |
| `0x0D` | `RTCWD` | weekday 0..6 | binary, base day unknown | medium — we WRITE Sunday=0 and IGNORE it on read |
| `0x0E` | `RTCDT` | day 01..31 | BCD | high |
| `0x0F` | `RTCMT` | month 01..12 | BCD | high |
| `0x10` | `RTCYR` | year 00..99 (2000..2099) | BCD | high; **reset value 00 → "unset": confirm** |
| `0x11..0x17` | `RTCSCA..RTCYRA` | alarm, same order | same | medium (alarm only; not used here) |
| `0x02` / `0x05` | `INT1` / `INT1M` | bit 7 `ALARM` (interrupt / mask) | | medium (alarm only) |
| `0x08` | `OOCC1` | `RTCWAK` wake-on-alarm | doc says `0x80`; the datasheet map I recall puts RTCWAK at bit 4 (`0x10`) with bit 7 reserved | **CONFLICT — to confirm on the bench before any alarm work** |

Not assumed: auto-increment on multi-byte *writes* (writes are one register per transaction, 2
bytes each, so no assumption is needed); a "stop clock while writing" bit (none in the datasheet;
handled by write order); a backup cell (the 5G has none that we know of — a drained main cell resets
the RTC, which is exactly the "unset" case).

**Read** (`rtc_read`): `i2c_read(0x08, 0x0A, a, 4)` → SC MN HR WD; `i2c_read(0x08, 0x0E, b, 3)`
→ DT MT YR; `i2c_read(0x08, 0x0A, c, 1)` → SC again. If `bcd(c) < bcd(a[0])` the seconds rolled
over between chunks (23:59:59 → 00:00:00 tears the date): re-read once. Decode BCD with a strict
digit check (any nibble > 9 → invalid). Valid iff `sec ≤ 59, min ≤ 59, hour ≤ 23, 1 ≤ month ≤
12, 1 ≤ day ≤ mdays(month, year), year ≠ 00`. Year 00 (2000) is the register file's reset value,
so the clock cannot represent "before 2000" and the brief's "year < 2000 → unset" becomes "year
2000 → unset" on this chip: the valid range is **2001..2099**. The NACK artefact (DATA registers
holding the pointer byte and stale bytes — `battery.c:698-727`) decodes as SC=10, DT=14 and a stale
YR; the trace test seeds exactly that shape and requires "unset" from the year gate, and the
software clock below adds a runtime check (an RTC that does not advance between two anchors is
declared unset and logged). Weekday is recomputed from the date; the register is never trusted.

**Write** (`rtc_write`): seven `i2c_send(0x08, {reg, bcd}, 2)` in the order **SC, MN, HR, WD, DT,
MT, YR** — seconds first, so the counter restarts and the next minute carry is ≥ 1 s away while
the remaining six writes take microseconds; then `i2c_wait` via a 1-byte read-back of YR (the
write path is lazily completed, `09-i2c.md`), then a full `rtc_read` and compare (±1 s).
The editor always writes `sec = 0`.

**Bench procedure (DEVICE)** — before trusting any of the above: (a) the boot line
`core: rtc raw SC MN HR WD DT MT YR valid N epoch XXXXXXXX` (7 hex bytes as read) on the first
flash: BCD digits ≤ 9 in every nibble confirms the encoding; the value Apple's firmware left
behind tells us whether the OF used the same registers; (b) set 2026-09-16 10:42 from the
editor, read back (the `rtc set` line), power off with PLAY (PMU standby), wait ≥ 1 h, boot, check
the clock advanced by the wall-clock hour — that is the always-on-domain claim; (c) same across a
suspend-to-RAM (PLL parked) — proves the wake resync; (d) leave it a week for drift; (e) drain or
disconnect the cell once and read the raw bytes back — that is the reset value.

### 2. Host-set time

**Why the record, not a file.** The applied-stamp must live in the firmware's record anyway (it
is the only thing the firmware can write); a second file would put the two halves of one state
machine in two places, cost a second root-directory walk at boot, and need its own pre-allocation
path in three host tools. The record already has an "append under the same version, gate on
length" rule (`config.h:72-93`) that was built for this. The Apple/iTunes `WRITE_BUFFER 0x3B`
RTC-sync path (`docs/hw/07-usb.md:151`) needs the device's own USB mass-storage stack answering the
SCSI command; in disk mode the ROM's stack answers and our firmware is not running. Say so in the
doc.

**Record fields** (payload offsets; `44..47` are left for plan 07-18's sound fields so the two
land without a rebase — if 07-18 never lands they stay reserved zeros; see Conflict surface):

```
off  size type  field            written by  meaning
48   4    u32   host_epoch       host        UTC Unix seconds when the host stamped; 0 = never
52   2    i16   host_off_min     host        host local UTC offset then, minutes, -720..+840
54   1    u8    time_flags       firmware    bit0 TIME_24H (default 0 = 12-hour), bit1 TIME_IN_TITLE (default 0)
55   1    u8    reserved         -           0
56   4    u32   applied_epoch    firmware    the host_epoch the firmware last ACTED on (set or judged stale); 0 = never
60   2    i16   utc_off_min      firmware    the device's display offset (copied from host_off_min on apply; 0 until then)
62   2    u16   reserved         -           0
64 = CFG_PAYLOAD_V2T   (length 44 -> 64; with 07-18: 48 -> 64)   CONFIG_VERSION stays 2
```

Decode gates the whole block on `len >= CFG_PAYLOAD_V2T`; a 44- or 48-byte record reads as
`host_epoch 0, applied 0, off 0, flags 0`. Clamps on decode: both offsets to `-720..+840`, flags
masked to `0x03`, epochs verbatim (validity is the state machine's job, and clamping an epoch would
manufacture a date). Encode writes everything from `settings_t` (the host stamp rides along
unchanged through every firmware save, which is what makes "applied == host" stable).

**RTC holds UTC; the device stores its offset.** Display = RTC + `utc_off_min`. The host writes
both; DST changes arrive as a new offset on the next sync without moving the RTC, so the
staleness rule below is judged on UTC and cannot be confused by a DST edge. Manual Set Date & Time
takes local civil time and writes `RTC := local − utc_off_min·60` with the offset unchanged (a
device that has never seen a host has offset 0 and runs "local as UTC" — internally consistent,
displays correctly).

**Host write mechanics** (`devicefs.StampConfigTime(volumeRoot string, now time.Time)`):
1. Read the file head (`ConfigMinBytes`); pick the newest valid slot `i` exactly as
   `ConfigFileValid` does (no valid slot → `EnsureConfig` creates the fresh file first, then this
   runs — a fresh install gets a clock on the first boot).
2. Copy the 1024 bytes of slot `i` **verbatim** — the resume locator, queue context, `44..47`,
   `time_flags`, `applied_epoch`, `utc_off_min` and anything a newer firmware appended are the
   device's and must survive. Set `length = max(length, 64)`; the bytes between the old length and
   64 are already zero (the firmware zero-fills the slot and CRCs the padding, `config.c:298-300`),
   so extending the length invents nothing.
3. Patch `48..53` (`host_epoch = now.UTC().Unix()`, `host_off_min = now's zone offset / 60`),
   `seq = seq_i + 1`, recompute the CRC over `[0, 1020)`.
4. Write the new slot at byte offset `(1−i)·1024` through a handle opened **without truncation**
   (new `OpenWriteThroughExisting`: `O_RDWR` on POSIX, `OPEN_EXISTING | FILE_FLAG_WRITE_THROUGH` on
   Windows), `WriteAt`, `Sync`, `Close`. The file's size, clusters and directory entry do not
   change — the same "only the first cluster's bytes move" argument the firmware's own writes rest
   on (`config.c:51-65`). The firmware loads the newest seq, so it picks the host slot and its next
   save alternates into slot `i`; `config_seq_newer` (`config.c:452-458`) copes with the wrap.
5. Idempotence: two stamps in a row (sync then eject) write two slots with two seqs; harmless.
   `--dry-run` never calls it. A device node instead of a mount point (`core eject /dev/sdb1`) is
   skipped with one printed line ("clock not stamped: give a mount point").

Where the host stamps: `syncer/execute.go` right after `EnsureLog` (the "device files" phase,
still before the index); `installer.go` after `EnsureConfig`; `cli/eject.go` and
`app/backend.go Eject` **first**, before the OS eject — eject is the last thing the user does
before unplugging, so this stamp is the freshest. The stamp's error on the device is the time
between the stamp and the boot that applies it: with eject stamping, that is the unplug-and-boot
delay (the ROM reboots on disconnect; boot is ~8 s) — under a minute in the normal flow.

**Firmware state machine** (`kernel/timesync.c`, pure, no hardware; `main.c` calls it once at
boot):

```c
typedef enum { TIMESYNC_NONE, TIMESYNC_SET, TIMESYNC_STALE } timesync_action_t;
#define TIMESYNC_STALE_S   600u        /* 10 min: the RTC already ran past the stamp */
#define EPOCH_2001         978307200u
#define EPOCH_2100         4102444800u
timesync_action_t timesync_decide(const settings_t *s, int writable,
                                  int rtc_valid, uint32_t rtc_epoch);
```

Rules, in order — the first that fires decides:

| # | condition | action | why |
|---|---|---|---|
| 1 | `!writable` (`config_writable()==0`) | NONE | nothing may be applied that cannot be recorded; without the record a re-apply on every boot is exactly the loop the design forbids |
| 2 | `host_epoch == 0` | NONE | never stamped |
| 3 | `host_epoch == applied_epoch` | NONE | already acted on (equality, not "newer": a host whose clock was wrong-in-the-future and later corrected still counts as a new stamp) |
| 4 | `host_epoch < EPOCH_2001 || host_epoch >= EPOCH_2100` | STALE | not a date this clock can hold; never look at it again |
| 5 | `rtc_valid && rtc_epoch >= host_epoch + TIMESYNC_STALE_S` | STALE | the clock is running and is already past the stamp by more than the stamp could plausibly be old: the iPod sat unbooted (dead cell, a day on the shelf); applying would set it back by that much. Cost: an RTC more than 10 min *fast* cannot be host-corrected (the user sees it and sets it) |
| 6 | otherwise | SET | RTC ← `host_epoch`; either the clock was unset, or behind, or ahead by < 10 min (a fast clock is pulled back at the cost of ≤ the stamp's age) |

On SET or STALE `main.c` writes `applied_epoch = host_epoch`, `utc_off_min = host_off_min` (the
offset is a fact the host knows better than we do, even when the epoch is stale), `time_flags`
untouched, then `settings_touch(); settings_commit(CFG_COMMIT_FORCE);` **immediately**, while the
drive is spinning from the mount and before `cfg_commit_clear` at `main.c:5621` — the record on the
platter says "acted on" before the library even loads. On SET, `hal_rtc_set` failing (`-1/-2`)
leaves `applied_epoch` alone and logs; the next boot retries the same stamp, which rule 5 then
bounds to within 10 min of it.

Why it cannot loop: a stamp is acted on at most once per `host_epoch` value while the mark
persists; the mark is written in the same boot, forced, before anything else; within the session
the in-RAM value blocks a second decision; a mark that failed to reach the disk (a refused save)
can only cause a re-apply on a *later* boot if that boot is still within 10 min of the stamp
(rule 5), so the worst case is one backwards step of < 10 min, once, and then the mark lands.
Why it cannot go backwards every boot: after the first boot either the mark is on disk (rules 3)
or the RTC has run past the window (rule 5); either way the stamp is inert from then on. The
UART/evlog line `core: timesync host XXXXXXXX off ±NNN applied XXXXXXXX rtc XXXXXXXX -> set|stale|none rc N`
records every decision.

Manual set never touches `applied_epoch`; a later host stamp goes through the same rules.

### 3. Software clock (no I²C per frame)

`kernel/rtc_cache.c` (pure): `rtc_cache_anchor(valid, epoch, now_us)`, `rtc_cache_now(now_us,
&epoch) → 0/1`, `rtc_cache_tick(now_us)` folds elapsed into the anchor (call from the 5 s cadence:
`USEC_TIMER` wraps at 71 min, so the anchor must be advanced more often than that),
`rtc_cache_minute(now_us)` = local-minute counter for the repaint edge. Hardware re-anchors: at
boot (after the timesync decision), after a manual set, on suspend wake (`main.c:5404` block — the
PLL park and the 10 Hz tick mean `USEC_TIMER` cannot be trusted across a suspend), and every
`RTC_RESYNC_S = 1800`. On every re-anchor compare the RTC's advance with the timer's: a
disagreement over 5 s is logged (`core: rtc drift`), an RTC that did not move at all is declared
unset (the NACK/absent-PMU shape) — both DEVICE items to read off the log.

### 4. Settings screens

Model (`ui/settings.h/.c`):
- `settings_t` gains `int time_24h; int time_in_title; int utc_off_min; uint32_t host_epoch;
  int host_off_min; uint32_t applied_epoch;` — the last three are runtime/host state like the
  resume locator ("no row shows these"). Defaults: `0, 0, 0, 0, 0, 0`. Reset Settings routes
  through `settings_defaults` and therefore forgets the host stamp and the mark — the next boot
  re-applies the host stamp (rule 5 still guards), which is the right reading of "reset".
- `ROOT_L[10] = { "Playback", "Sound", "Theme", "Display", "Clicker", "Date & Time", "About",
  "Boot Details", "Disk Mode", "Reset Settings" }`; `settings_kind(ROOT)` ACTION at 8/9;
  `settings_activate(ROOT)` 5 → `SETTINGS_ENTER_DATETIME`, 6 → ABOUT, 7 → DIAG, 8 → DISKMODE,
  9 → RESET. Ten rows scroll like nine did (`screen_settings.c:174-178`, `render.py:1574`).
- New screens `SETTINGS_DATETIME` (rows: `"Set Date & Time"` SUBMENU with a right value,
  `"Time Format"` SELECT `"12-hour"/"24-hour"`, `"Time in Title"` SELECT `"On"/"Off"`) and
  `SETTINGS_SETTIME` (count 1, kind `SETTINGS_KIND_INFO`-like custom; rendered by
  `settings_settime_render`). Actions `SETTINGS_ENTER_DATETIME`, `SETTINGS_ENTER_SETTIME` appended
  **after** `SETTINGS_ACTION_NOOP` so no existing value moves (main.c switches on names; the
  "NOOP last" comment at `settings.h:178-181` gets one line saying the two after it are later
  additions).
- The current time for the "Set Date & Time" row's right value (`"10:42 AM"`, `"22:42"`, or
  `"Not set"`) is **injected**, the way `ui_set_scroll_text` injects the marquee clock:
  `settings_set_now(int valid, uint32_t local_epoch)` stores it in settings.c; `settings_value`
  formats through `datetime_fmt_time`. Pure and testable; main.c calls it once per settings paint.

Editor model (`ui/settime.h/.c`, pure):

```c
typedef struct { int year, month, day, hour, min; int field; int use_12h; } settime_t;
enum { ST_YEAR, ST_MONTH, ST_DAY, ST_HOUR, ST_MIN, ST_AMPM, ST_FIELDS };
void settime_begin(settime_t *t, int valid, const datetime_t *now, int use_12h);
int  settime_adjust(settime_t *t, int delta);   /* wheel: year clamps 2001..2099, the rest wrap;
                                                   day re-clamped to mdays after month/year moves */
int  settime_next(settime_t *t);                /* SELECT: 1 when the last field was confirmed */
int  settime_field_count(const settime_t *t);   /* 5, or 6 with AM/PM */
void settime_civil(const settime_t *t, datetime_t *out);  /* sec = 0 */
```

Seed: the current local time if valid, else the host stamp if any, else `2026-01-01 12:00`.
Wheel delta is clamped to ±4 per event like the sliders (`main.c:6370-6372`). MENU cancels
(nothing written) and returns to Date & Time; SELECT on the last field commits:
`hal_rtc_set(local − utc_off·60)`, re-anchor, log, back to Date & Time with the row's value now
showing what was set.

**The widget** (`settings_settime_render(const settime_t *t)`, 320×240, list chrome tokens):

```
 y
 0   [ status strip: (clock or track) ................................. ▭▭ ]
30   « Set Date & Time                                            ← ui_header, back chevron
38   ────────────────────────────────────────────────────────────
                                                                       
72                       Wednesday 16 September 2026                  ← FONT_SUB, LINEN_MUTED2, live
                                                                       
        ┌────────┐  ┌──────┐  ┌──────┐      ┌──────┐   ┌──────┐  ┌──────┐
100     │  2026  │  │ Sep  │  │  16  │      │  10  │ : │  42  │  │  AM  │   ← FONT_TITLE (bold 18)
134     └────────┘  └──────┘  └──────┘      └──────┘   └──────┘  └──────┘     plates 34 px tall, r=6
150       YEAR        MONTH      DAY          HOUR       MINUTE             ← FONT_SMALL caps, LINEN_MUTED
                                                                       
224            Wheel changes · Select next · Menu cancels             ← FONT_SMALL, LINEN_MUTED2

  selected plate: filled LINEN_SEL_BG (= ink), text LINEN_SEL_FG (= surface)  — the selection bar's inversion
  other plates:   LINEN_SURFACE with a 1 px LINEN_BORDER outline (two ui_round_rect calls), text LINEN_INK
  widths: year 52, others 34, gaps 10, date/time gap 22, colon 8 → 282 px (12-hour), 238 px (24-hour), centred
  24-hour: no AM/PM plate, hour reads 00..23
```

Themes invert it for free (every colour is a `g_pal` token). The gallery gets `datetime.png`
(the list) and `settime.png` (the editor with MONTH selected).

### 5. Showing the time

- `status_strip_render` (`main.c:1006`) and the strip row in `top_banner_render` (`:4853`) share
  one new static `strip_left_text()`: the track name when `player_active()`, else the clock
  string when `g_settings.time_in_title && rtc_cache_now(...)`, else `""`. Same x, baseline, face
  (`FONT_SMALL`; the design's 9 px caps and our existing strip face agree) and colour
  (`LINEN_MUTED2`). The gallery's `status_strip(sc, left=...)` already takes the string.
- `main_menu_render` (`main.c:3612`): pass the clock as the header's right value
  (`menu_render_list` gains a `right` parameter, `NULL` for Music). `ui_header` measures it first
  and ellipsises the title, and "Core" is 30 px, so nothing can collide.
- **Width budget and the decision for the playing case.** The strip's left clip is 238 px
  (`12..250`), `"12:59 PM"` at regular-9 is ≈ 38 px, `"23:59"` ≈ 26 px. Showing the clock *and* the
  track name would cut the name's clip to ≈ 190 px (or ≈ 160 px with plan 05's `SLEEP 120` token
  in the right cluster) — a third of every title gone while the whole point of the strip since
  2026-09-14 is that title. So: strip = clock only when idle; the main menu header = clock always
  (with Time in Title on); Now Playing's own top row unchanged. Written into the guide in one
  sentence.
- Minute edge: one comparison per pass on `rtc_cache_minute()`; when it changes and Time in
  Title is on and the current screen has a strip or is the main menu → `dirty = 1` (the strip's
  band-only present path already exists for the gauge, `main.c:5760-5771`).
- `settings_render_cur` (`:3237`) calls `settings_set_now(...)` before `settings_render` so the
  Date & Time row shows the live time; the same minute edge repaints it.

### 6. Civil date ↔ epoch, integer C, 2000..2099 (`kernel/datetime.h/.c`)

```c
typedef struct { int year, month, day, hour, min, sec, wday; } datetime_t;   /* wday 0 = Sunday */
#define EPOCH_2000  946684800u                     /* 2000-01-01T00:00:00Z, a Saturday */
#define EPOCH_2100  4102444800u                    /* fits uint32 */
static const uint16_t cum_days[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
static int leap(int y) { return (y & 3) == 0; }   /* exact within 2000..2099 (2100 is excluded) */
int datetime_mdays(int year, int month)
{   static const uint8_t md[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    return md[month - 1] + (month == 2 && leap(year)); }

uint32_t datetime_to_epoch(const datetime_t *d)          /* caller passes a valid 2000..2099 date */
{
    uint32_t y    = (uint32_t)(d->year - 2000);                     /* 0..99 */
    uint32_t days = y * 365u + (y + 3u) / 4u                          /* leap days in years before y */
                  + cum_days[d->month - 1] + (uint32_t)((d->month > 2) && leap(d->year))
                  + (uint32_t)(d->day - 1);
    return EPOCH_2000 + days * 86400u + (uint32_t)d->hour * 3600u
         + (uint32_t)d->min * 60u + (uint32_t)d->sec;
}

int datetime_from_epoch(uint32_t t, datetime_t *d)       /* 0 if outside 2000..2099 */
{
    if (t < EPOCH_2000 || t >= EPOCH_2100) return 0;
    uint32_t s = t - EPOCH_2000, days = s / 86400u, rem = s % 86400u;
    d->hour = (int)(rem / 3600u); d->min = (int)(rem / 60u % 60u); d->sec = (int)(rem % 60u);
    d->wday = (int)((days + 6u) % 7u);                                /* 2000-01-01 = Saturday */
    uint32_t c = days / 1461u, r = days % 1461u;                      /* 4-year cycle, leap year first */
    uint32_t y = c * 4u;
    if (r >= 366u) { r -= 366u; y += 1u + r / 365u; r %= 365u; }
    d->year = 2000 + (int)y;
    int m = 1;
    while (m < 12 && r >= (uint32_t)datetime_mdays(d->year, m)) { r -= (uint32_t)datetime_mdays(d->year, m); m++; }
    d->month = m; d->day = (int)r + 1;
    return 1;
}
/* (y+3)/4 check: y=1 → 1 (2000 counted), y=4 → 1 (2004's own leap day is the month term), y=5 → 2. */
```

Formatting (`datetime_fmt_time(char *buf, const datetime_t*, int use_24h)`): `"10:42 AM"` /
`"12:05 PM"` / `"12:00 AM"` (hour 0 → 12) or `"00:05"`; `datetime_fmt_date(buf, d)` →
`"Wednesday 16 September 2026"` (full names, the editor's summary) and the 3-letter month for the
plate. Local time = `datetime_from_epoch(utc + off_min*60)` with the addition done in `int64`-free
`uint32` arithmetic (offsets are ±14 h, the epoch is ≥ 2001, no wrap possible). BCD helpers
`bcd_to_int(uint8_t) → -1 on a bad nibble` and `int_to_bcd` live in `rtc.c`.

## Files to change

**Firmware C**
- `hal/hal.h` — the Real-time clock section (`hal_rtc_get/set`, the "unset" contract, the alarm
  note pointing at `hal/hw/rtc.h`).
- `hal/hw/rtc.h`, `hal/hw/rtc.c` (new) — PMU constants (`PMU_RTCSC 0x0A` … `PMU_RTCYR 0x10`,
  alarm `0x11..0x17`, `PMU_INT1 0x02`, `PMU_INT1M 0x05`, `INT1_ALARM 0x80` — as comments/defines
  with the confidence notes), BCD, `rtc_read_raw/rtc_read/rtc_write`, `hal_rtc_get/set`. No
  `*_ADDR` symbols (they are I²C register indices, and `check_hw_consistency.py` scans `pp5022.h`
  for `_ADDR` against the docs — `battery.c`'s convention of driver-local `PMU_*` defines is the
  one to follow). `hal/hw/meson.build` — add `rtc.c`.
- `hal/sim/sim_hal.c` — `hal_rtc_get/set` over `time(NULL)` + a static offset; `CORE_SIM_RTC_UNSET`.
- `kernel/datetime.h/.c` (new), `kernel/rtc_cache.h/.c` (new), `kernel/timesync.h/.c` (new) —
  all pure; `kernel/meson.build` (hw) lists them.
- `kernel/config.h` — versioning comment (44/48 → 64 under v2); `kernel/config.c` — offsets
  `P_HOST_EPOCH 48 … CFG_PAYLOAD_V2T 64`, encode/decode/clamps, the layout comment.
- `ui/settings.h/.c` — fields, defaults, `ROOT_L` (10), `SETTINGS_DATETIME`/`SETTINGS_SETTIME`,
  `SETTINGS_ENTER_DATETIME`/`SETTINGS_ENTER_SETTIME`, `settings_set_now`, `DT_L[3]`, value/kind/
  activate/adjust (wheel on Time Format / Time in Title steps like Backlight).
- `ui/settime.h/.c` (new) — the editor model.
- `ui/screen_settings.c` — `"Date & Time"` title in `settings_render`; `settings_settime_render`.
- `kernel/main.c` (thin, ~90 lines): boot block after `config_load` (`:5530`): `hal_rtc_get` →
  `rtc_cache_anchor` → `timesync_decide` → `hal_rtc_set`/mark → `settings_touch` +
  `settings_commit(CFG_COMMIT_FORCE)` → the `rtc raw` / `timesync` UART lines; the `SETTINGS_SETTIME`
  arm in the `SCR_SETTINGS` branch (`:6362-6510`); `SETTINGS_ENTER_DATETIME/SETTIME` in the target
  switch (`:6413-6421`); `settings_render_cur` (`:3237`) → `settings_set_now` + the SETTIME painter;
  `strip_left_text()` at `:1012` and `:4853`; `menu_render_list` `right` param (`:3592`,
  `:3616`); the minute edge and `rtc_cache_tick` next to the battery cadence (`:5760`); the wake
  re-anchor at `:5404`; the 30-min re-anchor.
- `tests/meson.build` — new suites `datetime`, `timesync` (also covers `rtc_cache`), `settime`,
  `hw-rtc`; `config_test`/`settings_test` sources unchanged; suite count 58 → 62 (README and
  `core/README.md` print it — see Conflict surface).

**Go** (`cli/internal/…`)
- `devicefs/config.go` — `cfgPayloadV2T = 64`, the offsets, `type TimeStamp`, `DecodeConfigTime(b)
  (TimeStamp, ok)` (gated on length ≥ 64), `stampSlot(slot []byte, now) []byte`,
  `StampConfigTime(volumeRoot, now) (Stamped, error)`; `EncodeConfigSlot` **unchanged** (length 44,
  so `TestEncodeConfigSlotMatchesMakeConfig` and the C fixture parity hold); `EnsureConfig` unchanged,
  the callers stamp after it.
- `devicefs/writethrough_other.go`, `writethrough_windows.go` — `OpenWriteThroughExisting` (no
  truncate) and `writeAtThrough(path, off, b)`.
- `syncer/execute.go` (`:255-273`) — stamp after `EnsureLog`; `Report.ClockStamped time.Time`;
  `syncer.go` `Plan.Clock` (always planned when not a dry run); `cli/sync.go` — plan line
  `CORECFG.DAT: clock will be stamped`, report line `clock: stamped 2026-09-16 08:42 UTC (+02:00)`.
- `installer/installer.go` (`:309-315`) — stamp after `EnsureConfig`.
- `cli/eject.go`, `app/backend.go Eject` — stamp first when the target is a directory; print the
  one-line skip otherwise.
- `doctor/doctor.go checkConfig` (`:391-410`) — a `clock` line: `host stamp <t> (<off>), device
  applied: yes | pending | never stamped`, WARN when a stamp is pending (the device has not booted
  since).
- `cli/README.md` — sync/eject/install/doctor rows.

**Python**
- `tools/make_config.py` — `TIME_FIELDS` at `48..62` for `decode`/`--verify` (prints the stamp and
  the mark), a `--stamp MOUNTPOINT [--epoch N --utc-offset MIN]` action that is the reference
  implementation of `StampConfigTime` (same verbatim-copy, other-slot, no-truncate rule via
  `open(path, "r+b")` + `seek`), `PAYLOAD_LEN` and `--emit`/`--create` **unchanged** at 44.
- `docs/screens/render.py` — `ROOT_L` (10 rows), `screen_datetime()`, `screen_settime()`,
  `status_strip(left="10:42 AM")` used by a `mainmenu_clock.png` variant; still list `:2008-2040`.

## Tests to add

**Host C (`make sim && meson test -C build-sim`)**
- `tests/kernel/datetime_test.c` (new suite `datetime`) — **table test**, hand-derived vectors:
  `2000-01-01 00:00:00 → 946684800 (Sat)`, `2000-02-29 12:00:00`, `2000-03-01`, `2001-01-01 →
  978307200 (Mon)`, `2004-02-29`, `2004-03-01`, `2026-09-16 10:42:00 → 1789555320 (Wed)`,
  `2038-01-19 03:14:07 → 2147483647 (Tue)`, `2038-01-19 03:14:08`, `2099-12-31 23:59:59 →
  4102444799 (Thu)`, every month's last day in 2001 and 2004; the inverse of each; rejection of
  `946684799` and `4102444800`; `datetime_mdays` for Feb 2000/2001/2004/2096; **exhaustive
  loop**: for every day 2000-01-01..2099-12-31 at 00:00:00 and 23:59:59, `to_epoch` against an
  independent day counter (increment a running epoch by `mdays` per month) and `from_epoch` back —
  36 525 days; weekday continuity (each day's wday = previous + 1 mod 7); `fmt_time` 12 h/24 h
  edge hours (0, 11, 12, 13, 23); local conversion with offsets `-720`, `+840`, `+330`.
- `tests/kernel/timesync_test.c` (new suite `timesync`) — the decision table above, one check per
  row, plus sequences: (a) stamp → SET, then the same stamp at the next boot → NONE ("cannot
  loop"); (b) three boots with the stamp unchanged and a running RTC → the RTC epoch is never
  overwritten ("cannot go backwards on every boot"); (c) stamp applied, mark lost (applied stays
  0), RTC now 11 min past → STALE, not SET; (d) `!writable` → NONE and the record untouched;
  (e) DST: same epoch, new offset → NONE for the epoch (equality) but the caller copies the offset
  — assert the helper `timesync_apply(&s, action)` does exactly that; (f) a stamp from year 1999
  and one from 2100 → STALE. `rtc_cache`: anchor/tick/now across a `USEC_TIMER` wrap, the minute
  counter across a minute edge, "did not advance" → invalid.
- `tests/kernel/config_test.c` — extend: the six fields land at the documented offsets, length 64;
  round-trip; a 44-byte and a 48-byte record decode with all six zero; a 63-byte length keeps
  them zero; offset clamps (`-721 → -720`, `841 → 840`); flags `0xFF → 0x03`; the host stamp
  survives a firmware encode/decode cycle unchanged; the `make_config.py --emit` fixture (length
  44) still decodes to "no stamp". A new check that patching a slot the way `--stamp` does (copy,
  patch, seq+1, CRC) yields a record `config_decode` accepts with the resume tail intact — the
  fixture for it is `tools/make_config.py --stamp` on a `--emit` file (custom_target next to
  `corecfg_golden.bin`).
- `tests/ui/settings_test.c` — root count 10, `Date & Time` at 5, About/Boot Details/Disk Mode/
  Reset at 6/7/8/9 with the right kinds and actions; Date & Time rows: `Set Date & Time` →
  `SETTINGS_ENTER_SETTIME`, Time Format toggles `time_24h` and returns NONE, Time in Title likewise,
  `settings_adjust` steps them; `settings_set_now` → `"10:42 AM"`, `"22:42"`, `"Not set"`;
  defaults; Reset clears the stamp fields.
- `tests/ui/settime_test.c` (new suite `settime`) — field order 12 h (6 fields) and 24 h (5);
  wheel wrap on month/day/hour/min, clamp on year; `31 Jan → +1 month → 28 Feb` (2026) and
  `29 Feb` (2028); 12-hour display of hours 0/12/13; AM/PM flip changes the hour by 12; SELECT ×
  fields → done exactly once; `settime_civil` has `sec 0`; seed order (now → host stamp →
  2026-01-01 12:00).
- `tests/hw_mmio/rtc_trace_test.c` (new suite `hw-rtc`, built like `battery_trace_test` with
  `rtc.c` + `i2c.c`) — `rtc_read`: the exact pointer-read grammar (`0x0A`×4, `0x0E`×3, `0x0A`×1),
  seeded BCD `{0x00,0x42,0x10,0x03} {0x16,0x09,0x26}` → 2026-09-16 10:42:00 valid; a torn read
  (`mmio_mock_queue_read` on DATA0: 59 then 00 → the retry is emitted and the second result wins);
  invalid BCD (`0x4A` seconds) → unset; YR `0x00` → unset; the NACK shape `{0x0A, 0x30, 0x05,
  0x00} {0x0E, 0x05, 0x00}` → unset; `rtc_write(2026-09-16 10:42:00)`: seven 2-byte sends in the
  order SC..YR with `0x00,0x42,0x10,0x03,0x16,0x09,0x26`, then the read-back; `hal_rtc_set` with
  a read-back that disagrees → `-2`; epoch 2000 → `-3`; a wedged bus (STATUS busy forever) → `-1`
  with no DATA latch.

**Go (`cd core/cli && go test ./...`)**
- `devicefs/config_test.go` — `StampConfigTime`: writes the *other* slot, `seq+1`, the source
  slot byte-identical afterwards; a record with a non-zero resume locator, queue context, `44..47`
  and firmware-owned `54..63` keeps every one of those bytes; length 44 → 64 with `54..63` zero;
  length 64 → 64 with `applied/utc_off` preserved; the file's size and bytes outside the two slots
  unchanged (a marker at byte 4096 survives — the no-truncate proof); `DecodeConfigTime` on a
  44-byte record → `ok=false`; parity: a Python `make_config.py --stamp --epoch --utc-offset` on
  the same input is byte-identical (skipped when `python3` is absent, like the existing `--emit`
  test); zone offsets `+05:30` → 330 and `-08:00` → -480.
- `devicefs/ensure_test.go` — a fresh `EnsureConfig` followed by a stamp leaves slot 0 at seq 1
  and slot 1 at seq 2 with the stamp.
- `syncer/syncer_test.go` — execute stamps (report field set, both slots valid, newest carries
  `now`); `--dry-run` leaves the file untouched.
- `cli/sync_test.go`, `cli/doctor_test.go` — the printed lines; doctor's `pending` vs `applied`.
- `cli/eject` / `app` — the directory-vs-device predicate; the app's Eject calls the stamp on the
  fake backend.

## Docs to update

- `docs/hw/06-power.md` — new section "Real-time clock": the register table with confidences,
  BCD, the reset value, the read/write order and why, the alarm registers, the `RTCWAK` conflict,
  the bench procedure; the "State across sleep" bullet gains "and the RTC keeps counting — the
  firmware reads it at every boot". `docs/hw/07-usb.md:151` — one line: this path is Apple's
  stack; our clock is set through `CORECFG.DAT` (see 06-power.md).
- `docs/design/settings-persistence.md` — the record table (v2 length 64, the time block, the
  host-writes-a-slot rule and why it is safe: no truncation, other slot, verbatim tail).
- `docs/USER_GUIDE.md` — Settings: a `Date & Time` bullet (the three rows, the editor's controls,
  12/24-hour, Time in Title: strip when idle, main menu header always); "The status strip"
  paragraph; "The core app": `sync`, `eject` and `install` set the clock — the iPod is in Apple's
  disk mode on the cable, so the clock is written into `CORECFG.DAT` and the device takes it at
  the next boot; eject last so it is freshest; a clock that reads more than ten minutes ahead is
  left alone (set it by hand). "If something is wrong": the clock is `Not set` after a drained
  battery — sync or set it.
- `README.md` — Settings line (add Date & Time); suite count. `core/README.md` — "Settings and
  resume" (record length, the host stamp) and the suite count. `core/cli/README.md`,
  `tools/README.md` (`make_config.py --stamp`, `--verify` shows the stamp).
- `STATUS.md` — an **UNFLASHED** entry with the bench list (a–e above, plus: the strip clock, the
  editor, a sync → boot → clock check, `doctor`'s pending/applied line flipping); "What's NOT
  done" gains "Alarm: HAL and registers tabled in 06-power.md, `RTCWAK` bit unconfirmed, no UI".
- `CHANGELOG.md` — Unreleased: clock, Date & Time, host-set time.
- `design_reference/README.md` — a line under the screen inventory: the Date & Time editor has no
  jsx; its design is `plans/14-clock.md`'s widget (or a jsx added alongside).
- `docs/screens/` — `datetime.png`, `settime.png`, `mainmenu_clock.png`; `settings.png` regenerates
  with ten rows.

## Acceptance criteria

1. `make sim && meson test -C build-sim` green with the four new suites; `make hw && make
   verify-hw` clean under `-Werror` (no `_ADDR` symbols added, so `check_hw_consistency.py` is
   unaffected); `cd core/cli && go test ./...` green.
2. `config_decode` accepts every record the tree could have written (length 12, 24, 44, 48, 64)
   and `make_config.py --emit` still decodes on the device path; `EncodeConfigSlot` is still
   byte-identical to `--emit`.
3. `StampConfigTime` never changes a file's size or any byte outside the target slot, never
   truncates, and preserves every byte of the copied slot except `48..53`, `seq`, `length` and the
   CRC (test 1 in the Go list).
4. `timesync_decide` satisfies the table, and the two sequence tests prove "at most once per
   stamp" and "never on a later boot once the RTC has run 10 min past it".
5. Date math: the exhaustive 2000..2099 loop and the hand table pass; weekday of 2026-09-16 is
   Wednesday.
6. The editor: MENU writes nothing; SELECT on the last field writes exactly one `rtc_write`
   sequence (trace test on the driver, model test on the field walk); the day never exceeds the
   month.
7. With Time in Title off nothing on screen changes from today (gallery diff of every existing
   still is empty except `settings.png`'s tenth row).
8. Boot cost: one 8-byte RTC read (three I²C transactions, ~1 ms) and, only on a new stamp, one
   forced config save while the drive is already spinning; no I²C traffic per frame.
9. DEVICE (cannot be met in this job, listed for the bench): raw bytes are BCD; a set survives a
   PMU standby and a suspend; a `core eject` stamp shows on the device within a minute of boot and
   `doctor` flips from `pending` to `applied`.

## Risks

- **DEVICE — the register map is unconfirmed.** Every address is datasheet-derived; the doc has
  none of them. Mitigations: the six cross-checked addresses; the strict validity gate means a wrong
  map reads as "unset", never as a wrong time; the first flash logs the raw bytes before anything
  trusts them; a wrong *write* map could clobber a neighbouring PMU register — `0x09` (`OOCC2`)
  sits just below the block and `0x11` (alarm seconds) just above, both harmless; the write is only
  reachable from the editor and the boot apply, and the boot apply is gated on the record.
- **DEVICE — Apple's leftover.** The OF may have used a different year base or a binary
  counter; the validity gate and the boot line handle it, but a *plausible-looking* wrong date would
  pass. The user sees it in Date & Time and syncs or sets it.
- **DEVICE — `hal_rtc_set` read-back.** If the PCF needs a write-enable we do not know about, set
  returns -2, the editor shows the old time, the log says so; nothing else is affected.
- **Stale stamp semantics.** A clock more than 10 min fast cannot be host-corrected (rule 5). By
  design; documented in the guide. If the bench shows real users hit it, the window is one constant.
- **Host writes into the firmware's file.** The race is structural: the firmware is not running in
  disk mode. The residual risk is a host tool bug writing the wrong slot or truncating; the Go
  tests pin both, and `make_config.py --verify` shows both slots' seq so a bad stamp is visible.
- **Reset Settings forgets the mark.** The next boot re-applies the host stamp; rule 5 caps the
  effect. Acceptable, and simpler than carving the mark out of `settings_defaults`.
- **USEC_TIMER across suspend.** The software clock is re-anchored on wake; if the bench shows the
  10 Hz tick path skipping the wake block, the 30-min re-anchor bounds the error.
- **Strip repaint.** The minute edge adds a present per minute on idle list screens with Time in
  Title on — the same band-only present the gauge already uses.

## Conflict surface

- **Record payload.** Plan 07-18 appends `44..47` (`CFG_PAYLOAD_V2S = 48`). This plan starts at
  **48** so both land without a rebase; whoever lands second updates the length constant chain
  (`CFG_PAYLOAD_V2T` is defined as `48 + 16`, not `CFG_PAYLOAD_V2S + 16`, precisely so the Go
  golden/Python offsets stay literal). Plan 05 reserves nothing on disk (RAM-only). The three
  codecs (`config.c`, `devicefs/config.go`, `make_config.py`) and their tests are touched by both
  plans: sequence them.
- **`ui/settings.c` `ROOT_L` and the root indices.** Inserting `Date & Time` at 5 renumbers About/
  Boot Details/Disk Mode/Reset (`settings_kind` 7/8 → 8/9, `settings_activate`, `render.py
  ROOT_L`, `settings_test.c`). Plan 05 adds a Playback row (no conflict); any other plan touching
  the root list rebases one index.
- **`main.c` hot spots**: `status_strip_render` (`:1006`) and `top_banner_render` (`:4853`) — plan 05
  adds a right-cluster token there; both edits are additive (mine changes the `left` expression,
  05 changes the clip) — merge by hand. The `SCR_SETTINGS` branch (`:6362-6510`) — plans 05/07-18
  add dispatch cases there too. The boot block after `config_load` (`:5530`) is new territory.
- **`settings.h` enums**: new screens appended before `SETTINGS_SCREEN_COUNT`, new actions after
  `NOOP` — any plan appending to either enum conflicts textually, not semantically.
- **Suite count** (README, `core/README.md`): every sibling plan bumps 58; the last to land writes
  the real number.
- **`docs/USER_GUIDE.md` Settings list, `STATUS.md` top, `CHANGELOG.md` Unreleased** — shared
  with every plan; append-only edits.
- **Go `syncer/execute.go` device-files step and `cli/sync.go` printing** — only this plan touches
  them; `installer.go` and `eject` likewise.
