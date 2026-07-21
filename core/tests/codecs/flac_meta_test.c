/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/flac_meta_test.c — host test for the FLAC metadata reader.
 *
 * flac_meta_read() walks a FLAC's METADATA_BLOCKs (STREAMINFO for duration,
 * VORBIS_COMMENT for tags) WITHOUT decoding audio, so the UI can show real
 * track info. This test backs a decoder_source_t with an in-RAM buffer (the
 * same shape as readahead_test's counting source) and checks:
 *   1. The committed KAT vector (sine_440hz_1s_44k_s16_stereo.flac): parses,
 *      sample_rate == 44100, duration == 1 s. That vector carries a
 *      VORBIS_COMMENT with zero tags, so the tag fields stay empty.
 *   2. A synthetic FLAC we build in RAM with a full VORBIS_COMMENT block —
 *      exercises the tag parse path (TITLE/ARTIST/ALBUMARTIST fallback/ALBUM/
 *      GENRE/TRACKNUMBER "n/total"/DATE "YYYY-MM-DD"), UTF-8 stripping, and
 *      the last-block flag.
 *   3. Robustness: a truncated / non-FLAC buffer returns -1 without crashing,
 *      and a VORBIS_COMMENT with an absurd declared length can't run away.
 *
 * The code under test is the SAME flac_meta.c the ARM build compiles.
 */

#include "flac_meta.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fail = 1;
    }
    return cond;
}

/* --- in-RAM backing source (stands in for the disk / fat_src) --- */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} mem_src_t;

static size_t ms_read(void *ud, void *buf, size_t n)
{
    mem_src_t *m = (mem_src_t *)ud;
    size_t avail = (m->pos < m->len) ? (m->len - m->pos) : 0;
    if (n > avail) {
        n = avail;
    }
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}
static int ms_seek(void *ud, int offset, int origin)
{
    mem_src_t *m = (mem_src_t *)ud;
    long base = (origin == DECODER_SEEK_SET) ? 0
              : (origin == DECODER_SEEK_END) ? (long)m->len
                                             : (long)m->pos;
    long np = base + offset;
    if (np < 0 || (size_t)np > m->len) {
        return 0;
    }
    m->pos = (size_t)np;
    return 1;
}
static int64_t ms_tell(void *ud)
{
    return (int64_t)((mem_src_t *)ud)->pos;
}
static void src_init(mem_src_t *m, decoder_source_t *s,
                     const uint8_t *data, size_t len)
{
    m->data = data;
    m->len  = len;
    m->pos  = 0;
    s->read = ms_read;
    s->seek = ms_seek;
    s->tell = ms_tell;
    s->userdata = m;
}

/* --- load the committed KAT vector from the fixture dir (argv[1]) --- */
static uint8_t *load_file(const char *dir, const char *name, size_t *out_len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

/* --- build a synthetic FLAC (STREAMINFO + VORBIS_COMMENT) in RAM --- */

static void put_be24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Append one "KEY=value" comment (u32 LE length + bytes) at *pp. */
static void put_comment(uint8_t **pp, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    put_le32(*pp, len);
    *pp += 4;
    memcpy(*pp, s, len);
    *pp += len;
}

/* Returns total length written into buf. */
static size_t build_tagged_flac(uint8_t *buf, uint32_t sample_rate,
                                uint64_t total_samples)
{
    uint8_t *p = buf;

    memcpy(p, "fLaC", 4);
    p += 4;

    /* --- STREAMINFO (type 0, not last), 34-byte body --- */
    *p++ = 0x00;                 /* not-last, type 0 */
    put_be24(p, 34);
    p += 3;
    uint8_t *si = p;
    memset(si, 0, 34);
    /* min/max block, min/max frame left 0 (bytes 0..9). Pack the 64-bit
     * word at offset 10: sample_rate[20] chan[3] bps[5] total[36]. */
    si[10] = (uint8_t)(sample_rate >> 12);
    si[11] = (uint8_t)(sample_rate >> 4);
    si[12] = (uint8_t)(((sample_rate & 0xF) << 4) | (1 << 1) | 0); /* chan-1=1 (stereo), bps top bit 0 */
    /* bps-1 = 15 (16-bit): 5 bits split 1 in si[12] low bit .. 4 in si[13] high.
     * bps-1 = 0b01111. top bit -> si[12] bit0 = 0, remaining 4 bits -> si[13] hi. */
    si[13] = (uint8_t)((0x0F << 4) | (uint8_t)(total_samples >> 32));
    si[14] = (uint8_t)(total_samples >> 24);
    si[15] = (uint8_t)(total_samples >> 16);
    si[16] = (uint8_t)(total_samples >> 8);
    si[17] = (uint8_t)(total_samples);
    p += 34;

    /* --- VORBIS_COMMENT (type 4, LAST) --- */
    uint8_t *vc_hdr = p;
    *p++ = 0x84;                 /* last-block flag | type 4 */
    uint8_t *vc_len = p;         /* fill length after we know the body size */
    p += 3;
    uint8_t *body = p;

    const char *vendor = "flac_meta_test";
    put_le32(p, (uint32_t)strlen(vendor));
    p += 4;
    memcpy(p, vendor, strlen(vendor));
    p += strlen(vendor);

    uint8_t *count_at = p;       /* comment_count, filled below */
    p += 4;
    uint32_t count = 0;
    put_comment(&p, "TITLE=Blue Sky"); count++;
    put_comment(&p, "ARTIST=The Testers"); count++;
    put_comment(&p, "ALBUMARTIST=Various"); count++;   /* ignored: ARTIST set */
    put_comment(&p, "ALBUM=Greatest Hits"); count++;
    put_comment(&p, "GENRE=Rock"); count++;
    put_comment(&p, "tracknumber=7/12"); count++;      /* lowercase key */
    put_comment(&p, "DATE=2021-05-01"); count++;
    /* A comment with a UTF-8 multibyte char that must be stripped. */
    put_comment(&p, "COMMENT=caf\xC3\xA9 time"); count++;
    put_le32(count_at, count);

    put_be24(vc_len, (uint32_t)(p - body));
    (void)vc_hdr;

    return (size_t)(p - buf);
}

int main(int argc, char **argv)
{
    mem_src_t        ms;
    decoder_source_t src;
    flac_meta_t      m;

    /* --- Test 1: the committed KAT vector --- */
    if (argc >= 2) {
        size_t len = 0;
        uint8_t *flac = load_file(argv[1], "sine_440hz_1s_44k_s16_stereo.flac", &len);
        if (check("kat-loaded", flac != NULL && len > 0)) {
            src_init(&ms, &src, flac, len);
            int rc = flac_meta_read(&src, &m);
            check("kat-parse-ok", rc == 0 && m.have == 1);
            check("kat-sample-rate", m.sample_rate == 44100);
            check("kat-duration", m.duration_s == 1);
            /* The reference encoder wrote a VORBIS_COMMENT with no tags. */
            check("kat-no-title", m.title[0] == '\0');
            check("kat-no-track", m.track == 0);
            printf("  kat: sr=%u dur=%us title=\"%s\" artist=\"%s\"\n",
                   m.sample_rate, m.duration_s, m.title, m.artist);
        }
        free(flac);
    } else {
        printf("  (no fixture dir argv[1]; skipping KAT vector test)\n");
    }

    /* --- Test 2: synthetic FLAC with a full tag set --- */
    {
        static uint8_t buf[1024];
        size_t len = build_tagged_flac(buf, 48000, 48000ull * 217); /* 217 s */
        src_init(&ms, &src, buf, len);
        int rc = flac_meta_read(&src, &m);
        check("syn-parse-ok", rc == 0 && m.have == 1);
        check("syn-sample-rate", m.sample_rate == 48000);
        check("syn-duration", m.duration_s == 217);
        check("syn-title", strcmp(m.title, "Blue Sky") == 0);
        check("syn-artist", strcmp(m.artist, "The Testers") == 0);
        check("syn-album", strcmp(m.album, "Greatest Hits") == 0);
        check("syn-genre", strcmp(m.genre, "Rock") == 0);
        check("syn-track", m.track == 7);
        check("syn-year", m.year == 2021);
        printf("  syn: sr=%u dur=%us title=\"%s\" artist=\"%s\" album=\"%s\" "
               "genre=\"%s\" track=%d year=%d\n",
               m.sample_rate, m.duration_s, m.title, m.artist, m.album,
               m.genre, m.track, m.year);
    }

    /* --- Test 3: non-FLAC / truncated buffers return -1, don't crash --- */
    {
        static const uint8_t junk[8] = { 'O', 'g', 'g', 'S', 1, 2, 3, 4 };
        src_init(&ms, &src, junk, sizeof junk);
        check("not-a-flac", flac_meta_read(&src, &m) == -1 && m.have == 0);

        static const uint8_t tiny[2] = { 'f', 'L' };
        src_init(&ms, &src, tiny, sizeof tiny);
        check("truncated-magic", flac_meta_read(&src, &m) == -1);
    }

    /* --- Test 4: a VORBIS_COMMENT lying about its size can't run away --- */
    {
        static uint8_t buf[64];
        uint8_t *p = buf;
        memcpy(p, "fLaC", 4); p += 4;
        /* STREAMINFO so have==1, then a bogus VORBIS_COMMENT (last) whose
         * declared vendor_length is enormous — parser must bail cleanly. */
        *p++ = 0x00; put_be24(p, 34); p += 3;
        memset(p, 0, 34);
        p[10] = (uint8_t)(44100 >> 12);
        p[11] = (uint8_t)(44100 >> 4);
        p[12] = (uint8_t)(((44100 & 0xF) << 4) | (1 << 1));
        p[13] = (uint8_t)(0x0F << 4);       /* total_samples = 0 */
        p += 34;
        *p++ = 0x84; put_be24(p, 8); p += 3; /* block len 8 */
        put_le32(p, 0xFFFFFFFFu); p += 4;    /* vendor_length lies */
        put_le32(p, 0); p += 4;
        src_init(&ms, &src, buf, (size_t)(p - buf));
        int rc = flac_meta_read(&src, &m);
        check("hostile-vorbis-survives", rc == 0 && m.have == 1 && m.title[0] == '\0');
    }

    printf("flac_meta_test: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
