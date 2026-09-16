// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"encoding/binary"
	"hash/crc32"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

// The stamp writes into a file the DEVICE owns and the device is not running
// while we do it. That makes the invariants below the whole safety argument,
// and every one of them is a way a real iPod loses something:
//
//   - truncating or resizing CORECFG.DAT invalidates the cluster chain the
//     firmware resolved and can no longer repair;
//   - rewriting the slot we read from races the record the firmware is about
//     to load;
//   - rewriting any field but the host's two discards whatever the device put
//     there — a resume position, a queue, or a field a newer firmware added
//     that this build has never heard of.

// A CORECFG.DAT as EnsureConfig would leave it, plus a marker well past the
// two slots so a truncating write cannot hide.
func writeTestConfig(t *testing.T, dir string, slot0 []byte, slot1 []byte) string {
	t.Helper()
	blob := make([]byte, ConfigFileBytes)
	copy(blob, slot0)
	if slot1 != nil {
		copy(blob[ConfigSlotBytes:], slot1)
	}
	for i := 4096; i < 4096+16; i++ {
		blob[i] = 0xA5
	}
	path := filepath.Join(dir, ConfigName)
	if err := os.WriteFile(path, blob, 0o666); err != nil {
		t.Fatal(err)
	}
	return path
}

func mustStamp(t *testing.T, dir string, now time.Time) Stamped {
	t.Helper()
	s, err := StampConfigTime(dir, now)
	if err != nil {
		t.Fatalf("StampConfigTime: %v", err)
	}
	return s
}

func TestStampConfigTimeWritesTheOtherSlot(t *testing.T) {
	dir := t.TempDir()
	src := EncodeConfigSlot(DefaultSettings(), 7)
	path := writeTestConfig(t, dir, src[:], nil)

	zone := time.FixedZone("CEST", 2*3600)
	now := time.Unix(1789555320, 0).In(zone)
	got := mustStamp(t, dir, now)

	if got.Slot != 1 || got.Seq != 8 || got.OffMin != 120 {
		t.Fatalf("stamped slot %d seq %d off %d, want 1/8/120", got.Slot, got.Seq, got.OffMin)
	}

	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != ConfigFileBytes {
		t.Fatalf("file is %d bytes, want %d — the stamp must never resize it", len(b), ConfigFileBytes)
	}
	if !equalBytes(b[:ConfigSlotBytes], src[:]) {
		t.Error("slot 0 changed; the stamp must leave the slot it read from alone")
	}
	for i := 4096; i < 4096+16; i++ {
		if b[i] != 0xA5 {
			t.Fatalf("byte %d past the slots is %#02x: the file was truncated or rewritten", i, b[i])
		}
	}

	ts, ok := DecodeConfigTime(b[ConfigSlotBytes : 2*ConfigSlotBytes])
	if !ok {
		t.Fatal("the stamped slot does not decode")
	}
	if ts.HostEpoch != 1789555320 || ts.HostOffMin != 120 {
		t.Errorf("stamp reads back as %d/%d, want 1789555320/120", ts.HostEpoch, ts.HostOffMin)
	}
	if !ts.Pending() {
		t.Error("a fresh stamp must read as pending until the device boots")
	}
	seq, _, valid := DecodeConfigSlot(b[ConfigSlotBytes : 2*ConfigSlotBytes])
	if !valid || seq != 8 {
		t.Errorf("stamped slot: valid=%v seq=%d, want true/8", valid, seq)
	}
}

// Everything the DEVICE owns survives a stamp: the resume locator, the queue
// context, the sound tail, and the firmware's own half of the time block.
func TestStampConfigTimePreservesTheDevicesBytes(t *testing.T) {
	dir := t.TempDir()

	// Start from a record with every tail non-zero, built by hand so this test
	// does not depend on the encoder growing a way to write them.
	rec := EncodeConfigSlot(DefaultSettings(), 4)
	p := rec[cfgOffPayload:]
	binary.LittleEndian.PutUint16(rec[cfgOffLength:], cfgPayloadTime)
	binary.LittleEndian.PutUint32(p[12:], 0xDEADBEEF) // resume_hash
	binary.LittleEndian.PutUint32(p[16:], 1234)       // resume_secs
	binary.LittleEndian.PutUint32(p[20:], 5678)       // resume_total
	p[24] = 6                                         // resume_kind = playlist
	binary.LittleEndian.PutUint16(p[26:], 5999)       // resume_qidx
	binary.LittleEndian.PutUint32(p[28:], 0xC0FFEE01) // resume_seed
	p[pVolumeLimit] = 80
	p[pEQ] = 3
	p[pTimeFlags] = 0x03                                          // the device's rows
	binary.LittleEndian.PutUint32(p[pAppliedEpoch:], 42)          // the device's mark
	binary.LittleEndian.PutUint16(p[pUTCOffMin:], uint16(0xFE20)) // -480
	binary.LittleEndian.PutUint32(rec[cfgOffCRC:], crc32.ChecksumIEEE(rec[:cfgOffCRC]))

	path := writeTestConfig(t, dir, rec[:], nil)
	mustStamp(t, dir, time.Unix(1789555320, 0).UTC())

	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	out := b[ConfigSlotBytes : 2*ConfigSlotBytes]
	seq, s, ok := DecodeConfigSlot(out)
	if !ok {
		t.Fatal("the stamped slot does not decode")
	}
	if seq != 5 {
		t.Errorf("seq %d, want 5", seq)
	}
	q := out[cfgOffPayload:]
	if binary.LittleEndian.Uint32(q[12:]) != 0xDEADBEEF ||
		binary.LittleEndian.Uint32(q[16:]) != 1234 ||
		binary.LittleEndian.Uint32(q[20:]) != 5678 ||
		q[24] != 6 || binary.LittleEndian.Uint16(q[26:]) != 5999 ||
		binary.LittleEndian.Uint32(q[28:]) != 0xC0FFEE01 {
		t.Error("the resume locator / queue context did not survive the stamp")
	}
	if s.VolumeLimit != 80 || s.EQ != 3 {
		t.Errorf("sound tail came back as %d/%d, want 80/3", s.VolumeLimit, s.EQ)
	}
	ts, _ := DecodeConfigTime(out)
	if ts.AppliedEpoch != 42 || ts.UTCOffMin != -480 || !ts.Use24H || !ts.TimeInTitle {
		t.Errorf("the firmware's half of the time block was rewritten: %+v", ts)
	}
	if ts.HostEpoch != 1789555320 {
		t.Errorf("host epoch %d, want 1789555320", ts.HostEpoch)
	}
}

// A record from before the clock existed grows to 64 bytes, and the fields the
// device owns come up ZERO rather than out of the padding.
func TestStampConfigTimeGrowsAShortRecord(t *testing.T) {
	dir := t.TempDir()
	src := EncodeConfigSlot(DefaultSettings(), 1) // length 48
	if got := binary.LittleEndian.Uint16(src[cfgOffLength:]); got != cfgPayloadLen {
		t.Fatalf("EncodeConfigSlot now writes length %d: the golden moved", got)
	}
	path := writeTestConfig(t, dir, src[:], nil)
	mustStamp(t, dir, time.Unix(1789555320, 0).UTC())

	b, _ := os.ReadFile(path)
	out := b[ConfigSlotBytes : 2*ConfigSlotBytes]
	if got := binary.LittleEndian.Uint16(out[cfgOffLength:]); got != cfgPayloadTime {
		t.Fatalf("length %d after the stamp, want %d", got, cfgPayloadTime)
	}
	ts, ok := DecodeConfigTime(out)
	if !ok {
		t.Fatal("the grown slot does not decode as a time block")
	}
	if ts.AppliedEpoch != 0 || ts.UTCOffMin != 0 || ts.Use24H || ts.TimeInTitle {
		t.Errorf("a grown record invented device state: %+v", ts)
	}
	if !ts.Pending() {
		t.Error("the grown record must read as a pending stamp")
	}
	// And the un-grown original is still what it was.
	if _, ok := DecodeConfigTime(b[:ConfigSlotBytes]); ok {
		t.Error("slot 0 should still be a 48-byte record with no time block")
	}
}

// Two stamps in a row (sync then eject) alternate slots and both leave a valid
// file — idempotence is not required, harmlessness is.
func TestStampConfigTimeTwice(t *testing.T) {
	dir := t.TempDir()
	src := EncodeConfigSlot(DefaultSettings(), 1)
	path := writeTestConfig(t, dir, src[:], nil)

	first := mustStamp(t, dir, time.Unix(1789555320, 0).UTC())
	second := mustStamp(t, dir, time.Unix(1789555380, 0).UTC())
	if first.Slot != 1 || second.Slot != 0 {
		t.Fatalf("stamps landed in slots %d and %d, want 1 then 0", first.Slot, second.Slot)
	}
	if second.Seq != first.Seq+1 {
		t.Fatalf("seq %d then %d", first.Seq, second.Seq)
	}
	b, _ := os.ReadFile(path)
	newest, ok := ConfigFileValid(b)
	if !ok || newest != second.Seq {
		t.Fatalf("newest valid seq %d (ok=%v), want %d", newest, ok, second.Seq)
	}
	ts, _ := DecodeConfigTime(b[:ConfigSlotBytes])
	if ts.HostEpoch != 1789555380 {
		t.Errorf("the newest slot carries %d, want the second stamp", ts.HostEpoch)
	}
}

func TestStampConfigTimeRefusals(t *testing.T) {
	dir := t.TempDir()
	if _, err := StampConfigTime(dir, time.Now()); err == nil {
		t.Error("stamping a volume with no CORECFG.DAT must fail, not create one")
	}

	// A file with no valid record: EnsureConfig's job, not the stamp's.
	blank := make([]byte, ConfigFileBytes)
	if err := os.WriteFile(filepath.Join(dir, ConfigName), blank, 0o666); err != nil {
		t.Fatal(err)
	}
	if _, err := StampConfigTime(dir, time.Now()); err == nil {
		t.Error("stamping a file with no valid record must fail")
	}

	// A device node (or anything that is not a directory) is skipped quietly:
	// `core eject /dev/sdb1` has no filesystem for us to patch.
	s, did, err := StampConfigTimeIfVolume(filepath.Join(dir, ConfigName), time.Now())
	if err != nil || did || s.Seq != 0 {
		t.Errorf("a non-directory target must be skipped, got %+v %v %v", s, did, err)
	}
}

func TestStampZoneOffsets(t *testing.T) {
	cases := []struct {
		name   string
		offSec int
		want   int16
	}{
		{"IST", 5*3600 + 30*60, 330},
		{"PST", -8 * 3600, -480},
		{"UTC", 0, 0},
		{"Kiritimati", 14 * 3600, 840},
		{"Baker Island", -12 * 3600, -720},
	}
	for _, c := range cases {
		dir := t.TempDir()
		src := EncodeConfigSlot(DefaultSettings(), 1)
		path := writeTestConfig(t, dir, src[:], nil)
		now := time.Unix(1789555320, 0).In(time.FixedZone(c.name, c.offSec))
		mustStamp(t, dir, now)
		b, _ := os.ReadFile(path)
		ts, ok := DecodeConfigTime(b[ConfigSlotBytes : 2*ConfigSlotBytes])
		if !ok || ts.HostOffMin != c.want {
			t.Errorf("%s: offset %d (ok=%v), want %d", c.name, ts.HostOffMin, ok, c.want)
		}
	}
}

// DecodeConfigTime on records that have no time block.
func TestDecodeConfigTimeAbsent(t *testing.T) {
	src := EncodeConfigSlot(DefaultSettings(), 1) // length 48
	if _, ok := DecodeConfigTime(src[:]); ok {
		t.Error("a 48-byte record must report no stamp")
	}
	var junk [ConfigSlotBytes]byte
	if _, ok := DecodeConfigTime(junk[:]); ok {
		t.Error("an invalid slot must report no stamp")
	}
}

func TestSeqNewerWraps(t *testing.T) {
	if !seqNewer(1, 0) || seqNewer(0, 1) || seqNewer(5, 5) {
		t.Error("seqNewer is not a strict ordering")
	}
	if !seqNewer(0, 0xFFFFFFFF) {
		t.Error("seqNewer must read the wrap as one step forward, like config_seq_newer()")
	}
}

// PARITY. tools/make_config.py --stamp is the reference implementation; if the
// two ever disagree, a device stamped by one and read by the other is a bug
// nobody sees until a user's clock is wrong.
func TestStampMatchesMakeConfig(t *testing.T) {
	repo := repoRoot(t)
	if repo == "" {
		t.Skip("not inside the ipod_theme tree (set CORE_REPO)")
	}
	py := python3(t)
	if py == "" {
		t.Skip("python3 not on PATH")
	}

	const epoch = 1789555320
	const offMin = 330

	goDir := t.TempDir()
	pyDir := t.TempDir()
	src := EncodeConfigSlot(DefaultSettings(), 3)
	goPath := writeTestConfig(t, goDir, src[:], nil)
	pyPath := writeTestConfig(t, pyDir, src[:], nil)

	mustStamp(t, goDir, time.Unix(epoch, 0).In(time.FixedZone("IST", offMin*60)))

	cmd := exec.Command(py, filepath.Join(repo, "tools", "make_config.py"),
		"--stamp", pyDir,
		"--epoch", strconv.Itoa(epoch),
		"--utc-offset", strconv.Itoa(offMin))
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("make_config.py --stamp: %v\n%s", err, b)
	}

	goBytes, err := os.ReadFile(goPath)
	if err != nil {
		t.Fatal(err)
	}
	pyBytes, err := os.ReadFile(pyPath)
	if err != nil {
		t.Fatal(err)
	}
	if !equalBytes(goBytes, pyBytes) {
		for i := range goBytes {
			if goBytes[i] != pyBytes[i] {
				t.Fatalf("go and make_config.py --stamp differ at byte %d: %#02x vs %#02x",
					i, goBytes[i], pyBytes[i])
			}
		}
	}
}
