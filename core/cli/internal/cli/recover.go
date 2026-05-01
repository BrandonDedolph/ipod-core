package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newRecoverCmd() *cobra.Command {
	var (
		reflash    bool
		applyApple string
	)

	cmd := &cobra.Command{
		Use:   "recover",
		Short: "Restore factory firmware or reflash ours",
		Long: `Recovery path for a bricked or misbehaving iPod. The iPod must be in
Apple disk mode — hold Select+Play at boot to enter it (works
regardless of firmware state, since it's in ROM).

Default action restores the factory bootloader so the iPod boots
Apple's OS again. Use --reflash to re-install our firmware fresh.
Use --apple-firmware to point at a saved Apple firmware blob if the
factory firmware on the device is also damaged (Apple no longer hosts
these blobs; users keep their own copies).`,
		RunE: func(cmd *cobra.Command, args []string) error {
			_ = reflash
			_ = applyApple
			return errors.New("not yet implemented — needs hardware in the loop")
		},
	}

	cmd.Flags().BoolVar(&reflash, "reflash", false, "Re-install our firmware (instead of restoring factory)")
	cmd.Flags().StringVar(&applyApple, "apple-firmware", "", "Path to a saved Apple firmware blob to restore")
	return cmd
}
