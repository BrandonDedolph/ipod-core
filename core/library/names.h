/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/library/names.h — the name handling the music library is built on.
 *
 * WHY THIS FILE EXISTS
 *
 * Everything here was a `static` inside kernel/main.c, which meson builds
 * only for target == 'hw' (main.c reaches into the hw headers and
 * USEC_TIMER). None of it needs hardware: it is string folding, hashing,
 * splitting and formatting over NUL-terminated UTF-8. But because it sat in
 * main.c, the only way to check it was to flash the device — so the project
 * grew VERBATIM COPIES of the pieces it most needed to trust (name_hash in
 * tests/kernel/name_hash_ref.c) with a Python script diffing the copy against
 * the original. One definition, compiled into both the device image and the
 * host tests, is what that arrangement was standing in for.
 *
 * WHAT IS LOAD-BEARING
 *
 * name_hash() is the LOCATOR: a CORELIB.IDX record is bound to its file or
 * folder on disk by nothing but the equality of this hash, computed on the
 * host by tools/build_index.py and on the device by this function. A
 * disagreement for some name is not an error — the affected track silently
 * never resolves. Its behaviour is pinned by tests/kernel/name_hash_vectors.h
 * on both sides and must not change, including for inputs that look like
 * edge cases.
 *
 * copy_display_name() is the other contract: it is what a browse row, a queue
 * entry and lib_song_t.file all hold, and those are compared and hashed
 * against each other, so they must be produced by the one function.
 *
 * DEPENDENCIES: player.h for NAME_MAX only. No hw/, no MMIO, no globals.
 */

#ifndef CORE_LIBRARY_NAMES_H
#define CORE_LIBRARY_NAMES_H

#include <stdint.h>

#include "../player/player.h"        /* NAME_MAX: the stored display-name cap */

/* Case-insensitive ASCII match of a dirent name against a literal. */
int name_eq_ci(const char *a, const char *b);

/* Junk-filter for the album list: iPod/OS system folders and any dotfolder. */
int is_junk_dir(const char *name);

/* Copy `src` into `dst` (<= NAME_MAX bytes + NUL), dropping only C0 control
 * bytes; UTF-8 multibyte sequences pass through. `drop_ext` trims a trailing
 * ".ext". Truncation is byte-bounded. */
void copy_display_name(char *dst, const char *src, int drop_ext);

/* The record<->disk TIEBREAK. `stored` is the name a record carries (the
 * index's copy of a filename with its audio extension trimmed, or a folder
 * name); `disk` is a directory entry it might bind to. 1 when they are the
 * same bytes once `disk` has been through copy_display_name(drop_ext) — no
 * case or quote folding, which is the point: name_hash folds, so two names in
 * one folder that differ only by "It's" / "It\xe2\x80\x99s" (or case) share
 * a hash bucket, and this is what tells them apart. Only consulted when a
 * bucket holds more than one candidate; the single-candidate bind never
 * compares a byte. A stored name that outgrew its 63-byte field can never
 * match (the disk name was not cut), so such a pair keeps directory order. */
int name_bind_exact(const char *stored, const char *disk, int drop_ext);

/* Decode one UTF-8 sequence at *p, advance past it, return the codepoint (-1
 * at NUL, U+FFFD for a malformed, overlong or surrogate sequence). Malformed
 * bytes yield one byte of progress so a bad name can't stall. */
int mn_utf8_next(const unsigned char **p);

/* Case/quote-folded FNV-1a-32 over a UTF-8 name — the on-disk locator. MUST
 * stay byte-identical to tools/build_index.py name_hash(). */
uint32_t name_hash(const char *s);

/* Split an "Artist - Album" folder name on its first " - ". No separator:
 * artist empty, album = whole name. Both outputs NAME_MAX + 1 bytes. */
void split_artist_album(const char *name, char *artist, char *album);

/* A track filename without its leading "NN. " track-number prefix; returns a
 * pointer INTO `name`. */
const char *track_display(const char *name);

/* Classify by extension: 0 = FLAC (.fla/.flac), 1 = MP3 (.mp3), -1 = skip.
 * MP3 used to be behind a CORE_ENABLE_MP3 switch because dr_mp3's float
 * synthesis could not hit real time on this FPU-less CPU. The decoder is
 * fixed-point now (codecs/pvmp3), so the switch is gone: a shipped format
 * does not get a build flag. */
int classify_ext(const char *name);

/* Drop a RECOGNISED audio extension in place; anything else is left alone. */
void trim_audio_ext(char *name);

/* Case-insensitive (ASCII) compare, strcmp-signed — the library sort order. */
int title_cmp(const char *a, const char *b);

/* De-duplication / sort key for an artist name: past a leading "The ".
 * Returns a pointer INTO `s`. */
const char *artist_key(const char *s);

/* Uppercased first letter of `s`, '#' for anything not A-Z — the A-Z cue. */
char initial_of(const char *s);

/* Write unsigned `v` as decimal into `dst` (NUL-terminated), return the length.
 * `dst` needs 11 bytes. */
int u32_to_dec(char *dst, unsigned v);

/* "M:SS" (minutes uncapped): up to 10 minute digits + ':' + 2 + NUL. Every
 * fmt_time buffer is sized FMT_TIME_MAX so a long track can't overrun it. */
#define FMT_TIME_MAX 16
void fmt_time(char *buf, uint32_t s);

/* Format "a / b" into dst (needs >= 12 bytes for the values the UI passes). */
void fmt_count(char *dst, int a, int b);

#endif /* CORE_LIBRARY_NAMES_H */
