package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newInstallCmd() *cobra.Command {
	var (
		yes      bool
		bootonly bool
	)

	cmd := &cobra.Command{
		Use:   "install",
		Short: "First-time install onto a stock iPod",
		Long: `Installs the bootloader and our firmware on a stock iPod (Apple OS or
Rockbox).

The flow:
  1. Detect the connected iPod via USB (model, capacity, firmware mode).
  2. Prompt for confirmation (skip with --yes).
  3. Prompt for OS-level elevation (sudo / pkexec / UAC) since writing
     the firmware partition needs raw block-device access.
  4. Write our bootloader to the firmware partition.
  5. Mount the data partition and write /core.ipod + /.core/ assets.
  6. Unmount cleanly; tell the user to disconnect and reboot.

Recovery is documented in RECOVERY.md (Select+Play boots into Apple
disk mode regardless of firmware state, so this is reversible).`,
		RunE: func(cmd *cobra.Command, args []string) error {
			_ = yes
			_ = bootonly
			return errors.New("not yet implemented — needs hardware in the loop")
		},
	}

	cmd.Flags().BoolVarP(&yes, "yes", "y", false, "Skip confirmation prompts")
	cmd.Flags().BoolVar(&bootonly, "bootloader-only", false, "Install only the bootloader (skip firmware + assets)")
	return cmd
}
