package cli

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
)

// fakePod builds an in-memory iPod: a 2048-byte-sector device with the
// MBR in sector 0 and the synthetic firmware partition at sector 63,
// which is where the real one is. Returns the pod, an open handle, and
// the exact partition bytes a backup must reproduce.
func fakePod(t *testing.T, image []byte) (disk.IPod, disk.Handle, []byte) {
	t.Helper()
	part := syntheticPartition(t, image)
	pod, h := fakePodFrom(t, part)
	return pod, h, part
}

// --- core backup ------------------------------------------------------

// TestBackupFileName pins the default name. The size is in it because a
// loose backup file has to say which iPod it came from, and the
// timestamp has no colons because a colon is a legal path character on
// exactly one of the three operating systems this runs on.
func TestBackupFileName(t *testing.T) {
	when := time.Date(2026, 9, 14, 22, 5, 11, 0, time.FixedZone("PDT", -7*3600))
	got := backupFileName(131475456, when)
	want := "fwpart-131475456-2026-09-14T22-05-11-07-00.bin"
	if got != want {
		t.Errorf("backupFileName = %q, want %q", got, want)
	}
	if strings.Contains(got, ":") {
		t.Error("the backup name contains a colon, which Windows will not accept in a path")
	}
	if !strings.Contains(got, "131475456") {
		t.Error("the backup name does not carry the partition size")
	}
	// UTC must not collide with local time for the same instant.
	utc := backupFileName(131475456, when.UTC())
	if utc == got {
		t.Error("two different zone renderings of the same instant produced the same name")
	}
}

func TestDefaultBackupPath(t *testing.T) {
	home := t.TempDir()
	t.Setenv("XDG_CONFIG_HOME", home)
	t.Setenv("HOME", home)
	t.Setenv("AppData", home)

	got, err := defaultBackupPath(131475456, time.Now())
	if err != nil {
		t.Fatalf("defaultBackupPath: %v", err)
	}
	wantDir := filepath.Join(home, "core", "backups")
	if filepath.Dir(got) != wantDir {
		t.Errorf("default backup directory = %q, want %q", filepath.Dir(got), wantDir)
	}
	if !strings.HasSuffix(got, ".bin") {
		t.Errorf("default backup path %q does not end in .bin", got)
	}
}

// TestBackupDumpsTheWholePartition: byte for byte, preamble included.
// This is the assertion that `core backup` and ipodpatcher -r produce
// the same file; the device check compares them for real.
func TestBackupDumpsTheWholePartition(t *testing.T) {
	image := plausibleImage(8192)
	pod, h, part := fakePod(t, image)
	out := filepath.Join(t.TempDir(), "fwpart.bin")

	var buf bytes.Buffer
	if err := runBackup(&buf, pod, h, out); err != nil {
		t.Fatalf("runBackup: %v", err)
	}
	got, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read the backup: %v", err)
	}
	if !bytes.Equal(got, part) {
		t.Fatalf("the backup is %d bytes and the partition is %d; they differ",
			len(got), len(part))
	}
	text := buf.String()
	for _, want := range []string{pod.Disk.Path, "preamble OK", "OSOS", "checksum", "OK"} {
		if !strings.Contains(text, want) {
			t.Errorf("the backup report does not mention %q:\n%s", want, text)
		}
	}
}

// TestBackupRefusesToOverwrite: a backup you can clobber is not a
// backup, and the second run is exactly when you have just broken the
// device the first one came from.
func TestBackupRefusesToOverwrite(t *testing.T) {
	image := plausibleImage(8192)
	pod, h, _ := fakePod(t, image)
	out := filepath.Join(t.TempDir(), "fwpart.bin")

	if err := runBackup(&bytes.Buffer{}, pod, h, out); err != nil {
		t.Fatalf("first backup: %v", err)
	}
	before, _ := os.ReadFile(out)
	err := runBackup(&bytes.Buffer{}, pod, h, out)
	if err == nil {
		t.Fatal("a second backup overwrote the first")
	}
	if !strings.Contains(err.Error(), "already exists") {
		t.Errorf("the refusal does not say the file exists: %v", err)
	}
	after, _ := os.ReadFile(out)
	if !bytes.Equal(before, after) {
		t.Error("the refused backup still changed the file")
	}
}

// TestBackupReportsABadChecksum: a device whose OSOS does not verify is
// exactly the device someone is backing up before trying to fix it, so
// the backup must still be written — with the mismatch reported.
func TestBackupReportsABadChecksum(t *testing.T) {
	image := plausibleImage(8192)
	// Build the partition from the good image (so the entry carries the
	// good checksum), then corrupt one body byte underneath it.
	part := syntheticPartition(t, image)
	part[0x4800+0x800+10] ^= 0xFF
	pod, h := fakePodFrom(t, part)

	out := filepath.Join(t.TempDir(), "fwpart.bin")
	var buf bytes.Buffer
	if err := runBackup(&buf, pod, h, out); err != nil {
		t.Fatalf("runBackup on a device with a bad checksum: %v", err)
	}
	got, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("the backup was not written: %v", err)
	}
	if !bytes.Equal(got, part) {
		t.Error("the backup of a corrupt partition is not byte-identical to it")
	}
	if !strings.Contains(buf.String(), "does NOT verify") {
		t.Errorf("the bad checksum was not reported:\n%s", buf.String())
	}
}

// fakePodFrom builds a device around an arbitrary partition image,
// keeping the real geometry (2048-byte sectors, partition 0 at LBA 63).
func fakePodFrom(t *testing.T, part []byte) (disk.IPod, disk.Handle) {
	t.Helper()
	const (
		ss       = 2048
		startLBA = 63
	)
	start := int64(startLBA) * ss
	dev := make([]byte, start+int64(len(part)))
	parts := []disk.Partition{
		{Index: 0, Type: disk.TypeEmpty, StartLBA: startLBA, NumSectors: uint32(len(part) / ss)},
		{Index: 1, Type: disk.TypeFAT32CHS, StartLBA: startLBA + uint32(len(part)/ss), NumSectors: 1000},
		{Index: 2}, {Index: 3},
	}
	copy(dev, disk.BuildMBR(parts))
	copy(dev[start:], part)
	h, _, err := disk.NewMemDevice(dev, ss, false)
	if err != nil {
		t.Fatalf("NewMemDevice: %v", err)
	}
	t.Cleanup(func() { h.Close() })
	return disk.IPod{
		Disk: disk.Disk{
			Path: `\\.\PhysicalDrive9`, SizeBytes: 80026361856, SectorSize: ss,
			Vendor: "Apple", Model: "iPod", Serial: "TESTSERIAL", USB: true,
			Volumes: []string{"D:"}, MountPoints: []string{`D:\`},
		},
		Partitions:       parts,
		SectorSize:       ss,
		SectorSizeSource: disk.SectorSizeFromOS,
		FWPartStart:      start,
		FWPartLen:        int64(len(part)),
		DataPartStart:    start + int64(len(part)),
		Model:            disk.TestedModel,
		Tested:           true,
	}, h
}

// --- core firmware read -----------------------------------------------

// TestFirmwareReadWritesExactlyTheBody is the contract with
// ipodpatcher -rfb: exactly the entry's Length bytes, no zero padding
// up to the 0x800 boundary. ipodpatcher's own log says it pads the READ
// from 0x59bf8 to 0x5a000 and then writes a 367,608-byte file; if this
// wrote the padded length the two would never cmp equal.
func TestFirmwareReadWritesExactlyTheBody(t *testing.T) {
	// A length that is deliberately NOT a multiple of 0x800, like every
	// real core.bin.
	image := plausibleImage(8192 + 123)
	part := syntheticPartition(t, image)
	p := fwpart.Partition{R: bytes.NewReader(part), Size: int64(len(part))}
	out := filepath.Join(t.TempDir(), "core_read.bin")

	var buf bytes.Buffer
	if err := writeOSOSBody(&buf, p, out, false); err != nil {
		t.Fatalf("writeOSOSBody: %v", err)
	}
	got, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read the output: %v", err)
	}
	if len(got) != len(image) {
		t.Fatalf("wrote %d bytes, want exactly the entry Length of %d", len(got), len(image))
	}
	if !bytes.Equal(got, image) {
		t.Fatal("the bytes written differ from the image in the partition")
	}
	if !strings.Contains(buf.String(), "checksum") || !strings.Contains(buf.String(), "OK") {
		t.Errorf("the report does not confirm the checksum:\n%s", buf.String())
	}
}

func TestFirmwareReadOverwriteGuard(t *testing.T) {
	image := plausibleImage(4096)
	part := syntheticPartition(t, image)
	newPart := func() fwpart.Partition {
		return fwpart.Partition{R: bytes.NewReader(part), Size: int64(len(part))}
	}
	out := filepath.Join(t.TempDir(), "core_read.bin")

	if err := writeOSOSBody(&bytes.Buffer{}, newPart(), out, false); err != nil {
		t.Fatalf("first read: %v", err)
	}
	if err := writeOSOSBody(&bytes.Buffer{}, newPart(), out, false); err == nil {
		t.Error("a second read overwrote the output without --force")
	}
	if err := writeOSOSBody(&bytes.Buffer{}, newPart(), out, true); err != nil {
		t.Errorf("--force did not allow the overwrite: %v", err)
	}
}

func TestFirmwareReadReportsABadChecksum(t *testing.T) {
	image := plausibleImage(4096)
	part := syntheticPartition(t, image)
	part[0x4800+0x800+7] ^= 0xFF // corrupt the body, leave the entry alone
	p := fwpart.Partition{R: bytes.NewReader(part), Size: int64(len(part))}

	var buf bytes.Buffer
	out := filepath.Join(t.TempDir(), "core_read.bin")
	if err := writeOSOSBody(&buf, p, out, false); err != nil {
		t.Fatalf("writeOSOSBody: %v", err)
	}
	if !strings.Contains(buf.String(), "MISMATCH") {
		t.Errorf("a corrupt body was not reported:\n%s", buf.String())
	}
	if _, err := os.Stat(out); err != nil {
		t.Error("the image was not written; getting a bad image off the device is the point")
	}
}

func TestFirmwareReadRequiresOut(t *testing.T) {
	_, _, err := runCore(t, "firmware", "read")
	if err == nil || !strings.Contains(err.Error(), "--out is required") {
		t.Errorf("firmware read without --out = %v, want the --out requirement", err)
	}
}

// --- info --all-disks -------------------------------------------------

// TestInfoAllDisksDegradesGracefully: enumeration needs no privilege on
// any of the three platforms, identification does. Run as an ordinary
// user with no iPod attached, `info --all-disks` must still succeed and
// print the list — that is the flag people reach for when `core info`
// says no iPod and the iPod is right there.
func TestInfoAllDisksDegradesGracefully(t *testing.T) {
	stdout, _, err := runCore(t, "info", "--all-disks")
	if err != nil {
		t.Fatalf("info --all-disks failed on a machine with ordinary disks: %v", err)
	}
	if !strings.Contains(stdout, "path") && !strings.Contains(stdout, "no disks reported") {
		t.Errorf("info --all-disks printed neither a table nor an explanation:\n%s", stdout)
	}
}

func TestSectorSourceText(t *testing.T) {
	base := disk.IPod{
		Disk:       disk.Disk{SectorSize: 512},
		Partitions: []disk.Partition{{Index: 0, StartLBA: 63}},
		SectorSize: 2048,
	}
	base.SectorSizeSource = disk.SectorSizeFromOS
	if got := sectorSourceText(base); !strings.Contains(got, "confirmed") {
		t.Errorf("the OS-reported case reads %q", got)
	}
	base.SectorSizeSource = disk.SectorSizeProbed2048
	got := sectorSourceText(base)
	if !strings.Contains(got, "PROBED") || !strings.Contains(got, "512") {
		t.Errorf("the probed case does not say the OS was wrong and what it said: %q", got)
	}
}

// TestInfoJSONShape: --json is an interface other programs read, so the
// keys it promises have to be there and the sector size it reports must
// be the CONFIRMED one, not the OS's guess.
func TestInfoJSONShape(t *testing.T) {
	image := plausibleImage(8192)
	pod, h, _ := fakePod(t, image)
	pod.Disk.SectorSize = 512 // the OS was wrong
	pod.SectorSize = 2048     // the preamble said otherwise
	pod.SectorSizeSource = disk.SectorSizeProbed2048

	var buf bytes.Buffer
	if err := writeInfoJSON(&buf, pod, firmwarePartition(pod, h)); err != nil {
		t.Fatalf("writeInfoJSON: %v", err)
	}
	var got map[string]any
	if err := json.Unmarshal(buf.Bytes(), &got); err != nil {
		t.Fatalf("the JSON does not parse: %v\n%s", err, buf.String())
	}
	d, _ := got["disk"].(map[string]any)
	if d == nil {
		t.Fatal(`no "disk" object`)
	}
	if d["sector_size"] != float64(2048) {
		t.Errorf("sector_size = %v, want the confirmed 2048", d["sector_size"])
	}
	if d["os_sector_size"] != float64(512) {
		t.Errorf("os_sector_size = %v, want the OS's 512 kept alongside it", d["os_sector_size"])
	}
	if d["sector_size_source"] != disk.SectorSizeProbed2048 {
		t.Errorf("sector_size_source = %v", d["sector_size_source"])
	}
	fw, _ := got["firmware"].(map[string]any)
	if fw == nil {
		t.Fatal(`no "firmware" object`)
	}
	if fw["version"] != "unknown" {
		t.Errorf(`firmware.version = %v, want "unknown" until the S8 marker lands`, fw["version"])
	}
	osos, _ := fw["osos"].(map[string]any)
	if osos == nil {
		t.Fatal(`no "firmware.osos" object`)
	}
	if osos["length"] != float64(len(image)) {
		t.Errorf("osos.length = %v, want %d", osos["length"], len(image))
	}
	if osos["checksum_ok"] != true {
		t.Error("osos.checksum_ok is false for a partition built with the right sum")
	}
	if len(got["partitions"].([]any)) != 2 {
		t.Errorf("partitions = %v, want the two used slots", got["partitions"])
	}
}
