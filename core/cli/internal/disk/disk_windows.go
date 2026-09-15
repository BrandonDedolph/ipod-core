//go:build windows

package disk

import (
	"encoding/binary"
	"fmt"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

var (
	kernel32           = syscall.NewLazyDLL("kernel32.dll")
	procSetErrorMode   = kernel32.NewProc("SetErrorMode")
	suppressErrorsOnce sync.Once
)

// suppressDiskErrorDialogs stops Windows popping "There is no disk in
// the drive" at the user.
//
// Enumeration opens every drive letter A..Z to ask which physical disk
// it lives on, and on a machine with an empty card reader or optical
// drive that open raises a critical-error dialog the CLI has no way to
// dismiss — a modal box in front of a command-line tool, from a command
// the user did not think was touching that drive. SEM_FAILCRITICALERRORS
// turns it into the error return we already handle.
//
// Process-wide and never restored, which is the documented way to use
// it (SetThreadErrorMode would need runtime.LockOSThread to be correct
// under a goroutine scheduler) and is harmless in a binary whose whole
// job is talking to disks.
func suppressDiskErrorDialogs() {
	suppressErrorsOnce.Do(func() {
		const semFailCriticalErrors = 0x0001
		const semNoOpenFileErrorBox = 0x8000
		_, _, _ = procSetErrorMode.Call(semFailCriticalErrors | semNoOpenFileErrorBox)
	})
}

// Device-control codes, assembled by hand from CTL_CODE(). They are
// written out as literals with the CTL_CODE arguments in the comment so
// a reviewer can check the arithmetic without a Windows SDK.
const (
	// CTL_CODE(IOCTL_STORAGE_BASE 0x2d, 0x0500, METHOD_BUFFERED, FILE_ANY_ACCESS)
	ioctlStorageQueryProperty = 0x002D1400
	// CTL_CODE(IOCTL_DISK_BASE 0x07, 0x0017, METHOD_BUFFERED, FILE_READ_ACCESS)
	ioctlDiskGetLengthInfo = 0x0007405C
	// CTL_CODE(IOCTL_DISK_BASE 0x07, 0x0028, METHOD_BUFFERED, FILE_ANY_ACCESS)
	ioctlDiskGetDriveGeometryEx = 0x000700A0
	// CTL_CODE(IOCTL_VOLUME_BASE 0x56, 0x0000, METHOD_BUFFERED, FILE_ANY_ACCESS)
	ioctlVolumeGetVolumeDiskExtents = 0x00560000
	// CTL_CODE(FILE_DEVICE_FILE_SYSTEM 0x09, 6/7/8, METHOD_BUFFERED, FILE_ANY_ACCESS)
	fsctlLockVolume     = 0x00090018
	fsctlUnlockVolume   = 0x0009001C
	fsctlDismountVolume = 0x00090020
)

const (
	fileFlagNoBuffering   = 0x20000000
	fileFlagWriteThrough  = 0x80000000
	storageDeviceProperty = 0
	propertyStandardQuery = 0
	busTypeUSB            = 0x07
	maxPhysicalDrives     = 32
)

// openHandleW wraps CreateFileW. access 0 is the important case: a
// zero-access handle to \\.\PhysicalDriveN opens WITHOUT Administrator
// and still answers the metadata IOCTLs, which is why `core info
// --all-disks` can enumerate every disk from an ordinary console and
// only the raw read needs elevation.
func openHandleW(path string, access, flags uint32) (syscall.Handle, error) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return syscall.InvalidHandle, err
	}
	h, err := syscall.CreateFile(p, access,
		syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE, nil,
		syscall.OPEN_EXISTING, flags, 0)
	if err != nil {
		return syscall.InvalidHandle, err
	}
	return h, nil
}

func deviceIoControl(h syscall.Handle, code uint32, in, out []byte) (uint32, error) {
	var inPtr, outPtr *byte
	var inLen, outLen uint32
	if len(in) > 0 {
		inPtr, inLen = &in[0], uint32(len(in))
	}
	if len(out) > 0 {
		outPtr, outLen = &out[0], uint32(len(out))
	}
	var ret uint32
	err := syscall.DeviceIoControl(h, code, inPtr, inLen, outPtr, outLen, &ret, nil)
	return ret, err
}

// listDisks walks \\.\PhysicalDrive0..31.
//
// There is no "enumerate disks" call in Win32 that does not go through
// SetupAPI or WMI; probing the fixed range is what every tool from
// ipodpatcher to dd-for-windows does, and a machine with more than 32
// physical drives is not the machine someone is flashing an iPod from.
// Open failures are skipped silently: the numbers are sparse, and an
// absent drive 7 is not an error to report.
func listDisks() ([]Disk, error) {
	suppressDiskErrorDialogs()
	vols := volumesByDisk()
	var out []Disk
	for n := 0; n < maxPhysicalDrives; n++ {
		path := fmt.Sprintf(`\\.\PhysicalDrive%d`, n)
		h, err := openHandleW(path, 0, 0)
		if err != nil {
			continue
		}
		d := Disk{Path: path}
		queryStorageDevice(h, &d)
		ss, geoSize, geoErr := diskGeometry(h)
		if geoErr == nil && ss > 0 {
			d.SectorSize = int(ss)
		}
		if d.SectorSize == 0 {
			d.SectorSize = 512
		}
		if size, err := diskLength(h); err == nil && size > 0 {
			d.SizeBytes = size
		} else {
			d.SizeBytes = geoSize
		}
		for _, letter := range vols[uint32(n)] {
			d.Volumes = append(d.Volumes, letter)
			d.MountPoints = append(d.MountPoints, letter+`\`)
		}
		syscall.CloseHandle(h)
		out = append(out, d)
	}
	return out, nil
}

// STORAGE_DEVICE_DESCRIPTOR field offsets.
const (
	sddRemovableMedia = 10
	sddVendorIDOff    = 12
	sddProductIDOff   = 16
	sddSerialOff      = 24
	sddBusType        = 28
)

func queryStorageDevice(h syscall.Handle, d *Disk) {
	query := make([]byte, 12)
	binary.LittleEndian.PutUint32(query[0:4], storageDeviceProperty)
	binary.LittleEndian.PutUint32(query[4:8], propertyStandardQuery)
	buf := make([]byte, 2048)
	n, err := deviceIoControl(h, ioctlStorageQueryProperty, query, buf)
	if err != nil || n < sddBusType+4 {
		return
	}
	buf = buf[:n]
	d.Removable = buf[sddRemovableMedia] != 0
	d.USB = binary.LittleEndian.Uint32(buf[sddBusType:]) == busTypeUSB
	d.Vendor = descriptorString(buf, binary.LittleEndian.Uint32(buf[sddVendorIDOff:]))
	d.Model = descriptorString(buf, binary.LittleEndian.Uint32(buf[sddProductIDOff:]))
	d.Serial = descriptorString(buf, binary.LittleEndian.Uint32(buf[sddSerialOff:]))
}

// descriptorString reads a NUL-terminated ASCII string at a byte offset
// inside the descriptor. Offset 0 means "this device has no such
// string", which is common for the serial over a USB bridge.
func descriptorString(buf []byte, off uint32) string {
	if off == 0 || int(off) >= len(buf) {
		return ""
	}
	s := buf[off:]
	if i := indexByte(s, 0); i >= 0 {
		s = s[:i]
	}
	return strings.TrimSpace(string(s))
}

func indexByte(b []byte, c byte) int {
	for i := range b {
		if b[i] == c {
			return i
		}
	}
	return -1
}

// diskLength: GET_LENGTH_INFORMATION is a bare LARGE_INTEGER.
func diskLength(h syscall.Handle) (int64, error) {
	buf := make([]byte, 8)
	if _, err := deviceIoControl(h, ioctlDiskGetLengthInfo, nil, buf); err != nil {
		return 0, err
	}
	return int64(binary.LittleEndian.Uint64(buf)), nil
}

// diskGeometry: DISK_GEOMETRY_EX starts with a DISK_GEOMETRY, whose
// BytesPerSector is the last of its four DWORDs — 8 bytes of Cylinders,
// then MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector,
// so offset 20 — followed by a LARGE_INTEGER DiskSize at offset 24.
//
// That DiskSize matters more than it looks. This IOCTL is FILE_ANY_
// ACCESS, so it answers on a zero-access handle; IOCTL_DISK_GET_LENGTH_
// INFO is FILE_READ_ACCESS and does not. Without the fallback,
// `core info --all-disks` from an ordinary console printed every disk
// with a size of 0 — the enumeration worked and the one number people
// use to recognise their iPod was missing.
func diskGeometry(h syscall.Handle) (bytesPerSector uint32, diskSize int64, err error) {
	buf := make([]byte, 64)
	n, err := deviceIoControl(h, ioctlDiskGetDriveGeometryEx, nil, buf)
	if err != nil {
		return 0, 0, err
	}
	if n < 24 {
		return 0, 0, fmt.Errorf("disk: geometry reply was %d bytes", n)
	}
	bytesPerSector = binary.LittleEndian.Uint32(buf[20:24])
	if n >= 32 {
		diskSize = int64(binary.LittleEndian.Uint64(buf[24:32]))
	}
	return bytesPerSector, diskSize, nil
}

// volumesByDisk maps a physical-drive number to the drive letters that
// live on it, through IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS.
//
// Letters A..Z is the whole search space: a volume with no letter
// cannot be dismounted by name and is not one the user will recognise
// in a message. Each letter is opened with zero access so this works
// unelevated too.
func volumesByDisk() map[uint32][]string {
	suppressDiskErrorDialogs()
	out := map[uint32][]string{}
	for c := byte('A'); c <= 'Z'; c++ {
		letter := string(c) + ":"
		h, err := openHandleW(`\\.\`+letter, 0, 0)
		if err != nil {
			continue
		}
		// VOLUME_DISK_EXTENTS: DWORD count, 4 bytes padding, then
		// DISK_EXTENT{DWORD DiskNumber; 4 pad; LARGE_INTEGER Start;
		// LARGE_INTEGER Length} — 24 bytes each.
		buf := make([]byte, 8+24*16)
		n, err := deviceIoControl(h, ioctlVolumeGetVolumeDiskExtents, nil, buf)
		syscall.CloseHandle(h)
		if err != nil || n < 8+24 {
			continue
		}
		count := binary.LittleEndian.Uint32(buf[0:4])
		for i := uint32(0); i < count; i++ {
			base := 8 + int(i)*24
			if base+4 > len(buf) {
				break
			}
			disk := binary.LittleEndian.Uint32(buf[base : base+4])
			out[disk] = append(out[disk], letter)
		}
	}
	return out
}

// winDevice is an open \\.\PhysicalDriveN.
type winDevice struct {
	h        syscall.Handle
	path     string
	number   uint32
	ss       int
	size     int64
	write    bool
	buf      []byte   // sector-aligned bounce for NO_BUFFERING I/O
	volumes  []string // letters locked by Lock
	volumeHs []syscall.Handle
}

func openRaw(path string, write bool) (rawDevice, error) {
	suppressDiskErrorDialogs()
	access := uint32(syscall.GENERIC_READ)
	if write {
		access |= syscall.GENERIC_WRITE
	}
	// NO_BUFFERING because the cache Windows keeps over a physical
	// drive is the thing that makes a read-back "verify" a write that
	// never left memory; WRITE_THROUGH because the drive's own cache is
	// the next one down. Both are what make the aligned-I/O contract
	// mandatory rather than merely tidy.
	h, err := openHandleW(path, access, fileFlagNoBuffering|fileFlagWriteThrough)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	d := &winDevice{h: h, path: path, write: write}
	if n, err := strconv.ParseUint(
		strings.TrimPrefix(strings.ToUpper(path), `\\.\PHYSICALDRIVE`), 10, 32); err == nil {
		d.number = uint32(n)
	}
	ss, geoSize, geoErr := diskGeometry(h)
	if geoErr == nil && ss > 0 {
		d.ss = int(ss)
	} else {
		d.ss = 512
	}
	size, err := diskLength(h)
	if err != nil || size <= 0 {
		size = geoSize
	}
	if size <= 0 {
		syscall.CloseHandle(h)
		return nil, fmt.Errorf("disk: cannot determine the size of %s: %w", path, err)
	}
	d.size = size
	return d, nil
}

// alignedBuf hands back a slice of n bytes whose first byte is aligned
// to the sector size, which FILE_FLAG_NO_BUFFERING requires of every
// buffer as well as every offset and length. A Go slice from make() has
// no such guarantee, so the buffer is over-allocated and sliced to the
// next aligned address. Go's collector does not move heap objects, so
// the address stays valid for the life of the slice.
func (d *winDevice) alignedBuf(n int) []byte {
	need := n + d.ss
	if cap(d.buf) < need {
		d.buf = make([]byte, need)
	}
	b := d.buf[:need]
	off := int(uintptr(unsafe.Pointer(&b[0])) % uintptr(d.ss))
	if off != 0 {
		off = d.ss - off
	}
	return b[off : off+n]
}

func (d *winDevice) seek(off int64) error {
	if _, err := syscall.Seek(d.h, off, 0); err != nil {
		return fmt.Errorf("seek %s to %#x: %w", d.path, off, err)
	}
	return nil
}

func (d *winDevice) ReadSectors(p []byte, off int64) error {
	buf := d.alignedBuf(len(p))
	if err := d.seek(off); err != nil {
		return err
	}
	done := 0
	for done < len(buf) {
		n, err := syscall.Read(d.h, buf[done:])
		if err != nil {
			return fmt.Errorf("read %s at %#x: %w", d.path, off+int64(done), err)
		}
		if n == 0 {
			return fmt.Errorf("read %s at %#x: short read (%d of %d bytes)",
				d.path, off, done, len(buf))
		}
		done += n
	}
	copy(p, buf)
	return nil
}

func (d *winDevice) WriteSectors(p []byte, off int64) error {
	if !d.write {
		return ErrReadOnly
	}
	buf := d.alignedBuf(len(p))
	copy(buf, p)
	if err := d.seek(off); err != nil {
		return err
	}
	done := 0
	for done < len(buf) {
		n, err := syscall.Write(d.h, buf[done:])
		if err != nil {
			return fmt.Errorf("write %s at %#x: %w", d.path, off+int64(done), err)
		}
		if n == 0 {
			return fmt.Errorf("write %s at %#x: short write (%d of %d bytes)",
				d.path, off, done, len(buf))
		}
		done += n
	}
	return nil
}

func (d *winDevice) SectorSize() int { return d.ss }
func (d *winDevice) Size() int64     { return d.size }

// Lock takes the volumes away from the filesystem before a raw write.
//
// FSCTL_LOCK_VOLUME makes the filesystem flush and stop; without it
// Windows keeps a cached view of the FAT32 partition that will happily
// write itself back over whatever we put there. FSCTL_DISMOUNT_VOLUME
// then detaches it, so nothing reopens it mid-flash. Both are held by
// keeping the volume handles open — closing a handle releases its lock,
// which is also why Unlock and Close both have to be safe to call.
//
// The lock on the physical-drive handle itself is attempted last and is
// best-effort: it is not documented to apply to a disk handle, it does
// no harm where it is refused, and the volume locks are the ones doing
// the work.
func (d *winDevice) Lock() error {
	if len(d.volumeHs) > 0 {
		return nil
	}
	letters := volumesByDisk()[d.number]
	for _, letter := range letters {
		vh, err := openHandleW(`\\.\`+letter,
			syscall.GENERIC_READ|syscall.GENERIC_WRITE, 0)
		if err != nil {
			d.Unlock()
			return fmt.Errorf("disk: open volume %s to lock it: %w", letter, err)
		}
		if _, err := deviceIoControl(vh, fsctlLockVolume, nil, nil); err != nil {
			syscall.CloseHandle(vh)
			d.Unlock()
			return fmt.Errorf("disk: lock volume %s (close anything using it): %w", letter, err)
		}
		if _, err := deviceIoControl(vh, fsctlDismountVolume, nil, nil); err != nil {
			syscall.CloseHandle(vh)
			d.Unlock()
			return fmt.Errorf("disk: dismount volume %s: %w", letter, err)
		}
		d.volumes = append(d.volumes, letter)
		d.volumeHs = append(d.volumeHs, vh)
	}
	_, _ = deviceIoControl(d.h, fsctlLockVolume, nil, nil)
	return nil
}

func (d *winDevice) Unlock() error {
	for _, vh := range d.volumeHs {
		_, _ = deviceIoControl(vh, fsctlUnlockVolume, nil, nil)
		syscall.CloseHandle(vh)
	}
	d.volumeHs = nil
	d.volumes = nil
	_, _ = deviceIoControl(d.h, fsctlUnlockVolume, nil, nil)
	return nil
}

func (d *winDevice) Flush() error {
	if !d.write {
		return nil
	}
	if err := syscall.FlushFileBuffers(d.h); err != nil {
		return fmt.Errorf("disk: FlushFileBuffers %s: %w", d.path, err)
	}
	return nil
}

func (d *winDevice) Close() error {
	d.Unlock()
	return syscall.CloseHandle(d.h)
}
