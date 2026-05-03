/*
 * core/codecs/dr_mp3/tag_mp3.h — ID3v2 tag reader for MP3.
 *
 * dr_mp3 doesn't parse ID3 tags; it just skips the ID3v2 prefix when
 * decoding. We do our own minimal parser here to extract title /
 * artist / album so the NP screen can show real metadata. Self-
 * contained (no dependency on dr_mp3) — ID3v2 lives at the start of
 * the file and is independent of the audio data.
 *
 * Supports ID3v2.3 and v2.4 frame layouts (the two versions in the
 * wild). Text encodings 0 (ISO-8859-1) and 3 (UTF-8) are handled
 * losslessly; 1/2 (UTF-16 LE/BE) are read in best-effort: we strip
 * BOM and downconvert to ASCII for the BMP plane (high-byte chars
 * become '?'), which renders correctly for the common case of
 * ASCII-only metadata stored in UTF-16 by Windows tools. This keeps
 * the parser compact at the cost of unicode fidelity for non-Latin
 * scripts in UTF-16 frames — fine for now; we can revisit when the
 * UI gains a UTF-8 atlas wider than the BMP plane.
 */

#ifndef CORE_CODECS_DR_MP3_TAG_MP3_H
#define CORE_CODECS_DR_MP3_TAG_MP3_H

#include "../tags.h"

#include <stddef.h>

/*
 * Read ID3v2 tags from `bytes` (a complete MP3 file in memory) into
 * `*out`. Zeroes `*out` first, then fills any of TIT2 / TPE1 / TALB /
 * TCON / TCOM / APIC that are present.
 *
 * Returns:
 *    0 on success or if no ID3v2 header is present (the file is just
 *      treated as having no tags — `*out` stays zeroed).
 *   -1 only on argument errors (NULL input, len==0).
 *
 * Note: a corrupted ID3v2 header (bad sync, invalid sizes, etc) is
 * treated the same as "no tags" rather than as an error — the audio
 * is probably still playable, we just won't show metadata.
 */
int tag_mp3_read(const void *bytes, size_t len, audio_tags_t *out);

#endif /* CORE_CODECS_DR_MP3_TAG_MP3_H */
