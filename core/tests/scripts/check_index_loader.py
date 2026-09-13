#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check_index_loader.py — make sure the library loader USES the index
validator.

library/idx.c validates a CORELIB.IDX header (idx_header_parse) and checks the
records' CRC-32 (crc32_update); tests/library/idx_test.c tests them directly.
What a unit test cannot show is that kernel/main.c's library_load_index()
consults them: a validator the loader does not call is decoration, and that is
exactly what a refactor could quietly leave behind while the unit test kept
passing. So this script asserts, by reading main.c, that library_load_index():

  - calls idx_header_parse(), and hands it g_idx_size — the size check lives
    inside the parser, and compares against nothing if the directory entry's
    size is not passed;
  - calls crc32_update() and refuses on IDX_ECRC.

(This file used to be check_index_parity.py, whose first job was diffing the
test's verbatim copies of the functions against main.c. The copies are gone —
the test compiles library/idx.c — and only this half remains.)
"""

import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve()
CORE = HERE.parent.parent.parent            # <repo>/core

MAIN_C = CORE / "kernel" / "main.c"
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
            "       It moved or was renamed. Update LOADER in "
            "tests/scripts/check_index_loader.py."
        )
    j = i
    while lines[j] != "}":
        j += 1
    return "\n".join(lines[i:j + 1])


def main():
    fails = 0
    main_lines = MAIN_C.read_text(encoding="utf-8").split("\n")

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

    # A failed record read is not the end of the index. The loop used to
    # `break` on any got <= 0, so an EIO/ECORRUPT mid-file looked like EOF:
    # n < count flagged the library "too large", the CRC (which only runs
    # when every record streamed past) was skipped, and library_finish ran
    # on the partial set with g_lib_load_err 0 — no retry, no scan fallback,
    # half the library missing behind the wrong diagnosis. The loader must:
    #   - never conflate a negative return with EOF;
    #   - retry an EIO on the load's retry budget (the same one lib_readdir
    #     spends), and on a final EIO record it in g_lib_load_err — the disk,
    #     not the file — and refuse the load (IDX_EREAD);
    #   - refuse (IDX_EREAD) on any other negative or short return, which is
    #     the file, so library_ensure's scan fallback takes over.
    read_fails = 0
    # Code only: the loader's own comment quotes the old line as a warning.
    code = re.sub(r"/\*.*?\*/", "", loader, flags=re.S)
    if re.search(r"if\s*\(\s*got\s*<=\s*0\s*\)\s*break\s*;", code):
        read_fails += 1
        print("FAIL: library_load_index() still breaks out of the record loop "
              "on got <= 0 — a read error is being treated as end-of-file.",
              file=sys.stderr)
    if not re.search(r"got\s*==\s*FAT32_EIO[^;]*g_lib_retry_budget\s*>\s*0", code,
                     re.S):
        read_fails += 1
        print("FAIL: library_load_index() does not retry an EIO record read "
              "against g_lib_retry_budget.", file=sys.stderr)
    if not re.search(r"if\s*\(\s*got\s*==\s*FAT32_EIO\s*\)\s*\{\s*"
                     r"g_lib_load_err\s*=\s*FAT32_EIO\s*;\s*"
                     r"return\s+idx_reject\(\s*IDX_EREAD\s*\)\s*;", code, re.S):
        read_fails += 1
        print("FAIL: library_load_index() does not report a final EIO in "
              "g_lib_load_err and refuse the load with IDX_EREAD.",
              file=sys.stderr)
    if not re.search(r"if\s*\(\s*got\s*<\s*0\s*\|\|.*?\)\s*\{\s*"
                     r"return\s+idx_reject\(\s*IDX_EREAD\s*\)\s*;", code, re.S):
        read_fails += 1
        print("FAIL: library_load_index() does not refuse (IDX_EREAD) a "
              "negative or short record read.", file=sys.stderr)
    if read_fails == 0:
        print("OK: library_load_index() retries an EIO and refuses, never "
              "truncates, on a failed record read")
    fails += read_fails

    if fails:
        print(f"\n{fails} index loader failure(s)", file=sys.stderr)
        return 1
    print("index loader: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
