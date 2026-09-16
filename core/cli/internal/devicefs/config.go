// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
)

// CORECFG.DAT — the settings file. Two 1024-byte slots the device alternates
// between, then zero padding out to ConfigFileBytes so the whole thing is one
// cluster on a stock 80 GB volume.
//
// Record layout (little-endian), mirroring core/kernel/config.c:126-152 and
// tools/make_config.py:70-136:
//
//	off   size  field
//	0     4     magic     'C''O''R''E'  (0x45524F43 LE)
//	4     2     version   2
//	6     2     length    meaningful payload bytes that follow (48)
//	8     4     seq       monotonic; the higher slot wins
//	12    n     payload   see below
//	1020  4     crc32     zlib CRC-32 over bytes [0, 1020)
//
// `length`, not `version`, says how much payload to read: version gates
// whether the record is understood at all, length gates which fields are
// present inside one that is (core/kernel/config.c:136-146). That is what
// keeps a v1 record (length 12) readable by this build.
const (
	ConfigSlotBytes = 1024
	ConfigSlots     = 2
	ConfigMinBytes  = ConfigSlotBytes * ConfigSlots // 2048 — the smallest file the device accepts
	ConfigFileBytes = 32 * 1024                     // what the host creates

	ConfigMagic   = 0x45524F43 // 'C''O''R''E' LE
	ConfigVersion = 2

	cfgOffMagic   = 0
	cfgOffVersion = 4
	cfgOffLength  = 6
	cfgOffSeq     = 8
	cfgOffPayload = 12
	cfgOffCRC     = ConfigSlotBytes - 4       // 1020
	cfgPayloadMax = cfgOffCRC - cfgOffPayload // 1008
	cfgPayloadV1  = 12                        // settings only
	cfgPayloadV2  = cfgPayloadV1 + 12         // + resume locator = 24
	cfgPayloadV2Q = cfgPayloadV2 + 20         // + queue context = 44
	cfgPayloadLen = cfgPayloadV2Q + 4         // + sound tail = 48
)

// Payload v1 field offsets — one byte each, in this order. Mirrors the P_*
// enum in core/kernel/config.c:161-163; order and width are the on-disk
// contract.
const (
	pShuffle = iota
	pRepeat
	pResume
	pCrossfade
	pVolume
	pBass
	pTreble
	pBalance
	pBLSecs
	pBLBright
	pTheme
	pClicker
)

// The SOUND TAIL, appended after the queue context under the same record
// version (length 44 -> 48). Mirrors P_VOL_LIMIT.. in core/kernel/config.c.
const (
	pVolumeLimit = cfgPayloadV2Q + 0
	pEQ          = cfgPayloadV2Q + 1
)

// Settings is the part of the firmware's settings_t that lives in the v1
// payload — everything the host has any business writing.
//
// The resume locator (payload offset 12) and queue context (24) are written
// as zeros by the host and are not surfaced here: "where the user was" is the
// device's to own, and a fresh file has nothing to resume
// (tools/make_config.py). The sound tail at 44 IS a user preference, so it is
// modelled like the rest.
type Settings struct {
	Shuffle         uint8 // 0/1
	Repeat          uint8 // 0=off 1=all 2=one
	ResumeOnStartup uint8 // 0/1
	Crossfade       uint8 // 0/1
	Volume          uint8 // 0..100
	Bass            int8  // -12..12
	Treble          int8  // -12..12
	Balance         int8  // -100..100
	BacklightSecs   uint8
	BacklightBright uint8 // 1..32
	Theme           uint8
	Clicker         uint8 // 0..3

	// The sound tail. VolumeLimit is 10..100 with 100 meaning "no limit";
	// the firmware reads a 0 byte as UNSET and uses 100, so a file whose
	// new bytes were never written cannot pin the user at the 10% floor.
	// EQ is an index into core/ui/eq.c's preset table, 0 = Off.
	VolumeLimit uint8
	EQ          uint8
}

// DefaultSettings is settings_defaults() in core/ui/settings.c:157-183.
//
// These are not "the host tool's defaults". The record EnsureConfig writes is
// the one the firmware loads forever after, and a valid record on disk always
// beats settings_defaults() — so a wrong value here is not a preference, it is
// a setting the user can never have. ResumeOnStartup = 0 once silently
// disabled Resume on a device whose CORECFG.DAT this path created, with every
// other setting persisting correctly (diagnosed on hardware 2026-07-27; see
// tools/make_config.py:89-95). settings_defaults_test.go greps the C.
func DefaultSettings() Settings {
	return Settings{
		Shuffle:         0,
		Repeat:          0, // REPEAT_OFF — core/ui/settings.h:39
		ResumeOnStartup: 1,
		Crossfade:       0,
		Volume:          70,
		Bass:            0,
		Treble:          0,
		Balance:         0,
		BacklightSecs:   15,
		BacklightBright: 32,
		Theme:           0,
		Clicker:         1,
		VolumeLimit:     100, // no limit
		EQ:              0,   // EQ_OFF
	}
}

// EncodeConfigSlot builds one 1024-byte slot: a v2 record (length 48) holding
// s, sequence seq, a zero resume locator and a zero queue context.
//
// Field values are written as given. The firmware clamps every field on
// decode (core/kernel/config.c:385-401), so an out-of-range value costs the
// setting, never the record — and writing the caller's bytes verbatim is what
// keeps this byte-identical to tools/make_config.py's encode().
func EncodeConfigSlot(s Settings, seq uint32) [ConfigSlotBytes]byte {
	var rec [ConfigSlotBytes]byte

	binary.LittleEndian.PutUint32(rec[cfgOffMagic:], ConfigMagic)
	binary.LittleEndian.PutUint16(rec[cfgOffVersion:], ConfigVersion)
	binary.LittleEndian.PutUint16(rec[cfgOffLength:], cfgPayloadLen)
	binary.LittleEndian.PutUint32(rec[cfgOffSeq:], seq)

	p := rec[cfgOffPayload:]
	p[pShuffle] = s.Shuffle
	p[pRepeat] = s.Repeat
	p[pResume] = s.ResumeOnStartup
	p[pCrossfade] = s.Crossfade
	p[pVolume] = s.Volume
	p[pBass] = byte(s.Bass)
	p[pTreble] = byte(s.Treble)
	p[pBalance] = byte(s.Balance)
	p[pBLSecs] = s.BacklightSecs
	p[pBLBright] = s.BacklightBright
	p[pTheme] = s.Theme
	p[pClicker] = s.Clicker
	// p[12..44) — resume locator and queue context — stay zero.
	p[pVolumeLimit] = s.VolumeLimit
	p[pEQ] = s.EQ
	// p[46..48) — the sound tail's reserved half-word — stays zero.

	binary.LittleEndian.PutUint32(rec[cfgOffCRC:], crc32.ChecksumIEEE(rec[:cfgOffCRC]))
	return rec
}

// DecodeConfigSlot validates and decodes one slot, exactly as
// config_decode() does (core/kernel/config.c:354-449): magic, then version
// (0 is not a version, newer than we understand is declined), then the
// record's own length against [12, 1008], then the CRC over [0, 1020).
//
// The payload is read gated on that length, not on the version — so a v1
// record (length 12) still decodes here, with the v2 tail treated as absent
// rather than read out of the zero padding. The v2 tail carries only the
// resume locator and queue context, which Settings does not model, so an
// absent tail is simply nothing to read.
//
// Unlike the firmware, the returned Settings are NOT clamped: the host's job
// is to report what is on the disk, and clamping would hide a record the
// device will read differently.
func DecodeConfigSlot(b []byte) (seq uint32, s Settings, ok bool) {
	if len(b) < ConfigSlotBytes {
		return 0, Settings{}, false
	}
	b = b[:ConfigSlotBytes]

	if binary.LittleEndian.Uint32(b[cfgOffMagic:]) != ConfigMagic {
		return 0, Settings{}, false
	}
	ver := binary.LittleEndian.Uint16(b[cfgOffVersion:])
	if ver == 0 || ver > ConfigVersion {
		return 0, Settings{}, false
	}
	length := int(binary.LittleEndian.Uint16(b[cfgOffLength:]))
	if length < cfgPayloadV1 || length > cfgPayloadMax {
		return 0, Settings{}, false
	}
	if crc32.ChecksumIEEE(b[:cfgOffCRC]) != binary.LittleEndian.Uint32(b[cfgOffCRC:]) {
		return 0, Settings{}, false
	}

	p := b[cfgOffPayload:]
	s = Settings{
		Shuffle:         p[pShuffle],
		Repeat:          p[pRepeat],
		ResumeOnStartup: p[pResume],
		Crossfade:       p[pCrossfade],
		Volume:          p[pVolume],
		Bass:            int8(p[pBass]),
		Treble:          int8(p[pTreble]),
		Balance:         int8(p[pBalance]),
		BacklightSecs:   p[pBLSecs],
		BacklightBright: p[pBLBright],
		Theme:           p[pTheme],
		Clicker:         p[pClicker],
	}
	// length >= cfgPayloadV2 would carry the resume locator and, at
	// length >= cfgPayloadV2Q, the queue context. Neither is part of
	// Settings; the gate is preserved above so validity matches the device.
	//
	// The sound tail IS part of Settings, and it is gated the same way: a
	// 44-byte record (what every device in the field holds) reports what the
	// firmware will use for it, no limit and EQ off. Present bytes are
	// reported verbatim, like every other field — including a 0 limit, which
	// the firmware reads as unset.
	if length >= cfgPayloadLen {
		s.VolumeLimit = p[pVolumeLimit]
		s.EQ = p[pEQ]
	} else {
		s.VolumeLimit = 100
		s.EQ = 0
	}
	return binary.LittleEndian.Uint32(b[cfgOffSeq:]), s, true
}

// ConfigFileValid reports whether the head of a CORECFG.DAT (at least
// ConfigMinBytes of it) holds a record the firmware would load, and the
// sequence of the newest such slot. It is
// the guard that makes EnsureConfig idempotent: a re-import must never
// silently reset settings the user has saved.
func ConfigFileValid(head []byte) (seq uint32, ok bool) {
	// config_mount() refuses any file shorter than CONFIG_MIN_BYTES
	// (core/kernel/config.c:479, 641) before it looks at a slot, so a
	// 1024-byte file with a perfect slot 0 is still a file the device will
	// not load — and a fresh one must replace it.
	if len(head) < ConfigMinBytes {
		return 0, false
	}
	for i := 0; i < ConfigSlots; i++ {
		off := i * ConfigSlotBytes
		if off+ConfigSlotBytes > len(head) {
			break
		}
		if s, _, valid := DecodeConfigSlot(head[off : off+ConfigSlotBytes]); valid {
			if !ok || s > seq {
				seq, ok = s, true
			}
		}
	}
	return seq, ok
}

// EnsureConfig makes sure volumeRoot holds a CORECFG.DAT the firmware will
// load. An existing file whose slot 0 or slot 1 decodes is left exactly as it
// is (created == false); anything else — absent, short, or holding no valid
// record — is written fresh: ConfigFileBytes of zeros with slot 0 holding a
// DefaultSettings record at seq 1 and slot 1 left zero, so the device's first
// save lands in slot 1 and the two-slot alternation starts cleanly.
//
// Mirrors tools/make_config.py's do_create (tools/make_config.py:201-251),
// minus --force: this package never resets a user's settings.
func EnsureConfig(volumeRoot string) (created bool, err error) {
	if err := RefusesMount(volumeRoot); err != nil {
		return false, err
	}
	path := filepath.Join(volumeRoot, ConfigName)

	switch head, err := readHead(path, ConfigMinBytes); {
	case err == nil:
		if _, ok := ConfigFileValid(head); ok {
			return false, nil
		}
	case errors.Is(err, os.ErrNotExist):
		// fall through and create it
	default:
		return false, err
	}

	blob := make([]byte, ConfigFileBytes)
	slot := EncodeConfigSlot(DefaultSettings(), 1)
	copy(blob, slot[:])
	if err := writeFileThrough(path, blob); err != nil {
		return false, err
	}
	return true, nil
}

// readHead reads up to n bytes from the head of path. A file shorter than n
// comes back short (not an error) — the caller's validator decides.
func readHead(path string, n int) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	buf := make([]byte, n)
	got, err := io.ReadFull(f, buf)
	if err != nil && !errors.Is(err, io.ErrUnexpectedEOF) && !errors.Is(err, io.EOF) {
		return nil, err
	}
	return buf[:got], nil
}

// writeFileThrough creates path, writes b, and gets it onto the medium before
// returning. On removable media the bytes have to actually leave the page
// cache before the volume is unmounted, or the device sees a file full of
// nothing.
func writeFileThrough(path string, b []byte) (err error) {
	f, err := OpenWriteThrough(path)
	if err != nil {
		return err
	}
	defer func() {
		cerr := f.Close()
		if err == nil {
			err = cerr
		}
	}()
	if _, err := f.Write(b); err != nil {
		return fmt.Errorf("write %s: %w", path, err)
	}
	if err := f.Sync(); err != nil {
		return fmt.Errorf("flush %s: %w", path, err)
	}
	return nil
}
