#!/usr/bin/env bash
# Re-fetch dr_mp3.h from upstream. Idempotent.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
URL="https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h"

echo "==> Fetching $URL"
curl -sL -o "$DIR/dr_mp3.h" "$URL"

# The header line is "dr_mp3 - vX.Y.Z - <date>"; pull X.Y.Z out by
# eyeballing the third whitespace-delimited token. This parser is
# fragile against upstream comment-format changes — if it breaks,
# update LICENSE manually after re-fetching.
VER=$(awk -F'[" ]+' '/dr_mp3 - v[0-9]/ {print $4; exit}' "$DIR/dr_mp3.h")
echo "    dr_mp3 version: $VER"

echo "==> Update LICENSE manually with the new version string."
echo "==> If dr_mp3 actually changed, re-run:"
echo "      core/tests/scripts/gen_codec_vectors.sh"
echo "    to re-bake the MP3 reference PCM (dr_mp3 output may have"
echo "    drifted; the committed fixture is what dr_mp3 produced at"
echo "    the previous version)."
