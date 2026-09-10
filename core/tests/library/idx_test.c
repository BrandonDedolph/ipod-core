/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/library/idx_test.c — host tests for the CORELIB.IDX header and
 * integrity checks in library/idx.c: which files may the library loader
 * parse records out of?
 *
 * The loader used to check the magic and that rec_size was 256, and then
 * trusted everything else: the version bytes were never read and nothing
 * confirmed that the bytes after the header were the records the header
 * claimed. A half-copied index, or a future layout with a bumped version,
 * parsed field by field as plausible garbage — no error, just wrong
 * durations, titles and hashes. The rules are now:
 *
 *   - only versions 1 and 2 are known; anything else is refused;
 *   - the file must be exactly header + count * 256 bytes;
 *   - a v2 header carries a CRC-32 (zlib's) over the records.
 *
 * This compiles the REAL library/idx.c — the same source the ARM build links.
 * (Until idx.c existed the functions were statics in kernel/main.c, which the
 * host cannot compile, and this file carried verbatim copies kept honest by a
 * diff script.) What a unit test of the validator cannot show is that the
 * loader consults it: tests/scripts/check_index_loader.py checks that
 * library_load_index() in main.c actually calls idx_header_parse() with the
 * directory entry's size and refuses on IDX_ECRC — a validator the loader
 * does not consult is decoration.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../../library/idx.h"

static int g_fails;

static void check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
}

/* ---- header fixtures ---------------------------------------------------- */

static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A header as build_index.py writes it (or wrote it, for v1). */
static void mk_hdr(uint8_t *h, uint32_t ver, uint32_t rec, uint32_t count,
                   uint32_t crc)
{
    memset(h, 0xA5, IDX_HDR_V2);        /* the unused v1 tail is not zero */
    memcpy(h, "CIDX", 4);
    wr16(h + 4, ver);
    wr16(h + 6, rec);
    wr32(h + 8, count);
    wr32(h + 12, crc);
}

/* ---- 1. the CRC is zlib's --------------------------------------------- */

static void test_crc(void)
{
    /* The check value every CRC-32 implementation is documented against, and
     * what Python's zlib.crc32(b"123456789") returns. If this is wrong, every
     * v2 index the host writes is refused on the device. */
    const uint8_t s[] = "123456789";
    check("crc32(\"123456789\") == 0xCBF43926",
          ~crc32_update(0xFFFFFFFFu, s, 9) == 0xCBF43926u);
    check("crc32 of nothing is 0",
          ~crc32_update(0xFFFFFFFFu, s, 0) == 0u);

    /* The loader feeds the records in 16 KB batches; the result must not
     * depend on where the batch boundaries fall. */
    uint32_t c = crc32_update(0xFFFFFFFFu, s, 4);
    c = crc32_update(c, s + 4, 5);
    check("incremental over two batches equals one shot",
          ~c == 0xCBF43926u);

    /* One flipped bit anywhere is detected. */
    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = (uint8_t)(i * 7);
    uint32_t good = ~crc32_update(0xFFFFFFFFu, buf, 512);
    int all_differ = 1;
    for (int i = 0; i < 512; i += 37) {
        buf[i] ^= 0x10;
        if (~crc32_update(0xFFFFFFFFu, buf, 512) == good) all_differ = 0;
        buf[i] ^= 0x10;
    }
    check("a single flipped bit changes the CRC", all_differ);
}

/* ---- 2. headers this loader accepts ------------------------------------ */

static void test_accept(void)
{
    uint8_t h[IDX_HDR_V2];
    idx_hdr_t out;

    /* v1: what every index built before the CRC existed looks like. It must
     * keep loading — there is one on a device right now. */
    mk_hdr(h, 1, 256, 3, 0);
    check("v1 with the right size is accepted",
          idx_header_parse(h, IDX_HDR_V1 + 3 * 256, &out) == IDX_OK);
    check("v1: 12-byte header, no CRC, count read",
          out.hdr_len == 12 && out.has_crc == 0 && out.count == 3);

    mk_hdr(h, 1, 256, 0, 0);
    check("v1 with no records is accepted",
          idx_header_parse(h, IDX_HDR_V1, &out) == IDX_OK && out.count == 0);

    /* v2: what build_index.py writes now. */
    mk_hdr(h, 2, 256, 1200, 0xCBF43926u);
    check("v2 with the right size is accepted",
          idx_header_parse(h, IDX_HDR_V2 + 1200 * 256, &out) == IDX_OK);
    check("v2: 16-byte header, CRC read",
          out.hdr_len == 16 && out.has_crc == 1 && out.count == 1200 &&
          out.crc == 0xCBF43926u);

    mk_hdr(h, 2, 256, 0, 0);
    check("v2 with no records is accepted",
          idx_header_parse(h, IDX_HDR_V2, &out) == IDX_OK && out.count == 0);
}

/* ---- 3. headers it must refuse ----------------------------------------- */

static void test_refuse(void)
{
    uint8_t h[IDX_HDR_V2];
    idx_hdr_t out;

    mk_hdr(h, 1, 256, 3, 0);
    h[3] = 'Y';
    check("wrong magic", idx_header_parse(h, IDX_HDR_V1 + 3 * 256, &out) == IDX_EMAGIC);

    /* The version was never read before, so a v3 that re-laid out the record
     * loaded as garbage. Both directions: too new, and the 0 a zero-filled
     * file has. */
    mk_hdr(h, 3, 256, 3, 0);
    check("unknown version 3", idx_header_parse(h, IDX_HDR_V2 + 3 * 256, &out) == IDX_EVERSION);
    mk_hdr(h, 0, 256, 3, 0);
    check("version 0", idx_header_parse(h, IDX_HDR_V1 + 3 * 256, &out) == IDX_EVERSION);
    mk_hdr(h, 0xFFFF, 256, 3, 0);
    check("version 0xFFFF", idx_header_parse(h, IDX_HDR_V2 + 3 * 256, &out) == IDX_EVERSION);

    mk_hdr(h, 2, 255, 3, 0);
    check("record size 255", idx_header_parse(h, IDX_HDR_V2 + 3 * 255, &out) == IDX_ERECSIZE);
    mk_hdr(h, 2, 512, 3, 0);
    check("record size 512", idx_header_parse(h, IDX_HDR_V2 + 3 * 512, &out) == IDX_ERECSIZE);

    /* A copy that stopped mid-file. This is the one that happens. */
    mk_hdr(h, 2, 256, 3, 0);
    check("v2 truncated by one byte",
          idx_header_parse(h, IDX_HDR_V2 + 3 * 256 - 1, &out) == IDX_ESIZE);
    check("v2 truncated to a whole record",
          idx_header_parse(h, IDX_HDR_V2 + 2 * 256, &out) == IDX_ESIZE);
    check("v2 with trailing bytes",
          idx_header_parse(h, IDX_HDR_V2 + 3 * 256 + 1, &out) == IDX_ESIZE);
    mk_hdr(h, 1, 256, 3, 0);
    check("v1 truncated",
          idx_header_parse(h, IDX_HDR_V1 + 2 * 256, &out) == IDX_ESIZE);
    check("v1 sized as if it had a v2 header",
          idx_header_parse(h, IDX_HDR_V2 + 3 * 256, &out) == IDX_ESIZE);
    check("a header with nothing after it and count 3",
          idx_header_parse(h, IDX_HDR_V1, &out) == IDX_ESIZE);

    /* count * 256 wrapping to 0 would make a 16-byte file "consistent". */
    mk_hdr(h, 2, 256, 0x01000000u, 0);
    check("count that wraps the size arithmetic",
          idx_header_parse(h, IDX_HDR_V2, &out) == IDX_ESIZE);
    mk_hdr(h, 2, 256, 0xFFFFFFFFu, 0);
    check("count 0xFFFFFFFF",
          idx_header_parse(h, IDX_HDR_V2 + 0xFFFFFF00u, &out) == IDX_ESIZE);
}

int main(void)
{
    test_crc();
    test_accept();
    test_refuse();

    printf("idx_test: %s (%d failure%s)\n",
           g_fails == 0 ? "PASS" : "FAIL", g_fails, g_fails == 1 ? "" : "s");
    return g_fails == 0 ? 0 : 1;
}
