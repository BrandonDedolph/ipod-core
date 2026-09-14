# Status — picking up where we left off

The README is the canonical public story; this doc is the running list of
what works, what doesn't, and what to pick up next.

## 2026-09-13 — issue sweep landed on `main`, NOT yet flashed

Six read-only audits over the whole tree, then six fix branches merged;
a second wave the same day added Playlists (read path + resume kind),
Now Playing partial repaints, `hal_audio_frames_played()`, the audit
leftovers, and two rounds of cross-branch review fixes (52 commits in all,
54 host suites green, ARM `-Werror` + `verify-hw` clean, nothing pushed). The
two user-reported problems turned out to be one chain: a PLAY-hold "off"
was suspend-to-RAM, which never escalated to PMU standby and drained the
cell; the resulting cold boot then rebuilt the queue as the album.

What changed (see `git log 054c722..`):
- **Power-down** — PLAY is arbitrated by press length (`ui/keyhold.c`: tap
  = pause on release, hold = sleep; the down-edge no longer pauses).
  Suspend now: FLUSH + STANDBY + **SLEEP** on the drive (reset-to-wake),
  `lcd_sleep()` behind `SUSPEND_PANEL_SLEEP` (default 1), 100 ms loop
  period, battery sampled on the 5 s cadence with the DISKSAFE/SHUTOFF
  policy live, and **escalation to PMU standby after
  `SUSPEND_TO_STANDBY_US` (30 min) off external power**. `enter_standby()`
  quiesces codec/drive/panel first and RETURNS -1 on a refused PMU command
  (repaint + carry on) instead of spinning bright. The `lcd_wake()` absorb
  window now lands after the panel-init update, not before.
- **Resume keeps the real queue** — the settings record carries queue kind
  (album / songs / artist / genre / shuffle-songs), cursor and two shuffle
  seeds (payload 24 → 44 B, version unchanged, old records still load).
  Shuffle deals are seed-reproducible; the Shuffle Songs double-deal is gone.
  Album remains the fallback.
- **Player** — the pump parked the drive on EVERY pass (hundreds of STANDBY
  commands a second, and the one-shot spin-up probe stayed latched so track
  opens ran without retries); the elapsed clock and seek anchor wrapped at
  71 min; a failed seek now restarts from 0; skips while paused no longer
  kick the DAC (no click); a dropped DMA completion no longer replays a
  buffer on resume.
- **Config/FS** — a slot read error at boot no longer regresses the seq and
  silently discards the session's settings; forced saves wake a parked
  drive first and stay dirty until a reported success (`kernel/cfg_commit.c`);
  a CORELIB.IDX read error is refused instead of reported as "Library too
  large"; an orphaned LFN fragment no longer eats the next file's name;
  `build_index.py` no longer turns "7 rings" into track 7.
- **HAL** — a NACKed battery ADC read is a failed read, not a 3300 mV cell
  (was a false SHUTOFF path); SHUTOFF cannot fire on external power; I2C
  waits for the bus before its one-time reset; ATA ready waits are timed
  (10 s / 31 s after SRST, spin-up 8 s); backlight ISR flags volatile.
- **UI** — low-battery modal survives a plug-in/unplug and the toast is
  timed from first paint; pending SELECT dropped on screen change / Hold;
  modals replace rather than overflow the screen stack; rail-pinned sliders
  don't write; lock plate no longer busy-spins; stale strip gauge repaints.
- **Hold banner (2026-09-14)** — the centred 180x110 lock plate is gone. A
  Hold edge now inverts the TOP CHROME for a second instead: a dot-keyhole
  padlock that pops open, "Locked" / "Unlocked", and HOLD ON / HOLD OFF where
  the header count sits — 22 px on Now Playing, strip + header through the
  divider on lists and Settings. The title and the art are never covered. A
  button press while locked is drained from the wheel latch and re-shows the
  banner; wheel motion is dropped silently. The present is the band alone
  when nothing else is pending (a full frame when the strip is dirty or the
  panel is stale). Gallery redrawn to match:
  `docs/screens/lock.png`, `locked.png`, the new `locked_list.png`,
  `lock.gif`. **UNFLASHED.**
- **Transport confined to the player screens (2026-09-14)** — RIGHT/LEFT now
  skip ONLY on Now Playing and the queue view: a skip from a list you were
  merely browsing changed the music under you. On every other screen RIGHT
  PUSHES Now Playing (MENU returns to the exact row) and LEFT does nothing
  (MENU is already "back"); the wheel delta is cleared so the push does not
  arrive with a stale detent. `top_banner_render` stayed the reusable
  primitive it became, and ellipsises its label to the room left of the
  token. **UNFLASHED.** Bench list: (a) RIGHT on Albums → Now Playing,
  MENU → same row; same from inside a tracklist; (b) RIGHT/LEFT still skip on
  Now Playing and the queue; (c) LEFT on a list does nothing.
  *A track-change banner was built the same day and then dropped: the status
  strip already renames itself when the queue advances, and that is enough —
  no band, nothing to dismiss, one less thing composed over every list paint.*
- **Status strip on the Settings screens (2026-09-14)** — Settings used to be
  the one place the top band went blank: `ui/screen_settings.c` follows the
  jsx, which draws no strip. Crossing from a list into Settings dropped the
  battery and the playing track off the screen. `settings_render_cur()` now
  paints `status_strip_render()` over the painters' clear band (verified: no
  settings painter draws in rows 0..14 — the header's bold-13 ink starts at
  y 16), and the Hold banner's recoloured strip row no longer skips Settings.
  Gallery redrawn. **UNFLASHED.**
- **The strip is blank when nothing is playing (2026-09-14)** — it used to
  fall back to the `CORE` wordmark. It is a now-playing readout, not a
  wordmark: idle now shows only the battery (and the Hold padlock). The main
  menu's header still says `Core`, as does About's chip. Both C sites
  (`status_strip_render`, `top_banner_render`'s strip row) and the gallery
  renderer changed together. **UNFLASHED.**
- **Leaving Settings no longer spins the drive up (2026-09-14)** — the stall
  on every Settings back-out was a forced commit: the exit calls
  `resume_capture()` (which dirties the record whenever a track is loaded,
  because the position moved) and then `settings_commit(CFG_COMMIT_FORCE)`,
  and FORCE with parked platters means `ata_wakeup()` — 1-3 s — BEFORE the pop
  is rendered. New `CFG_COMMIT_SOFT`: no debounce, like FORCE, but a parked
  drive is never woken; the change stays pending. The IDLE rule tightened the
  same way (`parked` alone defers now, not `parked && player_active`) so the
  deferred write cannot ambush the user with a spin-up three seconds later
  mid-browse. The pending write lands the next time the platters turn for any
  reason, or at suspend / power-off / disk mode / the DISKSAFE flush, all of
  which still force. Trade-off, stated in `cfg_commit.h`: a change made while
  the drive is parked is lost only on a HARD power cut before any of those,
  and the suspend timeout forces a commit within 30 minutes of idle.
  `cfg_commit_test.c` grew the SOFT and parked-IDLE cases (40 → 50 checks).
  **UNFLASHED.** Bench: Settings → MENU with the drive parked is instant and
  silent; a setting changed there survives a suspend/wake and a power-off.
- **The boot is one screen now (2026-09-14)** — the "Core Player / loading"
  splash and the separate titled progress bar are gone; `boot_screen_render`
  draws both. The click-wheel mark (an AA ring + dot, `fill_disc_aa` — the
  plates' corner mask stops at r=16 and the ring is 19), "Core" under it, the
  device line under that, and along the bottom a 2 px ink bar with the phase in
  small caps — `LOADING`, then `LOADING LIBRARY` / `LOADING SONGS` /
  `SHUFFLING SONGS`. Bottom right, in the border colour, a build stamp:
  `git describe --always --dirty --abbrev=7`, baked in by a meson `vcs_tag`
  header (`kernel/core_version.h.in`), screen-only — never on the UART, so the
  clicky boot golden is untouched. **The saved theme is applied BEFORE the
  library load**: the settings read (which needs only `fs`) moved ahead of
  `library_ensure`, so a dark-theme user gets the Linen splash for the disk
  spin-up — unavoidable, the theme is *on* that disk — then one flip, and the
  bar, the placeholder album chips (`chip_placeholder_init` moved after
  `theme_set`) and the menu are all Onyx. Boot Details' OTHER bucket (total
  minus the named phases) absorbs the moved config read; `g_lib_load_ms` is
  measured inside `library_ensure` and is unchanged. **UNFLASHED.**
- **Boot Details no longer spins the drive up (2026-09-14)** — the page probed
  CORECFG.DAT's two slot LBAs and the log's header/next LBAs on EVERY paint,
  and each probe walks the FAT chain, so opening it with the platters parked
  cost a 1-3 s wake before the first pixel. The same four values are already
  probed at boot with the drive spinning; they are kept, the page re-probes
  only `if (!ata_is_parked())`, and a failed probe cannot blank a good
  boot-time value. Nothing about the page changes when the drive is up.
  **UNFLASHED.**
- **The unlock banner no longer eats a second of input (2026-09-14, device
  bug)** — while the Hold banner was up, the main loop `continue`d past the
  render for the whole ~1 s window, so every scroll and press after an UNLOCK
  was *applied* but nothing was drawn until it expired. The banner is
  confirmation, not a modal: the first event after Hold comes off disarms the
  window (`g_lock_flash.armed = 0`) and falls through to the normal render in
  that same pass. The LOCKED banner is unaffected — input while locked is
  swallowed earlier and re-arms the window. **UNFLASHED.**
- **`Fa` was tucked under the F's arm (2026-09-14)** — the atlas generator
  re-solves every letter pair optically, putting each pair's MEAN daylight
  over the x-height band on the face's target. A mean cannot see a recess:
  `F`'s right edge in that band is its open stem on most rows and its middle
  arm on one or two, so the solver kept pulling the next letter left until
  the arm row bottomed out on the 1 px no-touch floor. `Fa` at regular 12
  shipped at a 3 px pen step against the design's 6, with the bounding boxes
  4 px into each other where the font overlaps them 1 — and the same for
  every pair whose left glyph overhangs or recesses (`Ta Ya Va LT AV Fz rT`
  …). Fixed in `tools/atlas_gen.py` only, with a cap
  (`OPTICAL_MAX_BOX_OVERLAP_PX = 1`): a pair's boxes may overlap by 1 px, or
  by as much as the font's own kern for that pair already overlaps them,
  whichever is more. It only ever loosens, it moves 45–166 pairs per face out
  of ~4 200–6 000, and every other pair keeps the step the rhythm solve chose
  (the lowercase rhythm sd over `tools/ui_strings.txt` is unchanged to two
  decimals in all six faces). Regenerated: all six `core/ui/atlas/*.h` — kern
  tables only, no bitmap and no advance moved. regular 12 `Fa` 3→6 px,
  `Ta` 5→6, `LT` 4→5, `rT` 2→4; bold 18 `Fa` 8→9, `Ta` 8→10, `Fz` 6→9.
  `docs/screens/render.py` now reads the atlas `_KERN[]` table instead of
  re-deriving the FONT's kerning, and rounds the pen once per pair like
  `text.c`'s `pen_step` — so the gallery shows the device's spacing for the
  first time, and stills with `T/Y/L/V/F` pairs shifted by a pixel or two.
  **UNFLASHED.** Bench: Albums (`F-1 Trillion`, `Taylor Swift`), the Now
  Playing title in bold 18, About — read `Fa`, `Ta`, `LT` at arm's length.

**Device verdicts so far (2026-09-13 evening):** audio noise fixed
(VMID); panel sleep white on wake (off); suspend wakes with the PLL park
off; PMU power-off + wake OK; gauge, scrollbar, seven themes, white blank
flashed. Next bench items: pull CORELOG.BIN after a suspend and `--dump` it,
`chkdsk`, then re-try the 10 Hz tick and the gates ONE AT A TIME with the
log capturing the loop.

**First-flash checklist (all of the above is unverified on the device):**
1. Suspend → wake: panel comes back from `LCD_SLEEP` (white → set
   `SUSPEND_PANEL_SLEEP 0`); drive comes back from SLEEP via SRST (watch the
   UART for the spin-up wait); measure suspend draw before/after.
2. Hold PLAY 5 s → PMU standby; also let a suspend run past 30 min off the
   charger and confirm it powers down.
3. Resume into a Songs / Artist / Shuffle Songs context, press Next, check
   the order; confirm no click on a paused skip.
4. Battery line still reads sane values (the 2800–4600 mV plausibility band
   must not reject a real cell).
5. PLAY tap vs hold feel; a tap must never sleep, a hold must never pause.
6. Panel sleep at idle (`PANEL_SLEEP_AT_IDLE 1`, since 2026-09-13): let the
   backlight time out, wait 10 s, press a button — the screen must come
   back with the right content, not white. If white, set
   `PANEL_SLEEP_AT_IDLE 0` in `kernel/main.c`. Then hold PLAY from dark:
   the panel must wake once, sleep into suspend, and come back once. The
   `core: ui` UART line's `bcm_timeouts`/`refused` should read 0/0.
7. Suspend low power (since 2026-09-13, later): while asleep the SoC runs
   on the 24 MHz crystal with the PLL off, SER0/PWM/I²C gated, and a 10 Hz
   tick. Check: the `core: batt` line still prints sane values 10 s and
   60 s into a suspend (not -1 — the I²C re-gate must bring the controller
   back), the wheel wakes it from a 10 Hz sample, a hold to 5 s still
   reaches PMU standby from the crystal, and measure the draw. Rollback is
   per-park: drop `clock_suspend` from `suspend_lowpower_enter` first,
   then the gates.
8. Paused skips: press Next ×5 while paused, then Play — one bring-up,
   no pops on the skips; a 44.1 → 48 kHz skip while paused then Play
   must come up at 48 kHz.
9. **Event log — the write path's second caller; qualify it before it
   flushes.** `python3 tools/make_log.py --create` (Windows-native copy +
   `Write-VolumeCache`), then `sudo python3 tools/make_log.py --verify
   /dev/sdX`: note the header LBA and the next-write LBA. Boot; the UART
   line `core: evlog on seq .. boot .. lba <hdr>/<next>` MUST show the same
   two numbers. If not, power off before a flush. Then hold PLAY (suspend
   → forced flush, `core: evlog blk 00000000 final`), wake, disk mode,
   `--verify` again: block 1 valid, seq 0, FINAL, and `--dump` prints the
   boot narration; `chkdsk D:` (read-only, no `/f`) clean. Then let it run
   with the log ON until a `core: evlog blk` idle write lands (a full block
   is ~40 battery lines, ~4 min) and `chkdsk` once more. About must read
   `LOG <n> on`; `LOG off` means the file was not found or did not
   validate, and nothing is written.

Still open from the audit: the BCM power gate, the ROM's undocumented
`DEV_EN` bits (USB/FireWire/IDE) and a 32 kHz suspend point — `DEV_EN`
gating of SER0/PWM/I2C and PLL-off in suspend have since landed, see
"suspend power" below. `PANEL_SLEEP_AT_IDLE` is now 1 (unflashed; see
"What works" and checklist item 6).
The reserved playlist queue kind has since been wired — see
**Playlists** under "What works". Closed since: the same-hash
tiebreak (`name_bind_exact`, exact on-disk name wins when a bucket has two
candidates), `flac_meta.c` keeping UTF-8 on the scan fallback, and
`ata_identify()` waiting for !BSY before it reads ERR/DF. Also closed: a
skip while paused ran the full codec bring-up (WM_RESET + VMID charge) only
for the 5 s pause timer to power it down again — the bring-up is now owed to
the resume (`g_pl_bringup_pending` in `player.c`, with the player/HAL state
table; `player-queue` 13e'', `player-clock` 11), so ten paused Nexts cost
zero I²C. Add to the first-flash checklist: Next ×N while paused, then Play —
no pop, sound at the last track's rate. Nothing has been pushed.

### 2026-09-13, later — suspend power: PLL parked, peripherals gated, tick at 10 Hz

`suspend_to_ram` used to idle the SoC at the 30 MHz PLL point with the
PLL running, the 100 Hz tick waking the core to read the wheel, and the
UART, PWM and I2C clocks on. Now, after the drive/panel/backlight are
down (`suspend_lowpower_enter`, `kernel/main.c`):

- `clock_gate_suspend` — `DEV_EN` (`0x6000600C`) bits 6 (`DEV_SER0`),
  17 (`DEV_PWM`), 12 (`DEV_I2C`) cleared, one masked RMW each through
  the owning driver; each block re-gates itself on its next use, so the
  codec-off write, the 5 s battery sample and the PMU standby command
  work unchanged. `DEV_OPTO` (wake source) and the ROM's `0xC2000124`
  bits are not touched.
- `timer_set_rate(10)` — `TIMER1_CFG` (`0x60005000`) = `0xC0000000 |
  99999`; the tick counter keeps its 10 ms unit via the `USEC_TIMER`
  reconcile, the wheel is sampled at 10 Hz ("hold any button" wakes),
  and the loop also wakes on a latched button down-edge.
- `clock_suspend` — `CLOCK_SOURCE` (`0x60006020`) = `0x20002222`,
  `DEV_TIMING1` (`0x70000034`) = `0x0303`, `PLL_CONTROL` (`0x60006034`)
  `&= ~0x88000000`, `DEV_INIT2` (`0x70000020`) `&= ~0x40000000`. The
  24 MHz crystal, not the doc's 32 kHz point: that one is for a core
  that has stopped, and nothing in the doc says which peripheral clocks
  follow the core there.

The battery sample, every path into `enter_standby`, and the wake path
restore all three first (resume = the `clock_init` grammar, `TIMER1_CFG`
back to `9999`, the `DEV_EN` bits set again). Trace-tested host-side
(`hw-clock`, new `hw-clock-gate`, `hw-timer`; 55 suites), ARM `-Werror`
+ `verify-hw` clean. **Unverified on the device**: the saving itself
(nothing in `docs/hw/` gives a current figure for any of it — measure
suspend draw before/after at the bench), whether the I2C controller
needs a reset pulse after a gate, and the wheel/UART at the crystal
rate. If a suspend misbehaves, the three parks are independent — start
by leaving `clock_suspend` out of `suspend_lowpower_enter`.

### 2026-09-13, evening — first flash: what the device said

- **Panel sleep: WHITE on wake.** The backlight timed out during playback,
  the panel slept, and the first press came back solid white. Both
  `PANEL_SLEEP_AT_IDLE` and `SUSPEND_PANEL_SLEEP` are back to 0; the
  `lcd_sleep` before the PMU standby command is gated on the same flag.
- **Suspend: SOLVED, late.** Every short-hold suspend went dark and never
  woke; the 5 s hold (PMU power-off) came back from the third image on.
  Three bisect flashes of the low-power switches all "failed" because the
  review-round re-park guard in the idle loop ran UNCONDITIONALLY — the
  PLL park (CLOCK_SOURCE → crystal, PLL off) executed on every image,
  "all off" included, and the wake side never un-parked it. With the guard
  gated, the short-hold suspend wakes and resumes. So: **the PLL park
  breaks the wake** (wheel or tick does not survive the crystal source as
  written); the 10 Hz tick and the SER0/PWM/I2C gates were never actually
  tested alone. All three stay 0 (`SUSPEND_PARK_PLL/SLOW_TICK/GATE_CLOCKS`);
  the drive parks with STANDBY (`SUSPEND_ATA_SLEEP 0`) — SLEEP was swapped
  out mid-bisect and is untested on its own too.
- **Event log** (`CORELOG.BIN`, 4 MiB, on the device since the evening):
  Boot Details reports header/next LBAs 49238456 / 49238464 — header ~1 MB
  past CORECFG.DAT's slot, next block inside the header's cluster. Raw-disk
  `make_log.py --verify` still owed for the record.
- **Audio: FIXED and CONFIRMED.** Hiss with nothing playing, the wheel click
  on the jack and the drive's spin-up on the jack were all one thing: the
  codec played through VMIDSEL 0x2, the datasheet's low-power STANDBY
  divider (worst supply rejection). Playback now uses the normal 0x1
  divider, and the codec is forced cold at boot (a Menu+Select reset keeps
  its rails up). Second flash: jack silent idle, silent paused, no click
  bleed, no drive noise — owner confirmed all four.

### 2026-09-13, later — cross-branch review, two rounds

Five reviewers went over the merged result looking for what each parallel
author could not see. Fixed: a slow suspend entry (spin-up for a forced
save) pushed the PLAY hold past the 5 s escalation and turned a requested
sleep into a power-off — escalation is now also timed 2.5 s from when the
screen went dark (`SUSPEND_ESCALATE_DARK_US`); an in-suspend DISKSAFE
write left the drive in STANDBY instead of SLEEP (`ata_is_slept()` gate);
`ata_sleep()` no longer flushes an already-parked drive; a wake reset that
times out fails fast instead of stacking three budgets; the keyhold swallow
works before the sampler shows the press but only for PLAY and only for a
couple of feeds; a wedged I2C controller is reset on re-init; Next takes the
prefetched hand-over (Repeat-All + Shuffle re-deal) unless it is the same
track (Repeat-One); playlist resolution walks one folder per album instead
of the root per track, stops on a failing disk, and reports unusable
entries. Add to the first-flash checklist: open a 100-track playlist and
time it; hold PLAY on a dirty settings record and confirm the device
sleeps rather than powers off.

### 2026-09-13, later — the DAC's position is asked, not guessed

`hal_audio_frames_played()` (hal.h) reports what the converter has actually
clocked out. Hw: completed DMA buffers plus the timed part of the current
one (kick timestamp x rate — the late-kick detector's own figures), so the
residual is the 16-frame I2S FIFO (~0.4 ms) plus however late the pump pass
that reads it lands; frozen across stop/start, unmoved by a flush, zeroed by
init. Sim: SDL's pulls less the 1024-frame buffer in flight, interpolated
the same way, so the residual is the host mixer's own latency rather than a
guessed 350 ms (the sim does not link the player today; the backend is
there for when it does). `player.c`'s `frames_heard()` now pairs that count
with the ring's (`heard_rebase()` at every `hal_audio_init` and
`hal_audio_flush`) instead of subtracting a hardcoded 2 x 8192, so the
gapless hand-over — and the clock it anchors — lands on the true frame
where it was 0..186 ms late. The elapsed clock's accumulator/anchor design
is unchanged. Covered by `hw-audio-pingpong` (section 11) and
`player-clock` (sections 6–10: FIFO/SDL/device depths, count wrap, a
codec-cold pause, a seek, a 44.1 → 48 kHz hand-over). Unflashed: on the
device, listen for the title and clock flipping exactly as the next track
starts, and watch `audio_late_kicks()` — a late completion is now also a
late position, capped at the buffer.

### 2026-09-13, later — an on-disk event log (unflashed, device-gated)

Every `core:` line the UART carries is now also captured into an 8 KiB RAM
ring (`kernel/evlog.c`, a weak-symbol tap in `hal/hw/uart.c`; the wire bytes
are unchanged, the clicky golden still matches) and flushed one 2048 B block
at a time into `CORELOG.BIN`, a 4 MiB ring `tools/make_log.py --create`
pre-allocates next to `CORECFG.DAT`. Same rules as the settings file: the
device never creates or grows it, block 0 (header: magic/version/block
size/count/id/CRC) is validated at mount and never written, every block's
LBA is re-resolved through the cluster chain (`fat32_file_lba_at`, new)
before every write, whole physical sectors only, and the write goes
through `cfg_commit_gate` — idle: one full block, debounced, never wakes a
parked drive; forced at suspend entry and standby (FINAL block, wakes the
drive first); the DISKSAFE last write is exempt; nothing below it. The
cursor is found at boot by a binary search over the ring's seq numbers
(~11 reads for 2047 slots), not a sweep. Boot line:
`core: evlog on|off seq .. boot .. prev none|final|unflushed lba <hdr>/<next>`
— `prev` is the boot reason as far as the log knows it. About shows
`LOG <seq> on` / `LOG off` / `LOG <seq> err`. New narration: suspend
entering / idle loop / wake, standby entering, each flush's result.
Host: `evlog` (150 cases: format, the host tool's fixture decoded by the
firmware, every mount refusal, the whole flush policy, ring wrap, torn
blocks, the O(log n) scan on 64 slots) and `make-log` (the `--dump` round
trip), plus `fat32_file_lba_at` cases in `fat32`. 57 suites green, ARM
`-Werror` + `verify-hw` clean, image +14 KB. **Not flashed**: checklist
item 9 is the qualification `kernel/config.c`'s banner demands of a second
caller of `ata_write_sectors()` — this is that caller.

**To pull the log:** disk mode → `powershell.exe Copy-Item D:\CORELOG.BIN
C:\Users\<you>\CORELOG.BIN` (Windows-native; a drvfs read can serve a stale
copy) → `python3 tools/make_log.py --dump /mnt/c/Users/<you>/CORELOG.BIN`
(`--blocks` for the per-block headers).

### 2026-09-13, night — stutter solved, About redesigned, both confirmed on the device

The event log paid for itself on its first pull: all 12 "audio underrun"
counts came at one moment, playback start, coincident with "lcd: BCM
idle-wait timed out" and a 20 ms present gap — a partial present handed to
the BCM while it was still retiring the previous full frame stalled the main
loop past the PCM ring. One more timeout after every "suspend: wake" was the
resume stutter. Fix (1a7067b): only FULL presents set `g_present_cost_us`,
the transport partial is paced by `present_gap_us()` since `last_present`,
and `last_present` is re-stamped after `suspend_to_ram` returns.
`EVLOG_RING_BYTES` 8 → 16 KiB (the log was dropping bytes at boot).

About (bcb7bca) is now a dashboard: name + Core chip on one row, the three
stat columns, STORAGE and BATTERY on side-by-side `PAL_PLATE` cards each
with a big value, its own gauge and a caption ("53.5 of 74.5 GB", "3912
mV"), and one muted footer "ADC n · LOG n on". The "library too large"
warning is drawn by the renderer (`lib_truncated` argument), not painted
over from main.c. Rendered on the host for every theme before flashing.

**Device verdicts (owner, all three):** About layout good; play from a cold
boot → Boot Details underruns 0; short-hold suspend → wake → resume with no
stutter, underruns still 0. 58 suites green, ARM `-Werror` + `verify-hw`
clean, readback-verified flash.

**CORRECTION, same night (second log pull):** the pacing fix was not the
whole story. A run that ended in a power-off, then a cold boot that
restored the track PAUSED, then Play ~2 min later: 11 underruns, ring_low
8 %, decode 61 ms/kframe for 25 s, 20 BCM timeouts. The buffer under a
paused-at-boot track held only the 1.49 s ring prime's worth of file; the
main loop's 20 s idle spin-down parked the drive; Play drained the ring
into diskbuf's synchronous fallback, which blocked on the spin-up. Fix
(d9197e4): the paused pump stocks the anti-skip buffer to DISK_LOW while
the platters are up (never on a parked drive), the playing pump honours
`ata_is_parked()` alongside its own flag, and `player_resume` pre-pays
the spin-up before the DAC starts when the buffer is under DISK_LOW/2.
Five host cases (player-queue 13b). **Owner: "no more underruns" on all
three sequences** (paused-at-boot → park → Play; pause mid-song → park →
resume; suspend while playing → wake). Third log pull: the one flushed
window of that boot has a pause, a park and a resume with underruns 0 and
bcm_timeouts 0 — so the BCM timeouts were a side effect of the stalls,
not a separate fault. Most of that session was lost, though: the log's
last flush was a suspend entry and the rest sat in RAM through the
reboot into disk mode. 1879fa0: entering Disk Mode from Settings now
forces a settings commit and a FINAL log block first (narrated
"core: disk mode: entering"). The ROM Select+Play route still loses RAM
— unavoidable.

**Pushed 2026-09-14 (054c722..353f2de, 108 commits), CI green on every
job.** README and core/README rewritten for the device as it is; the
docs/screens gallery re-rendered against the firmware (render.py repaired —
it had not run since the atlas set changed; docs/screens/README.md holds
the rules; Settings-side screens checked against host renders of the real
C, library-side against main.c). Next bench: the three suspend switches
one at a time with the log capturing; playlists with real .m3u8 files.

**End of night, device image = 1879fa0.** Confirmed on the device today:
silent jack (VMID), steady charge gauge, scrollbar under fast scroll,
white blank at sleep, seven themes on a five-row picker, short-hold
suspend + wake, 5 s power-off + wake, event log end to end, About
dashboard, zero underruns on every resume path. Open: SUSPEND_SLOW_TICK
and SUSPEND_GATE_CLOCKS untested alone, clock_suspend (PLL park) needs a
rework before it comes back, ATA SLEEP vs STANDBY untested alone, USB
ground-loop noise is the cable. Nothing pushed.

## Where we are right now (2026-07-28)

**A full music player on real hardware, and it is now the device's own
firmware.** `core` is written into the firmware partition as the OSOS
image: the Apple boot ROM hands straight to `crt0.S`, which does the MMAP0
SDRAM remap itself. There is no chainloader, no boot menu, and no Apple
firmware left on the device — `ipodpatcher -wf` replaces the whole OSOS.
Recovery is unchanged and unconditional (see below).

The whole bare-metal stack is proven end to end on an actual iPod 5.5G
80GB — boot ROM → `crt0.S` + MMAP0 remap → clock/PLL → timer/IRQ → LCD
(BCM present) → I²C/WM8758/I²S → DMA → ATA PIO → FAT32 — and on top of it
a real player: **streaming FLAC off the iPod's own disk** (read-ahead over
an 8 MB anti-skip buffer, not preload, so full-length tracks play), a
host-built library index (`CORELIB.IDX`) for instant Songs / Albums /
Artists / Genres, **settings that persist to disk**, **resume-on-boot**,
and the full themed UI (Linen/Onyx plus five more palettes, see Settings
below). On-screen framebuffer console plus the new
Settings → Boot Details page are the cable-free debug channel; there is NO
serial cable (confirm hw state on screen instead).

**★ THE ROUTE BACK TO DISK MODE IS THE ONE THING NOTHING MAY COMPROMISE.**
Hold **Select + Play** at power-on. That lives in the boot ROM and runs
before any image loads, so nothing we flash can remove or pre-empt it —
proven on this device under the worst case (our firmware as the sole OSOS,
Apple's gone, black screen). Technique: let it go fully off, unplug, hold
Select + Play *first*, then plug USB in while still holding. The firmware
also offers a Disk Mode row under Settings, but that is convenience; the
ROM combo is the floor.

**Flashing changed with direct boot.** Copying `core.ipod` to the FAT32
data partition now does **nothing** — nothing loads it. The procedure is
`ipodpatcher <disk> -wf build-hw/core.ipod` with raw block-device access
(sudo on native Linux; an elevated shell on Windows, and pass `< NUL` or
ipodpatcher blocks on stdin), then **always verify**: `ipodpatcher <disk>
-rfb readback.bin` and `cmp` it against `core/build-hw/core.bin`.
Byte-identical or don't boot it. Back up the partition first (`-r`);
restoring it (`-w`) puts Apple's firmware back.

**Dev-environment note:** the clean flash environment is **native Linux**
(the iPod is a real `/dev/sdX`). The WSL path works too but goes through
Windows interop — `/mnt/d` writes silently do not persist (drvfs cache),
so file copies must be Windows-native + `Write-VolumeCache`. Toolchain on
Arch is all official `extra`: `pacman -S arm-none-eabi-gcc
arm-none-eabi-binutils arm-none-eabi-newlib meson ninja pkgconf`, then
`make hw` / `make ipod` / `make sim` from `core/`.

## What works on the device today

- **Direct boot** — our image is the OSOS; `crt0.S` sets up an IRAM stack,
  runs the remap stub from IRAM, copies `.data`, zeroes `.bss`, brings up
  cache, wakes the COP, and enters `kernel_main`. The backlight is lit
  *before* the LCD probe on purpose, so "backlight on" means our code ran.
- **Bring-up** — clock/PLL (30 MHz, refcounted 80 MHz boost), IRQ + 100 Hz
  timer, unified-cache management, real fault handlers + panic screen.
- **Display** — BCM framebuffer present path, including `bcm_init()` so a
  wedged BCM is no longer terminal (we can no longer assume a chainloader
  left it idle at frame one); on-screen console; the full RGB565 UI with
  damage-tracked partial presents.
- **Audio** — WM8758B bring-up over I²C, I²S transport, DMA-driven
  continuous playback fed by an SPSC PCM ring drained by the
  DMA-completion ISR.
- **Storage** — PIO ATA reader (aligned bulk reads straight into the caller
  buffer) + from-scratch read-only FAT32 (long names decoded to UTF-8),
  every chain walk bounded and every cluster validated.
- **Streaming decode** — `dr_flac` freestanding, fed by a read-ahead disk
  source over an 8 MB anti-skip buffer; a full-length track streams off the
  disk while the UI stays live. **FLAC only.** `dr_mp3` is built and linked
  and passes its host KAT, but MP3 is switched off on the device
  (`CORE_ENABLE_MP3 0`, `core/kernel/main.c:728`) and `classify_ext()` does
  not surface `.mp3` at all, so those files are invisible in the browser.
  Its float synthesis filter cannot hit real time on this FPU-less CPU —
  the PCM ring starves and playback stutters. Parked, not removed;
  re-enabling needs a fixed-point or second-core decoder.
- **FLAC seeking is O(log n)** — `DR_FLAC_NO_CRC` had silently compiled out
  dr_flac's binary-search seek (the search needs the header CRC to tell a
  real frame header from audio that looks like a sync code), and these
  files carry no SEEKTABLE, so every seek decoded from the start of the
  track. That was 5.1 s of an 8.4 s cold boot. Costs ~14 KB of text and
  some decode margin, which Boot Details now reports rather than assumes.
- **Library** — host-built `CORELIB.IDX` loads in one read → instant Songs /
  Albums / Artists / Genres; caps are 6000 songs / 1024 albums / 512
  artists / 128 genres, and hitting one sets a truncation warning rather
  than silently dropping tracks. Sorts are O(n log n). Records carry UTF-8
  fields + a normalized-name hash that binds each to its file independent
  of quote/case style (falls back to a per-file tag scan if the index is
  absent).
- **Settings persistence** — `CORECFG.DAT`, pre-allocated by
  `tools/make_config.py`, two alternating 1024 B slots (one whole *physical*
  sector each; this drive rejects sub-physical-sector access with IDNF),
  each record CRC-32 checked over its entire body including the header. The
  device never creates, grows, moves or deletes the file — it overwrites
  the bytes of the file's own first cluster, so zero FS metadata changes.
  The target LBA is re-resolved and re-validated through `fat32_file_lba()`
  before *every* write. `config_save()` was the only call to
  `ata_write_sectors()` in the firmware until the event log below became
  the second. **Proven on hardware 2026-07-27**; slot alternation
  confirmed from a raw disk dump, and `chkdsk` found no problems
  afterwards.
- **Event log** (unflashed, device-gated — checklist item 9) —
  `CORELOG.BIN`, pre-allocated by `tools/make_log.py` (4 MiB: a header
  block + 2047 ring blocks of 2048 B = two physical sectors each). The
  firmware taps every UART byte into an 8 KiB RAM ring and flushes it one
  block at a time (16 B header: magic, seq, boot id, length + FINAL bit,
  CRC-32; 2032 B of text) at block `1 + seq % 2047`, LBA re-resolved
  through the file's cluster chain before every write, through the same
  commit gate as the settings save (idle: full block, debounced, drive
  already up; forced at suspend/standby; the DISKSAFE last write; never
  below it). Block 0 is never written; if it does not validate the log is
  off and About says `LOG off`. Pull it: disk mode → `powershell.exe
  Copy-Item D:\CORELOG.BIN C:\...` → `python3 tools/make_log.py --dump`.
- **Resume on boot** — comes back on the track you left, **paused**, at the
  saved position, **in the queue you were playing it in**: Songs, an
  artist's songs, a genre, the same Shuffle Songs draw, or a playlist (the
  record's once-reserved context word now holds the playlist's name hash,
  `RESUME_KIND_PLAYLIST`), with the same shuffle order (Next is still the
  track that was coming next). Bound by
  the folded `name_hash()` of the filename (not an index or a cluster),
  cross-checked against duration ±2 s and required to be unique otherwise,
  so it survives a library rebuild. The queue is rebuilt from a kind byte
  plus two seeds (the record grew 24 → 44 bytes under the same version;
  old records still load and fall back to the album). Positions under 10 s
  aren't seeked. Any doubt at any step falls back to the track's album,
  and past that leaves the device exactly as if nothing had been saved.
  Parity between the C and host implementations is gated by
  `check_resume_parity.py` in `make verify-hw`. Not yet flashed.
- **Boot Details** (Settings → Boot Details) — a live per-phase breakdown of
  the last cold boot: LCD/BCM, disk + mount, library, resume (split into
  dir / open / seek), and OTHER derived as total-minus-named so unmeasured
  time shows up instead of vanishing. TOTAL is measured independently, not
  summed. Plus FLAC decode cost as a % of the 22676 µs/kframe real-time
  budget, the underrun count, and the CFG seq + slot LBAs. **Cold boot
  measured 8.4 s, of which 5.1 s was one FLAC seek** (since fixed). The
  post-fix total has NOT been read off the device — Boot Details shows it
  live; take the number from there rather than quoting arithmetic.
- **Power management** — CPU scaled to 30 MHz and halted at idle; the HDD
  spun down after 20 s idle, including while paused (and the parked flag
  now survives a settings write, so it re-parks afterwards); the codec
  powered down and the audio clocks gated at stop; the lock/unlock plate
  repainted on every Hold edge. **The power button** (`ui/keyhold.c`,
  host-tested): PLAY decided by press length — a tap toggles pause on
  release, a 2 s hold suspends, 5 s escalates to PMU standby; a
  hold-to-sleep no longer pauses first, so wake resumes. **Suspend**
  (`suspend_to_ram`) now actually powers things down: drive to ATA SLEEP
  (flush, standby, `0xE6`; a reset wakes it), panel to `LCD_SLEEP`
  (`SUSPEND_PANEL_SLEEP 1`, rollback documented beside it), clock
  unboosted and then **parked** — `DEV_EN` SER0/PWM/I2C gated, `TIMER1`
  at 10 Hz, PLL disabled + unpowered on the 24 MHz crystal (registers
  listed under "suspend power" above; **unverified on the device**,
  draw unmeasured) — codec off via the pump; it samples the battery on
  the 5 s cadence (clocks restored around the sample) and runs the
  DISKSAFE/SHUTOFF policy while asleep, escalates to
  real standby after `SUSPEND_TO_STANDBY_US` (30 min) on battery, and on
  wake does `lcd_wake` -> present -> backlight (the present retires the
  panel's wake-init before returning — `bcm_frame_commit` used to absorb
  BEFORE the update, i.e. never) and skips the resume if the jack is
  known empty. **Standby** (`enter_standby`) quiesces first (codec,
  settings, drive, panel) and is no longer `_Noreturn`: a PMU that refuses
  the write gets a relit, repainted, running device instead of a dark
  dead one. **UNVERIFIED ON THE DEVICE, all of it** — none of this has
  been flashed: whether the panel comes back from `LCD_SLEEP` (if it wakes
  white, `SUSPEND_PANEL_SLEEP 0`), the drive's post-SLEEP reset wake, the
  suspend draw before/after, and the refused-standby recovery. **LCD panel
  sleep at idle is ON** (`PANEL_SLEEP_AT_IDLE 1`, 2026-09-13, rollback
  documented beside it; **equally unflashed**): at backlight-off the loop
  now issues `LCD_SLEEP`; the wake is one block in the main loop that does
  `lcd_wake` -> paint -> full present (retires the panel init) -> backlight,
  the input sites no longer light the LED over a slept panel, and a present
  refused while slept is counted (`lcd_presents_refused`, host-tested) so
  the wake frame is full by construction. Idle sleep and suspend compose
  (sleep twice, wake once — idempotent, host-tested). `panic()` now wakes
  and lights the panel before it draws. `bcm_init()` exists but is compiled
  out. The traced idle path is in the commit message of the flip.
- **Charging** — `charger_set_max_current(500)` asserts HPWR so the LTC4066
  uses the 500 mA input cap instead of the 100 mA one we had been
  inheriting from the boot ROM. **UNVERIFIED ON HARDWARE** — needs an
  inline USB current meter. (Also out of USB spec without enumeration,
  which we can't do; safe on wall chargers and essentially all root ports.)
- **Full UTF-8 names** — atlas covers Latin-1 + smart punctuation, the text
  renderer decodes UTF-8, and display sources tag text so FAT-illegal
  characters (`?,*,:,/`) show correctly.
- **Text rendering** — the pen is 26.6 fixed point and carries the
  fractional advance across a whole string instead of rounding per glyph;
  each of the six shipped faces (Nunito regular 9/11/13, bold 11/13/17)
  carries its own kerning table (1,742–3,657 pairs — we previously applied
  none) and its own tracking value, solved from measured ink-to-ink
  daylight rather than guessed. Two host tools back this:
  `tools/text_preview.c` links `core/ui/text.c` and the shipped atlases
  unmodified and renders the real stack to a PPM, and
  `tools/text_metrics.py` reproduces the device's pen arithmetic against
  the baked alpha bitmaps and reports per-pair spacing (`--target` solves a
  tracking value). *Type still doesn't read right at 9–13 px; the remaining
  lever is the face, not the spacing — Nunito ships no TrueType hinting.*
- **Browsing UI** — main menu, Music submenu, Artists / Albums / Songs /
  Genres, an artist's **All Songs** (whole discography in one list, album on
  the sub-line), album detail (hero art + tracklist with per-disc sections +
  durations), scrolling marquee for long titles, two-line rows with 28 px
  album-art chips, letter-stepping on a sustained fast spin (Songs only).
- **Playlists — read path** (2026-09-13, **not yet flashed**) — Music →
  Playlists (and the main menu's Playlists row) lists
  `Music/Playlists/*.m3u8` (`.m3u` too) by filename, A–Z, re-read on every
  entry. Open one and it is a tracklist in the Songs shape (tag title /
  artist / duration for tracks the index knows, the filename for ones it
  doesn't); SELECT plays the whole playlist from that row, each track with
  its own album's cover. Entries may be volume-root-absolute
  (`/Music/Artist - Album/01 Song.flac`) or relative to `Music/Playlists/`
  (`../Artist - Album/01 Song.flac`); `\` separators and a drive letter are
  tolerated. A missing, non-audio or unreadable entry is skipped and
  counted, never fatal, and an empty result says which nothing it is.
  Caps: **64 playlists, 128 tracks per playlist** (the first that many;
  the overflow is reported, not silent). Rows bind to the library by the
  same full-name locator hash the album tracklist uses. Lives in
  `core/library/playlist.c` over `fs/m3u.c` + the new
  `fat32_resolve_path()`, host-tested end to end on an in-RAM volume with
  real VFAT long names. On the no-`Music/` layout the folder is
  `Playlists/` at the root. Writing playlists is still item 1 below.
- **Now Playing** — 120×120 cover, title/artist/album, TRACK N OF M,
  elapsed / −remaining, a rounded progress bar, shuffle/repeat tokens,
  battery, and wheel seek.
- **Gapless hand-over** — at decoder EOS the next track is opened over the
  same arena, disk buffer and ring while several seconds of the old track
  are still queued; when the two share a sample rate the DAC is never
  stopped. Presentation (title/clock/art) is deferred until playback
  crosses the boundary. **Not yet confirmed by ear on the device.**
- **Overlays** — volume (skinny-wave speaker icon + fill bar + %),
  lock/unlock padlock modals, charging screen ("CHARGED" when done), boot
  splash. Anti-aliased modal/progress corners.
- **Settings** — nine rows: Playback (shuffle / repeat / **resume**), Sound
  (volume / bass / treble / balance via the WM8758 EQ), Theme (seven live
  palette swaps — Linen, Onyx, Sage, Plaster, Olive, Umber, Mushroom; the
  picker scrolls, an unknown stored id lands on Linen), Display (backlight
  timeout + brightness), Clicker
  (7 piezo profiles + Off), About (dashboard: song/album/artist counts,
  storage, battery), Boot Details, Disk Mode, Reset. The list scrolls and
  has a scrollbar. Placeholder rows that did nothing were removed.
- **Battery gauge** — proportional fill, turns red at ≤20%.

## What's NOT done (pick up next)

1. **Playlists — write path.** Reading is done (above, unflashed). Saving
   or editing a playlist means creating and growing a file, which means
   **FAT32 cluster allocation**, which does not exist. The only write we
   have is an in-place overwrite of one pre-allocated file's first cluster
   (`config.c`). The read path was a day; the write path is a filesystem
   project.
2. **Search** — not implemented.
3. **A screen-tuned font face.** Advances, kerning and tracking are all
   fixed and measured, and the type still reads wrong at 9–13 px. Nunito
   ships no hinting bytecode, so the next lever is swapping the face for
   one designed for small sizes — not more spacing tuning.
4. **Flash and measure the power work** — the suspend/standby changes
   above are entirely device-unverified; the first flash should check the
   panel wakes from `LCD_SLEEP` (both after a suspend AND after a backlight
   timeout — panel sleep at idle is now on, checklist item 6), the drive
   wakes from SLEEP, and read the suspend draw. Panel sleep at idle is the
   one with the wider exposure: it fires on every backlight timeout, so a
   white wake there is the first thing to look for; the rollback is
   `PANEL_SLEEP_AT_IDLE 0`.
5. **Podcasts / Audiobooks / Composers** — greyed placeholders in the
   menus; no backing implementation.
6. **More codecs** — AAC / ALAC / Vorbis / Opus / WAV are stubbed in
   `codecs/README.md`; only FLAC is wired (MP3 builds but is disabled).
7. **On-device screenshot capture** — `lcd_screenshot_bmp()` is declared in
   `hal.h` and implemented only in the sim HAL; nothing calls it. The
   README shots come from `docs/screens/render.py`, which is a **standalone
   Python/PIL reimplementation** of the UI (real Nunito faces, real
   palette, real layout — but not a single pixel from firmware code). It
   can drift from the device silently. A device capture path would end that.
8. **Library sync is manual** — build the index on the host
   (`tools/build_index.py`), convert art (`tools/coreart.py`), pre-create
   `CORECFG.DAT` (`tools/make_config.py`) and `CORELOG.BIN`
   (`tools/make_log.py`), and copy to the device.
9. **Host CLI install/flash/recover are stubs** —
    `core/cli/internal/cli/install.go` says so outright; flashing is
    `ipodpatcher` by hand today.

## Testing

`meson test -C build-sim` from `core/` (**57/57** green):

- **Codec KAT** — FLAC + MP3 decoders bit-exact against reference PCM.
- **MMIO golden traces** — each freestanding hw driver is host-compiled against a
  recording mock bus (`-DMMIO_MOCK`) and asserted to emit its exact ordered
  register grammar (I²C, WM8758 bring-up, I²S, DMA, LCD present, BCM init,
  UART, clock, timer, battery, volume, backlight, and the PMU standby write).
- **Audio ping-pong** — the buffer state machine in `hal/hw/audio.c`: buffers
  alternate, no audio is repeated or dropped across DMA completions, a short
  source read zero-pads exactly the tail.
- **Player queue** — `core/player/player.c`'s queue, auto-advance, repeat,
  shuffle, next/prev, pause/resume, the gapless hand-over and the incremental
  queue builder, with the real player compiled against fake disk/codec/DAC.
- **Library locator hash** — golden vectors asserted from BOTH the C side and
  `tools/build_index.py`, plus a diff proving the testable copy still matches
  `kernel/main.c`. The hash is the only thing binding an index record to its
  file; if the implementations drift the track silently stops resolving.
- **Resume** — the matcher's name/duration/uniqueness rules, plus a parity
  check against the host side.
- **Settings persistence** — the config record layout, CRC, slot alternation,
  LBA resolution and the ATA write bus grammar.
- **Event log** — `kernel/evlog.c` on a RAM disk with a deliberately
  fragmented `CORELOG.BIN`: the block format and every rejection, the host
  tool's `--emit` fixture decoded by the firmware, every mount refusal, the
  flush policy through the real `cfg_commit` gate, ring wrap and remount,
  torn blocks at and off the cursor, an unreadable slot, the O(log n) boot
  scan; a recorder refuses any write outside the file's own clusters.
  `make-log` round-trips a synthesized ring through `--dump`.
- **FAT32** — the happy path, path resolution (`fat32_resolve_path`: every
  separator/case form, every refusal, EIO vs ENOENT), plus corrupt images:
  cyclic FAT chains, out-of-range clusters, a FAT16 boot sector, orphaned
  LFN runs, a truncated volume.
- **M3U8** — the reader against the playlists that actually break parsers.
- **Playlists** — `library/playlist.c` end to end on an in-RAM volume with
  real VFAT long names: the listing, every entry shape, every failure as a
  count, both caps, an unreadable album folder vs an unreadable playlist.
- **readahead / diskbuf / scheduler / console / clickwheel / pcm-ring / text /
  thumb / FLAC metadata / MP3 tags** — unit tests.

Some of those assert the *documented* contract rather than current behaviour and
are marked as expected failures where the fix belongs to another file. Run
`CORE_TEST_STRICT_XFAIL=1 meson test -C build-sim` to turn every such marker
into a hard failure and see what is still outstanding.

Note the suite only exists in a **sim** configure (`if target == 'sim'`); a hw
build registers zero tests. Seven of the 54 need `python3` (the generated
FAT32 images and the four parity/consistency scripts) — without it you get
47.

**Read this before trusting a green suite.** For a long stretch this section
said "25/25 green" and that number came from *stale gcc-14 binaries* — `meson
test --no-rebuild` happily runs whatever is already in the build directory. A
real rebuild failed: gcc had rolled 14 → 16 underneath the tree, gcc 15 had
added `-Wunterminated-string-initialization`, and the `-Werror` hw build no
longer compiled. `.github/workflows/ci.yml` now builds everything from scratch
on every push for exactly this reason, on both a pinned toolchain and the
current rolling one, and runs the suite again under ASan + UBSan.

Also gated by `make verify-hw` (and in CI) — five static checks against the
linked ARM image:

- `tests/scripts/check_hw_layout.sh` — crt0 / linker layout asserts.
- `tests/scripts/check_size.sh` — size budget; prints text/data/bss and fails
  past the documented ceiling.
- `tests/scripts/check_hw_consistency.py` — `hal/hw/pp5022.h` addresses vs
  `docs/hw/`.
- `tests/scripts/check_name_hash_parity.py` — the host tool's name hash
  against the golden vectors the C side (`library/names.c`) is tested on.
- `tests/scripts/check_resume_parity.py` — the resume matcher across C and host.

Current image: **~298 KB text, 260 B data, ~11.45 MB bss** (budgets 1 MB /
64 KB / 14 MB, image ceiling 30 MB inside the 32 MB SDRAM window). bss is at
81% of its ceiling — there is ~2.5 MB of headroom, not the "room for another
MB-scale buffer" the script's inline comment still claims. The FLAC CRC tables
and binary search are ~14 KB of the text growth.

The device build (`make hw` / `make ipod`) is the authoritative compile for the
firmware; the clang lints about `hw/pp5022.h` / `LCD_WIDTH` / `mmio_*` are
include-path noise from editor tooling, not build errors.

## Known stale/incorrect notes elsewhere in the tree

Found while writing this doc. Worth fixing when you next touch these files
(they are not this doc's to edit):

- `core/cli/internal/firmware/checksum.go:7` still says the partition checksum
  is `sum(bytes) + ModelNum`; `08-boot-dock.md:64` corrects that from a real
  device (no `MODEL_NUM` seed). Line 21 also still calls ipodloader2 "the
  shipping path".
- `core/docs/hw/08-boot-dock.md` still describes preserving Apple's
  `entryOffset` so we can chain back to it, and calls disk mode
  bootloader-mediated in one place and ROM-level in another. The ROM answer is
  the proven one.
- `core/docs/design/settings-persistence.md:5` says the write path is "not yet
  verified on hardware" — it was, on 2026-07-27.
- `core/fs/fat32.c:96` says the ATA write primitive "is not wired to anything
  yet" — `kernel/config.c` uses it.
- `core/player/player.c:64` comments "Sizing (4 MB)" next to an 8 MB define,
  and derives ~37 s of audio from it (the define's own comment says ~73 s).
- `core/tests/scripts/check_size.sh:17-24`'s inline "currently ~" figures are a
  release behind (226 KB → 298 KB text, 10.1 MB → 11.45 MB bss).
- `core/ui/atlas/nunito_bold_9.h` is generated by `tools/atlas_gen.sh` and
  committed, but no source includes it and `atlas.h` doesn't declare it —
  ~171 KB of dead source.
- Fixed while this was being written: `core/boot/crt0.S`, `core/hal/hw/power.h`
  (both said a bootloader hands off to us) and `tools/README.md` (now documents
  `text_preview.c` and `text_metrics.py`).

## Repository

GitHub: https://github.com/BrandonDedolph/ipod-core.

**`main` is NOT necessarily current.** This line used to claim it was; work
lands on local branches first and several commits have sat unpushed at a time,
so treat the remote as a floor, not the truth. Check before assuming:

```bash
git log --oneline origin/main..HEAD    # commits you have that the remote doesn't
git status -sb                          # ahead/behind for the current branch
```

See the README for the overview, `core/README.md` for the firmware build, and
`PLAN.md` for the roadmap.
