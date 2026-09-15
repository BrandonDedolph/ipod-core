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
