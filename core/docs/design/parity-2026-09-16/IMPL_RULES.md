# Implementer rules (read after BRIEF.md and your plan)

- Work ONLY inside your worktree directory on your branch. Never touch the main checkout, never
  `git checkout main`, never merge, never push. Commit as you go with the commit style in BRIEF.md.
- Implement the plan COMPLETELY, including tests, docs (USER_GUIDE.md, STATUS.md UNFLASHED entry,
  README where the plan says), and the gallery renderer changes if the plan names them. Where the plan
  gives a choice, take its recommendation. If you must deviate, record why in your final report.
- Before reporting done, from <worktree>/core run and paste the tail of:
    make sim && meson test -C build-sim
    make hw && make verify-hw
  Both must pass. Also `go test ./...` from core/cli if you touched Go, and any Python selftest the
  plan names. -Werror is on; warnings are failures.
- Anything that can only be verified on the device must be listed under "Device-only" in your report,
  not glossed over. Do not claim device behaviour you did not observe.
- Quality bar: this will be reviewed to a 10/10 standard: correctness in every edge case the plan lists,
  no dead code, no stale comments, names that match the codebase's idiom, tests that would actually catch
  a regression, and the docs telling the truth about what the device does.
- Final report format: branch; commit list (`git log --oneline main..HEAD`); files changed summary;
  test output tails; deviations from the plan; device-only items; anything left undone (must be none,
  or say why).
