/*
 * core/tests/scripts/capture_mp3_ref.c
 *
 * One-shot tool used by gen_codec_vectors.sh to capture OUR decoder's output
 * for a given .mp3 fixture. Run once when a new MP3 vector is added; commit
 * both the .mp3 and the captured .pcm.
 *
 * MP3 isn't bit-stable across decoder implementations, so the captured PCM is
 * "what core's decoder produces" — not external truth. The KAT (codec_kat.c)
 * re-decodes and memcmp's against this captured reference, giving us
 * regression protection: any change to pvmp3 or our wrapper that alters output
 * trips the test. Whether the output is RIGHT is the separate question the
 * mp3-accuracy suite asks against ffmpeg, to a tolerance.
 *
 * It goes through the SAME mp3_decoder_ops() the firmware runs, on the HOST
 * build of pvmp3 (the C-equivalent fixed-point ops). The ARM build rounds a
 * hair differently in its smull assembly, which is why the ARM side is only
 * ever held to the tolerance test — see tests/codec-vectors/README.md.
 *
 * Compiled ad-hoc by the bash regenerator; not part of the meson build.
 */

#include "../../codecs/pvmp3/mp3.h"

#include <stdio.h>
#include <stdlib.h>
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

    const decoder_ops_t *ops = mp3_decoder_ops();
    decoder_t d = {0};
    int rc = ops->open(&d, buf, (size_t)n, /*alloc=*/NULL);
    if (rc != DECODER_OK) {
        fprintf(stderr, "open failed: %d\n", rc);
        free(buf);
        return 1;
    }

    FILE *fout = fopen(argv[2], "wb");
    if (!fout) { perror("open output"); ops->close(&d); free(buf); return 1; }

    enum { BATCH = 4096 };
    int16_t pcm[BATCH * 2];           /* the wrapper never emits > 2 channels */
    long total_frames = 0;
    int  got;
    while ((got = ops->decode(&d, pcm, BATCH)) > 0) {
        size_t bytes = (size_t)got * d.channels * 2u;
        if (fwrite(pcm, 1, bytes, fout) != bytes) {
            perror("write");
            fclose(fout); ops->close(&d); free(buf); return 1;
        }
        total_frames += got;
    }

    fprintf(stderr, "captured %ld frames at %u Hz, %u ch (tag length %llu) -> %s\n",
            total_frames, d.sample_rate, d.channels,
            (unsigned long long)d.total_frames, argv[2]);
    fclose(fout);
    ops->close(&d);
    free(buf);
    return 0;
}
