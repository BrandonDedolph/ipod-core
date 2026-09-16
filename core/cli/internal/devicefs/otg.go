// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// The On-The-Go playlist needs two pre-allocated things on the volume, and the
// firmware can create neither — it can only overwrite the data sectors of a
// file that already exists.
//
//	<volume root>/COREOTG.DAT                    the LIVE list
//	<volume root>/Music/Playlists/On-The-Go N.m3u8   the five SAVED lists
//
// COREOTG.DAT — two 5120-byte slots the device alternates between, then zero
// padding out to OTGFileBytes so the whole thing is one cluster on a stock
// 80 GB volume. Mirrors core/kernel/otg_store.c and tools/make_otg.py:
//
//	off    size  field
//	0      4     magic     'C''O''T''G'  (0x47544F43 LE)
//	4      2     version   1
//	6      2     count     entries that follow, 0..512
//	8      4     seq       monotonic; the newer slot wins
//	12     2     gen       the list's mutation counter when it was saved
//	14     2     flags     reserved, 0
//	16     4096  entries   count * { u32 folder_hash, u32 file_hash }
//	4112   4     reserved  0
//	5116   4     crc32     zlib CRC-32 over bytes [0, 5116)
//
// The CRC covers the header, not just the entries: seq is what decides which
// slot wins, so a corrupt seq under a valid payload CRC would let a stale slot
// beat a good one.
const (
	OTGName      = "COREOTG.DAT"
	OTGSlotBytes = 5120
	OTGSlots     = 2
	OTGMinBytes  = OTGSlotBytes * OTGSlots // 10240 — the smallest the device takes
	OTGFileBytes = 32 * 1024               // what the host creates

	OTGMagic   = 0x47544F43 // 'C''O''T''G' LE
	OTGVersion = 1
	OTGMax     = 512 // OTG_MAX, core/library/otg.h

	otgOffMagic    = 0
	otgOffVersion  = 4
	otgOffCount    = 6
	otgOffSeq      = 8
	otgOffGen      = 12
	otgOffFlags    = 14
	otgOffEntries  = 16
	otgEntryBytes  = 8
	otgOffReserved = otgOffEntries + OTGMax*otgEntryBytes // 4112
	otgOffCRC      = OTGSlotBytes - 4                     // 5116
)

// The five saved lists. Ordinary extended M3U8 files carrying two extra
// comment lines, so every other reader — the firmware's own, a desktop
// player, `core sync --prune` — treats them as the playlists they are.
//
//	#EXTM3U\n                                          8 B
//	#CORE-OTG v1 count=00012 crc=1A2B3C4D gen=00042\n  48 B
//	/Music/Artist - Album/03. Title.flac\n        <- count lines
//	#CORE-OTG-END gen=00042\n                         24 B
//	\n\n\n...                            <- to the last byte of the file
//
// gen appears TWICE, and that repetition is the whole tear-detection scheme:
// the firmware writes the first 4096 bytes LAST, so a power cut leaves a
// header whose gen disagrees with the trailer's, or a line count that
// disagrees with the header's count. See core/library/otg_slot.h.
const (
	OTGPlaylistSlots = 5
	// OTGSlotFileBytes is 128 KiB, four stock clusters. The worst case is
	// 512 lines of '/' + M3U_PATH_MAX + '\n' plus the two directives =
	// 98896 bytes, so 96 KiB would be 592 bytes short of holding a full
	// list whole.
	OTGSlotFileBytes = 128 * 1024
	OTGSlotFileMin   = 4096
	OTGSlotSizeGrain = 1024

	OTGDirective   = "#CORE-OTG v1 "
	OTGTrailer     = "#CORE-OTG-END "
	otgHdrBytes    = 48
	otgEndBytes    = 24
	otgM3UTag      = "#EXTM3U\n"
	otgHdrTemplate = "#CORE-OTG v1 count=%05d crc=%08X gen=%05d\n"
	otgEndTemplate = "#CORE-OTG-END gen=%05d\n"
)

// OTGSlotName is the file name of saved list n (1..OTGPlaylistSlots).
func OTGSlotName(n int) string {
	return fmt.Sprintf("On-The-Go %d.m3u8", n)
}

// OTGSlotIndex is otg_slot_index() (core/library/otg_slot.c) over a FILE
// name: 1..OTGPlaylistSlots when name is exactly "On-The-Go N" with one of
// the playlist extensions, 0 otherwise. Case-insensitive, because the volume
// is.
//
// This is what keeps a user's source playlist from being planned onto a
// device slot, where the firmware would treat their file as a saved list.
func OTGSlotIndex(name string) int {
	ext := strings.ToLower(filepath.Ext(name))
	if ext != ".m3u8" && ext != ".m3u" {
		return 0
	}
	stem := name[:len(name)-len(ext)]
	const prefix = "on-the-go "
	if len(stem) != len(prefix)+1 || !strings.HasPrefix(strings.ToLower(stem), prefix) {
		return 0
	}
	n, err := strconv.Atoi(stem[len(prefix):])
	if err != nil || n < 1 || n > OTGPlaylistSlots {
		return 0
	}
	return n
}

// OTGEntry is one live-list entry: the folded hashes of the FULL on-disk
// names of the album folder and of the file inside it. The same pair
// CORELIB.IDX binds a record to its file by.
type OTGEntry struct {
	FolderHash uint32
	FileHash   uint32
}

// EncodeOTGSlot builds one 5120-byte slot. Every byte the entries do not
// cover is zero and the CRC covers the whole slot, so the same list always
// encodes to the same bytes — which is what makes the Go, Python and C
// encoders comparable byte for byte.
func EncodeOTGSlot(entries []OTGEntry, seq uint32, gen uint16) ([OTGSlotBytes]byte, error) {
	var rec [OTGSlotBytes]byte
	if len(entries) > OTGMax {
		return rec, fmt.Errorf("devicefs: %d On-The-Go entries; the format holds %d",
			len(entries), OTGMax)
	}
	binary.LittleEndian.PutUint32(rec[otgOffMagic:], OTGMagic)
	binary.LittleEndian.PutUint16(rec[otgOffVersion:], OTGVersion)
	binary.LittleEndian.PutUint16(rec[otgOffCount:], uint16(len(entries)))
	binary.LittleEndian.PutUint32(rec[otgOffSeq:], seq)
	binary.LittleEndian.PutUint16(rec[otgOffGen:], gen)
	binary.LittleEndian.PutUint16(rec[otgOffFlags:], 0)
	for i, e := range entries {
		off := otgOffEntries + i*otgEntryBytes
		binary.LittleEndian.PutUint32(rec[off:], e.FolderHash)
		binary.LittleEndian.PutUint32(rec[off+4:], e.FileHash)
	}
	binary.LittleEndian.PutUint32(rec[otgOffCRC:], crc32.ChecksumIEEE(rec[:otgOffCRC]))
	return rec, nil
}

// DecodeOTGSlot validates and decodes one slot, exactly as
// otg_slot_decode() does: magic, version (0 is not a version, newer than we
// understand is declined), count against the ceiling, then the CRC.
//
// A (0, 0) pair INSIDE the declared count is dropped, as the firmware drops
// it — that pair is what the padding is made of, so a hand-edited file cannot
// inject an entry that binds to nothing.
func DecodeOTGSlot(b []byte) (seq uint32, gen uint16, entries []OTGEntry, ok bool) {
	if len(b) < OTGSlotBytes {
		return 0, 0, nil, false
	}
	b = b[:OTGSlotBytes]
	if binary.LittleEndian.Uint32(b[otgOffMagic:]) != OTGMagic {
		return 0, 0, nil, false
	}
	ver := binary.LittleEndian.Uint16(b[otgOffVersion:])
	if ver == 0 || ver > OTGVersion {
		return 0, 0, nil, false
	}
	count := int(binary.LittleEndian.Uint16(b[otgOffCount:]))
	if count > OTGMax {
		return 0, 0, nil, false
	}
	if crc32.ChecksumIEEE(b[:otgOffCRC]) != binary.LittleEndian.Uint32(b[otgOffCRC:]) {
		return 0, 0, nil, false
	}
	for i := 0; i < count; i++ {
		off := otgOffEntries + i*otgEntryBytes
		e := OTGEntry{
			FolderHash: binary.LittleEndian.Uint32(b[off:]),
			FileHash:   binary.LittleEndian.Uint32(b[off+4:]),
		}
		if e.FolderHash == 0 && e.FileHash == 0 {
			continue
		}
		entries = append(entries, e)
	}
	return binary.LittleEndian.Uint32(b[otgOffSeq:]),
		binary.LittleEndian.Uint16(b[otgOffGen:]), entries, true
}

// OTGSeqNewer is the firmware's wrapping comparison (config_seq_newer's
// rule): the distance from b to a read as signed, so 0xFFFFFFFF -> 0 is +1
// and not a four-billion step backwards.
func OTGSeqNewer(a, b uint32) bool { return int32(a-b) > 0 }

// OTGFileValid reports whether the head of a COREOTG.DAT (at least
// OTGMinBytes of it) holds a slot the firmware would load, and the sequence
// of the newest such slot. It is the guard that makes EnsureOTG idempotent: a
// re-sync must never silently throw away the list the user built.
func OTGFileValid(head []byte) (seq uint32, ok bool) {
	// otg_store_mount() refuses any file shorter than OTG_STORE_MIN_BYTES
	// before it looks at a slot, so a 5120-byte file with a perfect slot 0
	// is still one the device will not load — and a fresh one must replace it.
	if len(head) < OTGMinBytes {
		return 0, false
	}
	for i := 0; i < OTGSlots; i++ {
		off := i * OTGSlotBytes
		if off+OTGSlotBytes > len(head) {
			break
		}
		if s, _, _, valid := DecodeOTGSlot(head[off : off+OTGSlotBytes]); valid {
			if !ok || OTGSeqNewer(s, seq) {
				seq, ok = s, true
			}
		}
	}
	return seq, ok
}

// EnsureOTG makes sure volumeRoot holds a COREOTG.DAT the firmware will load.
// An existing file whose slot 0 or slot 1 decodes is left exactly as it is
// (created == false); anything else — absent, short, or holding no valid slot
// — is written fresh: OTGFileBytes of zeros with slot 0 holding an EMPTY list
// at seq 1 and slot 1 left zero, so the device's first save lands in slot 1
// and the two-slot alternation starts cleanly.
//
// EnsureConfig's shape, and for the same reason: this package never resets
// something the user made.
func EnsureOTG(volumeRoot string) (created bool, err error) {
	if err := RefusesMount(volumeRoot); err != nil {
		return false, err
	}
	path := filepath.Join(volumeRoot, OTGName)

	switch head, err := readHead(path, OTGMinBytes); {
	case err == nil:
		if _, ok := OTGFileValid(head); ok {
			return false, nil
		}
	case errors.Is(err, os.ErrNotExist):
		// fall through and create it
	default:
		return false, err
	}

	blob := make([]byte, OTGFileBytes)
	slot, err := EncodeOTGSlot(nil, 1, 0)
	if err != nil {
		return false, err
	}
	copy(blob, slot[:])
	if err := writeFileThrough(path, blob); err != nil {
		return false, err
	}
	return true, nil
}

// EmptyOTGSlotFile is the whole contents of an unused slot playlist at
// `size`: the two directive lines and newlines to the last byte. Exactly what
// otg_slot_save() writes for an empty list, so the device can read back what
// the host created without a special case.
func EmptyOTGSlotFile(size int) ([]byte, error) {
	return otgSlotFile(size, nil, 0)
}

func otgSlotFile(size int, entries []string, gen uint16) ([]byte, error) {
	if size < OTGSlotFileMin || size%OTGSlotSizeGrain != 0 {
		return nil, fmt.Errorf("devicefs: On-The-Go slot size %d must be at least %d and a multiple of %d",
			size, OTGSlotFileMin, OTGSlotSizeGrain)
	}
	var body strings.Builder
	for _, e := range entries {
		body.WriteString(e)
		body.WriteByte('\n')
	}
	var out strings.Builder
	out.WriteString(otgM3UTag)
	fmt.Fprintf(&out, otgHdrTemplate, len(entries),
		crc32.ChecksumIEEE([]byte(body.String())), gen)
	out.WriteString(body.String())
	fmt.Fprintf(&out, otgEndTemplate, gen)
	if out.Len() > size {
		return nil, fmt.Errorf("devicefs: %d On-The-Go entries do not fit in %d bytes",
			len(entries), size)
	}
	b := make([]byte, size)
	copy(b, out.String())
	for i := out.Len(); i < size; i++ {
		b[i] = '\n'
	}
	return b, nil
}

// OTGSlotState is what a file sitting at a slot name actually is.
type OTGSlotState string

// The four things a slot path can be. "foreign" is a playlist of the user's
// own that happens to use the name: the device lists and plays it and never
// writes to it, and neither do we.
const (
	OTGSlotAbsent  OTGSlotState = "absent"
	OTGSlotEmpty   OTGSlotState = "empty"
	OTGSlotUsed    OTGSlotState = "used"
	OTGSlotDamaged OTGSlotState = "damaged"
	OTGSlotForeign OTGSlotState = "foreign"
)

// OTGSlotInfo is what a slot file says about itself.
type OTGSlotInfo struct {
	State OTGSlotState
	Count int    // entry lines the header claims
	Gen   uint16 // the header's gen
	CRC   uint32 // the header's CRC of the entry bytes
	// TrailerGen is the gen the trailer carries, and -1 when there is no
	// trailer. A mismatch with Gen is a torn save.
	TrailerGen int
	// Lines is how many entry lines are actually in the file.
	Lines int
	Size  int64
}

// ParseOTGSlotHeader reads the fixed-width directive out of the head of a
// slot file (the firmware reads the first 512 bytes and nothing more, and so
// does this). ok is false for a file with no directive — a foreign playlist.
//
// Parsed by OFFSET, not by a scan verb, because the format is fixed width and
// the firmware parses it that way (core/library/otg_slot.c): the device
// patches count and crc into a header it has already written, which is only
// sound while no field can change length. Anything of another shape is
// somebody else's comment line.
func ParseOTGSlotHeader(head []byte) (count int, crc uint32, gen uint16, ok bool) {
	for _, line := range strings.SplitAfter(string(stripBOM(head)), "\n") {
		if len(line) != otgHdrBytes || !strings.HasPrefix(line, OTGDirective) {
			continue
		}
		if line[13:19] != "count=" || line[24] != ' ' || line[25:29] != "crc=" ||
			line[37] != ' ' || line[38:42] != "gen=" || line[47] != '\n' {
			continue
		}
		c, err1 := parseDecN(line[19:24])
		x, err2 := strconv.ParseUint(line[29:37], 16, 32)
		g, err3 := parseDecN(line[42:47])
		if err1 != nil || err2 != nil || err3 != nil {
			continue
		}
		if !isHexDigits(line[29:37]) || c > OTGMax || g > 0xFFFF {
			continue
		}
		return int(c), uint32(x), uint16(g), true
	}
	return 0, 0, 0, false
}

// parseOTGTrailerGen is the same for the fixed-width trailer: the gen it
// carries, or -1 when the line is not one.
func parseOTGTrailerGen(line string) int {
	if len(line) != otgEndBytes || !strings.HasPrefix(line, OTGTrailer) ||
		line[14:18] != "gen=" || line[23] != '\n' {
		return -1
	}
	g, err := parseDecN(line[18:23])
	if err != nil || g > 0xFFFF {
		return -1
	}
	return int(g)
}

// parseDecN accepts EXACTLY the digits it is given and nothing else — no
// sign, no space, no shorter run. strconv.ParseUint would take "+1234".
func parseDecN(s string) (uint64, error) {
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, fmt.Errorf("not a decimal digit: %q", s[i])
		}
	}
	return strconv.ParseUint(s, 10, 32)
}

func isHexDigits(s string) bool {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F') && !(c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

// stripBOM trims a UTF-8 BOM, which fs/m3u.c skips at offset 0.
func stripBOM(b []byte) []byte {
	if len(b) >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF {
		return b[3:]
	}
	return b
}

// OTGSlotFileState classifies the file at `path`, applying the firmware's own
// tests: the header has to be there (or it is foreign), and the trailer has
// to be there carrying the header's gen with the header's count of entry
// lines (or it is damaged).
func OTGSlotFileState(path string) (OTGSlotInfo, error) {
	info := OTGSlotInfo{State: OTGSlotAbsent, TrailerGen: -1}
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return info, nil
	}
	if err != nil {
		return info, err
	}
	info.Size = int64(len(b))
	count, crc, gen, ok := ParseOTGSlotHeader(headOf(b))
	if !ok {
		info.State = OTGSlotForeign
		return info, nil
	}
	info.Count, info.CRC, info.Gen = count, crc, gen

	for _, raw := range strings.Split(string(stripBOM(b)), "\n") {
		line := strings.TrimSuffix(raw, "\r")
		switch {
		case line == "":
		case strings.HasPrefix(line, OTGTrailer):
			if g := parseOTGTrailerGen(line + "\n"); g >= 0 {
				info.TrailerGen = g
			}
		case strings.HasPrefix(line, "#"):
		default:
			info.Lines++
		}
	}
	switch {
	case info.TrailerGen != int(gen) || info.Lines != count:
		info.State = OTGSlotDamaged
	case count == 0:
		info.State = OTGSlotEmpty
	default:
		info.State = OTGSlotUsed
	}
	return info, nil
}

// headOf is the first 512 bytes, which is all otg_slot_probe() reads.
func headOf(b []byte) []byte {
	if len(b) > 512 {
		return b[:512]
	}
	return b
}

// ReadOTGSlotFile is the pull-back primitive: the tracks a saved On-The-Go
// list names, with the two directives and the padding stripped. It is what a
// host command that copies a saved list off the device needs, and it doubles
// as the read-back half of the round trip the tests assert.
//
// A slot the firmware would call damaged is returned WITH its entries and an
// error, not silently: "the file is torn" and "the file is empty" must not
// look the same. A foreign playlist returns its lines with no error — it is
// an ordinary playlist and reading it is the ordinary thing to do.
func ReadOTGSlotFile(path string) (entries []string, info OTGSlotInfo, err error) {
	info, err = OTGSlotFileState(path)
	if err != nil {
		return nil, info, err
	}
	if info.State == OTGSlotAbsent {
		return nil, info, os.ErrNotExist
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, info, err
	}
	for _, raw := range strings.Split(string(stripBOM(b)), "\n") {
		line := strings.TrimSuffix(raw, "\r")
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		entries = append(entries, line)
	}
	if info.State == OTGSlotDamaged {
		return entries, info, fmt.Errorf("%s: torn save (header gen %d, trailer gen %d, %d entry line(s) against a count of %d)",
			filepath.Base(path), info.Gen, info.TrailerGen, info.Lines, info.Count)
	}
	if info.State == OTGSlotUsed {
		var body strings.Builder
		for _, e := range entries {
			body.WriteString(e)
			body.WriteByte('\n')
		}
		if got := crc32.ChecksumIEEE([]byte(body.String())); got != info.CRC {
			// The device does not check this — gen and count are its test —
			// so it is not "damaged". It does mean something rewrote the
			// lines without updating the header, which is worth saying.
			return entries, info, fmt.Errorf("%s: the header's CRC is %08X and the entry lines hash to %08X",
				filepath.Base(path), info.CRC, got)
		}
	}
	return entries, info, nil
}

// EnsureOTGSlots creates the five slot playlists under musicDir/Playlists,
// ONLY where one is absent, and returns the file names it created.
//
// An existing file is never touched, whatever it holds: it may be a list the
// user saved, or it may be their own playlist that happens to use the name.
// That is stricter than EnsureConfig / EnsureLog, which rewrite a file the
// device would refuse — here there is nothing the device refuses, because a
// file with no directive is simply an ordinary playlist.
func EnsureOTGSlots(musicDir string) (created []string, err error) {
	dir := filepath.Join(musicDir, PlaylistDir)
	if err := os.MkdirAll(dir, 0o777); err != nil {
		return nil, err
	}
	blob, err := EmptyOTGSlotFile(OTGSlotFileBytes)
	if err != nil {
		return nil, err
	}
	for n := 1; n <= OTGPlaylistSlots; n++ {
		name := OTGSlotName(n)
		path := filepath.Join(dir, name)
		switch _, err := os.Stat(path); {
		case err == nil:
			continue
		case errors.Is(err, os.ErrNotExist):
		default:
			return created, err
		}
		if err := writeFileThrough(path, blob); err != nil {
			return created, err
		}
		created = append(created, name)
	}
	return created, nil
}
