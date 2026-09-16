/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/mp3_frame_test.c — the MP3 framing layer on its own.
 *
 * codecs/pvmp3/mp3_frame.c is the part of MP3 playback that is NOT the
 * decoder: where the audio starts and stops, how long a frame is, what the
 * Xing header says and where in the file a given second lives. All of it is
 * pure functions over bytes, so all of it can be checked here, exhaustively,
 * without a vector or a decoder — which matters because these are exactly the
 * places a hand-written parser goes wrong on a file it did not encode.
 *
 * Same mp3_frame.c the ARM build links into core.elf.
 */

#include "mp3_frame.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fails;

static void xpect(const char *what, int cond)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        g_fails++;
    }
}

/* Build a 4-byte MPEG header. ver_id/layer_id are the RAW header fields:
 * ver 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5, 1 = reserved;
 * layer 1 = Layer III, 2 = Layer II, 3 = Layer I, 0 = reserved. */
static void mk_hdr(uint8_t h[4], int ver_id, int layer_id, int br_ix,
                   int sr_ix, int pad, int mode, int crc)
{
    h[0] = 0xFFu;
    h[1] = (uint8_t)(0xE0u | (ver_id << 3) | (layer_id << 1) | (crc ? 0 : 1));
    h[2] = (uint8_t)((br_ix << 4) | (sr_ix << 2) | (pad << 1));
    h[3] = (uint8_t)(mode << 6);
}

/* ---------- header parsing ---------------------------------------------- */

static const uint16_t BR_V1[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const uint16_t BR_V2[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
};
static const uint32_t SR[4][3] = {
    { 11025, 12000, 8000 }, { 0, 0, 0 },
    { 22050, 24000, 16000 }, { 44100, 48000, 32000 },
};

static void test_headers(void)
{
    uint8_t h[4];
    mp3_header_t p;

    /* Every version x rate x bitrate x padding combination: the length must
     * match the spec formula and the derived fields must be right. This is
     * the table that decides how far the decoder advances per frame, so an
     * off-by-one anywhere in it desynchronises the whole stream. */
    int checked = 0;
    for (int ver_id = 0; ver_id <= 3; ver_id++) {
        if (ver_id == 1) continue;                     /* reserved */
        for (int sr_ix = 0; sr_ix < 3; sr_ix++) {
            for (int br_ix = 1; br_ix < 15; br_ix++) {
                for (int pad = 0; pad <= 1; pad++) {
                    mk_hdr(h, ver_id, 1, br_ix, sr_ix, pad, 0, 0);
                    if (!mp3_header_parse(h, &p)) {
                        /* Only the 1441-byte cap may reject a valid header,
                         * and nothing real exceeds it. */
                        printf("FAIL: header v%d r%d b%d p%d rejected\n",
                               ver_id, sr_ix, br_ix, pad);
                        g_fails++;
                        continue;
                    }
                    uint32_t rate = SR[ver_id][sr_ix];
                    uint32_t bps  = 1000u * (ver_id == 3 ? BR_V1[br_ix]
                                                         : BR_V2[br_ix]);
                    uint32_t want = (ver_id == 3 ? 144u : 72u) * bps / rate
                                    + (uint32_t)pad;
                    xpect("frame length matches the spec formula",
                          p.frame_bytes == want);
                    xpect("sample rate from the table", p.sample_rate == rate);
                    xpect("bitrate in bits/s", p.bitrate == bps);
                    xpect("samples per frame by version",
                          p.spf == (ver_id == 3 ? 1152 : 576));
                    checked++;
                }
            }
        }
    }
    xpect("every version/rate/bitrate/padding combination was parsed",
          checked == 2 * 3 * 14 * 3);

    mk_hdr(h, 3, 1, 9, 0, 0, 0, 0);          /* MPEG-1 128 kbps 44.1 kHz */
    xpect("MPEG-1 128k/44.1k is 417 bytes unpadded",
          mp3_header_parse(h, &p) && p.frame_bytes == 417);
    mk_hdr(h, 3, 1, 9, 0, 1, 0, 0);
    xpect("...and 418 padded", mp3_header_parse(h, &p) && p.frame_bytes == 418);
    mk_hdr(h, 3, 1, 14, 2, 1, 0, 0);         /* 320 kbps 32 kHz, padded */
    xpect("the largest Layer III frame is 1441 bytes",
          mp3_header_parse(h, &p) && p.frame_bytes == MP3_MAX_FRAME_BYTES);

    /* Channel mode. A CRC, when the frame carries one, sits between the
     * header and the side info and changes no length we compute — the Xing
     * offset ignores it by design (see mp3_side_info_bytes), so the parse must
     * be indifferent to the protection bit rather than react to it. */
    mk_hdr(h, 3, 1, 9, 0, 0, 3, 0);
    xpect("mode 3 is mono", mp3_header_parse(h, &p) && p.channels == 1);
    mk_hdr(h, 3, 1, 9, 0, 0, 1, 0);
    xpect("joint stereo is two channels",
          mp3_header_parse(h, &p) && p.channels == 2);
    {
        mp3_header_t prot, unprot;
        mk_hdr(h, 3, 1, 9, 0, 0, 0, 1);
        int a = mp3_header_parse(h, &prot);
        mk_hdr(h, 3, 1, 9, 0, 0, 0, 0);
        int b = mp3_header_parse(h, &unprot);
        xpect("the protection bit changes nothing the parse reports",
              a && b && prot.frame_bytes == unprot.frame_bytes &&
              prot.sample_rate == unprot.sample_rate);
    }

    /* Side-info size, which is where a Xing tag has to be looked for. */
    mk_hdr(h, 3, 1, 9, 0, 0, 0, 0);
    xpect("MPEG-1 stereo side info is 32 bytes",
          mp3_header_parse(h, &p) && mp3_side_info_bytes(&p) == 32);
    mk_hdr(h, 3, 1, 9, 0, 0, 3, 0);
    xpect("MPEG-1 mono side info is 17 bytes",
          mp3_header_parse(h, &p) && mp3_side_info_bytes(&p) == 17);
    mk_hdr(h, 2, 1, 9, 0, 0, 0, 0);
    xpect("MPEG-2 stereo side info is 17 bytes",
          mp3_header_parse(h, &p) && mp3_side_info_bytes(&p) == 17);
    mk_hdr(h, 2, 1, 9, 0, 0, 3, 0);
    xpect("MPEG-2 mono side info is 9 bytes",
          mp3_header_parse(h, &p) && mp3_side_info_bytes(&p) == 9);

    /* Everything that must be refused. */
    mk_hdr(h, 3, 1, 0, 0, 0, 0, 0);
    xpect("free format (bitrate index 0) is refused",
          !mp3_header_parse(h, &p));
    mk_hdr(h, 3, 1, 15, 0, 0, 0, 0);
    xpect("reserved bitrate index 15 is refused", !mp3_header_parse(h, &p));
    mk_hdr(h, 3, 1, 9, 3, 0, 0, 0);
    xpect("reserved sample-rate index 3 is refused", !mp3_header_parse(h, &p));
    mk_hdr(h, 1, 1, 9, 0, 0, 0, 0);
    xpect("reserved version id 1 is refused", !mp3_header_parse(h, &p));
    mk_hdr(h, 3, 2, 9, 0, 0, 0, 0);
    xpect("Layer II is refused", !mp3_header_parse(h, &p));
    mk_hdr(h, 3, 3, 9, 0, 0, 0, 0);
    xpect("Layer I is refused", !mp3_header_parse(h, &p));
    mk_hdr(h, 3, 0, 9, 0, 0, 0, 0);
    xpect("reserved layer 0 is refused", !mp3_header_parse(h, &p));
    mk_hdr(h, 3, 1, 9, 0, 0, 0, 0);
    h[1] = (uint8_t)(h[1] & 0xE7u);          /* break the top sync bits */
    h[0] = 0xFEu;
    xpect("a byte that is not 0xFF is not a sync word",
          !mp3_header_parse(h, &p));
    mk_hdr(h, 3, 1, 9, 0, 0, 0, 0);
    h[1] = (uint8_t)(h[1] & 0x1Fu);          /* 0xFF 0x1x: only 3 sync bits */
    xpect("an incomplete 11-bit sync word is refused",
          !mp3_header_parse(h, &p));
}

/* ---------- ID3v2 prefix -------------------------------------------------- */

/* An ID3v2 header: "ID3", major, 0, flags, sync-safe body size. */
static void mk_id3(uint8_t *p, int major, int flags, uint32_t body)
{
    p[0] = 'I'; p[1] = 'D'; p[2] = '3';
    p[3] = (uint8_t)major; p[4] = 0; p[5] = (uint8_t)flags;
    p[6] = (uint8_t)((body >> 21) & 0x7Fu);
    p[7] = (uint8_t)((body >> 14) & 0x7Fu);
    p[8] = (uint8_t)((body >> 7)  & 0x7Fu);
    p[9] = (uint8_t)( body        & 0x7Fu);
}

static void test_id3v2_prefix(void)
{
    uint8_t buf[512];
    memset(buf, 0, sizeof buf);

    xpect("no ID3v2 at all is length 0", mp3_id3v2_tag_len(buf) == 0);

    for (int major = 2; major <= 4; major++) {
        mk_id3(buf, major, 0, 100);
        xpect("a plain tag is 10 + its body", mp3_id3v2_tag_len(buf) == 110u);
    }

    /* The v2.4 footer flag adds another 10 bytes that the body size does not
     * count — miss it and the first "frame" found is the footer's own bytes. */
    mk_id3(buf, 4, 0x10, 100);
    xpect("a v2.4 footer adds 10 more", mp3_id3v2_tag_len(buf) == 120u);

    /* Sync-safe means bit 7 of every size byte is clear. A "tag" whose size
     * has one set is not a tag; trusting it would skip the wrong distance. */
    memset(buf, 0, sizeof buf);
    mk_id3(buf, 3, 0, 100);
    buf[8] = 0x80u;
    xpect("a non-sync-safe size is not an ID3v2 header",
          mp3_id3v2_tag_len(buf) == 0);

    memset(buf, 0, sizeof buf);
    mk_id3(buf, 5, 0, 100);
    xpect("an unknown major version is not an ID3v2 header",
          mp3_id3v2_tag_len(buf) == 0);

    memset(buf, 0, sizeof buf);
    mk_id3(buf, 3, 0xFFu, 100);
    buf[4] = 0xFFu;
    xpect("a revision byte of 0xFF is not an ID3v2 header",
          mp3_id3v2_tag_len(buf) == 0);
}

/* ---------- ID3v1 / APE tail --------------------------------------------- */

/* APEv2 global flags, from the spec. */
#define APE_HAS_HEADER 0x80000000u   /* a header precedes the items         */
#define APE_IS_HEADER  0x20000000u   /* this 32-byte block IS that header   */

/* A spec-correct 32-byte APEv2 footer at `f`:
 *   +0 "APETAGEX"  +8 version  +12 tag size  +16 item count  +20 flags  +24 reserved */
static void mk_ape_footer(uint8_t *f, uint32_t size, uint32_t items, uint32_t flags)
{
    memcpy(f, "APETAGEX", 8);
    const uint32_t w[4] = { 2000u, size, items, flags };
    for (int i = 0; i < 4; i++) {
        for (int b = 0; b < 4; b++) {
            f[8 + i * 4 + b] = (uint8_t)(w[i] >> (8 * b));   /* little-endian */
        }
    }
    memset(f + 24, 0, 8);
}

static void test_tail_tags(void)
{
    uint8_t tail[160];

    memset(tail, 0x5Au, sizeof tail);
    xpect("audio to the last byte has no tail tag",
          mp3_tail_tag_len(tail, sizeof tail) == 0);

    memcpy(tail + sizeof tail - 128, "TAG", 3);
    xpect("ID3v1 is 128 bytes", mp3_tail_tag_len(tail, sizeof tail) == 128u);

    /*
     * APEv2 footer, exactly as the spec lays it out and as ffmpeg and mutagen
     * write it: "APETAGEX", version, tag size, ITEM COUNT, then the global
     * flags at +20. Everything below writes the count as well as the flags,
     * so a parser that reads the flags at +16 gets the count and fails —
     * which is what this fixture used to hide.
     */
    memset(tail, 0x5Au, sizeof tail);
    uint8_t *f = tail + sizeof tail - 32;
    mk_ape_footer(f, 64, 3, 0);
    xpect("an APE tag with no header is its own size",
          mp3_tail_tag_len(tail, sizeof tail) == 64u);

    /* Bit 31, not bit 29: "a header precedes the items". A tag written by any
     * current tagger has one, so getting this wrong leaves 32 bytes of tag
     * inside the audio region of nearly every APE-tagged file. */
    mk_ape_footer(f, 64, 3, APE_HAS_HEADER);
    xpect("an APE tag with a header is 32 bytes longer",
          mp3_tail_tag_len(tail, sizeof tail) == 96u);

    /* The item count must not be mistaken for the flags. This one has 0x20000
     * items — bit 29 of nothing — and no header. */
    mk_ape_footer(f, 64, 0x20000, 0);
    xpect("the item count at +16 is not the flags",
          mp3_tail_tag_len(tail, sizeof tail) == 64u);

    /* Bit 29 says the block IS the header, so the items come AFTER it and it
     * is not the end of anything. */
    mk_ape_footer(f, 64, 3, APE_IS_HEADER | APE_HAS_HEADER);
    xpect("a block that says it is the header ends nothing",
          mp3_tail_tag_len(tail, sizeof tail) == 0);

    /* Both, which is what a Windows tagger leaves behind: the APE footer sits
     * BEFORE the ID3v1 block, not at the end of the file. */
    memset(tail, 0x5Au, sizeof tail);
    memcpy(tail + sizeof tail - 128, "TAG", 3);
    mk_ape_footer(tail + sizeof tail - 128 - 32, 64, 3, APE_HAS_HEADER);
    xpect("APE before ID3v1 is found and both are counted",
          mp3_tail_tag_len(tail, sizeof tail) == 128u + 32u + 64u);

    /* An APE tag may be far longer than the 160-byte window — only its
     * footer has to be in it — so a plausible size is reported and the CALLER
     * checks it against the file. An absurd one is not. */
    memset(tail, 0x5Au, sizeof tail);
    f = tail + sizeof tail - 32;
    mk_ape_footer(f, 65536, 3, 0);
    xpect("an APE tag bigger than the window is still reported",
          mp3_tail_tag_len(tail, sizeof tail) == 65536u);
    mk_ape_footer(f, 0x40000000u, 3, 0);
    xpect("an absurd APE size is ignored",
          mp3_tail_tag_len(tail, sizeof tail) == 0);
}

/* ---------- Xing / Info / VBRI -------------------------------------------- */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/*
 * Build a first frame carrying a Xing/Info tag. `flags` is the Xing flag word
 * (1 frames, 2 bytes, 4 TOC, 8 quality); lame != 0 appends a LAME tag with
 * the given delay/padding. Returns the frame length.
 */
static size_t mk_xing_frame(uint8_t *buf, const char *sig, uint32_t flags,
                            uint32_t frames, uint32_t bytes, const uint8_t *toc,
                            int lame, uint32_t delay, uint32_t padding)
{
    mk_hdr(buf, 3, 1, 9, 0, 0, 0, 0);          /* MPEG-1 128k 44.1k stereo */
    size_t o = 4u + 32u;                       /* past the side info */
    memcpy(buf + o, sig, 4); o += 4;
    put_be32(buf + o, flags); o += 4;
    if (flags & 1u) { put_be32(buf + o, frames); o += 4; }
    if (flags & 2u) { put_be32(buf + o, bytes);  o += 4; }
    if (flags & 4u) { memcpy(buf + o, toc, 100); o += 100; }
    if (flags & 8u) { put_be32(buf + o, 70); o += 4; }
    if (lame) {
        memcpy(buf + o, "LAME3.100", 9);
        uint8_t *g = buf + o + 21;
        g[0] = (uint8_t)(delay >> 4);
        g[1] = (uint8_t)(((delay & 0x0Fu) << 4) | ((padding >> 8) & 0x0Fu));
        g[2] = (uint8_t)(padding & 0xFFu);
        o += 36;
    }
    return o;
}

static void test_vbr_tags(void)
{
    uint8_t buf[512];
    uint8_t toc[100];
    mp3_header_t h;
    mp3_vbr_tag_t v;

    for (int i = 0; i < 100; i++) toc[i] = (uint8_t)(i * 256 / 100);

    memset(buf, 0, sizeof buf);
    size_t n = mk_xing_frame(buf, "Xing", 15u, 1000u, 300000u, toc, 1, 576, 1234);
    xpect("the frame header still parses", mp3_header_parse(buf, &h));
    xpect("a Xing tag is found", mp3_vbr_tag_parse(buf, n, &h, &v));
    xpect("Xing means variable bitrate", v.vbr == 1);
    xpect("frame count read", v.frames == 1000u);
    xpect("byte count read", v.bytes == 300000u);
    xpect("TOC read", v.have_toc && v.toc[0] == 0 && v.toc[50] == toc[50]);
    xpect("LAME encoder delay read", v.delay == 576u);
    xpect("LAME end padding read", v.padding == 1234u);

    memset(buf, 0, sizeof buf);
    n = mk_xing_frame(buf, "Info", 15u, 1000u, 300000u, toc, 1, 576, 0);
    xpect("an Info tag is found", mp3_vbr_tag_parse(buf, n, &h, &v));
    xpect("Info means constant bitrate", v.have && v.vbr == 0);

    /* Only the frame count. The LAME fields then sit 108 bytes earlier, so a
     * parser that assumed the all-flags layout would read the wrong bytes. */
    memset(buf, 0, sizeof buf);
    n = mk_xing_frame(buf, "Xing", 1u, 777u, 0, toc, 1, 1105, 42);
    xpect("a frames-only Xing is found", mp3_vbr_tag_parse(buf, n, &h, &v));
    xpect("frames-only: the count is right", v.frames == 777u);
    xpect("frames-only: no byte count", v.bytes == 0);
    xpect("frames-only: no TOC", !v.have_toc);
    xpect("frames-only: LAME fields still located",
          v.delay == 1105u && v.padding == 42u);

    /* "Lavc" and "Lavf" stamp the same extension — libavcodec when ffmpeg
     * encoded, libavformat when it only remuxed — and ffmpeg's own reader
     * honours all three strings. Missing one leaves a remuxed file's duration
     * long by the encoder delay. */
    for (int e = 0; e < 3; e++) {
        static const char *const ENC[3] = { "LAME", "Lavc", "Lavf" };
        memset(buf, 0, sizeof buf);
        n = mk_xing_frame(buf, "Xing", 15u, 1000u, 300000u, toc, 1, 777, 88);
        memcpy(buf + 4u + 32u + 8u + 4u + 4u + 100u + 4u, ENC[e], 4);
        xpect("a Xing with any of the three encoder strings is found",
              mp3_vbr_tag_parse(buf, n, &h, &v));
        xpect("...and its delay and padding are read",
              v.delay == 777u && v.padding == 88u);
    }
    memset(buf, 0, sizeof buf);
    n = mk_xing_frame(buf, "Xing", 15u, 1000u, 300000u, toc, 1, 777, 88);
    memcpy(buf + 4u + 32u + 8u + 4u + 4u + 100u + 4u, "Xxxx", 4);
    xpect("an unknown encoder string is not read as the extension",
          mp3_vbr_tag_parse(buf, n, &h, &v) && v.delay == 0 && v.padding == 0);

    /* No LAME extension at all: delay and padding must stay zero rather than
     * pick up whatever follows the tag. */
    memset(buf, 0, sizeof buf);
    n = mk_xing_frame(buf, "Xing", 3u, 500u, 1000u, toc, 0, 0, 0);
    xpect("a Xing with no LAME tag is found", mp3_vbr_tag_parse(buf, n, &h, &v));
    xpect("no LAME tag leaves delay/padding zero",
          v.delay == 0 && v.padding == 0);

    /* Truncated: the tag claims fields the buffer does not hold. */
    memset(buf, 0, sizeof buf);
    n = mk_xing_frame(buf, "Xing", 15u, 1000u, 300000u, toc, 1, 576, 0);
    xpect("a truncated Xing reports what it could read",
          mp3_vbr_tag_parse(buf, 4u + 32u + 12u, &h, &v) && v.frames == 1000u &&
          !v.have_toc);

    /* No tag: a normal first frame must not be mistaken for one. */
    memset(buf, 0, sizeof buf);
    mk_hdr(buf, 3, 1, 9, 0, 0, 0, 0);
    xpect("an ordinary frame carries no Xing",
          !mp3_vbr_tag_parse(buf, 417u, &h, &v));

    /* VBRI: Fraunhofer's, at a fixed 32 bytes past the header. */
    memset(buf, 0, sizeof buf);
    mk_hdr(buf, 3, 1, 9, 0, 0, 0, 0);
    memcpy(buf + 36, "VBRI", 4);
    put_be32(buf + 36 + 10, 123456u);          /* bytes  */
    put_be32(buf + 36 + 14, 321u);             /* frames */
    xpect("a VBRI header is found", mp3_vbr_tag_parse(buf, 417u, &h, &v));
    xpect("VBRI frame and byte counts read",
          v.frames == 321u && v.bytes == 123456u && v.vbr == 1);
}

/* ---------- streaming scan + seek map ------------------------------------ */

typedef struct { const uint8_t *p; size_t len, pos; } mem_t;

static size_t mem_read(void *ud, void *buf, size_t n)
{
    mem_t *m = (mem_t *)ud;
    size_t left = m->len - m->pos;
    if (n > left) n = left;
    memcpy(buf, m->p + m->pos, n);
    m->pos += n;
    return n;
}
static int mem_seek(void *ud, int off, int origin)
{
    mem_t *m = (mem_t *)ud;
    long base = (origin == DECODER_SEEK_SET) ? 0
              : (origin == DECODER_SEEK_END) ? (long)m->len : (long)m->pos;
    long np = base + off;
    if (np < 0 || (size_t)np > m->len) return 0;
    m->pos = (size_t)np;
    return 1;
}
static int64_t mem_tell(void *ud) { return (int64_t)((mem_t *)ud)->pos; }

/* A synthetic CBR file: `id3` bytes of ID3v2, optional junk, `n` frames of
 * 128 kbps 44.1 kHz stereo, optional ID3v1. Frame bodies are zero, which is
 * fine — nothing here decodes them. */
static size_t mk_file(uint8_t *buf, uint32_t id3_body, size_t junk,
                      int frames, int id3v1)
{
    size_t o = 0;
    if (id3_body) {
        mk_id3(buf, 3, 0, id3_body);
        memset(buf + 10, 0, id3_body);
        o = 10u + id3_body;
    }
    /* Junk that LOOKS like a sync word but is not a frame: a lone 0xFF 0xFB
     * with a reserved sample-rate index. Two-header validation is what has to
     * step over it. */
    for (size_t i = 0; i < junk; i++) {
        buf[o + i] = (i % 3 == 0) ? 0xFFu : 0xFBu;
    }
    o += junk;
    for (int i = 0; i < frames; i++) {
        mk_hdr(buf + o, 3, 1, 9, 0, 0, 0, 0);
        memset(buf + o + 4, 0, 413);
        o += 417;
    }
    if (id3v1) {
        memcpy(buf + o, "TAG", 3);
        memset(buf + o + 3, 0, 125);
        o += 128;
    }
    return o;
}

static void test_scan(void)
{
    static uint8_t buf[64 * 1024];
    mp3_stream_info_t si;

    memset(buf, 0, sizeof buf);
    size_t n = mk_file(buf, 500, 0, 40, 0);
    mem_t m = { buf, n, 0 };
    decoder_source_t src = { mem_read, mem_seek, mem_tell, &m };
    xpect("a plain CBR file scans", mp3_stream_scan(&src, &si) == 0);
    xpect("audio starts past the ID3v2 tag", si.audio_start == 510u);
    xpect("audio ends at the file end", si.audio_end == n);
    xpect("the first header was read", si.first.sample_rate == 44100u &&
                                       si.first.channels == 2u);
    xpect("no Xing tag in a bare stream",
          !si.vbr.have && si.audio_start == si.seek_start);
    /* 40 frames x 417 bytes at 128 kbps: the CBR estimate is exact. */
    xpect("the CBR duration estimate is the frame count",
          si.total_frames == (uint64_t)(40u * 417u) * 8u * 44100u / 128000u);
    xpect("a CBR stream has nothing to trim, so raw == total",
          si.raw_frames == si.total_frames);
    /*
     * And with no Xing count that length is EXTRAPOLATED from the first
     * frame's bitrate, so dividing the byte span by the frame count it implies
     * gives the first frame's length straight back, to within the truncation.
     * This is why the seek back-off (codecs/pvmp3/mp3.c) is a constant and not
     * a per-file "mean": on an untagged file that quotient carries no
     * information the first frame did not already have, and an untagged VBR
     * file is exactly the one whose first frame is unrepresentative.
     */
    {
        uint64_t frames = si.raw_frames / si.first.spf;
        uint64_t mean   = frames ? si.audio_bytes / frames : 0u;
        uint64_t diff   = (mean > si.first.frame_bytes)
                        ? mean - si.first.frame_bytes
                        : si.first.frame_bytes - mean;
        xpect("an untagged file's byte-span-over-frames is its first frame's "
              "length restated, not an independent measurement",
              frames > 0u && diff * 20u < si.first.frame_bytes);
    }
    xpect("the source is left at audio_start", m.pos == si.audio_start);

    /* Chained tags: a tagger that appends rather than rewrites leaves two,
     * and BOTH have to be stepped over or the second one's bytes are decoded
     * as audio. */
    memset(buf, 0, sizeof buf);
    mk_id3(buf, 3, 0, 500);
    mk_id3(buf + 510, 4, 0, 300);
    {
        size_t o = 510u + 310u;
        for (int i = 0; i < 40; i++) {
            mk_hdr(buf + o + (size_t)i * 417u, 3, 1, 9, 0, 0, 0, 0);
        }
        n = o + 40u * 417u;
        m = (mem_t){ buf, n, 0 };
        xpect("a file with two chained ID3v2 tags scans",
              mp3_stream_scan(&src, &si) == 0);
        xpect("...and audio starts past BOTH of them", si.audio_start == o);
    }

    /* Junk before the first frame. One 0xFF byte is not a frame; the scan has
     * to check that a candidate's own length lands on another header. */
    memset(buf, 0, sizeof buf);
    n = mk_file(buf, 0, 1000, 40, 0);
    m = (mem_t){ buf, n, 0 };
    xpect("a junk prefix is walked past", mp3_stream_scan(&src, &si) == 0);
    xpect("...to the real first frame", si.audio_start == 1000u);

    /* ID3v1 at the tail must not be decoded as audio. */
    memset(buf, 0, sizeof buf);
    n = mk_file(buf, 0, 0, 40, 1);
    m = (mem_t){ buf, n, 0 };
    xpect("a file with an ID3v1 tail scans", mp3_stream_scan(&src, &si) == 0);
    xpect("audio_end excludes the ID3v1 block",
          si.audio_end == n - 128u && si.audio_end == 40u * 417u);

    /* A Xing frame: it is silence carrying the tag, so decoding starts AFTER
     * it, while the TOC still measures from it. */
    memset(buf, 0, sizeof buf);
    uint8_t toc[100];
    for (int i = 0; i < 100; i++) toc[i] = (uint8_t)(i * 256 / 100);
    size_t xlen = mk_xing_frame(buf, "Xing", 15u, 40u, 41u * 417u, toc,
                                1, 576, 288);
    (void)xlen;
    memset(buf + 417, 0, sizeof buf - 417);
    for (int i = 1; i <= 40; i++) {
        mk_hdr(buf + (size_t)i * 417u, 3, 1, 9, 0, 0, 0, 0);
    }
    n = 41u * 417u;
    m = (mem_t){ buf, n, 0 };
    xpect("a Xing file scans", mp3_stream_scan(&src, &si) == 0);
    xpect("the Xing frame is stepped over", si.audio_start == 417u);
    xpect("...but the seek map still starts at it", si.seek_start == 0u);
    xpect("duration is frames x spf less delay and padding",
          si.total_frames == 40ull * 1152ull - 576ull - 288ull);
    xpect("the seek map still covers the untrimmed stream",
          si.raw_frames == 40ull * 1152ull);

    /* The seek map. Frame 0 is exact; the rest ride the TOC. */
    xpect("seeking to 0 lands on the first audio frame",
          mp3_seek_offset(&si, 0) == si.audio_start);
    uint64_t mid = mp3_seek_offset(&si, si.total_frames / 2u);
    xpect("a mid-file seek lands inside the audio region",
          mid > si.audio_start && mid < si.audio_end);
    xpect("a linear TOC maps the midpoint to the middle of the bytes",
          mid > si.seek_start + si.audio_bytes / 2u - si.audio_bytes / 50u &&
          mid < si.seek_start + si.audio_bytes / 2u + si.audio_bytes / 50u);
    xpect("a seek past the end is clamped inside the file",
          mp3_seek_offset(&si, si.total_frames * 10u) < si.audio_end);
    /* Not an MP3 at all. */
    memset(buf, 0x00, 4096);
    m = (mem_t){ buf, 4096, 0 };
    xpect("a file with no frame in it is refused",
          mp3_stream_scan(&src, &si) == -1);

    /* One frame, nothing after it: the second-header check has to accept EOF
     * rather than reject the only frame in the file. */
    memset(buf, 0, sizeof buf);
    n = mk_file(buf, 0, 0, 1, 0);
    m = (mem_t){ buf, n, 0 };
    xpect("a single-frame file scans", mp3_stream_scan(&src, &si) == 0);
    xpect("...starting at byte 0", si.audio_start == 0u);
}

int main(void)
{
    test_headers();
    test_id3v2_prefix();
    test_tail_tags();
    test_vbr_tags();
    test_scan();

    if (g_fails) {
        printf("mp3_frame_test: %d check%s failed\n",
               g_fails, g_fails == 1 ? "" : "s");
        return 1;
    }
    printf("ok: mp3_frame (headers, ID3/APE, Xing/VBRI, scan, seek map)\n");
    return 0;
}
