/*
 * core/tests/scripts/capture_mp3_ref.c
 *
 * One-shot tool used by gen_codec_vectors.sh to capture dr_mp3's
 * decoded output for a given .mp3 fixture. Run once when a new MP3
 * vector is added; commit both the .mp3 and the captured .pcm.
 *
 * MP3 isn't bit-stable across decoder implementations, so the
 * captured PCM is "what dr_mp3 produces" — not external truth. The
 * KAT (codec_kat.c) re-decodes and memcmp's against this captured
 * reference, giving us regression protection: any change to dr_mp3
 * or our wrapper that alters output trips the test.
 *
 * Compiled ad-hoc by the bash regenerator; not part of the meson
 * build. Self-contained — compiles with:
 *   gcc -O2 -o capture_mp3_ref capture_mp3_ref.c -lm
 */

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../../codecs/dr_mp3/dr_mp3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <in.mp3> <out.pcm>\n", argv[0]);
        return 2;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("open input"); return 1; }
    fseek(fin, 0, SEEK_END);
    long n = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (n <= 0) { fprintf(stderr, "input empty\n"); fclose(fin); return 1; }

    void *buf = malloc((size_t)n);
    if (!buf) { fprintf(stderr, "oom\n"); fclose(fin); return 1; }
    if (fread(buf, 1, (size_t)n, fin) != (size_t)n) {
        fprintf(stderr, "short read\n"); free(buf); fclose(fin); return 1;
    }
    fclose(fin);

    drmp3 d;
    if (!drmp3_init_memory(&d, buf, (size_t)n, NULL)) {
        fprintf(stderr, "drmp3_init_memory failed\n");
        free(buf);
        return 1;
    }
    /* Match the runtime wrapper's channel guard so a 5.1 MP3 (which
     * shouldn't ever exist in our test corpus) can't silently
     * overflow our stereo-sized batch buffer below. */
    if (d.channels > 2) {
        fprintf(stderr, "channels=%u > 2; capture_mp3_ref only supports mono/stereo\n",
                d.channels);
        drmp3_uninit(&d);
        free(buf);
        return 1;
    }

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("open output"); drmp3_uninit(&d); free(buf); return 1; }

    enum { BATCH = 4096 };
    int16_t pcm[BATCH * 2];           /* max 2 channels */
    long total_frames = 0;
    drmp3_uint64 got;
    while ((got = drmp3_read_pcm_frames_s16(&d, BATCH, pcm)) > 0) {
        size_t bytes = (size_t)got * d.channels * 2;
        if (fwrite(pcm, 1, bytes, fout) != bytes) {
            perror("write");
            fclose(fout); drmp3_uninit(&d); free(buf); return 1;
        }
        total_frames += (long)got;
    }

    fclose(fout);
    drmp3_uninit(&d);
    free(buf);

    fprintf(stderr, "captured %ld frames at %u Hz, %u ch -> %s\n",
            total_frames, d.sampleRate, d.channels, argv[2]);
    return 0;
}
