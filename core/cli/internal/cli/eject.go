package cli

import (
	"fmt"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/eject"
	"github.com/spf13/cobra"
)

// newEjectCmd builds "core eject".
//
// A sync flushes every file it writes, but the operating system still holds
// the volume's own metadata caches, and on Windows the disk's write cache is
// only emptied by a dismount. Pulling the cable before that is how a FAT
// volume ends up with a directory that names files whose clusters were never
// written — and the firmware cannot repair a filesystem, it can only refuse to
// mount one. So the last step of the daily flow is its own command.
//
// What it does per platform is deliberately visible in the output: the user is
// about to unplug hardware and deserves to know whether the volume was really
// dismounted or the command only asked politely.
//
// The per-OS bodies live in internal/eject, because core-app has the
// same button and the two must not drift.
func newEjectCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "eject <drive-or-mount>",
		Short: "Flush and eject the iPod's volume so the cable can be pulled",
		Long: `Flushes the volume's caches and ejects it.

  Windows   D:   — locks the volume, dismounts it, allows media removal
                   and ejects, all through the volume handle. If that is
                   refused (something still has a file open), it falls back
                   to the Shell "Eject" verb, the same one Explorer uses.
  Linux     /dev/sdb1 or a mount point — udisksctl unmount + power-off
                   when udisksctl is installed; otherwise the exact commands
                   to run are printed.
  macOS     /dev/disk4 or a mount point — diskutil eject.

Always flushes first.`,
		Args:         cobra.ExactArgs(1),
		SilenceUsage: true,
		RunE: func(cmd *cobra.Command, args []string) error {
			if args[0] == "" {
				return fmt.Errorf("name the volume to eject (a drive letter, a device node or a mount point)")
			}
			return eject.Eject(cmd.OutOrStdout(), args[0])
		},
	}
	return cmd
}
