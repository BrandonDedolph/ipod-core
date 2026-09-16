# Review r1 — feat/volume-eq (07-18 Volume Limit + EQ presets)

Worktree: `/home/brando/Projects/ipod_theme/.claude/worktrees/volume-eq`, HEAD `2b920c3`, 7 commits
over main. Plan: `plans/07-18-volume-limit-eq.md`. Reviewed 2026-09-16.

## What I ran (independently, from the worktree)

| command | result |
|---|---|
| `make sim && meson test -C build-sim` | 59/59 OK — includes the new `eq`, and `config`, `hw-volume`, `settings`, `cfg-commit`, `lcd-present`, `hw-audio-*` |
| `make hw && make verify-hw` | exit 0 / exit 0, zero `warning:` lines under `-Werror` |
| `cd core/cli && go test -count=1 ./internal/devicefs/` (and `./...`) | ok |
| `python3 tools/make_config.py --emit` → `b[:60].hex()`, `b[1020:].hex()` | `…64000000` / `033929d4`, bytes 60..1019 all zero — **byte-identical to `configGoldenHead`/`configGoldenCRC` in `config_test.go:40-41`** |
| independent script: eq.c gains+cutoffs → 9-bit words + pre-cut | **all 17 preset rows equal the plan's table**; Off = `0x02C 0x00C 0x00C 0x00C 0x02C`, pre-cut 0 |

The meson `corecfg-fixture` target regenerates the Python blob and `config_test.c:1419-1430` asserts
`blob[6] == 48` and the two new defaults against `settings_defaults()`; the Go want-list test greps
`s->volume_limit = 100` / `s->eq = EQ_OFF` out of settings.c. Three encoders, one commit (5c08683),
as the plan required.

## The points I was asked to look at

**WM8758B encoding** (`core/hal/hw/volume.c:196-199, 213-279`; `wm8758.h:158-176`). Gain code is
`(12 − dB) & 0x1F`: `0x0C` = 0 dB, `0x06` = +6, `0x12` = −6 — lower code = boost, same convention
the shipped `hal_tone_set` used (checked against main's `eq_gain_code`, unchanged). EQ1 bit 8 is
only ever `EQ_DAC_MODE`, set iff any band ≠ 0; `EQ_BW_NARROW` is applied only for `0 < b < 4`, and
`hal_eq_set` zeroes `narrow[]` on both shelves before caching so a caller cannot smuggle bit 8 onto
EQ1/EQ5 (`volume_trace_test.c` case 10 pins it). `EQ_CUTOFF_MASK` masks the 2-bit centre. Every
preset row's words match the plan (script above); Bass Booster and flat are additionally spelled as
literal bus bytes in the test, and the tone curve `(+3, −2)` still yields `0x129`/`0x02E`.

**DAC pre-cut vs. other DACVOL writers.** `grep -rn DACVOL hal kernel` finds exactly two writers:
`wm8758.c:230-231` (`init_seq_c`, 0xFF/0x1FF) and `volume.c:236-237` (`eq_latch`). `volume_latch()`
writes only the OUT1VOL pair; `hal_volume_set`, `hal_balance_set`, the resume-restore mute
(`hal_volume_set(0)`) and `wm8758_mute` (DACCTRL soft-mute) never touch DACVOL. **Not a blocker: a
volume change cannot undo the pre-cut.**

**Restore order vs. the per-track reset.** `wm8758_init()` (`wm8758.c:251-288`): `WM_RESET` →
rate program → `init_seq_b` → VMID settle → `init_seq_c` (DACVOL back to 0xFF) → `g_restore()` →
`volume_latch(); eq_latch();`. The pre-cut is re-applied *after* the reset's full-scale write, and
the hook is registered on both the cold path (`audio.c:332`) and the resume-from-suspend path
(`audio.c:718`). Case 12 replays Rock after a `hal_volume_set(60)` and checks 9 writes in order.

**Now Playing wheel + overlay maths** (`main.c:6349-6362, 3374-3383`). The wheel path is
`settings_volume_clamp(&g_settings, g_volume + step)`; HAL write / `g_settings.volume` /
`settings_touch()` only when it moved, plate armed regardless. Marker: `mx = bx + 124·limit/100`.
limit 10 → x ∈ [bx+10, bx+14]; limit 99 → x ≤ bx+124 = bx+bw = PX+158 (plate is PX..PX+200); rows
PY+9..PY+11 inside the 32-px plate; limit 100 → no marker. The fill ends at `bx+fw−1` with
`fw = mx − bx` when vol == limit, so apex and fill meet without overlap. `lcd_present_test.c` is
untouched and green. `render.py:926-934` mirrors the three rects; `volume_limit.png` shows the
apex at 60 over a 60 fill (viewed).

**44/48-byte decode** (`config.c:472-488`; `config_test.c:773-904`; `config_test.go:186-216`).
Gated on `len >= 48`; 44 → limit 100 / EQ Off / volume untouched; 47 → tail dropped; byte 0 → 100,
5 → 10 (and volume pulled to 10), 250 → 100; EQ 17 kept, 18/200 → Off; encode clamps both ways; a
flipped tail byte fails CRC. Go's 44-byte test asserts the same. Goldens byte-identical (above).

**Reset Settings** (`main.c:6497-6500`): `settings_defaults` (100 / `EQ_OFF` / volume 70) →
`settings_apply` → `hal_volume_set(70)` + `hal_eq_set(flat)` (DACVOL 0xFF, EQ3DMODE clear) →
`settings_touch`. Defaults pinned by `def-vol-limit` / `def-eq-off`.

**Greyed rows / selection.** Unselected locked rows use `S_MUTED2` (`PAL_MUTED2`), which every
one of the seven themes already uses for status-strip text on `PAL_SURFACE`, so contrast is the
same as existing chrome; selected locked rows use `S_SEL_SUB`, the per-theme "sub text on the
selection bar" colour (dark sub on light bar for the light-bar themes, light sub on ink for the
rest). The bar fill follows the same pair. `wheel_move` walks all six rows, so the selection bar
**can** land on Bass/Treble while locked — intended by the plan ("a readout"); SELECT there does not
toggle `g_set_editing` (`main.c:6449-6453`). `g_set_editing` cannot go stale on a locked row: while
editing, the wheel is captured (`main.c:6412`) so the selection cannot move, and the flag is reset on
Settings entry and on every descend. `sound.png` (viewed) shows Rock with Bass +5 / Treble +4 greyed.

**Guide honesty.** See finding 2.

## Findings

### 1. should-fix — `core/hal/hw/volume.c:213-221` (and `core/docs/hw/05-audio.md:433-437`): the write-order comment describes an order the code does not implement

The comment says "there is never a moment where the boost is live at full scale; **on the way back
down the gains come off first** for the same reason, which is why the DACVOL pair leads
unconditionally." Those two clauses contradict each other, and the code does the second: `eq_latch`
always writes LDACVOL/RDACVOL first. Constructed case: Bass Booster → Off. Write 1-2 put DACVOL back
to 0xFF while EQ1 still holds `0x126` (+6 dB on the DAC path); the boost is live at full scale until
write 3 clears it, ~5 I2C transactions later. The window is sub-millisecond and I would not expect
to hear it, but the comment (and the doc's "never a moment of boost at full scale") claims a
guarantee the code does not give. Correct looks like either (a) order by the sign of the pre-cut
delta — gains first when the new pre-cut is smaller than the cached one, DACVOL first otherwise —
or (b) keep DACVOL-first and say so honestly ("attenuate-before-boost is guaranteed on the way up;
on the way down there is a ≤1 ms window at full scale, accepted"). If (a), case 12's DACVOL-first
expectation still holds because restore follows a reset (cached pre-cut vs. hardware 0 dB). Verified
by reading `eq_latch` and the test.

### 2. should-fix — `docs/USER_GUIDE.md:145-146, 151-152`: "slightly quieter" / "a little quieter" understates a pre-cut of up to 12 dB

The pre-cut equals the curve's largest boost: 7 dB for Loudness, 6 for Bass Booster / Hip-Hop /
Treble Booster, and up to **12 dB** for the Bass/Treble sliders at +12. `CHANGELOG.md:16-17` says it
correctly ("with the DAC turned down by the size of the boost"); the guide should say the same and
give the magnitude, since 7-12 dB is roughly "half as loud", not "slightly". Worth one more clause:
the preset named **Loudness** is now the *quietest* preset overall (its 7 dB pre-cut is the largest
in the table) — a user who picks it expecting more level gets less, and nothing on screen says why.
Verified: `eq_precut_db` over the table (script), `eq_effective_curve` clamp at ±12.

### 3. nit — `core/ui/screen_settings.c:175-178`: "Sliders are double-height, so a slider screen shows fewer rows" is false

Every row is `ROW_H` (24) regardless of kind: `ry = LIST_Y0 + vr * ROW_H`, the bar is drawn at
`ry+17..ry+19` inside that row, and `sel_bar(LIST_Y0, ROW_H, vr)`. The falsehood predates this
branch, but the implementer rewrote the comment and kept the premise. Say "six rows fit in the
eight-row window" and drop the double-height claim.

### 4. nit — `core/hal/hw/wm8758.h:168-172` vs `core/docs/hw/05-audio.md:385-390`: the provenance of the mid-band tables is stated differently

The header says the tables are "transcribed from the WM8758B datasheet"; the doc (correctly, per
the plan) says "from the WM8758B PDF **from memory**". The header should carry the same caveat.
Related, device/datasheet-only: `EQ_BW_NARROW = 0x100` bakes in "0 = wide"; every preset writes bit
8 = 0, so if the polarity is the other way round every preset's three peaks are narrow — a
differently shaped preset, not a fault, as the doc says. I could not verify the polarity from
anything in the repo either, and my own recollection of the register tables of sibling Wolfson
parts is not consistent enough to call it; it belongs on the bench list explicitly (it is
currently folded into "verify the mid rows").

### 5. nit — `core/ui/settings.h:68-69`: "ignored while eq != EQ_OFF"

After 2b920c3 the rule is "while `eq` is a preset this build knows" — an out-of-range id reads as
Off in `eq_effective_curve`, `settings_row_locked` and `config_decode`. Trivial, but the header is
the contract the last commit deliberately tightened.

### 6. nit — history: f5aea9f adds `hal_tone_set` as a wrapper, b5abe73 deletes it

Both messages are good and explain why, and the intermediate state builds and passes, so this is
reviewable as-is; squashing the pair would leave `hal_eq_set` with one clean introduction.

## Acceptance criteria

1. `volume > volume_limit` impossible from either row — met (`vol-stops-at-the-limit`,
   `limit-down-again-drags-volume`, `clamp-bad-limit`; plus decode and `settings_apply` re-assert).
2. 44-byte record → limit 100 / EQ Off / nothing else changed — met (C + Go tests).
3. Off + flat byte-identical to the shipped build — met (case 7, literal bytes).
4. Every preset's words match the plan — met (case 8 for Bass Booster, formula check for all; my
   own script confirmed all 17 rows).
5. Plate paints nothing outside `VOL_PLATE_*` — met by construction (maths above) and the untouched
   `lcd_present` suite.

Deviations from the plan are all sound: `hal_tone_set` removed outright (zero callers; its
regression pin survives as case 11), `settings_row_locked` uses the in-range test (the bug the last
commit fixes is real), `settings_volume_clamp` clamps the limit first (defensive against a stray
struct; covered). Device-only items (EQ2–EQ4 centres, BW polarity, EQ-vs-DACVOL saturation order,
pop on the EQ3DMODE flip, all six bench items) are listed as UNFLASHED in STATUS.md with the plan's
bench list intact.

SCORE: 8/10

Verdict: this is a careful, well-tested branch — the register grammar is right and pinned as bus
bytes, the pre-cut cannot be clobbered by any other DACVOL writer, the restore hook replays it after
the per-track reset, the three encoders and their goldens moved together and actually agree, and
the UI coupling (limit ↔ volume, preset ↔ locked shelves) is stated once and tested from both sides.
It is not a 10 because the `eq_latch` comment and the hardware doc promise a "never at full scale"
ordering the code only delivers in one direction, and the user guide calls a 6–12 dB level drop
"slightly quieter" while leaving the Loudness preset's irony unmentioned; fix those two and the three
small comment nits and I would merge it.
