// SPDX-License-Identifier: Apache-2.0

//go:build windows

package devicefs

import (
	"os"
	"syscall"
)

// fileFlagWriteThrough is FILE_FLAG_WRITE_THROUGH from winbase.h. The
// syscall package does not export it, and defining the one constant is
// cheaper than taking a dependency on golang.org/x/sys for it.
const fileFlagWriteThrough = 0x80000000

// OpenWriteThrough creates (or truncates) path for writing with
// FILE_FLAG_WRITE_THROUGH, so every write goes to the medium instead of
// sitting in the Windows cache manager. The caller still calls Sync() before
// Close(): on Windows that is FlushFileBuffers, which is what actually
// empties the disk's own cache.
//
// This matters because the target is removable FAT32 that a user unplugs.
// Bytes still in a cache when the iPod leaves the dock are a library that
// half-arrived, and the firmware cannot repair a filesystem — it can only
// refuse to mount one.
func OpenWriteThrough(path string) (*os.File, error) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, &os.PathError{Op: "open", Path: path, Err: err}
	}
	h, err := syscall.CreateFile(
		p,
		syscall.GENERIC_READ|syscall.GENERIC_WRITE,
		syscall.FILE_SHARE_READ,
		nil,
		syscall.CREATE_ALWAYS,
		syscall.FILE_ATTRIBUTE_NORMAL|fileFlagWriteThrough,
		0,
	)
	if err != nil {
		return nil, &os.PathError{Op: "open", Path: path, Err: err}
	}
	return os.NewFile(uintptr(h), path), nil
}

// OpenWriteThroughExisting opens an EXISTING file for read/write with
// FILE_FLAG_WRITE_THROUGH and OPEN_EXISTING — the no-truncate half of the
// pair above. CREATE_ALWAYS would empty CORECFG.DAT, and an emptied
// CORECFG.DAT is a device that has forgotten every setting the user has, so
// the distinction is load-bearing rather than stylistic.
func OpenWriteThroughExisting(path string) (*os.File, error) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, &os.PathError{Op: "open", Path: path, Err: err}
	}
	h, err := syscall.CreateFile(
		p,
		syscall.GENERIC_READ|syscall.GENERIC_WRITE,
		syscall.FILE_SHARE_READ,
		nil,
		syscall.OPEN_EXISTING,
		syscall.FILE_ATTRIBUTE_NORMAL|fileFlagWriteThrough,
		0,
	)
	if err != nil {
		return nil, &os.PathError{Op: "open", Path: path, Err: err}
	}
	return os.NewFile(uintptr(h), path), nil
}
