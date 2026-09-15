//go:build linux && !novulkan

package app

// Snapshot needs a GPU backend, which this build does not have. See
// ErrNoGPU; TestSnapshot skips on it.
func Snapshot(st State, w, h int, out string) error { return ErrNoHeadless }

func snapshotWith(st State, w, h int, out string, tweak func(*UI)) error { return ErrNoHeadless }
