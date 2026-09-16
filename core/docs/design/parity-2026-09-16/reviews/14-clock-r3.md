# Review: feat/clock (plan 14-clock) — round 3

Worktree `/home/brando/Projects/ipod_theme/.claude/worktrees/clock`, HEAD `4893749`, eleven commits on
top of `main 4c4ce50` (MP3 merge underneath; branch delta vs main is unchanged in shape — 67 files, the
`menu_render_list` delta is still exactly the `right` parameter). The round-2 fixes are one commit,
`4893749 kernel, ui: round-2 review — two sentences the code still did not make true`, touching
`main.c`, `screen_settings.c`, `settings.h`, `settings-persistence.md` and the two READMEs.

## What I ran (fresh build dirs)

| Command | Result |
|---|---|
| `rm -rf build-sim && make sim && meson test -C build-sim` | **74/74 OK**, zero warnings; the branch's six suites present |
| `rm -rf build-hw && make hw && make verify-hw` | rc 0 / rc 0 under `-Werror` |
| `go vet ./... && go test -count=1 ./...` | clean; `-v` confirms `TestStampMatchesMakeConfig` (the Python parity), `TestSyncPrintsTheClock`, `TestSyncStampsTheClock` and the three `TestStampClock*` **ran and passed** |

The datetime/utc-from-local Python cross-checks and the gallery byte-compare from rounds 1–2 are
unaffected by this commit (no change to `datetime.c`, `render.py` or any still) and were not re-run.

## Round-2 items, re-verified against the code

**F1 — pre-policy boot write.** `battery_refresh(1)` is back after `library_ensure` (`main.c:6978`);
nothing samples the cell before the clock block (`:6876` read, `:6920` FORCE). The comment at
`main.c:6921-6943`, the new paragraph in `settings-persistence.md:216-227` and the commit body all say
the same thing, and I checked each clause against the code rather than the prose:
- "the policy cannot move `bat_level` until its median ring holds `BATTERY_FILTER_N` samples — five,
  5 s apart" — `battery.h:136`, `battery.c:393-405` (`battery_filter_ready()` is `bat_ring_n == 5`),
  and the cadence is `BATTERY_SAMPLE_US` in `battery_refresh`. True.
- "sampling here would not change that: five conversions microseconds apart are one spin-up-sagged
  reading with a quorum" — true of a median filter, and it is the reason the round-2 move was
  worthless rather than merely insufficient. Good that it is stated.
- "SHUTOFF cannot have fired yet (nothing has been able to judge the cell)" — true: the policy
  returns `NONE` until the ring is full, and `enter_standby` on the SHUTOFF edge is reached only from
  `battery_refresh`.
- "the platters are already up for the mount" — true at that point; "and the index load" reads as if
  the index had already loaded (it is the next thing to happen). A phrasing nit, not an overclaim
  about protection.
- "It is not a licence for any other early write" — present in the design doc. Good.
No sentence claims a verdict the gate does not give. **Fixed, and worded honestly.**

**F2 — retry from the read's own result on all three paths.** Every write to `g_clock_retry`:
`main.c:1286` (cadence carry path, rc < 0 → 1), `:1303` (common path, `(rc < 0) ? 1 : 0` — this is the
wake path with `trust_timer == 0` and the cadence path when the clock was not valid), `:6896` (boot
block, `(rtc_rc < 0)`). Consumer at `:7242` picks `RTC_RETRY_S` (60 s) vs `RTC_RESYNC_S`. A `-1`
therefore retries in a minute on the cadence, at a wake and after a boot; a `0` ("no time") waits the
cadence on all three. `STATUS.md` bench step 5's "the retry a minute later" is now true. **Fixed on
all three paths.**

**N7 — `settings_title()` consumer set.** All four `ui_header` calls in `screen_settings.c` read it:
`:319` (`settings_render`), `:413` (About), `:664` (Boot Details), `:849` (the editor). No literal
titles remain in the settings painters; `settings.h:346-349` says so; the suite pins all ten strings.
**Complete.**

**Suite count.** READMEs say 74; `meson test` says 74 (main's 70 + this branch's four). Correct.

## Findings

No blockers, no should-fix.

**Nits**
- `main.c:6935` / `settings-persistence.md:224`: "the platters are already up for the mount and the
  index load" — at the clock block the index load has not started yet; "for the mount, and are about
  to be for the index load" is what the order says. Wording only.
- The round-2 fix commit (`4893749`) repeats the F1 argument three times (comment, design doc, commit
  body) nearly verbatim. Defensible — each is read by a different person — but the design doc could
  carry the argument and the comment point at it.

## Acceptance criteria

1 ✓ (74 suites, hw clean, go clean, parity ran). 2–8 ✓ as in rounds 1–2, nothing in this commit
touches them. 9 DEVICE — correctly deferred; the bench text now matches the code on both the wake
drift and the retry interval.

SCORE: 9/10

Verdict: every sentence flagged in rounds 1 and 2 now says what the code does, and this round's fix
took the harder, correct route on F1 — it did not try to manufacture a verdict the filter cannot give,
it put the sample back and wrote down why the one pre-policy write is acceptable and why it is not a
precedent. The retry flag is derived from the read on all three paths, every settings painter reads
the model's title, and the counts are right. Fresh builds of the host suite, the ARM image and the Go
tree are all green with the Python parity test running. What remains is one wording nit and the
device-only bench, which no round can close; I would merge this.

## r4 — the wording commit (HEAD `9837e9e`, on top of `4893749`)

Read the hunk: two lines, comment and doc only (`git diff 4893749 HEAD --stat`: `main.c` 1/1,
`settings-persistence.md` 1/1). `main.c:6932` now reads "already up from the mount (the index load
follows)" and `settings-persistence.md:224` "already up from the mount and stay up for the index load"
— both match the order at `main.c:6876-6978` (read → FORCE → `library_ensure` → `battery_refresh`).
I re-ran `make hw` and `make verify-hw` on the worktree myself: rc 0 / rc 0 (a block-comment edit is
where a stray `*/` would land; it did not). The r3 nit is closed; the "argument stated three times"
remark was never a defect. No stale comment, doc line, dead code or misleading name remains that I can
point at; every acceptance criterion is met or is the device bench, correctly deferred; the eleven
commits plus this one read cleanly.

SCORE: 10/10
