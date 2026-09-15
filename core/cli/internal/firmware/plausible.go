package firmware

import (
	"fmt"
	"io"
)

// Sanity bounds on a raw firmware image.
//
// These are NOT the firmware-partition budget — the real limit is the
// capacity of the OSOS entry on the target device, which only
// fwpart.Directory.Capacity can answer and which `core flash` checks
// separately. They exist to catch the failure that actually happens:
// objcopy producing an empty or truncated core.bin, which without them
// packs into a perfectly "valid" core.ipod and makes `make ipod` report
// success — or, worse, gets flashed.
//
// This lived in internal/cli next to `firmware pack` until `core flash`
// needed the same rules for a raw .bin handed straight to it. One copy,
// in the package that owns the image format; internal/cli keeps thin
// wrappers so the pack command reads the same as before.
const (
	// MinImageBytes is far below any real build (our own image is
	// ~360 KB, a stock Rockbox build ~760 KB) but above the 0-and-change
	// bytes a failed objcopy leaves behind.
	MinImageBytes = 4096
	// MaxImageBytes is a generous ceiling; Apple's own OSOS images are a
	// few MB.
	MaxImageBytes = 32 << 20
)

// ValidateImage rejects images that cannot plausibly be firmware.
// Structural oddities that are merely suspicious are written to `warn`
// rather than failing.
//
// `action` is the verb in the refusals ("pack", "flash"), so the same
// rules can say "refusing to pack" and "refusing to flash" without two
// copies of the rules.
func ValidateImage(name, action string, image []byte, warn io.Writer) error {
	switch {
	case len(image) == 0:
		return fmt.Errorf("%s is empty; refusing to %s a 0-byte firmware image "+
			"(the usual cause is objcopy failing silently)", name, action)
	case len(image) < MinImageBytes:
		return fmt.Errorf("%s is only %d bytes, below the %d-byte minimum for a plausible "+
			"firmware image; refusing to %s what looks like a truncated build",
			name, len(image), MinImageBytes, action)
	case len(image) > MaxImageBytes:
		return fmt.Errorf("%s is %d bytes, above the %d-byte sanity ceiling for a firmware "+
			"image; refusing to %s (is this really core.bin?)",
			name, len(image), MaxImageBytes, action)
	}
	if b, uniform := UniformByte(image); uniform {
		return fmt.Errorf("%s is %d bytes of nothing but %#02x; refusing to %s a blank image",
			name, len(image), b, action)
	}
	// The reset vector of our image is a branch, but that is not a
	// property of .ipod images in general: a stock rockbox.ipod starts
	// with 0xE321F0D3 (MSR CPSR_c, ...), not a branch. So a non-branch
	// first word is worth mentioning and nothing more.
	if !LooksLikeARMBranch(image) && warn != nil {
		fmt.Fprintf(warn, "warning: %s does not begin with an ARM branch instruction "+
			"(first word %#08x); %sing anyway\n", name, FirstWordLE(image), action)
	}
	return nil
}

// UniformByte reports whether every byte of b is identical (the shape of
// an all-zero or erased-flash 0xFF buffer).
func UniformByte(b []byte) (byte, bool) {
	if len(b) == 0 {
		return 0, false
	}
	for _, x := range b[1:] {
		if x != b[0] {
			return 0, false
		}
	}
	return b[0], true
}

// FirstWordLE is the first little-endian 32-bit word of an image, 0 for
// anything shorter than a word.
func FirstWordLE(image []byte) uint32 {
	if len(image) < 4 {
		return 0
	}
	return uint32(image[0]) | uint32(image[1])<<8 | uint32(image[2])<<16 | uint32(image[3])<<24
}

// LooksLikeARMBranch reports whether the first word decodes as an ARM
// B/BL: bits 27:25 == 0b101, with any condition code other than 0b1111
// (which is the unconditional-instruction space, not a branch).
func LooksLikeARMBranch(image []byte) bool {
	if len(image) < 4 {
		return false
	}
	w := FirstWordLE(image)
	return (w&0x0E000000) == 0x0A000000 && (w>>28) != 0xF
}
