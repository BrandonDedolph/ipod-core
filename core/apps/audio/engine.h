/*
 * core/apps/audio/engine.h — codec-agnostic playback engine.
 *
 * Sits between decoder_t (compressed input) and hal_audio (DAC out).
 * Owns the decoder and a ring buffer of PCM samples.
 *
 * Threading model:
 *
 *   - audio_engine_play / pause / resume / stop / pump are called from
 *     the "control thread" (whoever runs the main loop on sim, or
 *     the audio task on hw).
 *   - audio_engine_pump pulls from the decoder and fills the ring.
 *     Call it whenever you have spare time; the ring smooths over
 *     the gap.
 *   - The HAL drains the ring on its own audio thread (sim) / IRQ
 *     context (hw) via the source callback the engine registers
 *     with hal_audio_set_source. The drain side never touches the
 *     decoder, so there's no contention.
 *   - The ring is single-producer single-consumer (SPSC). Index
 *     publishes use release/acquire ordering (via __atomic_store_n /
 *     __atomic_load_n with explicit memorders) so that on the
 *     dual-core PP5022 the consumer can't observe a bumped write_idx
 *     before the ring stores that filled that slot — and vice versa
 *     for read_idx. On the sim host (x86 TSO) these compile to plain
 *     loads/stores; on ARM they emit `dmb ish` barriers. Same source.
 *
 * Lifecycle:
 *
 *     audio_engine_t engine;
 *     audio_engine_init(&engine);
 *
 *     audio_engine_play(&engine, flac_decoder_ops(), bytes, len);
 *     while (!audio_engine_eos(&engine)) {
 *         audio_engine_pump(&engine);
 *         do_other_main_loop_work();
 *     }
 *     audio_engine_stop(&engine);
 *     audio_engine_close(&engine);
 *
 * Fixed at 16-bit s16 stereo internally — matches both the decoder
 * ABI's output format and the HAL's expectation. Mono input is
 * upmixed to stereo at decode time for one canonical engine state.
 */

#ifndef CORE_APPS_AUDIO_ENGINE_H
#define CORE_APPS_AUDIO_ENGINE_H

#include "../../codecs/decoder.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Ring capacity in PCM frames. 131,072 frames ≈ 2.97 s at 44.1 kHz
 * stereo, ~512 KB.
 *
 * On hw this is sized so a single HDD spin-up fills it and the disk
 * can spin down for many seconds before the next refill. On sim it's
 * just comfortable headroom so a stalled main loop doesn't underrun.
 *
 * Must be a power of two so the index wrap is a mask instead of a mod.
 */
#define AUDIO_RING_FRAMES (1u << 17)   /* 131,072 frames ≈ 2.97 s @ 44.1 kHz */

/*
 * audio_engine_t — opaque to callers in principle, but defined here so
 * it can live on the caller's stack (one engine per process; statically
 * sized).
 *
 * Don't poke fields directly; use the API below.
 */
typedef struct audio_engine {
    /* Decoder state. NULL when nothing is loaded. */
    decoder_t            decoder;
    const decoder_ops_t *ops;
    void                *src_bytes;          /* heap-allocated copy of the file */
    size_t               src_len;
    bool                 has_decoder;
    bool                 decoder_eof;        /* decoder returned 0 (EOS) */

    /* SPSC ring of interleaved s16 stereo frames.
     *
     * write_idx is published by the producer (pump) with release
     * ordering; read with acquire by the consumer.
     * read_idx is published by the consumer (HAL audio callback)
     * with release ordering; read with acquire by the producer.
     *
     * Same-thread reads (producer reading its own write_idx, etc.)
     * use relaxed loads. See engine.c for the helpers. */
    int16_t              ring[AUDIO_RING_FRAMES * 2];
    uint32_t             write_idx;          /* total frames produced */
    uint32_t             read_idx;           /* total frames consumed */

    /* Stream metadata, captured at play() time. */
    uint32_t             sample_rate;
    uint16_t             channels;           /* engine output is always 2; this is decoder native */

    /* Run-state flags. Updated only by the control thread. */
    bool                 playing;            /* hal_audio_start has been called */
} audio_engine_t;

/* ---------- Lifecycle --------------------------------------------- */

/*
 * Initialize an engine in place. Must be called once before any
 * other audio_engine_* function. Doesn't touch the HAL.
 */
void audio_engine_init(audio_engine_t *e);

/*
 * Free any resources held by the engine. Safe to call multiple times.
 * Implicitly stops if currently playing.
 */
void audio_engine_close(audio_engine_t *e);

/* ---------- Playback control --------------------------------------- */

/*
 * Open `ops` on `bytes` (which the engine takes ownership of via a
 * malloc + memcpy — caller may free `bytes` after this returns), and
 * begin playing. Configures hal_audio_init, registers the engine's
 * fill callback, calls hal_audio_start.
 *
 * Returns 0 on success, negative on failure (invalid input, decoder
 * open failure, hal_audio_init failure, oom).
 *
 * If a track is currently playing, stops it first.
 */
int audio_engine_play(audio_engine_t *e,
                      const decoder_ops_t *ops,
                      const void *bytes, size_t len);

/*
 * Pause output. Decoder state preserved; ring contents preserved.
 * audio_engine_resume continues seamlessly.
 */
void audio_engine_pause(audio_engine_t *e);

/* Resume from pause. */
void audio_engine_resume(audio_engine_t *e);

/*
 * Stop playback. Closes the decoder, frees the source bytes, drops
 * any pending PCM. After this, audio_engine_play can be called again
 * with a new track.
 */
void audio_engine_stop(audio_engine_t *e);

/* ---------- Pump ---------------------------------------------------- */

/*
 * Decode more frames into the ring if there's space. Call this
 * frequently from the main loop (every UI frame is plenty). Cheap
 * if the ring is already full — just returns 0.
 *
 * Returns the number of frames produced this call (>= 0).
 */
int audio_engine_pump(audio_engine_t *e);

/* ---------- Status -------------------------------------------------- */

/*
 * True when the decoder has signaled EOS *and* the ring has fully
 * drained. Use as the loop condition for "play to the end."
 */
bool audio_engine_eos(const audio_engine_t *e);

/* True between play() and stop(). */
bool audio_engine_is_playing(const audio_engine_t *e);

/*
 * Frames currently buffered in the ring (between decode and DAC).
 * For diagnostics / latency calculation.
 */
uint32_t audio_engine_ring_fill(const audio_engine_t *e);

#endif /* CORE_APPS_AUDIO_ENGINE_H */
