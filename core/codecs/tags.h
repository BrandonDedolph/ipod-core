/*
 * core/codecs/tags.h — common audio-metadata struct used by per-codec
 * tag readers (tag_flac.h, tag_mp3.h).
 *
 * Each codec has its own metadata format (Vorbis comments in FLAC,
 * ID3v2 in MP3, MP4 atoms in AAC, etc), but downstream consumers (the
 * NP screen, the indexer) only care about a small common subset:
 * title, artist, album. This struct is what every reader fills.
 *
 * The reader's job is to translate the codec-specific encoding into
 * UTF-8. Truncation at TAG_FIELD_MAX-1 bytes happens at the byte
 * boundary; we don't currently parse code points to truncate cleanly.
 */

#ifndef CORE_CODECS_TAGS_H
#define CORE_CODECS_TAGS_H

#include <stddef.h>

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
} audio_tags_t;

#endif /* CORE_CODECS_TAGS_H */
