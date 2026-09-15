//go:build !windows && !linux && !darwin

package disk

// Every other GOOS. The package still compiles so that `go build` for,
// say, freebsd does not fail on an import; it simply has no devices.
// Returning an empty list rather than an error keeps `core info` able
// to say "no iPod found" instead of crashing on a platform nobody has
// asked for yet.

func listDisks() ([]Disk, error) { return nil, nil }

func openRaw(path string, write bool) (rawDevice, error) { return nil, ErrUnsupported }
