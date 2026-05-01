// Package firmware deals with the iPod firmware-partition image format
// (the wire format the boot ROM expects, not our higher-level packaging).
//
// See core/docs/hw/08-boot-dock.md for the full spec.
package firmware

// ModelNum is the additive-checksum seed for each iPod model. The
// firmware-partition checksum is `sum(image_bytes) + ModelNum`,
// 32-bit wrapping. The boot ROM compares this against the value in
// the directory entry; mismatch = it refuses to load.
type ModelNum uint32

// Known model seeds. The set we actually care about is just iPodVideo.
const (
	ModelIPodVideo ModelNum = 0x05
	ModelIPodNano  ModelNum = 0x04 // for completeness; we don't target it
)

// Checksum computes the additive 32-bit checksum used by the iPod
// firmware-partition image format. Treats `data` as a sequence of
// unsigned bytes and accumulates into a 32-bit value, starting from
// the model seed.
//
// Performance: on Go 1.22 with bounds-check elision this is ~1 GB/s
// on a typical x86-64. We don't optimize further because the checksum
// runs at install time on hosts, not in the firmware hot path.
func Checksum(model ModelNum, data []byte) uint32 {
	c := uint32(model)
	for _, b := range data {
		c += uint32(b)
	}
	return c
}
