/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/meta_text.h — the text policy every tag reader shares.
 *
 * A FLAC's Vorbis comments and an MP3's ID3 frames arrive in different
 * encodings but land in the same flac_meta_t fields and are drawn by the same
 * UTF-8 renderer, so the rule for "what is allowed into a display field" has
 * to be one rule. It lives here rather than in either reader:
 *
 *   - printable ASCII and well-formed UTF-8 pass through;
 *   - C0 controls, DEL and bytes that are not part of a valid sequence are
 *     dropped (a tag is not a trusted string);
 *   - truncation happens only on a sequence boundary, so the renderer never
 *     sees a torn character.
 *
 * Freestanding-clean: no libc, no allocation, length-delimited input (neither
 * a Vorbis comment nor an ID3 frame is NUL-terminated).
 */
#ifndef CORE_CODECS_META_TEXT_H
#define CORE_CODECS_META_TEXT_H

#include <stdint.h>

/*
 * Length of the well-formed UTF-8 sequence starting at src[0] (1..4), or 0 if
 * it is not one: a bare continuation byte, an overlong form, a surrogate, a
 * codepoint past U+10FFFF, or a sequence cut short by `len`. The same rules
 * mn_utf8_next() in library/names.c applies.
 */
uint32_t meta_utf8_seq_len(const uint8_t *src, uint32_t len);

/*
 * Copy the displayable text of src[0..len) into a bounded, NUL-terminated
 * `dst` of `cap` bytes (cap includes the NUL), under the policy above.
 */
void meta_copy_printable(char *dst, uint32_t cap, const uint8_t *src, uint32_t len);

/*
 * The first run of decimal digits in src[0..len) as a non-negative int,
 * bounded so it cannot overflow wildly. Leading non-digits are skipped, so
 * "2021-05-01" -> 2021 and "3/12" -> 3. Returns 0 when there are no digits.
 */
int meta_parse_leading_int(const uint8_t *src, uint32_t len);

#endif /* CORE_CODECS_META_TEXT_H */
