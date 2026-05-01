#!/usr/bin/env bash
# Regenerate the codec test vectors. Run this once when adding a new
# vector or after changing the generator formulas. Output FLAC files
# get committed to the repo; downstream tests just use them.
#
# Requires: python3 (any 3.x), flac (the reference encoder).
#
# Usage:
#   ./gen_codec_vectors.sh        # regenerate everything
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VECTORS_DIR="$(cd "$SCRIPTS_DIR/../codec-vectors" && pwd)"

GEN="$SCRIPTS_DIR/gen_codec_vectors.py"

# Vector spec: name, sample_rate, channels, bits-per-sample
gen_flac() {
    local name="$1"
    local rate="$2"
    local channels="$3"
    local bps="$4"

    local pcm="$VECTORS_DIR/$name.pcm"
    local flac="$VECTORS_DIR/$name.flac"

    echo "==> $name"
    # Use system python explicitly so asdf's Python pin doesn't intercept.
    /usr/bin/python3 "$GEN" "$name" > "$pcm"

    # Force overwrite (-f), silent (-s), specific format flags.
    flac --silent --force \
         --endian=little --sign=signed \
         --sample-rate="$rate" --channels="$channels" --bps="$bps" \
         --no-padding --no-md5-sum \
         -o "$flac" "$pcm"

    # Keep the raw .pcm — the KAT memcmp's the decoded output against
    # it. Bit-exactness then doesn't depend on libm sin() being stable
    # across hosts; the bytes are committed.
    sha256sum "$flac" | awk '{print $1}' > "$flac.sha256"
    sha256sum "$pcm"  | awk '{print $1}' > "$pcm.sha256"
    printf "    %s\n" "$flac"
    printf "    %s\n" "$pcm"
}

mkdir -p "$VECTORS_DIR"

gen_flac sine_440hz_1s_44k_s16_stereo 44100 2 16

# Add more vectors here as new codecs land:
# gen_flac sine_440hz_1s_44k_s16_mono 44100 1 16
# (...)
