/*
 * core/codecs/dr_flac/tag_flac.c — Vorbis-comment reader implementation.
 *
 * dr_flac.h is included for declarations only (DR_FLAC_IMPLEMENTATION
 * is defined in flac.c which lives in the same static library).
 *
 * Flow:
 *   1. drflac_open_memory_with_metadata fires our onMeta callback for
 *      every metadata block. For VORBIS_COMMENT we capture the
 *      (commentCount, pComments) tuple via an iterator.
 *   2. Each comment is "KEY=VALUE" without a null terminator. We
 *      uppercase the key and match against TITLE / ARTIST / ALBUM.
 *   3. Truncate the value at TAG_FIELD_MAX-1 bytes (UTF-8 byte
 *      boundary; not code-point aware).
 *   4. Close the decoder; throw away the audio path.
 */

#include "tag_flac.h"
#include "dr_flac.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

static void copy_value(char *dst, size_t dst_size,
                       const char *src, size_t src_len) {
    if (src_len >= dst_size) src_len = dst_size - 1;
    memcpy(dst, src, src_len);
    dst[src_len] = 0;
}

/*
 * Match `comment` ("KEY=VALUE", no null terminator) against `key`
 * (case-insensitive, ASCII). On match, returns a pointer into the
 * comment to the first byte after '=' and writes the remaining
 * length to *out_value_len. Returns NULL on no match.
 */
static const char *match_key(const char *comment, drflac_uint32 comment_len,
                             const char *key,
                             drflac_uint32 *out_value_len) {
    size_t key_len = strlen(key);
    if (comment_len <= key_len) return NULL;
    if (comment[key_len] != '=') return NULL;
    for (size_t i = 0; i < key_len; i++) {
        if (toupper((unsigned char)comment[i]) !=
            toupper((unsigned char)key[i])) {
            return NULL;
        }
    }
    *out_value_len = comment_len - (drflac_uint32)key_len - 1;
    return comment + key_len + 1;
}

static void on_meta(void *user, drflac_metadata *m) {
    audio_tags_t *tags = (audio_tags_t *)user;
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;

    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it,
                                        m->data.vorbis_comment.commentCount,
                                        m->data.vorbis_comment.pComments);

    drflac_uint32 comment_len;
    const char *comment;
    while ((comment = drflac_next_vorbis_comment(&it, &comment_len)) != NULL) {
        drflac_uint32 vlen;
        const char *v;
        if ((v = match_key(comment, comment_len, "TITLE", &vlen)) && !tags->found_title) {
            copy_value(tags->title, sizeof(tags->title), v, vlen);
            tags->found_title = 1;
        } else if ((v = match_key(comment, comment_len, "ARTIST", &vlen)) && !tags->found_artist) {
            copy_value(tags->artist, sizeof(tags->artist), v, vlen);
            tags->found_artist = 1;
        } else if ((v = match_key(comment, comment_len, "ALBUM", &vlen)) && !tags->found_album) {
            copy_value(tags->album, sizeof(tags->album), v, vlen);
            tags->found_album = 1;
        }
    }
}

int tag_flac_read(const void *bytes, size_t len, audio_tags_t *out) {
    if (!bytes || len == 0 || !out) return -1;
    memset(out, 0, sizeof(*out));

    drflac *f = drflac_open_memory_with_metadata(bytes, len, on_meta, out, NULL);
    if (!f) return -1;
    drflac_close(f);
    return 0;
}
