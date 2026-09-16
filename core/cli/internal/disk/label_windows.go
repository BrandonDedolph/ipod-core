//go:build windows

package disk

import (
	"errors"
	"fmt"
	"strings"
	"syscall"
	"unsafe"
)

// The two kernel32 calls behind the iPod's name. kernel32 itself is the
// lazy DLL already loaded in disk_windows.go — one handle for the
// package, not one per file.
var (
	procGetVolumeInformationW = kernel32.NewProc("GetVolumeInformationW")
	procSetVolumeLabelW       = kernel32.NewProc("SetVolumeLabelW")
)

// VolumeLabel reads the FAT volume label of a mounted volume: the name
// Windows Explorer shows beside the drive letter.
//
// root is a drive letter or a mount point; VolumeRoot adds the trailing
// backslash the API insists on. The label comes back as it is stored,
// which on FAT means upper-case — `Brandon's iPod` reads back as
// `BRANDON'S I`, and showing that is more honest than folding it back
// to title case and implying the device kept what was typed.
//
// An empty label is not an error: a volume with no name is ordinary,
// and this returns "" for it.
func VolumeLabel(root string) (string, error) {
	suppressDiskErrorDialogs()
	r := VolumeRoot(root)
	p, err := syscall.UTF16PtrFromString(r)
	if err != nil {
		return "", fmt.Errorf("disk: %q is not a usable volume path: %w", root, err)
	}
	// MAX_PATH+1 WCHARs is what GetVolumeInformationW documents as the
	// buffer for the label; the count is in characters, not bytes.
	var name [261]uint16
	ret, _, callErr := procGetVolumeInformationW.Call(
		uintptr(unsafe.Pointer(p)),
		uintptr(unsafe.Pointer(&name[0])), uintptr(len(name)),
		0, 0, 0, 0, 0)
	if ret == 0 {
		return "", fmt.Errorf("disk: reading the volume label of %s: %w", r, callErr)
	}
	return syscall.UTF16ToString(name[:]), nil
}

// SetVolumeLabel writes the FAT volume label.
//
// The label must already be legal — call LegalLabel on whatever a user
// typed first; this refuses anything ValidLabel refuses rather than
// letting Windows silently keep the old name. An empty label is the one
// exception and means "clear it": SetVolumeLabelW deletes the label
// when it is handed a NULL, which is what a name of nothing has to
// mean.
//
// Two conditions the caller owns, because this call cannot see them:
// the process must be elevated (the app's manifest asks for it), and
// the volume must not be locked — the flasher's Lock dismounts the
// drive letter, so nothing may rename a volume during a flash.
func SetVolumeLabel(root, label string) error {
	if err := ValidLabel(label); err != nil && !errors.Is(err, ErrLabelEmpty) {
		return err
	}
	suppressDiskErrorDialogs()
	r := VolumeRoot(root)
	p, err := syscall.UTF16PtrFromString(r)
	if err != nil {
		return fmt.Errorf("disk: %q is not a usable volume path: %w", root, err)
	}
	var lp uintptr // NULL deletes the label
	if strings.TrimSpace(label) != "" {
		q, err := syscall.UTF16PtrFromString(label)
		if err != nil {
			return fmt.Errorf("disk: %q is not a usable label: %w", label, err)
		}
		lp = uintptr(unsafe.Pointer(q))
	}
	ret, _, callErr := procSetVolumeLabelW.Call(uintptr(unsafe.Pointer(p)), lp)
	if ret == 0 {
		return fmt.Errorf("disk: setting the volume label of %s to %q: %w", r, label, callErr)
	}
	return nil
}

// VolumeDiskSerial answers "which physical device is this drive letter
// on?" with that device's serial — the key the app files a friendly
// name under.
//
// It goes through a zero-access handle to \\.\D:, which opens WITHOUT
// Administrator (see openHandleW) and still answers
// IOCTL_STORAGE_QUERY_PROPERTY, so `core name D:` can file the name
// under the same serial FindIPods reports without asking for elevation
// the command otherwise does not need. An empty serial is not an error:
// plenty of USB bridges report none.
func VolumeDiskSerial(root string) (string, error) {
	letter := strings.TrimSuffix(VolumeRoot(root), `\`)
	if len(letter) != 2 || letter[1] != ':' {
		return "", fmt.Errorf("disk: %q is not a drive letter", root)
	}
	suppressDiskErrorDialogs()
	h, err := openHandleW(`\\.\`+letter, 0, 0)
	if err != nil {
		return "", fmt.Errorf("disk: opening %s to read its device serial: %w", letter, err)
	}
	defer syscall.CloseHandle(h)
	var d Disk
	queryStorageDevice(h, &d)
	return d.Serial, nil
}
