package fwpart

import (
	"bytes"
	"strconv"
	"strings"
)

// The host-findable version stamp carried inside the OSOS body.
//
// core/kernel/main.c defines
//
//	const char core_version_marker[] __attribute__((used, section(".rodata.core_version"))) =
//	    "CORE-FW-VERSION:" CORE_VERSION "|" CORE_BUILD_ID "\0";
//
// and core/boot/linker.ld KEEPs that section, so every image from v0.1.3
// on carries exactly one NUL-terminated string of the shape
// `CORE-FW-VERSION:v0.1.2|v0.1.2-dirty` somewhere in .rodata. Nothing
// fixes its offset — it moves with every build — so reading the version
// is always a scan of the body, never a read at an address.
//
// Images before v0.1.3 have no marker at all. That is "unknown", not an
// error: the device this project actually runs is one of them until the
// next flash, and a health check that fails on it would be wrong.
const (
	// VersionTag is the string scanned for. It has to be distinctive
	// enough that `strings core.bin` cannot find it by accident, which
	// is why the bare "v0.1.2" that has always been in .rodata is not
	// what is read.
	VersionTag = "CORE-FW-VERSION:"

	// MaxVersionPayload bounds the bytes taken after the tag while
	// looking for the terminating NUL. A tag followed by a megabyte of
	// non-NUL bytes is not a marker that got longer, it is a tag-shaped
	// coincidence in a data table, and scanning to the end of a 7 MB
	// body to decide so would be the slow way to reach the same answer.
	MaxVersionPayload = 128
)

// FindVersion scans an OSOS image body for the version marker and
// splits it into the nearest git tag and the full build id.
//
// ok is false for an image with no marker (every image before v0.1.3)
// and for a marker that does not parse: no terminating NUL within
// MaxVersionPayload bytes, no "|" separator, an empty half, or a
// control byte in either half. The two failures are deliberately not
// distinguished in the return values — no caller can do anything
// different about them, and both print as "unknown".
//
// When a body somehow carries more than one marker the FIRST is
// returned, with ok true. That case cannot happen in an image this
// project builds (verify-hw asserts the count is 1) and the count is
// available from CountVersionMarkers for a caller that wants to say so.
func FindVersion(body []byte) (version, buildID string, ok bool) {
	i := bytes.Index(body, []byte(VersionTag))
	if i < 0 {
		return "", "", false
	}
	return parseVersionPayload(body[i+len(VersionTag):])
}

// CountVersionMarkers reports how many times the tag occurs in the
// body, parseable or not. One is the only healthy answer; zero means a
// pre-v0.1.3 image, and more than one means the linker kept a copy it
// should have folded, which is a firmware bug worth printing.
func CountVersionMarkers(body []byte) int {
	return bytes.Count(body, []byte(VersionTag))
}

// parseVersionPayload reads the bytes following the tag: up to the NUL,
// split once at "|".
func parseVersionPayload(rest []byte) (version, buildID string, ok bool) {
	window := rest
	if len(window) > MaxVersionPayload {
		window = window[:MaxVersionPayload]
	}
	end := bytes.IndexByte(window, 0)
	if end < 0 {
		return "", "", false
	}
	text := string(window[:end])
	bar := strings.IndexByte(text, '|')
	if bar < 0 {
		return "", "", false
	}
	version, buildID = text[:bar], text[bar+1:]
	if version == "" || buildID == "" {
		return "", "", false
	}
	// A second "|" means the string is not the two fields it claims to
	// be; so does any byte the C string could not have carried from a
	// git describe.
	if strings.ContainsRune(buildID, '|') || !printable(version) || !printable(buildID) {
		return "", "", false
	}
	return version, buildID, true
}

func printable(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] < 0x20 || s[i] == 0x7f {
			return false
		}
	}
	return true
}

// VersionText renders what `core info` and `core doctor` print for a
// body, so the two commands cannot drift on the wording. The sentence
// for an image with no marker names the version the marker landed in,
// because "unknown" on its own reads like a failure.
func VersionText(body []byte) string {
	v, build, ok := FindVersion(body)
	if !ok {
		return "unknown (no version marker; images before v0.1.3 carry none)"
	}
	if n := CountVersionMarkers(body); n > 1 {
		return v + " (build " + build + "; WARNING: " +
			strconv.Itoa(n) + " version markers in this image, reporting the first)"
	}
	return v + " (build " + build + ")"
}
