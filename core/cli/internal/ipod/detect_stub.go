// Stub detect implementation used until per-platform code lands.
// Build tag here means the stub is the active implementation on every
// platform for now; per-platform files will replace this when written.

package ipod

import "errors"

func detect() (Device, error) {
	return Device{}, errors.New("ipod.Detect: not yet implemented (per-platform USB enumeration pending)")
}
