/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/codecs/id3_meta_test.c — the ID3 reader (codecs/pvmp3/id3_meta.c).
 *
 * The SAME source the ARM build links into core.elf. Tags are built in RAM
 * here, byte by byte, because the cases that matter are the ones a real
 * library is full of and a hand-rolled parser gets wrong: three tag versions
 * with three different frame headers, four text encodings (two of them
 * UTF-16, which is what every Windows tagger writes), TCON's numeric genre
 * codes, and sizes that lie.
 *
 * Each fixture ends in two real MPEG frames so id3_meta_read() can do its
 * other job — report the stream's rate and duration — and return success.
 */

#include "id3_meta.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fails;

static void xpect(const char *what, int cond)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        g_fails++;
    }
}

static void xstr(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("FAIL: %s: want \"%s\" got \"%s\"\n", what, want, got);
        g_fails++;
    }
}

/* ---------- a tag builder ------------------------------------------------ */

typedef struct {
    uint8_t  buf[8192];
    size_t   len;
    int      major;
} tag_t;

static void tag_begin(tag_t *t, int major, int flags)
{
    memset(t, 0, sizeof *t);
    t->major = major;
    t->buf[0] = 'I'; t->buf[1] = 'D'; t->buf[2] = '3';
    t->buf[3] = (uint8_t)major; t->buf[4] = 0; t->buf[5] = (uint8_t)flags;
    t->len = 10;
}

/* Append a frame with a raw body. */
static void tag_frame(tag_t *t, const char *id, const uint8_t *body, size_t n,
                      uint16_t fflags)
{
    uint8_t *p = t->buf + t->len;
    if (t->major == 2) {
        memcpy(p, id, 3);
        p[3] = (uint8_t)(n >> 16); p[4] = (uint8_t)(n >> 8); p[5] = (uint8_t)n;
        t->len += 6;
    } else {
        memcpy(p, id, 4);
        if (t->major >= 4) {
            p[4] = (uint8_t)((n >> 21) & 0x7Fu);
            p[5] = (uint8_t)((n >> 14) & 0x7Fu);
            p[6] = (uint8_t)((n >> 7)  & 0x7Fu);
            p[7] = (uint8_t)( n        & 0x7Fu);
        } else {
            p[4] = (uint8_t)(n >> 24); p[5] = (uint8_t)(n >> 16);
            p[6] = (uint8_t)(n >> 8);  p[7] = (uint8_t)n;
        }
        p[8] = (uint8_t)(fflags >> 8); p[9] = (uint8_t)fflags;
        t->len += 10;
    }
    memcpy(t->buf + t->len, body, n);
    t->len += n;
}

/* Append a text frame: one encoding byte then the given bytes. */
static void tag_text(tag_t *t, const char *id, uint8_t enc,
                     const uint8_t *text, size_t n)
{
    uint8_t body[1024];
    body[0] = enc;
    memcpy(body + 1, text, n);
    tag_frame(t, id, body, n + 1u, 0);
}

static void tag_text_ascii(tag_t *t, const char *id, const char *s)
{
    tag_text(t, id, 0, (const uint8_t *)s, strlen(s));
}

/* Close the tag (writing the sync-safe size) and append two MPEG-1 128 kbps
 * 44.1 kHz stereo frames so the file is a playable stream. */
static void tag_end(tag_t *t)
{
    uint32_t body = (uint32_t)(t->len - 10u);
    t->buf[6] = (uint8_t)((body >> 21) & 0x7Fu);
    t->buf[7] = (uint8_t)((body >> 14) & 0x7Fu);
    t->buf[8] = (uint8_t)((body >> 7)  & 0x7Fu);
    t->buf[9] = (uint8_t)( body        & 0x7Fu);
    for (int i = 0; i < 2; i++) {
        uint8_t *p = t->buf + t->len;
        p[0] = 0xFFu; p[1] = 0xFBu; p[2] = 0x90u; p[3] = 0x00u;
        memset(p + 4, 0, 413);
        t->len += 417;
    }
}

/* ---------- a source over it --------------------------------------------- */

typedef struct { const uint8_t *p; size_t len, pos; } mem_t;

static size_t mem_read(void *ud, void *buf, size_t n)
{
    mem_t *m = (mem_t *)ud;
    size_t left = m->len - m->pos;
    if (n > left) n = left;
    memcpy(buf, m->p + m->pos, n);
    m->pos += n;
    return n;
}
static int mem_seek(void *ud, int off, int origin)
{
    mem_t *m = (mem_t *)ud;
    long base = (origin == DECODER_SEEK_SET) ? 0
              : (origin == DECODER_SEEK_END) ? (long)m->len : (long)m->pos;
    long np = base + off;
    if (np < 0 || (size_t)np > m->len) return 0;
    m->pos = (size_t)np;
    return 1;
}
static int64_t mem_tell(void *ud) { return (int64_t)((mem_t *)ud)->pos; }

static int read_tag(const tag_t *t, flac_meta_t *out)
{
    mem_t m = { t->buf, t->len, 0 };
    decoder_source_t src = { mem_read, mem_seek, mem_tell, &m };
    return id3_meta_read(&src, out);
}

/* ---------- cases -------------------------------------------------------- */

static void test_v23_text(void)
{
    tag_t t;
    flac_meta_t m;

    tag_begin(&t, 3, 0);
    tag_text_ascii(&t, "TIT2", "Eroica");
    tag_text_ascii(&t, "TPE1", "Czech National Symphony Orchestra");
    tag_text_ascii(&t, "TALB", "Symphony No. 3");
    tag_text_ascii(&t, "TCON", "Classical");
    tag_text_ascii(&t, "TRCK", "7/12");
    tag_text_ascii(&t, "TYER", "1804");
    tag_end(&t);

    xpect("a v2.3 tag over a real stream reads", read_tag(&t, &m) == 0);
    xpect("have is set", m.have == 1);
    xstr("v2.3 TIT2", m.title, "Eroica");
    xstr("v2.3 TPE1", m.artist, "Czech National Symphony Orchestra");
    xstr("v2.3 TALB", m.album, "Symphony No. 3");
    xstr("v2.3 TCON", m.genre, "Classical");
    xpect("TRCK \"7/12\" is track 7", m.track == 7);
    xpect("TYER is the year", m.year == 1804);
    xpect("the stream's rate is reported", m.sample_rate == 44100u);
    xpect("ReplayGain is out of scope for MP3", m.have_rg == 0);

    /* TPE2 is the fallback, and TPE1 wins whichever order they appear in. */
    tag_begin(&t, 3, 0);
    tag_text_ascii(&t, "TPE2", "Various Artists");
    tag_end(&t);
    xpect("a TPE2-only tag reads", read_tag(&t, &m) == 0);
    xstr("TPE2 is the artist when TPE1 is absent", m.artist, "Various Artists");

    tag_begin(&t, 3, 0);
    tag_text_ascii(&t, "TPE2", "Various Artists");
    tag_text_ascii(&t, "TPE1", "Beethoven");
    tag_end(&t);
    xpect("TPE2 then TPE1 reads", read_tag(&t, &m) == 0);
    xstr("TPE1 wins over TPE2 even when it comes second",
         m.artist, "Beethoven");
}

static void test_v22_and_v24(void)
{
    tag_t t;
    flac_meta_t m;

    /* v2.2: three-character ids and a 6-byte frame header. */
    tag_begin(&t, 2, 0);
    tag_text_ascii(&t, "TT2", "Old Tag");
    tag_text_ascii(&t, "TP1", "Someone");
    tag_text_ascii(&t, "TAL", "An Album");
    tag_text_ascii(&t, "TRK", "3");
    tag_end(&t);
    xpect("a v2.2 tag reads", read_tag(&t, &m) == 0);
    xstr("v2.2 TT2", m.title, "Old Tag");
    xstr("v2.2 TP1", m.artist, "Someone");
    xstr("v2.2 TAL", m.album, "An Album");
    xpect("v2.2 TRK", m.track == 3);

    /* v2.4: SYNC-SAFE frame sizes. A parser that reads them as plain BE32
     * lands mid-frame for any size with bit 7 set in a byte — build a body
     * long enough (>= 128) that the two readings differ. */
    tag_begin(&t, 4, 0);
    char longtitle[200];
    memset(longtitle, 'a', sizeof longtitle - 1);
    longtitle[sizeof longtitle - 1] = '\0';
    tag_text_ascii(&t, "TIT2", longtitle);
    tag_text_ascii(&t, "TPE1", "After The Long One");
    tag_text_ascii(&t, "TDRC", "2021-05-01");
    tag_end(&t);
    xpect("a v2.4 tag reads", read_tag(&t, &m) == 0);
    xpect("v2.4 title is truncated to the field, not torn",
          strlen(m.title) == sizeof m.title - 1);
    xstr("the frame AFTER a >127-byte one is found (sync-safe sizes)",
         m.artist, "After The Long One");
    xpect("TDRC \"2021-05-01\" is the year", m.year == 2021);

    /* v2.4's data-length indicator puts four extra bytes in front of a body. */
    tag_begin(&t, 4, 0);
    {
        uint8_t body[64];
        const char *s = "Indicated";
        body[0] = 0; body[1] = 0; body[2] = 0; body[3] = 10;  /* the DLI */
        body[4] = 0;                                          /* encoding */
        memcpy(body + 5, s, strlen(s));
        tag_frame(&t, "TIT2", body, 5u + strlen(s), 0x0001u);
    }
    tag_end(&t);
    xpect("a v2.4 frame with a data-length indicator reads",
          read_tag(&t, &m) == 0);
    xstr("...and its four extra bytes are skipped", m.title, "Indicated");
}

static void test_encodings(void)
{
    tag_t t;
    flac_meta_t m;

    /* ISO-8859-1 to UTF-8: 0xE9 is U+00E9, two bytes out. */
    tag_begin(&t, 3, 0);
    {
        const uint8_t latin[] = { 'B', 'e', 'y', 'o', 'n', 'c', 0xE9 };
        tag_text(&t, "TIT2", 0, latin, sizeof latin);
    }
    tag_end(&t);
    xpect("a Latin-1 frame reads", read_tag(&t, &m) == 0);
    xstr("Latin-1 is converted to UTF-8", m.title, "Beyonc\xC3\xA9");

    /* UTF-8 passes through. */
    tag_begin(&t, 3, 0);
    tag_text(&t, "TIT2", 3, (const uint8_t *)"Bj\xC3\xB6rk", 6);
    tag_end(&t);
    xpect("a UTF-8 frame reads", read_tag(&t, &m) == 0);
    xstr("UTF-8 passes through", m.title, "Bj\xC3\xB6rk");

    /* UTF-16LE with a BOM — what every Windows tagger writes. The old reader
     * dropped the high byte and put '?' in the marquee. */
    tag_begin(&t, 3, 0);
    {
        const uint8_t u16le[] = {
            0xFF, 0xFE, 'B', 0, 'j', 0, 0xF6, 0x00, 'r', 0, 'k', 0
        };
        tag_text(&t, "TIT2", 1, u16le, sizeof u16le);
    }
    tag_end(&t);
    xpect("a UTF-16LE frame reads", read_tag(&t, &m) == 0);
    xstr("UTF-16LE converts to real UTF-8", m.title, "Bj\xC3\xB6rk");

    /* UTF-16BE with a BOM. */
    tag_begin(&t, 3, 0);
    {
        const uint8_t u16be[] = {
            0xFE, 0xFF, 0, 'B', 0, 'j', 0x00, 0xF6, 0, 'r', 0, 'k'
        };
        tag_text(&t, "TIT2", 1, u16be, sizeof u16be);
    }
    tag_end(&t);
    xpect("a UTF-16BE-with-BOM frame reads", read_tag(&t, &m) == 0);
    xstr("the BOM picks the endianness", m.title, "Bj\xC3\xB6rk");

    /* Encoding 2 is UTF-16BE with NO BOM. */
    tag_begin(&t, 3, 0);
    {
        const uint8_t u16be[] = { 0, 'B', 0, 'j', 0x00, 0xF6, 0, 'r', 0, 'k' };
        tag_text(&t, "TIT2", 2, u16be, sizeof u16be);
    }
    tag_end(&t);
    xpect("an encoding-2 frame reads", read_tag(&t, &m) == 0);
    xstr("encoding 2 is UTF-16BE without a BOM", m.title, "Bj\xC3\xB6rk");

    /* A surrogate pair: U+1D11E, the treble clef, is four UTF-8 bytes. */
    tag_begin(&t, 3, 0);
    {
        const uint8_t pair[] = { 0xFF, 0xFE, 0x34, 0xD8, 0x1E, 0xDD };
        tag_text(&t, "TIT2", 1, pair, sizeof pair);
    }
    tag_end(&t);
    xpect("a surrogate pair reads", read_tag(&t, &m) == 0);
    xstr("a surrogate pair becomes one 4-byte UTF-8 sequence",
         m.title, "\xF0\x9D\x84\x9E");

    /* A value terminator ends the value: v2.4 allows several per frame and we
     * take the first, which is what ffprobe reports. */
    tag_begin(&t, 4, 0);
    {
        const uint8_t two[] = { 'F', 'i', 'r', 's', 't', 0, 'S', 'e', 'c' };
        tag_text(&t, "TIT2", 0, two, sizeof two);
    }
    tag_end(&t);
    xpect("a multi-value v2.4 frame reads", read_tag(&t, &m) == 0);
    xstr("only the first value is kept", m.title, "First");
}

static void test_tcon(void)
{
    static const struct { const char *in, *want; } cases[] = {
        { "17",       "Rock"  },
        { "(17)",     "Rock"  },
        { "(17)Rock", "Rock"  },
        { "(RX)",     "Remix" },
        { "(CR)",     "Cover" },
        { "Rock",     "Rock"  },
        { "(255)",    "(255)" },
        { "(0)",      "Blues" },
        { "0",        "Blues" },
        { "(17)Progressive Rock", "Progressive Rock" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        tag_t t;
        flac_meta_t m;
        tag_begin(&t, 3, 0);
        tag_text_ascii(&t, "TCON", cases[i].in);
        tag_end(&t);
        if (read_tag(&t, &m) != 0) {
            printf("FAIL: TCON \"%s\": read failed\n", cases[i].in);
            g_fails++;
            continue;
        }
        if (strcmp(m.genre, cases[i].want) != 0) {
            printf("FAIL: TCON \"%s\": want \"%s\" got \"%s\"\n",
                   cases[i].in, cases[i].want, m.genre);
            g_fails++;
        }
    }
}

static void test_awkward(void)
{
    tag_t t;
    flac_meta_t m;

    /* An extended header, whose size the frame walk must step over. v2.3
     * states the size of what follows; v2.4's is sync-safe and includes
     * itself. Get it wrong and the first "frame id" is padding. */
    tag_begin(&t, 3, 0x40);
    {
        uint8_t ext[16] = { 0, 0, 0, 6, 0, 0, 0, 0, 0, 0 };
        memcpy(t.buf + t.len, ext, 10);
        t.len += 10;
    }
    tag_text_ascii(&t, "TIT2", "Past The Extended Header");
    tag_end(&t);
    xpect("a v2.3 extended header is skipped", read_tag(&t, &m) == 0);
    xstr("...and the frames after it are read",
         m.title, "Past The Extended Header");

    tag_begin(&t, 4, 0x40);
    {
        uint8_t ext[6] = { 0, 0, 0, 6, 1, 0 };
        memcpy(t.buf + t.len, ext, 6);
        t.len += 6;
    }
    tag_text_ascii(&t, "TIT2", "Past The v2.4 One");
    tag_end(&t);
    xpect("a v2.4 extended header is skipped", read_tag(&t, &m) == 0);
    xstr("...and the frames after it are read", m.title, "Past The v2.4 One");

    /* An APIC frame must be stepped over by its size, not scanned: a 4 KB
     * cover sitting before the title is the normal case. */
    tag_begin(&t, 3, 0);
    {
        static uint8_t apic[4096];
        memset(apic, 0xAB, sizeof apic);
        memcpy(apic, "\0image/jpeg\0\3cover\0", 19);
        tag_frame(&t, "APIC", apic, sizeof apic, 0);
    }
    tag_text_ascii(&t, "TIT2", "After The Cover");
    tag_end(&t);
    xpect("a tag with embedded art reads", read_tag(&t, &m) == 0);
    xstr("APIC is skipped by size", m.title, "After The Cover");

    /* Padding: a run of zero bytes where a frame id would be ends the walk. */
    tag_begin(&t, 3, 0);
    tag_text_ascii(&t, "TIT2", "Before Padding");
    memset(t.buf + t.len, 0, 256);
    t.len += 256;
    tag_end(&t);
    xpect("a padded tag reads", read_tag(&t, &m) == 0);
    xstr("...and the frames before the padding are kept",
         m.title, "Before Padding");

    /* An unsynchronised tag: the frame sizes no longer describe the bytes on
     * disk, so the tag is treated as opaque rather than mis-parsed. The
     * stream behind it must still report its rate and duration. */
    tag_begin(&t, 3, 0x80);
    tag_text_ascii(&t, "TIT2", "Unsynchronised");
    tag_end(&t);
    xpect("an unsynchronised tag still reports the stream",
          read_tag(&t, &m) == 0 && m.have == 1 && m.sample_rate == 44100u);
    xstr("...with no text read out of it", m.title, "");

    /* A frame whose size runs past the tag stops the walk instead of reading
     * into the audio. */
    tag_begin(&t, 3, 0);
    {
        uint8_t *p = t.buf + t.len;
        memcpy(p, "TIT2", 4);
        p[4] = 0; p[5] = 0; p[6] = 0xFFu; p[7] = 0xFFu;   /* 64 KB */
        p[8] = 0; p[9] = 0;
        t.len += 10;
    }
    tag_end(&t);
    xpect("a frame claiming more than the tag holds is ignored",
          read_tag(&t, &m) == 0);
    xstr("...leaving the title empty", m.title, "");

    /* A tag whose own size runs past the end of the file — a truncated
     * download, or a tagger that crashed. The frames that ARE there are still
     * read, the walk stops at the end of the file instead of running away,
     * and the scan ignores the bogus size and finds the audio behind it, so
     * the track still plays. */
    tag_begin(&t, 3, 0);
    tag_text_ascii(&t, "TIT2", "Truncated");
    tag_end(&t);
    t.buf[6] = 0x7Fu; t.buf[7] = 0x7Fu; t.buf[8] = 0x7Fu; t.buf[9] = 0x7Fu;
    xpect("a tag claiming more bytes than the file holds still plays",
          read_tag(&t, &m) == 0 && m.have == 1 && m.sample_rate == 44100u);
    xstr("...and the frames that are present are still read",
         m.title, "Truncated");

    /* No tag at all: still a playable stream, with a duration. */
    {
        static tag_t bare;               /* 8 KB: not a stack frame */
        memset(&bare, 0, sizeof bare);
        bare.major = 3;
        for (int i = 0; i < 15; i++) {
            uint8_t *p = bare.buf + bare.len;
            p[0] = 0xFFu; p[1] = 0xFBu; p[2] = 0x90u; p[3] = 0x00u;
            memset(p + 4, 0, 413);
            bare.len += 417;
        }
        xpect("an untagged MP3 still reads", read_tag(&bare, &m) == 0);
        xpect("...with empty text and a real rate",
              m.title[0] == '\0' && m.sample_rate == 44100u);
    }

    /* Not an MP3 at all. */
    {
        static tag_t junk;
        memset(&junk, 0, sizeof junk);
        memcpy(junk.buf, "fLaC", 4);
        junk.len = 4096;
        xpect("a non-MPEG file is declined", read_tag(&junk, &m) == -1);
    }
}

/*
 * A tag whose bulk is an embedded cover, with the text frames BEHIND it. The
 * walk has a budget, and the budget has to be spent on bytes actually read —
 * a 2 MB APIC is skipped with a seek, so it costs nothing and the title on the
 * far side of it is still found. Capping the DECLARED tag size instead threw
 * away the metadata of every file whose art happened to come first, which is
 * most of the files an iTunes library is made of.
 */
static void test_huge_cover_does_not_cost_the_title(void)
{
    enum { ART = 2u << 20 };                 /* 2 MB, twice the read budget */
    static uint8_t file[ART + 16384];
    const char *title = "Behind Two Megabytes Of Art";

    memset(file, 0, sizeof file);
    file[0] = 'I'; file[1] = 'D'; file[2] = '3';
    file[3] = 3; file[4] = 0; file[5] = 0;
    size_t n = 10;

    /* APIC: a 10-byte body prologue then ART bytes of "image". */
    uint8_t *p = file + n;
    uint32_t apic = 10u + ART;
    memcpy(p, "APIC", 4);
    p[4] = (uint8_t)(apic >> 24); p[5] = (uint8_t)(apic >> 16);
    p[6] = (uint8_t)(apic >> 8);  p[7] = (uint8_t)apic;
    p[8] = 0; p[9] = 0;
    memcpy(p + 10, "\0image/jpeg\0\3x\0", 16 > apic ? apic : 16);
    memset(p + 20, 0xAB, ART);
    n += 10u + apic;

    /* TIT2, after it. */
    p = file + n;
    uint32_t tlen = (uint32_t)(1u + strlen(title));
    memcpy(p, "TIT2", 4);
    p[4] = (uint8_t)(tlen >> 24); p[5] = (uint8_t)(tlen >> 16);
    p[6] = (uint8_t)(tlen >> 8);  p[7] = (uint8_t)tlen;
    p[8] = 0; p[9] = 0;
    p[10] = 0;                                /* ISO-8859-1 */
    memcpy(p + 11, title, strlen(title));
    n += 10u + tlen;

    uint32_t body = (uint32_t)(n - 10u);
    file[6] = (uint8_t)((body >> 21) & 0x7Fu);
    file[7] = (uint8_t)((body >> 14) & 0x7Fu);
    file[8] = (uint8_t)((body >> 7)  & 0x7Fu);
    file[9] = (uint8_t)( body        & 0x7Fu);

    for (int i = 0; i < 4; i++) {            /* a real stream behind it all */
        uint8_t *f = file + n;
        f[0] = 0xFFu; f[1] = 0xFBu; f[2] = 0x90u; f[3] = 0x00u;
        memset(f + 4, 0, 413);
        n += 417;
    }

    mem_t m = { file, n, 0 };
    decoder_source_t src = { mem_read, mem_seek, mem_tell, &m };
    flac_meta_t meta;
    xpect("a file with 2 MB of embedded art reads",
          id3_meta_read(&src, &meta) == 0);
    xstr("...and the title behind the art is still found", meta.title, title);
}

int main(void)
{
    test_v23_text();
    test_v22_and_v24();
    test_encodings();
    test_tcon();
    test_awkward();
    test_huge_cover_does_not_cost_the_title();

    if (g_fails) {
        printf("id3_meta_test: %d check%s failed\n",
               g_fails, g_fails == 1 ? "" : "s");
        return 1;
    }
    printf("ok: id3_meta (v2.2/2.3/2.4, four encodings, TCON, awkward tags)\n");
    return 0;
}
