// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"hash/crc32"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The golden slot 0: a DefaultSettings record at seq 1, as
// tools/make_config.py writes it. Generated ONCE with
//
//	python3 tools/make_config.py --emit /tmp/cfg.bin
//	python3 -c "b=open('/tmp/cfg.bin','rb').read()[:1024]; \
//	           print(b[:60].hex(), b[1020:].hex())"
//
// on 2026-09-16 from the tree at e891cd4. The slot is 1024 bytes of which
// only the 60-byte record head and the 4-byte CRC tail are non-zero, so the
// golden is spelled as head ++ zeros ++ crc rather than as 2048 characters
// of mostly "00" — same bytes, readable diff.
//
// Head, field by field:
//
//	434f5245  magic 'CORE'          3000      length 48
//	0200      version 2             01000000  seq 1
//	00 00 01 00 46 00 00 00 0f 20 00 01       the v1 payload:
//	shuffle 0, repeat 0, resume 1, crossfade 0, volume 70, bass 0,
//	treble 0, balance 0, backlight 15 s, brightness 32, theme 0, clicker 1
//	then 32 zero bytes: the resume locator and queue context of a fresh file
//	64 00 00 00                               the sound tail:
//	volume limit 100 (no limit), EQ 0 (Off), two reserved bytes
const (
	configGoldenHead = "434f5245020030000100000000000100460000000f200001000000000000000000000000000000000000000000000000000000000000000064000000"
	configGoldenCRC  = "033929d4"
)

func configGolden() string {
	return configGoldenHead + strings.Repeat("00", cfgOffCRC-60) + configGoldenCRC
}

func TestEncodeConfigSlotMatchesGolden(t *testing.T) {
	if len(configGoldenHead) != 2*60 {
		t.Fatalf("golden head is %d hex chars, want %d", len(configGoldenHead), 2*60)
	}
	slot := EncodeConfigSlot(DefaultSettings(), 1)
	if got, want := hex.EncodeToString(slot[:]), configGolden(); got != want {
		t.Errorf("slot 0 mismatch\n got %s\nwant %s", firstDiff(got, want), firstDiff(want, got))
	}
}

// firstDiff trims a 2048-character hex string to the neighbourhood of the
// first difference, so a failure reads as bytes rather than as a wall.
func firstDiff(a, b string) string {
	for i := 0; i+2 <= len(a) && i+2 <= len(b); i += 2 {
		if a[i:i+2] != b[i:i+2] {
			lo, hi := i-8, i+16
			if lo < 0 {
				lo = 0
			}
			if hi > len(a) {
				hi = len(a)
			}
			return fmt.Sprintf("%s (at byte %d)", a[lo:hi], i/2)
		}
	}
	if len(a) != len(b) {
		return fmt.Sprintf("%.32s... (%d bytes)", a, len(a)/2)
	}
	return fmt.Sprintf("%.32s...", a)
}

// TestEncodeConfigSlotMatchesMakeConfig re-runs the reference implementation
// and compares fresh. The golden above is what CI checks without python3;
// this is what catches make_config.py and this package drifting apart.
func TestEncodeConfigSlotMatchesMakeConfig(t *testing.T) {
	repo := repoRoot(t)
	if repo == "" {
		t.Skip("not inside the ipod_theme tree (set CORE_REPO)")
	}
	py := python3(t)
	if py == "" {
		t.Skip("python3 not on PATH")
	}
	out := filepath.Join(t.TempDir(), "cfg.bin")
	cmd := exec.Command(py, filepath.Join(repo, "tools", "make_config.py"), "--emit", out)
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("make_config.py --emit: %v\n%s", err, b)
	}
	b, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != ConfigMinBytes {
		t.Fatalf("--emit wrote %d bytes, want %d", len(b), ConfigMinBytes)
	}
	slot := EncodeConfigSlot(DefaultSettings(), 1)
	if !equalBytes(slot[:], b[:ConfigSlotBytes]) {
		t.Errorf("slot 0 differs from make_config.py --emit\n go   %x\n py   %x",
			slot[:64], b[:64])
	}
	for i, v := range b[ConfigSlotBytes:] {
		if v != 0 {
			t.Fatalf("slot 1 byte %d is %#02x, want 0 (the device's first save lands there)", i, v)
		}
	}
	// The golden itself, against a freshly generated file.
	if got, want := hex.EncodeToString(b[:ConfigSlotBytes]), configGolden(); got != want {
		t.Errorf("the embedded golden is stale: make_config.py now emits %s", firstDiff(got, want))
	}
}

func equalBytes(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func TestDecodeConfigSlotRoundTrip(t *testing.T) {
	want := Settings{
		Shuffle: 1, Repeat: 2, ResumeOnStartup: 1, Crossfade: 1, Volume: 100,
		Bass: -12, Treble: 12, Balance: -100,
		BacklightSecs: 30, BacklightBright: 1, Theme: 5, Clicker: 3,
		VolumeLimit: 40, EQ: 12,
	}
	rec := EncodeConfigSlot(want, 0xDEADBEEF)
	seq, got, ok := DecodeConfigSlot(rec[:])
	if !ok {
		t.Fatal("a slot this package just encoded did not decode")
	}
	if seq != 0xDEADBEEF {
		t.Errorf("seq = %#x, want 0xdeadbeef", seq)
	}
	if got != want {
		t.Errorf("settings round trip: got %+v, want %+v", got, want)
	}
}

// A v1 record — length 12, no resume locator — is still a record. The
// firmware gates the v2 tail on the record's own length, not on the version
// (core/kernel/config.c:404-413), so a file written by an older host must
// keep decoding rather than reading three 32-bit fields out of the padding.
func TestDecodeConfigSlotAcceptsV1Length12(t *testing.T) {
	rec := EncodeConfigSlot(DefaultSettings(), 7)

	// Rewrite it as v1: length 12, and zero everything past the v1 payload
	// so the record is exactly what a v1 host would have written.
	binary.LittleEndian.PutUint16(rec[cfgOffLength:], cfgPayloadV1)
	for i := cfgOffPayload + cfgPayloadV1; i < cfgOffCRC; i++ {
		rec[i] = 0
	}
	binary.LittleEndian.PutUint32(rec[cfgOffCRC:], crc32.ChecksumIEEE(rec[:cfgOffCRC]))

	seq, s, ok := DecodeConfigSlot(rec[:])
	if !ok {
		t.Fatal("a v1 (length 12) record was rejected")
	}
	if seq != 7 {
		t.Errorf("seq = %d, want 7", seq)
	}
	if s != DefaultSettings() {
		t.Errorf("v1 settings = %+v, want %+v", s, DefaultSettings())
	}

	// And a v1 VERSION field with the same length, which is what actually
	// sits on a device that has never seen this build.
	binary.LittleEndian.PutUint16(rec[cfgOffVersion:], 1)
	binary.LittleEndian.PutUint32(rec[cfgOffCRC:], crc32.ChecksumIEEE(rec[:cfgOffCRC]))
	if _, _, ok := DecodeConfigSlot(rec[:]); !ok {
		t.Error("a version-1 record was rejected")
	}
}

// The 44-byte record: what every device in the field holds, written by the
// build before the sound tail existed. It must still decode, and it must
// report the values the FIRMWARE will use for the fields that are not there —
// no volume limit, EQ off — rather than a zero limit, which would read as a
// device pinned at the 10% floor.
func TestDecodeConfigSlotAcceptsQueueContextLength44(t *testing.T) {
	rec := EncodeConfigSlot(Settings{
		Volume: 90, VolumeLimit: 40, EQ: 12,
	}, 9)

	binary.LittleEndian.PutUint16(rec[cfgOffLength:], cfgPayloadV2Q)
	for i := cfgOffPayload + cfgPayloadV2Q; i < cfgOffCRC; i++ {
		rec[i] = 0
	}
	binary.LittleEndian.PutUint32(rec[cfgOffCRC:], crc32.ChecksumIEEE(rec[:cfgOffCRC]))

	seq, s, ok := DecodeConfigSlot(rec[:])
	if !ok {
		t.Fatal("a 44-byte (queue context) record was rejected")
	}
	if seq != 9 {
		t.Errorf("seq = %d, want 9", seq)
	}
	if s.Volume != 90 {
		t.Errorf("Volume = %d, want 90 (untouched by an absent sound tail)", s.Volume)
	}
	if s.VolumeLimit != 100 || s.EQ != 0 {
		t.Errorf("absent sound tail decoded as limit %d / EQ %d, want 100 / 0",
			s.VolumeLimit, s.EQ)
	}
}

func TestDecodeConfigSlotRejects(t *testing.T) {
	reseal := func(rec *[ConfigSlotBytes]byte) {
		binary.LittleEndian.PutUint32(rec[cfgOffCRC:], crc32.ChecksumIEEE(rec[:cfgOffCRC]))
	}
	cases := []struct {
		name string
		mut  func(rec *[ConfigSlotBytes]byte)
	}{
		{"bad magic", func(r *[ConfigSlotBytes]byte) { r[0] ^= 0xFF; reseal(r) }},
		{"version 0", func(r *[ConfigSlotBytes]byte) {
			binary.LittleEndian.PutUint16(r[cfgOffVersion:], 0)
			reseal(r)
		}},
		{"version from the future", func(r *[ConfigSlotBytes]byte) {
			binary.LittleEndian.PutUint16(r[cfgOffVersion:], ConfigVersion+1)
			reseal(r)
		}},
		{"length below v1", func(r *[ConfigSlotBytes]byte) {
			binary.LittleEndian.PutUint16(r[cfgOffLength:], cfgPayloadV1-1)
			reseal(r)
		}},
		{"length past the CRC", func(r *[ConfigSlotBytes]byte) {
			binary.LittleEndian.PutUint16(r[cfgOffLength:], cfgPayloadMax+1)
			reseal(r)
		}},
		{"torn payload", func(r *[ConfigSlotBytes]byte) { r[cfgOffPayload+pVolume] ^= 0x01 }},
		{"all zero", func(r *[ConfigSlotBytes]byte) { *r = [ConfigSlotBytes]byte{} }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rec := EncodeConfigSlot(DefaultSettings(), 1)
			tc.mut(&rec)
			if _, _, ok := DecodeConfigSlot(rec[:]); ok {
				t.Error("decoded a record the firmware would reject")
			}
		})
	}
	if _, _, ok := DecodeConfigSlot(make([]byte, ConfigSlotBytes-1)); ok {
		t.Error("decoded a short buffer")
	}
}
