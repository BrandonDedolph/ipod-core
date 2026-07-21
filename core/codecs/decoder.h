/*
 * core/codecs/decoder.h — unified audio-decoder ABI.
 *
 * Every codec we ship (Helix MP3, Helix AAC, dr_flac, ALAC, Tremor,
 * libopus, hand-rolled WAV) plugs into this contract. The audio engine
 * doesn't know or care which one is decoding; it only sees decoder_t.
 *
 * Lifecycle:
 *
 *   const decoder_ops_t *flac_ops = flac_decoder_ops();
 *   decoder_t d;
 *   if (flac_ops->open(&d, src_bytes, src_len) < 0) error();
 *
 *   // d.sample_rate, d.channels, d.total_frames are now valid.
 *
 *   int16_t buf[FRAMES * 2];   // L/R interleaved
 *   int got;
 *   while ((got = flac_ops->decode(&d, buf, FRAMES)) > 0) {
 *       feed_pcm_to_dma(buf, got);
 *   }
 *   if (got < 0) error();
 *
 *   flac_ops->close(&d);
 *
 * Output format is the audio engine's lingua franca: 16-bit signed
 * interleaved PCM. Codecs that decode at higher bit depths (e.g. FLAC
 * 24-bit) downconvert to 16-bit inside the wrapper. The audio engine
 * later upsamples/dithers as needed for the DAC.
 *
 * No allocations on the hot path. The decoder_t itself holds enough
 * state for the wrapper; if the underlying lib needs heap, it goes
 * through the decoder_alloc_t struct passed to open(). On the host
 * (sim, KAT) callers may pass NULL, which means "use the wrapper's
 * default allocator" (typically malloc/free for sim). On hw the audio
 * engine will pass a non-NULL struct backed by our static sub-allocator.
 */

#ifndef CORE_CODECS_DECODER_H
#define CORE_CODECS_DECODER_H

#include <stddef.h>
#include <stdint.h>

/* Forward decls (recursive). */
struct decoder_ops;

/*
 * decoder_alloc_t — allocator hooks injected at open() time. Codec
 * wrappers translate these into the underlying lib's allocator API
 * (e.g. dr_flac's drflac_allocation_callbacks, Helix's MemAllocFn).
 *
 * Pass NULL to open() to use the wrapper's default (typically
 * malloc/free for sim builds; not available on hw, where the audio
 * engine always provides a non-NULL struct).
 *
 * userdata is opaque to the wrapper; useful for passing an arena
 * pointer that alloc/free close over.
 */
typedef struct decoder_alloc {
    void *(*alloc)(void *userdata, size_t bytes);
    void *(*realloc)(void *userdata, void *ptr, size_t bytes);
    void  (*free)(void *userdata, void *ptr);
    void *userdata;
} decoder_alloc_t;

/* Seek origins for decoder_source_t.seek (mirror C's SEEK_SET/CUR/END). END
 * matters: dr_flac seeks to the end at open() to learn the file size, so a
 * source that can't honour END makes it misjudge the length and refuse. */
enum {
    DECODER_SEEK_SET = 0,   /* offset from the start of the stream   */
    DECODER_SEEK_CUR = 1,   /* offset from the current position      */
    DECODER_SEEK_END = 2,   /* offset from the end of the stream     */
};

/*
 * decoder_source_t — a pull byte source for STREAMING opens, so a codec reads
 * its compressed input incrementally instead of needing the whole file in RAM
 * (essential on hw: a FLAC is tens of MB). Wrappers adapt these to the
 * underlying lib's stream callbacks (dr_flac's onRead/onSeek/onTell).
 *
 *   read: fill up to `bytes`; return bytes read (0 = end of stream).
 *   seek: reposition; origin is DECODER_SEEK_SET/CUR; return 1 ok / 0 fail.
 *         Backward seeks may be O(n) but happen only at open() — playback is
 *         forward-only.
 *   tell: current absolute byte position, or -1 if unknown.
 */
typedef struct decoder_source {
    size_t  (*read)(void *userdata, void *buf, size_t bytes);
    int     (*seek)(void *userdata, int offset, int origin);
    int64_t (*tell)(void *userdata);
    void     *userdata;
} decoder_source_t;

/*
 * decoder_t — open instance of a decoder. Lives on the caller's stack
 * or in a long-lived buffer; the codec wrapper stashes its private
 * state in `opaque`.
 *
 * Fields below `opaque` are populated by ops->open() and read-only
 * thereafter.
 */
typedef struct decoder {
    void *opaque;                       /* wrapper-private state */
    const struct decoder_ops *ops;      /* set by open() */

    /* Stream metadata, set by open(). */
    uint32_t sample_rate;               /* Hz, e.g. 44100 */
    uint16_t channels;                  /* 1 = mono, 2 = stereo */
    uint16_t bits_per_sample;           /* native bps; output is always 16 */
    uint64_t total_frames;              /* total PCM frames; 0 if unknown */
} decoder_t;

/*
 * decoder_ops — vtable for one codec. Wrappers expose a singleton via
 * a getter named e.g. flac_decoder_ops(). The pointer is process-static
 * — codecs are stateless beyond what's in decoder_t.
 */
typedef struct decoder_ops {
    /*
     * Format name for logging / dispatch. ASCII, lower-case.
     * Examples: "flac", "mp3", "aac", "alac", "vorbis", "opus", "wav".
     */
    const char *name;

    /*
     * Open the decoder on a memory buffer.
     *
     * `alloc` may be NULL — wrappers fall back to a default allocator
     * (malloc/free on sim; the audio engine on hw always provides a
     * non-NULL struct backed by our static sub-allocator).
     *
     * Returns 0 on success, negative on error (DECODER_ERR_*). On
     * success, fills d->opaque, d->sample_rate, d->channels,
     * d->bits_per_sample, d->total_frames.
     *
     * The src buffer must remain valid for the lifetime of the
     * decoder (until close()). Wrappers borrow, they don't copy.
     *
     * Note: this ABI assumes the entire compressed file is in memory.
     * Streaming-pull (Ogg/Opus from a long-running file source) will
     * need a separate open_stream() variant when we vendor those.
     */
    int (*open)(decoder_t *d, const void *src, size_t src_len,
                const decoder_alloc_t *alloc);

    /*
     * Decode up to max_frames PCM frames into out.
     *
     * out is a buffer of at least max_frames * channels * sizeof(int16_t)
     * bytes, written as L/R interleaved s16le.
     *
     * Returns:
     *   > 0: number of frames decoded
     *   = 0: end of stream
     *   < 0: error (DECODER_ERR_*)
     */
    int (*decode)(decoder_t *d, int16_t *out, int max_frames);

    /*
     * Seek to PCM frame `target_frame` (0-based). Returns 0 on
     * success, DECODER_ERR_UNSUPPORTED if the codec can't seek (e.g.,
     * a streaming Opus stream without granule-pos hints).
     */
    int (*seek)(decoder_t *d, uint64_t target_frame);

    /*
     * Close the decoder and release any resources held in d->opaque.
     * After close, the decoder_t is reset and may be re-opened.
     */
    void (*close)(decoder_t *d);
} decoder_ops_t;

/* Error codes returned by ops functions. */
enum {
    DECODER_OK            =  0,
    DECODER_ERR_INVALID   = -1,   /* malformed input */
    DECODER_ERR_TRUNCATED = -2,   /* unexpected end of stream */
    DECODER_ERR_UNSUPPORTED = -3, /* feature not supported (e.g. seek on raw stream) */
    DECODER_ERR_INTERNAL  = -4,   /* wrapped lib returned an unspecified error */
};

#endif /* CORE_CODECS_DECODER_H */
