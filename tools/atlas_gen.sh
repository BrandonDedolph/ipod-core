#!/usr/bin/env bash
# Regenerate the Nunito glyph atlases.
# One-shot — run when font sources change. Outputs are committed.
#
# Requires: tools/.venv (with Pillow). If missing, run:
#     /usr/bin/python3 -m venv tools/.venv
#     tools/.venv/bin/pip install Pillow
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$REPO/tools/.venv/bin/python3"
GEN="$REPO/tools/atlas_gen.py"
SRC="$REPO/tools/fonts-src"
OUT="$REPO/core/apps/ui/atlas"

if [[ ! -x "$PY" ]]; then
    echo "ERROR: $PY missing. Set up the venv:" >&2
    echo "  /usr/bin/python3 -m venv $REPO/tools/.venv" >&2
    echo "  $REPO/tools/.venv/bin/pip install Pillow" >&2
    exit 1
fi

mkdir -p "$OUT"

gen() {
    local ttf="$1"
    local px="$2"
    local sym="$3"
    local file="$4"
    echo "==> $sym"
    "$PY" "$GEN" "$SRC/$ttf" "$px" "$sym" "$OUT/$file"
}

# What we ship right now: body + title. More sizes added here as the
# UI calls for them.
gen Nunito-Regular.ttf 13 NUNITO_REGULAR_13 nunito_regular_13.h
gen Nunito-Bold.ttf    17 NUNITO_BOLD_17    nunito_bold_17.h
