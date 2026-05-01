package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newDebugCmd() *cobra.Command {
	var (
		port string
		baud int
	)

	cmd := &cobra.Command{
		Use:   "debug",
		Short: "Stream the dock-connector UART log",
		Long: `Opens the dock-connector UART (via a 30-pin → serial breakout) and
prints whatever the firmware is sending. Default 115200 8-N-1.

This is the primary debug channel when the LCD is broken or the
firmware has panicked too early to render its panic screen.`,
		RunE: func(cmd *cobra.Command, args []string) error {
			_ = port
			_ = baud
			return errors.New("not yet implemented — needs serial driver")
		},
	}
	cmd.Flags().StringVar(&port, "port", "", "Serial device path (autodetect if empty)")
	cmd.Flags().IntVar(&baud, "baud", 115200, "Baud rate")
	return cmd
}
