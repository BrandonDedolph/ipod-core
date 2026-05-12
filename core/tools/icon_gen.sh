#!/usr/bin/env bash
# Render the design's status-bar SVG icons (shuffle, repeat) into 12×10
# anti-aliased alpha masks. Output is C arrays you paste into
# core/apps/ui/chrome.c — the SVGs themselves live in tools/icons/.
#
# Approach:
#   1. rsvg-convert renders each SVG at 16× target (192×160). 16× is
#      overkill for AA quality but the build cost is irrelevant and it
#      removes any residual quantisation in the upsample stage.
#   2. Box-filter downsample to 12×10. Box is the right averager here:
#      gaussian / lanczos invent edge pixels (haloes, ringing) that the
#      firmware's chrome_blit_alpha can't compensate for.
#   3. Extract the alpha channel and emit a C array.
#
# Backends: prefer ImageMagick (matches the original pipeline). Fall
# back to Python+Pillow on dev machines without ImageMagick — both
# produce byte-identical output for box downsample of a single-channel
# alpha image.
#
# Re-run whenever icons/*.svg changes or the firmware grows the icons.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ICONS_DIR="$HERE/icons"
TARGET_W=12
TARGET_H=10
RENDER_W=192   # 16× upsample for AA quality (cheap at build time)
RENDER_H=160

if ! command -v rsvg-convert >/dev/null; then
    echo "need rsvg-convert (apt: librsvg2-bin)" >&2
    exit 1
fi

MAGICK=""
if command -v magick >/dev/null; then
    MAGICK=$(command -v magick)
elif command -v convert >/dev/null; then
    MAGICK=$(command -v convert)
fi

if [ -z "$MAGICK" ]; then
    if ! python3 -c "import PIL" 2>/dev/null; then
        echo "need ImageMagick (apt: imagemagick) or Python Pillow (pip install pillow)" >&2
        exit 1
    fi
fi

downsample_with_magick() {
    local big=$1 small=$2 txt=$3
    # Stage in two steps: resize first, then extract+dump. ImageMagick's
    # newer `magick` driver doesn't preserve the alpha channel cleanly
    # across `-alpha extract` chained with `-resize` in one pipe — the
    # txt: output ends up all-zeros. Two passes keeps it deterministic.
    "$MAGICK" "$big" -filter Box \
        -resize "${TARGET_W}x${TARGET_H}" "$small"
    "$MAGICK" "$small" -alpha extract -depth 8 txt: > "$txt"
}

downsample_with_pillow() {
    local big=$1 small=$2 txt=$3
    python3 - "$big" "$small" "$txt" "$TARGET_W" "$TARGET_H" <<'PY'
import sys
from PIL import Image
big, small, txt, w, h = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
img = Image.open(big).convert("RGBA")
# Image.BOX is box-filter downsample, matches ImageMagick -filter Box.
out = img.resize((w, h), Image.BOX)
out.save(small)
alpha = out.split()[-1]
# Emit ImageMagick-compatible txt: format so the parser below stays one path.
data = list(alpha.tobytes())
with open(txt, "w") as f:
    f.write(f"# ImageMagick pixel enumeration: {w},{h},255,gray\n")
    for y in range(h):
        for x in range(w):
            a = data[y*w + x]
            f.write(f"{x},{y}: ({a})  #{a:02X}{a:02X}{a:02X}  gray({a})\n")
PY
}

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
    if [ -n "$MAGICK" ]; then
        downsample_with_magick \
            "/tmp/${icon}_big.png" \
            "/tmp/${icon}_small.png" \
            "/tmp/${icon}.txt"
    else
        downsample_with_pillow \
            "/tmp/${icon}_big.png" \
            "/tmp/${icon}_small.png" \
            "/tmp/${icon}.txt"
    fi
    upper=$(echo "$icon" | tr a-z A-Z)
    emit_array "${upper}_ALPHA" "/tmp/${icon}.txt"
    echo
done
