/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/mp3_vectors.h — shared plumbing for the MP3 vector suites.
 *
 * Header-only so mp3-accuracy and mp3-seek can each be one source file: load
 * a fixture, decode an MP3 through the real mp3_decoder_ops(), decode the
 * committed ffmpeg reference (stored as FLAC, which is lossless and half the
 * size), and compare two runs of PCM the way a lossy codec has to be compared
 * — aligned, then measured against a tolerance rather than memcmp'd. Why that
 * is the only honest comparison is in codec-vectors/README.md.
 */
#ifndef CORE_TESTS_MP3_VECTORS_H
#define CORE_TESTS_MP3_VECTORS_H

#include "mp3.h"
#include "flac.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PCM peak used for the PSNR figure: full scale for 16-bit samples. ISO/IEC
 * 11172-4's "full accuracy" limit is an RMS error below 2^-15/sqrt(12) of full
 * scale, which is this same 101 dB expressed the other way round. */
#define MP3V_FULL_SCALE 32768.0

typedef struct {
    int16_t *pcm;          /* interleaved s16, malloc'd  */
    size_t   frames;
    unsigned channels;
    unsigned rate;
} mp3v_pcm_t;

static uint8_t *mp3v_slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (n > 0) ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) *out_len = (size_t)n;
    return buf;
}

static void mp3v_free(mp3v_pcm_t *p)
{
    free(p->pcm);
    p->pcm = NULL;
    p->frames = 0;
}

/* Decode a committed *.ffmpeg.flac reference with the tree's own dr_flac. */
static int mp3v_load_ref(const char *path, mp3v_pcm_t *out)
{
    memset(out, 0, sizeof *out);
    size_t   len = 0;
    uint8_t *raw = mp3v_slurp(path, &len);
    if (!raw) return 0;

    const decoder_ops_t *ops = flac_decoder_ops();
    decoder_t d = {0};
    if (ops->open(&d, raw, len, NULL) != DECODER_OK) {
        fprintf(stderr, "reference %s is not a FLAC\n", path);
        free(raw);
        return 0;
    }
    out->channels = d.channels;
    out->rate     = d.sample_rate;
    out->frames   = (size_t)d.total_frames;
    out->pcm      = (int16_t *)malloc(out->frames * out->channels
                                      * sizeof(int16_t));
    size_t got = 0;
    while (out->pcm && got < out->frames) {
        int want = (int)((out->frames - got > 4096) ? 4096 : out->frames - got);
        int n = ops->decode(&d, out->pcm + got * out->channels, want);
        if (n <= 0) break;
        got += (size_t)n;
    }
    out->frames = got;
    ops->close(&d);
    free(raw);
    return out->pcm != NULL && got > 0;
}

/* Decode a whole MP3 through the SHIPPING decoder ops. */
static int mp3v_decode_mp3(const uint8_t *bytes, size_t len, mp3v_pcm_t *out)
{
    memset(out, 0, sizeof *out);
    const decoder_ops_t *ops = mp3_decoder_ops();
    decoder_t d = {0};
    if (ops->open(&d, bytes, len, NULL) != DECODER_OK) return 0;

    out->channels = d.channels;
    out->rate     = d.sample_rate;

    size_t cap = 65536, n = 0;
    int16_t *buf = (int16_t *)malloc(cap * out->channels * sizeof(int16_t));
    int16_t  chunk[4096 * 2];
    int got;
    while (buf && (got = ops->decode(&d, chunk, 4096)) > 0) {
        if (n + (size_t)got > cap) {
            cap *= 2;
            int16_t *bigger = (int16_t *)realloc(
                buf, cap * out->channels * sizeof(int16_t));
            if (!bigger) { free(buf); buf = NULL; break; }
            buf = bigger;
        }
        memcpy(buf + n * out->channels, chunk,
               (size_t)got * out->channels * sizeof(int16_t));
        n += (size_t)got;
    }
    ops->close(&d);
    out->pcm    = buf;
    out->frames = n;
    return buf != NULL && n > 0;
}

/* Mean squared error, in LSB^2, over `frames` interleaved frames. */
static double mp3v_mse(const int16_t *a, const int16_t *b,
                       size_t frames, unsigned ch, int *peak)
{
    double   acc = 0.0;
    int      hi  = 0;
    size_t   n   = frames * ch;
    for (size_t i = 0; i < n; i++) {
        int diff = (int)a[i] - (int)b[i];
        if (diff < 0) diff = -diff;
        if (diff > hi) hi = diff;
        acc += (double)diff * (double)diff;
    }
    if (peak) *peak = hi;
    return (n > 0) ? acc / (double)n : 0.0;
}

static double mp3v_psnr(double mse)
{
    if (mse <= 0.0) return 1000.0;               /* identical */
    return 20.0 * log10(MP3V_FULL_SCALE / sqrt(mse));
}

/*
 * Where does `probe` sit inside `ref`? Returns the offset in FRAMES that
 * minimises the error over `window` frames, searching [0, max_off].
 *
 * Alignment is not a fudge: ffmpeg applies the LAME encoder delay and end
 * padding (gapless trimming) and we deliberately do not, so the two decodes
 * of the same file start a few hundred samples apart by design. Finding the
 * offset is also what makes the seek test meaningful — it asks WHERE the
 * decoder landed, measured in the audio itself.
 */
static long mp3v_best_offset(const int16_t *ref, size_t ref_frames,
                             const int16_t *probe, size_t probe_frames,
                             unsigned ch, long max_off, size_t window,
                             double *out_mse)
{
    if (window > probe_frames) window = probe_frames;
    long   best     = -1;
    double best_mse = 0.0;
    for (long off = 0; off <= max_off; off++) {
        if ((size_t)off + window > ref_frames) break;
        double mse = mp3v_mse(ref + (size_t)off * ch, probe, window, ch, NULL);
        if (best < 0 || mse < best_mse) {
            best     = off;
            best_mse = mse;
        }
    }
    if (out_mse) *out_mse = best_mse;
    return best;
}

#endif /* CORE_TESTS_MP3_VECTORS_H */
