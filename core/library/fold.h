/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/fold.h — folding a name down to what a typed query can match.
 *
 * WHY THIS IS NOT name_hash()
 *
 * names.c already folds: name_hash() lower-cases A-Z and pulls smart quotes
 * and en/em dashes back to ASCII before hashing, so a record binds to its
 * file even when the two disagree about how an apostrophe is spelt. Search
 * needs the same three rules — someone typing "its over" must find "It's
 * Over" however the tagger wrote it — and one more that name_hash MUST NOT
 * have: a diacritic fold, so "elan" finds "Élan".
 *
 * name_hash cannot grow that rule. It is the LOCATOR: the on-disk bytes of a
 * CORELIB.IDX record are bound to a file by the equality of that hash, it is
 * computed independently by tools/build_index.py on the host, and its output
 * is pinned byte for byte by tests/kernel/name_hash_vectors.h on both sides.
 * Changing what it folds would silently unbind every accented name in every
 * library built by an older host tool. Worse, it is deliberately LOSSY in one
 * direction only: two names in one folder that differ just by case or quote
 * style share a bucket on purpose, and name_bind_exact() is what tells them
 * apart. Folding accents in would put more names in each bucket for no gain
 * to the binding.
 *
 * So the diacritic table lives here, on the query path, where being generous
 * costs nothing but an extra match. The case/quote/dash rules are name_hash's,
 * deliberately, and the suite asserts the two agree on them.
 *
 * THE RING HAS NO APOSTROPHE. ui/search.c's character ring is A-Z, 0-9 and a
 * space: there is no key for punctuation and nowhere on 39 cells to put one.
 * So a fold that kept the apostrophe would leave "It's Over" reachable only
 * by "it" or by "over" — never by "its over", which is what someone types.
 * The quote characters therefore fold to NOTHING, which is strictly more
 * generous than name_hash (it folds them to one spelling) in the one
 * direction that is safe: every pair of names name_hash calls the same, this
 * still calls the same. The dash stays, because '-' is not a quote and
 * dropping it would join words a query then could not separate.
 *
 * DEPENDENCIES: names.h for mn_utf8_next() only. No hardware, no globals.
 */

#ifndef CORE_LIBRARY_FOLD_H
#define CORE_LIBRARY_FOLD_H

/*
 * One folded byte for one codepoint:
 *   'A'-'Z'            -> 'a'-'z'        (name_hash's rule)
 *   ' " U+2018 U+2019 U+201C U+201D -> DROPPED (0; see the ring, above —
 *                         name_hash unifies the spellings, this removes them,
 *                         and either way the two spellings agree)
 *   U+2013 U+2014      -> '-'            (name_hash's rule)
 *   other ASCII        -> itself
 *   Latin-1 Supplement and Latin Extended-A letters -> their base ASCII
 *                         letter, lower case: E-acute/E-grave/e-diaeresis ->
 *                         'e', O-slash -> 'o', L-stroke -> 'l', eszett -> 's',
 *                         AE -> 'a', thorn -> 't'
 *   everything else non-ASCII -> 0x80, which no ASCII query can contain, so a
 *                         Cyrillic or CJK title never matches a typed letter
 *                         by accident and never collapses two distinct names
 *                         into an all-0x80 string that matches everything.
 */
int fold_cp(int cp);   /* 0 = this codepoint contributes nothing */

/*
 * Fold `s` into `out`, which holds at most `max` bytes INCLUDING the NUL.
 * Returns the number of folded bytes written (never `max` or more). max <= 0
 * writes nothing and returns 0. Codepoints that fold to 0 are dropped, so the
 * output can be shorter than the input has codepoints. A name longer than the
 * buffer is cut between codepoints. Malformed UTF-8 decodes to U+FFFD -> 0x80
 * with at least one byte of input consumed, so a corrupt name cannot stall
 * the scan.
 */
int fold_ascii(const char *s, unsigned char *out, int max);

#endif /* CORE_LIBRARY_FOLD_H */
