// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
)

// CORELOG.BIN — the pre-allocated ring the firmware writes its UART narration
// into, one 2048-byte block at a time (core/kernel/evlog.c, mirrored by
// tools/make_log.py).
//
// The file is LogBlockCount(size) blocks of LogBlockBytes. Block 0 is the
// header and is written ONLY by the host; blocks 1..count-1 are the ring and
// are left zero, because "no magic" is what unwritten means to the device.
//
// Header block (little-endian), core/kernel/evlog.c:103-138:
//
//	off  size  field
//	0    4     magic        'C''L''O''G'  (0x474F4C43 LE)
//	4    2     version      1
//	6    2     block_size   2048
//	8    4     block_count  blocks in the file INCLUDING this one
//	12   4     file_id      random; tells two logs apart in a pile of dumps
//	16   4     crc32        zlib CRC-32 over bytes [0, 16)
//	(rest zero)
const (
	LogBlockBytes   = 2048
	LogMinBlocks    = 2     // header + one ring slot (core/kernel/evlog.h:100)
	LogMaxBlocks    = 65536 // the firmware's ceiling (core/kernel/evlog.h:101)
	LogVersion      = 1
	LogMagic        = 0x474F4C43 // 'C''L''O''G' LE
	LogDefaultBytes = 4 * 1024 * 1024

	logOffMagic   = 0
	logOffVersion = 4
	logOffBSize   = 6
	logOffCount   = 8
	logOffFileID  = 12
	logOffCRC     = 16
	logCRCBytes   = 16
)

// EncodeLogHeader builds block 0: magic, version, block size, the block count
// the file claims, the id, and the CRC over the first 16 bytes. Everything
// after byte 20 is zero. Mirrors evlog_header_encode()
// (core/kernel/evlog.c:128-139).
func EncodeLogHeader(blockCount uint32, fileID uint32) [LogBlockBytes]byte {
	var blk [LogBlockBytes]byte
	binary.LittleEndian.PutUint32(blk[logOffMagic:], LogMagic)
	binary.LittleEndian.PutUint16(blk[logOffVersion:], LogVersion)
	binary.LittleEndian.PutUint16(blk[logOffBSize:], LogBlockBytes)
	binary.LittleEndian.PutUint32(blk[logOffCount:], blockCount)
	binary.LittleEndian.PutUint32(blk[logOffFileID:], fileID)
	binary.LittleEndian.PutUint32(blk[logOffCRC:], crc32.ChecksumIEEE(blk[:logCRCBytes]))
	return blk
}

// DecodeLogHeader validates block 0 and returns its claimed block count and
// file id. Same checks, in the same order, as evlog_header_decode()
// (core/kernel/evlog.c:141-165).
func DecodeLogHeader(blk []byte) (blockCount, fileID uint32, ok bool) {
	if len(blk) < LogBlockBytes {
		return 0, 0, false
	}
	if binary.LittleEndian.Uint32(blk[logOffMagic:]) != LogMagic {
		return 0, 0, false
	}
	if binary.LittleEndian.Uint16(blk[logOffVersion:]) != LogVersion {
		return 0, 0, false
	}
	if binary.LittleEndian.Uint16(blk[logOffBSize:]) != LogBlockBytes {
		return 0, 0, false
	}
	n := binary.LittleEndian.Uint32(blk[logOffCount:])
	if n < LogMinBlocks || n > LogMaxBlocks {
		return 0, 0, false
	}
	if crc32.ChecksumIEEE(blk[:logCRCBytes]) != binary.LittleEndian.Uint32(blk[logOffCRC:]) {
		return 0, 0, false
	}
	return n, binary.LittleEndian.Uint32(blk[logOffFileID:]), true
}

// EnsureLog makes sure volumeRoot holds a CORELOG.BIN the firmware will mount:
// size bytes (LogDefaultBytes when size <= 0), a valid header in block 0 and a
// zero-filled ring behind it.
//
// An existing file whose header validates AND whose claimed block count
// matches its actual size on disk is left alone (created == false) — that is
// evlog_mount()'s own test (core/kernel/evlog.c:508-520: the header's count
// must equal size/2048, or the log stays off, silently). Anything else is
// rewritten, which discards whatever the ring held; there is nothing else to
// do with a log the device will refuse.
func EnsureLog(volumeRoot string, size int64) (created bool, err error) {
	if err := RefusesMount(volumeRoot); err != nil {
		return false, err
	}
	if size <= 0 {
		size = LogDefaultBytes
	}
	if size%LogBlockBytes != 0 {
		return false, fmt.Errorf("devicefs: log size %d is not a multiple of %d", size, LogBlockBytes)
	}
	count := size / LogBlockBytes
	if count < LogMinBlocks || count > LogMaxBlocks {
		return false, fmt.Errorf("devicefs: log size %d is %d blocks, want %d..%d",
			size, count, LogMinBlocks, LogMaxBlocks)
	}

	path := filepath.Join(volumeRoot, LogName)
	switch st, err := os.Stat(path); {
	case err == nil:
		head, herr := readHead(path, LogBlockBytes)
		if herr != nil {
			return false, herr
		}
		if n, _, ok := DecodeLogHeader(head); ok && int64(n)*LogBlockBytes == st.Size() {
			return false, nil
		}
	case errors.Is(err, os.ErrNotExist):
		// fall through and create it
	default:
		return false, err
	}

	fileID, err := randomFileID()
	if err != nil {
		return false, err
	}
	blob := make([]byte, size)
	hdr := EncodeLogHeader(uint32(count), fileID)
	copy(blob, hdr[:])
	if err := writeFileThrough(path, blob); err != nil {
		return false, err
	}
	return true, nil
}

// randomFileID is make_log.py's os.urandom(4): the id exists only to tell two
// dumps apart, so any source of four unpredictable bytes will do.
func randomFileID() (uint32, error) {
	var b [4]byte
	if _, err := rand.Read(b[:]); err != nil {
		return 0, fmt.Errorf("devicefs: random file id: %w", err)
	}
	return binary.LittleEndian.Uint32(b[:]), nil
}
