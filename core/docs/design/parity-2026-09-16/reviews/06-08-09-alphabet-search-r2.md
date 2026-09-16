# Review r2 — feat/alphabet-search (round 2: commit C amended to 515282d)

Worktree: `/home/brando/Projects/ipod_theme/.claude/worktrees/alphabet-search`, still rebased onto
main 5fac8e4. d4b3bcc (A) and 3f39638 (B) are byte-identical to r1 (same SHAs); only C changed
(2cb2ed1 → 515282d). Delta: `core/kernel/main.c` (+59/−24 lines), `core/tests/ui/gesture_test.c`
(+52), `STATUS.md`, `docs/USER_GUIDE.md`. Worktree clean.

## What I ran

| Command | Result |
|---|---|
| `make sim && meson test -C build-sim` | 66/66 OK; the five new `gesture drain:` cases run and pass (testlog) |
| `make hw && make verify-hw` | clean; text 382,496 / data 428 / bss 12,216,384 |
| negative proof of the new test | copied `gesture_test.c` + real `gesture.c`/`keyhold.c` to scratch, replaced the two `seekhold_void(&v.s)` calls with `(void)0`, compiled with `cc`: 3 of the 5 drain cases FAIL, the "WITHOUT the void ... skips" case still passes — the suite catches the regression |
| `make hw` of `git archive main` | text 374,716 / data 388 / bss 12,214,264 (stamp embedded as `v0.0.0\|unknown`) |

## r1 findings, re-verified against the code

**B1 (blocker) — FIXED.** `kernel/main.c:7348-7382`: the structure is now
`} else if (ev.buttons & WHEEL_BTN_RIGHT) { /*comment*/ seekhold_void(&g_ff); if (player_active() && !(SCR_SEARCH && PICK)) { ...push NP... } }`.
The void is the first statement of the non-player-screen RIGHT branch, before the guard, so it runs
on every screen that is not Now Playing/Queue — Search PICK included (the press is still in
`ev.buttons` for the `SCR_SEARCH` handler, as `seekhold_void` (`ui/gesture.c:53-60`) touches only
the machine). The four other `seekhold_void` sites (modal dismiss 7272/7273, backlight wake
7308/7309) are unchanged. The rewritten comment describes the feed-at-top / drain-below ordering
accurately (feed at 7123 samples `clickwheel_buttons()`, the drain reads the tick-latched event).

**The new `gesture_test` block** (`tests/ui/gesture_test.c:445-495`) drives the real machine: the
suite links `../ui/gesture.c` + `../ui/keyhold.c` (`tests/meson.build:254`), `step()` is a thin
wrapper over `seekhold_feed()` with `PASS_US` of simulated time, and the "voided" and "not voided"
rigs are identical except for the `seekhold_void()` call. The negative case ("WITHOUT the void that
same press skips") is the real `seekhold_feed` returning `SEEKHOLD_SKIP` on release after a
down-edge seen with `allowed = 1` — no re-implementation. What it cannot model is main.c's drain
itself (the `allowed` flip stands in for the Now Playing push); that is the same limitation every
gesture test here has and the comment says so. Removing the void makes three assertions fail, so a
future deletion of the call would not go unnoticed — provided someone deletes it from the test rig
too, which is the residual gap of any model-based test; acceptable.

**S1 — FIXED.** `main.c:3235`: `if (n >= 0) g_playlists_scanned = 1;`. A failed folder read no
longer latches; Search's entry (7482) re-reads on the next visit. Comment explains why.

**N1 — FIXED.** `main.c:7687-7695`: the from-search MENU branch sets `g_dir_depth = 0` before
`scr_pop()`, with a comment tying it to `scr_pop_to_root`'s rule.

**N2 — FIXED.** The stray `g_br_from_search = 0` on `search_play_hit`'s playlist error path is gone.

**N3 — FIXED.** `main.c:8617`: `search_reset(&g_search)` in `kernel_main`, with a comment.

**N4 — mostly fixed; one figure remains method-dependent.** STATUS now reports the `.data` +40 B
and bss +2,120 B — both exact against every baseline I have. The text figure "7,792 B against main"
is what you get comparing a *stamped* branch build to an *unstamped* `git archive` build
(`CORE-FW-VERSION:v0.0.0|unknown` vs `v0.1.3-43-g515282d` — different string lengths); against the
in-tree main build (same stamp shape) it is +7,756 B, and against my own archive build +7,780 B.
None of these is wrong, but the STATUS sentence says "against main" without saying how, and the
number is not reproducible by the obvious method. Nit: quote the in-tree delta (+7,756) or add
"(git-archive baseline; stamp excluded)". Not a correctness matter.

**N5 — FIXED.** `docs/USER_GUIDE.md:42-44` reflowed.

**N6** (hero.gif/boot stills drift with the stamp) — informational, unchanged, fine.

**Docs/commit text.** STATUS's Stage C entry and 515282d's commit message now state the void is
unconditional and why; I found no remaining sentence in STATUS, CHANGELOG, the guide or the three
commit messages that contradicts the code. The r1 analysis of the ten attention points stands
unchanged for A and B (identical commits) and for the C code paths not touched here.

## Residual nits (new or carried)

- `STATUS.md` text delta figure (N4 above).
- Cosmetic: `search_reset(&g_search)` sits between `wheel_set_letter_step` and `wheel_set_click`,
  splitting the four wheel seams the block comment above calls "all three/four"; moving it one line
  down keeps the seams together. Purely tidiness.

## Verdict

SCORE: 9/10

The blocker is gone and gone properly: the void is back on every non-player-screen RIGHT path
including the Search picker, the comment now explains the feed/drain race instead of asserting a
call that was not there, and the new test exercises the real `seekhold_feed`/`seekhold_void` in
both directions — I proved it fails without the call. The should-fix and all four code nits are
addressed exactly as described, the suites (66) and the ARM gate are green, and A/B are untouched.
What keeps it off 10 is one doc number: the STATUS text delta is measured against an unstamped
archive build and is not reproducible against main as checked out; the honest figure is +7,756 B.
Mergeable as-is; that figure can be corrected in the merge commit or the next STATUS touch.

## r3 — docs-only commit c26d036 on top of 515282d

Read the hunk (`STATUS.md:80-82`): the text delta is now "about 7.8 KB of text (7,756–7,792 B
depending on the build-stamp string in the baseline)", which brackets every figure I measured
(+7,756 in-tree, +7,780 / +7,792 archive baselines) and says why they differ. The commit touches
only `STATUS.md` (3+/2−), the worktree is clean, and the commit message explains the why. No code
changed since r2, so the 66/66 host suites and the clean `make hw && make verify-hw` stand. The
last r2 nit (where `search_reset` sits among the wheel seams) is tidiness with no false comment
behind it, not a defect. Nothing left that I would ask to change before merging.

SCORE: 10/10

The branch meets every acceptance criterion in the plan or defers it explicitly as device-only
with a bench list, the one real regression found in r1 is fixed and guarded by a test that fails
without the fix, every doc line I checked matches the code, and the history is four clean commits
that each build and explain their why.
