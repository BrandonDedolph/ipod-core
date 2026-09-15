package disk

import (
	"errors"
	"fmt"
	"io"
	"sync"
)

// rawDevice is what each OS file implements: a block device that can
// only be read and written in whole sectors, at sector-aligned offsets.
// Every alignment rule in the package lives in `aligned` below, so a
// per-OS file is just CreateFile/open plus two ioctls.
type rawDevice interface {
	// ReadSectors fills p from off. Both off and len(p) are
	// guaranteed to be multiples of SectorSize(), and the range is
	// guaranteed to be inside Size().
	ReadSectors(p []byte, off int64) error
	// WriteSectors writes p at off under the same guarantees.
	WriteSectors(p []byte, off int64) error
	SectorSize() int
	Size() int64
	Lock() error
	Unlock() error
	Flush() error
	Close() error
}

// maxBounce caps one bounce-buffer window. Big enough that a whole-
// partition backup (131 MB at 2048-byte sectors) is ~32 round trips
// rather than 64k of them, small enough that a handle does not quietly
// hold megabytes per open.
//
// A var, not a const, only so the tests can shrink it: the chunking
// loop is the part of this file most likely to be wrong, and a test
// that needs a 4 MB fake device to reach the second iteration is a test
// nobody runs.
var maxBounce int64 = 4 << 20

// aligned turns a sector-only rawDevice into a Handle that accepts any
// offset and any length.
//
// Reads at an unaligned offset read the enclosing sectors and copy out
// the middle. Writes that do not exactly cover whole sectors are
// read-modify-write: the enclosing sectors are read, patched in memory
// and written back whole. That is not an optimisation, it is the only
// thing the transport permits — a raw character device on macOS and a
// FILE_FLAG_NO_BUFFERING handle on Windows both reject a partial-sector
// write outright, and the firmware directory we have to amend is 40
// bytes inside a 2048-byte sector.
//
// The read-modify-write is also why fwpart.PlanWrite hands back a whole
// sector rather than 40 bytes: doing the RMW there, from the partition
// the caller already has open, keeps the "what will be written" that
// --dry-run prints identical to the bytes that reach the disk. This
// layer's RMW is the backstop for everything else.
type aligned struct {
	mu    sync.Mutex
	dev   rawDevice
	buf   []byte
	ss    int64
	size  int64
	write bool
}

func newAligned(dev rawDevice, write bool) (*aligned, error) {
	ss := dev.SectorSize()
	if ss <= 0 || ss&(ss-1) != 0 {
		return nil, fmt.Errorf("disk: device reports a sector size of %d, which is not a positive power of two", ss)
	}
	size := dev.Size()
	if size < 0 {
		return nil, fmt.Errorf("disk: device reports a size of %d bytes", size)
	}
	// A device whose size is not a whole number of sectors has a tail
	// that aligned I/O cannot address. Real disks are always exact
	// multiples; rounding down (rather than erroring) means a device
	// that is merely odd stays inspectable instead of unopenable.
	usable := size &^ (int64(ss) - 1)
	return &aligned{dev: dev, ss: int64(ss), size: usable, write: write}, nil
}

func (a *aligned) SectorSize() int { return int(a.ss) }
func (a *aligned) Size() int64     { return a.size }

func (a *aligned) Lock() error {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.dev.Lock()
}

func (a *aligned) Unlock() error {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.dev.Unlock()
}

func (a *aligned) Flush() error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.write {
		return nil
	}
	return a.dev.Flush()
}

func (a *aligned) Close() error {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.buf = nil
	return a.dev.Close()
}

// bounce returns a scratch slice of exactly n bytes. Callers hold
// a.mu, so one buffer per handle is enough.
func (a *aligned) bounce(n int) []byte {
	if cap(a.buf) < n {
		a.buf = make([]byte, n)
	}
	return a.buf[:n]
}

// alignDown / alignUp on the handle's sector size.
func (a *aligned) alignDown(off int64) int64 { return off &^ (a.ss - 1) }
func (a *aligned) alignUp(off int64) int64   { return (off + a.ss - 1) &^ (a.ss - 1) }

// ReadAt implements io.ReaderAt, including its awkward parts: a short
// read at the end of the device returns io.EOF alongside the bytes that
// were there, and a read starting at or past the end returns (0,
// io.EOF) with no I/O at all.
func (a *aligned) ReadAt(p []byte, off int64) (int, error) {
	if off < 0 {
		return 0, fmt.Errorf("disk: ReadAt at negative offset %d", off)
	}
	if len(p) == 0 {
		return 0, nil
	}
	if off >= a.size {
		return 0, io.EOF
	}
	var eof error
	if off+int64(len(p)) > a.size {
		p = p[:a.size-off]
		eof = io.EOF
	}

	a.mu.Lock()
	defer a.mu.Unlock()

	total := 0
	for len(p) > 0 {
		start := a.alignDown(off)
		skip := off - start
		window := a.alignUp(skip + int64(len(p)))
		if window > maxBounce {
			window = maxBounce
		}
		if start+window > a.size {
			window = a.size - start
		}
		buf := a.bounce(int(window))
		if err := a.dev.ReadSectors(buf, start); err != nil {
			return total, fmt.Errorf("disk: read %d bytes at %#x: %w", len(buf), start, err)
		}
		n := copy(p, buf[skip:])
		p = p[n:]
		off += int64(n)
		total += n
	}
	return total, eof
}

// WriteAt implements io.WriterAt. Unlike ReadAt it never writes a
// prefix: a write that would run past the end of the device is refused
// before any byte is written, because "we wrote most of your firmware"
// is not a recoverable outcome.
func (a *aligned) WriteAt(p []byte, off int64) (int, error) {
	if !a.write {
		return 0, ErrReadOnly
	}
	if off < 0 {
		return 0, fmt.Errorf("disk: WriteAt at negative offset %d", off)
	}
	if len(p) == 0 {
		return 0, nil
	}
	if off+int64(len(p)) > a.size {
		return 0, fmt.Errorf("disk: refusing to write %d bytes at %#x: the device holds %d bytes",
			len(p), off, a.size)
	}

	a.mu.Lock()
	defer a.mu.Unlock()

	total := 0
	for len(p) > 0 {
		start := a.alignDown(off)
		skip := off - start
		window := a.alignUp(skip + int64(len(p)))
		if window > maxBounce {
			window = maxBounce
		}
		if start+window > a.size {
			window = a.size - start
		}
		buf := a.bounce(int(window))

		n := int(window - skip)
		if n > len(p) {
			n = len(p)
		}
		// Read-modify-write whenever the payload does not cover the
		// window exactly. Skipping the read when it does is not an
		// optimisation either: on a 4 MiB window it avoids reading 4
		// MiB we are about to overwrite, which on this USB bridge is
		// most of the wall clock of a flash.
		if skip != 0 || int64(n) != window-skip {
			if err := a.dev.ReadSectors(buf, start); err != nil {
				return total, fmt.Errorf("disk: read-modify-write: read %d bytes at %#x: %w",
					len(buf), start, err)
			}
		}
		copy(buf[skip:], p[:n])
		if err := a.dev.WriteSectors(buf, start); err != nil {
			return total, fmt.Errorf("disk: write %d bytes at %#x: %w", len(buf), start, err)
		}
		p = p[n:]
		off += int64(n)
		total += n
	}
	return total, nil
}

// memDevice is an in-memory rawDevice. It lives in the non-test build
// because FindIPods' tests, the CLI's tests and (in S7) the flash
// sequence test all need one, and three copies of the same fake is how
// they drift apart.
type memDevice struct {
	data   []byte
	ss     int
	locked bool
	// Flushes counts Flush calls so a test can prove the flush
	// happened before the read-back.
	Flushes int
	// FailWriteAt, when >= 0, makes WriteSectors fail once the write
	// reaches that offset — the fault injection S7's verify-after-
	// failure test needs.
	FailWriteAt int64
	// ReadErr, when non-nil, makes every read fail: what a device the
	// user unplugged mid-command looks like.
	ReadErr error
}

// NewMemDevice returns an in-memory Handle of size bytes with the given
// sector size, for tests and for --dry-run.
func NewMemDevice(data []byte, sectorSize int, write bool) (Handle, *MemStats, error) {
	m := &memDevice{data: data, ss: sectorSize, FailWriteAt: -1}
	h, err := newAligned(m, write)
	if err != nil {
		return nil, nil, err
	}
	return h, &MemStats{dev: m}, nil
}

// MemStats exposes the bits of a memDevice a test wants to assert on
// without exporting the device itself.
type MemStats struct{ dev *memDevice }

func (s *MemStats) Bytes() []byte         { return s.dev.data }
func (s *MemStats) Flushes() int          { return s.dev.Flushes }
func (s *MemStats) Locked() bool          { return s.dev.locked }
func (s *MemStats) FailWriteAt(off int64) { s.dev.FailWriteAt = off }
func (s *MemStats) SetReadErr(err error)  { s.dev.ReadErr = err }

func (m *memDevice) ReadSectors(p []byte, off int64) error {
	if m.ReadErr != nil {
		return m.ReadErr
	}
	copy(p, m.data[off:])
	return nil
}

func (m *memDevice) WriteSectors(p []byte, off int64) error {
	if m.FailWriteAt >= 0 && off+int64(len(p)) > m.FailWriteAt {
		return errors.New("memDevice: injected write fault")
	}
	copy(m.data[off:], p)
	return nil
}

func (m *memDevice) SectorSize() int { return m.ss }
func (m *memDevice) Size() int64     { return int64(len(m.data)) }
func (m *memDevice) Lock() error     { m.locked = true; return nil }
func (m *memDevice) Unlock() error   { m.locked = false; return nil }
func (m *memDevice) Flush() error    { m.Flushes++; return nil }
func (m *memDevice) Close() error    { return nil }
