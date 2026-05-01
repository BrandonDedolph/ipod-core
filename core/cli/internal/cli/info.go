package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newInfoCmd() *cobra.Command {
	var jsonOut bool
	cmd := &cobra.Command{
		Use:   "info",
		Short: "Detect and identify a connected iPod",
		Long: `Probes for a connected iPod over USB and reports its model, generation,
storage capacity, currently-installed firmware (Apple/Rockbox/ours),
and serial number if available.

Useful as a sanity check before 'core install' or 'core update'.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			_ = jsonOut
			return errors.New("not yet implemented — needs USB detection")
		},
	}
	cmd.Flags().BoolVar(&jsonOut, "json", false, "Emit machine-readable JSON")
	return cmd
}
