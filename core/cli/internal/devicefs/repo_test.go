// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// repoRoot locates the ipod_theme tree: $CORE_REPO when set, otherwise by
// walking up from this file's directory looking for the firmware sources.
// Returns "" when there is none — every parity test here SKIPS in that case
// rather than failing, so the package still tests clean from a source
// tarball or a vendored checkout (plan §2, cross-cutting rules).
func repoRoot(t *testing.T) string {
	t.Helper()
	if r := os.Getenv("CORE_REPO"); r != "" {
		if isRepo(r) {
			return r
		}
		t.Fatalf("CORE_REPO=%s does not look like the ipod_theme tree", r)
	}
	dir, err := os.Getwd()
	if err != nil {
		return ""
	}
	for {
		if isRepo(dir) {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return ""
		}
		dir = parent
	}
}

func isRepo(dir string) bool {
	for _, f := range []string{
		filepath.Join(dir, "core", "kernel", "config.c"),
		filepath.Join(dir, "core", "ui", "settings.c"),
		filepath.Join(dir, "tools", "make_config.py"),
	} {
		if _, err := os.Stat(f); err != nil {
			return false
		}
	}
	return true
}

// python3 returns the interpreter to shell out to, or "" when there is none.
func python3(t *testing.T) string {
	t.Helper()
	p, err := exec.LookPath("python3")
	if err != nil {
		return ""
	}
	return p
}
