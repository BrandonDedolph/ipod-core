package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newTestCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "test [layer]",
		Short: "Run the test suite (unit / sim / hw / battery / soak)",
		Long: `Five layers per PLAN.md:

  unit     pure functions, run on host (codec KAT, layout math, etc.)
  sim      sim-driven integration: boot timing, golden-frame diffs
  hw       hardware smoke checklist (manual; logs to tests/runs/)
  battery  battery-life regression (dedicated device, weekly)
  soak     overnight sim run, leak / high-water detection

With no argument, runs unit + sim (the CI gates).`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			layer := "ci"
			if len(args) == 1 {
				layer = args[0]
			}
			switch layer {
			case "ci", "unit", "sim", "hw", "battery", "soak":
				return errors.New("not yet implemented — pending sim and KAT harness")
			default:
				return errors.New("unknown test layer: " + layer)
			}
		},
	}
	return cmd
}
