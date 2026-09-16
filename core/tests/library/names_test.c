/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/library/names_test.c — the library's name handling (core/library/names.c)
 * on the host.
 *
 * THE POINT OF THIS FILE. Every function it exercises was, until names.c
 * existed, a `static` inside kernel/main.c — which meson builds only for
 * target == 'hw'. The one piece the project could not live without checking
 * (name_hash) was tested against a verbatim COPY kept honest by a diff script;
 * everything else — the display-name copy every row and queue entry is made
 * by, the "Artist - Album" split the Artists menu is built from, the
 * extension classifier that decides what is playable, the formatters on the
 * Now Playing screen — had no test at all. This compiles the same names.c the
 * ARM build links.
 *
 * name_hash() itself is asserted against the golden vector table in
 * tests/kernel/name_hash_test.c (shared with the host tool); this file only
 * pins its offset basis and the decoder underneath it.
 *
 * Buffers that the functions write into are bracketed with sentinel bytes:
 * a bound that is off by one is invisible to a "did the string come out
 * right" assertion, and these are the functions a 63-byte FAT name reaches.
 */

#include <stdio.h>
#include <string.h>

#include "../../library/names.h"
#include "../xfail.h"

/* A NAME_MAX+1 destination with a guard byte on either side. */
typedef struct {
    char pre;
    char s[NAME_MAX + 1];
    char post;
} guarded_t;

static void guard_init(guarded_t *g)
{
    memset(g, 0x7E, sizeof *g);       /* '~': visible if it leaks into output */
}

static int guard_ok(const guarded_t *g)
{
    return g->pre == 0x7E && g->post == 0x7E;
}

/* One-step decode helper: returns the codepoint, writes the bytes consumed. */
static int decode(const char *s, int *consumed)
{
    const unsigned char *p = (const unsigned char *)s;
    int cp = mn_utf8_next(&p);
    *consumed = (int)(p - (const unsigned char *)s);
    return cp;
}

int main(void)
{
    xfail_ctx c = { "names", 0, 0, 0 };

    /* ---- name_eq_ci ---------------------------------------------------- */
    xpect(&c, "eq_ci: ASCII case is ignored", name_eq_ci("folder.ART", "Folder.art"));
    xpect(&c, "eq_ci: a prefix is not a match", !name_eq_ci("Music", "Music2") &&
                                                !name_eq_ci("Music2", "Music"));
    xpect(&c, "eq_ci: empty equals empty only", name_eq_ci("", "") && !name_eq_ci("", "a"));
    /* The fold is ASCII-only; a Latin-1 capital is a different byte. */
    xpect(&c, "eq_ci: non-ASCII bytes compare raw",
          !name_eq_ci("\303\211douard", "\303\251douard") &&
           name_eq_ci("\303\251douard", "\303\251DOUARD"));

    /* ---- is_junk_dir ----------------------------------------------------- */
    xpect(&c, "junk: any dotfolder", is_junk_dir(".Trashes") && is_junk_dir(".fseventsd"));
    xpect(&c, "junk: system folders in any case",
          is_junk_dir("iPod_Control") && is_junk_dir("IPOD_CONTROL") &&
          is_junk_dir("$RECYCLE.BIN") && is_junk_dir("System Volume Information"));
    xpect(&c, "junk: an album folder is not junk",
          !is_junk_dir("Adele - 25") && !is_junk_dir("Notes2") && !is_junk_dir(""));

    /* ---- copy_display_name ----------------------------------------------- */
    {
        guarded_t g;
        guard_init(&g);
        copy_display_name(g.s, "01. Intentions.flac", 1);
        xpect(&c, "copy: drop_ext trims the extension",
              strcmp(g.s, "01. Intentions") == 0 && guard_ok(&g));
        copy_display_name(g.s, "01. Intentions.flac", 0);
        xpect(&c, "copy: without drop_ext the name is kept whole",
              strcmp(g.s, "01. Intentions.flac") == 0);
        /* A dot at index 0 is not an extension: dot > 0 is the rule. */
        copy_display_name(g.s, ".hidden", 1);
        xpect(&c, "copy: a leading dot is not an extension",
              strcmp(g.s, ".hidden") == 0);
        /* drop_ext cuts at the LAST dot whatever precedes it. That is fine for
         * an on-disk name (which always ends in .flac) and exactly why the
         * index's 63-byte file[] field goes through trim_audio_ext instead. */
        copy_display_name(g.s, "16. TRAGIC (feat. Someone", 1);
        xpect(&c, "copy: drop_ext cuts at the last dot, not a known extension",
              strcmp(g.s, "16. TRAGIC (feat") == 0);
        copy_display_name(g.s, "a\001b\tc\037d", 0);
        xpect(&c, "copy: C0 control bytes are dropped", strcmp(g.s, "abcd") == 0);
        copy_display_name(g.s, "\303\211douard \342\200\223 Live", 0);
        xpect(&c, "copy: UTF-8 multibyte passes through",
              strcmp(g.s, "\303\211douard \342\200\223 Live") == 0);

        char longname[NAME_MAX * 2 + 1];
        memset(longname, 'x', sizeof longname - 1);
        longname[sizeof longname - 1] = '\0';
        guard_init(&g);
        copy_display_name(g.s, longname, 0);
        xpect(&c, "copy: truncates to NAME_MAX bytes + NUL and no further",
              (int)strlen(g.s) == NAME_MAX && guard_ok(&g));
        /* The extension is found in the ORIGINAL, then the copy is bounded:
         * a long name whose ".flac" sits past NAME_MAX still loses it. */
        memcpy(longname + sizeof longname - 6, ".flac", 6);
        copy_display_name(g.s, longname, 1);
        xpect(&c, "copy: drop_ext works on the untruncated name",
              (int)strlen(g.s) == NAME_MAX && g.s[NAME_MAX - 1] == 'x');
        copy_display_name(g.s, "", 1);
        xpect(&c, "copy: empty in, empty out", g.s[0] == '\0');
    }

    /* ---- name_bind_exact ------------------------------------------------- */
    {
        /* The premise: these two hash alike, so they land in one bucket and
         * only the tiebreak can tell them apart. */
        xpect(&c, "bind: It's and It\xe2\x80\x99s share a locator hash",
              name_hash("01. It's Fine.flac") ==
              name_hash("01. It\xe2\x80\x99s Fine.flac"));
        xpect(&c, "bind: the record's stem is the on-disk stem",
              name_bind_exact("01. It's Fine", "01. It's Fine.flac", 1));
        xpect(&c, "bind: a folded-apostrophe twin is NOT an exact match",
              !name_bind_exact("01. It's Fine", "01. It\xe2\x80\x99s Fine.flac", 1));
        xpect(&c, "bind: ...and the other way round",
              !name_bind_exact("01. It\xe2\x80\x99s Fine", "01. It's Fine.flac", 1) &&
              name_bind_exact("01. It\xe2\x80\x99s Fine", "01. It\xe2\x80\x99s Fine.flac", 1));
        xpect(&c, "bind: case is not folded either",
              !name_bind_exact("01. it's fine", "01. It's Fine.flac", 1));
        xpect(&c, "bind: a folder compares whole (no extension trimmed)",
              name_bind_exact("Adele - 25", "Adele - 25", 0) &&
              !name_bind_exact("Adele - 25", "Adele - 25.1", 0));
        xpect(&c, "bind: the disk name is copied like the stored one was",
              name_bind_exact("ab", "a\tb.flac", 1));
        xpect(&c, "bind: empty stored matches nothing on disk",
              !name_bind_exact("", "x.flac", 1) && name_bind_exact("", "", 0));

        /* A name past the 63-byte index field: the record holds the cut
         * (then trim_audio_ext, which finds no extension to trim), the disk
         * holds the whole thing. Never exact — the caller falls back. */
        char longname[NAME_MAX + 16];
        memset(longname, 'x', sizeof longname - 1);
        longname[sizeof longname - 1] = '\0';
        memcpy(longname + sizeof longname - 6, ".flac", 6);
        char stored[NAME_MAX + 1];
        memcpy(stored, longname, NAME_MAX - 1);
        stored[NAME_MAX - 1] = '\0';
        trim_audio_ext(stored);
        xpect(&c, "bind: a stem that outgrew the field is never exact",
              !name_bind_exact(stored, longname, 1));
    }

    /* ---- mn_utf8_next ---------------------------------------------------- */
    {
        int n;
        xpect(&c, "utf8: ASCII is one byte", decode("A", &n) == 'A' && n == 1);
        xpect(&c, "utf8: NUL is -1 and no progress", decode("", &n) == -1 && n == 0);
        xpect(&c, "utf8: 2-byte U+00E9", decode("\303\251", &n) == 0xE9 && n == 2);
        xpect(&c, "utf8: 3-byte U+20AC", decode("\342\202\254", &n) == 0x20AC && n == 3);
        xpect(&c, "utf8: 4-byte U+1F600", decode("\360\237\230\200", &n) == 0x1F600 && n == 4);
        /* Overlongs are consumed whole but yield U+FFFD — "C0 80" must never
         * become a NUL, "E0 80 AF" never a '/'. */
        xpect(&c, "utf8: overlong NUL rejected", decode("\300\200", &n) == 0xFFFD && n == 2);
        xpect(&c, "utf8: overlong slash rejected", decode("\340\200\257", &n) == 0xFFFD && n == 3);
        xpect(&c, "utf8: surrogate rejected", decode("\355\240\200", &n) == 0xFFFD && n == 3);
        xpect(&c, "utf8: above U+10FFFF rejected", decode("\364\220\200\200", &n) == 0xFFFD && n == 4);
        /* Malformed input makes one byte of progress so a loop always ends. */
        xpect(&c, "utf8: stray continuation byte", decode("\200a", &n) == 0xFFFD && n == 1);
        xpect(&c, "utf8: truncated lead at NUL", decode("\342\200", &n) == 0xFFFD && n == 1);
        xpect(&c, "utf8: lead followed by ASCII", decode("\303A", &n) == 0xFFFD && n == 1);
        xpect(&c, "utf8: 0xFF is not a lead", decode("\377", &n) == 0xFFFD && n == 1);
    }
    xpect(&c, "hash: empty name is the FNV-1a offset basis",
          name_hash("") == 0x811C9DC5u);

    /* ---- split_artist_album ---------------------------------------------- */
    {
        guarded_t a, b;
        guard_init(&a); guard_init(&b);
        split_artist_album("Adele - 25", a.s, b.s);
        xpect(&c, "split: Artist - Album",
              strcmp(a.s, "Adele") == 0 && strcmp(b.s, "25") == 0 &&
              guard_ok(&a) && guard_ok(&b));
        split_artist_album("A - B - C", a.s, b.s);
        xpect(&c, "split: on the FIRST separator",
              strcmp(a.s, "A") == 0 && strcmp(b.s, "B - C") == 0);
        split_artist_album("No Separator", a.s, b.s);
        xpect(&c, "split: no separator -> empty artist, whole name as album",
              a.s[0] == '\0' && strcmp(b.s, "No Separator") == 0);
        split_artist_album("A-B", a.s, b.s);
        xpect(&c, "split: a bare hyphen is not a separator",
              a.s[0] == '\0' && strcmp(b.s, "A-B") == 0);
        split_artist_album(" - X", a.s, b.s);
        xpect(&c, "split: separator at 0 -> empty artist",
              a.s[0] == '\0' && strcmp(b.s, "X") == 0);

        char longname[NAME_MAX * 2 + 8];
        memset(longname, 'y', NAME_MAX + 5);
        memcpy(longname + NAME_MAX + 5, " - Z", 5);
        guard_init(&a); guard_init(&b);
        split_artist_album(longname, a.s, b.s);
        xpect(&c, "split: an over-long artist is bounded to NAME_MAX",
              (int)strlen(a.s) == NAME_MAX && strcmp(b.s, "Z") == 0 &&
              guard_ok(&a) && guard_ok(&b));
    }

    /* ---- track_display --------------------------------------------------- */
    xpect(&c, "track: 'NN. Title' -> Title", strcmp(track_display("01. Intro"), "Intro") == 0);
    xpect(&c, "track: 'NN.Title' -> Title", strcmp(track_display("07.Intro"), "Intro") == 0);
    xpect(&c, "track: many digits and spaces", strcmp(track_display("007.   Bond"), "Bond") == 0);
    xpect(&c, "track: a title that starts with a number is kept",
          strcmp(track_display("99 Luftballons"), "99 Luftballons") == 0);
    xpect(&c, "track: digits then nothing is kept", strcmp(track_display("12."), "12.") == 0 &&
                                                  strcmp(track_display("12. "), "12. ") == 0);
    xpect(&c, "track: no prefix is untouched", strcmp(track_display("Intro"), "Intro") == 0);
    xpect(&c, "track: a dot with no digits is not a prefix",
          strcmp(track_display(".5 Song"), ".5 Song") == 0);

    /* ---- classify_ext ---------------------------------------------------- */
    xpect(&c, "ext: .flac / .fla in any case are FLAC",
          classify_ext("a.flac") == 0 && classify_ext("a.FLAC") == 0 &&
          classify_ext("a.Fla") == 0);
    xpect(&c, "ext: unknown, missing and empty extensions are skipped",
          classify_ext("a.ogg") == -1 && classify_ext("noext") == -1 &&
          classify_ext("a.") == -1 && classify_ext("a.fl") == -1);
    xpect(&c, "ext: only the LAST dot counts", classify_ext("x.flac.bak") == -1);
    xpect(&c, "ext: .mp3 in any case is MP3",
          classify_ext("a.mp3") == 1 && classify_ext("a.MP3") == 1 &&
          classify_ext("a.Mp3") == 1);
    xpect(&c, "ext: an extension that merely starts with mp3 is not MP3",
          classify_ext("a.mp3bak") == -1 && classify_ext("a.mp") == -1);
    /* The extension buffer holds four characters, so the match on "FLAC"
     * used to accept any longer extension beginning with those letters and
     * hand it to the decoder. classify_ext now rejects an extension it did
     * not consume to the end; this is what keeps that fixed. */
    xpect(&c, "ext: an extension that merely starts with flac is not FLAC",
          classify_ext("song.flacc") == -1 && classify_ext("song.flacbak") == -1);
    xpect(&c, "ext: a long extension starting with a short one is not FLAC",
          classify_ext("song.flax") == -1 && classify_ext("song.flacx") == -1);

    /* ---- trim_audio_ext -------------------------------------------------- */
    {
        char s[NAME_MAX + 1];
        strcpy(s, "01. Song.flac");
        trim_audio_ext(s);
        xpect(&c, "trim: a known extension is dropped", strcmp(s, "01. Song") == 0);
        /* The bug this exists for: an index file[] field is the first 63 bytes
         * of a longer name, so the ".flac" is gone and the last '.' is inside
         * the title. Cutting there produced "16"; now nothing is cut. */
        strcpy(s, "16. TRAGIC (feat. Someone Very Long Named Indeed and Others");
        trim_audio_ext(s);
        xpect(&c, "trim: an unrecognised extension is left alone",
              strcmp(s, "16. TRAGIC (feat. Someone Very Long Named Indeed and Others") == 0);
        strcpy(s, "noext");
        trim_audio_ext(s);
        xpect(&c, "trim: no dot, no change", strcmp(s, "noext") == 0);
    }

    /* ---- title_cmp / artist_key / initial_of ----------------------------- */
    xpect(&c, "cmp: case-insensitive equality", title_cmp("Hello", "hELLO") == 0);
    xpect(&c, "cmp: ordered, strcmp-signed", title_cmp("a", "b") < 0 && title_cmp("b", "a") > 0);
    xpect(&c, "cmp: a prefix sorts first", title_cmp("a", "ab") < 0 && title_cmp("ab", "a") > 0);
    /* strcmp would put "B" before "a" (0x42 < 0x61); the fold must not. */
    xpect(&c, "cmp: folds before comparing", title_cmp("B", "a") > 0 && title_cmp("a", "B") < 0);
    xpect(&c, "cmp: non-ASCII is not folded",
          title_cmp("\303\211", "\303\251") != 0);

    xpect(&c, "key: 'The ' is skipped in any case",
          strcmp(artist_key("The Kid LAROI"), "Kid LAROI") == 0 &&
          strcmp(artist_key("the weeknd"), "weeknd") == 0 &&
          strcmp(artist_key("THE WHO"), "WHO") == 0);
    xpect(&c, "key: 'The' must be a whole word",
          strcmp(artist_key("Theo"), "Theo") == 0 && strcmp(artist_key("The"), "The") == 0 &&
          strcmp(artist_key("Th"), "Th") == 0 && artist_key("")[0] == '\0');
    xpect(&c, "key+cmp: the dedup rule merges The/case variants",
          title_cmp(artist_key("The Kid LAROI"), artist_key("kid laroi")) == 0);

    xpect(&c, "initial: letters uppercase", initial_of("adele") == 'A' && initial_of("Zed") == 'Z');
    xpect(&c, "initial: leading spaces skipped", initial_of("  beck") == 'B');
    xpect(&c, "initial: digits, punctuation, non-ASCII and empty are '#'",
          initial_of("99 Luftballons") == '#' && initial_of("(Untitled)") == '#' &&
          initial_of("\303\251lan") == '#' && initial_of("") == '#');

    /* ---- u32_to_dec / fmt_time / fmt_count ------------------------------- */
    {
        struct { char pre; char s[11]; char post; } d;
        memset(&d, 0x7E, sizeof d);
        xpect(&c, "dec: zero", u32_to_dec(d.s, 0) == 1 && strcmp(d.s, "0") == 0);
        xpect(&c, "dec: ordinary", u32_to_dec(d.s, 100) == 3 && strcmp(d.s, "100") == 0);
        xpect(&c, "dec: UINT32_MAX fits 10 digits + NUL",
              u32_to_dec(d.s, 4294967295u) == 10 && strcmp(d.s, "4294967295") == 0 &&
              d.pre == 0x7E && d.post == 0x7E);

        struct { char pre; char s[FMT_TIME_MAX]; char post; } t;
        memset(&t, 0x7E, sizeof t);
        fmt_time(t.s, 0);     xpect(&c, "time: 0 -> 0:00", strcmp(t.s, "0:00") == 0);
        fmt_time(t.s, 59);    xpect(&c, "time: 59 -> 0:59", strcmp(t.s, "0:59") == 0);
        fmt_time(t.s, 60);    xpect(&c, "time: 60 -> 1:00", strcmp(t.s, "1:00") == 0);
        fmt_time(t.s, 3599);  xpect(&c, "time: 3599 -> 59:59", strcmp(t.s, "59:59") == 0);
        /* The old two-digit minute write showed 123 minutes as "23:00". */
        fmt_time(t.s, 7380);  xpect(&c, "time: minutes are not capped at two digits",
                                    strcmp(t.s, "123:00") == 0);
        fmt_time(t.s, 4294967295u);
        xpect(&c, "time: UINT32_MAX fits FMT_TIME_MAX",
              strcmp(t.s, "71582788:15") == 0 && t.pre == 0x7E && t.post == 0x7E);

        char n[24];
        fmt_count(n, 3, 12);  xpect(&c, "count: 3 / 12", strcmp(n, "3 / 12") == 0);
        fmt_count(n, 0, 0);   xpect(&c, "count: 0 / 0", strcmp(n, "0 / 0") == 0);
    }

    return xfail_done(&c);
}
