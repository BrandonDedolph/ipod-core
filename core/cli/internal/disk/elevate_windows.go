//go:build windows

package disk

import (
	"fmt"
	"os"
	"strings"
	"syscall"
	"unsafe"
)

var (
	shell32           = syscall.NewLazyDLL("shell32.dll")
	procShellExecuteE = shell32.NewProc("ShellExecuteExW")
)

// TokenElevation is the TOKEN_INFORMATION_CLASS value for the
// elevation flag. syscall exposes GetTokenInformation but none of the
// classes past TokenUser, so it is spelled out here.
const tokenElevation = 20

// IsElevated reports whether this process runs with an elevated token.
//
// Not "is the user an Administrator" — that is a different and much
// less useful question, because a member of Administrators gets a
// filtered token by default and cannot open \\.\PhysicalDrive1 with it.
// What matters is whether THIS token is the elevated one.
func IsElevated() bool {
	proc, err := syscall.GetCurrentProcess()
	if err != nil {
		return false
	}
	var tok syscall.Token
	if err := syscall.OpenProcessToken(proc, syscall.TOKEN_QUERY, &tok); err != nil {
		return false
	}
	defer tok.Close()
	var elevated uint32
	var retLen uint32
	err = syscall.GetTokenInformation(tok, tokenElevation,
		(*byte)(unsafe.Pointer(&elevated)), uint32(unsafe.Sizeof(elevated)), &retLen)
	return err == nil && elevated != 0
}

// SHELLEXECUTEINFOW. The field order and the padding Go inserts after
// nShow and dwHotKey match the Windows struct on both amd64 and 386,
// and cbSize is computed from the Go struct rather than hard-coded so
// that stays true.
type shellExecuteInfo struct {
	cbSize       uint32
	fMask        uint32
	hwnd         uintptr
	lpVerb       *uint16
	lpFile       *uint16
	lpParameters *uint16
	lpDirectory  *uint16
	nShow        int32
	hInstApp     uintptr
	lpIDList     uintptr
	lpClass      *uint16
	hkeyClass    uintptr
	dwHotKey     uint32
	hIcon        uintptr
	hProcess     syscall.Handle
}

const (
	seeMaskNoCloseProcess = 0x00000040
	seeMaskNoAsync        = 0x00000100
	seeMaskFlagNoUI       = 0x00000400
	swHide                = 0
	infinite              = 0xFFFFFFFF
)

// RelaunchElevated starts an elevated copy of this executable, waits
// for it, and returns its exit code and everything it printed.
//
// UAC cannot raise the privileges of a running process, so "run as
// administrator" on Windows always means a second process. That second
// process gets its own console window, which closes the instant it
// exits, taking its output with it — so the child is told to write to a
// log file (ElevatedLogFlag) and the parent reads it back and prints
// it. From the user's point of view one command ran; from the OS's
// point of view two processes did, and only the second one ever touched
// the disk.
//
// The args passed in must be the FULL argument list for the child,
// including the subcommand. The caller is expected to have added --yes
// itself: the child has no terminal anyone is looking at, so a typed
// confirmation there would hang forever behind a window that flashes
// past.
func RelaunchElevated(args []string) (exitCode int, log string, err error) {
	exe, err := os.Executable()
	if err != nil {
		return 0, "", fmt.Errorf("disk: locating this executable to relaunch it: %w", err)
	}
	return RelaunchElevatedExe(exe, args)
}

// RelaunchElevatedExe is RelaunchElevated with the child named
// explicitly instead of taken from os.Executable.
//
// It exists for core-app, the GUI. A `-H windowsgui` process has no
// console at all, so a child that IS core-app could neither be told
// what to do on a command line the user can read nor print anything
// back; the app therefore elevates the `core` CLI that ships beside it,
// which already knows how to `flash <file> --yes --no-relaunch
// --elevated-log <file>`. One write path, one set of safety checks, and
// the UAC prompt names core.exe — which is the binary the user was told
// writes to the disk.
func RelaunchElevatedExe(exe string, args []string) (exitCode int, log string, err error) {
	if exe == "" {
		return 0, "", fmt.Errorf("disk: no executable named to relaunch elevated")
	}
	logPath := elevatedLogPath()
	_ = os.Remove(logPath)
	full := append(append([]string{}, args...), ElevatedLogFlag, logPath)

	parts := make([]string, len(full))
	for i, a := range full {
		parts[i] = syscall.EscapeArg(a)
	}
	params := strings.Join(parts, " ")

	verb, err := syscall.UTF16PtrFromString("runas")
	if err != nil {
		return 0, "", err
	}
	file, err := syscall.UTF16PtrFromString(exe)
	if err != nil {
		return 0, "", err
	}
	var paramPtr *uint16
	if params != "" {
		if paramPtr, err = syscall.UTF16PtrFromString(params); err != nil {
			return 0, "", err
		}
	}
	cwd, _ := os.Getwd()
	var dirPtr *uint16
	if cwd != "" {
		dirPtr, _ = syscall.UTF16PtrFromString(cwd)
	}

	info := shellExecuteInfo{
		fMask:        seeMaskNoCloseProcess | seeMaskNoAsync | seeMaskFlagNoUI,
		lpVerb:       verb,
		lpFile:       file,
		lpParameters: paramPtr,
		lpDirectory:  dirPtr,
		nShow:        swHide,
	}
	info.cbSize = uint32(unsafe.Sizeof(info))

	r1, _, e1 := procShellExecuteE.Call(uintptr(unsafe.Pointer(&info)))
	if r1 == 0 {
		// ERROR_CANCELLED (1223) is the user clicking No on the UAC
		// prompt, which deserves its own sentence rather than a bare
		// errno.
		if e1 == syscall.Errno(1223) {
			return 0, "", fmt.Errorf("disk: the elevation prompt was declined")
		}
		return 0, "", fmt.Errorf("disk: ShellExecuteExW(runas, %s): %w", exe, e1)
	}
	if info.hProcess == 0 {
		return 0, "", fmt.Errorf("disk: ShellExecuteExW returned no process handle")
	}
	defer syscall.CloseHandle(info.hProcess)

	if _, err := syscall.WaitForSingleObject(info.hProcess, infinite); err != nil {
		return 0, "", fmt.Errorf("disk: waiting for the elevated process: %w", err)
	}
	var code uint32
	if err := syscall.GetExitCodeProcess(info.hProcess, &code); err != nil {
		return 0, "", fmt.Errorf("disk: reading the elevated process exit code: %w", err)
	}

	data, rerr := os.ReadFile(logPath)
	if rerr == nil {
		_ = os.Remove(logPath)
	}
	return int(code), string(data), nil
}
