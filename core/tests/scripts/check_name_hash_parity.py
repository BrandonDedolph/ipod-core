#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check_name_hash_parity.py — keep the two implementations of name_hash() in
step.

The library locator hash exists in two places:

  1. core/library/names.c          name_hash() + mn_utf8_next()  (on device)
  2. tools/build_index.py          name_hash() + norm_key()      (on the host)

(1) writes nothing and (2) reads nothing: the host tool stamps a hash into each
CORELIB.IDX record and the firmware uses that hash — and only that hash — to
find the matching file on disk. If the two implementations disagree for some
name, the affected track is not reported as missing; it silently never
resolves. names.c's comment already says the two "MUST stay byte-identical",
which is exactly the kind of promise that needs a test.

Both are held to ONE golden table, tests/kernel/name_hash_vectors.h. The C
side is asserted by tests/kernel/name_hash_test.c, which compiles the real
names.c (it used to compile a verbatim copy, and this script's first job was
diffing that copy against kernel/main.c — no longer needed). This script is
the Python side:

  (2) reproduces every golden vector, plus the collision pairs the fold exists
  for (case, smart quotes, dashes) and one non-collision.

Exits non-zero on any mismatch. NAME_HASH_XFAIL still parses (the marker stays
available for the next known C-side bug), but Python is held to the CORRECT
value either way, so both forms are asserted identically here.
"""

import importlib.util
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve()
CORE = HERE.parent.parent.parent            # <repo>/core
REPO = CORE.parent

VECTORS_H = CORE / "tests" / "kernel" / "name_hash_vectors.h"
BUILD_INDEX = REPO / "tools" / "build_index.py"


VEC_RE = re.compile(
    r'NAME_HASH_(VEC|XFAIL)\(\s*"(?P<label>[^"]*)"\s*,\s*'
    r'"(?P<lit>(?:[^"\\]|\\.)*)"\s*,\s*(?P<hash>0[xX][0-9A-Fa-f]+)u?',
    re.S,
)


def parse_vectors(path):
    """Parse the shared golden table into [(label, str, expected_hash)].

    The literal is raw UTF-8 bytes written with three-digit octal escapes;
    unicode_escape decodes those to code points 0..255, which map 1:1 to the
    bytes via latin-1. The result is then decoded as UTF-8 to recover the name
    the Python hash takes."""
    text = path.read_text(encoding="utf-8")
    # Ignore the two #ifndef guards in the header preamble.
    body = text.split("#endif /* CORE_TESTS")[0]
    vecs = []
    for m in VEC_RE.finditer(body):
        raw = m.group("lit").encode("ascii").decode("unicode_escape")
        name = raw.encode("latin-1").decode("utf-8")
        vecs.append((m.group("label"), name, int(m.group("hash"), 16)))
    return vecs


def load_build_index():
    spec = importlib.util.spec_from_file_location("build_index", BUILD_INDEX)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    fails = 0

    # --- the host tool must reproduce every golden vector ----------------
    vecs = parse_vectors(VECTORS_H)
    if len(vecs) < 10:
        raise SystemExit(f"FAIL: only parsed {len(vecs)} vectors from "
                         f"{VECTORS_H} — the table or the regex is broken")
    bi = load_build_index()
    for label, name, want in vecs:
        got = bi.name_hash(name)
        if got != want:
            fails += 1
            print(f"FAIL: build_index.name_hash({name!r}) = {got:08X}, "
                  f"golden {want:08X}  [{label}]", file=sys.stderr)
    if fails == 0:
        print(f"OK: tools/build_index.py name_hash matches all "
              f"{len(vecs)} golden vectors")

    # --- the fold's purpose, stated directly -----------------------------
    pairs = [
        ("case", "01. INTENTIONS.FLAC", "01. intentions.flac"),
        ("apostrophe", "Don’t Stop", "Don't Stop"),
        ("quotes", "“Hello”", '"Hello"'),
        ("en dash", "A – B", "A - B"),
        ("em dash", "A — B", "A - B"),
    ]
    for what, a, b in pairs:
        if bi.name_hash(a) != bi.name_hash(b):
            fails += 1
            print(f"FAIL: {what} fold does not collide in build_index.py: "
                  f"{a!r} vs {b!r}", file=sys.stderr)
    if bi.name_hash("01. Intentions.flac") == bi.name_hash("02. Intentions.flac"):
        fails += 1
        print("FAIL: distinct names collide in build_index.py", file=sys.stderr)

    if fails:
        print(f"\n{fails} name_hash parity failure(s)", file=sys.stderr)
        return 1
    print("name_hash parity: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
