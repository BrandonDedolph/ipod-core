# Review: feat/on-the-go (r2)

Worktree `/home/brando/Projects/ipod_theme/.claude/worktrees/on-the-go`, HEAD **e93e39c** on main b8f627c
(clock merged). Four commits: 8ad6d7e, ebc955b, ccc0af9 (the r1 three, re-based) + e93e39c (the r1 fixes).
`git range-diff` against r1's a26e9ef series: commits 1–3 differ only in rebase resolutions (the
`kernel_sources` list gaining the clock's three files beside `otg_store.c`, and the README/guide/cli-README
sentences merged with the clock's), nothing of this branch's own changed. 54 files, +9449/−218.

## What I ran (from the worktree, at e93e39c)

| command | result |
|---|---|
| `make sim && meson test -C build-sim` | **78/78 OK**, 0 fail |
| `make hw && make verify-hw` | clean under gcc-16 `-Werror`; text 424,976 B (= the "415 KB" STATUS claims), bss 12,382,512 B (11.81 MiB, as claimed); all verify checks OK |
| `go test -count=1 ./...` in `core/cli` | 21 packages ok; `-v`: `TestEncodeOTGSlotMatchesMakeOTG` and `TestEmptyOTGSlotFileMatchesMakeOTG` **PASS** against a fresh `make_otg.py` after the digits-only change |
| `make_otg.py --selftest` | OK; the four new non-digit directive cases (`+0012`, ` 0012`, `0000000G`, `gen=+0000`) all PASS |
| `render.py` then `git status` | `otg.png`, `otg_empty.png`, `otg_added.png`, `playlists.png` re-render **byte-identical**; only the pre-existing version-stamped boot images differ (restored) |
| `otg_slot_test` rebuilt with the writer's `if (!info.present) return OTG_SLOT_EFOREIGN;` stubbed out | **exactly five checks fail** (ERASE refuses / SAVE refuses / not one byte written / playlist byte-identical / plays as a playlist), as the commit message claims; the unmodified build passes |

## The r1 findings, re-verified against the code

**B1 (foreign file gets Delete / would be overwritten) — fixed, at two layers.**
- `otg_slot_of()` (otg_slot.c:368-388) answers 1..5 only when `otg_slot_index(name) > 0` AND `otg_slot_verify` succeeds AND `out->present`; a foreign file, an unreadable one, or a real slot file under another name is 0. `playlist_open` (main.c:3575-3585) now sets `g_pl_slot = 0`, and only when `g_pl_err == 0` asks `otg_slot_of`; `g_pl_damaged` is taken from the same verify. So the Delete row (`playlist_row_count`, `playlist_row_draw`, `row_select_tap`) cannot appear for a foreign or unreadable file.
- The writer refuses on its own: `otg_slot_save` (otg_slot.c:642-656) probes the first 512 B after the size checks and before any formatting, returns `OTG_SLOT_EFOREIGN` (−5) when the directive is absent, −1 when the probe itself fails. `otg_slot_erase` is `otg_slot_save` with no rows, so it inherits it.
- **No bypass:** the only callers of `otg_slot_save` in the tree are `otg_save_to_slot` (main.c:5330, target chosen by `otg_slot_free_index`, which already required `present && count == 0`) and `otg_delete_slot` (main.c:5416, guarded by `g_pl_slot > 0` from `otg_slot_of`); `otg_slot_erase` is called from nowhere else. `otg_slot_index` survives only in `playlist_scan`'s hide filter (read-only; it still probes for `present` before hiding) and inside `otg_slot_of`.
- **Right sector on a fragmented file:** the probe goes through `read_head` → `fat32_read_file(fs, clus, buf, ≤512)`, which reads from the file's FIRST cluster and follows the chain from there, so the head is the head whatever the chain looks like. (The fixture's foreign file is a contiguous 4-cluster run; the fragmented slot 1's probe was already exercised in r1's `test_save`.)
- **Damaged is still ours:** `otg_slot_of` returns n whenever `present`, damaged or not (test "a DAMAGED slot is still ours — Delete is how it is recovered"); the writer's probe likewise only asks for the directive, so Delete on a torn-but-headed slot goes through. Verified by reading both and by the test.
- The fixture's foreign slot 4 is 8192 B (a size the writer takes), the test asserts zero writes and byte identity, and that the file still lists and resolves as an ordinary playlist. Thirteen new checks; five of them are the ones that fail with the guard stubbed.

**S1 design-doc header** — fixed ("implemented, UNFLASHED", points at the guide). **S2 CHANGELOG** — one `## Unreleased` at line 6, the On-The-Go bullets under it. **S3 "damaged = free"** — corrected at all five sites (`otg_slot.h`, design-doc prose + table, `make_otg.py` `--dump` and `--verify`, `doctor.go`, guide), each with the load-bearing reason (Save only writes over padding). **S4** — STATUS records the 0x4C/0x52 comparison with the run shapes and the geometry. **N1 empty rows** — drawn greyed with the sentences underneath, `otg_empty.png` added and byte-identical. **N2 make_otg.py** — `return 1`; `str.isdigit()` on an ASCII-decoded line (the header is `decode("ascii")` in a try, the trailer `decode("ascii","replace")`, so Unicode digits cannot reach `int()`). **N3 otg_slot.h** — now says a mid-stage-0 failure can read as foreign. **N4** — the bench is first-flash checklist item 10 with the foreign-file check added. The `.mp3` row round-trips with `fmt == 1` and `name_hash("03 Third.mp3")`.

**Boot ordering after the clock rebase** (main.c:8163-8172): `cfg_commit_clear(&g_cfg_commit); otg_store_commit_clear(); if (clock_mark_unsaved) { settings_touch(); … }`. The OTG clear touches only otg_store's own `cfg_commit_t`; the clock's re-arm lands on the settings gate after both clears, so neither clear can discard the other's pending change. Correct resolution.

## Findings

### Should-fix

**S1 — `core/library/otg_slot.h:276-278`: "reads as foreign — listed and played, never written to again until a host sync" — a host sync does not recover it.** `EnsureOTGSlots` (`devicefs/otg.go:530`, `case err == nil: continue`) and `make_otg.py --create` (line 330, "exists — left alone", `--force` or not) create a slot file only when it is ABSENT; both deliberately never rewrite an existing one. So a slot whose stage-0 write failed part-way (header-less first sector ⇒ foreign) is stuck for good on the device AND survives every sync. The same is true of the more likely outcome of a sub-sector tear on a real drive — an *unreadable* first sector: `otg_slot_probe` returns −1, `playlist_scan` keeps it listed, it opens to "Could not read playlist" with no Delete row (`g_pl_err`), `otg_slot_free_index` skips it, and the writer refuses (−1). Both outcomes are the right failure mode (fail closed, nothing of the user's touched) and rare (a drive write failure or power cut inside the final ≤4 KiB run), but no document says how to get the slot back. The actual recovery is: delete `On-The-Go N.m3u8` on the host, then `core sync` / `make_otg.py --create` recreates it empty. Say that in `otg_slot.h`, in the design doc's foreign/damaged paragraph, in the guide's "A playlist of your own …" paragraph, and make `core doctor`'s foreign line (`doctor.go:589-591`) add "if you did not put this file here, delete it and run `core sync`" — doctor is the only thing that will ever show the user this state. Verified by reading both host creators and tracing the probe-failure path through `playlist_scan`, `playlist_open`, `otg_slot_free_index` and `otg_slot_save`.

### Nits

**N1 — `core/kernel/main.c:5639`: after hold-removing the LAST row the cursor lands on the greyed Save row, not Clear.** `g_otg_sel = otg_row_count() - 1` evaluates to 1 when the list has just become empty, while the new comment at main.c:9163 ("the cursor stays on row 0") and `render.py`'s `screen_otg_empty` (Clear selected) describe row 0, and the wheel is silent on an empty list so it cannot be moved back. Clamp to `OTG_ROW_CLEAR` when `g_otg.n == 0`. Cosmetic; every other path into the empty state (entry, Clear, Save) lands on row 0.

### Confirmed, no action

- `OTG_SLOT_EFOREIGN` (−5) is only ever compared for equality in the test; main.c treats every non-zero `rc` as "Could not save/delete", so a collision with a driver code would only affect the UART number.
- The writer's probe costs one cached FS-sector read per Save/Delete, before the load bar's long part; no behaviour change on the success path (the r1 byte-exact save test still passes with the `.mp3` row added).
- Python/Go parity is unaffected by the digits-only change (the parity fixtures compare encoded bytes; both parsers now reject the same inputs and Go already did).

r2 score: 8/10

---

# r3 — b015b82 on e93e39c (main b8f627c)

One commit, 11 files, +173/−24: docs and messages, the doctor lines, `--create`'s per-slot report,
and the one-line cursor clamp. Read every hunk.

## What I ran (at b015b82)

| command | result |
|---|---|
| `make sim && meson test -C build-sim` | **78/78 OK** |
| `make hw && make verify-hw` | clean; text 424,864 B (the "415 KB" STATUS claims), bss 12,382,512 B (11.81 MiB) — unchanged from r2 but for the clamp |
| `go test -count=1 ./...` | 22 packages ok; both `MakeOTG` parity tests PASS |
| `make_otg.py --selftest` | OK |
| `render.py` + `git status` | all four OTG stills and `playlists.png` byte-identical; only the version-stamped boot images differ (restored) |
| `core doctor --volume <fixture>` and `make_otg.py --create <fixture>` on a volume I staged with slot 1 unreadable (a directory at the path), 2 used, 3 torn (header gen 5 / trailer 4), 4 foreign (8 KiB, no directive), 5 empty | see below |

## The r2 findings, verified against the code and the live output

**S1 (recovery of a header-less / unreadable slot) — fixed as option (a), fail-closed with the way out written where it is needed.**
- `core doctor`, run live on the fixture: slot 1 → `FAIL … read …: is a directory — the device will never write to a slot it cannot read. If the file is not one of yours, delete Music/Playlists/On-The-Go 1.m3u8 and run `core sync` to put an empty one back`; slot 4 → `WARN … carries no On-The-Go header, so the device lists and plays it and NEVER writes to it. If you did not put it there (an interrupted save can leave one looking like this), delete Music/Playlists/On-The-Go 4.m3u8 and run `core sync` to put an empty one back`; slot 3 → the r2 "NOT free until that Delete rewrites it" line; 2 and 5 as before. That is exactly `doctor.go:584-603`, and `doctor/otg_test.go` now asserts `NEVER writes to it`, `delete`, `core sync` and the file name on the foreign line plus a new unreadable sub-test (a directory at the slot path → FAIL containing `core sync`).
- `make_otg.py --create`, run live a second time over the same fixture: the five slots print `exists but will not read (… Is a directory …) / the device will never write to it; if it is not yours, delete it and re-run`, `exists, 2 track(s) — left alone`, `exists, torn save — left alone (Delete Playlist on the device frees it)`, `exists with NO On-The-Go header — a playlist of your own, or a save interrupted at its last write … if it is not yours, delete it and re-run this.`, `exists, empty — left alone`. Matches `do_create` (make_otg.py:334-363); the unreadable case goes through the `OSError` branch. Still never writes an existing slot, `--force` or not.
- The recovery ("delete `Music/Playlists/On-The-Go N.m3u8`, then `core sync` / `make_otg.py --create` recreates an ABSENT slot empty") is now stated in `otg_slot.h:273-300` (with the unreadable-probe case, which I confirmed returns −1 at otg_slot.c's guard), the design doc's foreign paragraph and state table, the guide's On-The-Go paragraph and a new troubleshooting entry, `EnsureOTGSlots`'s and `--create`'s own comments, tools/README, cli/README's doctor row, and STATUS checklist item 10 step 6. The rationale for refusing a reset heuristic (stale entry lines parse as valid M3U8; a user's playlist may legitimately be empty or all-missing) is recorded in `otg_slot.h` and the design doc. I agree with the choice: there is no discriminator the host could apply that is not wrong in one direction, and "never overwrite a file the user made" is the branch's whole discipline.

**N1 (cursor after removing the last row) — fixed.** `row_select_hold` (main.c:5638-5645) sets `g_otg_sel = OTG_ROW_CLEAR` when `g_otg.n == 0`, else the old clamp; now every path into the empty state lands on row 0, as the empty-state comment and `otg_empty.png` say.

## Anything left?

I re-read the r3 hunks for a stale line and found none: every sentence about damaged/foreign/unreadable slots now matches what `otg_slot_free_index`, `otg_slot_of`, `otg_slot_save`'s probe, `EnsureOTGSlots` and `do_create` do. All prior r1/r2 verifications (disk-safety trace, tear ordering, cache drop, gate rules, LBA parity, Python/Go byte parity, the stubbed-guard experiment) still hold — the code they cover did not change in r3. Everything device-facing remains explicitly deferred to the bench in STATUS checklist item 10, which is the correct disposition for this job.

SCORE: 10/10
