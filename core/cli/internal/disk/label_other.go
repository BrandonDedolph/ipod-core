//go:build !windows

package disk

// Every other GOOS. Windows is the only target that matters for the
// iPod's name: it is the host the device is plugged into, and the
// per-OS ways of reading a FAT label elsewhere (blkid, diskutil, the
// by-label symlinks) are three more code paths for a name nobody on
// those platforms has asked to change yet.
//
// The stubs exist so internal/app and internal/cli compile everywhere
// and their tests run everywhere; LegalLabel, ValidLabel and VolumeRoot
// — the whole naming rule, and the part with the interesting edge
// cases — are portable and live in label.go.

// VolumeLabel is not implemented off Windows.
func VolumeLabel(root string) (string, error) { return "", ErrUnsupported }

// SetVolumeLabel is not implemented off Windows.
func SetVolumeLabel(root, label string) error { return ErrUnsupported }

// VolumeDiskSerial is not implemented off Windows.
func VolumeDiskSerial(root string) (string, error) { return "", ErrUnsupported }
