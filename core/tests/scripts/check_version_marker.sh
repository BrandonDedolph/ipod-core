#!/usr/bin/env sh
# check_version_marker.sh — the host-findable firmware version stamp.
#
# kernel/main.c defines `core_version_marker`, a string of the form
#
#     CORE-FW-VERSION:<tag>|<build id>
#
# that exists for ONE reader: the host. `core info` / `core update` scan the
# OSOS body pulled off the device for "CORE-FW-VERSION:" and report the version
# of the firmware actually running there (docs/design/companion-app-plan.md,
# S8). Nothing in the firmware reads it, nothing prints it — which is exactly
# why it needs a test: an unreferenced string in a tree built with
# -fdata-sections + -Wl,--gc-sections is one linker-script edit away from
# vanishing silently, and the failure would only show up as "unknown version"
# on a device weeks later. Measured: without boot/linker.ld's KEEP on
# .rodata.core_version the string is absent from core.bin entirely.
#
# Two assertions:
#   1. EXACTLY ONE marker in the flat image. Zero = the linker collected it.
#      More than one = a second definition crept in and the host would have to
#      guess which is the running version.
#   2. The <tag> half equals `git describe --tags --abbrev=0` — the same
#      command meson's vcs_tag runs for CORE_VERSION — so a stale build
#      directory cannot ship an image that claims the wrong release. Skipped
#      (not failed) outside a git checkout, in a tree with no tags, or without
#      git, which is how a source tarball and a fresh clone-without-tags build.
#
# The <build id> half is NOT checked: it is `git describe --tags --always
# --dirty --abbrev=7` and carries "-dirty" whenever the tree has uncommitted
# changes. Device images are routinely built that way on purpose, so a -dirty
# build id is expected, not an error.
#
# Usage: check_version_marker.sh <core.bin>
# Exits non-zero on a failed assertion.

set -eu

BIN="${1:?usage: check_version_marker.sh <core.bin>}"

[ -f "$BIN" ] || { printf 'check_version_marker: no such file: %s\n' "$BIN" >&2; exit 1; }

TAGPFX='CORE-FW-VERSION:'

# `strings` breaks records on non-printable bytes and boot/linker.ld puts a zero
# guard word in front of the marker, so the marker starts its own record and the
# anchor is meaningful: it keeps this from matching a mention of the tag inside
# some longer blob of printable bytes.
markers=$(strings -a "$BIN" | grep "^$TAGPFX" || true)
count=$(printf '%s' "$markers" | grep -c . || true)

if [ "$count" -ne 1 ]; then
    printf 'FAIL: expected exactly 1 "%s" marker in %s, found %d\n' \
           "$TAGPFX" "$BIN" "$count" >&2
    if [ "$count" -eq 0 ]; then
        printf '  (dropped by --gc-sections? see boot/linker.ld KEEP(*(.rodata.core_version)))\n' >&2
    else
        printf '%s\n' "$markers" | sed 's/^/  /' >&2
    fi
    exit 1
fi

marker=$markers
rest=${marker#"$TAGPFX"}
version=${rest%%|*}
build=${rest#*|}

if [ -z "$version" ] || [ "$rest" = "$build" ]; then
    printf 'FAIL: malformed marker %s (want %s<tag>|<build id>)\n' "$marker" "$TAGPFX" >&2
    exit 1
fi

expected=''
if command -v git >/dev/null 2>&1; then
    expected=$(git -C "$(dirname -- "$0")" describe --tags --abbrev=0 2>/dev/null || true)
fi

if [ -z "$expected" ]; then
    printf 'OK: one version marker, %s (build %s); tag check skipped (no git tag here)\n' \
           "$version" "$build"
    exit 0
fi

if [ "$version" != "$expected" ]; then
    printf 'FAIL: marker says version %s, `git describe --tags --abbrev=0` says %s\n' \
           "$version" "$expected" >&2
    printf '  stale build directory? re-run `ninja -C build-hw` so vcs_tag re-stamps CORE_VERSION\n' >&2
    exit 1
fi

printf 'OK: one version marker, %s (build %s), matches the nearest tag\n' "$version" "$build"
