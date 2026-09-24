/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/flac_rg_chain_test.c — the post-decoder chain, end to end, with
 * ReplayGain in it.
 *
 * Every codec suite before this one decoded an UNTAGGED fixture and stopped at
 * the decoder's output. Two things were therefore never tested:
 *
 *   1. The ReplayGain pre-scale (flac.c) — the ONLY arithmetic in the tree
 *      that changes a sample after the decoder. 897 of the library's 928
 *      FLACs carry the tag; 34 carry a POSITIVE gain with a full-scale peak,
 *      and until 2026-09-24 the gain was applied uncapped and "clipping
 *      handled by saturating" — hard clipping on every peak of those tracks.
 *   2. The path from the decoder to the DMA word: 1024-frame decode steps
 *      (player.c PLAY_STEP_FRAMES) -> pcm_ring_write -> 8192-frame ISR pulls
 *      (audio.c AUDIO_FRAMES_PER_BUF) -> the (R<<16)|L packing the I2S FIFO
 *      takes. A frame split across a chunk boundary, a wrap-arithmetic slip,
 *      a swapped half-word: any of them is a texture on the device and none
 *      had a host check.
 *
 * So: the committed sine fixture, with a VORBIS_COMMENT of our choosing
 * spliced in (in RAM — the fixture on disk is untouched and its KAT sha
 * stands), run through the SAME code the device runs — flac_meta_read,
 * flac_open_stream on the static arena, flac_set_gain_db_q8 with the peak
 * the player would pass, decode in device-sized steps, the real pcm_ring.c,
 * device-sized pulls, the DMA packing — and measured against the reference
 * PCM: bit-exact when untagged, and within ±1 LSB / >= 80 dB SNR of the
 * IDEALLY scaled reference when tagged, with no sample ever saturated.
 *
 * The +10 dB case is the one that fails on the old code: the sine peaks at
 * 15999 (6.23 dB below full scale), so +10 dB clips ~40 % of every cycle and
 * the SNR collapses to ~20 dB. With the peak cap the gain lands at +6.23 dB,
 * the peak at 32767, and the SNR stays above 80. argv[1] is the vectors
 * directory (passed by meson).
 */

#include "flac.h"
#include "arena.h"
#include "flac_meta.h"
#include "pcm_ring.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails;

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fails++;
    }
    return cond;
}

/* --- decoder_source_t backed by an in-RAM buffer (stands in for the disk) --- */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} membuf_t;

static size_t mem_read(void *ud, void *buf, size_t n)
{
    membuf_t *m = (membuf_t *)ud;
    size_t avail = (m->pos < m->len) ? (m->len - m->pos) : 0;
    if (n > avail) {
        n = avail;
    }
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}
static int mem_seek(void *ud, int offset, int origin)
{
    membuf_t *m = (membuf_t *)ud;
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
static int64_t mem_tell(void *ud)
{
    return (int64_t)((membuf_t *)ud)->pos;
}

static uint8_t *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        *out_len = (size_t)n;
    }
    return buf;
}

/* --- splice a VORBIS_COMMENT into a real FLAC, in RAM ---------------------
 * Copies every metadata block of `in` except its VORBIS_COMMENT (the fixture's
 * is empty), clears their last-block flags, appends ours as the last block,
 * then the audio frames verbatim. dr_flac needs only STREAMINFO to decode, so
 * the audio is untouched and the untagged case stays bit-exact. */
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t splice_tags(const uint8_t *in, size_t in_len,
                          const char *const *tags, size_t n_tags,
                          uint8_t *out, size_t out_cap)
{
    if (in_len < 4 || memcmp(in, "fLaC", 4) != 0) {
        return 0;
    }
    size_t ip = 4, op = 0;
    memcpy(out, "fLaC", 4);
    op += 4;
    int last = 0;
    while (!last && ip + 4 <= in_len) {
        uint8_t  hdr  = in[ip];
        uint32_t blen = ((uint32_t)in[ip + 1] << 16) | ((uint32_t)in[ip + 2] << 8)
                      | in[ip + 3];
        last = (hdr & 0x80) != 0;
        if (ip + 4 + blen > in_len) {
            return 0;
        }
        if ((hdr & 0x7F) != 4) {                 /* keep, not-last */
            if (op + 4 + blen > out_cap) {
                return 0;
            }
            out[op] = (uint8_t)(hdr & 0x7F);
            memcpy(out + op + 1, in + ip + 1, 3 + blen);
            op += 4 + blen;
        }
        ip += 4 + blen;
    }
    /* Our VORBIS_COMMENT, last. */
    size_t need = 4 + 4 + 5 + 4;
    for (size_t i = 0; i < n_tags; i++) {
        need += 4 + strlen(tags[i]);
    }
    if (op + need > out_cap) {
        return 0;
    }
    uint8_t *hdr = out + op;
    op += 4;
    size_t body = op;
    put_le32(out + op, 5);
    memcpy(out + op + 4, "chain", 5);
    op += 9;
    put_le32(out + op, (uint32_t)n_tags);
    op += 4;
    for (size_t i = 0; i < n_tags; i++) {
        uint32_t l = (uint32_t)strlen(tags[i]);
        put_le32(out + op, l);
        memcpy(out + op + 4, tags[i], l);
        op += 4 + l;
    }
    uint32_t blen = (uint32_t)(op - body);
    hdr[0] = 0x84;
    hdr[1] = (uint8_t)(blen >> 16);
    hdr[2] = (uint8_t)(blen >> 8);
    hdr[3] = (uint8_t)blen;
    /* Audio frames verbatim. */
    if (op + (in_len - ip) > out_cap) {
        return 0;
    }
    memcpy(out + op, in + ip, in_len - ip);
    op += in_len - ip;
    return op;
}

/* --- the chain -----------------------------------------------------------
 * The device's numbers: player.c decodes PLAY_STEP_FRAMES = 1024 per step,
 * audio.c pulls AUDIO_FRAMES_PER_BUF = 8192 per completion. The ring is
 * sized so the fixture wraps it several times. */
#define STEP_FRAMES  1024u
#define PULL_FRAMES  8192u
#define RING_FRAMES  (1u << 14)          /* 16384: 44100 frames wrap it 2.7x */

static uint8_t   g_arena_buf[256 * 1024];
static int16_t   g_ring_store[RING_FRAMES * 2];
static int16_t   g_step_buf[STEP_FRAMES * 2];
static int16_t   g_pull_buf[PULL_FRAMES * 2];
static uint32_t  g_dma_words[PULL_FRAMES];

typedef struct {
    int      rg_q8;          /* what flac_set_gain_db_q8 reported, 0 = not set */
    int      rg_asked_q8;    /* what the tag asked for                          */
    size_t   frames;         /* frames that came out the far end                */
} chain_result_t;

/* Pull whatever the "ISR" would, pack it into DMA words, unpack into out. */
static void pull_and_pack(pcm_ring_t *r, uint32_t frames,
                          int16_t *out, size_t *out_frames, size_t out_cap)
{
    uint32_t got = pcm_ring_read(r, g_pull_buf, frames);
    for (uint32_t f = 0; f < got; f++) {
        /* audio.c: "the pair [L,R] in memory IS (R<<16)|L" — the FIFO word. */
        g_dma_words[f] = ((uint32_t)(uint16_t)g_pull_buf[2 * f + 1] << 16)
                       | (uint16_t)g_pull_buf[2 * f];
    }
    for (uint32_t f = 0; f < got; f++) {
        if (*out_frames < out_cap) {
            out[2 * *out_frames]     = (int16_t)(uint16_t)(g_dma_words[f] & 0xFFFFu);
            out[2 * *out_frames + 1] = (int16_t)(uint16_t)(g_dma_words[f] >> 16);
        }
        (*out_frames)++;
    }
}

static int run_chain(const uint8_t *flac, size_t flac_len,
                     int16_t *out, size_t out_cap, chain_result_t *res)
{
    memset(res, 0, sizeof *res);

    /* 1. Tags, the way track_open reads them. */
    membuf_t         mb  = { flac, flac_len, 0 };
    decoder_source_t src = { mem_read, mem_seek, mem_tell, &mb };
    flac_meta_t      meta;
    if (flac_meta_read(&src, &meta) != 0 || !meta.have) {
        return -1;
    }

    /* 2. Open on the static arena, gain + peak the way track_open passes them. */
    decoder_arena_t arena;
    decoder_arena_init(&arena, g_arena_buf, sizeof g_arena_buf);
    decoder_alloc_t alloc = decoder_arena_allocator(&arena);
    mb.pos = 0;
    decoder_t d;
    if (flac_open_stream(&d, &src, &alloc) != DECODER_OK) {
        return -2;
    }
    if (meta.have_rg & FLAC_META_RG_TRACK) {
        uint32_t peak = (meta.have_rg & FLAC_META_RG_TRACK_PEAK) ? meta.rg_track_peak_q16 : 0;
        res->rg_asked_q8 = meta.rg_track_q8;
        res->rg_q8 = flac_set_gain_db_q8(&d, meta.rg_track_q8, peak);
    } else if (meta.have_rg & FLAC_META_RG_ALBUM) {
        uint32_t peak = (meta.have_rg & FLAC_META_RG_ALBUM_PEAK) ? meta.rg_album_peak_q16 : 0;
        res->rg_asked_q8 = meta.rg_album_q8;
        res->rg_q8 = flac_set_gain_db_q8(&d, meta.rg_album_q8, peak);
    }

    /* 3. Decode in device-sized steps into the real ring; pull in device-sized
     *    buffers whenever one is available, exactly as the ISR would. */
    pcm_ring_t ring;
    pcm_ring_init(&ring, g_ring_store, RING_FRAMES);
    size_t out_frames = 0;
    for (;;) {
        int got = d.ops->decode(&d, g_step_buf, (int)STEP_FRAMES);
        if (got <= 0) {
            break;
        }
        if (pcm_ring_write(&ring, g_step_buf, (uint32_t)got) != (uint32_t)got) {
            d.ops->close(&d);
            return -3;                     /* the ring must never be short here */
        }
        while (pcm_ring_fill(&ring) >= PULL_FRAMES) {
            pull_and_pack(&ring, PULL_FRAMES, out, &out_frames, out_cap);
        }
    }
    /* End of stream: the tail the last completion would drain. */
    pull_and_pack(&ring, pcm_ring_fill(&ring), out, &out_frames, out_cap);
    d.ops->close(&d);

    if (arena.oom) {
        return -4;
    }
    res->frames = out_frames;
    return 0;
}

/* --- measurement ---------------------------------------------------------
 * The gain the chain ACTUALLY applied is fitted by least squares
 * (sum(out*ref) / sum(ref*ref)), and the residual is measured against
 * ref * that. This separates the two things worth knowing: whether the level
 * is right (fitted_db against what the decoder reported — the Q12 gain table
 * is good to ~0.05 %, i.e. ~0.004 dB, and the fit sees straight through it)
 * and whether the WAVEFORM is right (the residual: rounding alone is under
 * 0.5 LSB per sample, ~90 dB SNR on this sine; clipping is tens of LSB per
 * peak and drags the SNR to ~20 dB, which is what the old code measures). */
typedef struct {
    double fitted_db;    /* gain actually applied, from the fit             */
    double snr_db;       /* out vs ref * fitted gain                        */
    int    max_abs;      /* loudest output sample                           */
    double max_err;      /* worst |out - ref * fitted| in LSB               */
} measure_t;

static void measure(const int16_t *out, const int16_t *ref, size_t samples,
                    measure_t *m)
{
    double xy = 0.0, xx = 0.0;
    memset(m, 0, sizeof *m);
    for (size_t i = 0; i < samples; i++) {
        xy += (double)out[i] * (double)ref[i];
        xx += (double)ref[i] * (double)ref[i];
    }
    double lin = (xx > 0.0) ? xy / xx : 0.0;
    m->fitted_db = 20.0 * log10(lin > 0.0 ? lin : 1e-9);
    double sig = 0.0, err = 0.0;
    for (size_t i = 0; i < samples; i++) {
        double ideal = (double)ref[i] * lin;
        double e     = (double)out[i] - ideal;
        sig += ideal * ideal;
        err += e * e;
        int a = out[i] < 0 ? -out[i] : out[i];
        if (a > m->max_abs) {
            m->max_abs = a;
        }
        double ae = e < 0 ? -e : e;
        if (ae > m->max_err) {
            m->max_err = ae;
        }
    }
    m->snr_db = (err > 0.0) ? 10.0 * log10(sig / err) : 200.0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vectors-dir>\n", argv[0]);
        return 2;
    }
    char p_flac[1024], p_pcm[1024];
    snprintf(p_flac, sizeof p_flac, "%s/sine_440hz_1s_44k_s16_stereo.flac", argv[1]);
    snprintf(p_pcm,  sizeof p_pcm,  "%s/sine_440hz_1s_44k_s16_stereo.pcm",  argv[1]);
    size_t flac_len = 0, ref_len = 0;
    uint8_t *flac = slurp(p_flac, &flac_len);
    uint8_t *refb = slurp(p_pcm, &ref_len);
    if (!flac || !refb) {
        fprintf(stderr, "cannot read fixtures\n");
        return 2;
    }
    const int16_t *ref        = (const int16_t *)refb;
    size_t         ref_frames = ref_len / 4;
    int16_t       *out        = (int16_t *)malloc(ref_len);
    uint8_t       *tagged     = (uint8_t *)malloc(flac_len + 1024);
    chain_result_t r;
    measure_t      m;

    /* The fixture's own peak, which the +10 dB case's tag must state. */
    int ref_peak = 0;
    for (size_t i = 0; i < ref_frames * 2; i++) {
        int a = ref[i] < 0 ? -ref[i] : ref[i];
        if (a > ref_peak) {
            ref_peak = a;
        }
    }
    double headroom_db = 20.0 * log10(32768.0 / (double)ref_peak);
    printf("fixture: %zu frames, peak %d (%.2f dB below full scale)\n",
           ref_frames, ref_peak, headroom_db);

    /* --- 1. untagged: the chain is a wire ------------------------------- */
    check("untagged: chain runs", run_chain(flac, flac_len, out, ref_frames, &r) == 0);
    check("untagged: every frame arrives, none twice", r.frames == ref_frames);
    check("untagged: no gain was set", r.rg_q8 == 0);
    check("untagged: ring -> pull -> DMA word is bit-exact",
          r.frames == ref_frames && memcmp(out, ref, ref_len) == 0);

    /* --- 2. +10 dB with the peak tagged: capped at the headroom ---------- */
    {
        char peak_tag[64];
        snprintf(peak_tag, sizeof peak_tag, "REPLAYGAIN_TRACK_PEAK=%.6f",
                 (double)ref_peak / 32768.0);
        const char *tags[] = { "REPLAYGAIN_TRACK_GAIN=+10.00 dB", peak_tag };
        size_t tl = splice_tags(flac, flac_len, tags, 2, tagged, flac_len + 1024);
        check("+10dB/peak: spliced", tl > 0);
        check("+10dB/peak: chain runs", run_chain(tagged, tl, out, ref_frames, &r) == 0);
        check("+10dB/peak: every frame arrives", r.frames == ref_frames);
        double got_db = r.rg_q8 / 256.0;
        printf("  asked %+.2f dB, got %+.2f dB (headroom %+.2f)\n",
               r.rg_asked_q8 / 256.0, got_db, headroom_db);
        check("+10dB/peak: the tag asked for +10", r.rg_asked_q8 == 2560);
        check("+10dB/peak: gain capped at the peak's headroom (+-0.02 dB)",
              fabs(got_db - headroom_db) <= 0.02);
        measure(out, ref, ref_frames * 2, &m);
        printf("  fitted %+.3f dB, peak %d, max err %.2f LSB, SNR %.1f dB\n",
               m.fitted_db, m.max_abs, m.max_err, m.snr_db);
        check("+10dB/peak: the level applied is the level reported (+-0.02 dB)",
              fabs(m.fitted_db - got_db) <= 0.02);
        check("+10dB/peak: the loudest sample is at full scale", m.max_abs >= 32700);
        check("+10dB/peak: waveform intact — SNR vs the scaled reference >= 80 dB",
              m.snr_db >= 80.0);
        check("+10dB/peak: no sample more than 1 LSB off (no clipped peaks)",
              m.max_err <= 1.0);
    }

    /* --- 3. +10 dB with NO peak tag: nothing says a boost is safe -------- */
    {
        const char *tags[] = { "REPLAYGAIN_TRACK_GAIN=+10.00 dB" };
        size_t tl = splice_tags(flac, flac_len, tags, 1, tagged, flac_len + 1024);
        check("+10dB/no peak: chain runs", tl > 0 && run_chain(tagged, tl, out, ref_frames, &r) == 0);
        check("+10dB/no peak: gain held at unity", r.rg_q8 == 0);
        check("+10dB/no peak: output bit-exact to the reference",
              r.frames == ref_frames && memcmp(out, ref, ref_len) == 0);
    }

    /* --- 4. -6.02 dB (half scale): the ordinary case, rounded not floored - */
    {
        const char *tags[] = { "REPLAYGAIN_TRACK_GAIN=-6.02 dB",
                               "REPLAYGAIN_TRACK_PEAK=1.000000" };
        size_t tl = splice_tags(flac, flac_len, tags, 2, tagged, flac_len + 1024);
        check("-6dB: chain runs", tl > 0 && run_chain(tagged, tl, out, ref_frames, &r) == 0);
        check("-6dB: gain applied as tagged", r.rg_q8 == r.rg_asked_q8 && r.rg_q8 == -1541);
        measure(out, ref, ref_frames * 2, &m);
        printf("  -6dB: fitted %+.3f dB, peak %d, max err %.2f LSB, SNR %.1f dB\n",
               m.fitted_db, m.max_abs, m.max_err, m.snr_db);
        check("-6dB: the level applied is the level tagged (+-0.02 dB)",
              fabs(m.fitted_db - r.rg_q8 / 256.0) <= 0.02);
        check("-6dB: every sample within 1 LSB of the scaled reference", m.max_err <= 1.0);
        check("-6dB: SNR >= 80 dB", m.snr_db >= 80.0);
    }

    /* --- 5. album gain + album peak, used when there is no track gain ---- */
    {
        const char *tags[] = { "REPLAYGAIN_ALBUM_GAIN=+3.00 dB",
                               "REPLAYGAIN_ALBUM_PEAK=1.000000" };
        size_t tl = splice_tags(flac, flac_len, tags, 2, tagged, flac_len + 1024);
        check("album: chain runs", tl > 0 && run_chain(tagged, tl, out, ref_frames, &r) == 0);
        check("album: the album tag was the one chosen", r.rg_asked_q8 == 768);
        check("album: +3 dB over a full-scale peak is capped to unity", r.rg_q8 == 0);
        check("album: output bit-exact to the reference",
              r.frames == ref_frames && memcmp(out, ref, ref_len) == 0);
    }

    free(out);
    free(tagged);
    free(flac);
    free(refb);
    printf("flac_rg_chain_test: %s\n", g_fails ? "FAIL" : "OK");
    return g_fails ? 1 : 0;
}
