// Command core is the unified host-side CLI for the iPod Video firmware
// project. One binary handles install, update, recovery, dev iteration,
// the simulator, and the test suite.
//
// See PLAN.md for the design; see core/cli/README.md for usage.
package main

import (
	"fmt"
	"os"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/cli"
)

func main() {
	if err := cli.Root().Execute(); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}
