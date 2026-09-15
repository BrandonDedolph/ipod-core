//go:build linux && !novulkan

package app

// Main cannot open a window in this build: see ErrNoGPU. The whole rest
// of the package — the model, the runner, the backend, the layout code
// — is still compiled and tested here, which is the point of the split.
func Main(o Options) error {
	logf(o, "core-app: %v", ErrNoGPU)
	return ErrNoGPU
}
