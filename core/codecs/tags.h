/*
 * core/codecs/tags.h — common audio-metadata struct used by per-codec
 * tag readers (tag_flac.h, tag_mp3.h).
 *
 * Each codec has its own metadata format (Vorbis comments in FLAC,
 * ID3v2 in MP3, MP4 atoms in AAC, etc), but downstream consumers (the
 * NP screen, the indexer) only care about a small common subset:
 * title, artist, album, embedded picture. This struct is what every
 * reader fills.
 *
 * The reader's job is to translate the codec-specific text encoding
 * into UTF-8. Truncation at TAG_FIELD_MAX-1 bytes happens at the byte
 * boundary; we don't currently parse code points to truncate cleanly.
 *
 * Picture data is heap-owned: tag readers heap-copy the source bytes
 * (FLAC PICTURE block / ID3v2 APIC frame) so callers can use them
 * after the source buffer is freed. Callers MUST call audio_tags_free
 * to release art_bytes when done — or transfer ownership by setting
 * `art_bytes = NULL` after moving the pointer.
 */

#ifndef CORE_CODECS_TAGS_H
#define CORE_CODECS_TAGS_H

#include <stddef.h>
#include <stdlib.h>

/* Match NP_TITLE_MAX / NP_ARTIST_MAX in apps/ui/now_playing.h so the
 * NP screen can copy without a second-stage truncation. */
#define TAG_FIELD_MAX 64

typedef struct {
    char title [TAG_FIELD_MAX];
    char artist[TAG_FIELD_MAX];
    char album [TAG_FIELD_MAX];
    int  found_title;
    int  found_artist;
    int  found_album;

    /* Embedded picture (album art) — owned heap allocation; NULL when
     * the file has no embedded art. art_len is meaningless when
     * art_bytes is NULL. */
    void  *art_bytes;
    size_t art_len;
    int    found_art;
} audio_tags_t;

/*
 * Release any heap-owned fields on `tags` (currently just art_bytes).
 * After the call, the struct is safe to reuse — pointers are NULLed.
 * NULL-safe: passing a NULL `tags` is a no-op.
 *
 * Inline so we don't need a tiny tags_lib compile target. If this
 * grows past a few lines, promote to its own .c.
 */
static inline void audio_tags_free(audio_tags_t *tags) {
    if (!tags) return;
    free(tags->art_bytes);
    tags->art_bytes = NULL;
    tags->art_len   = 0;
    tags->found_art = 0;
}

#endif /* CORE_CODECS_TAGS_H */
