//go:build windows

package eject

import (
	"fmt"
	"io"
	"os/exec"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

// The four control codes an eject is made of, from winioctl.h. They are
// spelled out rather than taken from golang.org/x/sys/windows so this package
// keeps the module's dependency list at cobra + x/image.
//
//	FSCTL_LOCK_VOLUME           CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 6,  METHOD_BUFFERED, FILE_ANY_ACCESS)
//	FSCTL_DISMOUNT_VOLUME       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 8,  METHOD_BUFFERED, FILE_ANY_ACCESS)
//	IOCTL_STORAGE_MEDIA_REMOVAL CTL_CODE(IOCTL_STORAGE_BASE,      0x0201, METHOD_BUFFERED, FILE_READ_ACCESS)
//	IOCTL_STORAGE_EJECT_MEDIA   CTL_CODE(IOCTL_STORAGE_BASE,      0x0202, METHOD_BUFFERED, FILE_READ_ACCESS)
const (
	fsctlLockVolume          = 0x00090018
	fsctlDismountVolume      = 0x00090020
	ioctlStorageMediaRemoval = 0x002D4804
	ioctlStorageEjectMedia   = 0x002D4808
)

var (
	kernel32            = syscall.NewLazyDLL("kernel32.dll")
	procDeviceIoControl = kernel32.NewProc("DeviceIoControl")
)

// Eject is the pure-Go route: open the volume, flush it, lock it,
// dismount it, re-allow media removal and eject. This is what Explorer's
// "Safely Remove Hardware" does, minus the tray icon.
//
// The Shell.Application COM verb (via PowerShell) is kept as the documented
// fallback for the one case the IOCTL route cannot handle: another process
// holding a file open makes FSCTL_LOCK_VOLUME fail, and the Shell route asks
// the volume's owner to close things first.
func Eject(w io.Writer, target string) error {
	vol, letter, err := volumePath(target)
	if err != nil {
		return err
	}

	ioctlErr := ejectByIOCTL(vol)
	if ioctlErr == nil {
		fmt.Fprintf(w, "%s: flushed, dismounted and ejected (volume IOCTL route). Safe to unplug.\n", letter)
		return nil
	}

	fmt.Fprintf(w, "%s: the volume route did not finish (%v); trying the Shell eject verb...\n", letter, ioctlErr)
	if err := ejectByShell(letter); err != nil {
		return fmt.Errorf("%s: could not eject.\n  volume IOCTL route: %v\n  Shell verb: %v\n"+
			"Close anything still reading the drive (Explorer windows, a media player, an antivirus scan) and try again",
			letter, ioctlErr, err)
	}
	fmt.Fprintf(w, "%s: ejected through the Shell verb. Safe to unplug.\n", letter)
	return nil
}

func ejectByIOCTL(vol string) error {
	p, err := syscall.UTF16PtrFromString(vol)
	if err != nil {
		return err
	}
	h, err := syscall.CreateFile(
		p,
		syscall.GENERIC_READ|syscall.GENERIC_WRITE,
		syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE,
		nil,
		syscall.OPEN_EXISTING,
		0,
		0,
	)
	if err != nil {
		return fmt.Errorf("open %s: %w", vol, err)
	}
	defer syscall.CloseHandle(h)

	// Flush first, always: everything below this line assumes the bytes are
	// already on the medium.
	if err := syscall.FlushFileBuffers(h); err != nil {
		return fmt.Errorf("flush %s: %w", vol, err)
	}

	// The lock is the step that actually fails in real life — anything with
	// a handle open on the volume refuses it — so it gets a few tries before
	// the caller falls back to the Shell verb.
	var lockErr error
	for try := 0; try < 4; try++ {
		if lockErr = deviceIoControl(h, fsctlLockVolume, nil); lockErr == nil {
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	if lockErr != nil {
		return fmt.Errorf("lock %s: %w", vol, lockErr)
	}
	if err := deviceIoControl(h, fsctlDismountVolume, nil); err != nil {
		return fmt.Errorf("dismount %s: %w", vol, err)
	}
	// PREVENT_MEDIA_REMOVAL{ BOOLEAN PreventMediaRemoval } = FALSE: undo any
	// removal lock a previous program left behind, or the eject is refused.
	if err := deviceIoControl(h, ioctlStorageMediaRemoval, []byte{0}); err != nil {
		return fmt.Errorf("allow media removal on %s: %w", vol, err)
	}
	if err := deviceIoControl(h, ioctlStorageEjectMedia, nil); err != nil {
		return fmt.Errorf("eject %s: %w", vol, err)
	}
	return nil
}

// deviceIoControl is the one kernel32 call this file needs, bound lazily so
// the package still builds for every other GOOS.
func deviceIoControl(h syscall.Handle, code uint32, in []byte) error {
	var (
		returned uint32
		inPtr    uintptr
		inLen    uintptr
	)
	if len(in) > 0 {
		inPtr = uintptr(unsafe.Pointer(&in[0]))
		inLen = uintptr(len(in))
	}
	r1, _, err := procDeviceIoControl.Call(
		uintptr(h),
		uintptr(code),
		inPtr, inLen,
		0, 0,
		uintptr(unsafe.Pointer(&returned)),
		0,
	)
	if r1 == 0 {
		if err == nil {
			return fmt.Errorf("DeviceIoControl(0x%08X) failed", code)
		}
		return err
	}
	return nil
}

// ejectByShell is the documented fallback: Explorer's own eject verb, through
// the Shell.Application COM object. Namespace(17) is ssfDRIVES, "This PC".
func ejectByShell(letter string) error {
	script := fmt.Sprintf(
		`$ErrorActionPreference='Stop';`+
			`$sh = New-Object -ComObject Shell.Application;`+
			`$item = $sh.Namespace(17).ParseName('%s');`+
			`if ($item -eq $null) { throw 'no such drive' };`+
			`$item.InvokeVerb('Eject')`, letter)
	out, err := exec.Command("powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script).CombinedOutput()
	if err != nil {
		msg := strings.TrimSpace(string(out))
		if msg == "" {
			return err
		}
		return fmt.Errorf("%v: %s", err, msg)
	}
	return nil
}

// volumePath turns whatever the user typed — "D", "d:", "D:\", "\\.\D:" —
// into the device path CreateFile wants and the "D:" form the Shell wants.
func volumePath(target string) (vol, letter string, err error) {
	t := strings.TrimSpace(target)
	t = strings.TrimPrefix(t, `\\.\`)
	t = strings.TrimRight(t, `\/`)
	if len(t) == 1 {
		t += ":"
	}
	if len(t) != 2 || t[1] != ':' {
		return "", "", fmt.Errorf("%q is not a drive letter; on Windows eject takes one (for example: core eject D:)", target)
	}
	letter = strings.ToUpper(t)
	return `\\.\` + letter, letter, nil
}
