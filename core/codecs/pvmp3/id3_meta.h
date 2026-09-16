/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/pvmp3/id3_meta.h — ID3 tag + duration reader for MP3.
 *
 * The MP3 counterpart of codecs/flac_meta.c, filling the SAME flac_meta_t so
 * every consumer above it (Now Playing, the library scan, the queue row) stays
 * codec-agnostic. The struct keeps its name — it is "track metadata", and it
 * is read from a Vorbis comment or from an ID3 tag depending on the file.
 *
 * What it reads, streaming, through a decoder_source_t positioned at the start
 * of the file:
 *
 *   - ID3v2.2 / 2.3 / 2.4 text frames: TIT2 title, TPE1 artist (TPE2 album
 *     artist as the fallback), TALB album, TCON genre, TRCK track, TDRC/TYER
 *     year — and their three-letter v2.2 spellings (TT2/TP1/TP2/TAL/TCO/TRK/TYE);
 *   - all four text encodings, including a REAL UTF-16 to UTF-8 conversion.
 *     The previous reader downconverted UTF-16 by dropping the high byte,
 *     which put '?' through the marquee of every Windows-tagged file; the
 *     atlas and copy_display_name are UTF-8 now, so there is no reason for it;
 *   - TCON's numeric forms ("17", "(17)", "(17)Rock", "(RX)") against the
 *     ID3v1 genre table;
 *   - the duration and sample rate, from the Xing/Info/VBRI header or the CBR
 *     estimate (codecs/pvmp3/mp3_frame.c).
 *
 * What it does not: ReplayGain (TXXX:REPLAYGAIN_*) is out of scope, so
 * have_rg stays 0 and the player leaves an MP3's gain alone. APIC frames are
 * skipped by size — album art on device comes from folder.art, not the file.
 *
 * Freestanding-clean: no libc/malloc, everything on the caller's flac_meta_t
 * or in small bounded locals, and the tag walk is capped.
 */
#ifndef CORE_CODECS_PVMP3_ID3_META_H
#define CORE_CODECS_PVMP3_ID3_META_H

#include "../decoder.h"
#include "../flac_meta.h"

/*
 * Parse an MP3's tags and duration from a source positioned at the START of
 * the file. *out is fully zeroed first, then populated.
 *
 * Returns 0 on success (out->have == 1, the file holds a decodable MPEG Layer
 * III stream), -1 when it does not — in which case *out is left zeroed, which
 * is the same "no metadata, fall back to the filename" flac_meta_read()
 * reports for a non-FLAC.
 *
 * A file with no ID3 tag at all still succeeds: the text fields stay empty and
 * duration_s/sample_rate come from the stream itself.
 */
int id3_meta_read(decoder_source_t *src, flac_meta_t *out);

#endif /* CORE_CODECS_PVMP3_ID3_META_H */
