package fwpart

import (
	"fmt"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/firmware"
)

// What is on the firmware partition, and what a write does to the OSOS
// row's entry point. Both are here because they are the same fact seen
// from two sides: our image starts executing at its first byte, Apple's
// does not, and the directory row is where that difference lives.

// Policy selects what PlanWrite does with the OSOS row's EntryOffset.
//
// On a stock iPod the row reads `entryOffset 0x736000` — 7.5 MB into
// Apple's 7.6 MB image. Our image is 368 KB and its entry point is byte
// 0 (crt0.S is first in core/boot/linker.ld), so writing our body while
// leaving Apple's entry point in place produces a row that points past
// the end of what was written: a device that boots to nothing until
// Select+Play. ipodpatcher's -wf zeroes the field; so does CoreImage.
//
// KeepEntry is the behaviour this package had before the field was
// noticed. Nothing in the flash path uses it — a Core image's entry
// point is its first byte on every device, so there is no case where
// keeping Apple's value is right (library-manager-plan.md, decision 1)
// — and it exists so that the difference is testable and so that a
// future writer for somebody else's image has a way to say "leave the
// row alone".
type Policy int

const (
	// KeepEntry preserves every field but Length and Checksum.
	KeepEntry Policy = iota
	// CoreImage additionally sets EntryOffset to 0. LoadAddr, Version
	// and LoadAddr2 are still preserved: those are the values
	// ipodpatcher leaves in place and the boot ROM expects.
	CoreImage
)

func (p Policy) String() string {
	switch p {
	case KeepEntry:
		return "keep-entry"
	case CoreImage:
		return "core-image"
	default:
		return fmt.Sprintf("Policy(%d)", int(p))
	}
}

// InstalledKind is the three-way answer to "is Core on this device?".
//
// It is a string and not an iota because the zero value of an iota
// would be a kind — an Installed nobody filled in would claim to be
// whichever one came first, and this value decides whether a write is
// an install or an update. "" is not a kind and reads as "unclassified"
// everywhere.
type InstalledKind string

const (
	// Core: the OSOS body carries a CORE-FW-VERSION marker. Only our
	// images do, from v0.1.3 on.
	Core InstalledKind = "core"
	// CoreOld: no marker, but the row has the shape only our images
	// have — entry point 0 and a body under a megabyte. Every Core
	// image before v0.1.3 lands here; it is an ordinary "update
	// available", not a reason to stop.
	CoreOld InstalledKind = "core-old"
	// Other: Apple's firmware (entry 0x736000, 7.6 MB), somebody
	// else's, or an OSOS entry that could not be read. This is the
	// state `core install` exists for.
	Other InstalledKind = "other"
)

func (k InstalledKind) String() string {
	if k == "" {
		return "unclassified"
	}
	return string(k)
}

// Installed is what Classify found.
type Installed struct {
	Kind InstalledKind
	// Version and BuildID are the two halves of the marker, set only
	// for Core.
	Version, BuildID string
	// Description is one line naming what is there, for the install
	// prompt and the app's Install card: "Apple firmware (7.6 MB, entry
	// 0x736000)", "Core v0.1.3 (build v0.1.3-2-gabc)".
	Description string
}

// IsCore reports whether this is one of ours — either era. It is the
// predicate `core install` refuses on and `core update` needs.
func (i Installed) IsCore() bool { return i.Kind == Core || i.Kind == CoreOld }

// CoreMaxImage bounds a Core image for the marker-less case. The images
// this project builds are ~368 KB and the OSOS capacity is 7.6 MB;
// a megabyte is well clear of one and well clear of the other, and the
// only thing on the wrong side of it is somebody's full-size firmware.
const CoreMaxImage = 1 << 20

// Classify decides what is installed from the OSOS directory row and
// its body.
//
// The predicate, in order (library-manager-plan.md, decision 2):
//
//   - the body carries a CORE-FW-VERSION marker -> Core, with the
//     version and build id;
//   - no marker, EntryOffset == 0 and Length < 1 MiB -> CoreOld;
//   - anything else -> Other.
//
// The marker alone cannot tell v0.1.2 from Apple, because v0.1.2 has
// none. The row can: Apple's entry point is 0x736000 and its image is
// 7.6 MB, and ours has always been 0 and a few hundred KB.
//
// A nil body is not special-cased: it simply has no marker, so a caller
// that could not read the body gets the row's answer.
func Classify(e firmware.DirectoryEntry, body []byte) Installed {
	if v, build, ok := FindVersion(body); ok {
		return Installed{
			Kind:        Core,
			Version:     v,
			BuildID:     build,
			Description: fmt.Sprintf("Core %s (build %s)", v, build),
		}
	}
	if e.EntryOffset == 0 && e.Length < CoreMaxImage {
		return Installed{
			Kind: CoreOld,
			Description: fmt.Sprintf(
				"Core before v0.1.3 (%d bytes, entry 0x0, no version marker)", e.Length),
		}
	}
	if e.EntryOffset != 0 && e.Length >= CoreMaxImage {
		// The shape every stock iPod has. It is named rather than
		// described because "Apple firmware" is what the person holding
		// the device calls it.
		return Installed{
			Kind: Other,
			Description: fmt.Sprintf("Apple firmware (%s, entry %#x)",
				megabytes(e.Length), e.EntryOffset),
		}
	}
	return Installed{
		Kind: Other,
		Description: fmt.Sprintf("unknown firmware (%d bytes, entry %#x)",
			e.Length, e.EntryOffset),
	}
}

func megabytes(n uint32) string { return fmt.Sprintf("%.1f MB", float64(n)/1e6) }

// ClassifyEntry reads e's body off the partition and classifies it.
//
// A body that cannot be read is Other, not an error: the length in a
// damaged row can point past the partition, and "this is not Core" is
// both true and the answer that leaves `core install` able to fix it.
func ClassifyEntry(p Partition, e firmware.DirectoryEntry) Installed {
	body, err := ReadBody(p, e)
	if err != nil {
		return Installed{
			Kind: Other,
			Description: fmt.Sprintf("unreadable OSOS image (%d bytes at %#x: %v)",
				e.Length, BodyOffset(e), err),
		}
	}
	return Classify(e, body)
}

// ClassifyPartition is the whole question asked of a device: parse the
// directory, find OSOS, read it, classify. The error is only for a
// partition with no directory or no OSOS — anything that parses gets an
// answer.
func ClassifyPartition(p Partition) (Installed, firmware.DirectoryEntry, error) {
	var none firmware.DirectoryEntry
	d, err := Parse(p)
	if err != nil {
		return Installed{}, none, err
	}
	_, osos, ok := d.OSOS()
	if !ok {
		return Installed{}, none, ErrNoOSOS
	}
	return ClassifyEntry(p, osos), osos, nil
}
