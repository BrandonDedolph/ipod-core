package disk

import (
	"errors"
	"testing"
)

// The real table, as ipodpatcher prints it for this project's device:
//
//	Part  Start Sector  End Sector  Size (MB)  Type
//	   0            63       64259      125.4  Empty (0x00)
//	   1         64260    39075370    76193.6  W95 FAT32 (0x0b)
//
// so partition 0 is 64,259 − 63 + 1 = 64,197 sectors and partition 1 is
// 39,011,111. At the 2048-byte sector the USB bridge reports, partition
// 0 is byte 129,024 and 131,475,456 bytes long — which is exactly the
// size of both partition dumps in the bring-up folder, and the reason
// that number is the fixture here rather than a round one.
const (
	realFWStart  = 63
	realFWCount  = 64197
	realFWEnd    = 64259
	realDataLBA  = 64260
	realDataEnd  = 39075370
	realDataSect = realDataEnd - realDataLBA + 1
)

func realTable() []Partition {
	return []Partition{
		{Index: 0, Type: TypeEmpty, StartLBA: realFWStart, NumSectors: realFWCount},
		{Index: 1, Type: TypeFAT32CHS, StartLBA: realDataLBA, NumSectors: realDataSect},
		{Index: 2},
		{Index: 3},
	}
}

func TestParseMBRRealTable(t *testing.T) {
	parts, err := ParseMBR(BuildMBR(realTable()))
	if err != nil {
		t.Fatalf("ParseMBR: %v", err)
	}
	if len(parts) != 4 {
		t.Fatalf("ParseMBR returned %d slots, want 4 (slot numbers are load-bearing)", len(parts))
	}

	fw := parts[0]
	if fw.Type != TypeEmpty {
		t.Errorf("partition 0 type = %#02x, want 0x00", fw.Type)
	}
	if !fw.Used() {
		t.Error("partition 0 reports Used()==false; type 0x00 with an extent IS the firmware partition")
	}
	if fw.StartLBA != realFWStart || fw.NumSectors != realFWCount {
		t.Errorf("partition 0 = start %d count %d, want %d/%d",
			fw.StartLBA, fw.NumSectors, realFWStart, realFWCount)
	}
	if fw.EndLBA() != realFWEnd {
		t.Errorf("partition 0 EndLBA = %d, want %d (ipodpatcher's number)", fw.EndLBA(), realFWEnd)
	}

	data := parts[1]
	if data.Type != TypeFAT32CHS {
		t.Errorf("partition 1 type = %#02x, want 0x0b", data.Type)
	}
	if data.StartLBA != realDataLBA {
		t.Errorf("partition 1 start = %d, want %d", data.StartLBA, realDataLBA)
	}
	for _, i := range []int{2, 3} {
		if parts[i].Used() {
			t.Errorf("slot %d reports Used() on an all-zero entry", i)
		}
	}
}

// TestParseMBRSectorUnits is the whole reason ByteStart takes an
// argument. The SAME table means two different byte ranges at 512 and
// at 2048, and picking the wrong one puts the firmware write 96 KB
// before the firmware.
func TestParseMBRSectorUnits(t *testing.T) {
	parts, err := ParseMBR(BuildMBR(realTable()))
	if err != nil {
		t.Fatalf("ParseMBR: %v", err)
	}
	fw := parts[0]

	cases := []struct {
		sectorSize  int
		start, size int64
	}{
		// 2048: what the USB bridge reports, and what produced the
		// 131,475,456-byte dumps.
		{2048, 129024, 131475456},
		// 512: what the same table would mean over an ATA bridge.
		{512, 32256, 32868864},
	}
	for _, c := range cases {
		if got := fw.ByteStart(c.sectorSize); got != c.start {
			t.Errorf("ByteStart(%d) = %d, want %d", c.sectorSize, got, c.start)
		}
		if got := fw.ByteLength(c.sectorSize); got != c.size {
			t.Errorf("ByteLength(%d) = %d, want %d", c.sectorSize, got, c.size)
		}
	}
	if got := parts[1].ByteStart(2048); got != 131604480 {
		t.Errorf("partition 1 ByteStart(2048) = %d, want 131604480 (right after partition 0)", got)
	}
}

func TestParseMBRSignature(t *testing.T) {
	good := BuildMBR(realTable())

	bad := append([]byte(nil), good...)
	bad[0x1FE], bad[0x1FF] = 0, 0
	if _, err := ParseMBR(bad); !errors.Is(err, ErrNoMBR) {
		t.Errorf("ParseMBR on a sector with no signature = %v, want ErrNoMBR", err)
	}
	if _, err := ParseMBR(good[:100]); !errors.Is(err, ErrNoMBR) {
		t.Errorf("ParseMBR on a short buffer = %v, want ErrNoMBR", err)
	}
	// A larger buffer (a whole 2048-byte sector read) must still parse:
	// the table is in the first 512 bytes of sector 0 whatever the
	// sector size is.
	big := make([]byte, 2048)
	copy(big, good)
	if _, err := ParseMBR(big); err != nil {
		t.Errorf("ParseMBR on a 2048-byte sector: %v", err)
	}
}

func TestPartitionTypeNames(t *testing.T) {
	cases := map[byte]string{
		TypeEmpty:    "Empty (0x00)",
		TypeFAT32CHS: "W95 FAT32 (0x0b)",
		TypeFAT32LBA: "W95 FAT32 LBA (0x0c)",
		0xAF:         "HFS/HFS+ (0xaf)",
	}
	for typ, want := range cases {
		if got := (Partition{Type: typ}).TypeName(); got != want {
			t.Errorf("TypeName(%#02x) = %q, want %q", typ, got, want)
		}
	}
}
