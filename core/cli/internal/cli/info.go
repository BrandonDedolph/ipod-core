package cli

import (
	"encoding/json"
	"fmt"
	"io"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
	"github.com/spf13/cobra"
)

func newInfoCmd() *cobra.Command {
	var (
		jsonOut  bool
		allDisks bool
	)
	cmd := &cobra.Command{
		Use:   "info",
		Short: "Identify the connected iPod and describe its firmware partition",
		Long: `Reports the connected iPod: disk path, model and serial, size, the
logical sector size (read from the OS and cross-checked against where
the Apple preamble actually is), the partition table, the firmware
partition's image directory with a fresh checksum per image, and the
OSOS entry's length, capacity and checksum status.

Read-only from first byte to last. It opens the disk without write
access, so it cannot modify anything even if it wanted to.

--all-disks lists every disk the OS can see instead, marking the ones
that pass iPod identification. That is the flag for "core says no iPod
but it is right there": disk enumeration needs no privilege, while
reading sector 0 does, so --all-disks still prints a full list from an
ordinary console on Windows and tells you which opens were refused.`,
		Args: cobra.NoArgs,
		RunE: func(cmd *cobra.Command, args []string) error {
			if allDisks {
				return runInfoAllDisks(cmd, jsonOut)
			}
			return runInfoDevice(cmd, jsonOut)
		},
	}
	cmd.Flags().BoolVar(&jsonOut, "json", false, "Emit machine-readable JSON")
	cmd.Flags().BoolVar(&allDisks, "all-disks", false,
		"List every disk the OS can see, not just the iPods")
	return cmd
}

// --- info (one device) ------------------------------------------------

func runInfoDevice(cmd *cobra.Command, jsonOut bool) error {
	pod, h, err := openDevice(cmd)
	if err != nil {
		return err
	}
	defer h.Close()

	p := firmwarePartition(pod, h)
	out := cmd.OutOrStdout()

	if jsonOut {
		return writeInfoJSON(out, pod, p)
	}

	describeDevice(out, pod)
	describePartitions(out, pod)
	fmt.Fprintf(out, "\nfirmware partition at %#x, %d bytes\n", pod.FWPartStart, pod.FWPartLen)
	d, err := printPartitionDirectory(out, p)
	if err != nil {
		return fmt.Errorf("reading the firmware partition on %s: %w", pod.Disk.Path, err)
	}
	fmt.Fprintf(out, "\n%s\n", firmwareVersionLine(p, d))
	return nil
}

// --- info --all-disks -------------------------------------------------

func runInfoAllDisks(cmd *cobra.Command, jsonOut bool) error {
	disks, err := disk.List()
	if err != nil {
		return fmt.Errorf("listing disks: %w", err)
	}
	pods, podErr := disk.FindIPods()
	isPod := map[string]disk.IPod{}
	for _, p := range pods {
		isPod[p.Disk.Path] = p
	}

	out := cmd.OutOrStdout()
	if jsonOut {
		type row struct {
			Disk  disk.Disk `json:"disk"`
			IsPod bool      `json:"is_ipod"`
		}
		rows := make([]row, 0, len(disks))
		for _, d := range disks {
			_, ok := isPod[d.Path]
			rows = append(rows, row{Disk: d, IsPod: ok})
		}
		enc := json.NewEncoder(out)
		enc.SetIndent("", "  ")
		return enc.Encode(rows)
	}

	if len(disks) == 0 {
		fmt.Fprintln(out, "no disks reported by the OS")
		return nil
	}
	fmt.Fprintf(out, "%-22s %-28s %-24s %-8s %s\n", "path", "model", "size", "sector", "volumes")
	for _, d := range disks {
		model := strings.TrimSpace(d.Vendor + " " + d.Model)
		if model == "" {
			model = "-"
		}
		vols := strings.Join(d.Volumes, " ")
		if vols == "" {
			vols = "-"
		}
		mark := ""
		if p, ok := isPod[d.Path]; ok {
			mark = "  <- iPod: " + p.Model
			if !p.Tested {
				mark += " (UNTESTED)"
			}
		}
		fmt.Fprintf(out, "%-22s %-28s %-24s %-8d %s%s\n",
			d.Path, truncate(model, 28), disk.HumanSize(d.SizeBytes), d.SectorSize, vols, mark)
	}
	if podErr != nil {
		// Enumeration succeeded and identification did not: on Windows
		// that is the normal unelevated result, and saying so is the
		// whole value of this flag.
		fmt.Fprintf(out, "\niPod identification could not run: %v\n", decorateDeviceError(podErr))
	} else if len(pods) == 0 {
		fmt.Fprintln(out, "\nnone of these disks is an iPod (partition 0 type 0x00 with the Apple "+
			"preamble, partition 1 FAT32, and an Apple/iPod model string)")
	}
	return nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	if n <= 1 {
		return s[:n]
	}
	return s[:n-1] + "…"
}

// --- info --json ------------------------------------------------------

// infoJSON is the machine-readable shape. It is a separate type from
// the internal structs on purpose: this is an interface other programs
// will read, and it should not change every time an internal field is
// renamed.
type infoJSON struct {
	Disk struct {
		Path         string   `json:"path"`
		Vendor       string   `json:"vendor"`
		Model        string   `json:"model"`
		Serial       string   `json:"serial"`
		SizeBytes    int64    `json:"size_bytes"`
		SectorSize   int      `json:"sector_size"`
		OSSectorHint int      `json:"os_sector_size"`
		SectorSource string   `json:"sector_size_source"`
		Removable    bool     `json:"removable"`
		USB          bool     `json:"usb"`
		Volumes      []string `json:"volumes"`
		MountPoints  []string `json:"mount_points"`
	} `json:"disk"`
	Hardware struct {
		Model          string `json:"model"`
		Tested         bool   `json:"tested"`
		UntestedReason string `json:"untested_reason,omitempty"`
	} `json:"hardware"`
	Partitions []partitionJSON `json:"partitions"`
	Firmware   struct {
		PartitionStart int64       `json:"partition_start"`
		PartitionLen   int64       `json:"partition_len"`
		DirectoryStart uint32      `json:"directory_start"`
		Version        uint16      `json:"directory_version"`
		Images         []imageJSON `json:"images"`
		OSOS           *ososJSON   `json:"osos,omitempty"`
		Version_       string      `json:"version"`
	} `json:"firmware"`
}

type partitionJSON struct {
	Index      int    `json:"index"`
	Type       byte   `json:"type"`
	TypeName   string `json:"type_name"`
	StartLBA   uint32 `json:"start_lba"`
	EndLBA     uint32 `json:"end_lba"`
	ByteStart  int64  `json:"byte_start"`
	ByteLength int64  `json:"byte_length"`
}

type imageJSON struct {
	Type       string `json:"type"`
	Container  string `json:"container"`
	DevOffset  uint32 `json:"dev_offset"`
	BodyOffset int64  `json:"body_offset"`
	Length     uint32 `json:"length"`
	LoadAddr   uint32 `json:"load_addr"`
	Checksum   uint32 `json:"checksum"`
	Recomputed uint32 `json:"recomputed"`
	ChecksumOK bool   `json:"checksum_ok"`
}

type ososJSON struct {
	Index    int    `json:"index"`
	Length   uint32 `json:"length"`
	Capacity uint32 `json:"capacity"`
	Free     int64  `json:"free"`
	Checksum uint32 `json:"checksum"`
	OK       bool   `json:"checksum_ok"`
}

func writeInfoJSON(out io.Writer, pod disk.IPod, p fwpart.Partition) error {
	var v infoJSON
	v.Disk.Path = pod.Disk.Path
	v.Disk.Vendor = pod.Disk.Vendor
	v.Disk.Model = pod.Disk.Model
	v.Disk.Serial = pod.Disk.Serial
	v.Disk.SizeBytes = pod.Disk.SizeBytes
	v.Disk.SectorSize = pod.SectorSize
	v.Disk.OSSectorHint = pod.Disk.SectorSize
	v.Disk.SectorSource = pod.SectorSizeSource
	v.Disk.Removable = pod.Disk.Removable
	v.Disk.USB = pod.Disk.USB
	v.Disk.Volumes = pod.Disk.Volumes
	v.Disk.MountPoints = pod.Disk.MountPoints
	v.Hardware.Model = pod.Model
	v.Hardware.Tested = pod.Tested
	v.Hardware.UntestedReason = pod.UntestedReason
	for _, part := range pod.Partitions {
		if !part.Used() {
			continue
		}
		v.Partitions = append(v.Partitions, partitionJSON{
			Index:      part.Index,
			Type:       part.Type,
			TypeName:   part.TypeName(),
			StartLBA:   part.StartLBA,
			EndLBA:     part.EndLBA(),
			ByteStart:  part.ByteStart(pod.SectorSize),
			ByteLength: part.ByteLength(pod.SectorSize),
		})
	}
	v.Firmware.PartitionStart = pod.FWPartStart
	v.Firmware.PartitionLen = pod.FWPartLen
	v.Firmware.Version_ = "unknown"

	d, err := fwpart.Parse(p)
	if err != nil {
		return fmt.Errorf("reading the firmware partition on %s: %w", pod.Disk.Path, err)
	}
	v.Firmware.Version_ = firmwareVersionString(p, d)
	v.Firmware.DirectoryStart = d.Start
	v.Firmware.Version = d.Version
	for _, e := range d.Entries {
		img := imageJSON{
			Type:       e.LogicalImageType(),
			Container:  string(e.ContainerID[:]),
			DevOffset:  e.DevOffset,
			BodyOffset: fwpart.BodyOffset(e),
			Length:     e.Length,
			LoadAddr:   e.LoadAddr,
			Checksum:   e.Checksum,
		}
		if sum, err := fwpart.EntryChecksum(p, e); err == nil {
			img.Recomputed = sum
			img.ChecksumOK = sum == e.Checksum
		}
		v.Firmware.Images = append(v.Firmware.Images, img)
	}
	if idx, osos, ok := d.OSOS(); ok {
		capacity := d.Capacity(idx)
		v.Firmware.OSOS = &ososJSON{
			Index:    idx,
			Length:   osos.Length,
			Capacity: capacity,
			Free:     int64(capacity) - int64(osos.Length),
			Checksum: osos.Checksum,
			OK:       fwpart.VerifyEntry(p, osos) == nil,
		}
	}

	enc := json.NewEncoder(out)
	enc.SetIndent("", "  ")
	return enc.Encode(v)
}
