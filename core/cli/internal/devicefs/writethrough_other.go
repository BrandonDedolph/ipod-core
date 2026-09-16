// SPDX-License-Identifier: Apache-2.0

//go:build !windows

package devicefs

import "os"

// OpenWriteThrough creates (or truncates) path for writing. There is no
// write-through open flag to ask for portably here, so the guarantee comes
// entirely from the caller's Sync() — see the Windows build for the other
// half of this pair.
func OpenWriteThrough(path string) (*os.File, error) {
	return os.OpenFile(path, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0o666)
}

// OpenWriteThroughExisting opens an EXISTING file for read/write without
// truncating it. StampConfigTime uses it to rewrite one 1024-byte slot of
// CORECFG.DAT in place: the file's size, its cluster chain and its directory
// entry must not change, because the firmware's FAT32 driver can read those
// but cannot repair them — the same argument the device's own writes rest on
// (core/kernel/config.c). O_CREATE is deliberately absent: a missing file is
// an error here, not something to invent.
func OpenWriteThroughExisting(path string) (*os.File, error) {
	return os.OpenFile(path, os.O_RDWR, 0o666)
}
