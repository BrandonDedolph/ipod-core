// Command core is the unified host-side CLI for the iPod Video firmware
// project. One binary handles install, update, recovery, dev iteration,
// the simulator, and the test suite.
//
// See PLAN.md for the design; see core/cli/README.md for usage.
package main

import (
	"errors"
	"fmt"
	"os"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cli"
)

func main() {
	// Build the root first and report through ITS error writer rather
	// than os.Stderr directly: --elevated-log retargets that writer, and
	// the elevated child's console closes the instant it exits, so an
	// error printed to the real stderr is an error nobody ever sees.
	root := cli.Root()
	if err := root.Execute(); err != nil {
		fmt.Fprintln(root.ErrOrStderr(), "error:", err)
		// `core flash` on Windows does its work in an elevated child
		// process. That child's exit code has to reach the shell that
		// ran the parent, or a script cannot tell a flash that failed
		// inside the UAC window from one that succeeded.
		var exit *cli.ExitError
		if errors.As(err, &exit) && exit.Code != 0 {
			os.Exit(exit.Code)
		}
		os.Exit(1)
	}
}
