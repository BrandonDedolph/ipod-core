package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newSimCmd() *cobra.Command {
	var (
		headless bool
		record   string
		disk     string
	)

	cmd := &cobra.Command{
		Use:   "sim [disk-image]",
		Short: "Launch the host simulator (SDL2 window or headless)",
		Long: `Runs core-sim, the host build of the firmware backed by an SDL2 HAL.

In interactive mode an SDL2 window opens at 320x240; the keyboard maps
to the click wheel (arrow keys for scroll, Enter for select, Esc for
back, Shift+arrows for fast scroll).

In --headless mode there is no window — the simulator renders to a
memory framebuffer and dumps PNGs to the directory given by --record.
Used by 'core test' for visual regression.`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if len(args) == 1 {
				disk = args[0]
			}
			_ = headless
			_ = record
			_ = disk
			return errors.New("not yet implemented — pending sim HAL skeleton (PR 5)")
		},
	}

	cmd.Flags().BoolVar(&headless, "headless", false, "Run without a window; render frames to PNG")
	cmd.Flags().StringVar(&record, "record", "", "Directory to write captured frames into (with --headless)")
	return cmd
}
