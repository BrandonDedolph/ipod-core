#!/usr/bin/env bash
# Re-fetch dr_flac.h from upstream. Idempotent; updates LICENSE comment.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
URL="https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h"

echo "==> Fetching $URL"
curl -sL -o "$DIR/dr_flac.h" "$URL"

VER=$(awk -F'[" ]+' '/dr_flac - v[0-9]/ {print $4; exit}' "$DIR/dr_flac.h")
echo "    dr_flac version: $VER"

echo "==> Update LICENSE manually with the new version string."
