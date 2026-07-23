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
OUT="$REPO/core/ui/atlas"

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

# Sizes the Linen design calls for. Each new atlas costs ~5-15 KB of
# .rodata (static const u8); cheap to keep around.
gen Nunito-Regular.ttf  9 NUNITO_REGULAR_9  nunito_regular_9.h
gen Nunito-Regular.ttf 11 NUNITO_REGULAR_11 nunito_regular_11.h
gen Nunito-Regular.ttf 13 NUNITO_REGULAR_13 nunito_regular_13.h
gen Nunito-Bold.ttf     9 NUNITO_BOLD_9     nunito_bold_9.h
gen Nunito-Bold.ttf    11 NUNITO_BOLD_11    nunito_bold_11.h
gen Nunito-Bold.ttf    13 NUNITO_BOLD_13    nunito_bold_13.h
gen Nunito-Bold.ttf    17 NUNITO_BOLD_17    nunito_bold_17.h
