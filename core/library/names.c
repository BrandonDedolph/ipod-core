/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/names.c — implementation of the library's name handling.
 *
 * Moved VERBATIM out of kernel/main.c (only the `static` came off, so the
 * host tests can link the same code the device runs). See names.h for why.
 */

#include "names.h"

/* Case-insensitive ASCII match of a dirent name against a literal. */
int name_eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* Junk-filter for the album list: skip iPod/OS system folders and any dotfolder
 * (.Trashes, .Spotlight-V100, .fseventsd, …) so only music folders show. */
int is_junk_dir(const char *name)
{
    if (name[0] == '.') {
        return 1;
    }
    static const char *const junk[] = {
        "iPod_Control", "Calendars", "Contacts", "Photos", "Recordings",
        "Notes", "System Volume Information", "$RECYCLE.BIN", "LOST.DIR",
        "Find My iPod",
    };
    for (unsigned i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        if (name_eq_ci(name, junk[i])) {
            return 1;
        }
    }
    return 0;
}

/* Copy `src` into `dst` (<= NAME_MAX bytes), keeping printable ASCII AND UTF-8
 * multibyte bytes (the atlas now covers Latin-1 + smart punctuation, and the FAT
 * reader hands us real UTF-8) — only C0 control bytes (0x00..0x1F, incl. the
 * legacy 0x01 placeholder) are dropped. If `drop_ext`, trim a trailing ".ext".
 * Truncation is byte-bounded; a split multibyte tail just renders as one U+FFFD. */
void copy_display_name(char *dst, const char *src, int drop_ext)
{
    int end = 0;
    while (src[end]) end++;
    if (drop_ext) {
        int dot = -1;
        for (int j = 0; src[j]; j++) {
            if (src[j] == '.') dot = j;
        }
        if (dot > 0) end = dot;              /* trim the extension */
    }
    int i = 0;
    for (int j = 0; j < end && i < NAME_MAX; j++) {
        unsigned char c = (unsigned char)src[j];
        if (c >= 0x20) {                     /* keep ASCII + all UTF-8 bytes */
            dst[i++] = (char)c;
        }
    }
    dst[i] = '\0';
}

/* Decode one UTF-8 sequence at *p, advance past it, return the codepoint (-1 at
 * NUL). Malformed bytes yield one byte of progress so a bad name can't stall. */
int mn_utf8_next(const unsigned char **p)
{
    unsigned char c = **p;
    if (c == 0) return -1;
    if (c < 0x80) { (*p)++; return c; }
    int n, cp;
    if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { (*p)++; return 0xFFFD; }
    const unsigned char *q = *p + 1;
    for (int i = 0; i < n; i++) {
        if ((q[i] & 0xC0) != 0x80) { (*p)++; return 0xFFFD; }
        cp = (cp << 6) | (q[i] & 0x3F);
    }
    *p += n + 1;
    /* Reject NON-MINIMAL (overlong) encodings and the UTF-16 surrogate range:
     * "C0 80" would otherwise decode to U+0000 (a NUL smuggled into a name) and
     * "E0 80 AF" to '/' (a path separator that never appears as a real byte).
     * The whole sequence is still consumed, so progress is unchanged. */
    static const int min_cp[3] = { 0x80, 0x800, 0x10000 };
    if (cp < min_cp[n - 1] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        return 0xFFFD;
    }
    return cp;
}

/* Case/quote-folded FNV-1a-32 over a UTF-8 name — the on-disk locator that binds
 * an index record to its file/folder without depending on byte-exact names
 * (quote-style drift can't break a match). MUST stay byte-identical to
 * tools/build_index.py name_hash(): fold smart quotes/dashes to ASCII, lowercase
 * A-Z, then FNV-1a over the re-encoded UTF-8 bytes. */
uint32_t name_hash(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t h = 0x811c9dc5u;
    for (;;) {
        int cp = mn_utf8_next(&p);
        if (cp < 0) break;
        if      (cp == 0x2018 || cp == 0x2019) cp = '\'';
        else if (cp == 0x201C || cp == 0x201D) cp = '"';
        else if (cp == 0x2013 || cp == 0x2014) cp = '-';
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        /* Re-encode. The 4-byte branch is load-bearing: without it an astral
         * codepoint (any emoji) was folded into a 3-byte sequence while
         * build_index.py emitted real 4-byte UTF-8, so the two hashes could
         * never agree and the track silently never resolved to its file. */
        unsigned char b[4]; int n;
        if      (cp < 0x80)   { b[0] = (unsigned char)cp; n = 1; }
        else if (cp < 0x800)  { b[0] = (unsigned char)(0xC0 | (cp >> 6));
                                b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
        else if (cp < 0x10000){ b[0] = (unsigned char)(0xE0 | (cp >> 12));
                                b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                                b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
        else                  { b[0] = (unsigned char)(0xF0 | (cp >> 18));
                                b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                                b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                                b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }
        for (int i = 0; i < n; i++) { h ^= b[i]; h *= 0x01000193u; }
    }
    return h;
}

/* Split a "Artist - Album" folder name on its first " - " separator (the loader
 * names album folders this way). No separator -> artist empty, album = whole
 * name. Both outputs NAME_MAX-bounded (copy_display_name, UTF-8-preserving). */
void split_artist_album(const char *name, char *artist, char *album)
{
    int sep = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == ' ' && name[i + 1] == '-' && name[i + 2] == ' ') {
            sep = i;
            break;
        }
    }
    if (sep < 0) {
        artist[0] = '\0';
        copy_display_name(album, name, 0);
        return;
    }
    char tmp[NAME_MAX + 1];
    int n = (sep < NAME_MAX) ? sep : NAME_MAX;
    for (int i = 0; i < n; i++) tmp[i] = name[i];
    tmp[n] = '\0';
    copy_display_name(artist, tmp, 0);
    copy_display_name(album, name + sep + 3, 0);
}

/* Strip a leading track-number prefix ("NN. " / "NN.") from a track filename so
 * the tracklist shows a clean title (the row's own number gutter provides the
 * index). Only a digits-then-'.' prefix is removed, so titles that merely start
 * with a number ("99 Luftballons") are left alone. */
const char *track_display(const char *name)
{
    const char *p = name;
    while (*p >= '0' && *p <= '9') p++;
    if (p != name && *p == '.') {
        p++;
        while (*p == ' ') p++;
        if (*p) return p;
    }
    return name;
}

/* Classify by extension: 0 = FLAC (.fla/.flac), 1 = MP3 (.mp3), -1 = skip. */
int classify_ext(const char *name)
{
    int dot = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') dot = i;
    }
    if (dot < 0) return -1;

    char ext[5];
    int n = 0;
    for (const char *e = name + dot + 1; *e && n < 4; e++) {
        char c = *e;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        ext[n++] = c;
    }
    ext[n] = '\0';

    if (n == 3 && ext[0] == 'F' && ext[1] == 'L' && ext[2] == 'A') return 0;
    if (n == 4 && ext[0] == 'F' && ext[1] == 'L' && ext[2] == 'A' && ext[3] == 'C') return 0;
#if CORE_ENABLE_MP3
    if (n == 3 && ext[0] == 'M' && ext[1] == 'P' && ext[2] == '3') return 1;
#endif
    return -1;
}

/* Drop a recognised audio extension from an index file[] field, IN PLACE —
 * and only a recognised one. The field is the first 63 bytes of the name; for
 * a name longer than that the ".flac" is not in it, and cutting at whatever
 * '.' remains produced the "16" of the bug described at lib_song_t. This is
 * the display placeholder until the resolve pass installs the on-disk stem. */
void trim_audio_ext(char *name)
{
    if (classify_ext(name) < 0) return;
    int dot = -1;
    for (int j = 0; name[j]; j++) if (name[j] == '.') dot = j;
    if (dot > 0) name[dot] = '\0';
}

/* Case-insensitive title compare (for the sort). */
int title_cmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return (int)ca - (int)cb;
    }
    return (int)*a - (int)*b;
}

/* De-duplication / sort key for an artist name: ignore a leading "The " so
 * "The Kid LAROI" and "Kid LAROI" collapse to one entry (and sort together
 * under K). Combined with title_cmp's case folding this also merges pure
 * case variants ("blackbear" vs "Blackbear"). Returns a pointer INTO `s`. */
const char *artist_key(const char *s)
{
    if ((s[0] == 'T' || s[0] == 't') && (s[1] == 'h' || s[1] == 'H') &&
        (s[2] == 'e' || s[2] == 'E') && s[3] == ' ') {
        return s + 4;
    }
    return s;
}

/* Uppercased first letter of `s`, '#' for anything not A-Z. */
char initial_of(const char *s)
{
    while (*s == ' ') s++;
    char c = *s;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return (c >= 'A' && c <= 'Z') ? c : '#';
}

/* Write unsigned `v` as decimal into `dst`, return the length. The one decimal
 * writer — replaces the do/while digit-reversal that was open-coded ~8 times. */
int u32_to_dec(char *dst, unsigned v)
{
    char nb[10];
    int t = 0;
    do { nb[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < t; i++) dst[i] = nb[t - 1 - i];
    dst[t] = '\0';
    return t;
}

/* Format `s` seconds as "M:SS" — minutes genuinely uncapped (the old two-digit
 * write rendered 123 min as "23:SS"). `buf` must be >= FMT_TIME_MAX bytes. */
void fmt_time(char *buf, uint32_t s)
{
    uint32_t m = s / 60, ss = s % 60;
    int i = 0;
    i += u32_to_dec(buf, m);          /* 1..10 digits, no truncation */
    buf[i++] = ':';
    buf[i++] = (char)('0' + ss / 10);
    buf[i++] = (char)('0' + ss % 10);
    buf[i]   = '\0';
}

/* Format "a / b" into dst (needs >= 12 bytes). */
void fmt_count(char *dst, int a, int b)
{
    int i = u32_to_dec(dst, (unsigned)a);
    dst[i++] = ' '; dst[i++] = '/'; dst[i++] = ' ';
    u32_to_dec(dst + i, (unsigned)b);
}
