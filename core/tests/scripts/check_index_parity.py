#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check_index_parity.py — keep the index validator's test copy honest, and
make sure the loader uses it.

kernel/main.c validates a CORELIB.IDX header (idx_header_parse) and checks the
records' CRC-32 (crc32_update) before trusting a record. Both are `static` in
a file that cannot be linked into a host test, so tests/kernel/index_test.c
holds VERBATIM COPIES — the same arrangement as resume_test.c and
name_hash_ref.c, checked the same way:

  A. each copy is byte-identical to main.c's;
  B. library_load_index() actually CALLS idx_header_parse() and compares the
     CRC. A validator the loader does not consult is decoration, and that is
     exactly what a refactor could quietly leave behind while this test kept
     passing against the copy.
"""

import difflib
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve()
CORE = HERE.parent.parent.parent            # <repo>/core

MAIN_C = CORE / "kernel" / "main.c"
TEST_C = CORE / "tests" / "kernel" / "index_test.c"

FUNCS = (
    "static uint32_t idx_rd32(",
    "static int idx_header_parse(",
    "static void crc32_tab_init(",
    "static uint32_t crc32_update(",
)
LOADER = "static int library_load_index("


def extract_func(lines, sig, path):
    """The source text of sig's function: from its definition line to the
    first line that is exactly '}'. Holds for this codebase's style, and
    needing no C parser is the point."""
    try:
        i = next(k for k, l in enumerate(lines) if l.startswith(sig))
    except StopIteration:
        raise SystemExit(
            f"FAIL: {path}: no function starting with {sig!r}.\n"
            "       It moved or was renamed — which is itself a parity break. "
            "Update FUNCS in tests/scripts/check_index_parity.py and re-sync "
            "tests/kernel/index_test.c."
        )
    j = i
    while lines[j] != "}":
        j += 1
    return "\n".join(lines[i:j + 1])


def main():
    fails = 0
    main_lines = MAIN_C.read_text(encoding="utf-8").split("\n")
    test_lines = TEST_C.read_text(encoding="utf-8").split("\n")

    # --- A. the copies must not have drifted -----------------------------
    for sig in FUNCS:
        orig = extract_func(main_lines, sig, MAIN_C)
        copy = extract_func(test_lines, sig, TEST_C)
        if orig != copy:
            fails += 1
            print(f"FAIL: {sig[:-1]} differs between\n"
                  f"        {MAIN_C}\n      and the copy in\n"
                  f"        {TEST_C}\n"
                  "      The copy is what the host test compiles, so it must "
                  "be re-pasted verbatim.", file=sys.stderr)
            for line in difflib.unified_diff(orig.split("\n"), copy.split("\n"),
                                             "main.c", "index_test.c",
                                             lineterm=""):
                print("      " + line, file=sys.stderr)
    if fails == 0:
        print(f"OK: {len(FUNCS)} index functions match {MAIN_C.name}")

    # --- B. the loader must consult them ---------------------------------
    loader = extract_func(main_lines, LOADER, MAIN_C)
    wants = {
        "idx_header_parse(": "the header validator",
        "crc32_update(": "the record CRC",
        "IDX_ECRC": "a CRC mismatch being refused",
    }
    for needle, what in wants.items():
        if needle not in loader:
            fails += 1
            print(f"FAIL: library_load_index() in {MAIN_C} does not reference "
                  f"{needle!r} — {what} is tested but not used.",
                  file=sys.stderr)
    # The size check lives inside idx_header_parse; the loader must hand it
    # the directory entry's size, or the check compares against nothing.
    if not re.search(r"idx_header_parse\(\s*hdr\s*,\s*g_idx_size", loader):
        fails += 1
        print("FAIL: library_load_index() does not pass g_idx_size to "
              "idx_header_parse(); the truncated-file check is inert.",
              file=sys.stderr)
    if fails == 0:
        print("OK: library_load_index() validates the header and the CRC")

    if fails:
        print(f"\n{fails} index parity failure(s)", file=sys.stderr)
        return 1
    print("index parity: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
