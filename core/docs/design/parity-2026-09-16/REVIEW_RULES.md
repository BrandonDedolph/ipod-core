# Reviewer rules

You are reviewing one feature branch in a worktree against its plan. Read BRIEF.md, then the plan,
then `git log --oneline main..HEAD` and `git diff main...HEAD` in the worktree. Read every changed file
in full where the diff is not self-explanatory; read the callers of anything whose contract changed.

Independently run, from <worktree>/core:
    make sim && meson test -C build-sim
    make hw && make verify-hw
plus `go test ./...` in core/cli if Go changed and any Python selftest the plan names. Do not trust the
implementer's pasted output.

Score the branch 1..10. 10 means: you would merge it to main as-is; every acceptance criterion in the
plan is met or explicitly and correctly deferred as device-only; no correctness bug in any edge case the
plan lists or you can construct; no stale comment, dead code, misleading name, or doc line that does not
match the code; tests would catch a regression of the behaviour they claim to cover; the commit history
is clean and the messages explain why. Anything less is not 10.

Findings: each with file:line, severity (blocker / should-fix / nit), what is wrong, what correct looks
like, and how you verified it (ran it / read it / constructed the case). Nits alone can still block a 10 if
they are real. Do not pad; do not invent findings to seem thorough. Do not fix anything yourself.

Write the review to the path you were given and end your reply with a line exactly of the form
    SCORE: <n>/10
followed by a one-paragraph verdict.
