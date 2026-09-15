#!/usr/bin/env bash
#
# build-all.sh — cross-compile the `core` host CLI for every platform we ship.
#
#     core/cli/scripts/build-all.sh [OUTDIR]        # default: core/cli/dist
#
# WHY THIS IS A SCRIPT AND NOT TWO COPIES OF THE SAME YAML. Both .github/
# workflows/ci.yml (every push, artifacts only) and .github/workflows/
# release.yml (tag push, uploaded to the GitHub release) build these five
# binaries with the same -ldflags stamp. If the flags lived in the workflows,
# the release binaries would eventually report a different version string than
# the CI ones and nobody would notice until a user pasted `core --version`
# into a bug report. One script, called from both, cannot drift.
#
# Not shipped here: core.ipod. CI's pinned gcc 14 would produce a different
# firmware binary than the gcc 16 image actually verified on the device, so
# the .ipod on a release stays the locally built, locally flashed one.
#
# CGO_ENABLED=0 for every `core` CLI target: the CLI has no cgo, and a
# static binary is what makes "download one file and run it" true on
# every target.
#
# core-app (the Gio GUI, S10) is the exception and the reason this script
# has a "skipped" list. Gio's Windows backend is pure Go, so
# core-app-windows-amd64.exe cross-builds from anywhere with
# CGO_ENABLED=0 and -H windowsgui (no console flashes on launch). Its X11
# and Cocoa backends are cgo, so core-app-linux-* and core-app-darwin-*
# can only be built where a C toolchain and that platform's headers are:
# the linux ones on a Linux runner with the GUI -dev packages, the darwin
# ones on macOS. This script builds what the host can and PRINTS WHAT IT
# SKIPPED rather than failing, because a developer on one machine can
# never build all ten and a release job that silently published six
# binaries would be worse than one that says which four are missing.
#
# On Linux the GUI build also needs -tags novulkan unless vulkan/vulkan.h
# is installed; CORE_APP_TAGS overrides the tag list.

set -euo pipefail

cd "$(dirname "$0")/.."          # core/cli — the Go module root
MODULE="$(go list -m)"
OUT="${1:-dist}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# The stamp. `git describe` gives the tag when we are on one (release.yml
# always is) and <tag>-<n>-g<sha>[-dirty] otherwise, so a CI artifact says out
# loud how far past the last release it is. Without git at all — a source
# tarball — the strings stay at the package defaults.
if git rev-parse --git-dir >/dev/null 2>&1; then
  VERSION="${CORE_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo '(devel)')}"
  COMMIT="${CORE_COMMIT:-$(git rev-parse HEAD)}"
else
  VERSION="${CORE_VERSION:-(devel)}"
  COMMIT="${CORE_COMMIT:-unknown}"
fi
# SOURCE_DATE_EPOCH, when set, keeps the stamp reproducible.
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
  DATE="$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null \
        || date -u -r "$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)"
else
  DATE="${CORE_DATE:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
fi

LDFLAGS="-s -w"
LDFLAGS="$LDFLAGS -X ${MODULE}/internal/version.Version=${VERSION}"
LDFLAGS="$LDFLAGS -X ${MODULE}/internal/version.Commit=${COMMIT}"
LDFLAGS="$LDFLAGS -X ${MODULE}/internal/version.Date=${DATE}"

echo "core cross-build (CLI + GUI)"
echo "  module  $MODULE"
echo "  version $VERSION"
echo "  commit  $COMMIT"
echo "  date    $DATE"
echo "  out     $OUT"

# name                    GOOS     GOARCH
TARGETS="
core-linux-amd64          linux    amd64
core-linux-arm64          linux    arm64
core-darwin-amd64         darwin   amd64
core-darwin-arm64         darwin   arm64
core-windows-amd64.exe    windows  amd64
"

# A here-string, not a pipe: a pipeline would run the loop in a subshell and
# `set -e` there is easy to get wrong — a failed cross-build has to fail the
# script, not just skip a file and let the upload step publish four binaries.
while read -r name goos goarch; do
  [ -n "$name" ] || continue
  echo "  -> $name"
  CGO_ENABLED=0 GOOS="$goos" GOARCH="$goarch" \
    go build -trimpath -ldflags "$LDFLAGS" -o "$OUT/$name" ./cmd/core
done <<< "$TARGETS"

# ---------------------------------------------------------------------
# core-app, the desktop GUI.
# ---------------------------------------------------------------------
HOST_OS="$(go env GOHOSTOS)"
HOST_ARCH="$(go env GOHOSTARCH)"
# Tag list for a cgo GUI build. Empty on darwin; novulkan on Linux unless
# the caller says otherwise, because Gio compiles a Vulkan path there and
# vulkan/vulkan.h is not installed on every machine that can run the app
# perfectly well through OpenGL ES.
if [ "$HOST_OS" = "linux" ]; then
  APP_TAGS="${CORE_APP_TAGS-novulkan}"
else
  APP_TAGS="${CORE_APP_TAGS-}"
fi
TAGFLAG=""
[ -n "$APP_TAGS" ] && TAGFLAG="-tags $APP_TAGS"

SKIPPED=""
echo
echo "core-app GUI builds (host $HOST_OS/$HOST_ARCH)"

# Windows: pure Go backend, so this one cross-builds from anywhere.
# -H windowsgui is what stops a console window flashing up on launch.
echo "  -> core-app-windows-amd64.exe"
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 \
  go build -trimpath -ldflags "$LDFLAGS -H windowsgui" \
    -o "$OUT/core-app-windows-amd64.exe" ./cmd/core-app

# name                       GOOS     GOARCH
APP_TARGETS="
core-app-linux-amd64         linux    amd64
core-app-linux-arm64         linux    arm64
core-app-darwin-amd64        darwin   amd64
core-app-darwin-arm64        darwin   arm64
"

while read -r name goos goarch; do
  [ -n "$name" ] || continue
  # cgo cannot cross-compile without a cross C toolchain, and neither
  # X11 nor Cocoa headers exist off their own platform. Same GOOS and
  # same GOARCH, or it is not this machine's job.
  if [ "$goos" != "$HOST_OS" ] || [ "$goarch" != "$HOST_ARCH" ]; then
    SKIPPED="$SKIPPED $name"
    continue
  fi
  echo "  -> $name ($TAGFLAG)"
  # shellcheck disable=SC2086
  CGO_ENABLED=1 GOOS="$goos" GOARCH="$goarch" \
    go build -trimpath $TAGFLAG -ldflags "$LDFLAGS" -o "$OUT/$name" ./cmd/core-app
done <<< "$APP_TARGETS"

echo
if [ -n "$SKIPPED" ]; then
  echo "SKIPPED (cgo: build these on their own runner —"
  echo "         release.yml has a linux job and a macos-latest job for exactly this):"
  for s in $SKIPPED; do echo "  - $s"; done
else
  echo "nothing skipped: every core-app target this host can build was built"
fi

echo
ls -l "$OUT"
