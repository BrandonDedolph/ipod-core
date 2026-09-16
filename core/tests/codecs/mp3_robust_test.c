/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/mp3_robust_test.c — the files a real library actually contains.
 *
 * Sync-word hunting is where MP3 decoders crash: the format has no container,
 * no length field and no checksum anyone honours, so a decoder spends its life
 * guessing where the next frame is from bytes it has no reason to trust. A
 * truncated download, a bad sector, an ID3 tag that lies about its own size or
 * a file that is not MPEG at all must each end in a DEFINED answer — a refused
 * open or a clean end of stream — and never in a read past the end of a
 * buffer. The suite runs under the ASan/UBSan job like every other, which is
 * what makes "never" checkable rather than aspirational.
 *
 * Nothing here needs a new fixture: every case is built from the committed
 * sine vector or synthesised outright.
 */

#include "mp3.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails;

static void xpect(const char *what, int cond)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        g_fails++;
    }
}

static uint8_t *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (n > 0) ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

/* Open, decode everything, close. Returns the open result; *frames gets the
 * PCM frames that came out (0 when the open failed). */
static int decode_all(const uint8_t *bytes, size_t len, long *frames)
{
    const decoder_ops_t *ops = mp3_decoder_ops();
    decoder_t d = {0};
    *frames = 0;
    int rc = ops->open(&d, bytes, len, NULL);
    if (rc != DECODER_OK) return rc;

    int16_t buf[4096 * 2];
    int got;
    while ((got = ops->decode(&d, buf, 4096)) > 0) {
        *frames += got;
    }
    xpect("decode() reports end of stream as 0, never as an error", got == 0);
    ops->close(&d);
    return rc;
}

/* A synthetic stream of `n` identical frames. */
static size_t synth(uint8_t *buf, int ver_id, int layer_id, int br_ix,
                    int sr_ix, int n)
{
    /* Frame length by the spec formula, so the frames chain properly. */
    static const uint16_t BR_V1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112,
                                        128, 160, 192, 224, 256, 320, 0 };
    static const uint16_t BR_V2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80,
                                        96, 112, 128, 144, 160, 0 };
    static const uint32_t SR[4][3] = {
        { 11025, 12000, 8000 }, { 0, 0, 0 },
        { 22050, 24000, 16000 }, { 44100, 48000, 32000 },
    };
    uint32_t bps  = 1000u * (ver_id == 3 ? BR_V1[br_ix] : BR_V2[br_ix]);
    uint32_t len  = (ver_id == 3 ? 144u : 72u) * bps / SR[ver_id][sr_ix];
    size_t   o    = 0;
    for (int i = 0; i < n; i++) {
        buf[o + 0] = 0xFFu;
        buf[o + 1] = (uint8_t)(0xE0u | (ver_id << 3) | (layer_id << 1) | 1u);
        buf[o + 2] = (uint8_t)((br_ix << 4) | (sr_ix << 2));
        buf[o + 3] = 0x00u;
        memset(buf + o + 4, 0, len - 4u);
        o += len;
    }
    return o;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <codec-vectors-dir>\n", argv[0]);
        return 2;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/sine_440hz_1s_44k_s16_stereo_128k.mp3",
             argv[1]);
    size_t   good_len = 0;
    uint8_t *good = slurp(path, &good_len);
    if (!good) {
        fprintf(stderr, "cannot read the sine vector\n");
        return 2;
    }

    long whole = 0;
    xpect("the intact vector opens", decode_all(good, good_len, &whole) == DECODER_OK);
    xpect("...and decodes a second of audio", whole > 40000 && whole < 50000);

    /* ---- truncated tail ------------------------------------------------ */
    {
        long n = 0;
        size_t cut = good_len * 6u / 10u;
        xpect("a file cut off two thirds through still opens",
              decode_all(good, cut, &n) == DECODER_OK);
        xpect("...and stops at the last whole frame, without a partial one",
              n > 0 && n < whole && (n % 1152) == 0);
    }

    /* ---- a flipped byte inside a frame ---------------------------------- */
    {
        uint8_t *bad = (uint8_t *)malloc(good_len);
        memcpy(bad, good, good_len);
        bad[good_len / 2] ^= 0xFFu;
        long n = 0;
        xpect("a file with a corrupted byte still opens",
              decode_all(bad, good_len, &n) == DECODER_OK);
        /* Whether pvmp3 mutes the damaged frame or our resync steps over it,
         * the track must still be about a second long — not truncated at the
         * damage and not looping. */
        xpect("...and still decodes about the same amount of audio",
              n > whole - 6000 && n <= whole);
        free(bad);
    }

    /* ---- junk spliced between frames ------------------------------------ */
    {
        size_t   junk = 1000;
        uint8_t *mix  = (uint8_t *)malloc(good_len + junk);
        size_t   at   = good_len / 2;
        memcpy(mix, good, at);
        for (size_t i = 0; i < junk; i++) {
            mix[at + i] = (uint8_t)((i % 5u == 0u) ? 0xFFu : (i * 37u));
        }
        memcpy(mix + at + junk, good + at, good_len - at);
        long n = 0;
        xpect("a file with junk spliced into it opens",
              decode_all(mix, good_len + junk, &n) == DECODER_OK);
        xpect("...and resyncs past the junk instead of stopping there",
              n > whole - 6000);
        free(mix);
    }

    /* ---- wrong layer ----------------------------------------------------- */
    {
        static uint8_t buf[65536];
        size_t n = synth(buf, 3, 2, 9, 0, 40);          /* Layer II */
        long   f = 0;
        xpect("a Layer II file is refused, not decoded as Layer III",
              decode_all(buf, n, &f) == DECODER_ERR_INVALID);
        n = synth(buf, 3, 3, 9, 0, 40);                 /* Layer I */
        xpect("a Layer I file is refused",
              decode_all(buf, n, &f) == DECODER_ERR_INVALID);
    }

    /* ---- rates the DAC cannot clock -------------------------------------- */
    {
        static uint8_t buf[65536];
        long   f = 0;
        /* MPEG-2.5: 11025 / 12000 / 8000 at sr_ix 0 / 1 / 2. */
        for (int sr_ix = 0; sr_ix < 3; sr_ix++) {
            size_t n = synth(buf, 0, 1, 1, sr_ix, 40);
            xpect("an MPEG-2.5 file is refused (the WM8758B cannot clock it)",
                  decode_all(buf, n, &f) == DECODER_ERR_UNSUPPORTED);
        }
        /* MPEG-2 16 kHz is below the codec's floor too; 22.05 and 24 are not. */
        size_t n = synth(buf, 2, 1, 4, 2, 40);          /* 16000 Hz */
        xpect("MPEG-2 at 16 kHz is refused",
              decode_all(buf, n, &f) == DECODER_ERR_UNSUPPORTED);
        n = synth(buf, 2, 1, 4, 0, 40);                 /* 22050 Hz */
        xpect("MPEG-2 at 22.05 kHz is accepted",
              decode_all(buf, n, &f) == DECODER_OK);
        n = synth(buf, 2, 1, 4, 1, 40);                 /* 24000 Hz */
        xpect("MPEG-2 at 24 kHz is accepted",
              decode_all(buf, n, &f) == DECODER_OK);
    }

    /* ---- an ID3 tag longer than the file --------------------------------- */
    {
        static uint8_t buf[4096];
        memset(buf, 0, sizeof buf);
        memcpy(buf, "ID3", 3);
        buf[3] = 3; buf[4] = 0; buf[5] = 0;
        buf[6] = 0x7Fu; buf[7] = 0x7Fu; buf[8] = 0x7Fu; buf[9] = 0x7Fu;
        long f = 0;
        xpect("a tag claiming 256 MB in a 4 KB file is refused cleanly",
              decode_all(buf, sizeof buf, &f) == DECODER_ERR_INVALID);

        /* The same lie in front of real audio: the bogus size is ignored and
         * the audio behind it still plays. */
        size_t need = 10u + good_len;
        uint8_t *mix = (uint8_t *)malloc(need);
        memcpy(mix, buf, 10);
        memcpy(mix + 10, good, good_len);
        long n = 0;
        xpect("...and audio behind such a tag is still found",
              decode_all(mix, need, &n) == DECODER_OK && n > whole - 6000);
        free(mix);
    }

    /* ---- not MPEG at all -------------------------------------------------- */
    {
        static uint8_t buf[8192];
        long f = 0;
        memset(buf, 0, sizeof buf);
        xpect("a file of zeroes is refused",
              decode_all(buf, sizeof buf, &f) == DECODER_ERR_INVALID);
        memset(buf, 0xFFu, sizeof buf);
        xpect("a file of 0xFF (sync words everywhere, frames nowhere) is refused",
              decode_all(buf, sizeof buf, &f) == DECODER_ERR_INVALID);
        memcpy(buf, "fLaC", 4);
        xpect("a FLAC handed to the MP3 decoder is refused",
              decode_all(buf, sizeof buf, &f) == DECODER_ERR_INVALID);
    }

    /* ---- free format ------------------------------------------------------ */
    {
        static uint8_t buf[8192];
        long f = 0;
        memset(buf, 0, sizeof buf);
        for (int i = 0; i < 8; i++) {
            buf[i * 512 + 0] = 0xFFu;
            buf[i * 512 + 1] = 0xFBu;
            buf[i * 512 + 2] = 0x00u;          /* bitrate index 0 = free */
            buf[i * 512 + 3] = 0x00u;
        }
        xpect("a free-format stream is refused (its frames have no length)",
              decode_all(buf, sizeof buf, &f) == DECODER_ERR_INVALID);
    }

    /* ---- degenerate arguments --------------------------------------------- */
    {
        const decoder_ops_t *ops = mp3_decoder_ops();
        decoder_t d = {0};
        xpect("a zero-length open is refused",
              ops->open(&d, good, 0, NULL) == DECODER_ERR_INVALID);
        xpect("a NULL buffer is refused",
              ops->open(&d, NULL, 100, NULL) == DECODER_ERR_INVALID);
        uint8_t tiny[3] = { 0xFFu, 0xFBu, 0x90u };
        long f = 0;
        xpect("a three-byte file is refused",
              decode_all(tiny, sizeof tiny, &f) == DECODER_ERR_INVALID);

        xpect("the vector opens for the argument checks",
              ops->open(&d, good, good_len, NULL) == DECODER_OK);
        int16_t out[16];
        xpect("decode() with no room is an argument error",
              ops->decode(&d, out, 0) == DECODER_ERR_INVALID);
        xpect("decode() into NULL is an argument error",
              ops->decode(&d, NULL, 16) == DECODER_ERR_INVALID);
        ops->close(&d);
        xpect("close() clears the ops pointer", d.ops == NULL);
    }

    free(good);
    if (g_fails) {
        printf("mp3_robust_test: %d check%s failed\n",
               g_fails, g_fails == 1 ? "" : "s");
        return 1;
    }
    printf("ok: every malformed MP3 ends in a defined answer\n");
    return 0;
}
