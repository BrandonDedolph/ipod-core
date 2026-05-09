#!/usr/bin/env bash
# Render the design's status-bar SVG icons (shuffle, repeat) into 12×10
# anti-aliased alpha masks. Output is C arrays you paste into
# core/apps/ui/chrome.c — the SVGs themselves live in tools/icons/.
#
# Approach:
#   1. rsvg-convert renders each SVG at 8× target (96×80) so librsvg's
#      built-in AA handles the heavy lifting.
#   2. ImageMagick's box filter downsamples to 12×10. Box is the right
#      averager here: gaussian / lanczos invent edge pixels that don't
#      match what the firmware's chrome_blit_alpha expects.
#   3. Extract the alpha channel and dump as text.
#   4. Reformat into a C uint8_t[120] array.
#
# Re-run whenever themes.jsx changes the icon paths or the firmware
# decides to grow the icons.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ICONS_DIR="$HERE/icons"
TARGET_W=12
TARGET_H=10
RENDER_W=96   # 8× upsample for AA quality
RENDER_H=80

if ! command -v rsvg-convert >/dev/null; then
    echo "need rsvg-convert (apt: librsvg2-bin)" >&2
    exit 1
fi
if ! command -v magick >/dev/null && ! command -v convert >/dev/null; then
    echo "need ImageMagick" >&2
    exit 1
fi
MAGICK=$(command -v magick || command -v convert)

emit_array() {
    local name=$1 file=$2
    python3 - "$name" "$file" <<'PY'
import re, sys
name, path = sys.argv[1], sys.argv[2]
W, H = 12, 10
grid = [[0]*W for _ in range(H)]
for line in open(path):
    m = re.match(r'(\d+),(\d+):\s*\((\d+)\)', line)
    if not m: continue
    x, y, a = int(m[1]), int(m[2]), int(m[3])
    if 0 <= x < W and 0 <= y < H:
        grid[y][x] = a
print(f"static const uint8_t {name}[{W} * {H}] = {{")
for row in grid:
    print("    " + ", ".join(f"{v:3d}" for v in row) + ",")
print("};")
PY
}

cd "$ICONS_DIR"
for icon in shuffle repeat; do
    rsvg-convert -w $RENDER_W -h $RENDER_H "${icon}.svg" > "/tmp/${icon}_big.png"
    # Stage in two steps: resize first, then extract+dump. ImageMagick's
    # newer `magick` driver doesn't preserve the alpha channel cleanly
    # across `-alpha extract` chained with `-resize` in one pipe — the
    # txt: output ends up all-zeros. Two passes keeps it deterministic.
    "$MAGICK" "/tmp/${icon}_big.png" -filter Box \
        -resize "${TARGET_W}x${TARGET_H}" "/tmp/${icon}_small.png"
    "$MAGICK" "/tmp/${icon}_small.png" -alpha extract -depth 8 txt: \
        > "/tmp/${icon}.txt"
    upper=$(echo "$icon" | tr a-z A-Z)
    emit_array "${upper}_ALPHA" "/tmp/${icon}.txt"
    echo
done
