/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/pvmp3/mp3_frame.h — MPEG audio framing: headers, tags, Xing.
 *
 * pvmp3 decodes ONE frame at a time and knows nothing about the file around
 * it. Everything between the frames is this file's job:
 *
 *   - the 4-byte MPEG header (version / layer / bitrate / rate / frame length),
 *     and the two-header validation that tells a real frame from a byte that
 *     merely looks like a sync word;
 *   - the tags that bracket the audio: an ID3v2 prefix (possibly chained,
 *     possibly with a footer) at the head, ID3v1 and/or APE at the tail;
 *   - the Xing/Info and VBRI headers that carry the frame count, the byte
 *     count and the 100-point seek TOC, plus the LAME/Lavc encoder delay and
 *     end padding that ffmpeg (and therefore our host index) subtracts from
 *     the duration;
 *   - the target byte offset for a seek to a PCM frame.
 *
 * Everything but mp3_stream_scan() is a pure function over a byte buffer with
 * an explicit length, which is what makes the awkward cases testable on the
 * host (see tests/codecs/mp3_frame_test.c). Freestanding-clean: no libc, no
 * allocation, no float.
 */
#ifndef CORE_CODECS_PVMP3_MP3_FRAME_H
#define CORE_CODECS_PVMP3_MP3_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "../decoder.h"

/* MPEG version, in the order the header's two bits count UP in sample rate. */
enum {
    MP3_MPEG_25 = 0,   /* MPEG 2.5 (unofficial): 8000 / 11025 / 12000 Hz */
    MP3_MPEG_2  = 1,   /* MPEG 2 (LSF):         16000 / 22050 / 24000 Hz */
    MP3_MPEG_1  = 2,   /* MPEG 1:               32000 / 44100 / 48000 Hz */
};

/* The largest Layer III frame is 1441 bytes (MPEG-1, 320 kbps, 32 kHz, padded).
 * The wrapper's staging buffer is this size so one frame always fits whole. */
#define MP3_MAX_FRAME_BYTES 1441

/* One parsed frame header. Only Layer III is accepted (see mp3_header_parse). */
typedef struct {
    uint32_t sample_rate;   /* Hz                                              */
    uint32_t bitrate;       /* bits/s                                          */
    uint32_t frame_bytes;   /* whole frame incl. the 4-byte header and padding */
    uint16_t spf;           /* PCM frames the frame decodes to: 1152 or 576    */
    uint8_t  version;       /* MP3_MPEG_*                                      */
    uint8_t  channels;      /* 1 = mono (MPG_MD_MONO), else 2                  */
} mp3_header_t;

/*
 * Parse a 4-byte MPEG header. Returns 1 when `h` is a Layer III frame header
 * we can decode, 0 otherwise — which deliberately includes Layer I/II, the
 * reserved version and sample-rate codes, free format (bitrate index 0) and
 * the reserved bitrate index 15. A caller resyncing through junk treats 0 as
 * "not a frame, step one byte".
 */
int mp3_header_parse(const uint8_t h[4], mp3_header_t *out);

/*
 * Bytes of side information between the header (plus its optional CRC) and
 * the main data: 32/17 for MPEG-1 stereo/mono, 17/9 for MPEG-2 and 2.5. This
 * is also where a Xing/Info tag sits, measured from the end of the header and
 * ignoring the CRC — which is how LAME writes it and how ffmpeg reads it, so
 * it is also what keeps our duration equal to ffprobe's.
 */
int mp3_side_info_bytes(const mp3_header_t *h);

/*
 * Total length of the ID3v2 tag whose 10-byte header is at `hdr` — body and
 * v2.4 footer included — or 0 when that is not an ID3v2 header. Tags chain (a
 * tagger that appends rather than rewrites leaves two), so a caller skipping
 * the prefix calls this until it answers 0.
 */
uint32_t mp3_id3v2_tag_len(const uint8_t hdr[10]);

/*
 * Length of the ID3v1 and/or APE tag at the END of a file whose last
 * `len` bytes are in `tail` (pass at least 160 bytes, or the whole file when
 * it is shorter). 0 when the file ends with audio.
 */
uint32_t mp3_tail_tag_len(const uint8_t *tail, size_t len);

/* Xing / Info / VBRI, as far as we use it. */
typedef struct {
    uint8_t  have;        /* a tag was found in the first frame            */
    uint8_t  vbr;         /* "Xing" or VBRI (variable); 0 = "Info" (CBR)   */
    uint8_t  have_toc;    /* toc[] is populated                            */
    uint16_t delay;       /* LAME/Lavc encoder delay, PCM frames           */
    uint16_t padding;     /* LAME/Lavc end padding, PCM frames             */
    uint32_t frames;      /* MPEG frames AFTER this one, 0 = absent        */
    uint32_t bytes;       /* bytes of the audio region, 0 = absent         */
    uint8_t  toc[100];    /* seek table, percent-of-time -> 1/256 of bytes */
} mp3_vbr_tag_t;

/*
 * Parse the Xing/Info or VBRI header carried by the FIRST frame. `frame` is
 * that frame's bytes (starting at its sync word) and `len` how many of them
 * are readable. Returns 1 when a tag was found. A frame that carries one is
 * pure silence and must not be decoded.
 */
int mp3_vbr_tag_parse(const uint8_t *frame, size_t len,
                      const mp3_header_t *h, mp3_vbr_tag_t *out);

/* What mp3_stream_scan() learned about a file. */
typedef struct {
    mp3_header_t  first;        /* the header of the first frame in the file, */
                                /*   which is the Xing/Info frame when there   */
                                /*   is one. Its version, rate and channels    */
                                /*   are the stream's; its frame_bytes is that */
                                /*   ONE frame's, and on a VBR file that says  */
                                /*   nothing about the rest, so do not size    */
                                /*   a read against it                         */
    mp3_vbr_tag_t vbr;
    uint64_t audio_start;       /* first byte we DECODE from: past the ID3v2   */
                                /*   prefix AND past a Xing/Info frame          */
    uint64_t seek_start;        /* origin the Xing TOC and the byte ratio map   */
                                /*   from: the MPEG stream's first byte, which  */
                                /*   IS the Xing frame when there is one        */
    uint64_t audio_end;         /* one past the last audio byte (tags cut off)  */
    uint64_t audio_bytes;       /* bytes from seek_start to audio_end (or the   */
                                /*   count Xing states, which is the same span) */
    uint64_t total_frames;      /* PCM frames the track IS, 0 = unknown. The   */
                                /*   encoder delay and end padding are taken    */
                                /*   off, which is ffprobe's rule and so the    */
                                /*   host index's                               */
    uint64_t raw_frames;        /* PCM frames the BYTES hold — total_frames plus */
                                /*   the delay and padding. The seek map works   */
                                /*   in these, because that is what is on disk   */
} mp3_stream_info_t;

/*
 * Walk a file's tags and first frame through `src` (positioned anywhere; the
 * scan seeks). Fills *out and leaves the source positioned at audio_start.
 *
 * Returns 0 when a first frame was validated, -1 when the file holds no
 * MPEG Layer III frame we recognise (which is how "this is not an MP3" is
 * reported). *out is zeroed first either way.
 */
int mp3_stream_scan(decoder_source_t *src, mp3_stream_info_t *out);

/*
 * Byte offset to start decoding at for PCM frame `target_frame`: the Xing TOC
 * interpolated when the file has one, proportional by byte otherwise (exact
 * for CBR). Always inside [audio_start, audio_end); never the seek itself —
 * the caller still has to resync to a real header from there.
 */
uint64_t mp3_seek_offset(const mp3_stream_info_t *si, uint64_t target_frame);

#endif /* CORE_CODECS_PVMP3_MP3_FRAME_H */
