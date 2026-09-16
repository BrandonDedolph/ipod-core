# Review: feat/headphone (plan 11 — pause on headphone unplug), round 2

Worktree: `/home/brando/Projects/ipod_theme/.claude/worktrees/headphone`
HEAD 0043b94 (rebased; three commits: 49040f9, bfe979b, 0043b94). Reviewed as the tree diff
`d79692d..0043b94` (r1 HEAD to r2 HEAD, 11 files, +175/−132) on top of the round-1 reading.

## What I ran (independently, from `<worktree>/core`)

| Check | Result |
|---|---|
| `make sim && meson test -C build-sim`, header default (TRUSTED 0) | 59/59 OK (`jackwatch`, all three `hw-headphone-*` included) |
| `make hw && make verify-hw`, TRUSTED 0 | clean under gcc-16 `-Werror`; size budget, layout, version marker, `check_hw_consistency`, parity checks all OK |
| Same two with `HEADPHONE_DETECT_TRUSTED 1` in `hal/hw/headphone.h` | 59/59 OK; hw clean; header restored, `git status` clean afterwards |
| Eight mutants of the round-2 `ui/jackwatch.c` (prime from −1; drop the `playing` guard; IN→PAUSE; IN→NONE; OUT→NONE; OUT→PAUSE; `prime` acts; first raw sample counts) | baseline exit 0; every mutant exit 1 |

## Round-1 findings, re-verified against the code

| r1 | Status |
|---|---|
| 1 "transcript"/"three knobs" in `headphone.h:16-20,54`, `tests/meson.build:698`, trace-test header | Fixed: "until the pin has been READ on hardware … sets the two knobs below (TRUSTED, and ACTIVE_LOW if …)". No `transcript` left in `hal/hw/`, `tests/`, or the hw doc. Read it. |
| 2 gallery README About caption | Fixed (`docs/screens/README.md:109-111`, `JACK 1 n0` with the untrusted rationale). |
| 3 `paused_by` unconsumed | Removed from struct, reset, feed, prime, tests. `grep paused_by` over `core/` is empty. |
| 4 second edge detector in `main.c` | Fixed: `jackwatch_feed` returns `IN`/`OUT`/`PAUSE`/`NONE`; `main.c:5877-5892` is a `switch` that only prints. No `was`/`last` compare remains. |
| 5 unreachable `headphone_raw()` fallback | Deleted; `.raw = (int8_t)headphone_raw()` unconditionally, comment says "three reads, paid on this page alone" (`main.c:3283-3292`). Accurate. |
| 6 `main.c` net size | +120/−30 → now roughly the same; comments trimmed. Still above the plan's ~40 but with no policy in it. Not re-raised. |
| 7 "pulled and re-inserted during sleep stays paused" was false | **Made true** (below), and the doc, guide, STATUS 14b and the test now describe the implemented behaviour. |

## The new suspend-loop feed (`kernel/main.c:5415-5433`), against each question asked

- **What runs inside the park.** The added code per 100 ms pass is `mmio_read32(USEC_TIMER)`, `hal_headphones_present()` (TRUSTED 1: one USEC_TIMER read + one GPIOA read + the pure debouncer; TRUSTED 0: `return -1`, nothing), and `jackwatch_feed` (pure integers). No `uart_puts`, no `player_*`, no ATA, no boost. Read `hal/hw/headphone.c:122-141` and `ui/jackwatch.c` in full; nothing in either reaches a peripheral other than those two registers.
- **SER0 clock gating.** Nothing prints from inside the loop. The one new line, `core: jack out during suspend, staying paused`, is emitted at `main.c:5496-5500`, after `suspend_lowpower_leave()` (`:5462`, which is where `clock_gate_resume()` lives under `SUSPEND_GATE_CLOCKS`) and after the existing `core: suspend: wake` line on the same UART. Correct under either value of the gate switch (default 0). The `jack_pulled` flag is a plain local; it survives the `break` paths.
- **−1 inside the loop (the shipping image).** `jackwatch_feed(…, −1, …)` returns `NONE` before touching state (`jackwatch.c:26-31`), so `was_playing` and `jack_pulled` never move; at wake `hp = −1`, `jackwatch_prime` is a no-op, `hp != 0` resumes. Identical to `main` today. Verified by reading; the `hw-headphone-untrusted` binary still pins zero bus events for those calls.
- **`playing = was_playing`.** Right predicate: the `player_pause()` on the way down (`:5212-5214`) was the sleep's, so `player_active() && !player_paused()` would be 0 here and no pull could ever register. With `was_playing`: pull while it had been playing → `PAUSE` → `was_playing = 0`, `jack_pulled = 1`; re-insert later in the same sleep → `IN` → nothing; wake with `hp = 1` → `was_playing` is 0 → no resume. That is the plan's matrix row 9 and STATUS 14b, now actually implemented. Pull while it had been paused → `OUT` → nothing changes → wakes paused as before. Constructed both from the code.
- **30-minute standby escalation (`:5441-5448`).** The feed precedes it and now shares its `now` sample (`(uint32_t)(now - suspend_t0) > SUSPEND_TO_STANDBY_US`); previously the escalation took its own timer read a few microseconds later. Semantically identical. `enter_standby()` normally does not return, so the jack state dies with the power and is reset at the next cold boot. On refusal: `was_playing = 0; break;` → wake path → prime → no resume; `jack_pulled` may already be 1 and then the wake line is printed, which is true. Fine.
- **Refused-standby / battery-verdict paths.** (a) Hold-escalation refused before the idle loop (`:5284-5288`): `standby_refused = 1` so the loop never runs; no feed; wake prime is the only update. (b) `battery_refresh` sets `g_standby_refused` (`:5398-5402`): `break` before that pass's feed. (c) 30-min refusal: `break` after the feed. In all three `was_playing` is already 0, so nothing resumes regardless of the jack. Correct.
- **Is the wake prime still needed?** It is now a backstop only, and its effect is cosmetic: on any path, a level that moved between the last feed and the first main-loop pass would produce `OUT` or `IN` on that pass — a log line, never a `PAUSE`, because (i) if the wake resumed then `hp` was 1 and `last` was already 1, and (ii) if it did not resume then `playing` is 0. The comment (`:5488-5493`) and the hw doc ("stops a level that moved on one of those paths from reading as a fresh edge on the first loop pass back") say exactly that and no more. The read itself is needed anyway for the resume gate. Keep it; not a finding.
- **Nothing wakes or spins the drive.** Confirmed above: the new code touches USEC_TIMER and GPIOA only. The drive-touching things in this loop (`battery_refresh` DISKSAFE write) are pre-existing and unchanged.
- **Debounce cadence.** `SUSPEND_IDLE_MS` is `100u` (`:5115`) against `HEADPHONE_DEBOUNCE_US` 200 000 — two samples per window, as the hw doc says. A pull in the final <200 ms before the wake press may still read 1 at the wake sample: the wake resumes, and the main loop's feed pauses it ~100-200 ms later (`PAUSE`, with `core: jack out, pause`). Correct outcome, one short blip, inherent to any debounce; not a defect.
- **PLAY-release wait (`:5279-5293`).** No feed runs there, but `g_jack.last` persists from the main loop's last pass (the jack block at `:5865` runs before the input switch that calls `suspend_to_ram` at `:5945`), so a pull during that window is seen on the idle loop's first feed. Correct.

## Docs vs code, round 2

- hw doc "Suspend and wake" (`core/docs/hw/10-headphone-jack.md:367-383`), the policy table with `PAUSE`/`OUT`/`IN`, STATUS 14/14b including the exact log string, the guide's Sleep bullet ("even if you plug them back in before waking it"): all match the code as it now is.
- `docs/screens/README.md` caption matches `about.png`.

## Findings

1. **nit — `core/ui/jackwatch.h:26,32` — the header's own "WHAT IT DECIDES" table still describes the round-1 return values.** "plug back in -> NOTHING" and "pull while paused -> nothing to pause" sit thirty lines above an enum whose comment says those cases return `IN` and `OUT`. The intent ("never an instruction") is still true, but a reader taking NOTHING as `JACKWATCH_NONE` is misled by the file that defines it. Correct looks like "-> IN, a notification, never a resume" / "-> OUT, a notification; nothing to pause". Read it.
2. **nit — `core/tests/meson.build:205` — "a pull while paused is silent"** is contradicted by the test it describes, whose assertion is now `== JACKWATCH_OUT` and whose label is "OUT, which is a notification, not a pause". One word. Read it.
3. **nit — `core/tests/ui/jackwatch_test.c:170-171` — "the pull was already counted, so the wake still declines"** attributes the wake's decision to `pauses`, which nothing in `main.c` reads (the header itself says so at `jackwatch.h:89-91`). The wake declines because the suspend loop dropped `was_playing` on the `PAUSE`. The assertion is right; the sentence names the wrong mechanism.

No blockers, no should-fix. No correctness issue in any case I could construct.

---

# Round 3 — HEAD 998f56a ("ui: the jackwatch table names IN and OUT for what they are")

One commit on top of 0043b94, three files, comments and one test label only (`git diff 0043b94 HEAD`
read in full: `core/ui/jackwatch.h:26-32`, `core/tests/meson.build:205`,
`core/tests/ui/jackwatch_test.c:172-174`). Worktree clean.

**Re-ran independently:** `make sim && meson test -C build-sim` 59/59 OK; `make hw` clean under
`-Werror`, size budget OK; `make verify-hw` every check OK. (The TRUSTED 1 configuration was proven
at 0043b94 and this commit changes no compiled code — no `#if`, no source line — so I did not repeat it.)

**Each r2 nit against the hunk:**

1. `jackwatch.h` table now reads "plug back in -> IN, an observation only: nothing resumes" and
   "pull while paused -> OUT, an observation only: nothing to pause". Matches `jackwatch.c:48-58`
   (`return JACKWATCH_OUT` under `!playing`, `return JACKWATCH_IN` on the 0->1 edge) and the enum
   comment at `jackwatch.h:58-68`. Resolved.
2. `tests/meson.build:205` now says "a pull while paused is only an OUT; re-inserting is only an IN,
   NEVER a resume". Matches the two assertions it describes (`jackwatch_test.c:126-127`, `:139-140`).
   Resolved. The line is 94 columns beside 80-column neighbours; the repo enforces no line length
   (no `.editorconfig`/`.clang-format`, nothing in `verify-hw`) and three other lines in that file
   already exceed 90, so this is not a finding.
3. Test label now reads "the PAUSE already dropped main.c's was_playing, so the wake still declines",
   which is the mechanism at `kernel/main.c:5429-5433`. Resolved.

Commit message is imperative, explains why, carries the trailer. History is four commits, each a
coherent step (feature, docs, review-round behaviour fix, review-round wording), no fixup noise.

No findings remain. Every acceptance criterion in the plan is met or explicitly deferred to the
bench as UNFLASHED (criterion 3's line count is the only literal miss, flagged in r1 and accepted:
the excess is comments and a narration helper, with no policy in `main.c`).

SCORE: 10/10

Verdict: mergeable as-is. The policy lives in one tested module whose every return value is pinned
and whose mutants all die; the suspend loop feeds that same module with only two register reads and
prints nothing inside the park; the shipping image is provably inert on the debounced path; the
About token, the log lines, the guide, the hw doc, STATUS and the gallery all describe the code that
is actually there; both header states build and pass; and the last three stale sentences are gone.
What is left is owed to the device and is labelled that way.
