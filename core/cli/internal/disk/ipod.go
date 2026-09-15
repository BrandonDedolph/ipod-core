package disk

import (
	"errors"
	"fmt"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
)

// The tested-hardware window.
//
// One machine has ever booted this firmware: the 5.5G 80 GB with an
// MK8010GAH. ipodpatcher reports its partition 1 ending at sector
// 39,075,370 of 2048 bytes, so the disk is 80,026,361,856 bytes — the
// "80 GB" on the label, which is 74.53 GiB to the OS. That is the
// number the lower bound is set from: a decimal-80-GB drive is 74.5
// GiB, so a bound written in GiB has to sit below 74.53 or it excludes
// the only device we have.
//
// Hence [74 GiB, 80 GiB]. It admits the 80 GB 5.5G and the handful of
// drives that report a few hundred MB either side of it; it excludes
// the 30 GB (27.9 GiB) and 60 GB (55.9 GiB) 5G/5.5G models, and every
// iFlash/SD rebuild, which are all 5.5G-shaped but have never run this
// code. Tested==false is not "refuse" — it is "refuse to WRITE without
// --untested-hardware" (S7). Reading, inspecting and backing up an
// untested iPod is exactly what someone with a 60 GB should be able to
// do.
const (
	TestedMinBytes int64 = 74 << 30 // 79,456,894,976
	TestedMaxBytes int64 = 80 << 30 // 85,899,345,920
	// TestedModel is the model string for a disk inside the window.
	TestedModel = "iPod Video 5.5G 80 GB"
)

// SectorSizeSource records how an IPod's sector size was settled.
const (
	// SectorSizeFromOS: the size the driver reported put the Apple
	// preamble exactly where partition 0 says it is.
	SectorSizeFromOS = "os"
	// SectorSizeProbed512 / SectorSizeProbed2048: the driver's number
	// did NOT land on the preamble and this one did. That is a fact
	// worth printing, not papering over — it means the OS and the USB
	// bridge disagree about the unit the partition table is in, and
	// every byte offset in the flash path depends on the answer.
	SectorSizeProbed512  = "probe-512"
	SectorSizeProbed2048 = "probe-2048"
)

// IPod is a disk that passed identification.
type IPod struct {
	// Disk is the device as the OS described it.
	Disk Disk
	// Partitions is the whole MBR, all four slots.
	Partitions []Partition
	// SectorSize is the unit the partition table is expressed in, as
	// confirmed by the preamble cross-check — NOT necessarily
	// Disk.SectorSize.
	SectorSize int
	// SectorSizeSource is one of the SectorSize* constants above.
	SectorSizeSource string
	// FWPartStart / FWPartLen bound the firmware partition in bytes.
	FWPartStart, FWPartLen int64
	// DataPartStart is the first byte of the FAT32 music partition.
	DataPartStart int64
	// Model is the human name: TestedModel, or a description of what
	// was actually found.
	Model string
	// Tested reports whether this is the hardware the firmware has
	// booted on. The write path gates on it.
	Tested bool
	// UntestedReason explains a false Tested.
	UntestedReason string
}

// Describe is the line `core info` and the multi-device error print.
func (p IPod) Describe() string {
	s := fmt.Sprintf("%s  %s  %s", p.Disk.Path, p.Model, HumanSize(p.Disk.SizeBytes))
	if !p.Tested {
		s += "  [UNTESTED HARDWARE]"
	}
	return s
}

// Injection points for the tests. FindIPods is the one function in this
// package with real logic in it and no way to reach it without a disk
// attached, so the two calls it makes into the OS are variables. They
// are package-level rather than an interface because the alternative —
// threading a Lister through Open, the CLI and back — buys nothing: one
// function needs faking, in one test file.
var (
	listDisks_ = List
	openDisk_  = Open
)

// FindIPods returns every attached disk that is an iPod.
//
// Three independent checks, all required, in the order that costs the
// least first:
//
//  1. The MBR is the iPod's shape: slot 0 is type 0x00 with a real
//     extent (the firmware partition — see Partition.Used), slot 1 is
//     FAT32 (0x0B or 0x0C).
//  2. The Apple firmware preamble is at partition 0's first sector,
//     with the "]ih[" directory marker at 0x100 — fwpart.CheckPreamble.
//     This is the check that actually protects a stranger's disk: a
//     table can coincide, this cannot.
//  3. The OS's own idea of the device says Apple or iPod.
//
// Anything that fails any of them is silently skipped, including disks
// that cannot be opened at all. The one loud case is "there were disks,
// and every single open was refused" — that is not "no iPod", that is
// "run me as Administrator", and it is returned as ErrRawAccessDenied.
func FindIPods() ([]IPod, error) {
	disks, err := listDisks_()
	if err != nil {
		return nil, err
	}
	var (
		found  []IPod
		denied int
		opened int
	)
	for _, d := range disks {
		h, err := openDisk_(d.Path, false)
		if err != nil {
			if isAccessDenied(err) {
				denied++
			}
			continue
		}
		opened++
		p, ok := identify(d, h)
		h.Close()
		if ok {
			found = append(found, p)
		}
	}
	if len(found) == 0 && opened == 0 && denied > 0 {
		return nil, fmt.Errorf("%w: %d disk(s) present, none could be opened for reading",
			ErrRawAccessDenied, denied)
	}
	return found, nil
}

// SelectIPod resolves a --device value against the attached iPods.
//
// An empty selector requires exactly one; ErrNoDevice and
// ErrMultipleDevices are the two failures, and the multiple case lists
// what it found, because "specify which one" without saying which ones
// exist is the error message this project already had and could not
// act on.
func SelectIPod(selector string) (IPod, error) {
	pods, err := FindIPods()
	if err != nil {
		return IPod{}, err
	}
	if selector != "" {
		for _, p := range pods {
			if matchesSelector(p, selector) {
				return p, nil
			}
		}
		return IPod{}, fmt.Errorf("%w: --device %q matched none of the %d iPod(s) found",
			ErrNoDevice, selector, len(pods))
	}
	switch len(pods) {
	case 0:
		return IPod{}, ErrNoDevice
	case 1:
		return pods[0], nil
	default:
		var b strings.Builder
		b.WriteString(ErrMultipleDevices.Error())
		for _, p := range pods {
			b.WriteString("\n  " + p.Describe())
		}
		return IPod{}, errors.New(b.String())
	}
}

// matchesSelector accepts the device path (exactly, or case-folded on
// Windows where `\\.\physicaldrive1` is the same device) or the serial
// number, which is the only stable name across replugs.
func matchesSelector(p IPod, sel string) bool {
	if p.Disk.Path == sel || strings.EqualFold(p.Disk.Path, sel) {
		return true
	}
	if p.Disk.Serial != "" && strings.EqualFold(strings.TrimSpace(p.Disk.Serial), strings.TrimSpace(sel)) {
		return true
	}
	return false
}

// identify runs the three checks on one open disk.
func identify(d Disk, h Handle) (IPod, bool) {
	mbr := make([]byte, MBRSize)
	if _, err := h.ReadAt(mbr, 0); err != nil {
		return IPod{}, false
	}
	parts, err := ParseMBR(mbr)
	if err != nil {
		return IPod{}, false
	}
	fw, data := parts[0], parts[1]
	if fw.Type != TypeEmpty || !fw.Used() {
		return IPod{}, false
	}
	if data.Type != TypeFAT32CHS && data.Type != TypeFAT32LBA {
		return IPod{}, false
	}
	ss, source, ok := resolveSectorSize(h, fw, d.SectorSize)
	if !ok {
		return IPod{}, false
	}
	if !looksApple(d) {
		return IPod{}, false
	}

	p := IPod{
		Disk:             d,
		Partitions:       parts,
		SectorSize:       ss,
		SectorSizeSource: source,
		FWPartStart:      fw.ByteStart(ss),
		FWPartLen:        fw.ByteLength(ss),
		DataPartStart:    data.ByteStart(ss),
	}
	p.Tested, p.Model, p.UntestedReason = classifyModel(d.SizeBytes)
	return p, true
}

// resolveSectorSize settles the unit the partition table is written in.
//
// The OS's answer is tried first and is almost always right. It is
// still cross-checked, because it is the single number that every byte
// offset in the flash path is multiplied by: if the driver says 512 and
// the bridge means 2048, "partition 0 starts at sector 63" points at
// byte 32,256 instead of 129,024, the preamble check fails, and — had
// we not checked — a write would land 96 KB into Apple's boot block.
//
// The test is the preamble itself: sector 63 × the unit must be the
// "{{~~" banner. 512 and 2048 are then tried in turn. Reporting which
// one matched is part of the answer, not a detail.
func resolveSectorSize(h Handle, fw Partition, osSize int) (int, string, bool) {
	try := func(ss int) bool {
		if ss <= 0 {
			return false
		}
		head := make([]byte, fwpart.PreambleWindow)
		off := fw.ByteStart(ss)
		if off < 0 || off+int64(len(head)) > h.Size() {
			return false
		}
		if _, err := h.ReadAt(head, off); err != nil {
			return false
		}
		return fwpart.CheckPreamble(head) == nil
	}
	if try(osSize) {
		return osSize, SectorSizeFromOS, true
	}
	if osSize != 512 && try(512) {
		return 512, SectorSizeProbed512, true
	}
	if osSize != 2048 && try(2048) {
		return 2048, SectorSizeProbed2048, true
	}
	return 0, "", false
}

// looksApple is check 3: what the OS says the device is.
func looksApple(d Disk) bool {
	v := strings.ToLower(d.Vendor)
	m := strings.ToLower(d.Model)
	return strings.Contains(v, "apple") || strings.Contains(m, "apple") || strings.Contains(m, "ipod")
}

// classifyModel turns a disk size into the Tested gate.
func classifyModel(size int64) (tested bool, model, reason string) {
	if size >= TestedMinBytes && size <= TestedMaxBytes {
		return true, TestedModel, ""
	}
	switch {
	case size < TestedMinBytes:
		reason = fmt.Sprintf("%s is below the %s tested window (%s .. %s)",
			HumanSize(size), TestedModel, HumanSize(TestedMinBytes), HumanSize(TestedMaxBytes))
	default:
		reason = fmt.Sprintf("%s is above the %s tested window (%s .. %s); "+
			"an iFlash/SD rebuild reports a size like this",
			HumanSize(size), TestedModel, HumanSize(TestedMinBytes), HumanSize(TestedMaxBytes))
	}
	return false, fmt.Sprintf("iPod (%s, untested capacity)", HumanSize(size)), reason
}

// UntestedHardwareError is what the write path (S7) returns when Tested
// is false and --untested-hardware was not passed. It is defined here,
// next to the window it refers to, so the two cannot drift.
type UntestedHardwareError struct{ Pod IPod }

func (e *UntestedHardwareError) Error() string {
	return fmt.Sprintf("%s: %s\n"+
		"Only the %s has ever booted this firmware. Pass --untested-hardware to\n"+
		"write to it anyway; reading, inspecting and backing it up need no flag.",
		e.Pod.Disk.Path, e.Pod.UntestedReason, TestedModel)
}
