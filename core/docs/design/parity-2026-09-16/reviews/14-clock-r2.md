# Review: feat/clock (plan 14-clock) — round 2

Worktree `/home/brando/Projects/ipod_theme/.claude/worktrees/clock`, HEAD `a476374`, ten commits on
top of `main 8d64349` (the alphabet/search merge landed underneath). The round-1 fixes are one commit,
`a476374 ui, kernel, docs: round-1 review — the lines that were not true`.

## What I ran (fresh build dirs)

| Command | Result |
|---|---|
| `rm -rf build-sim && make sim && meson test -C build-sim` | **70/70 OK**, no warnings |
| `rm -rf build-hw && make hw && make verify-hw` | rc 0 / rc 0 under `-Werror` |
| `go vet ./... && go test -count=1 ./...` | clean; `-v` confirms `TestStampMatchesMakeConfig`, `TestSyncPrintsTheClock`, `TestSyncStampsTheClock`, `TestSyncDryRunWritesNothing`, `TestStampClock{WritesAndNarrates,SkipsANonVolume,OnAnUnpreparedVolume}`, `TestDoctorReportsTheClock` all **ran and passed** |
| Python cross-check of the new `datetime_utc_from_local` (own harness `scratchpad/ufl.c` + script) | 2 040 cases — 2 000 random (epoch, offset) pairs plus both rails at every representative zone, the underflow case and the two impossible zones — **0 mismatches** against `local − off·60` with the 2001..2099 gate |
| `docs/screens/render.py` in place (fonts + temporary art symlink, restored afterwards) | every still byte-identical except the six that embed `git describe` (boot/loading/bootdetails/two gifs) — same set as round 1, so the rebase onto search's `render.py` changed nothing this branch owns |

## Round-1 items, re-verified against the code

| # | Claim | Verdict |
|---|---|---|
| S1 | titles are model data, both painters read it, all ten pinned | **Fixed.** `ui/settings.c:345` `settings_title()`; `screen_settings.c:319` (`settings_render`) and `:839` (`settime_render`) consume it; `settings_test.c:708-731` asserts every screen plus the fall-through shape. Nit below on the two painters that still carry literals. |
| S2 | Reset restores `utc_off_min`; settings.h "three of the six"; guide | **Fixed.** `main.c:8371-8379` saves/restores `host_epoch`, `host_off_min`, `utc_off_min`; `settings.h:179-185` now counts three; `USER_GUIDE.md` Reset bullet says the on-screen time keeps reading correctly and the two rows reset. Interaction with a pending stamp traced: after a Reset `applied_epoch` is 0 and the stamp is kept, so `doctor` reports *pending* and the next boot re-examines it — rule 3 does not fire, rule 5 judges it (STALE for any stamp ≥ 10 min old, SET otherwise), and `timesync_apply` copies `host_off_min` over the restored `utc_off_min`, which is the same number in every case but a zone change on the host — where the host's is the newer fact. No regression in the session: the display offset never moves. |
| S3 | `battery_refresh(1)` moved before the boot clock block, so the FORCE commit's battery gate "is a real verdict" | **Moved, but the claim is still false — see F1.** |
| S4 | no alarm-bit constant; table "to confirm" with both readings; RTCWAK/EXTONWAK; ADCC2 naming | **Fixed.** `INT1_ALARM` is gone (grepped the tree); `rtc.h:88-108` and `06-power.md` INT1 row record both readings as unsettled; the RTCWAK row says `0x80` is very likely EXTONWAK's high bit; the cross-check paragraph now says "by address and function, not by name" with the `0x2E`/`0x2F` naming spelled out. Matches what I can vouch for. |
| S5 | README table mended | **Fixed.** `design_reference/README.md:32-40` — the two rows are back inside the table, the note is below it. |
| N1/N2 | `clock_resync(why, trust_timer)`; wake does not fold the park delta; bench says a wake drift is the sleep | **Half fixed — see F2.** The fold is gone at wake (`main.c:1282` requires `trust_timer`; wake passes 0 at `:6670`, cadence passes 1 at `:7223`). The bench text in `STATUS.md` about the wake drift is right. The "retry a minute later" sentence is not. |
| N3 | comment on the minute-edge paint | **Fixed.** `main.c:7205-7213` now says full paint and why. |
| N5 | `datetime_utc_from_local()` with both rails tested | **Fixed.** `datetime.c:170-192`; `datetime_test.c` covers both rails, the identity, the round trip through +330/−480, impossible zones and underflow; the editor commit at `main.c:8198-8208` routes through it and takes the `-3` path on refusal. My Python check agrees on 2 040 cases. |
| N4 | sync/eject tests; `Plan.Clock` inversion fixed | **Fixed.** `cli/sync_test.go TestSyncPrintsTheClock` asserts the dry-run plan line, no report line on a dry run, the report line's shape and time on a real run, and the pending stamp on disk; `eject/clock_test.go` covers the wrapper the app's button calls. `Plan.Clock` semantics: `syncer.go:452` sets it `true` unconditionally, `sync.go:202` is its only consumer (the plan line), `Execute` returns at `o.DryRun` (`execute.go:90`) before the stamp, and `TestSyncStampsTheClock` asserts `Clock == true` *and* `ClockStamped.IsZero()` on a dry run. Consistent: the plan describes a real run, the dry run writes nothing. The field can now never be false, which makes it documentation in JSON rather than a decision; fine. |
| N6 | declined as pre-existing | Accepted; the branch does not make it worse. |

**Rebase of `menu_render_list`:** `git diff main HEAD -- core/kernel/main.c` shows the branch's only
delta to that function is the `right` parameter (`right ? right : ""` into `ui_header`); the
`ui_scroll_window` / `ui_scrollbar` body came from main with search's nine-row Music menu. Both
callers updated (`"Core"` with the clock buffer, `"Music"` with `NULL`). Correct.

**Suite count:** README and core/README say 70; `meson test` says 70.

## Findings

No blockers. Two should-fix, both cases of a sentence the code does not make true — the class this
round was supposed to close.

**F1 — should-fix — `main.c:6830-6842` + `:6916-6918`: the boot FORCE commit's battery gate is still
never a "real verdict".** `battery_policy_feed()` (`hal/hw/battery.c:393-405`) returns
`BATTERY_EVENT_NONE` and leaves `bat_level` untouched until the median ring holds
`BATTERY_FILTER_N = 5` samples (`battery.h:136`, `battery_filter_ready()` at `:368`), and its own
comment says the window is not armed for "the first 20 s after boot". One `battery_refresh(1)` puts
one sample in the ring, so at the clock block `bat_level` is still the static `BATTERY_LEVEL_OK` and
`battery_disk_writes_allowed()` still answers from the default. The new comment at `:6916` ("a real
verdict here, since the sample above precedes this") and the commit message ("so the sentence is true
instead of decorative") are therefore both wrong, for exactly the reason round 1 flagged. The NACK
path is unchanged (`policy_feed(-1)` touches nothing; "batt read failed" is logged), and a spin-up-
sagged first sample cannot move the level either, so moving the sample is harmless and primes the
gauge earlier — but it does not buy the protection it is now described as buying. Correct looks like
one of: (a) state plainly that this write is pre-policy by design — the platters are already up for
the mount, and the DISKSAFE line cannot be judged until the 5-sample window fills ~20 s in — and
drop the "real verdict" clause; or (b) if a genuine gate is wanted, that is a policy change (a
partial-window rule) that belongs in `battery.c`, not here. (a) is the honest one-line fix. Verified
by reading the policy feed and the readiness predicate.

**F2 — should-fix — `main.c:1297` vs `STATUS.md:333-338`: a wake whose read gets no answer retries in
30 minutes, not "a minute later".** With `trust_timer == 0` the early-return branch at `:1282` is
skipped, `wallclock_resync(&g_wclock, rc == 1 /* 0 */, …)` resets the clock, and the common path
then sets `g_clock_retry = 0` unconditionally, so `due` at `:7221` is `RTC_RESYNC_S` (1800 s). The
same applies to a boot whose `rtc_read_raw` returns −1 (the boot block never sets `g_clock_retry`).
`STATUS.md` bench step 5 says "the clock went unknown until the retry a minute later, by design". The
cheap, better fix is `g_clock_retry = (rc < 0) ? 1 : 0;` on the common path (and the same in the boot
block), which makes the sentence true and gives a transient bus miss the one-minute retry it gets on
the cadence; the alternative is to correct the sentence to "half an hour". Verified by reading
`clock_resync` in full and every write to `g_clock_retry` (three sites).

### Nits

**N7 — `screen_settings.c:413` and `:663`** still pass literal `"About"` and `"Boot Details"` to
`ui_header` while `settings_title()` answers the same strings for `SETTINGS_ABOUT`/`SETTINGS_DIAG` and
the test pins them. The model is asserted but not consumed for those two screens, which is a smaller
version of the drift S1 fixed. Either route them through `settings_title()` or say in the
`settings_title` comment that the two dashboard painters own their headers.

**N8 — commit shape.** The round-1 fixes are one commit spanning HAL doc, kernel, UI, Go, docs and
tests. The body is thorough and each item is traceable, so it is acceptable; F1's claim is in that
body and will want a correcting line when it is fixed.

## Acceptance criteria (unchanged from round 1 where not touched)

1 ✓ (70 suites, hw clean, go clean). 2 ✓. 3 ✓. 4 ✓. 5 ✓ (plus the new inverse, cross-checked).
6 ✓ (and the zone rails are now refused rather than wrapped). 7 ✓ (re-rendered after the rebase).
8 ✓. 9 DEVICE — correctly deferred; the bench text is right about the wake drift and wrong about the
retry interval (F2).

SCORE: 8/10

Verdict: the round-1 list was worked through honestly and almost all of it is genuinely closed — the
title lives in the model and is pinned for every screen, Reset no longer strands the display on UTC,
the alarm bit no longer ships as a number, the README table renders, the wake path no longer folds the
park delta, the zone rails are refused instead of wrapped, and the new sync/eject tests found and fixed
a real inversion in `Plan.Clock`. The rebase onto search is clean, the stills are unchanged, and every
suite, the ARM image, vet, tests and the Python parity test pass on a fresh build. What keeps it off
9 is that two of the round-1 fixes assert something the code does not do: the boot commit's battery
gate is still on the policy's default because the median filter cannot arm on one sample, and a wake
that gets no answer waits thirty minutes, not the "minute later" the bench text promises. Both are
one-line fixes, and both are exactly the kind of untrue sentence this round set out to remove.
