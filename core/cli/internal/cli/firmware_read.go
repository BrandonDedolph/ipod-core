package cli

import (
	"bytes"
	"errors"
	"fmt"
	"io"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/spf13/cobra"
)

func newFirmwareReadCmd() *cobra.Command {
	var (
		out   string
		force bool
	)
	cmd := &cobra.Command{
		Use:   "read",
		Short: "Read the OSOS image body off a connected iPod",
		Long: `Writes the running firmware image — exactly the OSOS entry's Length
bytes, starting at its devOffset + 0x800 — to a file.

This is ipodpatcher's -rfb, and the output must compare equal to it byte
for byte. Note what is NOT included: the zero padding ipodpatcher writes
up to the next 0x800 boundary is outside the entry's Length and outside
its checksum, so it is not part of the image and is not written here.
(ipodpatcher agrees: its log says "Padding read from 0x59bf8 to 0x5a000"
and the file it produces is 367,608 bytes, not 368,640.)

The result is a raw image, the same shape as core.bin: "core firmware
inspect" on it prints the plain sum a directory entry must carry.

Read-only with respect to the device.`,
		Args: cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			if out == "" {
				return errors.New("--out is required")
			}
			pod, h, err := openDevice(cmd)
			if err != nil {
				return err
			}
			defer h.Close()
			return writeOSOSBody(cmd.OutOrStdout(), firmwarePartition(pod, h), out, force)
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "", "Output raw image path (required)")
	cmd.Flags().BoolVarP(&force, "force", "f", false,
		"Overwrite the output file if it already exists")
	return cmd
}

// writeOSOSBody is the whole command minus the device plumbing, so the
// test can drive it from a synthetic partition instead of an iPod.
func writeOSOSBody(w io.Writer, p fwpart.Partition, out string, force bool) error {
	d, err := fwpart.Parse(p)
	if err != nil {
		return err
	}
	idx, osos, ok := d.OSOS()
	if !ok {
		return fwpart.ErrNoOSOS
	}
	body, err := fwpart.ReadBody(p, osos)
	if err != nil {
		return err
	}

	// The checksum is checked, and a mismatch is reported without
	// refusing to write. A device whose OSOS does not verify is
	// precisely the device whose image someone needs to get off it and
	// look at.
	sum := fwpart.ImageChecksum(body)
	err = writeFileAtomic(out, force,
		func(dst io.Writer) error {
			n, err := dst.Write(body)
			if err != nil {
				return fmt.Errorf("write %s: %w", out, err)
			}
			if n != len(body) {
				return fmt.Errorf("write %s: %d of %d bytes", out, n, len(body))
			}
			return nil
		},
		func(data []byte) error {
			if !bytes.Equal(data, body) {
				return fmt.Errorf("read-back verification of %s failed: "+
					"%d bytes on disk, %d read from the device", out, len(data), len(body))
			}
			return nil
		})
	if err != nil {
		return err
	}

	fmt.Fprintf(w, "wrote %s (%d bytes, OSOS entry %d at devOffset %#x, body %#x)\n",
		out, len(body), idx, osos.DevOffset, fwpart.BodyOffset(osos))
	if sum == osos.Checksum {
		fmt.Fprintf(w, "checksum %#08x OK (plain sum, no model seed)\n", sum)
	} else {
		fmt.Fprintf(w, "checksum MISMATCH: the entry stores %#08x, the %d bytes read sum to %#08x\n",
			osos.Checksum, len(body), sum)
	}
	return nil
}
