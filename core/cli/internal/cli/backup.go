package cli

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flasher"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/spf13/cobra"
)

func newBackupCmd() *cobra.Command {
	var out string
	cmd := &cobra.Command{
		Use:   "backup",
		Short: "Dump the whole firmware partition to a file",
		Long: `Copies the entire firmware partition — preamble, image directory, and
every image including Apple's — to a file, byte for byte. This is the
same bytes as ipodpatcher's -r, and it is the artifact "core flash
--from-backup" restores from.

The whole partition, not just our image, because the preamble and
Apple's RSRC/AUPD/HIBE images cannot be rebuilt from anything: the only
copy of a given device's is the one on that device.

The file is written to a temp name, fsynced, and renamed into place, so
an interrupted backup leaves no half-file that looks like a backup. An
existing file is never overwritten — a backup you can clobber is not a
backup.

Default output:
  <user config dir>/core/backups/fwpart-<bytes>-<timestamp>.bin

Read-only with respect to the device.`,
		Args: cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			pod, h, err := openDevice(cmd)
			if err != nil {
				return err
			}
			defer h.Close()

			if out == "" {
				out, err = defaultBackupPath(pod.FWPartLen, time.Now())
				if err != nil {
					return err
				}
			}
			if dir := filepath.Dir(out); dir != "" {
				if err := os.MkdirAll(dir, 0o755); err != nil {
					return fmt.Errorf("create %s: %w", dir, err)
				}
			}
			return runBackup(cmd.OutOrStdout(), pod, h, out)
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "",
		"Output file (default: <user config dir>/core/backups/fwpart-<bytes>-<timestamp>.bin)")
	return cmd
}

func runBackup(w io.Writer, pod disk.IPod, h disk.Handle, out string) error {
	describeDevice(w, pod)
	fmt.Fprintf(w, "\nreading partition 0: %d bytes at %#x\n", pod.FWPartLen, pod.FWPartStart)

	src := io.NewSectionReader(h, pod.FWPartStart, pod.FWPartLen)
	err := writeFileAtomic(out, false, func(dst io.Writer) error {
		n, err := io.Copy(dst, src)
		if err != nil {
			return fmt.Errorf("read %s: %w", pod.Disk.Path, err)
		}
		if n != pod.FWPartLen {
			return fmt.Errorf("read %d of %d bytes from %s", n, pod.FWPartLen, pod.Disk.Path)
		}
		return nil
	}, nil)
	if err != nil {
		return err
	}

	fmt.Fprintf(w, "wrote %s (%d bytes)\n", out, pod.FWPartLen)
	return reportBackupHealth(w, out)
}

// reportBackupHealth parses the file that was just written, not the
// device it came from.
//
// That is the point: a backup nobody has read back is a file, not a
// backup. Parsing it proves the preamble, the directory and the OSOS
// checksum survived the copy — and it proves them about the bytes on
// disk, after the fsync, rather than about the bytes we believe we
// read.
func reportBackupHealth(w io.Writer, path string) error {
	f, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("re-open the backup to verify it: %w", err)
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return fmt.Errorf("stat the backup: %w", err)
	}
	p := fwpart.Partition{R: f, Size: st.Size()}
	d, err := fwpart.Parse(p)
	if err != nil {
		return fmt.Errorf("the backup does not parse as a firmware partition: %w", err)
	}
	fmt.Fprintf(w, "verified    preamble OK, directory version %d at %#x, %d images\n",
		d.Version, d.Start, len(d.Entries))

	_, osos, ok := d.OSOS()
	if !ok {
		fmt.Fprintf(w, "verified    no OSOS entry in the backup — do NOT restore from this file\n")
		return fmt.Errorf("the backup has no OSOS image")
	}
	if err := fwpart.VerifyEntry(p, osos); err != nil {
		// Not fatal: a device with a bad OSOS checksum is exactly the
		// device someone is backing up before trying to fix it, and
		// refusing to keep the backup would be the wrong way round.
		fmt.Fprintf(w, "verified    OSOS %d bytes, checksum %#08x does NOT verify: %v\n",
			osos.Length, osos.Checksum, err)
		return nil
	}
	fmt.Fprintf(w, "verified    OSOS %d bytes, checksum %#08x OK\n", osos.Length, osos.Checksum)
	return nil
}

// backupFileName and defaultBackupPath delegate to internal/flasher.
// `core flash` takes the same kind of backup, to the same default
// place, and two implementations of "where do backups go" is how a
// restore ends up looking in the wrong directory.
func backupFileName(sizeBytes int64, t time.Time) string {
	return flasher.BackupFileName(sizeBytes, t)
}

func defaultBackupPath(sizeBytes int64, t time.Time) (string, error) {
	dir, err := flasher.DefaultBackupDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, backupFileName(sizeBytes, t)), nil
}
