#!/usr/bin/env bash
# Regenerate the codec test vectors. Run this once when adding a new
# vector. Output (.pcm, .flac, .mp3, .mp3.ref.pcm, .ffmpeg.flac) gets
# committed; the KAT just memcmps against the committed bytes.
#
# Two kinds of MP3 reference are produced and they are NOT interchangeable:
#   *.mp3.ref.pcm    our own decoder's output, the KAT regression pin
#   *.ffmpeg.flac    ffmpeg's decode of the same file, the accuracy TRUTH,
#                    compared to a tolerance because two correct MP3 decoders
#                    are never bit-identical
# See ../codec-vectors/README.md.
#
# Requires:
#   - python3 (any 3.x)            — synthetic-PCM generator
#   - flac                          — FLAC reference encoder
#   - lame                          — MP3 encoder (CBR + VBR with a Xing TOC)
#   - ffmpeg                        — reference MP3 decoder, resampling
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

# Preflight: lame does the encoding (it is the one that writes a Xing TOC
# for the VBR vector), ffmpeg does the reference decoding and resampling.
for tool in lame ffmpeg flac; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: $tool is not on PATH; MP3 fixture generation needs it." >&2
        exit 1
    }
done

# Build the one-shot capture tool. This is intentionally NOT part of the meson
# build — it's a fixture-generation helper, not a runtime component. It is
# compiled fresh each run from the SAME wrapper and the same vendored pvmp3
# the firmware links, so the captured PCM is what the runtime decoder produces.
echo "==> building capture_mp3_ref"
PVMP3_DIR="$SCRIPTS_DIR/../../codecs/pvmp3"
# shellcheck disable=SC2046  # the glob is the point
gcc -O2 -w -o "$CAPTURE_BIN" "$CAPTURE_SRC" \
    "$PVMP3_DIR/mp3.c" "$PVMP3_DIR/mp3_frame.c" \
    $(ls "$PVMP3_DIR"/upstream/*.c) -I"$PVMP3_DIR/upstream" -lm

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

# Encodes the same source PCM that the FLAC step produced into MP3, then
# captures OUR decoder's output as the reference. The committed reference is
# what pvmp3 + our wrapper produce *now*; any change to either that alters
# output trips the KAT.
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

# ---------- MP3 accuracy vectors (real music, ffmpeg truth) -----------

# Real music, because a 440 Hz sine exercises almost none of the decoder: no
# short blocks, no intensity stereo, no interesting Huffman tables. Source is
# a public-domain Musopen recording; see ../codec-vectors/README.md for the
# provenance and MUSOPEN_SRC below for how to re-cut it.
#
#   $1 output stem, $2 sample rate, $3 channels, $4.. the lame arguments
MUSOPEN_SRC="${MUSOPEN_SRC:-}"

gen_accuracy() {
    local stem="$1" rate="$2" channels="$3"
    shift 3

    local mp3="$VECTORS_DIR/$stem.mp3"
    local ref_flac="$VECTORS_DIR/$stem.ffmpeg.flac"
    local tmp_pcm="$VECTORS_DIR/.$stem.src.pcm"
    local tmp_ref="$VECTORS_DIR/.$stem.ref.pcm"

    if [[ -z "$MUSOPEN_SRC" ]]; then
        echo "    skipped (set MUSOPEN_SRC to the source recording to regenerate)"
        return 0
    fi

    echo "==> accuracy/$stem"
    ffmpeg -hide_banner -loglevel error -y -i "$MUSOPEN_SRC" \
        -ss 2 -t 2 -ar "$rate" -ac "$channels" -f s16le "$tmp_pcm"

    local chan_arg="j"
    [[ "$channels" == "1" ]] && chan_arg="m"
    lame --quiet -r -s "$(awk "BEGIN{print $rate/1000}")" --bitwidth 16 \
         --signed --little-endian -m "$chan_arg" "$@" "$tmp_pcm" "$mp3"

    # The TRUTH reference is ffmpeg's decode OF THE MP3 (not the source PCM):
    # the test asks "does our decoder agree with a known-good one", not "how
    # lossy is MP3". Stored as FLAC because it is lossless and half the size.
    ffmpeg -hide_banner -loglevel error -y -i "$mp3" -f s16le "$tmp_ref"
    flac --silent --force --endian=little --sign=signed \
         --sample-rate="$rate" --channels="$channels" --bps=16 -8 \
         --no-padding --no-md5-sum -o "$ref_flac" "$tmp_ref"
    rm -f "$tmp_pcm" "$tmp_ref"

    sha256sum "$mp3"      | awk '{print $1}' > "$mp3.sha256"
    sha256sum "$ref_flac" | awk '{print $1}' > "$ref_flac.sha256"
    printf "    %s\n    %s\n" "$mp3" "$ref_flac"
}

gen_flac sine_440hz_1s_44k_s16_stereo 44100 2 16

gen_mp3  sine_440hz_1s_44k_s16_stereo 44100 2 128k

gen_accuracy beethoven_2s_cbr128_44k_stereo 44100 2 -b 128 --cbr
gen_accuracy beethoven_2s_vbr_v5_44k_stereo 44100 2 -V 5
gen_accuracy beethoven_2s_cbr32_22k_mono    22050 1 -b 32 --cbr

# Add more vectors here as new codecs land:
# gen_flac sine_440hz_1s_44k_s16_mono 44100 1 16
# gen_mp3  sine_440hz_1s_44k_s16_stereo 44100 2 192k
# (...)
