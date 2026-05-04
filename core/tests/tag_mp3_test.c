/* core/tests/tag_mp3_test.c — TCON numeric -> genre-name resolution. */

#include "../codecs/dr_mp3/tag_mp3.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FAIL(fmt, ...) \
    do { fprintf(stderr, "[tag_mp3_test:%d] FAIL: " fmt "\n", __LINE__, ##__VA_ARGS__); \
         return 1; } while (0)

static size_t build_tag_with_tcon(uint8_t *buf, size_t cap, const char *tcon_text) {
    size_t text_len = strlen(tcon_text);
    size_t fsize    = 1 + text_len;
    size_t tag_size = 10 + fsize;
    size_t total    = 10 + tag_size;
    if (total > cap) return 0;

    /* "ID3" v2.3.0, no flags, synchsafe tag_size. */
    buf[0] = 'I'; buf[1] = 'D'; buf[2] = '3';
    buf[3] = 3;   buf[4] = 0;   buf[5] = 0;
    buf[6] = (uint8_t)((tag_size >> 21) & 0x7f);
    buf[7] = (uint8_t)((tag_size >> 14) & 0x7f);
    buf[8] = (uint8_t)((tag_size >>  7) & 0x7f);
    buf[9] = (uint8_t)((tag_size      ) & 0x7f);

    uint8_t *frame = buf + 10;
    frame[0] = 'T'; frame[1] = 'C'; frame[2] = 'O'; frame[3] = 'N';
    /* v2.3 frame size: plain BE32. */
    frame[4] = (uint8_t)((fsize >> 24) & 0xff);
    frame[5] = (uint8_t)((fsize >> 16) & 0xff);
    frame[6] = (uint8_t)((fsize >>  8) & 0xff);
    frame[7] = (uint8_t)((fsize      ) & 0xff);
    frame[8] = 0; frame[9] = 0;
    /* body: encoding=0 (ISO-8859-1), then text. */
    frame[10] = 0;
    memcpy(frame + 11, tcon_text, text_len);
    return total;
}

static int check(const char *tcon_text, const char *want_genre) {
    uint8_t buf[256];
    size_t len = build_tag_with_tcon(buf, sizeof(buf), tcon_text);
    if (len == 0) FAIL("build failed for \"%s\"", tcon_text);
    audio_tags_t out;
    if (tag_mp3_read(buf, len, &out) != 0) FAIL("tag_mp3_read rc != 0 for \"%s\"", tcon_text);
    if (strcmp(out.genre, want_genre) != 0)
        FAIL("TCON \"%s\": want \"%s\" got \"%s\"", tcon_text, want_genre, out.genre);
    audio_tags_free(&out);
    return 0;
}

int main(void) {
    int failures = 0;
    failures += check("17",         "Rock");
    failures += check("(17)",       "Rock");
    failures += check("(17)Rock",   "Rock");
    failures += check("(RX)",       "Remix");
    failures += check("(CR)",       "Cover");
    failures += check("Rock",       "Rock");
    failures += check("(255)",      "(255)");
    failures += check("(0)",        "Blues");
    failures += check("0",          "Blues");
    if (failures > 0) {
        fprintf(stderr, "tag_mp3_test: %d case%s failed\n",
                failures, failures == 1 ? "" : "s");
        return 1;
    }
    fprintf(stdout, "ok: tag_mp3 TCON resolution passed\n");
    return 0;
}
