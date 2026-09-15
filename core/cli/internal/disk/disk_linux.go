//go:build linux

package disk

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"unsafe"
)

// Block-device ioctls. The three we need are stable kernel ABI and
// spelled out here rather than pulled from golang.org/x/sys/unix: two
// constants and one Syscall are not worth a dependency, and the sysfs
// fallbacks below mean a failed ioctl is not fatal anyway.
const (
	blkGetSize64 = 0x80081272 // BLKGETSIZE64: u64 bytes
	blkSSZGet    = 0x00001268 // BLKSSZGET:    int logical sector size
	blkFlsBuf    = 0x00001261 // BLKFLSBUF:    flush the buffer cache
)

const sysBlock = "/sys/block"

// listDisks enumerates whole block devices from sysfs.
//
// /sys/block holds one entry per whole device, which is exactly the
// list we want — partitions live underneath their disk, not here. The
// filter is "has a device/ symlink": that keeps sd*, nvme*, mmcblk* and
// usb-storage, and drops loop, ram, zram, dm-* and md*, which have no
// backing hardware and nothing an iPod check could ever match.
func listDisks() ([]Disk, error) {
	entries, err := os.ReadDir(sysBlock)
	if err != nil {
		return nil, fmt.Errorf("disk: read %s: %w", sysBlock, err)
	}
	mounts := readMountInfo()
	var out []Disk
	for _, e := range entries {
		name := e.Name()
		base := filepath.Join(sysBlock, name)
		if _, err := os.Stat(filepath.Join(base, "device")); err != nil {
			continue
		}
		d := Disk{Path: "/dev/" + name}
		// /sys/block/X/size is in 512-byte units ALWAYS, regardless of
		// the device's logical block size. That is the kernel's unit,
		// not the disk's, and multiplying it by logical_block_size is
		// the classic way to report a 4x-too-big 4Kn drive.
		if n, ok := readSysUint(filepath.Join(base, "size")); ok {
			d.SizeBytes = int64(n) * 512
		}
		if n, ok := readSysUint(filepath.Join(base, "queue", "logical_block_size")); ok {
			d.SectorSize = int(n)
		}
		if d.SectorSize == 0 {
			d.SectorSize = 512
		}
		if n, ok := readSysUint(filepath.Join(base, "removable")); ok {
			d.Removable = n != 0
		}
		d.Vendor = readSysString(filepath.Join(base, "device", "vendor"))
		d.Model = readSysString(filepath.Join(base, "device", "model"))
		d.Serial = readSysString(filepath.Join(base, "device", "serial"))
		// The bus is read off the sysfs path, which spells out the
		// whole topology: a USB disk resolves through
		// ../devices/pci…/usb1/1-2/…, an internal SATA one does not.
		if real, err := filepath.EvalSymlinks(base); err == nil {
			d.USB = strings.Contains(real, "/usb")
		}
		for _, part := range partitionsOf(name) {
			d.Volumes = append(d.Volumes, "/dev/"+part.name)
			if mp, ok := mounts[part.dev]; ok {
				d.MountPoints = append(d.MountPoints, mp)
			}
		}
		out = append(out, d)
	}
	return out, nil
}

type sysPart struct {
	name string // "sdb1"
	dev  string // "8:17"
}

// partitionsOf lists a disk's partitions from sysfs, in on-disk order.
func partitionsOf(disk string) []sysPart {
	entries, err := os.ReadDir(filepath.Join(sysBlock, disk))
	if err != nil {
		return nil
	}
	var out []sysPart
	for _, e := range entries {
		if !strings.HasPrefix(e.Name(), disk) {
			continue
		}
		pdir := filepath.Join(sysBlock, disk, e.Name())
		if _, err := os.Stat(filepath.Join(pdir, "partition")); err != nil {
			continue
		}
		out = append(out, sysPart{name: e.Name(), dev: readSysString(filepath.Join(pdir, "dev"))})
	}
	return out
}

func readSysString(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

func readSysUint(path string) (uint64, bool) {
	s := readSysString(path)
	if s == "" {
		return 0, false
	}
	n, err := strconv.ParseUint(s, 10, 64)
	return n, err == nil
}

// readMountInfo maps "major:minor" to the mount point.
//
// /proc/self/mountinfo rather than /proc/mounts because the device
// column in mountinfo is the kernel's own major:minor, which cannot be
// spoofed by a bind mount or confused by a /dev/disk/by-uuid symlink,
// and because it is the file that survives a mount namespace.
func readMountInfo() map[string]string {
	f, err := os.Open("/proc/self/mountinfo")
	if err != nil {
		return nil
	}
	defer f.Close()
	out := map[string]string{}
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 64<<10), 1<<20)
	for sc.Scan() {
		// 36 35 98:0 /mnt1 /mnt2 rw,noatime - ext3 /dev/root rw
		//  0  1   2     3     4      5
		f := strings.Fields(sc.Text())
		if len(f) < 5 {
			continue
		}
		out[f[2]] = unescapeMountPath(f[4])
	}
	return out
}

// unescapeMountPath undoes the octal escaping mountinfo applies to
// space, tab, newline and backslash.
func unescapeMountPath(s string) string {
	if !strings.Contains(s, `\`) {
		return s
	}
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+3 < len(s) {
			if n, err := strconv.ParseUint(s[i+1:i+4], 8, 8); err == nil {
				b.WriteByte(byte(n))
				i += 3
				continue
			}
		}
		b.WriteByte(s[i])
	}
	return b.String()
}

// linuxDevice is an open /dev/sdX.
type linuxDevice struct {
	f     *os.File
	path  string
	ss    int
	size  int64
	write bool
}

func openRaw(path string, write bool) (rawDevice, error) {
	flag := os.O_RDONLY
	if write {
		flag = os.O_RDWR
	}
	f, err := os.OpenFile(path, flag, 0)
	if err != nil {
		return nil, err
	}
	d := &linuxDevice{f: f, path: path, write: write}

	if n, err := ioctlUint32(f, blkSSZGet); err == nil && n > 0 {
		d.ss = int(n)
	} else if v, ok := readSysUint(filepath.Join(sysBlock, filepath.Base(path), "queue", "logical_block_size")); ok {
		d.ss = int(v)
	} else {
		d.ss = 512
	}
	if n, err := ioctlUint64(f, blkGetSize64); err == nil && n > 0 {
		d.size = int64(n)
	} else if end, err := f.Seek(0, 2); err == nil && end > 0 {
		d.size = end
	} else {
		f.Close()
		return nil, fmt.Errorf("disk: cannot determine the size of %s", path)
	}
	return d, nil
}

func ioctlUint32(f *os.File, req uintptr) (uint32, error) {
	var v uint32
	_, _, e := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), req, uintptr(unsafe.Pointer(&v)))
	if e != 0 {
		return 0, e
	}
	return v, nil
}

func ioctlUint64(f *os.File, req uintptr) (uint64, error) {
	var v uint64
	_, _, e := syscall.Syscall(syscall.SYS_IOCTL, f.Fd(), req, uintptr(unsafe.Pointer(&v)))
	if e != 0 {
		return 0, e
	}
	return v, nil
}

func (d *linuxDevice) ReadSectors(p []byte, off int64) error {
	_, err := d.f.ReadAt(p, off)
	return err
}

func (d *linuxDevice) WriteSectors(p []byte, off int64) error {
	_, err := d.f.WriteAt(p, off)
	return err
}

func (d *linuxDevice) SectorSize() int { return d.ss }
func (d *linuxDevice) Size() int64     { return d.size }

// Lock has no kernel primitive behind it on Linux, so it is a check
// rather than an action: if any partition of this disk is mounted, the
// page cache for those partitions is live and a raw write underneath it
// would be silently undone (or worse, half-undone) the next time the
// filesystem flushed. Refusing is the whole of the protection.
//
// The stronger thing available — open(2) with O_EXCL on the block
// device, which makes the kernel refuse subsequent mounts — is not used
// because it would have to be decided at Open time, and every read-only
// command would then fight with a mounted music partition it has no
// reason to care about.
func (d *linuxDevice) Lock() error {
	name := filepath.Base(d.path)
	mounts := readMountInfo()
	var mounted []string
	for _, p := range partitionsOf(name) {
		if mp, ok := mounts[p.dev]; ok {
			mounted = append(mounted, "/dev/"+p.name+" on "+mp)
		}
	}
	if len(mounted) > 0 {
		return fmt.Errorf("%w: %s; unmount it first (umount %s)",
			ErrMounted, strings.Join(mounted, ", "), strings.Join(mounted, " "))
	}
	return nil
}

// Unlock is a no-op: Lock took nothing to give back.
func (d *linuxDevice) Unlock() error { return nil }

// Flush fsyncs the block device and, if the kernel allows it, drops the
// buffer cache for it so a following read-back comes from the disk
// rather than from the pages we just wrote. BLKFLSBUF needs
// CAP_SYS_ADMIN and is best-effort; the fsync is the part that matters.
func (d *linuxDevice) Flush() error {
	if !d.write {
		return nil
	}
	if err := d.f.Sync(); err != nil {
		return fmt.Errorf("disk: fsync %s: %w", d.path, err)
	}
	_, _, _ = syscall.Syscall(syscall.SYS_IOCTL, d.f.Fd(), blkFlsBuf, 0)
	return nil
}

func (d *linuxDevice) Close() error { return d.f.Close() }
