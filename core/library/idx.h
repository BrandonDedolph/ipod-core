/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/idx.h — CORELIB.IDX: header validation and integrity.
 *
 * The host-built index (tools/build_index.py) is a header followed by
 * 256-byte records. The loader used to check the magic and that rec_size was
 * 256, and nothing else: the version bytes were never read, and there was no
 * check that the bytes after the header were the records the header claimed.
 * A half-copied index (a copy that stopped mid-file, or a tool that pre-sized
 * the file and never filled it) or a future layout with a bumped version
 * parsed field by field as plausible garbage — durations, titles and hashes
 * from wherever the offsets happened to land. Now:
 *
 *   - the version must be one this loader was written for (1 or 2); anything
 *     else is rejected before a record is read, so a v3 that moves fields
 *     falls back to the tag scan instead of loading nonsense;
 *   - the file size must be exactly header + count * 256 — so a truncated
 *     copy is refused up front, whatever its version;
 *   - a v2 header carries a CRC-32 (zlib's, the same one config.c checks its
 *     record with) over the records, verified as they stream past.
 *
 * v1 (the 12-byte header build_index.py wrote before the CRC existed) is
 * still accepted, on the size check alone, so an index already on a device
 * keeps loading; the host writes v2 now.
 *
 * WHY THIS FILE EXISTS. These functions were statics in kernel/main.c, which
 * the host cannot compile, so tests/kernel/index_test.c carried VERBATIM
 * COPIES with a script diffing them against the original. They read a file
 * off the user's disk and decide whether to trust it, which is exactly the
 * code that must be testable directly. The stream reads and the record
 * decode stay in main.c's library_load_index; this is the pure part — bytes
 * in, verdict out. No hw/, no MMIO, no globals beyond the CRC table.
 */

#ifndef CORE_LIBRARY_IDX_H
#define CORE_LIBRARY_IDX_H

#include <stdint.h>

#define IDX_REC_SIZE 256u
#define IDX_HDR_V1   12u
#define IDX_HDR_V2   16u

enum {
    IDX_OK = 0,
    IDX_EMAGIC,          /* not a CIDX file                                   */
    IDX_EVERSION,        /* a version this loader does not know               */
    IDX_ERECSIZE,        /* record size is not 256                            */
    IDX_ESIZE,           /* file size != header + count * 256 (truncated)     */
    IDX_ECRC,            /* the records are not the ones the header signed    */
    IDX_EREAD,           /* the stream came up short                          */
};

typedef struct {
    uint32_t hdr_len;    /* 12 or 16                                          */
    uint32_t count;
    uint32_t crc;        /* records' CRC-32 from the header (v2)              */
    int      has_crc;    /* v2: verify `crc`; v1: size check only             */
} idx_hdr_t;

/*
 * Validate a header. `h` holds at least IDX_HDR_V1 bytes, and IDX_HDR_V2 when
 * the version word says v2 (the caller reads the four extra bytes only then,
 * because on a v1 file they would be the first bytes of record 0 and the
 * stream has no way back). `file_size` is the directory entry's size.
 * Returns IDX_OK or the IDX_E* reason; `out` is only meaningful on IDX_OK.
 */
int idx_header_parse(const uint8_t *h, uint32_t file_size, idx_hdr_t *out);

/*
 * CRC-32 (reflected, polynomial 0xEDB88320, init/final 0xFFFFFFFF): what
 * zlib.crc32 computes, so build_index.py can stamp it. Incremental: seed with
 * 0xFFFFFFFF, feed the batches as they stream in, invert at the end. The
 * 1 KB table behind it is filled on first use.
 */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);

#endif /* CORE_LIBRARY_IDX_H */
