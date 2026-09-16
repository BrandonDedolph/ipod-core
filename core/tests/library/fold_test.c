/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/library/fold_test.c — the search fold (core/library/fold.c) on the
 * host, and the one claim it makes about name_hash.
 *
 * THE POINT OF THIS FILE. fold.c exists because name_hash() must NOT grow a
 * diacritic rule: its output is the on-disk locator, computed independently by
 * tools/build_index.py and pinned byte for byte on both sides, so a change
 * there unbinds every accented name in every library an older host built. The
 * price of that separation is a second folder, and a second folder is only
 * safe while it AGREES with the first on the rules they share. §4 below is
 * that agreement, asserted through name_hash itself rather than by reading
 * both functions and hoping.
 *
 * The rest is the table. A 192-entry lookup is exactly the kind of thing that
 * is off by one somewhere in the middle and shows up months later as one
 * artist being unsearchable, so the spot checks straddle every seam in it.
 */

#include <stdio.h>
#include <string.h>

#include "fold.h"
#include "names.h"

#include "../xfail.h"

/* UTF-8 encode one codepoint, for building test names by number. */
static int enc(char *out, int cp)
{
    unsigned char *p = (unsigned char *)out;
    if (cp < 0x80)    { p[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800)   { p[0] = (unsigned char)(0xC0 | (cp >> 6));
                        p[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { p[0] = (unsigned char)(0xE0 | (cp >> 12));
                        p[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                        p[2] = (unsigned char)(0x80 | (cp & 0x3F)); return 3; }
    p[0] = (unsigned char)(0xF0 | (cp >> 18));
    p[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    p[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    p[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* fold_ascii over a string, into a fresh buffer; returns the length. */
static int fold_str(const char *s, char *out, int max)
{
    return fold_ascii(s, (unsigned char *)out, max);
}

static int fold_eq(const char *s, const char *want)
{
    char got[128];
    int n = fold_str(s, got, (int)sizeof got);
    if (n != (int)strlen(want) || memcmp(got, want, (size_t)n) != 0) {
        fprintf(stderr, "[fold] \"%s\" folded to \"%s\", want \"%s\"\n",
                s, got, want);
        return 0;
    }
    return 1;
}

int main(void)
{
    xfail_ctx c = { "fold", 0, 0, 0 };

    /* ---- 1. the rules fold.c shares with name_hash ---------------------- */
    xpect(&c, "A-Z lower-cases, other ASCII passes through",
          fold_cp('A') == 'a' && fold_cp('Z') == 'z' && fold_cp('a') == 'a' &&
          fold_cp('7') == '7' && fold_cp(' ') == ' ' && fold_cp('!') == '!' &&
          fold_cp('[') == '[' && fold_cp('~') == '~');
    /* The ring is A-Z, 0-9 and a space: nobody can type an apostrophe, so a
     * fold that kept one would put "It's Over" out of reach of "its over". */
    xpect(&c, "every quote spelling, straight or smart, folds to nothing",
          fold_cp('\'') == 0 && fold_cp('"') == 0 &&
          fold_cp(0x2018) == 0 && fold_cp(0x2019) == 0 &&
          fold_cp(0x201C) == 0 && fold_cp(0x201D) == 0);
    xpect(&c, "en and em dashes fold to '-'",
          fold_cp(0x2013) == '-' && fold_cp(0x2014) == '-');

    /* ---- 2. the diacritic table, at every seam --------------------------- */
    xpect(&c, "Latin-1: the vowels",
          fold_cp(0x00C9) == 'e' && fold_cp(0x00E9) == 'e' &&   /* E-acute   */
          fold_cp(0x00C8) == 'e' && fold_cp(0x00EB) == 'e' &&   /* E-grave.. */
          fold_cp(0x00C0) == 'a' && fold_cp(0x00FC) == 'u' &&
          fold_cp(0x00D6) == 'o' && fold_cp(0x00EF) == 'i');
    xpect(&c, "Latin-1: the letters that are not just a vowel with a hat",
          fold_cp(0x00D8) == 'o' &&      /* O-slash  */
          fold_cp(0x00DF) == 's' &&      /* eszett   */
          fold_cp(0x00C6) == 'a' &&      /* AE       */
          fold_cp(0x00DE) == 't' &&      /* thorn    */
          fold_cp(0x00D0) == 'd' &&      /* eth      */
          fold_cp(0x00C7) == 'c' &&      /* C-cedilla*/
          fold_cp(0x00D1) == 'n' &&      /* N-tilde  */
          fold_cp(0x00FF) == 'y');       /* y-diaeresis */
    /* The two NON-letters inside the Latin-1 letter block. Getting these
     * wrong shifts every entry after them by one. */
    xpect(&c, "multiplication and division signs are not letters",
          fold_cp(0x00D7) == 0x80 && fold_cp(0x00F7) == 0x80);
    xpect(&c, "Latin Extended-A: both ends and the awkward ones",
          fold_cp(0x0100) == 'a' &&      /* A-macron, first in the block */
          fold_cp(0x017F) == 's' &&      /* long s,   last in the block  */
          fold_cp(0x0141) == 'l' &&      /* L-stroke                     */
          fold_cp(0x0142) == 'l' &&
          fold_cp(0x0152) == 'o' &&      /* OE ligature                  */
          fold_cp(0x0132) == 'i' &&      /* IJ ligature                  */
          fold_cp(0x0160) == 's' &&      /* S-caron                      */
          fold_cp(0x017D) == 'z' &&      /* Z-caron                      */
          fold_cp(0x0131) == 'i');       /* dotless i                    */
    /* Every entry in the table must be a lower-case ASCII letter or the
     * "unmatchable" byte — nothing else, and never an upper-case letter that
     * a lower-case query could not reach. */
    {
        int bad = 0;
        for (int cp = 0xC0; cp <= 0x17F; cp++) {
            int f = fold_cp(cp);
            if (!((f >= 'a' && f <= 'z') || f == 0x80)) {
                fprintf(stderr, "[fold] U+%04X folds to %d\n", cp, f);
                bad++;
            }
        }
        xpect(&c, "every table entry is a lower-case letter or 0x80", bad == 0);
    }
    xpect(&c, "everything else non-ASCII is the unmatchable byte",
          fold_cp(0x0180) == 0x80 &&     /* just past the table          */
          fold_cp(0x00BF) == 0x80 &&     /* just before it               */
          fold_cp(0x0416) == 0x80 &&     /* Cyrillic Zhe                 */
          fold_cp(0x4E2D) == 0x80 &&     /* CJK                          */
          fold_cp(0x1F600) == 0x80 &&    /* an emoji                     */
          fold_cp(0xFFFD) == 0x80);      /* what a malformed byte decodes to */

    /* ---- 3. fold_ascii ---------------------------------------------------- */
    xpect(&c, "a plain name folds to itself, lower case",
          fold_eq("Sunflower", "sunflower"));
    xpect(&c, "an accented name reaches its ASCII spelling",
          fold_eq("\xC3\x89lan", "elan"));                      /* Élan      */
    xpect(&c, "an apostrophe is not in the way of a typed query",
          fold_eq("It\xE2\x80\x99s Over", "its over") &&
          fold_eq("It's Over", "its over") &&
          fold_eq("Rock 'n' Roll", "rock n roll"));
    xpect(&c, "an em dash matches a hyphen",
          fold_eq("A \xE2\x80\x94 B", "a - b"));
    xpect(&c, "a non-Latin name folds to unmatchable bytes, not to nothing",
          fold_eq("\xD0\x96\xD0\x96", "\x80\x80"));

    /* Bounds. Sentinels either side: fold_ascii must write NOTHING outside
     * [0, max) of its buffer, whatever it is handed. */
    {
        struct { unsigned char lo[4]; unsigned char buf[16]; unsigned char hi[4]; } box;
        memset(&box, 0xA5, sizeof box);
        int n = fold_ascii("abcdefghijklmnopqrstuvwxyz", box.buf, 16);
        int clean = 1;
        for (int i = 0; i < 4; i++) {
            if (box.lo[i] != 0xA5 || box.hi[i] != 0xA5) clean = 0;
        }
        xpect(&c, "a long name is cut to max-1 bytes and NUL-terminated",
              n == 15 && box.buf[15] == '\0' &&
              memcmp(box.buf, "abcdefghijklmno", 15) == 0 && clean);

        memset(&box, 0xA5, sizeof box);
        n = fold_ascii("abc", box.buf, 1);
        xpect(&c, "max 1 is room for the NUL alone",
              n == 0 && box.buf[0] == '\0' && box.buf[1] == 0xA5);

        memset(&box, 0xA5, sizeof box);
        n = fold_ascii("abc", box.buf, 0);
        xpect(&c, "max 0 writes nothing at all",
              n == 0 && box.buf[0] == 0xA5);

        /* Truncation lands between codepoints, never inside one: the fold
         * emits one byte per codepoint, so a cut is always clean. */
        memset(&box, 0xA5, sizeof box);
        n = fold_ascii("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", box.buf, 3);  /* éééé */
        xpect(&c, "a multibyte name truncates on a codepoint boundary",
              n == 2 && box.buf[0] == 'e' && box.buf[1] == 'e' &&
              box.buf[2] == '\0' && box.buf[3] == 0xA5);
    }

    /* Malformed UTF-8 must make PROGRESS: a corrupt name that stalled the
     * decoder would hang the scan over the whole library. */
    {
        char got[64];
        int n = fold_str("a\xFF\xFE" "b", got, (int)sizeof got);
        xpect(&c, "malformed bytes fold to 0x80 and the name still ends",
              n == 4 && got[0] == 'a' && (unsigned char)got[1] == 0x80 &&
              (unsigned char)got[2] == 0x80 && got[3] == 'b');
        /* A truncated sequence at the very end: one byte of progress, a NUL,
         * and no read past it. */
        n = fold_str("ab\xE2\x80", got, (int)sizeof got);
        xpect(&c, "a truncated sequence at the end terminates",
              n >= 2 && got[0] == 'a' && got[1] == 'b' && got[n] == '\0');
    }

    /* ---- 4. the agreement with name_hash --------------------------------- */
    /*
     * The claim fold.h makes is "the case/quote/dash rules are name_hash's".
     * Asserted the only way that cannot drift: two spellings name_hash calls
     * the same name must fold the same too — and, the other way, a pair it
     * calls DIFFERENT (an accent) must be one this file deliberately unifies.
     */
    {
        static const struct { const char *a, *b; } same[] = {
            { "It's Over",  "IT'S OVER" },                        /* case   */
            { "It's Over",  "It\xE2\x80\x99s Over" },              /* quote  */
            { "\"Heroes\"", "\xE2\x80\x9CHeroes\xE2\x80\x9D" },    /* quotes */
            { "A-B",        "A\xE2\x80\x93" "B" },                 /* en dash*/
            { "A-B",        "A\xE2\x80\x94" "B" },                 /* em dash*/
        };
        int bad = 0;
        for (unsigned i = 0; i < sizeof same / sizeof same[0]; i++) {
            char fa[128], fb[128];
            int na = fold_str(same[i].a, fa, (int)sizeof fa);
            int nb = fold_str(same[i].b, fb, (int)sizeof fb);
            int hash_same = (name_hash(same[i].a) == name_hash(same[i].b));
            int fold_same = (na == nb && memcmp(fa, fb, (size_t)na) == 0);
            if (!hash_same || !fold_same) {
                fprintf(stderr, "[fold] \"%s\" vs \"%s\": hash %s, fold %s\n",
                        same[i].a, same[i].b, hash_same ? "same" : "DIFFER",
                        fold_same ? "same" : "DIFFER");
                bad++;
            }
        }
        xpect(&c, "every pair name_hash calls one name folds to one string",
              bad == 0);
    }
    {
        /* And the one rule that is deliberately NOT shared. If this ever
         * starts agreeing, name_hash has grown a diacritic fold and every
         * library built by an older host tool has silently come unbound. */
        char fa[128], fb[128];
        int na = fold_str("\xC3\x89lan", fa, (int)sizeof fa);   /* Élan */
        int nb = fold_str("Elan",         fb, (int)sizeof fb);
        xpect(&c, "the diacritic fold is this file's alone, not name_hash's",
              na == nb && memcmp(fa, fb, (size_t)na) == 0 &&
              name_hash("\xC3\x89lan") != name_hash("Elan"));
    }

    /* ---- 5. the quotes are the ONLY thing that disappears ----------------- */
    /* A fold that silently dropped anything else would make two different
     * names look identical to the scan — which is how a search starts
     * returning rows that have nothing to do with what was typed. Over the
     * whole BMP: every codepoint contributes exactly one byte, or is one of
     * the six quote characters. */
    {
        int bad = 0, dropped = 0;
        for (int cp = 1; cp < 0x10000; cp++) {
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;    /* not encodable  */
            char s[8];
            int n = enc(s, cp);
            s[n] = '\0';
            unsigned char out[8];
            int got = fold_ascii(s, out, (int)sizeof out);
            if (got == 0) {
                dropped++;
                if (!(cp == '\'' || cp == '"' || cp == 0x2018 || cp == 0x2019 ||
                      cp == 0x201C || cp == 0x201D)) {
                    fprintf(stderr, "[fold] U+%04X vanished\n", cp);
                    bad++;
                }
            } else if (got != 1) {
                fprintf(stderr, "[fold] U+%04X folded to %d bytes\n", cp, got);
                bad++;
            }
        }
        xpect(&c, "exactly the six quote codepoints vanish; every other one "
                  "folds to a single byte", bad == 0 && dropped == 6);
    }

    return xfail_done(&c);
}
