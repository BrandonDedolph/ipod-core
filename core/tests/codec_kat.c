/*
 * core/tests/codec_kat.c — Codec known-answer tests.
 *
 * For each vendored codec, decodes a deterministic fixture and bit-
 * compares the PCM output against the reference produced by the same
 * formula in core/tests/scripts/gen_codec_vectors.py.
 *
 * The reference PCM is regenerated in C below; the FLAC fixture is on
 * disk (committed into core/tests/codec-vectors/). If the encoder
 * (`flac` CLI) and our re-derivation of the formula here are aligned,
 * dr_flac should produce bit-identical output and this test passes.
 *
 * Run as: ./build-sim/tests/codec_kat <path-to-codec-vectors-dir>
 */

#include "../codecs/dr_flac/flac.h"
#include "../codecs/decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 44100
#define DURATION_S  1
#define FREQ_HZ     440.0
#define AMPLITUDE   16000
#define CHANNELS    2
#define FRAMES      (SAMPLE_RATE * DURATION_S)

/* Mirror of gen_codec_vectors.py's gen_sine_440hz_1s_44k_stereo(). */
static void gen_reference(int16_t *out) {
    for (int n = 0; n < FRAMES; n++) {
        double phase = 2.0 * M_PI * FREQ_HZ * (double)n / (double)SAMPLE_RATE;
        int16_t s = (int16_t)(AMPLITUDE * sin(phase));
        out[n * 2 + 0] = s;
        out[n * 2 + 1] = s;
    }
}

static long load_file(const char *path, void **out_buf) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "open %s: ", path);
        perror(NULL);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) {
        fprintf(stderr, "%s: empty\n", path);
        fclose(fp);
        return -1;
    }
    void *buf = malloc((size_t)n);
    if (!buf) {
        fprintf(stderr, "oom\n");
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fprintf(stderr, "%s: short read\n", path);
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out_buf = buf;
    return n;
}

static int test_flac_sine(const char *vectors_dir) {
    char path[1024];
    snprintf(path, sizeof(path),
             "%s/sine_440hz_1s_44k_s16_stereo.flac", vectors_dir);

    void *flac_bytes = NULL;
    long  flac_len   = load_file(path, &flac_bytes);
    if (flac_len < 0) return 1;

    decoder_t d = {0};
    const decoder_ops_t *ops = flac_decoder_ops();
    int rc = ops->open(&d, flac_bytes, (size_t)flac_len);
    if (rc != DECODER_OK) {
        fprintf(stderr, "flac open failed: %d\n", rc);
        free(flac_bytes);
        return 1;
    }

    /* Sanity-check stream metadata against the fixture spec. */
    if (d.sample_rate != SAMPLE_RATE) {
        fprintf(stderr, "sample_rate %u, want %d\n", d.sample_rate, SAMPLE_RATE);
        ops->close(&d);
        free(flac_bytes);
        return 1;
    }
    if (d.channels != CHANNELS) {
        fprintf(stderr, "channels %u, want %d\n", d.channels, CHANNELS);
        ops->close(&d);
        free(flac_bytes);
        return 1;
    }
    if (d.total_frames != FRAMES) {
        fprintf(stderr, "total_frames %llu, want %d\n",
                (unsigned long long)d.total_frames, FRAMES);
        ops->close(&d);
        free(flac_bytes);
        return 1;
    }

    /* Decode all frames into a contiguous buffer. */
    int16_t *decoded = malloc(sizeof(int16_t) * FRAMES * CHANNELS);
    if (!decoded) {
        fprintf(stderr, "oom decoded\n");
        ops->close(&d);
        free(flac_bytes);
        return 1;
    }

    int total = 0;
    while (total < FRAMES) {
        int got = ops->decode(&d, decoded + total * CHANNELS, FRAMES - total);
        if (got <= 0) break;
        total += got;
    }
    if (total != FRAMES) {
        fprintf(stderr, "decoded %d frames, want %d\n", total, FRAMES);
        free(decoded);
        ops->close(&d);
        free(flac_bytes);
        return 1;
    }

    /* Regenerate the reference PCM and compare bit-exact. */
    int16_t *reference = malloc(sizeof(int16_t) * FRAMES * CHANNELS);
    if (!reference) {
        fprintf(stderr, "oom reference\n");
        free(decoded);
        ops->close(&d);
        free(flac_bytes);
        return 1;
    }
    gen_reference(reference);

    int mismatches = 0;
    for (int i = 0; i < FRAMES * CHANNELS; i++) {
        if (decoded[i] != reference[i]) {
            if (mismatches < 4) {
                fprintf(stderr,
                    "mismatch at sample %d: decoded=%d reference=%d\n",
                    i, decoded[i], reference[i]);
            }
            mismatches++;
        }
    }

    free(reference);
    free(decoded);
    ops->close(&d);
    free(flac_bytes);

    if (mismatches > 0) {
        fprintf(stderr, "FAIL: %d/%d sample mismatches\n",
                mismatches, FRAMES * CHANNELS);
        return 1;
    }

    printf("OK: flac decoded %d frames, %d samples bit-exact\n",
           FRAMES, FRAMES * CHANNELS);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr,
            "usage: %s <codec-vectors-dir>\n"
            "  e.g.  %s ../tests/codec-vectors\n",
            argv[0], argv[0]);
        return 2;
    }
    return test_flac_sine(argv[1]);
}
