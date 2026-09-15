// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

// The contract both Ensure* functions carry: missing → created; valid →
// untouched, byte for byte and mtime for mtime; invalid → rewritten. The
// middle case is the one that matters on a real device — a re-sync that
// rewrote CORECFG.DAT would silently reset every setting the user has saved,
// and one that rewrote CORELOG.BIN would throw away the log that explains
// what the device did overnight.

func TestEnsureConfigCreatesAndIsIdempotent(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, ConfigName)

	created, err := EnsureConfig(root)
	if err != nil {
		t.Fatal(err)
	}
	if !created {
		t.Fatal("first EnsureConfig reported created=false")
	}

	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != ConfigFileBytes {
		t.Errorf("file is %d bytes, want %d", len(b), ConfigFileBytes)
	}

	// Slot 0 valid at seq 1 with the defaults; slot 1 zero so the device's
	// first save lands there and the alternation starts cleanly.
	seq, s, ok := DecodeConfigSlot(b[:ConfigSlotBytes])
	if !ok {
		t.Fatal("slot 0 does not decode")
	}
	if seq != 1 {
		t.Errorf("slot 0 seq = %d, want 1", seq)
	}
	if s != DefaultSettings() {
		t.Errorf("slot 0 settings = %+v, want %+v", s, DefaultSettings())
	}
	if _, _, ok := DecodeConfigSlot(b[ConfigSlotBytes : 2*ConfigSlotBytes]); ok {
		t.Error("slot 1 decodes; it must be empty on a fresh file")
	}
	for i, v := range b[2*ConfigSlotBytes:] {
		if v != 0 {
			t.Fatalf("padding byte %d is %#02x, want 0", i+2*ConfigSlotBytes, v)
		}
	}

	// Either slot alone must be enough to call the file valid: that is the
	// state the device leaves behind after every save.
	for _, slot := range []int{0, 1} {
		t.Run([]string{"slot0 only", "slot1 only"}[slot], func(t *testing.T) {
			dir := t.TempDir()
			blob := make([]byte, ConfigFileBytes)
			rec := EncodeConfigSlot(DefaultSettings(), uint32(9+slot))
			copy(blob[slot*ConfigSlotBytes:], rec[:])
			writeFixture(t, filepath.Join(dir, ConfigName), blob)
			before := stat(t, filepath.Join(dir, ConfigName))

			created, err := EnsureConfig(dir)
			if err != nil {
				t.Fatal(err)
			}
			if created {
				t.Error("rewrote a file that already held a valid record")
			}
			assertUntouched(t, filepath.Join(dir, ConfigName), before, blob)
		})
	}

	// Second run on the file we just created: untouched.
	before := stat(t, path)
	created, err = EnsureConfig(root)
	if err != nil {
		t.Fatal(err)
	}
	if created {
		t.Error("second EnsureConfig rewrote the file")
	}
	assertUntouched(t, path, before, b)
}

func TestEnsureConfigRewritesInvalid(t *testing.T) {
	cases := map[string][]byte{
		"all zero":  make([]byte, ConfigFileBytes),
		"too short": make([]byte, 512),
		// A valid slot 0 in a file the device refuses on size alone
		// (config.c:479 wants CONFIG_MIN_BYTES): must be rewritten, not kept.
		"valid slot, file under CONFIG_MIN_BYTES": func() []byte {
			rec := EncodeConfigSlot(DefaultSettings(), 7)
			return rec[:]
		}(),
		"garbage": []byte("this is not a CORECFG.DAT at all, not even nearly"),
		"torn crc": func() []byte {
			b := make([]byte, ConfigFileBytes)
			rec := EncodeConfigSlot(DefaultSettings(), 3)
			copy(b, rec[:])
			b[cfgOffPayload+pVolume] ^= 0x01 // CRC no longer covers it
			return b
		}(),
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			path := filepath.Join(dir, ConfigName)
			writeFixture(t, path, body)

			created, err := EnsureConfig(dir)
			if err != nil {
				t.Fatal(err)
			}
			if !created {
				t.Fatal("left an invalid file alone")
			}
			b, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			if len(b) != ConfigFileBytes {
				t.Fatalf("rewritten file is %d bytes, want %d", len(b), ConfigFileBytes)
			}
			if seq, ok := ConfigFileValid(b); !ok || seq != 1 {
				t.Fatalf("rewritten file: seq=%d ok=%v, want a valid seq 1", seq, ok)
			}
		})
	}
}

func TestEnsureLogCreatesAndIsIdempotent(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, LogName)

	created, err := EnsureLog(root, 0)
	if err != nil {
		t.Fatal(err)
	}
	if !created {
		t.Fatal("first EnsureLog reported created=false")
	}

	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(b) != LogDefaultBytes {
		t.Fatalf("file is %d bytes, want the 4 MiB default", len(b))
	}
	n, id, ok := DecodeLogHeader(b[:LogBlockBytes])
	if !ok {
		t.Fatal("the header does not decode")
	}
	if n != LogDefaultBytes/LogBlockBytes {
		t.Errorf("block count = %d, want %d", n, LogDefaultBytes/LogBlockBytes)
	}
	if id == 0 {
		t.Error("file id is 0; it should be random")
	}
	for i, v := range b[LogBlockBytes:] {
		if v != 0 {
			t.Fatalf("ring byte %d is %#02x — the ring must start zero", i, v)
		}
	}

	before := stat(t, path)
	created, err = EnsureLog(root, 0)
	if err != nil {
		t.Fatal(err)
	}
	if created {
		t.Error("second EnsureLog rewrote the file")
	}
	assertUntouched(t, path, before, b)
}

func TestEnsureLogRewritesInvalid(t *testing.T) {
	const size = 8 * LogBlockBytes

	mk := func(mut func([]byte)) []byte {
		b := make([]byte, size)
		h := EncodeLogHeader(size/LogBlockBytes, 0x11223344)
		copy(b, h[:])
		if mut != nil {
			mut(b)
		}
		return b
	}
	cases := map[string][]byte{
		"all zero":  make([]byte, size),
		"bad magic": mk(func(b []byte) { b[0] ^= 0xFF }),
		"bad crc":   mk(func(b []byte) { b[logOffCRC] ^= 0x01 }),
		// The header validates but claims a different length than the file
		// has — evlog_mount() refuses that, so the host must fix it.
		"count disagrees with size": func() []byte {
			b := make([]byte, size)
			h := EncodeLogHeader(size/LogBlockBytes+1, 0x55)
			copy(b, h[:])
			return b
		}(),
	}
	for name, body := range cases {
		t.Run(name, func(t *testing.T) {
			dir := t.TempDir()
			path := filepath.Join(dir, LogName)
			writeFixture(t, path, body)

			created, err := EnsureLog(dir, size)
			if err != nil {
				t.Fatal(err)
			}
			if !created {
				t.Fatal("left a log the device would refuse")
			}
			b, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			n, _, ok := DecodeLogHeader(b[:LogBlockBytes])
			if !ok || int(n)*LogBlockBytes != len(b) {
				t.Fatalf("rewritten log: count=%d ok=%v size=%d", n, ok, len(b))
			}
		})
	}
}

func TestEnsureLogRejectsBadSizes(t *testing.T) {
	dir := t.TempDir()
	for _, size := range []int64{1, LogBlockBytes + 1, LogBlockBytes, (LogMaxBlocks + 1) * LogBlockBytes} {
		if _, err := EnsureLog(dir, size); err == nil {
			t.Errorf("size %d was accepted", size)
		}
	}
	if _, err := os.Stat(filepath.Join(dir, LogName)); err == nil {
		t.Error("a rejected size still created a file")
	}
}

// ---- helpers ------------------------------------------------------------

func writeFixture(t *testing.T, path string, b []byte) {
	t.Helper()
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
	// Backdate it: "untouched" has to mean something even on a filesystem
	// whose mtime resolution is coarser than a test run.
	old := time.Now().Add(-2 * time.Hour)
	if err := os.Chtimes(path, old, old); err != nil {
		t.Fatal(err)
	}
}

func stat(t *testing.T, path string) os.FileInfo {
	t.Helper()
	fi, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	return fi
}

func assertUntouched(t *testing.T, path string, before os.FileInfo, want []byte) {
	t.Helper()
	after := stat(t, path)
	if !after.ModTime().Equal(before.ModTime()) {
		t.Errorf("%s mtime moved %v -> %v", filepath.Base(path),
			before.ModTime(), after.ModTime())
	}
	if after.Size() != before.Size() {
		t.Errorf("%s size %d -> %d", filepath.Base(path), before.Size(), after.Size())
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !equalBytes(got, want) {
		t.Errorf("%s contents changed", filepath.Base(path))
	}
}
