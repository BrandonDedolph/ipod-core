package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newUpdateCmd() *cobra.Command {
	var (
		zip      string
		checkSig bool
		self     bool
	)

	cmd := &cobra.Command{
		Use:   "update [release-zip]",
		Short: "Update an iPod already running our firmware",
		Long: `Updates the firmware on a connected iPod that's already running our
build. The iPod must be in update mode (USB connected, "Connected —
safe to update" screen visible).

By default fetches the latest release from GitHub Releases, verifies
the signature, and applies it atomically (writes new files alongside
old, then renames into place). Keeps /core.ipod.prev as a fallback
the bootloader uses if the new image fails to boot N times.

Use --self to update the host CLI itself (the 'core' binary) rather
than the iPod's firmware.`,
		Args: cobra.MaximumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if len(args) == 1 {
				zip = args[0]
			}
			_ = zip
			_ = checkSig
			if self {
				return errors.New("not yet implemented — self-update")
			}
			return errors.New("not yet implemented — needs hardware in the loop")
		},
	}

	cmd.Flags().BoolVar(&checkSig, "verify", true, "Verify release-zip signature against embedded public key")
	cmd.Flags().BoolVar(&self, "self", false, "Update the host CLI binary instead of the iPod firmware")
	return cmd
}
