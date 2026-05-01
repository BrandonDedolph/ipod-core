#!/usr/bin/env bash
# Regenerate the codec test vectors. Run this once when adding a new
# vector. Output (.pcm, .flac, .mp3, .mp3.ref.pcm) gets committed; the
# KAT just memcmps against the committed bytes.
#
# Requires:
#   - python3 (any 3.x)            — synthetic-PCM generator
#   - flac                          — FLAC reference encoder
#   - ffmpeg (with libmp3lame)      — MP3 encoder
#   - gcc                           — to build the one-shot capture tool
#
# Usage:
#   ./gen_codec_vectors.sh        # regenerate everything
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VECTORS_DIR="$(cd "$SCRIPTS_DIR/../codec-vectors" && pwd)"

GEN="$SCRIPTS_DIR/gen_codec_vectors.py"
CAPTURE_SRC="$SCRIPTS_DIR/capture_mp3_ref.c"
CAPTURE_BIN="$(mktemp -t capture_mp3_ref.XXXXXX)"
trap 'rm -f "$CAPTURE_BIN"' EXIT

mkdir -p "$VECTORS_DIR"

# Build the one-shot capture tool. This is intentionally NOT part of
# the meson build — it's a fixture-generation helper, not a runtime
# component. It's compiled fresh each run from the same dr_mp3.h the
# wrapper uses, so the captured PCM matches what the runtime decoder
# will produce.
echo "==> building capture_mp3_ref"
gcc -O2 -Wall -o "$CAPTURE_BIN" "$CAPTURE_SRC" -lm

# ---------- FLAC vectors (lossless) ----------------------------------

# Vector spec: name, sample_rate, channels, bits-per-sample
gen_flac() {
    local name="$1"
    local rate="$2"
    local channels="$3"
    local bps="$4"

    local pcm="$VECTORS_DIR/$name.pcm"
    local flac="$VECTORS_DIR/$name.flac"

    echo "==> flac/$name"
    # Use system python explicitly so asdf's Python pin doesn't intercept.
    /usr/bin/python3 "$GEN" "$name" > "$pcm"

    flac --silent --force \
         --endian=little --sign=signed \
         --sample-rate="$rate" --channels="$channels" --bps="$bps" \
         --no-padding --no-md5-sum \
         -o "$flac" "$pcm"

    # Keep the raw .pcm — the KAT memcmp's against it. Stability across
    # libms isn't a concern then; the bytes are committed.
    sha256sum "$flac" | awk '{print $1}' > "$flac.sha256"
    sha256sum "$pcm"  | awk '{print $1}' > "$pcm.sha256"
    printf "    %s\n    %s\n" "$flac" "$pcm"
}

# ---------- MP3 vectors (lossy) --------------------------------------

# Encodes the same source PCM that the FLAC step produced into MP3,
# then captures dr_mp3's decoded output as the reference. The
# committed reference is what dr_mp3 *currently* produces; any change
# to dr_mp3 or our wrapper that alters output trips the KAT.
gen_mp3() {
    local source_name="$1"     # e.g. sine_440hz_1s_44k_s16_stereo (must already have .pcm)
    local rate="$2"
    local channels="$3"
    local bitrate="$4"         # e.g. 128k

    local src_pcm="$VECTORS_DIR/$source_name.pcm"
    local mp3="$VECTORS_DIR/${source_name}_${bitrate}.mp3"
    local ref="$VECTORS_DIR/${source_name}_${bitrate}.mp3.ref.pcm"

    if [[ ! -f "$src_pcm" ]]; then
        echo "ERROR: $src_pcm missing — run gen_flac for $source_name first" >&2
        return 1
    fi

    echo "==> mp3/${source_name}_${bitrate}"
    ffmpeg -hide_banner -loglevel error -y \
        -f s16le -ar "$rate" -ac "$channels" -i "$src_pcm" \
        -c:a libmp3lame -b:a "$bitrate" \
        "$mp3"

    "$CAPTURE_BIN" "$mp3" "$ref"

    sha256sum "$mp3" | awk '{print $1}' > "$mp3.sha256"
    sha256sum "$ref" | awk '{print $1}' > "$ref.sha256"
    printf "    %s\n    %s\n" "$mp3" "$ref"
}

# ---------- run -------------------------------------------------------

gen_flac sine_440hz_1s_44k_s16_stereo 44100 2 16

gen_mp3  sine_440hz_1s_44k_s16_stereo 44100 2 128k

# Add more vectors here as new codecs land:
# gen_flac sine_440hz_1s_44k_s16_mono 44100 1 16
# gen_mp3  sine_440hz_1s_44k_s16_stereo 44100 2 192k
# (...)
