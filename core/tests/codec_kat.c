/*
 * core/tests/codec_kat.c — Codec known-answer tests.
 *
 * For each vendored codec: decode a deterministic fixture and bit-
 * compare the decoded PCM against a committed reference PCM file (the
 * input that was originally encoded). Lossless codecs (FLAC, ALAC,
 * WAV) must be byte-exact; lossy codecs (MP3, AAC, Vorbis, Opus) will
 * compare against pre-recorded decoded reference PCM instead — that
 * lands when those codecs do.
 *
 * We reference *committed* PCM bytes rather than re-deriving them by
 * formula to avoid cross-libm bit-stability issues — `sin()` is not
 * required to round identically across glibc, musl, macOS libm, etc.,
 * so a "regenerate the same PCM in C" approach would silently flake
 * across hosts. The .pcm + .flac get checked in together; the test
 * just memcmps.
 *
 * Run as: ./build-sim/tests/codec_kat <path-to-codec-vectors-dir>
 */

#include "../codecs/dr_flac/flac.h"
#include "../codecs/decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char flac_path[1024], pcm_path[1024];
    snprintf(flac_path, sizeof(flac_path),
             "%s/sine_440hz_1s_44k_s16_stereo.flac", vectors_dir);
    snprintf(pcm_path, sizeof(pcm_path),
             "%s/sine_440hz_1s_44k_s16_stereo.pcm", vectors_dir);

    void *flac_bytes = NULL;
    long  flac_len   = load_file(flac_path, &flac_bytes);
    if (flac_len < 0) return 1;

    void *ref_bytes  = NULL;
    long  ref_len    = load_file(pcm_path, &ref_bytes);
    if (ref_len < 0) {
        free(flac_bytes);
        return 1;
    }

    decoder_t d = {0};
    const decoder_ops_t *ops = flac_decoder_ops();
    int rc = ops->open(&d, flac_bytes, (size_t)flac_len, /*alloc=*/NULL);
    if (rc != DECODER_OK) {
        fprintf(stderr, "flac open failed: %d\n", rc);
        free(flac_bytes);
        free(ref_bytes);
        return 1;
    }

    /* Sanity-check stream metadata against the fixture spec. */
    if (d.sample_rate != 44100 || d.channels != 2) {
        fprintf(stderr, "metadata: sample_rate=%u channels=%u "
                        "(expected 44100/2)\n",
                d.sample_rate, d.channels);
        ops->close(&d);
        free(flac_bytes);
        free(ref_bytes);
        return 1;
    }

    /* Reference PCM is interleaved s16le. d.total_frames * 2ch * 2B
     * should match the .pcm file size. */
    long expected_bytes = (long)d.total_frames * d.channels * 2;
    if (expected_bytes != ref_len) {
        fprintf(stderr, "ref pcm size %ld, expected %ld "
                        "(total_frames=%llu)\n",
                ref_len, expected_bytes,
                (unsigned long long)d.total_frames);
        ops->close(&d);
        free(flac_bytes);
        free(ref_bytes);
        return 1;
    }

    int16_t *decoded = malloc((size_t)expected_bytes);
    if (!decoded) {
        fprintf(stderr, "oom decoded\n");
        ops->close(&d);
        free(flac_bytes);
        free(ref_bytes);
        return 1;
    }

    int total_frames = (int)d.total_frames;
    int got_total    = 0;
    while (got_total < total_frames) {
        int got = ops->decode(&d, decoded + got_total * d.channels,
                              total_frames - got_total);
        if (got <= 0) break;
        got_total += got;
    }
    if (got_total != total_frames) {
        fprintf(stderr, "decoded %d frames, want %d\n",
                got_total, total_frames);
        free(decoded);
        ops->close(&d);
        free(flac_bytes);
        free(ref_bytes);
        return 1;
    }

    int rc_cmp = memcmp(decoded, ref_bytes, (size_t)expected_bytes);
    if (rc_cmp != 0) {
        /* Find the first mismatch for a useful error message. */
        const int16_t *a = decoded;
        const int16_t *b = (const int16_t *)ref_bytes;
        long n = expected_bytes / 2;
        for (long i = 0; i < n; i++) {
            if (a[i] != b[i]) {
                fprintf(stderr,
                    "FAIL: first mismatch at sample %ld: decoded=%d ref=%d\n",
                    i, a[i], b[i]);
                break;
            }
        }
        free(decoded);
        ops->close(&d);
        free(flac_bytes);
        free(ref_bytes);
        return 1;
    }

    printf("OK: flac decoded %d frames, %ld bytes bit-exact\n",
           total_frames, expected_bytes);
    free(decoded);
    ops->close(&d);
    free(flac_bytes);
    free(ref_bytes);
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
