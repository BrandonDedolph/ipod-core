package app

import "errors"

// ErrNoGPU is returned by Main and Snapshot in a build that has no Gio
// GPU backend compiled in.
//
// This is a build-tag situation, not a missing feature. Gio's Linux
// backend compiles a Vulkan path by default and `vulkan/vulkan.h` is
// not installed on every machine that can perfectly well run the app
// through OpenGL ES — this WSL box being one. `-tags novulkan` drops
// the Vulkan path and the build succeeds; without it the two files that
// import gioui.org/app and gioui.org/gpu/headless are excluded from a
// Linux build entirely, so `go build ./...` and `go test ./...` still
// work on a machine that cannot compile them and would otherwise fail
// with a C error nobody reading Go would recognise.
//
// Windows and macOS builds are unaffected: their backends never import
// the Vulkan headers, so the real files are always in.
var ErrNoGPU = errors.New("core-app: this build has no GPU backend — " + gpuAdvice)

// ErrNoHeadless is Snapshot's failure to get an offscreen surface:
// either the build has no GPU backend (ErrNoGPU's situation) or the
// machine has no EGL/GL at all. Either way it is a fact about the
// machine, so the snapshot test skips on it rather than failing.
var ErrNoHeadless = errors.New("core-app: no headless GPU surface — " + gpuAdvice)
