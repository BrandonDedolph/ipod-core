#!/usr/bin/env bash
#
# Re-vendor pvmp3 (AOSP PacketVideo MPEG-1/2/2.5 Layer III decoder) into
# upstream/. Idempotent: it rebuilds upstream/ from scratch every run, so a
# clean `git status` afterwards is the proof that what is committed is exactly
# "the pinned tarball, renamed to C, plus patches/".
#
# Offline reproduction (CI, or a re-check without network): point
# PVMP3_TARBALL at a previously downloaded copy of the archive.
#
#   PVMP3_TARBALL=/tmp/mp3dec.tar.gz ./vendor.sh
#
# Bumping the pin: change COMMIT, run this, re-run SHA256SUMS.upstream through
# the printed instructions, then re-run the host suites — `codec-kat` compares
# against PCM captured from THIS decoder, so any upstream change that moves a
# sample trips it deliberately (see ../../tests/codec-vectors/README.md).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# platform/frameworks/av, media/module/codecs/mp3dec (was
# media/libstagefright/codecs/mp3dec before Android 11). refs/heads/main.
COMMIT="e2f098935447ca4945946de5cb69db843fe3f003"
SUBDIR="media/module/codecs/mp3dec"
URL="https://android.googlesource.com/platform/frameworks/av/+archive/${COMMIT}/${SUBDIR}.tar.gz"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ -n "${PVMP3_TARBALL:-}" ]; then
    echo "==> Using cached tarball $PVMP3_TARBALL"
    cp "$PVMP3_TARBALL" "$WORK/mp3dec.tar.gz"
else
    echo "==> Fetching $URL"
    curl -sfL -o "$WORK/mp3dec.tar.gz" "$URL"
fi

mkdir -p "$WORK/raw"
tar xzf "$WORK/mp3dec.tar.gz" -C "$WORK/raw"

# The gitiles +archive tarball is generated on demand, so its gzip bytes are
# not stable. The extracted FILES are; that is what the manifest pins.
echo "==> Verifying the extracted tree against SHA256SUMS.upstream"
( cd "$WORK/raw" && sha256sum -c --quiet "$DIR/SHA256SUMS.upstream" )

echo "==> Laying out upstream/ (src + include flattened, .cpp -> .c)"
rm -rf "$DIR/upstream"
mkdir -p "$DIR/upstream/asm"
cp "$WORK"/raw/src/*.h "$WORK"/raw/include/*.h "$DIR/upstream/"
for f in "$WORK"/raw/src/*.cpp; do
    cp "$f" "$DIR/upstream/$(basename "${f%.cpp}").c"
done
cp "$WORK"/raw/src/asm/*.s "$DIR/upstream/asm/"
# test/, fuzzer/, Android.bp and TEST_MAPPING are Android build/test scaffolding
# we neither compile nor ship.

echo "==> Applying patches/"
for p in "$DIR"/patches/*.patch; do
    echo "    $(basename "$p")"
    ( cd "$DIR/upstream" && patch -p1 --no-backup-if-mismatch -s < "$p" )
done

echo "==> Licence files"
cp "$WORK/raw/NOTICE" "$DIR/LICENSE"
cp "$WORK/raw/patent_disclaimer.txt" "$DIR/patent_disclaimer.txt"

echo
echo "pvmp3 vendored at $COMMIT."
echo "To re-pin after a COMMIT bump, regenerate the manifest with:"
echo "    ( cd <extracted tarball> && find . -type f | sort | xargs sha256sum ) \\"
echo "        > $DIR/SHA256SUMS.upstream"
echo "Then re-run the host suites; see README.md."
