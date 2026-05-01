// Stub detect implementation, active on every platform until real
// per-platform USB enumeration lands.
//
// The build tag below means: this file is included on every platform
// EXCEPT the ones that already have a real implementation. As soon as
// detect_linux.go lands, add `linux` to the exclusion list here so we
// don't get a duplicate `detect()` symbol; same when darwin/windows
// gain real implementations.
//
//go:build !corehas_real_detect

package ipod

import "errors"

func detect() (Device, error) {
	return Device{}, errors.New("ipod.Detect: not yet implemented (per-platform USB enumeration pending)")
}
