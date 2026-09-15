package cli

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
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
	cmd.AddCommand(newFirmwareInspectCmd())
	cmd.AddCommand(newFirmwareReadCmd())
	return cmd
}

// claimOutputPath enforces the no-clobber rule for a command's output.
//
// Without --force we create the path with O_EXCL, which is the only
// atomic "this name was not already taken" primitive we have; the
// zero-byte placeholder it leaves behind is renamed over by
// writeFileAtomic, or removed if anything later fails. Reports whether
// a placeholder was created so the caller knows what to clean up.
func claimOutputPath(path string, force bool) (created bool, err error) {
	if force {
		return false, nil
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if err != nil {
		if os.IsExist(err) {
			return false, fmt.Errorf("%s already exists; pass --force to overwrite", path)
		}
		return false, fmt.Errorf("create %s: %w", path, err)
	}
	if err := f.Close(); err != nil {
		_ = os.Remove(path)
		return false, fmt.Errorf("create %s: %w", path, err)
	}
	return true, nil
}

// writeFileAtomic produces `path` such that it is either the previous
// content or the complete new content — never a truncated prefix.
//
// This matters because "make ipod" runs `firmware pack --force` on every
// build. Opening the destination with O_TRUNC destroys the last known-good
// core.ipod before the first byte of the replacement is written, and the
// 8-byte header goes out ahead of the image, so a failure mid-write leaves
// a file that still parses as a valid .ipod header with a truncated image
// behind it. That is exactly the artifact you do not want to flash.
//
// So: write to a temp file in the destination directory, fsync it, read it
// back and hand it to `verify`, and only then rename it into place. Rename
// within a directory is atomic, so a reader either sees the old file or the
// new one. Any failure removes the temp file (and the O_EXCL placeholder,
// if we made one) and leaves the previous output untouched.
func writeFileAtomic(path string, force bool, write func(io.Writer) error, verify func([]byte) error) error {
	claimed, err := claimOutputPath(path, force)
	if err != nil {
		return err
	}
	cleanupClaim := func() {
		if claimed {
			_ = os.Remove(path)
		}
	}

	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, "."+filepath.Base(path)+".tmp-*")
	if err != nil {
		cleanupClaim()
		return fmt.Errorf("create temp file in %s: %w", dir, err)
	}
	tmpName := tmp.Name()
	fail := func(err error) error {
		_ = tmp.Close()
		_ = os.Remove(tmpName)
		cleanupClaim()
		return err
	}

	if err := write(tmp); err != nil {
		return fail(err)
	}
	// fsync before trusting the bytes. Without it the read-back below can
	// be served entirely from the page cache and "verify" data the kernel
	// later fails to write out.
	if err := tmp.Sync(); err != nil {
		return fail(fmt.Errorf("fsync %s: %w", tmpName, err))
	}
	// Check Close explicitly: on many filesystems this is where a deferred
	// write finally reports ENOSPC or EIO. A deferred, unchecked
	// `defer f.Close()` turns that into a silent success and hands the
	// user a truncated image to flash.
	if err := tmp.Close(); err != nil {
		return fail(fmt.Errorf("close %s: %w", tmpName, err))
	}
	// os.CreateTemp makes the file 0600; the direct-write path this
	// replaced produced 0644, and the output is an artifact meant to be
	// read and copied around, not a secret.
	if err := os.Chmod(tmpName, 0o644); err != nil {
		return fail(fmt.Errorf("chmod %s: %w", tmpName, err))
	}
	if verify != nil {
		data, err := os.ReadFile(tmpName)
		if err != nil {
			return fail(fmt.Errorf("read back %s: %w", tmpName, err))
		}
		if err := verify(data); err != nil {
			return fail(err)
		}
	}
	if err := os.Rename(tmpName, path); err != nil {
		return fail(fmt.Errorf("rename %s -> %s: %w", tmpName, path, err))
	}
	syncDir(dir)
	return nil
}

// syncDir best-effort fsyncs a directory so the rename itself is durable.
// Failure is ignored: opening a directory for read is not portable (it
// fails on Windows), and a non-durable-but-correct rename is still an
// improvement over a truncating write.
func syncDir(dir string) {
	d, err := os.Open(dir)
	if err != nil {
		return
	}
	_ = d.Sync()
	_ = d.Close()
}

// The plausibility rules for a raw firmware image now live in
// internal/firmware, because `core flash` applies the same ones to a
// .bin handed straight to it and two copies of "is this really a
// firmware image?" is exactly the kind of pair that drifts. These are
// the names this package already used, kept so the pack command and its
// tests read unchanged.
const (
	minPackImageBytes = firmware.MinImageBytes
	maxPackImageBytes = firmware.MaxImageBytes
)

func validatePackImage(path string, image []byte, warn io.Writer) error {
	return firmware.ValidateImage(path, "pack", image, warn)
}

func uniformByte(b []byte) (byte, bool)    { return firmware.UniformByte(b) }
func firstWordLE(image []byte) uint32      { return firmware.FirstWordLE(image) }
func looksLikeARMBranch(image []byte) bool { return firmware.LooksLikeARMBranch(image) }

func newFirmwarePackCmd() *cobra.Command {
	var (
		out   string
		force bool
	)
	cmd := &cobra.Command{
		Use:   "pack <image.bin>",
		Short: "Wrap a raw firmware image in the .ipod transport format",
		Long: `Reads a flat firmware binary (typically produced by objcopy -O binary
from the hw-build ELF) and emits a .ipod-format file: a 4-byte big-endian
additive checksum, the 4-byte model name ("ipvd" for iPod Video), then
the image bytes.

The input is sanity-checked first (non-empty, plausible size, not a blank
buffer) so a failed objcopy can't produce a "valid" .ipod. The output is
written to a temp file, fsynced, read back and re-verified, and only then
renamed into place — an interrupted pack leaves the previous image intact
rather than a truncated one that still parses.

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
			if err := validatePackImage(args[0], image, cmd.ErrOrStderr()); err != nil {
				return err
			}

			err = writeFileAtomic(out, force,
				func(w io.Writer) error {
					return firmware.WriteIPodFile(w,
						firmware.ModelIPodVideo, firmware.ModelNameIPodVideo, image)
				},
				func(data []byte) error {
					name, got, err := firmware.ReadIPodFile(bytes.NewReader(data))
					if err != nil {
						return fmt.Errorf("read-back verification of %s failed: %w", out, err)
					}
					if name != firmware.ModelNameIPodVideo {
						return fmt.Errorf("read-back verification of %s failed: model %q, want %q",
							out, string(name[:]), string(firmware.ModelNameIPodVideo[:]))
					}
					if !bytes.Equal(got, image) {
						return fmt.Errorf("read-back verification of %s failed: "+
							"%d image bytes on disk, %d written", out, len(got), len(image))
					}
					return nil
				})
			if err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(),
				"wrote %s (%d image bytes + %d header bytes, checksum verified)\n",
				out, len(image), firmware.IPodFileHeaderSize)
			return nil
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "", "Output .ipod path (required)")
	cmd.Flags().BoolVarP(&force, "force", "f", false,
		"Overwrite the output file if it already exists")
	return cmd
}

func newFirmwareUnpackCmd() *cobra.Command {
	var (
		out             string
		force           bool
		ignoreChecksum  bool
		ignoreModelName bool
	)
	cmd := &cobra.Command{
		Use:   "unpack <image.ipod>",
		Short: "Extract the raw image bytes from a .ipod file (verifying checksum)",
		Long: `Extracts the image payload from a .ipod-format file, verifying the
embedded additive checksum against the model seed.

ReadIPodFile deliberately returns the image bytes alongside a checksum
error so recovery tooling can inspect a damaged image. Pass
--ignore-checksum to actually write those bytes out; the command prints
a loud warning and the result must not be flashed to a device.`,
		Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			if out == "" {
				return errors.New("--out is required")
			}
			f, err := os.Open(args[0])
			if err != nil {
				return fmt.Errorf("open %s: %w", args[0], err)
			}
			defer f.Close()

			name, image, err := firmware.ReadIPodFile(f)
			if err != nil {
				recoverable := (ignoreChecksum && errors.Is(err, firmware.ErrIPodChecksumMismatch)) ||
					(ignoreModelName && errors.Is(err, firmware.ErrUnknownModelName))
				if !recoverable || image == nil {
					return err
				}
				fmt.Fprintf(cmd.ErrOrStderr(),
					"WARNING: %v\n"+
						"WARNING: writing the image anyway because you asked; these bytes are\n"+
						"WARNING: UNVERIFIED and must not be flashed to a device.\n", err)
			}
			werr := writeFileAtomic(out, force,
				func(w io.Writer) error {
					if _, err := w.Write(image); err != nil {
						return fmt.Errorf("write %s: %w", out, err)
					}
					return nil
				},
				func(data []byte) error {
					if !bytes.Equal(data, image) {
						return fmt.Errorf("read-back verification of %s failed: "+
							"%d bytes on disk, %d written", out, len(data), len(image))
					}
					return nil
				})
			if werr != nil {
				return werr
			}
			fmt.Fprintf(cmd.OutOrStdout(),
				"unpacked %s (model=%q, %d image bytes) → %s\n",
				args[0], string(name[:]), len(image), out)
			return nil
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "", "Output raw image path (required)")
	cmd.Flags().BoolVarP(&force, "force", "f", false,
		"Overwrite the output file if it already exists")
	cmd.Flags().BoolVar(&ignoreChecksum, "ignore-checksum", false,
		"Write the image even if the embedded checksum does not verify (recovery only)")
	cmd.Flags().BoolVar(&ignoreModelName, "ignore-model", false,
		"Write the image even if the embedded model name is unknown (recovery only)")
	return cmd
}

// --- firmware inspect -------------------------------------------------

// inspectKind is what a file handed to "firmware inspect" turned out to
// be. The three are told apart by their first bytes, in this order.
type inspectKind int

const (
	// kindPartition: the Apple preamble is present, so these bytes are
	// (the start of) firmware partition 0 — a whole-partition dump
	// from ipodpatcher -r or from "core backup".
	kindPartition inspectKind = iota
	// kindIPodFile: the 8-byte .ipod transport header, whose model
	// name sits at bytes 4..8.
	kindIPodFile
	// kindRawImage: anything else. A flat core.bin is not
	// self-identifying, so this is the fallback, not a positive match.
	kindRawImage
)

// classify decides what kind of file this is from its leading bytes.
//
// Order matters: a partition dump also has bytes at 4..8, and a .ipod
// file has no preamble, so the preamble is tested first and the .ipod
// header second.
//
// A .ipod file is recognized by its model name alone, not by a matching
// checksum. Detection and verification are different questions — a
// corrupt .ipod is still a .ipod, and "this is a raw image" is a much
// worse answer to give about one than "this .ipod's checksum is BAD".
func classify(head []byte) inspectKind {
	if fwpart.CheckPreamble(head) == nil {
		return kindPartition
	}
	if len(head) >= firmware.IPodFileHeaderSize {
		var name firmware.ModelName
		copy(name[:], head[4:8])
		if _, ok := firmware.ModelNumForName(name); ok {
			return kindIPodFile
		}
	}
	return kindRawImage
}

func newFirmwareInspectCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "inspect <partition.bin | image.ipod | core.bin>",
		Short: "Describe a firmware partition dump, a .ipod file, or a raw image",
		Long: `Reads a file and reports what it is.

A whole-partition dump (ipodpatcher -r, or "core backup") prints the
image directory the way ipodpatcher -l does — one row per image, with
the body offset (devOffset + 0x800), the length, and the stored checksum
next to a fresh sum of the body, so a corrupt or stale image shows up as
BAD. Apple leaves its AUPD and HIBE entries stale on a shipping device,
so BAD on those two is normal and means nothing about the OS image; OSOS
and RSRC are the rows that must verify.

A .ipod file prints its transport header: the big-endian checksum, the
model name, and whether the checksum recomputes. Note the two checksums
in play are different — the .ipod header is seeded with the model number
(5 for the Video), the partition directory entry is a plain sum with no
seed.

A raw image prints its size, the plain sum a directory entry would have
to carry for it, and the .ipod header it would be packed with.

This command only reads. It never opens a device and never writes.`,
		Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			path := args[0]
			f, err := os.Open(path)
			if err != nil {
				return fmt.Errorf("open %s: %w", path, err)
			}
			defer f.Close()
			st, err := f.Stat()
			if err != nil {
				return fmt.Errorf("stat %s: %w", path, err)
			}
			size := st.Size()
			if size == 0 {
				return fmt.Errorf("%s is empty", path)
			}

			headLen := int64(fwpart.PreambleWindow)
			if size < headLen {
				headLen = size
			}
			head := make([]byte, headLen)
			if _, err := f.ReadAt(head, 0); err != nil && !errors.Is(err, io.EOF) {
				return fmt.Errorf("read %s: %w", path, err)
			}

			out := cmd.OutOrStdout()
			switch classify(head) {
			case kindPartition:
				return inspectPartition(out, path, fwpart.Partition{R: f, Size: size})
			case kindIPodFile:
				return inspectIPodFile(out, path, f, size)
			default:
				return inspectRawImage(out, path, f, size)
			}
		},
	}
	return cmd
}

func inspectPartition(out io.Writer, path string, p fwpart.Partition) error {
	fmt.Fprintf(out, "%s: iPod firmware partition, %d bytes\n", path, p.Size)
	_, err := printPartitionDirectory(out, p)
	return err
}

// printPartitionDirectory is the shared directory printer: the table
// "firmware inspect" prints for a dump on disk, and the one "core info"
// prints for the partition on a connected device. One function because
// they are the same table of the same thing — the first version of this
// had two, and they had already drifted on whether AUPD/HIBE being BAD
// deserved a warning.
//
// It returns the parsed directory so a caller that also wants to answer
// questions about it (info --json) does not parse twice.
func printPartitionDirectory(out io.Writer, p fwpart.Partition) (*fwpart.Directory, error) {
	d, err := fwpart.Parse(p)
	if err != nil {
		return nil, err
	}
	fmt.Fprintf(out, "  preamble   OK (Apple banner, %q at %#x)\n",
		firmware.DirectoryMarker[:], firmware.DirectoryMarkerOffset)
	fmt.Fprintf(out, "  directory  version %d at %#x, %d images\n\n",
		d.Version, d.Start, len(d.Entries))

	const row = "  %-2s %-4s %-4s %9s %9s %9s %9s %10s %9s %7s %10s %10s %s\n"
	fmt.Fprintf(out, row, "#", "type", "cont", "entryAt", "devOffset", "body",
		"len", "addr", "entryOff", "vers", "chksum", "recomputed", "state")
	for i, e := range d.Entries {
		state := "OK"
		recomputed := "-"
		sum, err := fwpart.EntryChecksum(p, e)
		switch {
		case err != nil:
			state = "UNREADABLE"
		case sum != e.Checksum:
			state, recomputed = "BAD", fmt.Sprintf("%#08x", sum)
		default:
			recomputed = fmt.Sprintf("%#08x", sum)
		}
		fmt.Fprintf(out, row,
			strconv.Itoa(i),
			strings.ToUpper(e.LogicalImageType()),
			string(e.ContainerID[:]),
			fmt.Sprintf("%#x", d.EntryOffset(i)),
			fmt.Sprintf("%#x", e.DevOffset),
			fmt.Sprintf("%#x", fwpart.BodyOffset(e)),
			strconv.FormatUint(uint64(e.Length), 10),
			fmt.Sprintf("%#08x", e.LoadAddr),
			fmt.Sprintf("%#x", e.EntryOffset),
			fmt.Sprintf("%#x", e.Version),
			fmt.Sprintf("%#08x", e.Checksum),
			recomputed, state)
	}

	idx, osos, ok := d.OSOS()
	if !ok {
		fmt.Fprintf(out, "\n  no OSOS image in this directory\n")
		return d, nil
	}
	capacity := d.Capacity(idx)
	limit := fwpart.BodyOffset(osos) + int64(capacity)
	fmt.Fprintf(out, "\n  OSOS capacity %d bytes (body %#x .. %#x); image uses %d, %d free\n",
		capacity, fwpart.BodyOffset(osos), limit, osos.Length, int64(capacity)-int64(osos.Length))
	if err := fwpart.VerifyEntry(p, osos); err != nil {
		fmt.Fprintf(out, "  OSOS checksum does NOT verify: %v\n", err)
	}
	return d, nil
}

func inspectIPodFile(out io.Writer, path string, r io.ReaderAt, size int64) error {
	data := make([]byte, size)
	if _, err := r.ReadAt(data, 0); err != nil && !errors.Is(err, io.EOF) {
		return fmt.Errorf("read %s: %w", path, err)
	}
	name, image, err := firmware.ReadIPodFile(bytes.NewReader(data))
	stored := binary.BigEndian.Uint32(data[0:4])
	seed, _ := firmware.ModelNumForName(name)

	fmt.Fprintf(out, "%s: .ipod transport file, %d bytes\n", path, size)
	fmt.Fprintf(out, "  model      %q (checksum seed %d)\n", string(name[:]), seed)
	fmt.Fprintf(out, "  image      %d bytes\n", len(image))
	fmt.Fprintf(out, "  header sum %#08x stored", stored)
	switch {
	case err == nil:
		fmt.Fprintf(out, ", recomputes — OK\n")
	case errors.Is(err, firmware.ErrIPodChecksumMismatch):
		fmt.Fprintf(out, ", %#08x computed — BAD\n", firmware.Checksum(seed, image))
	default:
		fmt.Fprintf(out, " — %v\n", err)
	}
	fmt.Fprintf(out, "  plain sum  %#08x (what a directory entry would carry; no seed)\n",
		fwpart.ImageChecksum(image))
	fmt.Fprintf(out, "  body write %d bytes once zero-padded to %#x\n",
		padTo(len(image), fwpart.BodyAlign), fwpart.BodyAlign)
	return nil
}

func inspectRawImage(out io.Writer, path string, r io.ReaderAt, size int64) error {
	image := make([]byte, size)
	if _, err := r.ReadAt(image, 0); err != nil && !errors.Is(err, io.EOF) {
		return fmt.Errorf("read %s: %w", path, err)
	}
	sum := fwpart.ImageChecksum(image)
	fmt.Fprintf(out, "%s: raw firmware image (no preamble, no .ipod header), %d bytes\n",
		path, size)
	fmt.Fprintf(out, "  plain sum  %#08x (what a directory entry's chksum must carry)\n", sum)
	fmt.Fprintf(out, "  first word %#08x", firstWordLE(image))
	if looksLikeARMBranch(image) {
		fmt.Fprintf(out, " (ARM branch)\n")
	} else {
		fmt.Fprintf(out, " (not an ARM branch)\n")
	}
	fmt.Fprintf(out, "  .ipod      header would be %#08x big-endian + %q (sum + seed %d), file %d bytes\n",
		firmware.Checksum(firmware.ModelIPodVideo, image),
		string(firmware.ModelNameIPodVideo[:]), firmware.ModelIPodVideo,
		int(size)+firmware.IPodFileHeaderSize)
	fmt.Fprintf(out, "  body write %d bytes once zero-padded to %#x\n",
		padTo(len(image), fwpart.BodyAlign), fwpart.BodyAlign)
	return nil
}

// padTo rounds n up to a multiple of align.
func padTo(n, align int) int { return (n + align - 1) &^ (align - 1) }
