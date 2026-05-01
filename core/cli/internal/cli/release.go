package cli

import (
	"errors"

	"github.com/spf13/cobra"
)

func newReleaseCmd() *cobra.Command {
	var (
		ver  string
		sign bool
	)

	cmd := &cobra.Command{
		Use:   "release",
		Short: "Build, sign, and package a release zip",
		Long: `Builds the firmware, packages assets, and emits a release zip ready
for distribution via GitHub Releases.

Output:
  build/release/core-{os}-{arch}        — host CLI binaries
  build/release/core-firmware-vX.Y.Z.zip — firmware payload + assets
  build/release/checksums.txt           — signed
`,
		RunE: func(cmd *cobra.Command, args []string) error {
			_ = ver
			_ = sign
			return errors.New("not yet implemented — pending hw build path")
		},
	}
	cmd.Flags().StringVar(&ver, "version", "", "Release version (defaults to git describe)")
	cmd.Flags().BoolVar(&sign, "sign", true, "Sign artifacts with the release key")
	return cmd
}
