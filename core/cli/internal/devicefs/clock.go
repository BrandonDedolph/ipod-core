// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"time"
)

// Setting the iPod's clock.
//
// The device has a real-time clock (the PCF50605's, core/docs/hw/06-power.md)
// and no way to learn the time on its own: no network, and on the cable it is
// Apple's boot-ROM disk-mode stack answering the host, not our firmware — the
// iTunes WRITE_BUFFER RTC path in core/docs/hw/07-usb.md needs the device's
// own USB stack, which is not running. So the host cannot TELL the firmware
// anything. What it can do is leave a note where the firmware will read it:
// the settings record it already owns.
//
// StampConfigTime patches the host's current time into the newest slot's copy
// of CORECFG.DAT and writes that copy to the OTHER slot with seq+1 — the same
// alternation the device uses, so the two never fight over a slot. At the next
// boot the firmware compares the stamp with its own mark and with the running
// clock, and decides once (core/kernel/timesync.h). The cost of the scheme is
// latency: the clock the device gets is the clock at the moment of the stamp,
// so `core eject` stamps last, immediately before the volume goes away.
//
// Everything else in the copied slot is preserved BYTE FOR BYTE. The resume
// locator, the queue context, the sound tail, the firmware's own
// applied_epoch / utc_off_min / time_flags and anything a newer firmware has
// appended are the device's, and a host that "helpfully" rewrote them from its
// own model would be silently discarding whatever it did not know about.
const (
	cfgPayloadTime = cfgPayloadLen + 16 // + the time block = 64

	pHostEpoch    = cfgPayloadLen + 0  // 48: u32 UTC seconds
	pHostOffMin   = cfgPayloadLen + 4  // 52: i16 minutes
	pTimeFlags    = cfgPayloadLen + 6  // 54: u8  (the firmware's)
	pAppliedEpoch = cfgPayloadLen + 8  // 56: u32 (the firmware's)
	pUTCOffMin    = cfgPayloadLen + 12 // 60: i16 (the firmware's)

	// Real-world UTC offsets: UTC-12:00 .. UTC+14:00. The firmware clamps to
	// the same range on decode (core/kernel/config.c), so writing outside it
	// would only mean writing something the device reads back differently.
	utcOffMinMinutes = -720
	utcOffMaxMinutes = 840
)

// TimeStamp is the time block as it sits in a record: what the host wrote and
// what the device has done about it.
type TimeStamp struct {
	HostEpoch    uint32 // UTC seconds the host stamped; 0 = never
	HostOffMin   int16  // the host's UTC offset then, in minutes
	AppliedEpoch uint32 // the stamp the firmware acted on; 0 = none yet
	UTCOffMin    int16  // the device's display offset
	Use24H       bool   // the device's Time Format setting
	TimeInTitle  bool   // the device's Time in Title setting
}

// Pending reports whether a stamp is waiting for the device to boot. It is the
// question `core doctor` asks: a stamp the firmware has not acted on is not an
// error (the device simply has not been switched on since), but it does mean
// the clock on the iPod is not yet the clock the host wrote.
func (t TimeStamp) Pending() bool {
	return t.HostEpoch != 0 && t.AppliedEpoch != t.HostEpoch
}

// DecodeConfigTime reads the time block out of one slot. ok is false when the
// slot is not a valid record or is shorter than the time block — a record
// written before the clock existed (length 48) simply has no stamp, which is
// not a failure.
func DecodeConfigTime(b []byte) (TimeStamp, bool) {
	if _, _, valid := DecodeConfigSlot(b); !valid {
		return TimeStamp{}, false
	}
	if int(binary.LittleEndian.Uint16(b[cfgOffLength:])) < cfgPayloadTime {
		return TimeStamp{}, false
	}
	p := b[cfgOffPayload:]
	flags := p[pTimeFlags]
	return TimeStamp{
		HostEpoch:    binary.LittleEndian.Uint32(p[pHostEpoch:]),
		HostOffMin:   int16(binary.LittleEndian.Uint16(p[pHostOffMin:])),
		AppliedEpoch: binary.LittleEndian.Uint32(p[pAppliedEpoch:]),
		UTCOffMin:    int16(binary.LittleEndian.Uint16(p[pUTCOffMin:])),
		Use24H:       flags&0x01 != 0,
		TimeInTitle:  flags&0x02 != 0,
	}, true
}

// stampSlot is the patch itself, pure so it can be tested against
// tools/make_config.py's stamp_slot() byte for byte: copy the record, grow the
// declared length to cover the time block, write host_epoch / host_off_min,
// bump the sequence, recompute the CRC. Nothing else moves.
func stampSlot(slot []byte, now time.Time) []byte {
	rec := make([]byte, ConfigSlotBytes)
	copy(rec, slot[:ConfigSlotBytes])

	if length := int(binary.LittleEndian.Uint16(rec[cfgOffLength:])); length < cfgPayloadTime {
		// The bytes between the old length and the time block are not fields
		// yet, so they are zeroed rather than promoted: a 48-byte record has
		// no applied_epoch, and inventing one out of padding would tell the
		// firmware a stamp had already been acted on when it had not.
		for i := cfgOffPayload + length; i < cfgOffPayload+cfgPayloadTime; i++ {
			rec[i] = 0
		}
		binary.LittleEndian.PutUint16(rec[cfgOffLength:], cfgPayloadTime)
	}

	_, offSec := now.Zone()
	offMin := offSec / 60
	if offMin < utcOffMinMinutes {
		offMin = utcOffMinMinutes
	} else if offMin > utcOffMaxMinutes {
		offMin = utcOffMaxMinutes
	}

	p := rec[cfgOffPayload:]
	binary.LittleEndian.PutUint32(p[pHostEpoch:], uint32(now.UTC().Unix()))
	binary.LittleEndian.PutUint16(p[pHostOffMin:], uint16(int16(offMin)))

	seq := binary.LittleEndian.Uint32(rec[cfgOffSeq:])
	binary.LittleEndian.PutUint32(rec[cfgOffSeq:], seq+1)
	binary.LittleEndian.PutUint32(rec[cfgOffCRC:], crc32.ChecksumIEEE(rec[:cfgOffCRC]))
	return rec
}

// Stamped is what StampConfigTime did, for the reports to print.
type Stamped struct {
	When   time.Time // the moment written
	OffMin int       // the UTC offset written, in minutes
	Slot   int       // the slot it landed in
	Seq    uint32    // its sequence number
}

// StampConfigTime writes the host's current time into volumeRoot's
// CORECFG.DAT. The file must already exist and hold a valid record —
// EnsureConfig runs first in every caller, so a fresh install gets a clock on
// its first boot too.
//
// The write is ONE slot, through a handle opened WITHOUT truncation, at a
// fixed offset. The file's size, cluster chain and directory entry do not
// change; nothing the firmware's read-only FAT32 driver depends on is touched.
func StampConfigTime(volumeRoot string, now time.Time) (Stamped, error) {
	path := filepath.Join(volumeRoot, ConfigName)

	head, err := readHead(path, ConfigMinBytes)
	if err != nil {
		return Stamped{}, err
	}
	if len(head) < ConfigMinBytes {
		return Stamped{}, fmt.Errorf("%s: %d bytes, need %d", path, len(head), ConfigMinBytes)
	}

	src, have := -1, uint32(0)
	for i := 0; i < ConfigSlots; i++ {
		off := i * ConfigSlotBytes
		if seq, _, ok := DecodeConfigSlot(head[off : off+ConfigSlotBytes]); ok {
			if src < 0 || seqNewer(seq, have) {
				src, have = i, seq
			}
		}
	}
	if src < 0 {
		return Stamped{}, fmt.Errorf("%s holds no valid record", path)
	}

	dst := 1 - src
	rec := stampSlot(head[src*ConfigSlotBytes:(src+1)*ConfigSlotBytes], now)

	if err := writeSlotThrough(path, dst, rec); err != nil {
		return Stamped{}, err
	}

	_, offSec := now.Zone()
	return Stamped{
		When:   now,
		OffMin: offSec / 60,
		Slot:   dst,
		Seq:    have + 1,
	}, nil
}

// writeSlotThrough rewrites one slot of an existing file in place and gets it
// onto the medium before returning. The close error is reported, like
// writeFileThrough's: on removable media that is where a failed flush shows up.
func writeSlotThrough(path string, slot int, rec []byte) (err error) {
	f, err := OpenWriteThroughExisting(path)
	if err != nil {
		return err
	}
	defer func() {
		cerr := f.Close()
		if err == nil && cerr != nil {
			err = fmt.Errorf("close %s: %w", path, cerr)
		}
	}()
	if _, err := f.WriteAt(rec, int64(slot*ConfigSlotBytes)); err != nil {
		return fmt.Errorf("write %s: %w", path, err)
	}
	if err := f.Sync(); err != nil {
		return fmt.Errorf("sync %s: %w", path, err)
	}
	return nil
}

// StampConfigTimeIfVolume stamps only when target is a directory we can write
// into. `core eject /dev/sdb1` names a device node, not a mount point, and
// there is no filesystem there to patch — the caller prints the one-line skip
// this returns as (Stamped{}, false, nil).
func StampConfigTimeIfVolume(target string, now time.Time) (Stamped, bool, error) {
	st, err := os.Stat(target)
	if err != nil || !st.IsDir() {
		return Stamped{}, false, nil
	}
	s, err := StampConfigTime(target, now)
	if err != nil {
		return Stamped{}, false, err
	}
	return s, true, nil
}

// seqNewer is config_seq_newer() (core/kernel/config.c): a signed difference,
// so 0xFFFFFFFF -> 0 reads as one step forward rather than four billion back.
// The host has to agree with the device about which slot is live, or a stamp
// would be written over the record the firmware is about to load.
func seqNewer(a, b uint32) bool {
	return int32(a-b) > 0
}
