# Review r2 — feat/volume-eq (07-18 Volume Limit + EQ presets)

Worktree: `/home/brando/Projects/ipod_theme/.claude/worktrees/volume-eq`, HEAD `d8e1399`, rebased onto
main `9c39b54` (sleep timer + headphone merges), four commits. Reviewed 2026-09-16 against r1
(`07-18-volume-eq-r1.md`) and the plan.

## What I ran (independently, clean build dirs)

| command | result |
|---|---|
| `rm -rf build-sim build-hw; make sim && meson test -C build-sim` | **61/61 OK** — `eq`, `settings`, `config`, `hw-volume`, `sleeptimer`, `lcd-present`, `hw-audio-*` all in |
| `make hw && make verify-hw` | exit 0 / exit 0, zero `warning:` lines |
| `cd core/cli && go test -count=1 ./...` | ok (devicefs 0.263 s, uncached) |
| `python3 tools/make_config.py --emit` → head/CRC | `…64000000` / `033929d4`, bytes 60..1019 zero — byte-identical to `config_test.go:40-41` |
| script: guide's per-preset dB vs `eq.c` `max(0, max gain)` | 17/17 match; quietest = Loudness, 7 dB |

## r1 findings, re-verified against the code

**r1-1 (write order) — fixed, and correctly.** `volume.c:230-296`: `eq_latch()` computes the new
pre-cut, then `if (precut >= g_dac_precut) { dacvol_write; eq_bands_write; } else { eq_bands_write;
dacvol_write; }` and sets `g_dac_precut = precut`. `hal_codec_restore()` (`volume.c:334-342`) does
`volume_latch(); g_dac_precut = 0; eq_latch();`. The comment, `volume.h:60-66`, `05-audio.md`
"Write order follows the sign of the pre-cut change" table and the commit body all now describe
what the code does. `test_eq_write_order_both_ways` (`volume_trace_test.c:488-547`) pins, as literal
bus bytes: up (flat → Bass Booster) `0x16 F3, 0x19 F3, 0x25 26 …`; down (Bass Booster → flat)
`0x24 2C … 0x2C 2C, 0x16 FF, 0x19 FF`, exactly seven writes; equal (Rock → Dance, both 5 dB)
DACVOL `0xF5` leads. `check_eq_burst` now takes `prev_precut` and derives the order independently.

**State model of `g_dac_precut`** (what the coordinator asked for), traced:

| moment | codec DACVOL | `g_dac_precut` on entry | cached curve pre-cut | branch taken | correct? |
|---|---|---|---|---|---|
| cold init: `wm8758_init` → `init_seq_c` (0xFF) → hook | 0 dB | 0 (static init) | 0 (flat) | `>=` → DACVOL then bands | yes (byte-identical to shipped) |
| boot `settings_apply` → `hal_eq_set(user curve, P)` | 0 dB | 0 | P | `>=` → cut then boost | yes |
| per-track: `WM_RESET` … `init_seq_c` (0xFF) → restore | 0 dB | zeroed to 0 | P | `>=` → cut then boost | yes |
| Reset Settings: `hal_eq_set(flat)` from preset P>0 | −P | P | 0 | `<` → unboost then DACVOL 0xFF | yes |
| preset change while paused | see below | P_old | P_new | by sign | yes |

*Preset change while paused.* A suspended pause (`player.c:1665 hal_audio_suspend` →
`codec_power_off` → `wm8758_powerdown`) runs `powerdown_seq`, which writes DACCTRL soft-mute, the
OUT1 mutes, OUT4TOADC and the three PWRMGMT registers — **no `WM_RESET`, no DACVOL**, so the EQ/DACVOL
registers keep their values (or, if the digital core is unpowered, are irrelevant) and the next
`hal_audio_wake` (`audio.c:716-722`) runs `wm8758_set_restore` + `wm8758_init` → `WM_RESET` → restore,
which re-syncs from RAM. A non-suspended pause leaves the codec up and `hal_eq_set` writes live, ordered
by sign as usual. `WM_RESET` appears only in `init_seq_a`, `wm8758_init` always calls the hook, and both
callers (`audio.c:332, 718`) register it before calling — so there is no path on which the codec loses
its registers without the restore re-basing `g_dac_precut`.

*Can the "equal" case ever write in the wrong order after a restore?* No — and stronger than the
implementer states: at every entry to `hal_codec_restore`, `g_dac_precut` **already equals** the cached
curve's pre-cut (invariant: `eq_latch` is the only writer of both `g_dac_precut` and the last-latched
curve, and `hal_eq_set` calls it after every cache update). So the restore is always the equal case,
and equal takes DACVOL-first, which is right for a codec that has just been reset to 0xFF. See nit 1.

*Does the new test start from the state it claims?* Yes. `eq_reset_to_flat()` (`:239-247`) calls
`hal_eq_set(flat)` — which drives `g_dac_precut` to 0 whatever the previous test left — then clears
the bus; "going down" runs directly after the boost call (`g_dac_precut` 6); "equal" runs after Rock
(5); the restore case after Rock with volume 60. Note the mock bus records writes and does not model
codec registers, so "what the codec holds" is the driver's own belief; that is what the invariant
above and the reset analysis cover. I confirmed the implementer's mutation claim by reading: forcing
DACVOL-first unconditionally would fail the four literal "order down" checks plus `check_eq_burst`'s
seven positional checks for that case (11–12 checks).

**r1-2 (guide honesty) — fixed.** `USER_GUIDE.md:158-165` now gives the real pre-cut per preset
(verified against `eq.c` by script, 17/17), says Loudness is the quietest setting despite its name,
and puts the Bass/Treble cost at 6 dB / 12 dB "roughly half as loud". Matches the code.

**r1-3** (`screen_settings.c:175-178`) — fixed; the comment now says every row is `ROW_H` and the bar
sits at `ry+17`, which is what the code does.
**r1-4** — fixed; `wm8758.h:165-175` says "UNVERIFIED — FROM DATASHEET MEMORY", states the assumed
polarity, and what being wrong costs; STATUS bench item (g) is the datasheet check, with the correct
note that flipping `EQ_BW_NARROW` alone would move every preset from wide to narrow peaks.
**r1-5** (`settings.h:76`) — fixed: "ignored while eq names a preset this build knows".
**r1-6** — fixed: four commits, `hal_tone_set` is removed in the same commit as its last caller
(`4f088c4`); all four bodies explain why, including the order-by-sign rule and its reason.

## Rebase resolution (main 9c39b54: sleep timer, headphone)

- `config.c`: sound tail still `P_VOL_LIMIT 44 / P_EQ 45 / P_SND_PAD 46 / CFG_PAYLOAD_V2S 48`;
  main's `s->sleep_timer_min = 0` in `config_decode` (`:496-500`) is retained *after* the new
  `volume <= volume_limit` clamp; the encode-side "deliberately ABSENT" note (`:375`) is intact.
  `sleep_timer_min` appears nowhere in `make_config.py` or `config.go`, so 48..63 stay unclaimed.
- `settings.c`: `PLAY_L[4]` with "Sleep Timer", `settings_count(PLAYBACK) == 4`, alongside `SOUND_L[6]`.
- `settings_test.c`: main's `count-play == 4` and the `sleep-*` checks (`:228, :287-323`) coexist
  with the Sound tests; the `memcmp` "eq-cycle-touches-only-eq" check still passes with the extra
  field in the struct.
- `settings.h`: `sleep_timer_min` runtime-only comment kept; `volume_limit`/`eq` documented as before.
- Go/Python goldens unchanged from r1 and still identical; the `config_test.go` comment cites tree
  `e891cd4`, which is the pre-rebase branch tip the bytes were generated from — the bytes are the
  same on `d8e1399` (I regenerated them), so the citation is stale-but-true. Not worth a finding.

## Findings

### 1. nit — `core/hal/hw/volume.c:337-340`: the `g_dac_precut = 0` in `hal_codec_restore` is unobservable, and the comment implies it is load-bearing

By the invariant above, `g_dac_precut == precut(cached curve)` on every entry to
`hal_codec_restore`, so `eq_latch` takes the `>=` branch (DACVOL first) with or without the zeroing;
`test_codec_restore_replays_eq` passes if the line is deleted. The comment — "say so, and eq_latch
will re-cut BEFORE it re-boosts rather than after" — suggests that without it the replay would boost
first, which is not so given the equal-case tie-break. The line is still worth keeping (it is what
makes the restore order independent of the tie-break, and it is the truthful statement of what the
codec holds), but the comment should say that: "belt-and-braces: the equal case already leads with
DACVOL, this makes the replay not depend on that tie-break". Verified by reading `eq_latch`,
`hal_eq_set`, `hal_codec_restore` and constructing the invariant; no code change needed.

Nothing else. No blockers, no should-fixes.

## Acceptance criteria (plan)

All five host-provable criteria remain met (see r1); criterion 3's pin (`test_eq_flat_is_the_shipped_tone_path`)
still holds under the new ordering because flat-from-flat is the equal case. The device-only list in
STATUS.md is complete and now carries (g).

SCORE: 9/10

Verdict: round 2 resolves everything from r1 properly rather than cosmetically — the write order now
follows the sign of the pre-cut change, the driver tracks what the codec holds and re-bases it at every
reset, both directions and the equal case are pinned as literal bus bytes, the guide tells the truth
about the level cost per preset, the header and STATUS say plainly which register facts came from
memory, the history is four clean commits, and the rebase onto the sleep-timer main is correct in every
shared file with the sound tail still at 44..47. The one remaining item is a comment that overstates
what the restore's zeroing does; fix that line and I would merge it as-is.

---

# r3 — HEAD `9130eb7` (one commit on top of d8e1399)

Commit `9130eb7 hal: the restore pre-cut reset is belt and braces, and says so`. `git diff d8e1399..HEAD --stat`:
`core/hal/hw/volume.c | 7 +++++--` — the only change is the comment above `g_dac_precut = 0;` in
`hal_codec_restore` (`volume.c:351-356`). Read the hunk: it now says the codec holds no pre-cut after
`init_seq_c`, that with the cached curve's pre-cut equal to the believed one `eq_latch`'s tie-break
already writes DACVOL first, that the line therefore changes no write order today, and that it keeps
the driver's belief true if that tie-break ever changes. That is precisely the invariant traced in
r2 (`g_dac_precut == precut(cached curve)` on every entry to the restore), and the commit body says
the same. The r2 nit is closed; no code moved.

Re-run on `9130eb7`: `make sim && meson test -C build-sim` 61/61 OK; `make hw` exit 0,
`make verify-hw` exit 0, zero `warning:` lines. Go and the make_config parity are untouched by a
C-comment commit (verified on d8e1399 in r2, same bytes).

Checked once more against the 10/10 bar in REVIEW_RULES: every host-provable acceptance criterion is
met and tested from both sides; the device-only items are named explicitly as UNFLASHED with a bench
list including the datasheet check; the constructed edge cases (limit 10/99/100, 44/47/48-byte
records, limit byte 0/5/250, EQ byte 17/18/200, boost→flat and flat→boost and equal orders, restore
after reset, preset change while paused, unknown preset not locking the shelves) all behave; I found
no remaining comment, name or doc line that disagrees with the code; the tests would catch a
regression of every behaviour they claim (including the order-by-sign, whose unconditional mutation
fails 11–12 positional checks); and the history is five commits, each with a body that explains why.

No findings.

SCORE: 10/10

Verdict: the branch is mergeable to main as-is. The one thing left from r2 was a comment that
overstated what a defensive line did; it now says exactly what the code does and why the line is
kept, and nothing else changed. Register grammar, pre-cut ordering in both directions, restore
after the per-track reset, the three encoders and their goldens, the limit/volume and
preset/shelf couplings, the guide's numbers and the provenance of the unverified datasheet facts
are all consistent with the code and pinned by tests.
