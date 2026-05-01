package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newFlashCmd() *cobra.Command {
	var watch bool

	cmd := &cobra.Command{
		Use:   "flash",
		Short: "Push core.ipod + assets to a connected iPod (dev iteration)",
		Long: `Detects a connected iPod (mounted as MSC), copies build/core.ipod
and the .core/ asset tree to its FAT partition, syncs, and prompts the
user to eject + reboot. Round-trip from 'core build hw' to 'watching
the new build run' is ~15s.

This is for development iteration, not first-time install. Use
'core install' for that — it also handles the bootloader.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			_ = watch
			return errors.New("not yet implemented — needs hardware in the loop")
		},
	}

	cmd.Flags().BoolVar(&watch, "watch", false, "Re-flash on every rebuild (tight inner loop)")
	return cmd
}
