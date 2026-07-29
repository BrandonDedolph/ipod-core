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
# Sizes are chosen for RASTERISATION QUALITY, not roundness — see
# tools/glyph_quality.py. Nunito's fringe (ink below 25% alpha, i.e. blur that
# never resolves) is strongly non-monotonic in pixel size, so a size can be
# both larger and mushier:
#     regular  11: 23.4%   12: 25.2%   13: 37.6%   14: 37.1%
#     bold     11: 33.4%   12: 20.1%   13: 14.7%   17: 22.9%   18: 14.4%
# regular 13 and bold 11/17 all sit on bad landings. Shipping them next to
# bold 13 (the sharpest size the face has) is what made the UI look uneven:
# a 37.6% row directly under a 14.7% header is a 2.5x mismatch in one list.
gen Nunito-Regular.ttf  9 NUNITO_REGULAR_9  nunito_regular_9.h
gen Nunito-Regular.ttf 11 NUNITO_REGULAR_11 nunito_regular_11.h
gen Nunito-Regular.ttf 12 NUNITO_REGULAR_12 nunito_regular_12.h
gen Nunito-Bold.ttf    12 NUNITO_BOLD_12    nunito_bold_12.h
gen Nunito-Bold.ttf    13 NUNITO_BOLD_13    nunito_bold_13.h
gen Nunito-Bold.ttf    18 NUNITO_BOLD_18    nunito_bold_18.h
