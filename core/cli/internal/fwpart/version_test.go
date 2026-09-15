package fwpart

import (
	"bytes"
	"strings"
	"testing"
)

// marker builds the exact bytes the firmware emits: the tag, the two
// halves separated by "|", and the NUL the C string literal ends with.
func marker(version, build string) []byte {
	return append([]byte(VersionTag+version+"|"+build), 0)
}

// bodyAround buries a marker in something body-shaped, because the real
// one is somewhere in .rodata with megabytes of code in front of it and
// a scan that only works at offset 0 would pass a test and fail a
// device.
func bodyAround(mark []byte) []byte {
	var b bytes.Buffer
	b.Write(bytes.Repeat([]byte{0xEA, 0x00, 0x00, 0x0E}, 512))
	b.WriteString("Now Playing\x00Settings\x00")
	b.Write(mark)
	b.WriteString("v0.1.2\x00about\x00")
	b.Write(bytes.Repeat([]byte{0}, 64))
	return b.Bytes()
}

func TestFindVersionFindsTheMarker(t *testing.T) {
	body := bodyAround(marker("v0.1.3", "v0.1.3-4-gabc1234-dirty"))
	v, build, ok := FindVersion(body)
	if !ok {
		t.Fatal("FindVersion did not find a marker that is in the body")
	}
	if v != "v0.1.3" || build != "v0.1.3-4-gabc1234-dirty" {
		t.Errorf("FindVersion = %q, %q", v, build)
	}
	if n := CountVersionMarkers(body); n != 1 {
		t.Errorf("CountVersionMarkers = %d, want 1", n)
	}
	if got, want := VersionText(body), "v0.1.3 (build v0.1.3-4-gabc1234-dirty)"; got != want {
		t.Errorf("VersionText = %q, want %q", got, want)
	}
}

// The shape in 08-boot-dock.md, verbatim.
func TestFindVersionDocumentedExample(t *testing.T) {
	v, build, ok := FindVersion(marker("v0.1.2", "v0.1.2-dirty"))
	if !ok || v != "v0.1.2" || build != "v0.1.2-dirty" {
		t.Errorf("FindVersion = %q, %q, %v", v, build, ok)
	}
}

// A pre-v0.1.3 image is the device's own state until the next flash:
// no marker, and the bare "v0.1.2" that has always been in .rodata must
// not be mistaken for one.
func TestFindVersionNotFound(t *testing.T) {
	body := []byte("core\x00v0.1.2\x00CORE_VERSION\x00Settings\x00")
	if v, build, ok := FindVersion(body); ok {
		t.Errorf("FindVersion invented %q/%q in an unmarked image", v, build)
	}
	if n := CountVersionMarkers(body); n != 0 {
		t.Errorf("CountVersionMarkers = %d, want 0", n)
	}
	want := "unknown (no version marker; images before v0.1.3 carry none)"
	if got := VersionText(body); got != want {
		t.Errorf("VersionText = %q, want %q", got, want)
	}
}

func TestFindVersionMalformed(t *testing.T) {
	long := append([]byte(VersionTag), bytes.Repeat([]byte("x"), MaxVersionPayload+32)...)
	for _, tc := range []struct {
		name string
		body []byte
	}{
		{"no separator", append([]byte(VersionTag+"v0.1.3"), 0)},
		{"no terminator", []byte(VersionTag + "v0.1.3|v0.1.3-dirty")},
		{"empty version", append([]byte(VersionTag+"|v0.1.3-dirty"), 0)},
		{"empty build", append([]byte(VersionTag+"v0.1.3|"), 0)},
		{"empty payload", append([]byte(VersionTag), 0)},
		{"three fields", append([]byte(VersionTag+"v0.1.3|a|b"), 0)},
		{"control byte", append([]byte(VersionTag+"v0.1.3|a\nb"), 0)},
		{"runaway payload", long},
		{"tag truncated at the end of the body", []byte("code" + VersionTag)},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if v, build, ok := FindVersion(tc.body); ok {
				t.Errorf("FindVersion accepted a malformed marker as %q/%q", v, build)
			}
			if !strings.HasPrefix(VersionText(tc.body), "unknown") {
				t.Errorf("VersionText = %q, want an unknown line", VersionText(tc.body))
			}
		})
	}
}

// Two markers cannot come out of this tree's build (verify-hw counts
// them), so the rule is "report the first and say how many" rather than
// "refuse": a host that goes silent because the firmware has one string
// too many is worse than one that names the version and the oddity.
func TestFindVersionTwoMarkers(t *testing.T) {
	body := append(bodyAround(marker("v0.1.3", "v0.1.3")), marker("v0.9.9", "v0.9.9-dirty")...)
	v, build, ok := FindVersion(body)
	if !ok || v != "v0.1.3" || build != "v0.1.3" {
		t.Fatalf("FindVersion = %q, %q, %v; want the FIRST marker", v, build, ok)
	}
	if n := CountVersionMarkers(body); n != 2 {
		t.Fatalf("CountVersionMarkers = %d, want 2", n)
	}
	text := VersionText(body)
	if !strings.HasPrefix(text, "v0.1.3 (build v0.1.3;") || !strings.Contains(text, "2 version markers") {
		t.Errorf("VersionText = %q, want the first version plus a duplicate warning", text)
	}
}

// The scan must survive a marker at the very first byte and at the very
// last usable byte, because "somewhere in .rodata" includes both ends.
func TestFindVersionAtTheEdges(t *testing.T) {
	m := marker("v1.0.0", "v1.0.0")
	if _, _, ok := FindVersion(m); !ok {
		t.Error("a marker at offset 0 was not found")
	}
	if _, _, ok := FindVersion(append(bytes.Repeat([]byte{0xFF}, 4096), m...)); !ok {
		t.Error("a marker at the end of the body was not found")
	}
	if _, _, ok := FindVersion(nil); ok {
		t.Error("an empty body reported a version")
	}
}
