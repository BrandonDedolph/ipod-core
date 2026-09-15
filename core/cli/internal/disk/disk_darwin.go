//go:build darwin

package disk

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"time"
)

// macOS raw-disk access, through diskutil.
//
// The IOKit route (IORegistry, IOMedia properties) is the "proper" one
// and needs cgo, which this project does not use anywhere — the whole
// binary has to cross-build from a Linux box with `GOOS=darwin go
// build`. diskutil's -plist output is the same data, is stable enough
// that Apple's own installers parse it, and costs one exec per disk.
//
// The cost is that a machine with diskutil disabled or a sandbox that
// blocks exec has no disk enumeration at all, which is reported as an
// error rather than as "no disks".

// diskutilTimeout bounds a diskutil call. A disk that is spinning up or
// a network volume that is not answering can hang diskutil for minutes;
// `core info` should say "macOS did not answer" instead.
const diskutilTimeout = 20 * time.Second

func diskutil(args ...string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(context.Background(), diskutilTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "diskutil", args...)
	out, err := cmd.Output()
	if err != nil {
		if ctx.Err() != nil {
			return nil, fmt.Errorf("disk: diskutil %s timed out after %s",
				strings.Join(args, " "), diskutilTimeout)
		}
		return nil, fmt.Errorf("disk: diskutil %s: %w", strings.Join(args, " "), err)
	}
	return out, nil
}

func listDisks() ([]Disk, error) {
	out, err := diskutil("list", "-plist")
	if err != nil {
		return nil, err
	}
	root, err := parsePlist(out)
	if err != nil {
		return nil, fmt.Errorf("disk: parsing `diskutil list -plist`: %w", err)
	}
	top := plistDict(root)

	// Volumes come from AllDisksAndPartitions, which nests partitions
	// under their whole disk; WholeDisks is the flat list of the disks
	// themselves. Using both avoids having to guess whether "disk4s2"
	// belongs to "disk4" by string prefix, which is wrong for APFS
	// synthesised disks.
	parts := map[string][]string{}
	mounts := map[string][]string{}
	for _, entry := range plistArr(top["AllDisksAndPartitions"]) {
		d := plistDict(entry)
		id := plistStr(d, "DeviceIdentifier")
		if id == "" {
			continue
		}
		for _, p := range plistArr(d["Partitions"]) {
			pd := plistDict(p)
			pid := plistStr(pd, "DeviceIdentifier")
			if pid == "" {
				continue
			}
			parts[id] = append(parts[id], "/dev/"+pid)
			if mp := plistStr(pd, "MountPoint"); mp != "" {
				mounts[id] = append(mounts[id], mp)
			}
		}
	}

	var disks []Disk
	for _, w := range plistArr(top["WholeDisks"]) {
		id, _ := w.(string)
		if id == "" {
			continue
		}
		d := Disk{Path: "/dev/" + id, SectorSize: 512}
		if info, err := diskInfo("/dev/" + id); err == nil {
			applyDiskInfo(&d, info)
		}
		d.Volumes = parts[id]
		d.MountPoints = mounts[id]
		disks = append(disks, d)
	}
	return disks, nil
}

// diskInfo runs `diskutil info -plist` for one device node.
func diskInfo(path string) (map[string]any, error) {
	out, err := diskutil("info", "-plist", path)
	if err != nil {
		return nil, err
	}
	root, err := parsePlist(out)
	if err != nil {
		return nil, fmt.Errorf("disk: parsing `diskutil info -plist %s`: %w", path, err)
	}
	return plistDict(root), nil
}

func applyDiskInfo(d *Disk, info map[string]any) {
	if n := plistInt(info, "Size"); n > 0 {
		d.SizeBytes = n
	} else if n := plistInt(info, "TotalSize"); n > 0 {
		d.SizeBytes = n
	}
	if n := plistInt(info, "DeviceBlockSize"); n > 0 {
		d.SectorSize = int(n)
	}
	// MediaName is the SCSI/USB product string ("iPod", "Apple iPod
	// Media"); IORegistryEntryName is the friendlier node name. Either
	// is what the "vendor says Apple" check reads.
	d.Model = plistStr(info, "MediaName")
	if d.Model == "" {
		d.Model = plistStr(info, "IORegistryEntryName")
	}
	d.Vendor = plistStr(info, "IORegistryEntryName")
	d.Removable = plistBool(info, "Removable") || plistBool(info, "RemovableMedia") ||
		plistBool(info, "Ejectable")
	d.USB = strings.EqualFold(plistStr(info, "BusProtocol"), "USB")
}

// darwinDevice is an open /dev/rdiskN.
type darwinDevice struct {
	f     *os.File
	path  string // the /dev/rdiskN we opened
	disk  string // the /dev/diskN diskutil knows
	ss    int
	size  int64
	write bool
	held  bool
}

// rawPath turns /dev/disk4 into /dev/rdisk4.
//
// The buffered node (/dev/diskN) goes through the unified buffer cache,
// which means a read-back after a write can be served from the pages we
// just wrote — the exact failure mode a read-back verify exists to
// catch. The raw node is unbuffered and reaches the device, at the cost
// of requiring every I/O to be sector-aligned, which the aligned
// wrapper already guarantees.
func rawPath(path string) string {
	const pfx = "/dev/disk"
	if strings.HasPrefix(path, pfx) {
		return "/dev/rdisk" + strings.TrimPrefix(path, pfx)
	}
	return path
}

func diskPath(path string) string {
	const pfx = "/dev/rdisk"
	if strings.HasPrefix(path, pfx) {
		return "/dev/disk" + strings.TrimPrefix(path, pfx)
	}
	return path
}

func openRaw(path string, write bool) (rawDevice, error) {
	rp := rawPath(path)
	flag := os.O_RDONLY
	if write {
		flag = os.O_RDWR
	}
	f, err := os.OpenFile(rp, flag, 0)
	if err != nil {
		return nil, err
	}
	d := &darwinDevice{f: f, path: rp, disk: diskPath(path), ss: 512, write: write}
	info, ierr := diskInfo(d.disk)
	if ierr == nil {
		if n := plistInt(info, "DeviceBlockSize"); n > 0 {
			d.ss = int(n)
		}
		if n := plistInt(info, "Size"); n > 0 {
			d.size = n
		} else if n := plistInt(info, "TotalSize"); n > 0 {
			d.size = n
		}
	}
	if d.size == 0 {
		// Seeking to the end works on the raw node and is the only
		// fallback that needs no ioctl (and therefore no cgo-adjacent
		// constant table per architecture).
		if end, err := f.Seek(0, 2); err == nil && end > 0 {
			d.size = end
		} else {
			f.Close()
			return nil, fmt.Errorf("disk: cannot determine the size of %s", path)
		}
	}
	return d, nil
}

func (d *darwinDevice) ReadSectors(p []byte, off int64) error {
	_, err := d.f.ReadAt(p, off)
	return err
}

func (d *darwinDevice) WriteSectors(p []byte, off int64) error {
	_, err := d.f.WriteAt(p, off)
	return err
}

func (d *darwinDevice) SectorSize() int { return d.ss }
func (d *darwinDevice) Size() int64     { return d.size }

// Lock unmounts every filesystem on the disk. macOS refuses a write to
// a raw device with a mounted volume on it outright (and has since
// 10.11), so this is not advisory: without it the write fails.
func (d *darwinDevice) Lock() error {
	if _, err := diskutil("unmountDisk", d.disk); err != nil {
		return fmt.Errorf("%w: %v", ErrMounted, err)
	}
	d.held = true
	return nil
}

// Unlock remounts. Best-effort: a partition we cannot remount is a
// nuisance, not a data loss, and the user can do it from Finder.
func (d *darwinDevice) Unlock() error {
	if !d.held {
		return nil
	}
	d.held = false
	_, _ = diskutil("mountDisk", d.disk)
	return nil
}

// Flush is a no-op on the raw node: /dev/rdiskN is unbuffered, so a
// write that returned has already reached the device. There is no
// F_FULLFSYNC equivalent for a character device and fsync on one
// returns ENOTTY, which would turn a successful flash into a reported
// failure.
func (d *darwinDevice) Flush() error { return nil }

func (d *darwinDevice) Close() error {
	d.Unlock()
	return d.f.Close()
}
