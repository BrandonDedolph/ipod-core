/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/pvmp3/id3_meta.c — ID3v2 tag + duration reader (see id3_meta.h).
 *
 * Cleanroom implementation from the published ID3v2.2/2.3/2.4 informal
 * standards. Layout:
 *
 *   header, 10 bytes: "ID3", major, revision, flags, size (4 sync-safe bytes,
 *                     counting the frames and any padding, not the header)
 *     flags: bit7 unsynchronisation, bit6 extended header, bit4 footer (2.4)
 *   [extended header, skipped by its own size]
 *   frames, until the size runs out or an all-zero frame id (padding) starts:
 *     v2.2: id[3] size[3 big-endian]
 *     v2.3: id[4] size[4 big-endian]        flags[2]
 *     v2.4: id[4] size[4 sync-safe]         flags[2]
 *   [footer, a mirror of the header, already counted by mp3_frame.c]
 *
 * A text frame's body is one encoding byte then the text: 0 ISO-8859-1,
 * 1 UTF-16 with a BOM, 2 UTF-16BE, 3 UTF-8. v2.4 allows several NUL-separated
 * values in one frame; we take the first, which is what every tagger writes
 * for these fields and what ffprobe reports.
 *
 * Everything is bounded: the walk stops at the declared tag size, and
 * separately once it has READ ID3_MAX_TAG_BYTES out of the tag — a body it
 * skips is a seek, not a read, so an embedded cover costs nothing and the
 * text behind it is still found. A frame longer than ID3_FRAME_CAP is read up
 * to the cap and the rest skipped, and every read is length-checked. A
 * malformed tag yields empty fields, never a runaway.
 */

#include "id3_meta.h"

#include "mp3_frame.h"
#include "../meta_text.h"

enum {
    /* How many bytes of a tag we will READ. A frame we skip costs a seek, not
     * a read, so a tag whose bulk is an embedded cover walks past it for free
     * and this cap never comes near — while a corrupt sync-safe size, or a
     * tag that really is megabytes of text, still cannot walk the whole disk.
     * Capping the DECLARED size instead would have thrown away the title of
     * any file whose art happened to sit in front of it. */
    ID3_MAX_TAG_BYTES = 1u << 20,
    /* Bytes of any one frame body we pull in. 256 bytes is 128 UTF-16 code
     * units — four times the longest field we keep. */
    ID3_FRAME_CAP     = 256,
    /* UTF-8 scratch a decoded frame lands in before the display-field copy. */
    ID3_TEXT_CAP      = 192,
};

/* ID3v1 genre table (Eric Kemp's original 0-79 + the Winamp extension to
 * 191), which TCON's numeric forms index into. */
static const char *const ID3V1_GENRES[] = {
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
#define ID3V1_GENRE_COUNT ((uint32_t)(sizeof ID3V1_GENRES / sizeof ID3V1_GENRES[0]))

/* -------- source helpers (same shape as flac_meta.c's) ------------------- */

static int read_full(decoder_source_t *src, void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < n) {
        size_t got = src->read(src->userdata, p + done, n - done);
        if (got == 0) return 0;
        done += (uint32_t)got;
    }
    return 1;
}

static int skip_bytes(decoder_source_t *src, uint32_t n)
{
    if (n == 0) return 1;
    if (src->seek && src->seek(src->userdata, (int)n, DECODER_SEEK_CUR)) return 1;
    uint8_t tmp[128];
    while (n > 0) {
        uint32_t chunk = (n < sizeof tmp) ? n : (uint32_t)sizeof tmp;
        if (!read_full(src, tmp, chunk)) return 0;
        n -= chunk;
    }
    return 1;
}

/* -------- small text helpers -------------------------------------------- */

static uint32_t cstr_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* A frame id: `idlen` bytes at `id` against a NUL-terminated name. */
static int fid_is(const uint8_t *id, uint32_t idlen, const char *name)
{
    for (uint32_t i = 0; i < idlen; i++) {
        if (name[i] == '\0' || id[i] != (uint8_t)name[i]) return 0;
    }
    return name[idlen] == '\0';
}

/* One UTF-8 encoding of a code point into dst (4 bytes of room needed).
 * Returns the bytes written, 0 when the code point is not encodable. */
static uint32_t utf8_put(uint8_t *dst, uint32_t cp)
{
    if (cp < 0x80u) {
        dst[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800u) {
        dst[0] = (uint8_t)(0xC0u | (cp >> 6));
        dst[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;   /* lone surrogate */
        dst[0] = (uint8_t)(0xE0u | (cp >> 12));
        dst[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        dst[0] = (uint8_t)(0xF0u | (cp >> 18));
        dst[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        dst[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

/*
 * Decode a text frame body (encoding byte already consumed: `enc` is it,
 * src[0..len) is the text) into UTF-8 in dst[0..cap). Returns the byte count.
 * Stops at the value terminator, so a multi-value v2.4 frame yields its first
 * value.
 */
static uint32_t decode_text(uint8_t enc, const uint8_t *src, uint32_t len,
                            uint8_t *dst, uint32_t cap)
{
    uint32_t o = 0;

    if (enc == 0u) {                      /* ISO-8859-1 -> UTF-8 */
        for (uint32_t i = 0; i < len && o + 4u <= cap; i++) {
            if (src[i] == 0u) break;
            o += utf8_put(dst + o, src[i]);
        }
        return o;
    }
    if (enc == 3u) {                      /* already UTF-8 */
        for (uint32_t i = 0; i < len && o < cap; i++) {
            if (src[i] == 0u) break;
            dst[o++] = src[i];
        }
        return o;
    }
    if (enc != 1u && enc != 2u) return 0; /* not an encoding we know */

    /* UTF-16. enc 1 carries a BOM, enc 2 is big-endian with none. */
    int      big = (enc == 2u);
    uint32_t i   = 0;
    if (enc == 1u && len >= 2u) {
        if (src[0] == 0xFFu && src[1] == 0xFEu)      { big = 0; i = 2u; }
        else if (src[0] == 0xFEu && src[1] == 0xFFu) { big = 1; i = 2u; }
        /* A missing BOM is malformed; UTF-16LE is what the taggers that get
         * this wrong produce, so assume it rather than drop the frame. */
    }
    while (i + 2u <= len && o + 4u <= cap) {
        uint32_t u = big ? ((uint32_t)src[i] << 8 | src[i + 1u])
                         : ((uint32_t)src[i + 1u] << 8 | src[i]);
        i += 2u;
        if (u == 0u) break;                            /* value terminator */
        if (u >= 0xD800u && u <= 0xDBFFu) {            /* surrogate pair */
            if (i + 2u > len) break;
            uint32_t lo = big ? ((uint32_t)src[i] << 8 | src[i + 1u])
                              : ((uint32_t)src[i + 1u] << 8 | src[i]);
            if (lo < 0xDC00u || lo > 0xDFFFu) break;   /* unpaired: stop */
            i += 2u;
            u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
        }
        o += utf8_put(dst + o, u);
    }
    return o;
}

/*
 * The genre text a TCON body means: "(RX)" is Remix, "(CR)" is Cover, "(N)"
 * and a bare "N" index the ID3v1 table, and real text after a code wins over
 * the code ("(17)Progressive Rock"). A number the table does not have stays
 * literal. Returns the run to copy — into `s` or a table entry, never a copy.
 */
static const uint8_t *tcon_resolve(const uint8_t *s, uint32_t n, uint32_t *plen)
{
    *plen = n;
    if (n < 2u || s[0] != '(' || s[1] == '(') {
        /* A bare number is a genre index too; anything else is the text. */
        uint32_t i = 0, num = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && num < 100000u) {
            num = num * 10u + (uint32_t)(s[i] - '0');
            i++;
        }
        if (i > 0u && i == n && num < ID3V1_GENRE_COUNT) {
            const char *g = ID3V1_GENRES[num];
            *plen = cstr_len(g);
            return (const uint8_t *)g;
        }
        return s;
    }

    uint32_t    i    = 1u;
    const char *word = 0;
    if (n >= 4u && s[1] == 'R' && s[2] == 'X' && s[3] == ')') {
        word = "Remix";
        i = 4u;
    } else if (n >= 4u && s[1] == 'C' && s[2] == 'R' && s[3] == ')') {
        word = "Cover";
        i = 4u;
    } else {
        uint32_t num = 0;
        int      any = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && num < 100000u) {
            num = num * 10u + (uint32_t)(s[i] - '0');
            any = 1;
            i++;
        }
        if (!any || i >= n || s[i] != ')') return s;    /* not a code at all */
        i++;
        if (num < ID3V1_GENRE_COUNT) word = ID3V1_GENRES[num];
    }
    if (i < n) {                                 /* "(17)Rock" -> "Rock" */
        *plen = n - i;
        return s + i;
    }
    if (word) {
        *plen = cstr_len(word);
        return (const uint8_t *)word;
    }
    return s;                                    /* "(255)" stays literal */
}

/* -------- the frame walk ------------------------------------------------- */

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7Fu) << 21) | ((uint32_t)(p[1] & 0x7Fu) << 14) |
           ((uint32_t)(p[2] & 0x7Fu) << 7)  |  (uint32_t)(p[3] & 0x7Fu);
}

/* Stash one decoded text frame in the field it belongs to. */
static void store(flac_meta_t *out, const uint8_t *id, uint32_t idlen,
                  const uint8_t *txt, uint32_t len)
{
    if (len == 0u) return;

    if (fid_is(id, idlen, "TIT2") || fid_is(id, idlen, "TT2")) {
        meta_copy_printable(out->title, sizeof out->title, txt, len);
    } else if (fid_is(id, idlen, "TPE1") || fid_is(id, idlen, "TP1")) {
        meta_copy_printable(out->artist, sizeof out->artist, txt, len);
    } else if (fid_is(id, idlen, "TPE2") || fid_is(id, idlen, "TP2")) {
        /* Album artist is the fallback only — TPE1 wins if it ever shows up,
         * in either order, exactly as flac_meta.c prefers ARTIST. */
        if (out->artist[0] == '\0') {
            meta_copy_printable(out->artist, sizeof out->artist, txt, len);
        }
    } else if (fid_is(id, idlen, "TALB") || fid_is(id, idlen, "TAL")) {
        meta_copy_printable(out->album, sizeof out->album, txt, len);
    } else if (fid_is(id, idlen, "TCON") || fid_is(id, idlen, "TCO")) {
        uint32_t glen = 0;
        const uint8_t *g = tcon_resolve(txt, len, &glen);
        meta_copy_printable(out->genre, sizeof out->genre, g, glen);
    } else if (fid_is(id, idlen, "TRCK") || fid_is(id, idlen, "TRK")) {
        out->track = meta_parse_leading_int(txt, len);   /* "7/12" -> 7 */
    } else if (fid_is(id, idlen, "TDRC") || fid_is(id, idlen, "TYER") ||
               fid_is(id, idlen, "TYE")) {
        int y = meta_parse_leading_int(txt, len);        /* "2021-05-01" */
        if (y >= 1000 && y <= 9999) out->year = y;
    }
}

/*
 * One frame's body: read what we care about, skip the rest. Returns the bytes
 * it READ (0 for a frame skipped whole), or -1 when the source ran out.
 */
static int32_t take_frame(decoder_source_t *src, flac_meta_t *out,
                          const uint8_t *id, uint32_t idlen, uint32_t size)
{
    /* "T???" is the text-frame family. TXXX/TXX is the odd one out: its body
     * is a description AND a value, so it does not decode like the rest — and
     * the only thing in it we might want (ReplayGain) is out of scope. */
    if (id[0] != 'T' || fid_is(id, idlen, "TXXX") || fid_is(id, idlen, "TXX")) {
        return skip_bytes(src, size) ? 0 : -1;
    }
    uint32_t want = (size < ID3_FRAME_CAP) ? size : (uint32_t)ID3_FRAME_CAP;
    uint8_t  body[ID3_FRAME_CAP];
    if (!read_full(src, body, want)) return -1;
    if (want >= 2u) {
        uint8_t  utf8[ID3_TEXT_CAP];
        uint32_t n = decode_text(body[0], body + 1, want - 1u,
                                 utf8, sizeof utf8);
        store(out, id, idlen, utf8, n);
    }
    if (!skip_bytes(src, size - want)) return -1;
    return (int32_t)want;
}

/*
 * Walk the ID3v2 tag at the current source position. Returns nothing: a tag
 * that is absent, truncated or malformed simply leaves the fields empty.
 */
static void parse_id3v2(decoder_source_t *src, flac_meta_t *out)
{
    uint8_t h[10];
    if (!read_full(src, h, sizeof h)) return;
    if (h[0] != 'I' || h[1] != 'D' || h[2] != '3') return;
    if (h[3] < 2u || h[3] > 4u || h[4] == 0xFFu) return;
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80u) return;

    uint8_t  major = h[3];
    uint8_t  flags = h[5];
    uint32_t left  = syncsafe32(h + 6);
    /* What is left to READ, as opposed to `left`, which is what is left of the
     * tag. Only frame headers and the bodies we keep spend it. */
    uint32_t budget = ID3_MAX_TAG_BYTES;

    /*
     * Unsynchronisation rewrites every 0xFF 0x00 pair inside the tag, so the
     * frame sizes no longer describe the bytes on disk. Reversing it would
     * mean buffering the whole tag; the frames are not worth that, so the tag
     * is treated as opaque and the track falls back to its filename. Rare:
     * taggers stopped setting this once decoders learned to sync properly.
     */
    if (flags & 0x80u) return;

    if (flags & 0x40u) {                    /* extended header */
        uint8_t e[4];
        if (left < 4u || !read_full(src, e, 4u)) return;
        left -= 4u;
        /* v2.3 states the size of what FOLLOWS the field; v2.4 states a
         * sync-safe size that INCLUDES it. */
        uint32_t ext = (major >= 4u) ? syncsafe32(e) : be32(e);
        if (major >= 4u) ext = (ext >= 4u) ? ext - 4u : 0u;
        if (ext > left) return;
        if (!skip_bytes(src, ext)) return;
        left -= ext;
    }

    uint32_t idlen  = (major == 2u) ? 3u : 4u;
    uint32_t hdrlen = (major == 2u) ? 6u : 10u;

    while (left >= hdrlen && budget >= hdrlen) {
        uint8_t fh[10];
        if (!read_full(src, fh, hdrlen)) return;
        left   -= hdrlen;
        budget -= hdrlen;
        if (fh[0] == 0u) return;            /* padding: the frames are done */

        uint32_t size;
        uint16_t fflags = 0;
        if (major == 2u) {
            size = be24(fh + 3);
        } else {
            size   = (major >= 4u) ? syncsafe32(fh + 4) : be32(fh + 4);
            fflags = (uint16_t)(((uint16_t)fh[8] << 8) | fh[9]);
        }
        if (size == 0u || size > left) return;

        /* Compressed (0x0080 in v2.3 / 0x0008 in v2.4), encrypted (0x0040 /
         * 0x0004) or per-frame unsynchronised (v2.4, 0x0002) bodies are not
         * the bytes the size describes; skip them whole. */
        uint16_t opaque = (major >= 4u) ? 0x000Cu : 0x00C0u;
        if (major >= 4u) opaque |= 0x0002u;
        if (fflags & opaque) {
            if (!skip_bytes(src, size)) return;
            left -= size;
            continue;
        }
        /* v2.4 data-length indicator: four extra bytes in front of the body. */
        if (major >= 4u && (fflags & 0x0001u)) {
            if (size <= 4u || !skip_bytes(src, 4u)) return;
            size -= 4u;
            left -= 4u;
        }

        int32_t got = take_frame(src, out, fh, idlen, size);
        if (got < 0) return;
        left   -= size;
        budget -= (budget > (uint32_t)got) ? (uint32_t)got : budget;
    }
}

/* -------- entry point ---------------------------------------------------- */

int id3_meta_read(decoder_source_t *src, flac_meta_t *out)
{
    for (uint32_t i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;
    if (!src || !src->read || !src->seek || !src->tell) return -1;

    /* The tag is a prefix, so this is one forward pass from byte 0 — the
     * shape the 32 KB read-ahead above us is built for. */
    if (src->seek(src->userdata, 0, DECODER_SEEK_SET)) {
        parse_id3v2(src, out);
    }

    /* Then the stream itself, for the rate and the duration. The scan seeks
     * on its own, so wherever the tag walk stopped is irrelevant. */
    mp3_stream_info_t si;
    if (mp3_stream_scan(src, &si) != 0) {
        for (uint32_t i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;
        return -1;                          /* not an MP3 we can play */
    }
    out->have        = 1;
    out->sample_rate = si.first.sample_rate;
    out->duration_s  = (si.first.sample_rate > 0u)
                     ? (uint32_t)(si.total_frames / si.first.sample_rate) : 0u;
    return 0;
}
