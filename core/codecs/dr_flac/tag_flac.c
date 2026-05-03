/*
 * core/codecs/dr_flac/tag_flac.c — Vorbis-comment + PICTURE reader.
 *
 * dr_flac.h is included for declarations only (DR_FLAC_IMPLEMENTATION
 * is defined in flac.c which lives in the same static library).
 *
 * Flow:
 *   1. drflac_open_memory_with_metadata fires our onMeta callback for
 *      every metadata block.
 *   2. VORBIS_COMMENT blocks → iterate KEY=VALUE comments, match
 *      TITLE/ARTIST/ALBUM, truncate-copy into the audio_tags_t fields.
 *   3. PICTURE blocks → if we don't already have a front-cover stashed,
 *      heap-copy the picture bytes into tags->art_bytes. Front cover
 *      (type 3) is preferred — if we see one after a non-front
 *      already stashed, it overrides.
 *   4. Close the decoder; throw away the audio path.
 *
 * Memory: the heap-copy in (3) is the tag reader taking ownership of
 * a small buffer (typical album art is 50-500 KB). Caller releases via
 * audio_tags_free.
 */

#include "tag_flac.h"
#include "dr_flac.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
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

/* FLAC PICTURE block "type" enum value for "Cover (front)". Per the
 * FLAC spec / ID3v2 picture-type registry. */
#define PICTURE_TYPE_FRONT_COVER 3

/* If true, our currently-stashed picture (tags->art_bytes != NULL)
 * was a front cover and should not be overridden by a later non-front
 * picture. Tracked via a sentinel flag in tags->found_art:
 *   0 = nothing stashed
 *   1 = stashed, non-front (override OK if a front cover comes later)
 *   2 = stashed, front cover (don't override) */

static void stash_picture(audio_tags_t *tags, drflac_metadata *m) {
    drflac_uint32 size = m->data.picture.pictureDataSize;
    const drflac_uint8 *data = m->data.picture.pPictureData;
    drflac_uint32 type = m->data.picture.type;
    if (size == 0 || !data) return;

    /* If we already have a front cover, leave it alone. */
    if (tags->found_art == 2) return;
    /* If we already have a non-front and the new one isn't a front,
     * keep the existing one (first-wins for same priority). */
    if (tags->found_art == 1 && type != PICTURE_TYPE_FRONT_COVER) return;

    void *copy = malloc(size);
    if (!copy) return;        /* OOM: silently no art */
    memcpy(copy, data, size);

    /* Drop any prior copy before installing the new one. */
    free(tags->art_bytes);
    tags->art_bytes = copy;
    tags->art_len   = size;
    tags->found_art = (type == PICTURE_TYPE_FRONT_COVER) ? 2 : 1;
}

static void on_meta(void *user, drflac_metadata *m) {
    audio_tags_t *tags = (audio_tags_t *)user;
    switch (m->type) {
    case DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT: {
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
            } else if ((v = match_key(comment, comment_len, "GENRE", &vlen)) && !tags->found_genre) {
                copy_value(tags->genre, sizeof(tags->genre), v, vlen);
                tags->found_genre = 1;
            } else if ((v = match_key(comment, comment_len, "COMPOSER", &vlen)) && !tags->found_composer) {
                copy_value(tags->composer, sizeof(tags->composer), v, vlen);
                tags->found_composer = 1;
            }
        }
        break;
    }
    case DRFLAC_METADATA_BLOCK_TYPE_PICTURE:
        stash_picture(tags, m);
        break;
    default:
        break;
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
