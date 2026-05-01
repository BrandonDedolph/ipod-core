package cli

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/spf13/cobra"
)

func newBuildCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "build [hw|sim]",
		Short: "Build the firmware (hw) or simulator (sim)",
		Long: `Wraps meson + ninja for the C build. Equivalent to running 'make hw'
or 'make sim' from the core/ directory; provided here so the Go CLI is
the single entry point for everyday development.

Both targets share one source tree; -Dtarget switches the configuration.`,
		Args:      cobra.ExactArgs(1),
		ValidArgs: []string{"hw", "sim"},
		RunE: func(cmd *cobra.Command, args []string) error {
			target := args[0]
			if target != "hw" && target != "sim" {
				return fmt.Errorf("target must be 'hw' or 'sim', got %q", target)
			}

			coreDir, err := findCoreDir()
			if err != nil {
				return err
			}

			mk := exec.Command("make", "-C", coreDir, target)
			mk.Stdout = os.Stdout
			mk.Stderr = os.Stderr
			return mk.Run()
		},
	}
	return cmd
}

// findCoreDir walks up from the CWD looking for a directory named "core"
// containing a meson.build. This lets `core build` work from anywhere
// inside the repo, not just from the repo root.
func findCoreDir() (string, error) {
	cwd, err := os.Getwd()
	if err != nil {
		return "", err
	}

	dir := cwd
	for {
		candidate := filepath.Join(dir, "core", "meson.build")
		if _, err := os.Stat(candidate); err == nil {
			return filepath.Join(dir, "core"), nil
		}

		// Also recognize when CWD is already core/.
		if filepath.Base(dir) == "core" {
			candidate := filepath.Join(dir, "meson.build")
			if _, err := os.Stat(candidate); err == nil {
				return dir, nil
			}
		}

		parent := filepath.Dir(dir)
		if parent == dir {
			return "", fmt.Errorf("could not find core/ directory above %s", cwd)
		}
		dir = parent
	}
}
