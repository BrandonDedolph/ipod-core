/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/idx.c — CORELIB.IDX header validation and record CRC.
 *
 * Moved VERBATIM out of kernel/main.c (only the `static` came off the two
 * entry points, so the host tests can link the same code the device runs).
 * See idx.h for the rules and why.
 */

#include "idx.h"

static uint32_t idx_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Validate a header. `h` holds at least IDX_HDR_V1 bytes, and IDX_HDR_V2 when
 * the version word says v2 (the caller reads the four extra bytes only then,
 * because on a v1 file they would be the first bytes of record 0 and the
 * stream has no way back). `file_size` is the directory entry's size.
 */
int idx_header_parse(const uint8_t *h, uint32_t file_size, idx_hdr_t *out)
{
    if (h[0] != 'C' || h[1] != 'I' || h[2] != 'D' || h[3] != 'X') return IDX_EMAGIC;
    uint32_t ver = (uint32_t)h[4] | ((uint32_t)h[5] << 8);
    uint32_t rec = (uint32_t)h[6] | ((uint32_t)h[7] << 8);
    if (ver != 1 && ver != 2) return IDX_EVERSION;
    if (rec != IDX_REC_SIZE) return IDX_ERECSIZE;
    out->count   = idx_rd32(h + 8);
    out->hdr_len = (ver == 2) ? IDX_HDR_V2 : IDX_HDR_V1;
    out->has_crc = (ver == 2);
    out->crc     = (ver == 2) ? idx_rd32(h + 12) : 0;
    /* count * 256 must not wrap: a count of 0x01000000 would otherwise pass
     * the size check against a 16-byte file and set the loop up to read 16M
     * records that are not there. */
    if (out->count > (0xFFFFFFFFu - IDX_HDR_V2) / IDX_REC_SIZE) return IDX_ESIZE;
    if (file_size != out->hdr_len + out->count * IDX_REC_SIZE) return IDX_ESIZE;
    return IDX_OK;
}

/*
 * CRC-32 (reflected, polynomial 0xEDB88320, init/final 0xFFFFFFFF): what
 * zlib.crc32 computes, so build_index.py can stamp it. Table-driven, unlike
 * config.c's bitwise crc32_buf: that one runs over a 1 KB record, this one
 * over the whole index — up to 1.5 MB at LIB_MAX_SONGS — and eight shift
 * steps per byte at 80 MHz would be most of a second on the boot path. The
 * table is 1 KB of .bss, filled on first use. Incremental: seed with
 * 0xFFFFFFFF, feed the batches as they stream in, invert at the end.
 */
static uint32_t g_crc_tab[256];
static int      g_crc_tab_ready;

static void crc32_tab_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)0u - (c & 1u);
            c = (c >> 1) ^ (0xEDB88320u & mask);
        }
        g_crc_tab[i] = c;
    }
    g_crc_tab_ready = 1;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    if (!g_crc_tab_ready) crc32_tab_init();
    for (uint32_t i = 0; i < n; i++) {
        crc = g_crc_tab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}
