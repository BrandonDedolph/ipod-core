/*
 * core/tests/codec_kat.c — Codec known-answer tests.
 *
 * For each vendored codec: decode a fixture and bit-compare the
 * decoded PCM against a committed reference PCM file.
 *
 * Lossless codecs (FLAC, ALAC, WAV) compare against the original
 * input PCM that was encoded — round-trip must be exact.
 *
 * Lossy codecs (MP3, AAC, Vorbis, Opus) compare against PCM captured
 * from our own decoder at fixture-creation time. That gives us
 * regression protection ("did dr_mp3 change its output?") without
 * requiring a perfect oracle (different MP3 decoders aren't bit-stable).
 *
 * We reference *committed* PCM bytes rather than re-deriving them by
 * formula to avoid cross-libm bit-stability issues — `sin()` is not
 * required to round identically across glibc, musl, macOS libm, etc.
 *
 * Run as: ./build-sim/tests/codec_kat <path-to-codec-vectors-dir>
 */

#include "../codecs/decoder.h"
#include "../codecs/dr_flac/flac.h"
#include "../codecs/dr_mp3/mp3.h"

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

/*
 * Run one KAT: open the codec, decode all frames, byte-compare against
 * the reference PCM. Returns 0 on pass, 1 on fail. Logs to stdout/stderr.
 */
static int run_kat(const char         *label,
                   const decoder_ops_t *ops,
                   const char         *fixture_path,
                   const char         *ref_path,
                   uint32_t            expected_rate,
                   uint16_t            expected_channels) {
    void *enc = NULL;
    long  enc_len = load_file(fixture_path, &enc);
    if (enc_len < 0) return 1;

    void *ref = NULL;
    long  ref_len = load_file(ref_path, &ref);
    if (ref_len < 0) { free(enc); return 1; }

    decoder_t d = {0};
    int rc = ops->open(&d, enc, (size_t)enc_len, /*alloc=*/NULL);
    if (rc != DECODER_OK) {
        fprintf(stderr, "[%s] open failed: %d\n", label, rc);
        free(enc); free(ref);
        return 1;
    }

    if (d.sample_rate != expected_rate || d.channels != expected_channels) {
        fprintf(stderr, "[%s] metadata mismatch: %u Hz / %u ch (want %u / %u)\n",
                label, d.sample_rate, d.channels,
                expected_rate, expected_channels);
        ops->close(&d);
        free(enc); free(ref);
        return 1;
    }

    /* Reference PCM is interleaved s16le. Total expected frames =
     * ref_len / channels / 2. We decode in batches and accumulate
     * up to that count. */
    long expected_bytes  = ref_len;
    long expected_frames = ref_len / d.channels / 2;
    int16_t *decoded = malloc((size_t)expected_bytes);
    if (!decoded) {
        fprintf(stderr, "[%s] oom decoded\n", label);
        ops->close(&d);
        free(enc); free(ref);
        return 1;
    }

    long got_total = 0;
    while (got_total < expected_frames) {
        int batch = (int)((expected_frames - got_total) > 4096
                          ? 4096 : (expected_frames - got_total));
        int got = ops->decode(&d, decoded + got_total * d.channels, batch);
        if (got <= 0) break;
        got_total += got;
    }
    if (got_total != expected_frames) {
        fprintf(stderr, "[%s] decoded %ld frames, want %ld\n",
                label, got_total, expected_frames);
        free(decoded); ops->close(&d); free(enc); free(ref);
        return 1;
    }

    if (memcmp(decoded, ref, (size_t)expected_bytes) != 0) {
        const int16_t *a = decoded;
        const int16_t *b = (const int16_t *)ref;
        long n = expected_bytes / 2;
        for (long i = 0; i < n; i++) {
            if (a[i] != b[i]) {
                fprintf(stderr,
                    "[%s] FAIL: first mismatch at sample %ld: decoded=%d ref=%d\n",
                    label, i, a[i], b[i]);
                break;
            }
        }
        free(decoded); ops->close(&d); free(enc); free(ref);
        return 1;
    }

    printf("OK: %s decoded %ld frames, %ld bytes bit-exact\n",
           label, got_total, expected_bytes);
    free(decoded); ops->close(&d); free(enc); free(ref);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <codec-vectors-dir>\n", argv[0]);
        return 2;
    }
    const char *vd = argv[1];

    char p_flac[1024], p_pcm[1024], p_mp3[1024], p_mp3ref[1024];
    snprintf(p_flac, sizeof(p_flac),
             "%s/sine_440hz_1s_44k_s16_stereo.flac", vd);
    snprintf(p_pcm, sizeof(p_pcm),
             "%s/sine_440hz_1s_44k_s16_stereo.pcm", vd);
    snprintf(p_mp3, sizeof(p_mp3),
             "%s/sine_440hz_1s_44k_s16_stereo_128k.mp3", vd);
    snprintf(p_mp3ref, sizeof(p_mp3ref),
             "%s/sine_440hz_1s_44k_s16_stereo_128k.mp3.ref.pcm", vd);

    int fails = 0;
    fails += run_kat("flac", flac_decoder_ops(), p_flac,   p_pcm,    44100, 2);
    fails += run_kat("mp3",  mp3_decoder_ops(),  p_mp3,    p_mp3ref, 44100, 2);
    return fails == 0 ? 0 : 1;
}
