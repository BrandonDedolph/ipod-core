/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/mp3_freestanding_test.c — host test for the streaming MP3
 * decode path used on hw.
 *
 * Exercises exactly what the device will do, minus the disk: open the KAT
 * MP3 through mp3_open_stream() (a decoder_source_t pull source, here backed
 * by an in-memory buffer) with the static arena allocator instead of libc
 * malloc, decode every frame, and bit-compare the PCM against the reference
 * (the same sine_440hz_1s vector codec_kat uses). Also reports the arena
 * high-water mark so the on-device arena can be sized from a real decode.
 *
 * MP3 is lossy, so the reference is NOT external truth: it is PCM captured
 * from dr_mp3 itself on first generation. Decoding the same bytes through the
 * same dr_mp3 (only the byte source differs: memory-stream here vs whole-file
 * memory when the ref was captured) reproduces it bit-for-bit, so we assert an
 * exact match — any drift in dr_mp3 or our wrapper trips this.
 *
 * The code under test (mp3.c + arena.c) is the SAME source the ARM build
 * compiles; only the byte source differs (memory vs fat32_stream). argv[1] is
 * the vectors directory (passed by meson).
 */

#include "mp3.h"
#include "arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
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
    size_t avail = m->len - m->pos;
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

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <vectors-dir>\n", argv[0]);
        return 2;
    }
    char p_mp3[1024], p_pcm[1024];
    snprintf(p_mp3, sizeof p_mp3,
             "%s/sine_440hz_1s_44k_s16_stereo_128k.mp3", argv[1]);
    snprintf(p_pcm, sizeof p_pcm,
             "%s/sine_440hz_1s_44k_s16_stereo_128k.mp3.ref.pcm", argv[1]);

    size_t mp3_len = 0, ref_len = 0;
    uint8_t *mp3_bytes = slurp(p_mp3, &mp3_len);
    uint8_t *ref_pcm   = slurp(p_pcm, &ref_len);
    if (!mp3_bytes || !ref_pcm) {
        fprintf(stderr, "cannot read fixtures\n");
        return 2;
    }

    int fails = 0;

    /* Static arena — no libc malloc reaches the decoder. */
    static uint8_t arena_buf[256 * 1024];
    decoder_arena_t arena;
    decoder_arena_init(&arena, arena_buf, sizeof arena_buf);
    decoder_alloc_t alloc = decoder_arena_allocator(&arena);

    membuf_t mb = { mp3_bytes, mp3_len, 0 };
    decoder_source_t src = { mem_read, mem_seek, mem_tell, &mb };

    decoder_t d;
    int rc = mp3_open_stream(&d, &src, &alloc);
    fails += check("mp3_open_stream returns DECODER_OK", rc == DECODER_OK);
    fails += check("sample_rate == 44100", d.sample_rate == 44100);
    fails += check("channels == 2", d.channels == 2);
    if (rc != DECODER_OK) {
        return 1;
    }

    /* Decode all frames into a growing buffer. */
    size_t   cap = ref_len + 4096, len = 0;
    uint8_t *out = (uint8_t *)malloc(cap);
    int16_t  frame_buf[4096 * 2];
    int      got;
    while ((got = d.ops->decode(&d, frame_buf, 4096)) > 0) {
        size_t bytes = (size_t)got * 2u * sizeof(int16_t);
        if (len + bytes <= cap) {
            memcpy(out + len, frame_buf, bytes);
        }
        len += bytes;
    }
    d.ops->close(&d);

    fails += check("decoded byte count matches reference", len == ref_len);
    fails += check("decoded PCM is bit-exact vs reference",
                   len == ref_len && memcmp(out, ref_pcm, ref_len) == 0);

    printf("arena high-water: %zu bytes (of %zu)\n",
           arena.high_water, sizeof arena_buf);
    fails += check("arena never OOM'd", arena.oom == 0);

    free(out);
    free(mp3_bytes);
    free(ref_pcm);
    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
