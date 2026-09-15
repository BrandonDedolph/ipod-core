package disk

import (
	"bytes"
	"errors"
	"io"
	"math/rand"
	"testing"
)

// pattern fills n bytes with something where every byte is a function
// of its own offset, so a misplaced copy shows up as a wrong VALUE and
// not just a wrong length.
func pattern(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i*7 + i/251)
	}
	return b
}

func newFake(t *testing.T, size, sectorSize int, write bool) (Handle, *MemStats, []byte) {
	t.Helper()
	want := pattern(size)
	backing := append([]byte(nil), want...)
	h, stats, err := NewMemDevice(backing, sectorSize, write)
	if err != nil {
		t.Fatalf("NewMemDevice: %v", err)
	}
	t.Cleanup(func() { h.Close() })
	return h, stats, want
}

// TestAlignedReadAtOddOffsets is the whole point of the bounce buffer:
// the device can only be read in whole 512-byte sectors, and callers
// (fwpart reading a 40-byte directory row at 0x4200, the MBR parser
// reading 512 bytes at 0) must not have to know that.
func TestAlignedReadAtOddOffsets(t *testing.T) {
	const size, ss = 8192, 512
	h, _, want := newFake(t, size, ss, false)

	cases := []struct{ off, n int }{
		{0, 1}, {0, 512}, {0, 8192},
		{1, 1}, {1, 511}, {1, 512}, {1, 513},
		{511, 2},     // straddles the first sector boundary
		{511, 1026},  // straddles three sectors
		{0x100, 4},   // the "]ih[" marker read
		{1000, 1500}, // unaligned start and end
		{7679, 513},  // ends exactly at the device end
		{8191, 1},    // last byte
	}
	for _, c := range cases {
		got := make([]byte, c.n)
		n, err := h.ReadAt(got, int64(c.off))
		if err != nil {
			t.Errorf("ReadAt(%d bytes at %d): %v", c.n, c.off, err)
			continue
		}
		if n != c.n {
			t.Errorf("ReadAt(%d bytes at %d) read %d bytes", c.n, c.off, n)
			continue
		}
		if !bytes.Equal(got, want[c.off:c.off+c.n]) {
			t.Errorf("ReadAt(%d bytes at %d) returned the wrong bytes", c.n, c.off)
		}
	}
}

// TestAlignedReadPastEOF pins the io.ReaderAt contract, which is the
// part everyone gets wrong: a read that runs off the end returns the
// bytes that WERE there together with io.EOF, and a read that starts at
// or past the end returns (0, io.EOF) and touches nothing.
func TestAlignedReadPastEOF(t *testing.T) {
	const size, ss = 4096, 512
	h, _, want := newFake(t, size, ss, false)

	buf := make([]byte, 100)
	n, err := h.ReadAt(buf, size-10)
	if n != 10 {
		t.Errorf("ReadAt 100 bytes at size-10 read %d bytes, want 10", n)
	}
	if !errors.Is(err, io.EOF) {
		t.Errorf("ReadAt past the end: err = %v, want io.EOF", err)
	}
	if !bytes.Equal(buf[:10], want[size-10:]) {
		t.Error("the 10 bytes before the end came back wrong")
	}

	if n, err := h.ReadAt(buf, size); n != 0 || !errors.Is(err, io.EOF) {
		t.Errorf("ReadAt at exactly the end = (%d, %v), want (0, io.EOF)", n, err)
	}
	if n, err := h.ReadAt(buf, size+4096); n != 0 || !errors.Is(err, io.EOF) {
		t.Errorf("ReadAt well past the end = (%d, %v), want (0, io.EOF)", n, err)
	}
	if _, err := h.ReadAt(buf, -1); err == nil {
		t.Error("ReadAt at a negative offset succeeded")
	}
}

// TestAlignedWriteSpansThreeSectors is the read-modify-write case that
// the firmware directory needs: a payload that starts inside one
// sector, covers a whole one, and ends inside a third. Everything
// outside the payload must be byte-identical afterwards — that is the
// promise that keeps the other three directory entries alive.
func TestAlignedWriteSpansThreeSectors(t *testing.T) {
	const size, ss = 8192, 512
	h, stats, want := newFake(t, size, ss, true)

	off := 512 + 40                // inside sector 1
	payload := pattern(1024 + 200) // ends inside sector 3
	for i := range payload {
		payload[i] ^= 0xA5
	}
	n, err := h.WriteAt(payload, int64(off))
	if err != nil {
		t.Fatalf("WriteAt: %v", err)
	}
	if n != len(payload) {
		t.Fatalf("WriteAt wrote %d of %d bytes", n, len(payload))
	}
	copy(want[off:], payload)
	if !bytes.Equal(stats.Bytes(), want) {
		t.Fatal("the device contents differ from the expected image after a 3-sector write")
	}

	got := make([]byte, len(payload))
	if _, err := h.ReadAt(got, int64(off)); err != nil {
		t.Fatalf("ReadAt after write: %v", err)
	}
	if !bytes.Equal(got, payload) {
		t.Error("reading back the payload at the same odd offset returned different bytes")
	}
}

// TestAlignedWriteRandomRoundTrip hammers the loop with random offsets
// and lengths against a plain byte slice as the oracle. A hand-picked
// table finds the cases the author thought of; this finds the rest.
func TestAlignedWriteRandomRoundTrip(t *testing.T) {
	const size, ss = 16384, 2048 // the sector size this project's bridge reports
	h, stats, want := newFake(t, size, ss, true)

	rng := rand.New(rand.NewSource(20260914))
	for i := 0; i < 400; i++ {
		off := rng.Intn(size)
		n := rng.Intn(size-off) + 1
		payload := make([]byte, n)
		rng.Read(payload)
		if _, err := h.WriteAt(payload, int64(off)); err != nil {
			t.Fatalf("iteration %d: WriteAt(%d bytes at %d): %v", i, n, off, err)
		}
		copy(want[off:], payload)
		if !bytes.Equal(stats.Bytes(), want) {
			t.Fatalf("iteration %d: device diverged after WriteAt(%d bytes at %d)", i, n, off)
		}
		got := make([]byte, n)
		if _, err := h.ReadAt(got, int64(off)); err != nil {
			t.Fatalf("iteration %d: ReadAt: %v", i, err)
		}
		if !bytes.Equal(got, payload) {
			t.Fatalf("iteration %d: ReadAt(%d bytes at %d) != what was written", i, n, off)
		}
	}
}

// TestAlignedChunking shrinks maxBounce so a small fake device still
// exercises the multi-window loop in both directions.
func TestAlignedChunking(t *testing.T) {
	old := maxBounce
	maxBounce = 1024
	t.Cleanup(func() { maxBounce = old })

	const size, ss = 8192, 512
	h, stats, want := newFake(t, size, ss, true)

	// A read far longer than one window, starting unaligned.
	got := make([]byte, 5000)
	if _, err := h.ReadAt(got, 37); err != nil {
		t.Fatalf("chunked ReadAt: %v", err)
	}
	if !bytes.Equal(got, want[37:37+5000]) {
		t.Error("chunked read returned the wrong bytes")
	}

	payload := bytes.Repeat([]byte{0x5A}, 5000)
	if _, err := h.WriteAt(payload, 37); err != nil {
		t.Fatalf("chunked WriteAt: %v", err)
	}
	copy(want[37:], payload)
	if !bytes.Equal(stats.Bytes(), want) {
		t.Error("chunked write produced the wrong device image")
	}
}

// TestAlignedWriteRefusals: a write past the end writes NOTHING (no
// prefix), and a read-only handle refuses outright.
func TestAlignedWriteRefusals(t *testing.T) {
	const size, ss = 4096, 512
	h, stats, want := newFake(t, size, ss, true)

	if _, err := h.WriteAt(bytes.Repeat([]byte{1}, 100), size-10); err == nil {
		t.Error("a write running past the end of the device succeeded")
	}
	if !bytes.Equal(stats.Bytes(), want) {
		t.Error("the refused write still changed the device")
	}
	if _, err := h.WriteAt([]byte{1}, -1); err == nil {
		t.Error("a write at a negative offset succeeded")
	}

	ro, _, _ := newFake(t, size, ss, false)
	if _, err := ro.WriteAt([]byte{1}, 0); !errors.Is(err, ErrReadOnly) {
		t.Errorf("WriteAt on a read-only handle = %v, want ErrReadOnly", err)
	}
	if err := ro.Flush(); err != nil {
		t.Errorf("Flush on a read-only handle = %v, want nil (no-op)", err)
	}
}

// TestAlignedRejectsBadGeometry: a sector size that is not a power of
// two makes every offset computation in this file wrong, so the handle
// must not be constructed at all.
func TestAlignedRejectsBadGeometry(t *testing.T) {
	for _, ss := range []int{0, -512, 3, 1000} {
		if _, _, err := NewMemDevice(make([]byte, 4096), ss, false); err == nil {
			t.Errorf("NewMemDevice accepted a sector size of %d", ss)
		}
	}
}

// TestAlignedRoundsSizeDown documents the odd-size rule: a device whose
// length is not a whole number of sectors keeps working, minus the tail
// that aligned I/O cannot address.
func TestAlignedRoundsSizeDown(t *testing.T) {
	h, _, err := NewMemDevice(make([]byte, 5000), 512, false)
	if err != nil {
		t.Fatalf("NewMemDevice: %v", err)
	}
	defer h.Close()
	if got := h.Size(); got != 4608 {
		t.Errorf("Size() = %d for a 5000-byte device with 512-byte sectors, want 4608", got)
	}
}

// TestAlignedLockFlushPassThrough proves the Handle wrapper forwards
// the lifecycle calls rather than swallowing them; the flash path's
// "backup, lock, write, flush, verify" sequence depends on every one of
// them reaching the device.
func TestAlignedLockFlushPassThrough(t *testing.T) {
	h, stats, _ := newFake(t, 4096, 512, true)
	if err := h.Lock(); err != nil {
		t.Fatalf("Lock: %v", err)
	}
	if !stats.Locked() {
		t.Error("Lock did not reach the device")
	}
	if err := h.Flush(); err != nil {
		t.Fatalf("Flush: %v", err)
	}
	if stats.Flushes() != 1 {
		t.Errorf("Flushes = %d, want 1", stats.Flushes())
	}
	if err := h.Unlock(); err != nil {
		t.Fatalf("Unlock: %v", err)
	}
	if stats.Locked() {
		t.Error("Unlock did not reach the device")
	}
}
