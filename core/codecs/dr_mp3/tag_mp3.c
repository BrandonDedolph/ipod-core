/*
 * core/codecs/dr_mp3/tag_mp3.c — minimal ID3v2 parser.
 *
 * Layout:
 *   [10-byte header]
 *     "ID3", v[2], v[1] = 0,
 *     flags (1 byte; we ignore),
 *     size (4 bytes synchsafe — total frame-data size, excluding header)
 *   [optional extended header, if flags bit 6 set; we skip via its own size]
 *   [frames]
 *     id   = 4 ASCII bytes
 *     size = 4 bytes (v2.3 = regular BE; v2.4 = synchsafe)
 *     flags = 2 bytes
 *     content = `size` bytes
 *
 * We extract five text frames + one picture frame:
 *   TIT2 → title
 *   TPE1 → artist
 *   TALB → album
 *   TCON → genre
 *   TCOM → composer
 *   APIC → embedded picture (album art)
 *
 * Text frames are encoded with a 1-byte prefix:
 *   0 = ISO-8859-1, 1 = UTF-16 with BOM, 2 = UTF-16BE, 3 = UTF-8 (v2.4)
 *
 * UTF-16 is downconverted by dropping the high byte of each code unit
 * (good for ASCII-in-UTF-16, returns '?' for code points >= 0x80) —
 * see header for rationale.
 *
 * APIC body layout:
 *   1 byte  text-encoding (for description)
 *   N bytes MIME type, null-terminated ASCII (e.g. "image/jpeg")
 *   1 byte  picture type (0x03 = front cover)
 *   N bytes description, null-terminated (encoding above)
 *   ...    picture data, runs to end of frame
 *
 * Robustness contract: any malformed header or out-of-bounds size is
 * treated as "no tags found", not as an error. The audio stream is
 * usually still playable; we just won't show metadata for it.
 */

#include "tag_mp3.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------- helpers ---------------------------------------------------- */

/* ID3v1 genre table (Eric Kemp's original 0-79 + Winamp extension to 191). */
static const char *id3v1_genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
    "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
    "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
    "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
    "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
    "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
    "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
    "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
    "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
    "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion",
    "Bebob", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
    "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock",
    "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour",
    "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony",
    "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club",
    "Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul",
    "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House",
    "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror",
    "Indie", "BritPop", "Negerpunk", "Polsk Punk", "Beat", "Christian Gangsta",
    "Heavy Metal", "Black Metal", "Crossover", "Contemporary Christian",
    "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop",
    "Synthpop", "Abstract", "Art Rock", "Baroque", "Bhangra", "Big Beat",
    "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM", "Eclectic", "Electro",
    "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM",
    "Illbient", "Industro-Goth", "Jam Band", "Krautrock", "Leftfield",
    "Lounge", "Math Rock", "New Romantic", "Nu-Breakz", "Post-Punk",
    "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock",
    "World Music", "Neoclassical", "Audiobook", "Audio Theatre",
    "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep",
    "Garage Rock", "Psybient",
};
#define ID3V1_GENRE_COUNT (sizeof(id3v1_genres) / sizeof(id3v1_genres[0]))

/* Resolve TCON: "(N)"/"N" -> id3v1_genres[N], "(RX)"=Remix, "(CR)"=Cover, "(N)Text" prefers Text. */
static void resolve_tcon_genre(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) return;
    dst[0] = 0;
    if (!src || !src[0]) return;

    if (src[0] == '(' && src[1] != '(') {
        size_t i = 1;
        if (src[i] == 'R' && src[i + 1] == 'X' && src[i + 2] == ')') {
            const char *tail = src + i + 3;
            if (*tail) { strncpy(dst, tail, dst_size - 1); dst[dst_size - 1] = 0; return; }
            strncpy(dst, "Remix", dst_size - 1); dst[dst_size - 1] = 0; return;
        }
        if (src[i] == 'C' && src[i + 1] == 'R' && src[i + 2] == ')') {
            const char *tail = src + i + 3;
            if (*tail) { strncpy(dst, tail, dst_size - 1); dst[dst_size - 1] = 0; return; }
            strncpy(dst, "Cover", dst_size - 1); dst[dst_size - 1] = 0; return;
        }
        unsigned n = 0;
        int have = 0;
        while (src[i] >= '0' && src[i] <= '9') { n = n * 10 + (unsigned)(src[i] - '0'); have = 1; i++; }
        if (have && src[i] == ')') {
            const char *tail = src + i + 1;
            if (*tail) { strncpy(dst, tail, dst_size - 1); dst[dst_size - 1] = 0; return; }
            if (n < ID3V1_GENRE_COUNT) {
                strncpy(dst, id3v1_genres[n], dst_size - 1); dst[dst_size - 1] = 0; return;
            }
        }
        strncpy(dst, src, dst_size - 1); dst[dst_size - 1] = 0; return;
    }

    {
        size_t i = 0;
        unsigned n = 0;
        while (src[i] >= '0' && src[i] <= '9') { n = n * 10 + (unsigned)(src[i] - '0'); i++; }
        if (i > 0 && src[i] == 0 && n < ID3V1_GENRE_COUNT) {
            strncpy(dst, id3v1_genres[n], dst_size - 1); dst[dst_size - 1] = 0; return;
        }
    }

    strncpy(dst, src, dst_size - 1); dst[dst_size - 1] = 0;
}

/* Synchsafe 32-bit integer: each byte contributes 7 bits. */
static uint32_t read_synchsafe(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) |
           ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) <<  7) |
           ((uint32_t)(p[3] & 0x7f));
}

/* Plain big-endian 32-bit integer (used for frame sizes in v2.3). */
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/*
 * Convert a text frame's content to UTF-8 in `dst` (null-terminated,
 * truncated at dst_size-1 bytes). `body` points to the text-encoding
 * byte; `body_len` covers the encoding byte plus the text payload.
 */
static void copy_text_frame(char *dst, size_t dst_size,
                            const uint8_t *body, size_t body_len) {
    if (dst_size == 0) return;
    dst[0] = 0;
    if (body_len < 1) return;

    uint8_t enc = body[0];
    const uint8_t *text = body + 1;
    size_t text_len = body_len - 1;

    if (enc == 0 || enc == 3) {
        /* ISO-8859-1 (treat bytes < 0x80 as ASCII; bytes >= 0x80
         * passed through as Latin-1, which renders incorrectly for
         * the BMP-plane atlas but harmless) OR UTF-8 (already
         * UTF-8). Same code path; differ only in semantics. */
        size_t copy = text_len < dst_size - 1 ? text_len : dst_size - 1;
        size_t out = 0;
        for (size_t i = 0; i < copy; i++) {
            if (text[i] == 0) break;          /* tag end */
            dst[out++] = (char)text[i];
        }
        dst[out] = 0;
    } else if (enc == 1 || enc == 2) {
        /* UTF-16. enc==1 has a BOM at the start; enc==2 is UTF-16BE
         * with no BOM. Detect endianness from BOM if present, else
         * assume BE. Downconvert by dropping the high byte; non-ASCII
         * code points become '?' to avoid garbage. */
        size_t off = 0;
        int big_endian = (enc == 2);
        if (enc == 1 && text_len >= 2) {
            if (text[0] == 0xff && text[1] == 0xfe) { big_endian = 0; off = 2; }
            else if (text[0] == 0xfe && text[1] == 0xff) { big_endian = 1; off = 2; }
            /* No BOM: leave big_endian default (BE), don't consume any. */
        }
        size_t out = 0;
        while (off + 1 < text_len && out < dst_size - 1) {
            uint8_t hi = big_endian ? text[off]     : text[off + 1];
            uint8_t lo = big_endian ? text[off + 1] : text[off];
            uint16_t cu = ((uint16_t)hi << 8) | lo;
            if (cu == 0) break;               /* tag end */
            dst[out++] = (cu < 0x80) ? (char)cu : '?';
            off += 2;
        }
        dst[out] = 0;
    } else {
        /* Unknown encoding — leave dst empty. */
    }
}

/* ---------- APIC (album art) ------------------------------------------ */

/* ID3v2 picture type for "Cover (front)". */
#define APIC_TYPE_FRONT_COVER 0x03

/*
 * Walk a UTF-16 null-terminated string starting at body[off..body_len).
 * Returns the offset just past the trailing 0x0000 code unit, or 0 if
 * no terminator was found. Used to skip the APIC description field
 * when it's UTF-16-encoded.
 */
static size_t skip_utf16_terminated(const uint8_t *body, size_t off, size_t body_len) {
    while (off + 1 < body_len) {
        if (body[off] == 0 && body[off + 1] == 0) return off + 2;
        off += 2;
    }
    return 0;
}

/* Same for null-terminated single-byte strings (Latin-1 / UTF-8). */
static size_t skip_byte_terminated(const uint8_t *body, size_t off, size_t body_len) {
    while (off < body_len) {
        if (body[off] == 0) return off + 1;
        off++;
    }
    return 0;
}

/*
 * Limitation: v2.4 frame-flag bits for compression / encryption /
 * data-length-indicator are not handled. The parser sees the raw body
 * after our generic header read; if the frame is compressed it will
 * misparse silently (no crash because every read stays within fsize).
 * Same posture as the text-frame path. Few real-world MP3s use these
 * flags on APIC; revisit when one shows up.
 */
static void stash_picture_apic(audio_tags_t *tags,
                               const uint8_t *fbody, size_t fsize) {
    if (fsize < 3) return;            /* enc + at least one MIME byte + null */

    uint8_t enc = fbody[0];
    /* Description is encoded per `enc`; MIME is always Latin-1 per spec. */

    /* MIME type — null-terminated single-byte string starting at off=1. */
    size_t off = skip_byte_terminated(fbody, 1, fsize);
    if (off == 0 || off >= fsize) return;

    /* Picture type. */
    uint8_t pic_type = fbody[off];
    off++;
    if (off >= fsize) return;

    /* Description (size depends on enc). UTF-16 needs a 0x0000 unit
     * terminator; Latin-1/UTF-8 just a 0x00 byte. */
    if (enc == 1 || enc == 2) {
        off = skip_utf16_terminated(fbody, off, fsize);
    } else {
        off = skip_byte_terminated(fbody, off, fsize);
    }
    if (off == 0 || off > fsize) return;

    /* Picture data: from `off` to end of frame. */
    size_t pic_len = fsize - off;
    if (pic_len == 0) return;

    /* Same front-cover-preferred logic as tag_flac.c:
     *   found_art == 0 → nothing stashed (take any)
     *   found_art == 1 → non-front stashed (override only with front)
     *   found_art == 2 → front stashed (don't override) */
    if (tags->found_art == 2) return;
    if (tags->found_art == 1 && pic_type != APIC_TYPE_FRONT_COVER) return;

    void *copy = malloc(pic_len);
    if (!copy) return;
    memcpy(copy, fbody + off, pic_len);

    free(tags->art_bytes);
    tags->art_bytes = copy;
    tags->art_len   = pic_len;
    tags->found_art = (pic_type == APIC_TYPE_FRONT_COVER) ? 2 : 1;
}

/* ---------- frame iteration ------------------------------------------- */

static int matches(const uint8_t *id4, const char *want4) {
    return id4[0] == (uint8_t)want4[0] &&
           id4[1] == (uint8_t)want4[1] &&
           id4[2] == (uint8_t)want4[2] &&
           id4[3] == (uint8_t)want4[3];
}

int tag_mp3_read(const void *bytes, size_t len, audio_tags_t *out) {
    if (!bytes || len == 0 || !out) return -1;
    memset(out, 0, sizeof(*out));

    const uint8_t *p = (const uint8_t *)bytes;

    /* Need at least the 10-byte header. */
    if (len < 10) return 0;
    if (p[0] != 'I' || p[1] != 'D' || p[2] != '3') return 0;

    uint8_t major = p[3];
    /* uint8_t minor = p[4]; — unused, but versions 0/0xff are reserved/invalid. */
    uint8_t flags = p[5];
    uint32_t tag_size = read_synchsafe(&p[6]);

    /* The header itself is 10 bytes; tag_size is the size of the body
     * (extended header + frames + padding) that follows it. */
    if ((uint64_t)10 + tag_size > len) return 0;     /* truncated */

    /* Only versions 2.3 and 2.4 are common. v2.2 had 3-byte frame
     * IDs and is rare; treat as no tags rather than mis-parse. */
    if (major != 3 && major != 4) return 0;

    const uint8_t *body     = p + 10;
    const uint8_t *body_end = body + tag_size;

    /* Skip extended header if flags bit 6 set. Layout differs slightly
     * between v2.3 and v2.4, but in both the first 4 bytes of the
     * extended header are its own size field — synchsafe in v2.4,
     * regular BE in v2.3. */
    if ((flags & 0x40) && body + 4 <= body_end) {
        uint32_t ext_size = (major == 4) ? read_synchsafe(body) : read_be32(body);
        /* In v2.3 the ext_size excludes its own 4 bytes; in v2.4 it
         * includes them. Both interpretations leave us pointing at
         * the first frame after this advance. */
        size_t skip = (major == 4) ? ext_size : (4 + ext_size);
        if (skip > (size_t)(body_end - body)) return 0;
        body += skip;
    }

    /* Iterate frames. */
    while (body + 10 <= body_end) {
        const uint8_t *id     = body;
        uint32_t       fsize  = (major == 4) ? read_synchsafe(body + 4)
                                             : read_be32(body + 4);
        /* uint16_t fflags = (body[8] << 8) | body[9]; — unused */
        const uint8_t *fbody  = body + 10;

        /* A zero ID byte signals padding — we're done. */
        if (id[0] == 0) break;

        /* Frame must fit in the remaining body. Compare via the
         * size_t form rather than `fbody + fsize > body_end` so a
         * 32-bit target can't wrap on a maliciously huge fsize. */
        if (fsize > (size_t)(body_end - fbody)) break;

        if (matches(id, "TIT2") && !out->found_title) {
            copy_text_frame(out->title, sizeof(out->title), fbody, fsize);
            out->found_title = (out->title[0] != 0);
        } else if (matches(id, "TPE1") && !out->found_artist) {
            copy_text_frame(out->artist, sizeof(out->artist), fbody, fsize);
            out->found_artist = (out->artist[0] != 0);
        } else if (matches(id, "TALB") && !out->found_album) {
            copy_text_frame(out->album, sizeof(out->album), fbody, fsize);
            out->found_album = (out->album[0] != 0);
        } else if (matches(id, "TCON") && !out->found_genre) {
            char raw[TAG_FIELD_MAX];
            copy_text_frame(raw, sizeof(raw), fbody, fsize);
            resolve_tcon_genre(out->genre, sizeof(out->genre), raw);
            out->found_genre = (out->genre[0] != 0);
        } else if (matches(id, "TCOM") && !out->found_composer) {
            copy_text_frame(out->composer, sizeof(out->composer), fbody, fsize);
            out->found_composer = (out->composer[0] != 0);
        } else if (matches(id, "APIC")) {
            stash_picture_apic(out, fbody, fsize);
        }

        body = fbody + fsize;
    }
    return 0;
}
