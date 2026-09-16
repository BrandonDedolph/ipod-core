/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/fold.c — folding a name down to what a typed query can match.
 * See fold.h for why this is separate from (and deliberately more generous
 * than) name_hash's folding.
 */

#include "fold.h"
#include "names.h"                 /* mn_utf8_next */

/*
 * U+00C0 .. U+017F, one base letter each — Latin-1 Supplement's letters and
 * the whole of Latin Extended-A. The two non-letters inside the range, U+00D7
 * MULTIPLICATION SIGN and U+00F7 DIVISION SIGN, fold to 0x80 like any other
 * unmatched codepoint; they are spliced in as their own literals so the hex
 * escape cannot swallow the letter after it.
 *
 * Ligatures fold to their first letter (AE -> a, OE -> o, IJ -> i): a query is
 * typed letter by letter and "aero" should still reach "Ærø".
 */
static const char fold_latin[] =
    /* C0 À Á Â Ã Ä Å Æ Ç È É Ê Ë Ì Í Î Ï */  "aaaaaaaceeeeiiii"
    /* D0 Ð Ñ Ò Ó Ô Õ Ö */                    "dnooooo"
    /* D7 multiplication sign */              "\x80"
    /* D8 Ø Ù Ú Û Ü Ý Þ ß */                  "ouuuuyts"
    /* E0 à á â ã ä å æ ç è é ê ë ì í î ï */  "aaaaaaaceeeeiiii"
    /* F0 ð ñ ò ó ô õ ö */                    "dnooooo"
    /* F7 division sign */                    "\x80"
    /* F8 ø ù ú û ü ý þ ÿ */                  "ouuuuyty"
    /* 0100 Ā ā Ă ă Ą ą Ć ć Ĉ ĉ Ċ ċ Č č Ď ď */ "aaaaaaccccccccdd"
    /* 0110 Đ đ Ē ē Ĕ ĕ Ė ė Ę ę Ě ě Ĝ ĝ Ğ ğ */ "ddeeeeeeeeeegggg"
    /* 0120 Ġ ġ Ģ ģ Ĥ ĥ Ħ ħ Ĩ ĩ Ī ī Ĭ ĭ Į į */ "gggghhhhiiiiiiii"
    /* 0130 İ ı Ĳ ĳ Ĵ ĵ Ķ ķ ĸ Ĺ ĺ Ļ ļ Ľ ľ Ŀ */ "iiiijjkkklllllll"
    /* 0140 ŀ Ł ł Ń ń Ņ ņ Ň ň ŉ Ŋ ŋ Ō ō Ŏ ŏ */ "lllnnnnnnnnnoooo"
    /* 0150 Ő ő Œ œ Ŕ ŕ Ŗ ŗ Ř ř Ś ś Ŝ ŝ Ş ş */ "oooorrrrrrssssss"
    /* 0160 Š š Ţ ţ Ť ť Ŧ ŧ Ũ ũ Ū ū Ŭ ŭ Ů ů */ "ssttttttuuuuuuuu"
    /* 0170 Ű ű Ų ų Ŵ ŵ Ŷ ŷ Ÿ Ź ź Ż ż Ž ž ſ */ "uuuuwwyyyzzzzzzs";

/* 0x180 - 0xC0 = 192 entries, plus the NUL the literal carries. A miscount in
 * the table above shifts every letter after it, which is exactly the kind of
 * mistake that shows up as one artist being unsearchable. */
_Static_assert(sizeof fold_latin == 192 + 1, "fold_latin covers U+00C0..U+017F");

int fold_cp(int cp)
{
    /* The quotes first: name_hash folds the smart pair onto the ASCII one, and
     * this drops all four, so the two still agree about which names are the
     * same name. The ring cannot type an apostrophe — see fold.h. */
    if (cp == '\'' || cp == '"' ||
        cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D) {
        return 0;
    }
    if (cp == 0x2013 || cp == 0x2014) cp = '-';    /* name_hash's dash rule */
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp < 0x80)              return cp;
    if (cp >= 0xC0 && cp <= 0x17F) {
        return (unsigned char)fold_latin[cp - 0xC0];
    }
    return 0x80;
}

int fold_ascii(const char *s, unsigned char *out, int max)
{
    if (max <= 0) {
        return 0;
    }
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (n < max - 1) {
        int cp = mn_utf8_next(&p);
        if (cp < 0) break;                     /* NUL: the whole name fitted */
        int f = fold_cp(cp);
        if (f == 0) continue;                  /* contributes nothing */
        out[n++] = (unsigned char)f;
    }
    out[n] = '\0';
    return n;
}
