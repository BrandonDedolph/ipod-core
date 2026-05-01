// Package version exposes the build version of the core CLI.
//
// At build time the linker stamps these via -ldflags. Without
// stamping the strings remain "(devel)" and "unknown".
package version

import (
	"fmt"
	"runtime"
)

var (
	// Version is the semver tag at build time, or "(devel)".
	Version = "(devel)"

	// Commit is the git SHA at build time, or "unknown".
	Commit = "unknown"

	// Date is the build date (RFC3339), or "unknown".
	Date = "unknown"
)

// Full returns a human-readable version string for the --version flag.
func Full() string {
	return fmt.Sprintf("%s (commit %s, built %s, %s/%s)",
		Version, Commit, Date, runtime.GOOS, runtime.GOARCH)
}
