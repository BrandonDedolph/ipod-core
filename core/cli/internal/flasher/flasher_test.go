package flasher

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/fwpart"
)

// --- the synthetic device --------------------------------------------
//
// The same shape as the real 5.5G, shrunk so it fits in a test: 2048-byte
// sectors, partition 0 at LBA 63, the Apple preamble, a v3 directory at
// 0x4200 and TWO images. Two, not one, because the assertion that
// matters most in this package is that everything except the OSOS body
// and its own directory row comes back byte-identical — and a directory
// with one row cannot show that the second row was left alone.

const (
	testSectorSize = 2048
	testStartLBA   = 63
	testPartSize   = 1 << 20
	testDirStart   = 0x4200
	ososDevOffset  = 0x4800 // body at 0x5000
	rsrcDevOffset  = 0x80000
	// Capacity of the OSOS entry: RSRC's devOffset minus the OSOS body.
	testCapacity = rsrcDevOffset - (ososDevOffset + fwpart.BodyBias)
)

// plausibleImage returns deterministic bytes that pass the plausibility
// rules: large enough, not uniform, and starting with an ARM branch.
func plausibleImage(n int, seed int64) []byte {
	b := make([]byte, n)
	rand.New(rand.NewSource(seed)).Read(b)
	b[0], b[1], b[2], b[3] = 0x0E, 0x00, 0x00, 0xEA // 0xEA00000E: b <somewhere>
	return b
}

func sumBytes(b []byte) uint32 {
	var s uint32
	for _, x := range b {
		s += uint32(x)
	}
	return s
}

func entryBytes(t *testing.T, e firmware.DirectoryEntry) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := firmware.WriteDirectoryEntry(&buf, e); err != nil {
		t.Fatalf("encode entry: %v", err)
	}
	return buf.Bytes()
}

// syntheticPartition builds the partition image: preamble, directory,
// an OSOS body and an RSRC body.
func syntheticPartition(t *testing.T, osos, rsrc []byte) []byte {
	t.Helper()
	part := make([]byte, testPartSize)
	off := 0
	for _, line := range []string{
		"{{~~  /-----\\   ", "{{~~ /       \\  ", "{{~~|         | ",
		"{{~~| S T O P | ", "{{~~|         | ", "{{~~ \\       /  ",
		"{{~~  \\-----/   ",
	} {
		off += copy(part[off:], line)
	}
	off += copy(part[off:], "Copyright(C) 2001 Apple Computer, Inc.")
	for i := off; i < 0xFF; i++ {
		part[i] = '-'
	}
	copy(part[0x100:], firmware.DirectoryMarker[:])
	// LE32 0x4000 at 0x104 (+0x200 = 0x4200), LE16 version 3 at 0x10A.
	part[0x104], part[0x105], part[0x106] = 0x00, 0x40, 0x00
	part[0x10A] = 3

	copy(part[testDirStart:], entryBytes(t, firmware.DirectoryEntry{
		ContainerID: [4]byte{'!', 'A', 'T', 'A'},
		ImageType:   [4]byte{'s', 'o', 's', 'o'},
		DevOffset:   ososDevOffset,
		Length:      uint32(len(osos)),
		LoadAddr:    0x10000000,
		Checksum:    sumBytes(osos),
		Version:     0xB012,
		LoadAddr2:   0xFFFFFFFF,
	}))
	copy(part[testDirStart+fwpart.EntrySize:], entryBytes(t, firmware.DirectoryEntry{
		ContainerID: [4]byte{'!', 'A', 'T', 'A'},
		ImageType:   [4]byte{'c', 'r', 's', 'r'},
		DevOffset:   rsrcDevOffset,
		Length:      uint32(len(rsrc)),
		LoadAddr:    0x10000000,
		EntryOffset: 0x1234,
		Checksum:    sumBytes(rsrc),
		Version:     0xB012,
		LoadAddr2:   0xFFFFFFFF,
	}))
	// The terminator row as the device writes it: zero except LoadAddr2.
	copy(part[testDirStart+2*fwpart.EntrySize+36:], []byte{0xFF, 0xFF, 0xFF, 0xFF})

	copy(part[ososDevOffset+fwpart.BodyBias:], osos)
	copy(part[rsrcDevOffset+fwpart.BodyBias:], rsrc)
	return part
}

// --- the fake device and the injection seams --------------------------

type env struct {
	t   *testing.T
	dev []byte // the whole disk, shared by every handle
	pod disk.IPod
	out bytes.Buffer

	// what happened
	opens      []bool // the write flag of each Open, in order
	writeCalls int
	locks      int
	unlocks    int
	flushes    int

	// what to do
	failWriteN    int // fail the Nth WriteAt (1-based); 0 = never
	openErr       error
	selectErr     error
	elevated      bool
	goos          string
	relaunchArgs  []string
	relaunchCalls int
	relaunchCode  int
	relaunchLog   string
	confirmWith   string
	confirms      int

	backupDir string
}

func newEnv(t *testing.T, part []byte) *env {
	t.Helper()
	start := int64(testStartLBA) * testSectorSize
	dev := make([]byte, start+int64(len(part)))
	parts := []disk.Partition{
		{Index: 0, Type: disk.TypeEmpty, StartLBA: testStartLBA,
			NumSectors: uint32(len(part) / testSectorSize)},
		{Index: 1, Type: disk.TypeFAT32CHS,
			StartLBA: testStartLBA + uint32(len(part)/testSectorSize), NumSectors: 1000},
		{Index: 2}, {Index: 3},
	}
	copy(dev, disk.BuildMBR(parts))
	copy(dev[start:], part)

	e := &env{
		t: t, dev: dev, goos: "linux", elevated: true,
		backupDir: filepath.Join(t.TempDir(), "backups"),
	}
	e.pod = disk.IPod{
		Disk: disk.Disk{
			Path: `\\.\PhysicalDrive9`, SizeBytes: 80026361856, SectorSize: testSectorSize,
			Vendor: "Apple", Model: "iPod", Serial: "TESTSERIAL", USB: true,
			Volumes: []string{"D:"}, MountPoints: []string{`D:\`},
		},
		Partitions:       parts,
		SectorSize:       testSectorSize,
		SectorSizeSource: disk.SectorSizeFromOS,
		FWPartStart:      start,
		FWPartLen:        int64(len(part)),
		DataPartStart:    start + int64(len(part)),
		Model:            disk.TestedModel,
		Tested:           true,
	}
	return e
}

// partition returns the current bytes of partition 0 on the fake device.
func (e *env) partition() []byte {
	return e.dev[e.pod.FWPartStart : e.pod.FWPartStart+e.pod.FWPartLen]
}

func (e *env) deps() Deps {
	return Deps{
		SelectDevice: func(string) (disk.IPod, error) {
			if e.selectErr != nil {
				return disk.IPod{}, e.selectErr
			}
			return e.pod, nil
		},
		Open: func(path string, write bool) (disk.Handle, error) {
			e.opens = append(e.opens, write)
			if e.openErr != nil {
				return nil, e.openErr
			}
			h, _, err := disk.NewMemDevice(e.dev, testSectorSize, write)
			if err != nil {
				return nil, err
			}
			return &recHandle{Handle: h, e: e, write: write}, nil
		},
		IsElevated: func() bool { return e.elevated },
		Relaunch: func(args []string) (int, string, error) {
			e.relaunchCalls++
			e.relaunchArgs = args
			return e.relaunchCode, e.relaunchLog, nil
		},
		Executable: func() (string, error) { return `C:\tools\core.exe`, nil },
		GOOS:       e.goos,
		ChildArgs:  []string{"flash", "core.ipod"},
		Confirm: func(string) (string, error) {
			e.confirms++
			return e.confirmWith, nil
		},
		Out:       &e.out,
		Now:       func() time.Time { return time.Date(2026, 9, 15, 1, 2, 3, 0, time.UTC) },
		ConfigDir: func() (string, error) { return "", errors.New("no config dir in tests") },
	}
}

// recHandle counts what the sequence did to the device and can fail a
// chosen WriteAt — which is how the "the body landed and the directory
// did not" case is reached without an iPod.
type recHandle struct {
	disk.Handle
	e     *env
	write bool
}

func (h *recHandle) WriteAt(p []byte, off int64) (int, error) {
	h.e.writeCalls++
	if h.e.failWriteN != 0 && h.e.writeCalls == h.e.failWriteN {
		return 0, fmt.Errorf("injected fault on WriteAt #%d", h.e.writeCalls)
	}
	return h.Handle.WriteAt(p, off)
}
func (h *recHandle) Lock() error   { h.e.locks++; return h.Handle.Lock() }
func (h *recHandle) Unlock() error { h.e.unlocks++; return h.Handle.Unlock() }
func (h *recHandle) Flush() error  { h.e.flushes++; return h.Handle.Flush() }

// writeIPod packs an image into a .ipod file and returns its path.
func writeIPod(t *testing.T, dir, name string, image []byte) string {
	t.Helper()
	var buf bytes.Buffer
	if err := firmware.WriteIPodFile(&buf,
		firmware.ModelIPodVideo, firmware.ModelNameIPodVideo, image); err != nil {
		t.Fatalf("pack: %v", err)
	}
	return writeTemp(t, dir, name, buf.Bytes())
}

func writeTemp(t *testing.T, dir, name string, data []byte) string {
	t.Helper()
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, data, 0o644); err != nil {
		t.Fatalf("write %s: %v", p, err)
	}
	return p
}

// readEntry decodes directory row idx out of a partition image.
func readEntry(t *testing.T, part []byte, idx int) firmware.DirectoryEntry {
	t.Helper()
	off := testDirStart + idx*fwpart.EntrySize
	e, err := firmware.ReadDirectoryEntry(bytes.NewReader(part[off : off+fwpart.EntrySize]))
	if err != nil {
		t.Fatalf("decode entry %d: %v", idx, err)
	}
	return e
}

// onlyBackup returns the single file in the backup directory.
func onlyBackup(t *testing.T, dir string) string {
	t.Helper()
	ents, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("read the backup directory: %v", err)
	}
	if len(ents) != 1 {
		t.Fatalf("the backup directory holds %d entries, want exactly 1", len(ents))
	}
	return filepath.Join(dir, ents[0].Name())
}

func backupCount(t *testing.T, dir string) int {
	t.Helper()
	ents, err := os.ReadDir(dir)
	if os.IsNotExist(err) {
		return 0
	}
	if err != nil {
		t.Fatalf("read the backup directory: %v", err)
	}
	return len(ents)
}

// --- the full sequence ------------------------------------------------

// TestFlashFullSequence is the whole of S7 in one assertion set: the
// body and the entry become the new image, everything else on the
// partition is byte-identical, and the backup on disk is the partition
// as it was.
func TestFlashFullSequence(t *testing.T) {
	old := plausibleImage(8192, 1)
	rsrc := plausibleImage(4096, 2)
	before := syntheticPartition(t, old, rsrc)
	e := newEnv(t, append([]byte(nil), before...))
	image := plausibleImage(12345, 3) // not a multiple of 0x800, like every real core.bin
	path := writeIPod(t, t.TempDir(), "core.ipod", image)

	res, err := Flash(context.Background(),
		Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if err != nil {
		t.Fatalf("Flash: %v\n%s", err, e.out.String())
	}
	if !res.Verified {
		t.Error("Flash returned without Verified")
	}
	if res.Mode != ModeOSOS {
		t.Errorf("Mode = %q", res.Mode)
	}

	after := e.partition()

	// 1. the body is the image, zero-padded to the 0x800 boundary.
	bodyOff := ososDevOffset + fwpart.BodyBias
	if got := after[bodyOff : bodyOff+len(image)]; !bytes.Equal(got, image) {
		t.Error("the body on the device is not the image")
	}
	padded := (len(image) + fwpart.BodyAlign - 1) &^ (fwpart.BodyAlign - 1)
	pad := after[bodyOff+len(image) : bodyOff+padded]
	if !bytes.Equal(pad, make([]byte, len(pad))) {
		t.Errorf("the %d padding bytes past the image are not zero", len(pad))
	}

	// 2. the OSOS row carries the new length and sum and nothing else
	//    changed.
	wantEntry := readEntry(t, before, 0)
	wantEntry.Length = uint32(len(image))
	wantEntry.Checksum = fwpart.ImageChecksum(image)
	if got := readEntry(t, after, 0); got != wantEntry {
		t.Errorf("OSOS row =\n  %+v\nwant\n  %+v", got, wantEntry)
	}
	if res.OldLength != uint32(len(old)) || res.NewLength != uint32(len(image)) {
		t.Errorf("Result lengths %d → %d, want %d → %d",
			res.OldLength, res.NewLength, len(old), len(image))
	}
	if !res.OldChecksumOK {
		t.Error("the image already on the device should have verified")
	}

	// 3. everything else is byte-identical: the preamble, the directory
	//    header, the RSRC row, the terminator row, and Apple's image.
	for _, span := range []struct {
		name   string
		lo, hi int
	}{
		{"preamble and directory header", 0, testDirStart},
		{"the RSRC row and the terminator", testDirStart + fwpart.EntrySize, ososDevOffset},
		{"the gap before the OSOS body", ososDevOffset, ososDevOffset + fwpart.BodyBias},
		{"the RSRC body", rsrcDevOffset, testPartSize},
	} {
		if !bytes.Equal(before[span.lo:span.hi], after[span.lo:span.hi]) {
			t.Errorf("%s changed (%#x..%#x)", span.name, span.lo, span.hi)
		}
	}

	// 4. the backup is the partition as it was.
	backup := onlyBackup(t, e.backupDir)
	if backup != res.BackupPath {
		t.Errorf("Result.BackupPath = %q, the file on disk is %q", res.BackupPath, backup)
	}
	got, err := os.ReadFile(backup)
	if err != nil {
		t.Fatalf("read the backup: %v", err)
	}
	if !bytes.Equal(got, before) {
		t.Error("the backup is not the partition as it was before the write")
	}

	// 5. the order of operations: three read-only opens (plan, backup,
	//    verify) around exactly one write open, and the device was
	//    locked, flushed and unlocked.
	if want := []bool{false, false, true, false}; !equalBools(e.opens, want) {
		t.Errorf("opens (write flag, in order) = %v, want %v", e.opens, want)
	}
	if e.locks != 1 || e.unlocks != 1 || e.flushes != 1 {
		t.Errorf("locks=%d unlocks=%d flushes=%d, want 1/1/1", e.locks, e.unlocks, e.flushes)
	}
	if e.writeCalls != 2 {
		t.Errorf("WriteAt was called %d times, want 2 (body, then the directory sector)", e.writeCalls)
	}

	text := e.out.String()
	for _, want := range []string{"core flash — plan", "capacity", "VERIFIED", backup} {
		if !strings.Contains(text, want) {
			t.Errorf("the output does not mention %q:\n%s", want, text)
		}
	}
}

func equalBools(a, b []bool) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// TestFlashAcceptsARawBin: a .bin goes through the same plausibility
// rules `firmware pack` applies, and lands the same way.
func TestFlashAcceptsARawBin(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	image := plausibleImage(9000, 4)
	path := writeTemp(t, t.TempDir(), "core.bin", image)

	res, err := Flash(context.Background(),
		Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if err != nil {
		t.Fatalf("Flash: %v\n%s", err, e.out.String())
	}
	if !res.Verified {
		t.Fatal("a raw .bin did not verify")
	}
	bodyOff := ososDevOffset + fwpart.BodyBias
	if !bytes.Equal(e.partition()[bodyOff:bodyOff+len(image)], image) {
		t.Error("the raw image is not on the device")
	}
}

// --- the fault after the body write -----------------------------------

// TestFlashFaultAfterTheBodyWrite is the failure the backup exists for:
// the body lands, the directory row does not, and the device is left
// with an entry whose checksum no longer describes the body it points
// at. The message has to name the backup and the way back.
func TestFlashFaultAfterTheBodyWrite(t *testing.T) {
	old := plausibleImage(8192, 1)
	before := syntheticPartition(t, old, plausibleImage(4096, 2))
	e := newEnv(t, append([]byte(nil), before...))
	e.failWriteN = 2 // the directory-sector write
	image := plausibleImage(12345, 3)
	path := writeIPod(t, t.TempDir(), "core.ipod", image)

	res, err := Flash(context.Background(),
		Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if err == nil {
		t.Fatalf("a failed directory write reported success\n%s", e.out.String())
	}
	if res.Verified {
		t.Error("Verified is set after a failed write")
	}
	msg := err.Error()
	backup := onlyBackup(t, e.backupDir)
	for _, want := range []string{backup, "core flash --from-backup", "ipodpatcher", "disk mode"} {
		if !strings.Contains(msg, want) {
			t.Errorf("the failure does not mention %q:\n%s", want, msg)
		}
	}
	if !strings.Contains(msg, "did NOT verify") {
		t.Errorf("the failure does not say the read-back failed:\n%s", msg)
	}

	// The device really is in the half-written state, and the backup
	// really is the way out of it.
	after := e.partition()
	p := fwpart.Partition{R: bytes.NewReader(after), Size: int64(len(after))}
	dir, perr := fwpart.Parse(p)
	if perr != nil {
		t.Fatalf("the partition no longer parses: %v", perr)
	}
	_, osos, _ := dir.OSOS()
	if err := fwpart.VerifyEntry(p, osos); err == nil {
		t.Error("the OSOS entry still verifies; the fault was not injected where it was meant to be")
	}
	if got, _ := os.ReadFile(backup); !bytes.Equal(got, before) {
		t.Error("the backup is not the partition as it was")
	}
}

// --- --dry-run --------------------------------------------------------

// TestFlashDryRunTouchesNothing: no write handle, no backup, no changed
// byte — and the plan still printed.
func TestFlashDryRunTouchesNothing(t *testing.T) {
	before := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	e := newEnv(t, append([]byte(nil), before...))
	path := writeIPod(t, t.TempDir(), "core.ipod", plausibleImage(12345, 3))

	res, err := Flash(context.Background(),
		Options{Image: path, DryRun: true, BackupDir: e.backupDir}, e.deps())
	if err != nil {
		t.Fatalf("Flash --dry-run: %v", err)
	}
	if res.Verified || res.BackupPath != "" {
		t.Errorf("--dry-run reported Verified=%v BackupPath=%q", res.Verified, res.BackupPath)
	}
	for i, write := range e.opens {
		if write {
			t.Errorf("--dry-run opened the device for writing (open %d)", i)
		}
	}
	if e.writeCalls != 0 || e.locks != 0 {
		t.Errorf("--dry-run wrote %d times and locked %d times", e.writeCalls, e.locks)
	}
	if n := backupCount(t, e.backupDir); n != 0 {
		t.Errorf("--dry-run left %d files in the backup directory", n)
	}
	if e.confirms != 0 {
		t.Error("--dry-run asked for a confirmation")
	}
	if !bytes.Equal(e.partition(), before) {
		t.Error("--dry-run changed the device")
	}
	if !strings.Contains(e.out.String(), "nothing was opened for writing") {
		t.Errorf("--dry-run does not say what it did not do:\n%s", e.out.String())
	}
}

// --- refusals before the backup ---------------------------------------

// TestFlashRefusesAnOversizeImageBeforeTheBackup: the capacity check is
// fwpart's, and it has to happen while the only handle open is a
// read-only one.
func TestFlashRefusesAnOversizeImageBeforeTheBackup(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	image := plausibleImage(testCapacity+1, 5)
	path := writeTemp(t, t.TempDir(), "core.bin", image)

	res, err := Flash(context.Background(),
		Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if !errors.Is(err, fwpart.ErrImageTooLarge) {
		t.Fatalf("Flash with an oversize image = %v, want ErrImageTooLarge", err)
	}
	if res.BackupPath != "" {
		t.Error("a refused flash still took a backup")
	}
	if n := backupCount(t, e.backupDir); n != 0 {
		t.Errorf("the backup directory holds %d files after a refusal", n)
	}
	for _, write := range e.opens {
		if write {
			t.Error("a refused flash opened the device for writing")
		}
	}
}

// TestFlashTestedGate: the write path refuses hardware the firmware has
// never booted on, and says what the flag is.
func TestFlashTestedGate(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	e.pod.Tested = false
	e.pod.Model = "iPod (27.9 GiB, untested capacity)"
	e.pod.UntestedReason = "27.9 GiB is below the tested window"
	path := writeIPod(t, t.TempDir(), "core.ipod", plausibleImage(12345, 3))

	_, err := Flash(context.Background(),
		Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	var gate *disk.UntestedHardwareError
	if !errors.As(err, &gate) {
		t.Fatalf("Flash onto untested hardware = %v, want UntestedHardwareError", err)
	}
	if !strings.Contains(err.Error(), "--untested-hardware") {
		t.Errorf("the refusal does not name the flag:\n%v", err)
	}
	if !strings.Contains(err.Error(), disk.TestedModel) {
		t.Errorf("the refusal does not say which model has booted this firmware:\n%v", err)
	}
	if n := backupCount(t, e.backupDir); n != 0 {
		t.Error("the tested gate ran after the backup")
	}

	// And the flag lets it through.
	e2 := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	e2.pod.Tested = false
	e2.pod.UntestedReason = "27.9 GiB is below the tested window"
	res, err := Flash(context.Background(),
		Options{Image: path, Yes: true, Untested: true, BackupDir: e2.backupDir}, e2.deps())
	if err != nil {
		t.Fatalf("--untested-hardware did not allow the write: %v", err)
	}
	if !res.Verified {
		t.Error("--untested-hardware wrote but did not verify")
	}
	if !strings.Contains(e2.out.String(), "UNTESTED") {
		t.Error("the plan does not say the hardware is untested")
	}
}

// TestFlashRefusesABadIPodChecksum. `firmware unpack --ignore-checksum`
// exists for looking at a damaged image; nothing may flash one.
func TestFlashRefusesABadIPodChecksum(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	dir := t.TempDir()
	path := writeIPod(t, dir, "core.ipod", plausibleImage(12345, 3))
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	data[100] ^= 0xFF // one image byte; the stored header checksum now disagrees
	writeTemp(t, dir, "core.ipod", data)

	_, err = Flash(context.Background(),
		Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if !errors.Is(err, ErrBadInput) {
		t.Fatalf("Flash with a corrupt .ipod = %v, want ErrBadInput", err)
	}
	if !strings.Contains(err.Error(), "checksum") {
		t.Errorf("the refusal does not mention the checksum:\n%v", err)
	}
	if len(e.opens) != 0 {
		t.Error("a corrupt .ipod still reached the device")
	}
}

// TestFlashRefusesAPartitionDumpAsAnImage: the two files sit in the same
// folder and the wrong one would flash Apple's preamble as if it were
// code.
func TestFlashRefusesAPartitionDumpAsAnImage(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	part := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	path := writeTemp(t, t.TempDir(), "fwpart.bin", part)

	_, err := Flash(context.Background(),
		Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if !errors.Is(err, ErrBadInput) {
		t.Fatalf("Flash with a partition dump = %v, want ErrBadInput", err)
	}
	if !strings.Contains(err.Error(), "--from-backup") {
		t.Errorf("the refusal does not point at --from-backup:\n%v", err)
	}
}

// --- the typed confirmation -------------------------------------------

func TestFlashConfirmation(t *testing.T) {
	before := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	path := writeIPod(t, t.TempDir(), "core.ipod", plausibleImage(12345, 3))

	t.Run("wrong text aborts", func(t *testing.T) {
		e := newEnv(t, append([]byte(nil), before...))
		e.confirmWith = "y\n"
		res, err := Flash(context.Background(),
			Options{Image: path, BackupDir: e.backupDir}, e.deps())
		if !errors.Is(err, ErrAborted) {
			t.Fatalf("a wrong confirmation = %v, want ErrAborted", err)
		}
		if !res.Aborted {
			t.Error("Result.Aborted is not set")
		}
		if n := backupCount(t, e.backupDir); n != 0 || e.writeCalls != 0 {
			t.Error("an aborted flash still backed up or wrote")
		}
		if !bytes.Equal(e.partition(), before) {
			t.Error("an aborted flash changed the device")
		}
	})

	t.Run("the device path proceeds", func(t *testing.T) {
		e := newEnv(t, append([]byte(nil), before...))
		e.confirmWith = e.pod.Disk.Path + "\n"
		res, err := Flash(context.Background(),
			Options{Image: path, BackupDir: e.backupDir}, e.deps())
		if err != nil {
			t.Fatalf("Flash: %v", err)
		}
		if !res.Verified {
			t.Error("the confirmed flash did not verify")
		}
		if e.confirms != 1 {
			t.Errorf("Confirm was called %d times, want 1", e.confirms)
		}
	})
}

// --- --from-backup ----------------------------------------------------

// TestRestoreFromBackup: the recovery path puts the whole partition
// back, preamble and Apple's images included.
func TestRestoreFromBackup(t *testing.T) {
	good := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	backup := writeTemp(t, t.TempDir(), "fwpart.bin", good)

	// The device is in a different state: a later image and a scribble
	// in Apple's RSRC body.
	broken := syntheticPartition(t, plausibleImage(20000, 9), plausibleImage(4096, 2))
	broken[rsrcDevOffset+fwpart.BodyBias+5] ^= 0xFF
	e := newEnv(t, broken)

	res, err := Flash(context.Background(),
		Options{FromBackup: backup, Yes: true, BackupDir: e.backupDir}, e.deps())
	if err != nil {
		t.Fatalf("Flash --from-backup: %v\n%s", err, e.out.String())
	}
	if res.Mode != ModeRestore || !res.Verified {
		t.Fatalf("Mode=%q Verified=%v", res.Mode, res.Verified)
	}
	if !bytes.Equal(e.partition(), good) {
		t.Error("the partition is not the backup, byte for byte")
	}
	// The backup of what was there is still taken first.
	pre, rerr := os.ReadFile(onlyBackup(t, e.backupDir))
	if rerr != nil {
		t.Fatal(rerr)
	}
	if !bytes.Equal(pre, broken) {
		t.Error("the pre-restore backup is not the partition as it was")
	}
}

// TestRestoreRefusesABadPreamble: the preamble is the one thing the boot
// ROM insists on and the one thing no backup can be rebuilt without.
func TestRestoreRefusesABadPreamble(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	bad := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	copy(bad[0:4], []byte("XXXX"))
	path := writeTemp(t, t.TempDir(), "fwpart.bin", bad)

	_, err := Flash(context.Background(),
		Options{FromBackup: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if !errors.Is(err, ErrBadInput) {
		t.Fatalf("restore from a bad preamble = %v, want ErrBadInput", err)
	}
	if !errors.Is(err, fwpart.ErrNoPreamble) {
		t.Errorf("the refusal does not carry ErrNoPreamble: %v", err)
	}
	if len(e.opens) != 0 || e.writeCalls != 0 {
		t.Error("a file with no preamble still reached the device")
	}
}

// TestRestoreRefusesABadOSOSChecksum: unlike the device's own image, a
// backup that would not boot is refused — writing it over a partition
// that might still be recoverable makes things worse.
func TestRestoreRefusesABadOSOSChecksum(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	bad := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	bad[ososDevOffset+fwpart.BodyBias+3] ^= 0xFF
	path := writeTemp(t, t.TempDir(), "fwpart.bin", bad)

	_, err := Flash(context.Background(),
		Options{FromBackup: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if !errors.Is(err, ErrBadInput) {
		t.Fatalf("restore from a bad OSOS = %v, want ErrBadInput", err)
	}
	if !strings.Contains(err.Error(), "would not boot") {
		t.Errorf("the refusal does not explain itself:\n%v", err)
	}
}

// TestRestoreRefusesASizeMismatch: a restore to the wrong model is the
// mistake with no undo, and the partition size is the number that names
// the model.
func TestRestoreRefusesASizeMismatch(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	short := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	path := writeTemp(t, t.TempDir(), "fwpart.bin", short[:len(short)-testSectorSize])

	_, err := Flash(context.Background(),
		Options{FromBackup: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if !errors.Is(err, ErrBadInput) {
		t.Fatalf("restore of the wrong size = %v, want ErrBadInput", err)
	}
	if !strings.Contains(err.Error(), "different model") {
		t.Errorf("the refusal does not say why the size matters:\n%v", err)
	}
	if e.writeCalls != 0 {
		t.Error("a size mismatch still wrote")
	}
}

// --- elevation --------------------------------------------------------

// TestElevationDecisionTable drives the three outcomes without ever
// starting a process: elevated writes here, unelevated Windows hands the
// job to a child, and --no-relaunch (which every child is given) refuses
// with the command instead. That last row is the loop guard.
func TestElevationDecisionTable(t *testing.T) {
	before := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	path := writeIPod(t, t.TempDir(), "core.ipod", plausibleImage(12345, 3))

	t.Run("elevated writes in this process", func(t *testing.T) {
		e := newEnv(t, append([]byte(nil), before...))
		e.goos, e.elevated = "windows", true
		res, err := Flash(context.Background(),
			Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
		if err != nil {
			t.Fatalf("Flash: %v", err)
		}
		if e.relaunchCalls != 0 {
			t.Error("an elevated process relaunched itself")
		}
		if !res.Verified {
			t.Error("the elevated process did not verify")
		}
	})

	t.Run("windows unelevated relaunches", func(t *testing.T) {
		e := newEnv(t, append([]byte(nil), before...))
		e.goos, e.elevated = "windows", false
		e.relaunchLog = "child said VERIFIED\n"
		res, err := Flash(context.Background(),
			Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
		if err != nil {
			t.Fatalf("Flash: %v", err)
		}
		if e.relaunchCalls != 1 {
			t.Fatalf("Relaunch was called %d times, want 1", e.relaunchCalls)
		}
		if !res.Relaunched {
			t.Error("Result.Relaunched is not set")
		}
		args := strings.Join(e.relaunchArgs, " ")
		for _, want := range []string{"--yes", "--no-relaunch", "flash"} {
			if !strings.Contains(args, want) {
				t.Errorf("the child args %q do not contain %q", args, want)
			}
		}
		// The parent must not have done the work itself.
		if e.writeCalls != 0 {
			t.Error("the parent wrote to the device as well as relaunching")
		}
		if n := backupCount(t, e.backupDir); n != 0 {
			t.Error("the parent took a backup as well as relaunching")
		}
		if !strings.Contains(e.out.String(), "child said VERIFIED") {
			t.Errorf("the parent did not print the child's log:\n%s", e.out.String())
		}
	})

	t.Run("the child's exit code comes back", func(t *testing.T) {
		e := newEnv(t, append([]byte(nil), before...))
		e.goos, e.elevated = "windows", false
		e.relaunchCode, e.relaunchLog = 3, "error: the device vanished\n"
		res, err := Flash(context.Background(),
			Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
		if err == nil {
			t.Fatal("a child that exited 3 reported success")
		}
		if res.ChildExit != 3 {
			t.Errorf("Result.ChildExit = %d, want 3", res.ChildExit)
		}
		if !strings.Contains(e.out.String(), "the device vanished") {
			t.Error("the child's error did not reach the parent's output")
		}
	})

	t.Run("no-relaunch refuses with the command", func(t *testing.T) {
		e := newEnv(t, append([]byte(nil), before...))
		e.goos, e.elevated = "windows", false
		_, err := Flash(context.Background(),
			Options{Image: path, Yes: true, NoRelaunch: true, BackupDir: e.backupDir}, e.deps())
		if !errors.Is(err, ErrNeedsElevation) {
			t.Fatalf("--no-relaunch unelevated = %v, want ErrNeedsElevation", err)
		}
		if e.relaunchCalls != 0 {
			t.Fatal("--no-relaunch relaunched anyway — this is the loop guard")
		}
		for _, want := range []string{"RunAs", "core.exe", "flash"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("the refusal does not contain %q:\n%v", want, err)
			}
		}
		if strings.Contains(err.Error(), "--no-relaunch") {
			t.Error("the command printed for a human to run carries --no-relaunch")
		}
		if e.writeCalls != 0 {
			t.Error("the refused flash still wrote")
		}
	})

	t.Run("unix unelevated prints sudo", func(t *testing.T) {
		e := newEnv(t, append([]byte(nil), before...))
		e.goos, e.elevated = "linux", false
		_, err := Flash(context.Background(),
			Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
		if !errors.Is(err, ErrNeedsElevation) {
			t.Fatalf("unelevated on linux = %v, want ErrNeedsElevation", err)
		}
		if e.relaunchCalls != 0 {
			t.Error("a unix host tried to relaunch; sudo re-runs in place")
		}
		if !strings.Contains(err.Error(), "sudo ") {
			t.Errorf("the refusal does not carry the sudo line:\n%v", err)
		}
	})
}

// TestChildArgs pins what the elevated child is told: the user's own
// arguments, with anything that would make it prompt or relaunch
// removed, plus --yes and --no-relaunch exactly once.
func TestChildArgs(t *testing.T) {
	got := childArgs([]string{
		"flash", "core.ipod", "--yes", "-y", "--no-relaunch",
		"--elevated-log", "old.log", "--elevated-log=older.log", "--backup-dir", "B",
	})
	want := []string{"flash", "core.ipod", "--backup-dir", "B", "--yes", "--no-relaunch"}
	if strings.Join(got, " ") != strings.Join(want, " ") {
		t.Errorf("childArgs = %v\nwant %v", got, want)
	}
	if n := strings.Count(strings.Join(got, " "), "--yes"); n != 1 {
		t.Errorf("--yes appears %d times", n)
	}
}

// --- the backup itself ------------------------------------------------

// TestBackupFileName pins the default name: the size says which iPod a
// loose backup came from, and there is no colon because a colon is a
// legal path character on exactly one of the three operating systems.
func TestBackupFileName(t *testing.T) {
	when := time.Date(2026, 9, 15, 22, 5, 11, 0, time.FixedZone("PDT", -7*3600))
	got := BackupFileName(131475456, when)
	if want := "fwpart-131475456-2026-09-15T22-05-11-07-00.bin"; got != want {
		t.Errorf("BackupFileName = %q, want %q", got, want)
	}
	if strings.Contains(got, ":") {
		t.Error("the name contains a colon, which Windows will not accept in a path")
	}
}

// TestBackupIsNeverOverwritten: the second run is exactly when you have
// just broken the device the first backup came from.
func TestBackupIsNeverOverwritten(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	path := writeIPod(t, t.TempDir(), "core.ipod", plausibleImage(12345, 3))
	o := Options{Image: path, Yes: true, BackupDir: e.backupDir}

	if _, err := Flash(context.Background(), o, e.deps()); err != nil {
		t.Fatalf("first flash: %v", err)
	}
	first := onlyBackup(t, e.backupDir)
	before, _ := os.ReadFile(first)

	// Deps.Now is fixed, so the second run computes the same name.
	e2 := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	e2.backupDir = e.backupDir
	_, err := Flash(context.Background(), o, e2.deps())
	if err == nil || !strings.Contains(err.Error(), "already exists") {
		t.Fatalf("a second flash to the same backup name = %v, want a refusal", err)
	}
	if e2.writeCalls != 0 {
		t.Error("the device was written although the backup was refused")
	}
	after, _ := os.ReadFile(first)
	if !bytes.Equal(before, after) {
		t.Error("the first backup was modified")
	}
}

// TestFlashRefusesWithNoInput and the both-at-once case.
func TestFlashInputSelection(t *testing.T) {
	e := newEnv(t, syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2)))
	if _, err := Flash(context.Background(), Options{}, e.deps()); !errors.Is(err, ErrBadInput) {
		t.Errorf("Flash with no input = %v, want ErrBadInput", err)
	}
	_, err := Flash(context.Background(), Options{Image: "a", FromBackup: "b"}, e.deps())
	if !errors.Is(err, ErrBadInput) {
		t.Errorf("Flash with both inputs = %v, want ErrBadInput", err)
	}
}

// TestFlashReportsABadChecksumOnTheDeviceWithoutRefusing: a previous bad
// flash is precisely what someone is here to fix.
func TestFlashReportsABadChecksumOnTheDeviceWithoutRefusing(t *testing.T) {
	part := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	part[ososDevOffset+fwpart.BodyBias+11] ^= 0xFF
	e := newEnv(t, part)
	path := writeIPod(t, t.TempDir(), "core.ipod", plausibleImage(12345, 3))

	res, err := Flash(context.Background(),
		Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if err != nil {
		t.Fatalf("Flash over a bad image: %v", err)
	}
	if res.OldChecksumOK {
		t.Error("OldChecksumOK is true for a corrupt image")
	}
	if !res.Verified {
		t.Error("the replacement did not verify")
	}
	if !strings.Contains(e.out.String(), "does NOT verify") {
		t.Errorf("the plan did not report the bad image:\n%s", e.out.String())
	}
}

// TestIPodpatcherDiskNumber: the fallback line has to name the disk the
// user would type.
func TestIPodpatcherDiskNumber(t *testing.T) {
	for path, want := range map[string]int{
		`\\.\PhysicalDrive1`:  1,
		`\\.\PhysicalDrive12`: 12,
		"/dev/disk4":          4,
		"/dev/sdb":            1,
		"":                    1,
	} {
		if got := ipodpatcherDisk(path); got != want {
			t.Errorf("ipodpatcherDisk(%q) = %d, want %d", path, got, want)
		}
	}
}

// --- review additions (S7 review) -------------------------------------

// TestBackupPathIsDecidedOnce: the plan prints a backup path and the
// backup lands under exactly that name, even when the clock moves
// between the two. Before the fix Now() was called twice.
func TestBackupPathIsDecidedOnce(t *testing.T) {
	before := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	e := newEnv(t, append([]byte(nil), before...))
	path := writeIPod(t, t.TempDir(), "core.ipod", plausibleImage(12345, 3))
	d := e.deps()
	tick := 0
	d.Now = func() time.Time {
		tick++
		return time.Date(2026, 9, 15, 1, 2, tick, 0, time.UTC) // a new second per call
	}
	res, err := Flash(context.Background(), Options{Image: path, Yes: true, BackupDir: e.backupDir}, d)
	if err != nil {
		t.Fatalf("Flash: %v\n%s", err, e.out.String())
	}
	if !strings.Contains(e.out.String(), "backup       "+res.BackupPath) {
		t.Errorf("the plan printed a different backup path than the one written (%s):\n%s",
			res.BackupPath, e.out.String())
	}
	if _, err := os.Stat(res.BackupPath); err != nil {
		t.Errorf("Result.BackupPath does not exist: %v", err)
	}
}

// TestFlashRefusesAnotherModelsIPodFile: a nano .ipod verifies as a
// .ipod file and must still not be written to a Video.
func TestFlashRefusesAnotherModelsIPodFile(t *testing.T) {
	before := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	e := newEnv(t, append([]byte(nil), before...))
	image := plausibleImage(12345, 3)
	var buf bytes.Buffer
	if err := firmware.WriteIPodFile(&buf, firmware.ModelIPodNano, firmware.ModelNameIPodNano, image); err != nil {
		t.Fatal(err)
	}
	path := writeTemp(t, t.TempDir(), "nano.ipod", buf.Bytes())
	_, err := Flash(context.Background(), Options{Image: path, Yes: true, BackupDir: e.backupDir}, e.deps())
	if !errors.Is(err, ErrBadInput) || !strings.Contains(err.Error(), "nano") {
		t.Fatalf("a nano .ipod = %v, want ErrBadInput naming the model", err)
	}
	if e.writeCalls != 0 || backupCount(t, e.backupDir) != 0 {
		t.Error("the refused image still reached the device or a backup")
	}
}

// TestFallbackNamesTheRightIPodpatcherVerb: -wf for a .ipod, -wfb for a
// bare image, -w for a partition dump.
func TestFallbackNamesTheRightIPodpatcherVerb(t *testing.T) {
	before := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	dir := t.TempDir()
	ipod := writeIPod(t, dir, "core.ipod", plausibleImage(12345, 3))
	bin := writeTemp(t, dir, "core.bin", plausibleImage(12345, 3))
	dump := writeTemp(t, dir, "fwpart.bin", before)
	cases := []struct {
		name string
		o    Options
		verb string
	}{
		{"ipod", Options{Image: ipod}, " -wf "},
		{"bin", Options{Image: bin}, " -wfb "},
		{"restore", Options{FromBackup: dump}, " -w "},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			e := newEnv(t, append([]byte(nil), before...))
			e.failWriteN = 1
			c.o.Yes, c.o.BackupDir = true, e.backupDir
			_, err := Flash(context.Background(), c.o, e.deps())
			if err == nil {
				t.Fatal("the injected fault did not fail the flash")
			}
			if !strings.Contains(err.Error(), "ipodpatcher 9"+c.verb) {
				t.Errorf("fallback does not say `ipodpatcher 9%s…`:\n%v", c.verb, err)
			}
		})
	}
}

// TestWindowsAdviceBeforeAPlanLeadsWithDryRun: when the device could not
// even be read, the pasted line must not be the --yes one alone.
func TestWindowsAdviceBeforeAPlanLeadsWithDryRun(t *testing.T) {
	before := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	e := newEnv(t, append([]byte(nil), before...))
	e.goos, e.elevated = "windows", false
	e.selectErr = disk.ErrRawAccessDenied
	path := writeIPod(t, t.TempDir(), "core.ipod", plausibleImage(12345, 3))
	_, err := Flash(context.Background(), Options{Image: path, BackupDir: e.backupDir}, e.deps())
	if err == nil {
		t.Fatal("a denied device read reported success")
	}
	msg := err.Error()
	dry := strings.Index(msg, "--dry-run")
	yes := strings.Index(msg, "--yes")
	if dry < 0 || yes < 0 || dry > yes {
		t.Errorf("the advice should show a --dry-run line before the --yes line:\n%s", msg)
	}
	if e.relaunchCalls != 0 {
		t.Error("relaunched without a plan")
	}

	// After a plan was shown (device readable, write refused), the
	// single --yes line is right.
	e2 := newEnv(t, append([]byte(nil), before...))
	e2.goos, e2.elevated = "windows", false
	_, err = Flash(context.Background(), Options{Image: path, NoRelaunch: true, Yes: true,
		BackupDir: e2.backupDir}, e2.deps())
	if err == nil || strings.Contains(err.Error(), "--dry-run") {
		t.Errorf("after a shown plan the advice should be the single write line:\n%v", err)
	}
}

// ctxCancelHandle cancels the context on the first WriteAt, so the
// restore loop sees a dead ctx after its first chunk.
type ctxCancelHandle struct {
	disk.Handle
	cancel func()
}

func (h *ctxCancelHandle) WriteAt(p []byte, off int64) (int, error) {
	h.cancel()
	return h.Handle.WriteAt(p, off)
}

// TestRestoreFinishesOnceStarted: a cancel that arrives after the first
// chunk landed must not stop a whole-partition restore halfway.
func TestRestoreFinishesOnceStarted(t *testing.T) {
	good := syntheticPartition(t, plausibleImage(8192, 1), plausibleImage(4096, 2))
	backup := writeTemp(t, t.TempDir(), "fwpart.bin", good)
	broken := syntheticPartition(t, plausibleImage(20000, 9), plausibleImage(4096, 2))
	e := newEnv(t, broken)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	d := e.deps()
	inner := d.Open
	d.Open = func(path string, write bool) (disk.Handle, error) {
		h, err := inner(path, write)
		if err != nil || !write {
			return h, err
		}
		return &ctxCancelHandle{Handle: h, cancel: cancel}, nil
	}
	res, err := Flash(ctx, Options{FromBackup: backup, Yes: true, BackupDir: e.backupDir}, d)
	if err != nil {
		t.Fatalf("restore stopped on a mid-write cancel: %v\n%s", err, e.out.String())
	}
	if !res.Verified || !bytes.Equal(e.partition(), good) {
		t.Error("the restore did not complete")
	}
}

func TestConfirmPromptRoundTrips(t *testing.T) {
	for _, target := range []string{`\\.\PhysicalDrive2`, "/dev/sdb", "/dev/rdisk4"} {
		got, ok := ConfirmTarget(ConfirmPrompt(target))
		if !ok || got != target {
			t.Errorf("ConfirmTarget(ConfirmPrompt(%q)) = %q, %v", target, got, ok)
		}
	}
	for _, bad := range []string{"", "type the device path: ", ConfirmPrompt("")} {
		if got, ok := ConfirmTarget(bad); ok {
			t.Errorf("ConfirmTarget(%q) = %q, ok; want !ok", bad, got)
		}
	}
}
