//go:build !windows && !linux && !darwin

package eject

import (
	"fmt"
	"io"
	"runtime"
)

// Eject has no implementation on this OS. Saying so is the whole point:
// the alternative is a command that exits 0 and lets the user pull the cable.
func Eject(w io.Writer, target string) error {
	return fmt.Errorf("eject is not implemented on %s; unmount %s with the system's own tool before unplugging",
		runtime.GOOS, target)
}
