package disk

import (
	"errors"
	"io/fs"
	"strings"
	"testing"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
)

// applePreamble builds the first 512 bytes of an iPod firmware
// partition: the "{{~~" banner, the copyright line fwpart.CheckPreamble
// looks for, and the "]ih[" directory marker at 0x100. Only the three
// checked landmarks matter; the ASCII art around them does not.
func applePreamble() []byte {
	b := make([]byte, 512)
	copy(b, "{{~~  /-----\\")
	copy(b[0x30:], "Copyright(C) 2001 Apple Computer, Inc.---")
	copy(b[firmware.DirectoryMarkerOffset:], firmware.DirectoryMarker[:])
	return b
}

// fakeIPodImage builds a device image: an MBR in sector 0 and an Apple
// preamble at partition 0's start, as measured at trueSectorSize.
func fakeIPodImage(trueSectorSize int, parts []Partition) []byte {
	start := parts[0].ByteStart(trueSectorSize)
	size := start + 4096
	if r := size % int64(trueSectorSize); r != 0 {
		size += int64(trueSectorSize) - r
	}
	img := make([]byte, size)
	copy(img, BuildMBR(parts))
	copy(img[start:], applePreamble())
	return img
}

type fakeDisk struct {
	disk     Disk
	image    []byte
	openErr  error
	handleSS int // the sector size the HANDLE reports (may differ from Disk.SectorSize)
}

// installFakes points FindIPods at a fixed set of disks. Returns a
// cleanup the test defers.
func installFakes(t *testing.T, fakes ...fakeDisk) {
	t.Helper()
	oldList, oldOpen := listDisks_, openDisk_
	t.Cleanup(func() { listDisks_, openDisk_ = oldList, oldOpen })

	byPath := map[string]fakeDisk{}
	var disks []Disk
	for _, f := range fakes {
		byPath[f.disk.Path] = f
		disks = append(disks, f.disk)
	}
	listDisks_ = func() ([]Disk, error) { return disks, nil }
	openDisk_ = func(path string, write bool) (Handle, error) {
		f, ok := byPath[path]
		if !ok {
			return nil, fs.ErrNotExist
		}
		if f.openErr != nil {
			return nil, f.openErr
		}
		ss := f.handleSS
		if ss == 0 {
			ss = f.disk.SectorSize
		}
		h, _, err := NewMemDevice(append([]byte(nil), f.image...), ss, write)
		return h, err
	}
}

// realIPodDisk is what Windows reports for this project's device: an
// Apple product string, a USB bus, 80,026,361,856 bytes, and a bridge
// that says its sectors are 2048 bytes.
func realIPodDisk(path string) Disk {
	return Disk{
		Path:       path,
		SizeBytes:  80026361856,
		SectorSize: 2048,
		Vendor:     "Apple",
		Model:      "iPod",
		Serial:     "000A27001234ABCD",
		Removable:  true,
		USB:        true,
		Volumes:    []string{"D:"},
	}
}

func TestFindIPodsHappyPath(t *testing.T) {
	installFakes(t, fakeDisk{
		disk:  realIPodDisk(`\\.\PhysicalDrive1`),
		image: fakeIPodImage(2048, realTable()),
	})

	pods, err := FindIPods()
	if err != nil {
		t.Fatalf("FindIPods: %v", err)
	}
	if len(pods) != 1 {
		t.Fatalf("FindIPods found %d iPods, want 1", len(pods))
	}
	p := pods[0]
	if p.SectorSize != 2048 || p.SectorSizeSource != SectorSizeFromOS {
		t.Errorf("sector size = %d (%s), want 2048 from the OS", p.SectorSize, p.SectorSizeSource)
	}
	if p.FWPartStart != 129024 {
		t.Errorf("FWPartStart = %d, want 129024", p.FWPartStart)
	}
	if p.FWPartLen != 131475456 {
		t.Errorf("FWPartLen = %d, want 131475456 (the size of both bring-up dumps)", p.FWPartLen)
	}
	if p.DataPartStart != 131604480 {
		t.Errorf("DataPartStart = %d, want 131604480", p.DataPartStart)
	}
	if !p.Tested {
		t.Errorf("the 80 GB device is not Tested: %s", p.UntestedReason)
	}
	if p.Model != TestedModel {
		t.Errorf("Model = %q, want %q", p.Model, TestedModel)
	}
}

// TestFindIPodsSectorSizeCrossCheck is the reviewer-checklist item
// "sector size comes from the OS and is cross-checked by the preamble
// position". Both directions of a disagreement must be caught AND
// reported, because the correction is silent otherwise and the fact
// that the OS was wrong is exactly what a person debugging a failed
// flash needs to see.
func TestFindIPodsSectorSizeCrossCheck(t *testing.T) {
	cases := []struct {
		name       string
		osSize     int // what the driver claims
		trueSize   int // what the partition table is really in
		wantSize   int
		wantSource string
	}{
		{"os is right", 2048, 2048, 2048, SectorSizeFromOS},
		{"os says 512, bridge means 2048", 512, 2048, 2048, SectorSizeProbed2048},
		{"os says 2048, disk means 512", 2048, 512, 512, SectorSizeProbed512},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			d := realIPodDisk(`\\.\PhysicalDrive1`)
			d.SectorSize = c.osSize
			installFakes(t, fakeDisk{
				disk:     d,
				image:    fakeIPodImage(c.trueSize, realTable()),
				handleSS: c.osSize,
			})
			pods, err := FindIPods()
			if err != nil {
				t.Fatalf("FindIPods: %v", err)
			}
			if len(pods) != 1 {
				t.Fatalf("found %d iPods, want 1", len(pods))
			}
			if pods[0].SectorSize != c.wantSize {
				t.Errorf("SectorSize = %d, want %d", pods[0].SectorSize, c.wantSize)
			}
			if pods[0].SectorSizeSource != c.wantSource {
				t.Errorf("SectorSizeSource = %q, want %q", pods[0].SectorSizeSource, c.wantSource)
			}
			if pods[0].FWPartStart != int64(realFWStart)*int64(c.wantSize) {
				t.Errorf("FWPartStart = %d, want %d",
					pods[0].FWPartStart, int64(realFWStart)*int64(c.wantSize))
			}
		})
	}
}

// TestFindIPodsRejects covers every disk that must NOT be identified.
// Each case changes exactly one of the three checks.
func TestFindIPodsRejects(t *testing.T) {
	notAnIPod := func(mutate func(*fakeDisk)) fakeDisk {
		f := fakeDisk{
			disk:  realIPodDisk(`\\.\PhysicalDrive0`),
			image: fakeIPodImage(2048, realTable()),
		}
		mutate(&f)
		return f
	}

	cases := []struct {
		name string
		disk fakeDisk
	}{
		{"no Apple preamble at partition 0", notAnIPod(func(f *fakeDisk) {
			copy(f.image[129024:129024+8], make([]byte, 8))
		})},
		{"partition 0 is not type 0x00", notAnIPod(func(f *fakeDisk) {
			p := realTable()
			p[0].Type = 0x83
			copy(f.image, BuildMBR(p))
		})},
		{"partition 0 slot is empty", notAnIPod(func(f *fakeDisk) {
			p := realTable()
			p[0] = Partition{Index: 0}
			copy(f.image, BuildMBR(p))
		})},
		{"partition 1 is not FAT32", notAnIPod(func(f *fakeDisk) {
			p := realTable()
			p[1].Type = 0xAF
			copy(f.image, BuildMBR(p))
		})},
		{"no MBR signature", notAnIPod(func(f *fakeDisk) {
			f.image[0x1FE], f.image[0x1FF] = 0, 0
		})},
		{"the OS says it is a Seagate", notAnIPod(func(f *fakeDisk) {
			f.disk.Vendor, f.disk.Model = "Seagate", "ST2000DM008"
		})},
		{"the disk cannot be opened", notAnIPod(func(f *fakeDisk) {
			f.openErr = errors.New("device busy")
		})},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			installFakes(t, c.disk)
			pods, err := FindIPods()
			if err != nil {
				t.Fatalf("FindIPods: %v", err)
			}
			if len(pods) != 0 {
				t.Errorf("FindIPods identified an iPod it should have skipped: %+v", pods[0].Describe())
			}
		})
	}
}

// TestFindIPodsTestedGate: an iPod that is not the 80 GB is still found
// and still readable — it just is not Tested, which is what the write
// path (S7) refuses on.
func TestFindIPodsTestedGate(t *testing.T) {
	cases := []struct {
		name       string
		size       int64
		wantTested bool
	}{
		{"30 GB 5G", 29_996_000_000, false},
		{"60 GB 5.5G", 59_992_000_000, false},
		{"the real 80 GB", 80_026_361_856, true},
		{"just inside the lower bound", TestedMinBytes, true},
		{"just below the lower bound", TestedMinBytes - 1, false},
		{"just inside the upper bound", TestedMaxBytes, true},
		{"a 128 GB iFlash rebuild", 128_035_676_160, false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			d := realIPodDisk(`\\.\PhysicalDrive1`)
			d.SizeBytes = c.size
			installFakes(t, fakeDisk{disk: d, image: fakeIPodImage(2048, realTable())})
			pods, err := FindIPods()
			if err != nil || len(pods) != 1 {
				t.Fatalf("FindIPods = %v, %v", pods, err)
			}
			if pods[0].Tested != c.wantTested {
				t.Errorf("Tested = %v, want %v (%s)", pods[0].Tested, c.wantTested, HumanSize(c.size))
			}
			if !c.wantTested {
				if pods[0].UntestedReason == "" {
					t.Error("an untested device gave no reason")
				}
				ue := &UntestedHardwareError{Pod: pods[0]}
				if !strings.Contains(ue.Error(), "--untested-hardware") {
					t.Error("the untested-hardware error does not name the flag that overrides it")
				}
				if !strings.Contains(pods[0].Describe(), "UNTESTED") {
					t.Error("Describe() does not mark an untested device")
				}
			}
		})
	}
}

// TestFindIPodsAllOpensDenied: "no iPod found" and "you are not
// Administrator" are different answers, and only one of them is
// actionable. Windows without elevation produces the second.
func TestFindIPodsAllOpensDenied(t *testing.T) {
	installFakes(t,
		fakeDisk{disk: realIPodDisk(`\\.\PhysicalDrive0`), openErr: fs.ErrPermission},
		fakeDisk{disk: realIPodDisk(`\\.\PhysicalDrive1`), openErr: fs.ErrPermission},
	)
	_, err := FindIPods()
	if !errors.Is(err, ErrRawAccessDenied) {
		t.Fatalf("FindIPods with every open denied = %v, want ErrRawAccessDenied", err)
	}
	if !strings.Contains(err.Error(), "2 disk(s)") {
		t.Errorf("the error does not say how many disks were there: %v", err)
	}
}

func TestSelectIPod(t *testing.T) {
	d0 := realIPodDisk(`\\.\PhysicalDrive1`)
	d1 := realIPodDisk(`\\.\PhysicalDrive2`)
	d1.Serial = "000A27009999FFFF"
	image := fakeIPodImage(2048, realTable())

	t.Run("no device", func(t *testing.T) {
		installFakes(t)
		if _, err := SelectIPod(""); !errors.Is(err, ErrNoDevice) {
			t.Errorf("SelectIPod with nothing attached = %v, want ErrNoDevice", err)
		}
	})

	t.Run("exactly one", func(t *testing.T) {
		installFakes(t, fakeDisk{disk: d0, image: image})
		p, err := SelectIPod("")
		if err != nil {
			t.Fatalf("SelectIPod: %v", err)
		}
		if p.Disk.Path != d0.Path {
			t.Errorf("selected %s, want %s", p.Disk.Path, d0.Path)
		}
	})

	t.Run("two without --device", func(t *testing.T) {
		installFakes(t, fakeDisk{disk: d0, image: image}, fakeDisk{disk: d1, image: image})
		_, err := SelectIPod("")
		if err == nil {
			t.Fatal("SelectIPod picked one of two iPods at random")
		}
		// The error must LIST them; "specify which one" with no list is
		// the message this project already had and could not act on.
		for _, want := range []string{d0.Path, d1.Path, "select one with --device"} {
			if !strings.Contains(err.Error(), want) {
				t.Errorf("the multi-device error does not mention %q:\n%v", want, err)
			}
		}
	})

	t.Run("selected by path and by serial", func(t *testing.T) {
		installFakes(t, fakeDisk{disk: d0, image: image}, fakeDisk{disk: d1, image: image})
		for _, sel := range []string{d1.Path, `\\.\physicaldrive2`, d1.Serial} {
			p, err := SelectIPod(sel)
			if err != nil {
				t.Errorf("SelectIPod(%q): %v", sel, err)
				continue
			}
			if p.Disk.Path != d1.Path {
				t.Errorf("SelectIPod(%q) chose %s, want %s", sel, p.Disk.Path, d1.Path)
			}
		}
		if _, err := SelectIPod("/dev/nope"); !errors.Is(err, ErrNoDevice) {
			t.Errorf("SelectIPod with an unmatched selector = %v, want ErrNoDevice", err)
		}
	})
}
