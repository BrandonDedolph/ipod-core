// Package disk is the only place in core that opens a raw block device.
//
// It does three things and nothing else: enumerate the disks the OS can
// see (List), open one for aligned raw I/O (Open), and decide which of
// them is an iPod (FindIPods). Everything above it — `info`, `backup`,
// `firmware read`, and later `flash` — works through the Handle
// interface, so the write path can be exercised against an in-memory
// fake and the per-OS code stays small enough to read in one sitting.
//
// Three things are deliberately NOT here:
//
//   - USB VID/PID. The safety checklist in internal/fwpart/doc.go asks
//     for it, and getting it portably means libusb (cgo) or three more
//     per-OS enumerations. What replaces it is the SCSI/USB inquiry
//     strings the OS already has — vendor "Apple", product "iPod" —
//     plus the on-disk evidence (the Apple firmware preamble and the
//     5.5G partition table), which is strictly better evidence about
//     the bytes we are going to write than a descriptor is. See
//     identify() for the three checks actually performed.
//   - Any write. This package can open a device for writing and hands
//     back a WriterAt, but nothing here decides to use it. That
//     decision belongs to `core flash` (S7).
//   - Caching. List() re-enumerates every call. Disks come and go
//     between an enumeration and an open, which is exactly why Open
//     re-reads the geometry from the handle rather than trusting the
//     Disk struct.
package disk

import (
	"errors"
	"fmt"
	"io"
	"strings"
)

// Disk is one whole block device as the OS describes it. Nothing in
// here is read from the disk's own contents; it is all metadata from
// the driver, which is why SectorSize is cross-checked against the
// partition contents in FindIPods before it is trusted.
type Disk struct {
	// Path is what Open takes: `\\.\PhysicalDriveN`, /dev/sdX,
	// /dev/diskN.
	Path string
	// SizeBytes is the whole-device size.
	SizeBytes int64
	// SectorSize is the LOGICAL sector size the OS reports. On this
	// project's device the USB bridge reports 2048 even though the
	// drive itself is 512-byte LBA; the partition table is expressed
	// in whatever unit the transport reports, so this number (not a
	// constant) is what multiplies an LBA.
	SectorSize int
	// Vendor, Model, Serial come from the SCSI inquiry / sysfs /
	// diskutil. Any of them may be empty.
	Vendor, Model, Serial string
	// Removable is the driver's removable-media bit.
	Removable bool
	// USB is true when the device hangs off a USB bus.
	USB bool
	// Volumes are the OS names of the partitions/volumes on this disk:
	// "D:" on Windows, /dev/sdb1 on Linux, /dev/disk4s1 on macOS.
	Volumes []string
	// MountPoints are where those volumes are mounted, if anywhere.
	MountPoints []string
}

// Describe is the one-line form used by error messages and `info`.
func (d Disk) Describe() string {
	name := strings.TrimSpace(d.Vendor + " " + d.Model)
	if name == "" {
		name = "(no model string)"
	}
	s := fmt.Sprintf("%s  %s  %s", d.Path, name, HumanSize(d.SizeBytes))
	if len(d.Volumes) > 0 {
		s += "  [" + strings.Join(d.Volumes, " ") + "]"
	}
	return s
}

// Handle is an open raw device.
//
// ReadAt and WriteAt accept ANY offset and ANY length: the
// implementation does the read-modify-write through a bounce buffer so
// callers never have to know the sector size. (They can ask — the
// firmware partition's own offsets are expressed in sectors — but they
// are not forced to align their I/O.) Both are safe for concurrent use;
// the bounce buffer is behind a mutex.
type Handle interface {
	io.ReaderAt
	io.WriterAt
	// SectorSize is the logical sector size of the transport.
	SectorSize() int
	// Size is the addressable size, rounded down to a whole sector.
	Size() int64
	// Lock makes the OS stop touching the device: dismounting its
	// volumes on Windows, refusing if anything is mounted on Linux,
	// `diskutil unmountDisk` on macOS. It is a no-op on a fake.
	Lock() error
	// Unlock reverses Lock. Closing also unlocks.
	Unlock() error
	// Flush pushes writes past any cache the OS still owns. On a
	// handle opened read-only it is a no-op.
	Flush() error
	// Close releases the handle (and the lock, if held).
	Close() error
}

// Errors this package reports. The device-selection two are the ones
// the CLI turns into a usage message, so they are sentinels.
var (
	// ErrNoDevice: nothing that passes identify() is attached.
	ErrNoDevice = errors.New("disk: no iPod found")
	// ErrMultipleDevices: more than one; the caller must pass
	// --device.
	ErrMultipleDevices = errors.New("disk: multiple iPods found; select one with --device")
	// ErrRawAccessDenied: the OS refused the raw open. On Windows
	// this is the normal result without Administrator; on Linux and
	// macOS it means not root.
	ErrRawAccessDenied = errors.New("disk: raw device access denied")
	// ErrReadOnly: a WriteAt on a handle opened with write=false.
	ErrReadOnly = errors.New("disk: handle was opened read-only")
	// ErrMounted: Lock refused because a filesystem on the device is
	// mounted (Linux).
	ErrMounted = errors.New("disk: a partition of this device is mounted")
	// ErrUnsupported: this GOOS has no raw-device implementation.
	ErrUnsupported = errors.New("disk: raw device access is not implemented on this platform")
)

// List enumerates the whole block devices the OS can see. It never
// reads a single byte of their contents, so it works without
// Administrator on Windows and without root on Linux (sysfs is
// world-readable); only Open needs privilege.
func List() ([]Disk, error) { return listDisks() }

// Open opens a raw device for aligned I/O. write=false gives a handle
// whose WriteAt always fails, which is what every read-only command
// asks for — a read-only open is also the only kind that can succeed
// while the device is mounted.
func Open(path string, write bool) (Handle, error) {
	dev, err := openRaw(path, write)
	if err != nil {
		return nil, err
	}
	h, err := newAligned(dev, write)
	if err != nil {
		dev.Close()
		return nil, err
	}
	return h, nil
}

// HumanSize formats a byte count the way a disk vendor and an OS
// disagree about it: decimal GB (what the label says) with the binary
// GiB alongside (what the OS reports), because the 80 GB device in this
// project measures 74.5 GiB and every size check in here is written in
// GiB.
func HumanSize(n int64) string {
	if n <= 0 {
		return "0 B"
	}
	const gib = 1 << 30
	if n < gib {
		return fmt.Sprintf("%d B (%.1f MiB)", n, float64(n)/(1<<20))
	}
	return fmt.Sprintf("%.1f GB / %.2f GiB", float64(n)/1e9, float64(n)/gib)
}
