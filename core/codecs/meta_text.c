/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/meta_text.c — the shared tag-text policy (see meta_text.h).
 * Extracted from flac_meta.c when the ID3 reader arrived and needed exactly
 * the same rules over differently-encoded bytes.
 */

#include "meta_text.h"

/* Length of the well-formed UTF-8 sequence starting at src[0] (1..4), or 0
 * if it is not one: a bare continuation byte, an overlong form, a surrogate,
 * a codepoint past U+10FFFF, or a sequence cut short by `len`. The same
 * rules mn_utf8_next() in library/names.c applies, restated here over a
 * length-delimited buffer (neither a Vorbis comment value nor an ID3 frame
 * body is NUL-terminated). */
uint32_t meta_utf8_seq_len(const uint8_t *src, uint32_t len)
{
    uint8_t c = src[0];
    uint32_t n;
    uint8_t lo = 0x80, hi = 0xBF;             /* bounds on the second byte */
    if (c < 0x80) {
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        if (c == 0xE0) lo = 0xA0;             /* overlong */
        if (c == 0xED) hi = 0x9F;             /* surrogates */
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        if (c == 0xF0) lo = 0x90;             /* overlong */
        if (c == 0xF4) hi = 0x8F;             /* > U+10FFFF */
    } else {
        return 0;                             /* C0/C1, F5..FF, or a stray 10xxxxxx */
    }
    if (n > len || src[1] < lo || src[1] > hi) {
        return 0;
    }
    for (uint32_t i = 2; i < n; i++) {
        if ((src[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    return n;
}

/* Copy the displayable text of src[0..len) into a bounded, NUL-terminated
 * dst of `cap` bytes (cap includes the NUL): printable ASCII and every
 * well-formed UTF-8 sequence pass through; C0 controls, DEL and bytes that
 * are not part of a valid sequence are dropped. Truncation happens only on a
 * sequence boundary — a sequence that does not fit whole is left out, never
 * cut — so the renderer (which decodes UTF-8, see ui/text) never sees a torn
 * character. This used to keep 0x20..0x7E only, which is why the per-file
 * tag scan (the fallback when CORELIB.IDX is absent) showed "Beyonc" where
 * the index path — whose fields come through the host's utf8_field — showed
 * "Beyoncé". */
void meta_copy_printable(char *dst, uint32_t cap, const uint8_t *src, uint32_t len)
{
    uint32_t i = 0;
    for (uint32_t j = 0; j < len && i + 1 < cap; ) {
        uint8_t c = src[j];
        if (c < 0x80) {
            if (c >= 0x20 && c != 0x7F) {
                dst[i++] = (char)c;
            }
            j++;
            continue;
        }
        uint32_t n = meta_utf8_seq_len(src + j, len - j);
        if (n == 0) {
            j++;                              /* not UTF-8: drop the byte */
            continue;
        }
        if (i + n + 1 > cap) {
            break;                            /* would tear it: stop here */
        }
        for (uint32_t k = 0; k < n; k++) {
            dst[i++] = (char)src[j + k];
        }
        j += n;
    }
    dst[i] = '\0';
}

/* Parse the first run of decimal digits in src[0..len) as a non-negative int
 * (bounded so it can't overflow wildly). Skips any leading non-digits, so
 * "2021-05-01" -> 2021 and "3/12" -> 3. Returns 0 if no digits. */
int meta_parse_leading_int(const uint8_t *src, uint32_t len)
{
    uint32_t j = 0;
    while (j < len && (src[j] < '0' || src[j] > '9')) {
        j++;
    }
    int v = 0, any = 0;
    while (j < len && src[j] >= '0' && src[j] <= '9') {
        if (v < 100000000) {               /* clamp — years/tracks are small */
            v = v * 10 + (src[j] - '0');
        }
        any = 1;
        j++;
    }
    return any ? v : 0;
}
