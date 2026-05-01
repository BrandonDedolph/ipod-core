package firmware

import (
	"bytes"
	"testing"
)

func TestChecksum_EmptyImage(t *testing.T) {
	// Empty image: sum is just the model seed.
	got := Checksum(ModelIPodVideo, nil)
	want := uint32(0x05)
	if got != want {
		t.Errorf("Checksum(empty) = %#x, want %#x", got, want)
	}
}

func TestChecksum_KnownInput(t *testing.T) {
	// Hand-computed: 0x05 + 0x10 + 0x20 + 0x30 = 0x65.
	got := Checksum(ModelIPodVideo, []byte{0x10, 0x20, 0x30})
	want := uint32(0x65)
	if got != want {
		t.Errorf("Checksum([16,32,48]) = %#x, want %#x", got, want)
	}
}

func TestChecksum_LargeNoWrap(t *testing.T) {
	// 256 KB of 0xFF bytes: 256 * 1024 * 0xFF = 0x3FC0000.
	// Plus model seed (0x05) = 0x3FC0005. No wrap at 32-bit.
	data := bytes.Repeat([]byte{0xFF}, 256*1024)
	got := Checksum(ModelIPodVideo, data)
	want := uint32(0x3FC0005)
	if got != want {
		t.Errorf("Checksum(256K of 0xFF) = %#x, want %#x", got, want)
	}
}

func TestChecksum_Wraps32Bit(t *testing.T) {
	// Feed enough 0xFF bytes to overflow uint32 by 0xFF and confirm wrap to 0xFE.
	// Size chosen so size*0xFF == 0xFFFFFFFF + 0xFF.
	const size = (0xFFFF_FFFF / 0xFF) + 1
	data := bytes.Repeat([]byte{0xFF}, size)
	got := Checksum(0, data) // seed = 0; the wrap is the only thing under test
	want := uint32(0xFE)
	if got != want {
		t.Errorf("Checksum(wrap) = %#x, want %#x", got, want)
	}
}

func TestChecksum_DifferentModelSeeds(t *testing.T) {
	data := []byte{0x42}
	if Checksum(ModelIPodVideo, data) == Checksum(ModelIPodNano, data) {
		t.Error("seeds should produce different sums for non-empty data")
	}
}

// Sanity check: confirm the algorithm is order-independent at the byte
// level (additive). This guarantee lets us checksum streamed input
// without buffering the whole image.
func TestChecksum_OrderIndependent(t *testing.T) {
	a := []byte{0x11, 0x22, 0x33, 0x44}
	b := []byte{0x44, 0x33, 0x22, 0x11}
	if Checksum(ModelIPodVideo, a) != Checksum(ModelIPodVideo, b) {
		t.Error("byte-order shouldn't change the additive sum")
	}
}

func BenchmarkChecksum_1MB(b *testing.B) {
	data := bytes.Repeat([]byte{0xAB}, 1<<20)
	b.SetBytes(int64(len(data)))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		_ = Checksum(ModelIPodVideo, data)
	}
}
