package cli

import (
	"errors"
	"fmt"
	"os"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
	"github.com/spf13/cobra"
)

func newFirmwareCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "firmware",
		Short: "Low-level firmware image operations (.ipod packaging, partition format)",
		Long: `Operations on the iPod firmware-partition image format and the .ipod
transport format used by ipodpatcher-style installers.

These are building blocks; everyday flashing goes through "core flash"
and "core install", which call into this package internally.`,
	}
	cmd.AddCommand(newFirmwarePackCmd())
	cmd.AddCommand(newFirmwareUnpackCmd())
	return cmd
}

func newFirmwarePackCmd() *cobra.Command {
	var out string
	cmd := &cobra.Command{
		Use:   "pack <image.bin>",
		Short: "Wrap a raw firmware image in the .ipod transport format",
		Long: `Reads a flat firmware binary (typically produced by objcopy -O binary
from the hw-build ELF) and emits a .ipod-format file: a 4-byte big-endian
additive checksum, the 4-byte model name ("ipvd" for iPod Video), then
the image bytes.

The output is what "core install" / "core update" write to the device.`,
		Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if out == "" {
				return errors.New("--out is required")
			}
			image, err := os.ReadFile(args[0])
			if err != nil {
				return fmt.Errorf("read %s: %w", args[0], err)
			}

			f, err := os.Create(out)
			if err != nil {
				return fmt.Errorf("create %s: %w", out, err)
			}
			defer f.Close()

			if err := firmware.WriteIPodFile(f, firmware.ModelIPodVideo, firmware.ModelNameIPodVideo, image); err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(),
				"wrote %s (%d image bytes + %d header bytes)\n",
				out, len(image), firmware.IPodFileHeaderSize)
			return nil
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "", "Output .ipod path (required)")
	return cmd
}

func newFirmwareUnpackCmd() *cobra.Command {
	var out string
	cmd := &cobra.Command{
		Use:   "unpack <image.ipod>",
		Short: "Extract the raw image bytes from a .ipod file (verifying checksum)",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if out == "" {
				return errors.New("--out is required")
			}
			f, err := os.Open(args[0])
			if err != nil {
				return fmt.Errorf("open %s: %w", args[0], err)
			}
			defer f.Close()

			name, image, err := firmware.ReadIPodFile(f, firmware.ModelIPodVideo)
			if err != nil {
				return err
			}
			if err := os.WriteFile(out, image, 0o644); err != nil {
				return fmt.Errorf("write %s: %w", out, err)
			}
			fmt.Fprintf(cmd.OutOrStdout(),
				"unpacked %s (model=%q, %d image bytes) → %s\n",
				args[0], string(name[:]), len(image), out)
			return nil
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "", "Output raw image path (required)")
	return cmd
}
