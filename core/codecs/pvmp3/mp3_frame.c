/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/pvmp3/mp3_frame.c — MPEG framing, tags, Xing/VBRI, seek math.
 * See mp3_frame.h for what each function is for.
 */

#include "mp3_frame.h"

/* Bitrate tables, kbps, indexed by the header's 4-bit bitrate index.
 * Index 0 is free format and 15 is reserved; both are rejected, so the
 * entries are 0 and never read. Layer III only. */
static const uint16_t BITRATE_V1[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const uint16_t BITRATE_V2[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
};

/* Sample rate by [version][index]; index 3 is reserved and rejected. */
static const uint16_t SAMPLE_RATE[3][4] = {
    { 11025, 12000,  8000, 0 },   /* MP3_MPEG_25 */
    { 22050, 24000, 16000, 0 },   /* MP3_MPEG_2  */
    { 44100, 48000, 32000, 0 },   /* MP3_MPEG_1  */
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8)  |  (uint32_t)p[0];
}

static int tag_is(const uint8_t *p, const char *sig)
{
    for (int i = 0; sig[i]; i++) {
        if (p[i] != (uint8_t)sig[i]) return 0;
    }
    return 1;
}

/* ---------- frame header ------------------------------------------------ */

int mp3_header_parse(const uint8_t h[4], mp3_header_t *out)
{
    if (h[0] != 0xFFu || (h[1] & 0xE0u) != 0xE0u) return 0;   /* 11-bit sync */

    unsigned version_id = (h[1] >> 3) & 3u;   /* 1 = reserved */
    unsigned layer_id   = (h[1] >> 1) & 3u;   /* 1 = Layer III */
    unsigned bitrate_ix = (unsigned)h[2] >> 4;
    unsigned rate_ix    = ((unsigned)h[2] >> 2) & 3u;
    unsigned mode       = (unsigned)h[3] >> 6;

    if (version_id == 1u) return 0;           /* reserved version */
    if (layer_id != 1u)   return 0;           /* Layer I/II: not ours */
    if (bitrate_ix == 0u || bitrate_ix == 15u) return 0;  /* free / reserved */
    if (rate_ix == 3u)    return 0;           /* reserved sample rate */

    /* The header counts versions 0=2.5, 2=2, 3=1; MP3_MPEG_* counts them in
     * ascending sample rate so the rate table indexes directly. */
    unsigned version = (version_id == 0u) ? (unsigned)MP3_MPEG_25
                     : (version_id == 2u) ? (unsigned)MP3_MPEG_2
                                          : (unsigned)MP3_MPEG_1;

    uint32_t rate    = SAMPLE_RATE[version][rate_ix];
    uint32_t bitrate = 1000u * (uint32_t)((version == MP3_MPEG_1)
                                          ? BITRATE_V1[bitrate_ix]
                                          : BITRATE_V2[bitrate_ix]);
    unsigned padding = ((unsigned)h[2] >> 1) & 1u;

    /* A Layer III frame carries 1152 samples on MPEG-1 and 576 on the half
     * rate versions, so the byte length is (samples/8) * bitrate / rate. */
    uint32_t coeff = (version == MP3_MPEG_1) ? 144u : 72u;

    out->version     = (uint8_t)version;
    out->sample_rate = rate;
    out->bitrate     = bitrate;
    out->spf         = (version == MP3_MPEG_1) ? 1152u : 576u;
    out->channels    = (mode == 3u) ? 1u : 2u;     /* 3 = MPG_MD_MONO */
    out->frame_bytes = coeff * bitrate / rate + padding;
    return (out->frame_bytes >= 4u && out->frame_bytes <= MP3_MAX_FRAME_BYTES);
}

int mp3_side_info_bytes(const mp3_header_t *h)
{
    if (h->version == MP3_MPEG_1) {
        return (h->channels == 1) ? 17 : 32;
    }
    return (h->channels == 1) ? 9 : 17;
}

/* Two headers agree when they describe the same stream — the check that tells
 * a real frame from a byte pair that merely looks like a sync word. */
static int headers_agree(const mp3_header_t *a, const mp3_header_t *b)
{
    return a->version == b->version && a->sample_rate == b->sample_rate &&
           a->channels == b->channels;
}

/* ---------- ID3v2 / ID3v1 / APE ----------------------------------------- */

uint32_t mp3_id3v2_tag_len(const uint8_t p[10])
{
    if (!tag_is(p, "ID3")) return 0;
    if (p[3] < 2u || p[3] > 4u || p[4] == 0xFFu) return 0;
    /* The size is four SYNC-SAFE bytes: bit 7 of each is always 0, so a
     * would-be sync word can never appear inside it. Anything else means
     * this is not really a tag header. */
    if ((p[6] | p[7] | p[8] | p[9]) & 0x80u) return 0;
    uint32_t size = ((uint32_t)p[6] << 21) | ((uint32_t)p[7] << 14) |
                    ((uint32_t)p[8] << 7)  |  (uint32_t)p[9];
    uint32_t footer = (p[5] & 0x10u) ? 10u : 0u;   /* v2.4 footer */
    return 10u + size + footer;
}

/* No real APE tag comes close; a declared size past this is corruption. */
#define MP3_MAX_TAIL_TAG (16u * 1024u * 1024u)

uint32_t mp3_tail_tag_len(const uint8_t *tail, size_t len)
{
    uint32_t total = 0;

    /* ID3v1 is a fixed 128-byte record; "TAG" is its whole magic. Guard
     * against "TAG" being the start of an ID3v1 EXTENDED tag ("TAG+", 227
     * bytes) sitting just before it — we only need the audio bound, and
     * treating the extension as audio simply leaves 227 junk bytes that the
     * frame walk resyncs past at EOF. */
    if (len >= 128u && tag_is(tail + len - 128u, "TAG")) {
        total = 128u;
    }

    /*
     * An APE tag's FOOTER is the last 32 bytes of the tag (so before any
     * ID3v1 block, not at the end of the file):
     *
     *   +0   "APETAGEX"
     *   +8   version, 4 LE (2000 for APEv2)
     *   +12  tag size, 4 LE — the items AND this footer, but NOT the header
     *   +16  item count, 4 LE
     *   +20  global flags, 4 LE — bit 31 "a header precedes the items",
     *        bit 30 "no footer", bit 29 "this block IS the header"
     *   +24  8 reserved
     *
     * The flags are at +20 and the header bit is 31. Reading them at +16
     * reads the item count instead, and bit 29 is never set on a footer, so
     * a tag with a header was under-counted by exactly those 32 bytes and
     * they were left inside the audio region.
     *
     * The tag itself may be longer than `tail` — only its footer has to be in
     * the window — so the length is sanity-capped here and it is the CALLER
     * that checks it against the real file size.
     */
    if (len - total >= 32u) {
        const uint8_t *f = tail + len - total - 32u;
        if (tag_is(f, "APETAGEX")) {
            uint32_t size  = le32(f + 12);
            uint32_t flags = le32(f + 20);
            /* Bit 29 says this block is the tag's HEADER. Finding one here
             * means the items follow it, so it is not the end of anything and
             * there is nothing to subtract. */
            if (!(flags & 0x20000000u)) {
                uint32_t whole = size + ((flags & 0x80000000u) ? 32u : 0u);
                if (whole >= 32u && whole <= MP3_MAX_TAIL_TAG) total += whole;
            }
        }
    }
    return total;
}

/* ---------- Xing / Info / VBRI ------------------------------------------ */

int mp3_vbr_tag_parse(const uint8_t *frame, size_t len,
                      const mp3_header_t *h, mp3_vbr_tag_t *out)
{
    for (size_t i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;

    /* Xing/Info sits right after the side information, measured from the end
     * of the 4-byte header and ignoring the CRC — LAME's placement, and what
     * ffmpeg reads, so our duration matches ffprobe's. */
    size_t off = 4u + (size_t)mp3_side_info_bytes(h);
    if (off + 8u <= len && (tag_is(frame + off, "Xing") ||
                            tag_is(frame + off, "Info"))) {
        out->have = 1;
        out->vbr  = tag_is(frame + off, "Xing") ? 1u : 0u;
        uint32_t flags = be32(frame + off + 4);
        size_t   p     = off + 8u;
        if (flags & 1u) {                          /* frame count */
            if (p + 4u > len) return 1;
            out->frames = be32(frame + p);
            p += 4u;
        }
        if (flags & 2u) {                          /* byte count */
            if (p + 4u > len) return 1;
            out->bytes = be32(frame + p);
            p += 4u;
        }
        if (flags & 4u) {                          /* 100-entry TOC */
            if (p + 100u > len) return 1;
            for (int i = 0; i < 100; i++) out->toc[i] = frame[p + (size_t)i];
            out->have_toc = 1;
            p += 100u;
        }
        if (flags & 8u) {                          /* quality indicator */
            if (p + 4u > len) return 1;
            p += 4u;
        }
        /* The LAME extension: a 9-byte encoder string, then at +21 two packed
         * 12-bit fields — the encoder delay and the end padding that ffprobe
         * subtracts from the duration. Three writers stamp it and ffmpeg's
         * own reader honours all three, so we do too: "LAME" itself, "Lavc"
         * when libavcodec encoded, and "Lavf" when libavformat only remuxed. */
        if (p + 24u <= len &&
            (tag_is(frame + p, "LAME") || tag_is(frame + p, "Lavc") ||
             tag_is(frame + p, "Lavf"))) {
            const uint8_t *g = frame + p + 21u;
            out->delay   = (uint16_t)(((uint32_t)g[0] << 4) | (g[1] >> 4));
            out->padding = (uint16_t)((((uint32_t)g[1] & 0x0Fu) << 8) | g[2]);
        }
        return 1;
    }

    /* VBRI (Fraunhofer) is at a fixed 32 bytes past the header instead. */
    off = 4u + 32u;
    if (off + 26u <= len && tag_is(frame + off, "VBRI")) {
        out->have   = 1;
        out->vbr    = 1;
        out->bytes  = be32(frame + off + 10);
        out->frames = be32(frame + off + 14);
        /* VBRI carries its own variable-width TOC. v1 uses the counts only;
         * seeks fall back to proportional-by-byte, which is what the same
         * file gets from any decoder that ignores it. */
        return 1;
    }
    return 0;
}

/* ---------- streaming scan ---------------------------------------------- */

/* How far past audio_start we hunt for the first frame before giving up. A
 * file whose first 64 KB hold no valid frame is not one we can play, and the
 * bound is what keeps a garbage file from turning open() into a whole-file
 * read over PIO. */
#define MP3_SCAN_LIMIT 65536u

/* Enough of the first frame to hold a header, the widest side info and a
 * whole Xing/Info + LAME tag (4 + 32 + 8 + 4 + 4 + 100 + 4 + 24 = 180). */
#define MP3_SCAN_WINDOW 256u

static int src_seek_set(decoder_source_t *src, uint64_t pos)
{
    /* decoder_source_t.seek takes an int offset; every source in the tree
     * (readahead, diskbuf, fat_src) treats SET as absolute, and a file we can
     * address is under 2 GB, so the cast is safe for anything we open. */
    if (pos > 0x7FFFFFFFu) return 0;
    return src->seek(src->userdata, (int)pos, DECODER_SEEK_SET);
}

static size_t src_read_at(decoder_source_t *src, uint64_t pos,
                          uint8_t *buf, size_t want)
{
    if (!src_seek_set(src, pos)) return 0;
    return src->read(src->userdata, buf, want);
}

int mp3_stream_scan(decoder_source_t *src, mp3_stream_info_t *out)
{
    for (size_t i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;
    if (!src || !src->read || !src->seek || !src->tell) return -1;

    /* File size: the sources all honour SEEK_END (dr_flac needs it too). */
    if (!src->seek(src->userdata, 0, DECODER_SEEK_END)) return -1;
    int64_t sz = src->tell(src->userdata);
    if (sz <= 4) return -1;
    uint64_t file_size = (uint64_t)sz;

    /* ID3v2 prefix. Walked 10 bytes at a time so a 1 MB embedded cover costs
     * one header read, not a read of the art. */
    uint64_t start = 0;
    uint8_t  hdr[10];
    while (start + 10u <= file_size) {
        if (src_read_at(src, start, hdr, sizeof hdr) != sizeof hdr) break;
        uint32_t n = mp3_id3v2_tag_len(hdr);
        if (n == 0u || file_size - start < n) break;
        start += n;
    }

    /* Tail tags bound the audio on the other side. */
    uint8_t tail[160];
    size_t  tail_want = (file_size - start < sizeof tail)
                      ? (size_t)(file_size - start) : sizeof tail;
    uint64_t end = file_size;
    if (tail_want >= 32u) {
        size_t got = src_read_at(src, file_size - tail_want, tail, tail_want);
        if (got == tail_want) {
            uint32_t t = mp3_tail_tag_len(tail, tail_want);
            if ((uint64_t)t < file_size - start) end = file_size - t;
        }
    }

    /* First frame: a header whose own length lands on a header describing the
     * same stream. One sync word is worth nothing — ID3 text, cover art and
     * APE values all contain 0xFF bytes. */
    uint8_t  win[MP3_SCAN_WINDOW];
    uint64_t pos   = start;
    uint64_t limit = start + MP3_SCAN_LIMIT;
    if (limit > end) limit = end;
    mp3_header_t fh;
    int found = 0;
    while (!found && pos + 4u <= limit) {
        size_t want = (size_t)((end - pos < MP3_SCAN_WINDOW)
                               ? (end - pos) : MP3_SCAN_WINDOW);
        size_t got  = src_read_at(src, pos, win, want);
        if (got < 4u) break;
        for (size_t i = 0; i + 4u <= got && pos + i + 4u <= limit; i++) {
            if (win[i] != 0xFFu) continue;
            if (!mp3_header_parse(win + i, &fh)) continue;
            uint64_t nxt = pos + i + fh.frame_bytes;
            if (nxt + 4u > end) {                   /* single-frame file */
                pos += i;
                found = 1;
                break;
            }
            uint8_t nh[4];
            mp3_header_t h2;
            if (src_read_at(src, nxt, nh, 4u) == 4u &&
                mp3_header_parse(nh, &h2) && headers_agree(&fh, &h2)) {
                pos += i;
                found = 1;
                break;
            }
        }
        if (found) break;
        /* Overlap by 3 so a header straddling the window boundary is seen. */
        pos += (uint64_t)got - 3u;
    }
    if (!found) return -1;

    out->first       = fh;
    out->audio_start = pos;
    out->seek_start  = pos;
    out->audio_end   = end;

    /* Xing/Info/VBRI lives in that first frame. */
    size_t fwant = (size_t)((end - pos < MP3_SCAN_WINDOW)
                            ? (end - pos) : MP3_SCAN_WINDOW);
    size_t fgot  = src_read_at(src, pos, win, fwant);
    if (fgot >= 8u && mp3_vbr_tag_parse(win, fgot, &fh, &out->vbr)) {
        /* That frame is pure silence carrying the tag. Decoding starts after
         * it; the TOC still measures from it (seek_start), because its byte
         * count covers the whole MPEG stream including itself. */
        out->audio_start = pos + fh.frame_bytes;
        if (out->audio_start > end) out->audio_start = end;
    }

    /* The byte span the seek math maps the duration over. */
    out->audio_bytes = (out->vbr.bytes > 0u &&
                        (uint64_t)out->vbr.bytes <= end - out->seek_start)
                     ? (uint64_t)out->vbr.bytes
                     : (end - out->seek_start);

    /*
     * Duration. With a Xing/VBRI frame count it is exact: frames x samples
     * per frame, less the encoder delay and end padding, which is precisely
     * ffprobe's rule and therefore what the host index stamped. Without one,
     * the CBR estimate from the bitrate — exact for CBR, approximate for a
     * VBR file that lost its tag, which beats a progress bar that reads 0.
     */
    if (out->vbr.have && out->vbr.frames > 0u) {
        out->raw_frames   = (uint64_t)out->vbr.frames * fh.spf;
        uint64_t trim     = (uint64_t)out->vbr.delay + out->vbr.padding;
        out->total_frames = (out->raw_frames > trim) ? out->raw_frames - trim
                                                     : 0u;
    } else if (fh.bitrate > 0u) {
        out->total_frames = (out->audio_bytes * 8u * fh.sample_rate) / fh.bitrate;
        out->raw_frames   = out->total_frames;   /* nothing to trim */
    }

    /* Leave the source where the decoder wants to start reading. */
    if (!src_seek_set(src, out->audio_start)) return -1;
    return 0;
}

/* ---------- seek math ---------------------------------------------------- */

uint64_t mp3_seek_offset(const mp3_stream_info_t *si, uint64_t target_frame)
{
    uint64_t span = si->audio_bytes;
    uint64_t off  = si->audio_start;

    /* Frame 0 is the top of the track, which is an exact answer the TOC's
     * 1 %-of-the-file granularity would only approximate. */
    if (target_frame == 0u) return si->audio_start;

    if (si->raw_frames > 0u && span > 0u) {
        /*
         * The caller counts from the first sample of the TRACK; the bytes
         * hold the encoder's delay in front of it. Shift into the stream's
         * own timeline before mapping, or every seek lands early by a share
         * of the delay that grows across the file.
         */
        uint64_t pos = target_frame + si->vbr.delay;
        if (pos >= si->raw_frames) pos = si->raw_frames - 1u;

        if (si->vbr.have_toc) {
            /*
             * The TOC is 100 entries of "at this percent of the DURATION, the
             * stream is this many 1/256ths of the way through the BYTES".
             * Interpolate inside the bucket so a scrub inside one percent of
             * a long track still moves.
             */
            uint64_t num = pos * 100u;
            uint32_t i   = (uint32_t)(num / si->raw_frames);
            if (i > 99u) i = 99u;
            uint64_t rem = num - (uint64_t)i * si->raw_frames;
            uint32_t a   = si->vbr.toc[i];
            uint32_t b   = (i < 99u) ? si->vbr.toc[i + 1u] : 256u;
            if (b < a) b = a;                   /* a non-monotonic TOC happens */
            /* Position in 1/65536ths of the audio region. */
            uint64_t f16 = ((uint64_t)a << 8) +
                           ((uint64_t)(b - a) * rem * 256u) / si->raw_frames;
            off = si->seek_start + (span * f16) / 65536u;
        } else {
            off = si->seek_start + (span * pos) / si->raw_frames;
        }
    }

    if (off < si->audio_start) off = si->audio_start;
    if (si->audio_end > 4u && off > si->audio_end - 4u) {
        off = si->audio_end - 4u;
    }
    return off;
}
