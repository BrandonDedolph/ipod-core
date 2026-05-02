#!/usr/bin/env bash
# Re-fetch stb_image.h from upstream. Idempotent.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
URL="https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"

echo "==> Fetching $URL"
curl -sL -o "$DIR/stb_image.h" "$URL"

VER=$(awk -F'[" ]+' '/stb_image - v[0-9]/ {print $4; exit}' "$DIR/stb_image.h")
echo "    stb_image version: $VER"
echo "==> Update LICENSE manually with the new version string."
