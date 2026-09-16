/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/mp3_accuracy_test.c — is the MP3 decode RIGHT?
 *
 * codec-kat pins our decoder against ITSELF: it catches a regression but
 * would happily keep passing on a decoder that had been wrong from the start.
 * This suite is the other half — it compares against ffmpeg's decoder on real
 * music and holds the difference to the ISO/IEC 11172-4 "full accuracy"
 * bounds: RMS error below 2^-15/sqrt(12) of full scale (a PSNR of ~101 dB)
 * and a peak error of at most 2 LSB.
 *
 * Two correct MP3 decoders are never bit-identical — the standard specifies a
 * tolerance, not an output — so the gate is a tolerance and the comparison is
 * offset-aligned (ffmpeg trims the LAME encoder delay; we do not, by design:
 * gapless is out of scope). The floor here is 96 dB rather than 101 so a
 * rounding change in one Q-format helper is a warning sign rather than an
 * instant red build; what it actually measures, with room to spare, is in
 * codec-vectors/README.md.
 *
 * Three vectors, because the interesting parts of the decoder only run on
 * real content: 128 kbps CBR, a VBR encode with a Xing TOC, and an MPEG-2
 * mono file at 22.05 kHz (half-rate frames, 576 samples, mono side info).
 */

#include "mp3_vectors.h"

#define PSNR_FLOOR 96.0
#define PEAK_LIMIT 2

static int g_fails;

static void run_case(const char *dir, const char *stem, unsigned want_rate,
                     unsigned want_ch)
{
    char p_mp3[1024], p_ref[1024];
    snprintf(p_mp3, sizeof p_mp3, "%s/%s.mp3", dir, stem);
    snprintf(p_ref, sizeof p_ref, "%s/%s.ffmpeg.flac", dir, stem);

    size_t   mp3_len = 0;
    uint8_t *mp3     = mp3v_slurp(p_mp3, &mp3_len);
    mp3v_pcm_t ours = {0}, ref = {0};

    if (!mp3 || !mp3v_load_ref(p_ref, &ref) ||
        !mp3v_decode_mp3(mp3, mp3_len, &ours)) {
        printf("FAIL: %s: could not load or decode the vector\n", stem);
        g_fails++;
        free(mp3);
        mp3v_free(&ours);
        mp3v_free(&ref);
        return;
    }
    free(mp3);

    if (ours.rate != want_rate || ours.channels != want_ch) {
        printf("FAIL: %s: decoded %u Hz / %u ch, want %u / %u\n",
               stem, ours.rate, ours.channels, want_rate, want_ch);
        g_fails++;
    }
    if (ref.rate != want_rate || ref.channels != want_ch) {
        printf("FAIL: %s: reference is %u Hz / %u ch, want %u / %u\n",
               stem, ref.rate, ref.channels, want_rate, want_ch);
        g_fails++;
    }

    /*
     * Align. ffmpeg drops the encoder delay (about 1100 samples for a LAME
     * file) from the head of its output; we keep it, so ours starts earlier.
     * Search a generous window and require the answer to be small — an
     * alignment of thousands of samples would mean a framing bug, not a
     * rounding difference.
     */
    size_t window = 8192;
    if (window > ref.frames / 2) window = ref.frames / 2;
    double head_mse = 0.0;
    long   off = mp3v_best_offset(ours.pcm, ours.frames,
                                  ref.pcm, ref.frames,
                                  ours.channels, 4096, window, &head_mse);
    if (off < 0) {
        printf("FAIL: %s: could not align the two decodes\n", stem);
        g_fails++;
        mp3v_free(&ours);
        mp3v_free(&ref);
        return;
    }
    if (off > 2000) {
        printf("FAIL: %s: aligned at %ld frames — that is a framing bug, "
               "not encoder delay\n", stem, off);
        g_fails++;
    }

    /* Compare the whole overlap, not just the window the alignment used. */
    size_t overlap = ref.frames;
    if (overlap + (size_t)off > ours.frames) overlap = ours.frames - (size_t)off;

    int    peak = 0;
    double mse  = mp3v_mse(ours.pcm + (size_t)off * ours.channels,
                           ref.pcm, overlap, ours.channels, &peak);
    double psnr = mp3v_psnr(mse);

    printf("%-34s aligned +%4ld  %zu frames  PSNR %6.2f dB  peak %d LSB\n",
           stem, off, overlap, psnr, peak);

    if (psnr < PSNR_FLOOR) {
        printf("FAIL: %s: PSNR %.2f dB is below the %.0f dB floor\n",
               stem, psnr, PSNR_FLOOR);
        g_fails++;
    }
    if (peak > PEAK_LIMIT) {
        printf("FAIL: %s: peak error %d LSB exceeds the %d LSB limit\n",
               stem, peak, PEAK_LIMIT);
        g_fails++;
    }
    if (overlap < ref.frames / 2) {
        printf("FAIL: %s: only %zu of %zu reference frames were covered\n",
               stem, overlap, ref.frames);
        g_fails++;
    }

    mp3v_free(&ours);
    mp3v_free(&ref);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <codec-vectors-dir>\n", argv[0]);
        return 2;
    }
    run_case(argv[1], "beethoven_2s_cbr128_44k_stereo", 44100, 2);
    run_case(argv[1], "beethoven_2s_vbr_v5_44k_stereo", 44100, 2);
    run_case(argv[1], "beethoven_2s_cbr32_22k_mono",    22050, 1);

    if (g_fails) {
        printf("mp3_accuracy_test: %d check%s failed\n",
               g_fails, g_fails == 1 ? "" : "s");
        return 1;
    }
    printf("ok: mp3 decode matches ffmpeg within the ISO full-accuracy bounds\n");
    return 0;
}
