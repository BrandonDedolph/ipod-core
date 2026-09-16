package disk

import (
	"errors"
	"fmt"
	"strings"
	"unicode"
)

// The iPod's name is the FAT32 volume label of its music partition —
// what Windows shows beside `D:`, and the only name on the device that
// a host and the firmware could ever agree on. This file is the
// portable half: the rule that turns what a person types into a label
// FAT can hold. The two Windows calls that read and write it live in
// label_windows.go.

// MaxLabel is the length of a FAT volume label in bytes. The label is
// stored in the root directory's volume entry (and mirrored in the boot
// sector's BS_VolLab) as a fixed 11-byte field, space-padded, with no
// terminator and no length — so 11 is not a limit that can be raised by
// asking nicely.
const MaxLabel = 11

// labelPunct is the punctuation a FAT label may contain, beside A–Z,
// 0–9 and the space.
//
// Everything outside it is rejected by Windows itself (`* ? . , ; : / \
// | + = < > [ ] "` are the reserved ones) or is non-ASCII, which is
// worse than rejected: a byte above 0x7F is interpreted through the
// OEM code page of whoever is reading the volume, so `Musique été` on
// this machine is `Musique Ã©tÃ©` on the next one. The rule drops those
// rather than writing a name that means something different on every
// host.
const labelPunct = "!#$%&'()-@^_`{}~"

// ErrLabelEmpty is what ValidLabel says about a label with nothing in
// it. It is not always a failure: FAT spells "this volume has no name"
// as eleven spaces, and clearing the name is a thing a user may ask
// for. It is an error only where a label was expected.
var ErrLabelEmpty = errors.New("disk: the label is empty (FAT has no name for this volume)")

// LegalLabel turns a friendly name into the label FAT can actually
// store. It is pure, total, and the only place the rule is written.
//
// In order: every rune is upper-cased; letters A–Z and digits are kept;
// the punctuation in labelPunct is kept; a run of spaces becomes one
// space; everything else — accents, CJK, emoji, tabs, `.` and the other
// reserved characters — is dropped; the result is trimmed and cut to
// MaxLabel bytes. By the time the cut happens the string is ASCII, so a
// byte cut is a character cut and cannot split a rune.
//
// Worked examples (these are the test table):
//
//	"Brandon's iPod"  -> "BRANDON'S I"   (cut at 11: the apostrophe is legal)
//	"Musique été"     -> "MUSIQUE T"     (the accented runes are dropped)
//	"my  ipod"        -> "MY IPOD"       (the double space collapses)
//	"***"             -> ""              (nothing legal survives; ValidLabel refuses it)
//	""                -> ""
//
// Dropping rather than transliterating is deliberate: `é` -> `E` is a
// guess about a language, and a name the user did not type is worse
// than a shorter one they did.
func LegalLabel(friendly string) string {
	var b strings.Builder
	b.Grow(len(friendly))
	lastSpace := true // leading spaces collapse away entirely
	for _, r := range friendly {
		r = unicode.ToUpper(r)
		switch {
		case r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
			b.WriteRune(r)
			lastSpace = false
		case r == ' ':
			if !lastSpace {
				b.WriteByte(' ')
				lastSpace = true
			}
		case r < 0x80 && strings.ContainsRune(labelPunct, r):
			b.WriteRune(r)
			lastSpace = false
		}
	}
	out := strings.TrimRight(b.String(), " ")
	if len(out) > MaxLabel {
		out = out[:MaxLabel]
	}
	return strings.TrimRight(out, " ")
}

// ValidLabel reports whether a string is already a label FAT can store:
// at most MaxLabel bytes, no character outside the allowed set, and not
// empty. LegalLabel's output always passes it unless it is empty, which
// is the case a caller has to decide about — see ErrLabelEmpty.
func ValidLabel(label string) error {
	if strings.TrimSpace(label) == "" {
		return ErrLabelEmpty
	}
	if len(label) > MaxLabel {
		return fmt.Errorf("disk: %q is %d bytes; a FAT label holds %d",
			label, len(label), MaxLabel)
	}
	for i := 0; i < len(label); i++ {
		c := label[i]
		switch {
		case c >= 'A' && c <= 'Z', c >= '0' && c <= '9', c == ' ':
		case c < 0x80 && strings.IndexByte(labelPunct, c) >= 0:
		default:
			return fmt.Errorf("disk: %q cannot be a FAT label: %q is not allowed "+
				"(A-Z, 0-9, space and %s are)", label, string(rune(c)), labelPunct)
		}
	}
	return nil
}

// VolumeRoot normalises what a user types for a volume into what the
// Windows volume calls demand: a path that ends in a separator.
// `GetVolumeInformationW("D:")` does not mean drive D — it means the
// current directory ON drive D, which is a different thing and usually
// an error. Everywhere else the string is handed back unchanged.
func VolumeRoot(path string) string {
	if path == "" {
		return ""
	}
	if len(path) == 2 && path[1] == ':' {
		return path + `\`
	}
	if strings.HasSuffix(path, `\`) || strings.HasSuffix(path, "/") {
		return path
	}
	if len(path) >= 2 && path[1] == ':' {
		return path + `\`
	}
	return path
}
