/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/audio.c — hw backend for the hal_audio playback contract.
 *
 * Ties together the audio subsystem for interrupt-fed continuous
 * playback: I2C (i2c.c) -> WM8758 codec (wm8758.c) -> I2S serializer
 * (i2s.c), fed by the DMA engine (dma.c) instead of polled writes. A
 * ping-pong pair of PCM buffers is drained by DMA channel 0; each
 * DMA-completion IRQ kicks the other buffer and refills the drained one
 * from the registered source callback (the audio engine's fill function
 * on a real stream; a tone generator during bring-up).
 *
 * Freestanding-clean and asm-free (host-trace-testable). The source
 * callback runs in IRQ context, per the hal_audio contract in hal/hal.h.
 */

#include "hal.h"          /* hal_audio_* contract + audio_source_fn */
#include "pp5022.h"
#include "mmio.h"
#include "i2c.h"
#include "wm8758.h"
#include "i2s.h"
#include "dma.h"
#include "volume.h"       /* hal_codec_restore(): re-apply user state after a reset */
#include "irqlock.h"      /* hw_irq_save/restore: set_source quiescence          */
#include "audio.h"
#include "../../kernel/cache.h"   /* cache_commit(): flush before DMA reads */
#include "../../kernel/clock.h"   /* lock out frequency switches while streaming */

/*
 * Ping-pong PCM buffers: 8192 frames each = 32 KB = ~186 ms at 44.1 kHz. The
 * DMA byte-count field is 16 bits written as bytes-4 (05-audio.md, "DMA
 * engine"), so 32768 bytes encodes as 0x7FFC and fits with room to spare; the
 * ceiling is 65536 bytes = 16384 frames.
 *
 * *** WHAT THE REAL DEADLINE IS. *** This comment used to claim "the ISR can
 * be delayed ~180 ms without the DAC underrunning". That was wrong, and every
 * downstream decision (notably the UI's 150 ms repaint throttle) was reasoned
 * against the wrong number.
 *
 * The channel is programmed SINGLE | WAIT_REQ and there is no second
 * descriptor armed: the next buffer is only kicked from INSIDE the completion
 * ISR. So at the instant the completion IRQ fires, the only audio still in
 * flight is whatever the I2S TX FIFO holds — 16 frames, i.e. about 363 us at
 * 44.1 kHz. THAT is the deadline for getting into audio_dma_isr and issuing
 * the next kick. Not 186 ms. Anything that masks IRQs for longer than ~360 us
 * risks an audible gap, and the LCD present's IRQ-masked pixel stream is well
 * past that.
 *
 * *** CAN THIS BE FIXED IN HARDWARE? NO. *** The engine documented in
 * 05-audio.md is a single-shot channel: one command word carries the source
 * address and byte count, START launches it, and completion is reported by a
 * status read. There is no descriptor chain, no linked list, no shadow/next
 * register, and no way to queue a second transfer while one is running — so
 * the next buffer genuinely cannot be pre-armed. (Feeding the same I2S request
 * line from a second DMA channel is not a documented arrangement and the
 * arbitration between them is unspecified, so it is not a fix either.) The
 * deadline stays a FIFO depth; buffer size cannot change that.
 *
 * What the buffer size DOES buy is frequency: at 8192 frames a completion IRQ
 * lands every ~186 ms instead of every ~93 ms, halving the number of ~360 us
 * windows that a long IRQ-masked section can collide with. That is the honest
 * argument for the size — it lowers the probability of a miss, it does not
 * widen the deadline.
 *
 * Layout: interleaved int16 [L,R,L,R,...]. The DMA reads 32-bit words, and on
 * little-endian ARM the pair [L,R] in memory IS (R<<16)|L, which is exactly
 * the I2S FIFO packing — so the buffer feeds the FIFO with no repack.
 */
#define AUDIO_FRAMES_PER_BUF 8192u
#define AUDIO_BUF_BYTES      (AUDIO_FRAMES_PER_BUF * 4u)   /* 4 bytes/frame */

/*
 * Length of the fade applied to the tail of a SHORT read. A starved source
 * used to be spliced straight to zero, which is a step discontinuity in the
 * waveform — an audible click, once per underrun. ~64 frames (1.5 ms at
 * 44.1 kHz) is long enough to kill the click and far too short to hear as a
 * fade.
 */
#define AUDIO_TAIL_RAMP_FRAMES 64u

static int16_t          audio_buf[2][AUDIO_FRAMES_PER_BUF * 2u];
static audio_source_fn  g_source;
static void            *g_source_ud;
static volatile int      g_active;       /* buffer DMA is currently draining */
static volatile int      g_running;
static volatile uint32_t g_completions;
static volatile uint32_t g_underruns;    /* short reads seen by fill_buffer   */
static uint16_t          g_channels = 2; /* 1 = expand mono to the stereo link */
static uint32_t          g_rate     = 44100u;

/*
 * Pause/resume bookkeeping (hal.h promises "the internal buffer is not
 * cleared — a subsequent hal_audio_start resumes from where we left off").
 *
 * g_primed says both ping-pong buffers hold PCM already pulled from the ring.
 * hal_audio_start used to ignore that and unconditionally re-prime both from
 * the ring, so every pause/resume silently threw away up to two buffers
 * (~370 ms) of already-decoded, never-heard audio.
 *
 * g_kick_us timestamps the last kick. The DMA is paced by the I2S FIFO, which
 * drains at exactly the sample rate, so elapsed microseconds convert directly
 * into frames already clocked out of the active buffer — accurate to the FIFO
 * depth (~16 frames, 0.4 ms). The engine exposes no residual byte count, so
 * this is the only way to resume mid-buffer instead of replaying it.
 */
static volatile int      g_primed;
static volatile uint32_t g_kick_us;
static volatile uint32_t g_kick_bytes;   /* byte count of the outstanding kick */
/*
 * Per-buffer: it holds PCM pulled from the source that the DMA has not yet
 * been pointed at. Set by fill_buffer, cleared by audio_kick.
 *
 * Exists for one window. hal_audio_stop() clears g_running, then masks the
 * completion IRQ, then stops the engine and reads its status. A completion
 * that lands inside that window — after the clear, or one dma_playback_stop's
 * status read swallows — is acked and dropped by audio_dma_isr without the
 * refill it would normally do. The engine HAS finished that buffer, so the
 * stop samples it as fully done and hal_audio_start() moves on to the other
 * one, which is fresh; but the completion after THAT kicks the dropped buffer
 * again, still holding the PCM the listener already heard: ~186 ms replayed
 * on resume. The resume path checks the flags and refills on demand.
 */
static volatile int      g_filled[2];

/*
 * LATE RE-KICKS — the one audio failure this driver could not see.
 *
 * audio_underruns() counts SHORT READS: the ring had less PCM than a buffer
 * needed. That is decode starvation, and it is not the only way the sound
 * breaks. The other way is that the ring is perfectly full and the CPU simply
 * does not reach audio_dma_isr in time: the channel is SINGLE|WAIT_REQ with no
 * chained descriptor, so once a transfer completes the only audio still in
 * flight is the 16-frame I2S FIFO (~363 us at 44.1 kHz). Miss that window and
 * the DAC clocks out whatever the FIFO last held.
 *
 * From the outside those two look identical — a tick or a hiccup — but nothing
 * in the firmware distinguished them, because a late ISR produces no short
 * read. Anything that masks interrupts for longer than the FIFO can cause it;
 * the LCD pixel stream is the obvious suspect, and until now the only detector
 * was a person listening.
 *
 * At ISR entry we know when the transfer was kicked and how many bytes it
 * carried, so we know when it should have completed. Anything past that is
 * latency we did not have. Cheap: two timer reads and a compare per buffer,
 * about five times a second.
 */
/*
 * The I2S TX FIFO is 16 frames (05-audio.md). At 44.1 kHz that is ~363 us of
 * cover; at 48 kHz ~333. Use the tighter figure as the threshold so the
 * counter does not under-report on a 48 kHz album, and treat anything beyond
 * it as a real miss rather than jitter — a few microseconds of ISR entry
 * latency is normal and uninteresting.
 */
#define AUDIO_FIFO_SLACK_US  333u

static volatile uint32_t g_late_kicks;   /* completions serviced past the FIFO */
static volatile uint32_t g_late_worst_us;/* worst overshoot seen, microseconds */
/*
 * FIFO starvation INSIDE a transfer — see audio_fifo_service() in audio.h.
 * Sampled by the tick ISR while g_running: how many samples found the TX
 * FIFO empty, and the most free slots any sample saw (16 = empty; a healthy
 * stream sits at 0..4, the request level).
 */
static volatile uint32_t g_fifo_empty;
static volatile uint32_t g_fifo_worst_free;
/*
 * Bytes of the outstanding kick already clocked out when hal_audio_stop() cut
 * the DMA — sampled THERE, not recomputed on resume.
 *
 * hal_audio_start() used to derive this itself from (now - g_kick_us), which
 * is only the frames-consumed figure if no time has passed since the stop.
 * Across a real pause it also counts every second the user sat paused, so it
 * always saturated at g_kick_bytes: the remainder of the buffer the listener
 * was in the middle of hearing got dropped and playback jumped into the next
 * one. Audible as a stutter/skip on every unpause (device, 2026-07-27).
 */
static volatile uint32_t g_stop_done;

/*
 * WHERE THE DAC IS — hal_audio_frames_played().
 *
 * The player used to guess this: frames pulled from its ring, less the full
 * two-buffer depth of this driver. Never ahead of the listener, but behind by
 * 0..186 ms depending on where in the current buffer the DAC was, and every
 * gapless hand-over anchored its clock that far late. Everything needed for a
 * real answer was already here for the late-kick detector: the completion
 * count and the kick timestamp.
 *
 * g_played_at_kick is the number of frames the DAC had clocked out when the
 * outstanding kick began — every earlier kick's length summed: whole buffers
 * retired by the completion ISR, plus, when a resume re-kicks the remainder
 * of a buffer, the part of it the pause got through. The DAC's position NOW
 * is that plus the in-flight part of the outstanding kick: while running,
 * the frames the I2S clock has drained since g_kick_us (the same conversion
 * hal_audio_stop uses for the resume offset, accurate to the 16-frame FIFO);
 * stopped, the offset the stop sampled (g_stop_done), which is exactly where
 * the resume will pick up, so the count does not move across a pause.
 *
 * Silence counts: a padded tail is clocked out like any other frame and the
 * time it takes is time the listener waits through. Discarded PCM does not:
 * hal_audio_flush() folds the heard part of the active buffer into the base
 * and drops the rest unheard, so the count carries straight across a seek.
 * hal_audio_init() zeroes it — a new stream, a new zero.
 */
static volatile uint32_t g_played_at_kick;

/*
 * The codec is powered down and the I2S/MCLK clocks are gated — set by
 * hal_audio_suspend() and hal_audio_close(), cleared by hal_audio_wake() and
 * hal_audio_init(). It exists so the power-down sequence runs exactly once:
 * the UI closes the HAL on the active->inactive edge, and a stop that follows
 * a suspended pause would otherwise run wm8758_powerdown() against a codec
 * whose rails are already off and gate clocks that are already gated —
 * harmless register-wise, but a second I2C sequence for nothing, and a
 * hal_audio_wake() has to know whether there is anything to wake.
 */
static volatile int      g_cold;

/*
 * DMA-visible physical address of a buffer. SDRAM is dual-mapped: our .bss
 * lives at the post-MMAP0-remap logical base (0x00000000-based), and the
 * same bytes are reachable at the native SDRAM base (0x10000000 + offset).
 * The DMA engine may not honor the CPU's MMAP0 remap, so we hand it the
 * native alias, which addresses the same bytes either way.
 *
 * *** This is the #1 on-device risk for DMA playback. *** If the tone is
 * silent or garbled with the completion IRQ firing, the alias is wrong for
 * this SoC — try the raw logical address ((uint32_t)(uintptr_t)audio_buf[i]).
 */
static uint32_t buf_phys(int i)
{
    return SDRAM_NATIVE_BASE + (uint32_t)(uintptr_t)audio_buf[i];
}

/* Kick the DMA at buffer `i` from byte offset `off` for `bytes`, recording
 * when and how much so a later pause can work out how far it got. The buffer
 * is now the DMA's: whatever it holds is being consumed (see g_filled). */
static void audio_kick(int i, uint32_t off, uint32_t bytes)
{
    g_filled[i]  = 0;
    g_kick_us    = mmio_read32(USEC_TIMER_ADDR);
    g_kick_bytes = bytes;
    dma_playback_kick(buf_phys(i) + off, bytes);
}

/*
 * Refill buffer `i` from the source.
 *
 * A short read means the source is starved. The old behaviour was to zero-fill
 * from the short-read point, which splices the waveform to 0 instantly — a
 * step discontinuity, i.e. a click, on every underrun. Instead we fade the
 * last AUDIO_TAIL_RAMP_FRAMES of real audio down to zero and then pad, so a
 * starved decoder costs a soft drop-out rather than a pop. Underruns are also
 * COUNTED (audio_underruns()) so starvation is observable at all — it used to
 * be completely invisible.
 *
 * Mono sources are expanded to the stereo link here: the source writes
 * `frames` mono samples into the front of the buffer and we duplicate each
 * into an [L,R] pair, walking backwards so the expansion is safe in place.
 *
 * CACHE COHERENCY: this is a CPU write that the DMA then reads. The unified
 * cache is write-back, so we cache_commit() (flush dirty lines to SDRAM) after
 * filling — otherwise the DMA, reading the buffer's native SDRAM alias, would
 * fetch stale data. Called from both the priming path and the completion ISR.
 */
static void fill_buffer(int i)
{
    int16_t *buf = audio_buf[i];
    int got = 0;
    if (g_source != 0) {
        got = g_source(g_source_ud, buf, (int)AUDIO_FRAMES_PER_BUF);
    }
    if (got < 0) {
        got = 0;
    }
    unsigned n = (unsigned)got;
    if (n > AUDIO_FRAMES_PER_BUF) {
        n = AUDIO_FRAMES_PER_BUF;
    }

    /* Mono -> stereo, in place, back to front (dst index 2f >= src index f). */
    if (g_channels == 1u) {
        for (unsigned f = n; f-- != 0; ) {
            int16_t s = buf[f];
            buf[2u * f]      = s;
            buf[2u * f + 1u] = s;
        }
    }

    if (n < AUDIO_FRAMES_PER_BUF) {
        g_underruns++;

        /* Fade the tail of the real audio instead of cutting it. */
        unsigned ramp = n < AUDIO_TAIL_RAMP_FRAMES ? n : AUDIO_TAIL_RAMP_FRAMES;
        for (unsigned k = 0; k < ramp; k++) {
            unsigned f = n - ramp + k;
            /* Linear gain (ramp-1-k)/ramp, integer-only. */
            int32_t g = (int32_t)(ramp - 1u - k);
            buf[2u * f]      = (int16_t)(((int32_t)buf[2u * f]      * g) / (int32_t)ramp);
            buf[2u * f + 1u] = (int16_t)(((int32_t)buf[2u * f + 1u] * g) / (int32_t)ramp);
        }
        for (unsigned f = n; f < AUDIO_FRAMES_PER_BUF; f++) {
            buf[2u * f]      = 0;
            buf[2u * f + 1u] = 0;
        }
    }
    cache_commit();      /* flush so the DMA reads fresh PCM, not stale SDRAM */
    g_filled[i] = 1;
}

int hal_audio_init(uint32_t sample_rate, uint16_t channels)
{
    /*
     * Rate: the codec's PLL preset table decides what is reachable (44.1 kHz
     * and its 22.05 kHz half from preset 0; 48 / 32 / 24 kHz from preset 1 —
     * see wm8758.c). Anything else is rejected rather than silently resampled,
     * per the hal.h contract. That is a small set on purpose: a 48 kHz album,
     * a 32 kHz file and a 24 kHz podcast used to fail to open outright.
     */
    if (wm8758_set_rate(sample_rate) != 0) {
        return -1;
    }
    /*
     * Channels: 1 or 2. The I2S link is always stereo — the serializer clocks
     * two 16-bit frames per word and the DMA feeds it 32-bit [R<<16|L] pairs —
     * so mono is handled HERE by duplicating each source sample into both
     * halves (see fill_buffer). The caller does not have to pre-expand.
     */
    if (channels != 1u && channels != 2u) {
        return -1;
    }
    g_channels = channels;

    /*
     * ONE discipline for every stream start, whatever the caller did first.
     * A new stream over a running one is stopped here — soft-mute ramp over
     * the last of the old audio, THEN the DMA cut — so nothing below ever
     * re-clocks a DAC that is live or resets a FIFO the engine is feeding.
     * No caller does this today (the player stops before every open); the
     * invariant the rest of this function rests on — "not running means
     * muted" — should not depend on that staying true.
     */
    if (g_running) {
        hal_audio_stop();
    }

    i2c_init();
    int codec_bad;
    if (g_cold) {
        /*
         * COLD (boot, after the persistent-pause power-down, after a close):
         * the datasheet power-up, a WM_RESET first and a 100 ms VMID rise
         * inside. MCLK first — i2s_init ungates DEV_EXTCLOCKS, and the codec
         * masters its clocks from a PLL that needs it. The restore hook is
         * handed over BEFORE the bring-up: the reset wipes the user's
         * volume/balance/bass/treble and wm8758_init puts them back at the
         * datasheet's unmute-and-set-volume step.
         */
        i2s_init();
        wm8758_set_restore(hal_codec_restore);
        codec_bad = wm8758_init();
    } else {
        /*
         * WARM (a track change: Next/Prev, a row played over a playing
         * track, a rate change at a gapless boundary, the Play after a
         * paused skip): the codec is NOT reset. It used to be — a full
         * WM_RESET + VMID cycle per track, and since this morning a
         * power-down in front of it — and every one of those cycles was a
         * thump on the jack: the datasheet sequence is a power-up, and the
         * rails have no business moving between two songs. The codec is
         * muted (the stop left it so), warm, and clocked at the previous
         * track's rate; wm8758_retune writes nothing when that rate is this
         * stream's, and re-programs the PLL under the mute when it is not.
         * The I2S FIFO reset comes AFTER the retune so any clock wobble the
         * re-lock put on the link is flushed before the first kick. Nothing
         * to restore: no reset, so the user's codec state is still there.
         */
        codec_bad = wm8758_retune();
        i2s_init();
    }
    dma_playback_init();

    g_rate        = sample_rate;
    g_source      = 0;
    g_source_ud   = 0;
    g_active      = 0;
    g_running     = 0;
    g_primed      = 0;
    g_filled[0]   = 0;
    g_filled[1]   = 0;
    g_cold        = 0;       /* brought up (cold) or still up (warm) either way */
    g_completions = 0;
    g_underruns   = 0;
    g_late_kicks  = 0;
    g_late_worst_us = 0;
    g_fifo_empty  = 0;
    g_fifo_worst_free = 0;
    /* A new stream: the DAC has played none of it, and no kick from the old
     * one may be read as its in-flight part. */
    g_played_at_kick = 0;
    g_kick_bytes     = 0;
    g_stop_done      = 0;

    /*
     * Propagate codec bring-up failure. Without this the UI shows a moving
     * progress bar over total silence with nothing anywhere reporting why.
     * The signal is coarse by necessity — the PP502x I2C controller has no
     * per-byte NAK status (09-i2c.md), so the only thing observable from the
     * host side is the BUSY-clear timeout — but a wedged control bus is
     * exactly the case worth surfacing.
     */
    return codec_bad != 0 ? -2 : 0;
}

void hal_audio_set_source(audio_source_fn fn, void *userdata)
{
    /*
     * hal.h's quiescence guarantee: "when this function returns the previously
     * registered fn/userdata are no longer in flight in any callback ... safe
     * to free userdata immediately". Two plain global stores delivered neither
     * half of that — the completion ISR could be inside the OLD callback while
     * this returned, and could observe a torn (new fn, old userdata) pair
     * between the two stores.
     *
     * Masking IRQs at the core fixes both on this single-core, single-priority
     * design: audio_dma_isr only ever runs at IRQ level, so if we are executing
     * here with IRQs masked then no ISR is mid-callback (it would have had to
     * complete before we got the core back), and the pair updates atomically
     * with respect to it.
     */
    uint32_t f = hw_irq_save();
    g_source    = fn;
    g_source_ud = userdata;
    hw_irq_restore(f);
}

void hal_audio_start(void)
{
    if (g_running) {
        return;
    }
    i2s_tx_enable();
    /*
     * The DAC stays SOFT-MUTED until the DMA is streaming — the unmute is the
     * last thing this function does, after the kick, on both paths below.
     *
     * It used to be here, first. But the TX FIFO has been empty since the
     * stop (it drains in ~363 us) or since the reset a bring-up did, and
     * nothing feeds it until audio_kick: between the two the serializer emits
     * whatever it emits on underrun, and an unmuting DAC — ramping UP out of
     * soft-mute — plays it. On a cold start that window also holds two
     * 32 KB buffer fills and cache flushes. That was the noise on every
     * Play. With the kick first, the unmute ramp fades in real audio.
     */
    clock_set_audio_dma_active(1);             /* freeze the CPU/SDRAM clocks */
    mmio_write32(CPU_INT_EN_ADDR, DMA_MASK);   /* enable IRQ 26 */

    if (g_primed) {
        /*
         * RESUME. hal.h: "the internal buffer is not cleared — a subsequent
         * hal_audio_start resumes from where we left off." This used to
         * re-prime BOTH buffers from the ring, discarding up to ~370 ms of
         * already-decoded audio the listener never heard — an audible skip on
         * every unpause.
         *
         * Pick up inside the active buffer at the point the DMA had reached
         * when we stopped — g_stop_done, sampled by hal_audio_stop() at the
         * moment it cut the engine. Deriving it here instead (from
         * now - g_kick_us) counted the pause itself, so it always saturated
         * and dropped the rest of the buffer the listener was hearing.
         */
        uint32_t done = g_stop_done;     /* measured at stop; see g_stop_done */
        if (done > g_kick_bytes) {
            done = g_kick_bytes;         /* defensive: kick changed under us */
        }
        uint32_t left = g_kick_bytes - done;
        int switched  = 0;
        /* The part the pause got through is behind the kick that follows;
         * the resumed kick then counts from zero (see g_played_at_kick). */
        g_played_at_kick += done >> 2;
        if (left < 4u) {                  /* SIZE is bytes-4: 4 is the minimum */
            /* The active buffer had effectively finished: go straight to the
             * other one, which the completion ISR loaded. */
            g_active = g_active ^ 1;
            done     = 0;
            left     = AUDIO_BUF_BYTES;
            switched = 1;
        }
        /*
         * Whatever the completion after this one kicks must be fresh. It
         * normally is — the ISR that kicked the active buffer refilled the
         * other — but a completion dropped inside hal_audio_stop's window
         * (see g_filled) leaves that other buffer holding PCM the listener
         * has already heard. Refill on demand, BEFORE kicking: with only a
         * few bytes left in the active buffer the completion is immediate.
         * The refill runs with g_running still 0, so a stray completion
         * during it is a no-op rather than a double kick.
         */
        int other = g_active ^ 1;
        if (!g_filled[other]) {
            fill_buffer(other);
        }
        if (switched && !g_filled[g_active]) {
            fill_buffer(g_active);       /* defensive: never kick stale PCM */
        }
        g_running = 1;
        audio_kick(g_active, done, left);
        wm8758_mute(false);              /* PCM is flowing: NOW undo the stop's mute */
        return;
    }

    /* COLD START: prime both buffers, then kick buffer 0. The core I-bit must
     * already be unmasked (arch_irq_enable) by the caller. */
    fill_buffer(0);
    fill_buffer(1);
    g_active      = 0;
    g_completions = 0;
    g_running     = 1;
    g_primed      = 1;
    audio_kick(0, 0, AUDIO_BUF_BYTES);
    wm8758_mute(false);                  /* PCM is flowing: NOW undo the bring-up's mute */
}

void audio_dma_isr(void)
{
    dma_playback_ack();            /* read STATUS -> clear the pending IRQ */
    if (!g_running) {
        return;
    }

    /*
     * How late are we? The kick was stamped at g_kick_us and carried
     * g_kick_bytes (4 bytes per stereo frame), so it should have drained after
     * frames/rate seconds. Past that, the FIFO is the only thing still holding
     * the output up. See g_late_kicks.
     */
    {
        uint32_t now     = mmio_read32(USEC_TIMER_ADDR);
        uint32_t elapsed = now - g_kick_us;
        uint32_t frames  = g_kick_bytes >> 2;
        uint32_t due_us  = (uint32_t)(((uint64_t)frames * 1000000u) / g_rate);
        if (elapsed > due_us) {
            uint32_t over = elapsed - due_us;
            if (over > AUDIO_FIFO_SLACK_US) {
                g_late_kicks++;
                if (over > g_late_worst_us) {
                    g_late_worst_us = over;
                }
            }
        }
    }

    int just = g_active;
    int next = just ^ 1;

    /* The kick that just completed has been clocked out in full. Retire it
     * BEFORE the next kick overwrites g_kick_bytes. */
    g_played_at_kick += g_kick_bytes >> 2;

    /* Keep the FIFO fed with the already-filled other buffer FIRST, then
     * refill the one that just drained. This ordering is what makes the
     * deadline a FIFO depth rather than zero — see the header comment. */
    audio_kick(next, 0, AUDIO_BUF_BYTES);
    g_active = next;
    g_completions++;
    fill_buffer(just);
}

int hal_audio_drain(uint32_t timeout_ms)
{
    /*
     * The last ~186 ms of every track used to be thrown away: the player
     * advances on ring-empty, but at that moment the two ping-pong buffers
     * still hold un-clocked audio that hal_audio_stop -> dma_playback_stop
     * simply discards. Call this first and that tail actually reaches the DAC.
     *
     * Both in-flight buffers have retired once the completion count has
     * advanced by two. After the ring empties fill_buffer pads with silence
     * (ramped, not spliced), so waiting two buffers plays out the real tail
     * and then quiet — never more music.
     *
     * PRECONDITION: IRQs enabled at the core (the count only moves in the
     * completion ISR). Bounded by wall clock, so a stalled DMA cannot hang
     * the caller: returns -1 on timeout, 0 when drained.
     */
    if (!g_running) {
        return 0;
    }
    uint32_t target = g_completions + 2u;
    uint32_t t0     = mmio_read32(USEC_TIMER_ADDR);
    /* Clamp before scaling: USEC_TIMER wraps every ~71 minutes, so anything
     * near that is meaningless anyway, and the multiply must not overflow. */
    uint32_t limit  = timeout_ms > 60000u ? 60000000u : timeout_ms * 1000u;
    while ((int32_t)(g_completions - target) < 0) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) > limit) {
            return -1;
        }
    }
    return 0;
}

void hal_audio_stop(void)
{
    /*
     * MUTE FIRST, and let the mute FINISH. Cutting the DMA with the DAC live
     * leaves the serializer repeating whatever the FIFO last held and drops
     * the output rail mid-waveform — the click on every stop and every skip.
     * wm8758_mute(true) does not return until the soft-mute ramp is over, so
     * the ramp fades the real audio the DMA is still delivering, and the cut
     * below lands on a DAC that is already silent.
     *
     * Only while running: a DAC that is not streaming is muted by invariant
     * (the bring-up leaves it muted, and only hal_audio_start unmutes), so a
     * stop-while-stopped — a seek while paused — has nothing to mute and
     * should not pay the ramp wait for it.
     */
    int was_running = g_running;

    if (was_running) {
        wm8758_mute(true);
    }
    g_running = 0;
    mmio_write32(CPU_INT_DIS_ADDR, DMA_MASK);   /* mask IRQ 26 */
    dma_playback_stop();
    /*
     * Sample the DMA position NOW — after dma_playback_stop(), because the
     * engine kept clocking bytes through the mute above (the I2C transfer
     * plus the ramp it waits out: ~23 ms of audio at 44.1 kHz, faded, and
     * the resume picks up after it), and before any more time can pass.
     * Rounded DOWN to a whole frame so L/R phase cannot invert.
     *
     * ONLY when we were actually running. Stopping an already-stopped engine
     * used to recompute this from now - g_kick_us, which counts the entire
     * time we sat stopped: g_stop_done saturated at g_kick_bytes and the next
     * resume skipped the whole buffer the listener was paused inside. Any
     * stop-while-paused hit it — seeking while paused, most visibly.
     */
    if (was_running) {
        uint32_t elapsed = mmio_read32(USEC_TIMER_ADDR) - g_kick_us;
        uint32_t frames  = (uint32_t)(((uint64_t)elapsed * g_rate) / 1000000u);
        uint32_t done    = frames * 4u;
        if (done > g_kick_bytes) {
            done = g_kick_bytes;
        }
        g_stop_done = done;
    }
    clock_set_audio_dma_active(0);              /* clocks may move again */
    /* g_primed deliberately survives: the buffers still hold unplayed PCM and
     * hal_audio_start() resumes into them (see there). A caller that has
     * replaced what the source will produce — a seek — says so with
     * hal_audio_flush(). */
}

void hal_audio_flush(void)
{
    /*
     * Drop the ping-pong contents so the next hal_audio_start() COLD-primes
     * from the source instead of resuming into them.
     *
     * hal_audio_stop() keeps g_primed on purpose — that is what makes unpause
     * seamless — but the two buffers hold up to 2 x 186 ms of already-decoded
     * PCM, and after a seek that PCM belongs to the position the listener just
     * left. player_seek_to stopped, re-primed the ring at the new offset and
     * started again, so the resume path kicked the remainder of the old active
     * buffer, the ISR kicked the other old buffer, and only the third came
     * from the new position: up to ~370 ms of the old spot, then a hard cut.
     *
     * Only the flag is cleared. The PCM itself is overwritten by fill_buffer
     * during the cold prime, and zeroing 64 KB here would just be slower.
     * g_stop_done goes too so a stale resume offset cannot outlive it — but
     * the frames it stands for WERE heard, so they move into the played
     * count's base first; only the unheard remainder is discarded. Not while
     * running: g_stop_done is stale then, and the count is being timed off
     * the live kick anyway.
     */
    if (!g_running) {
        uint32_t heard = g_stop_done;
        if (heard > g_kick_bytes) {
            heard = g_kick_bytes;
        }
        g_played_at_kick += heard >> 2;
    }
    g_primed    = 0;
    g_stop_done = 0;
}

uint32_t hal_audio_frames_played(void)
{
    /*
     * One consistent (base, kick) pair: the completion ISR advances
     * g_played_at_kick and restamps the kick together, and a read torn across
     * that lands a whole buffer off. Masking IRQs for four loads is ~1 us,
     * nowhere near the ~360 us FIFO deadline (and a no-op on the host).
     */
    uint32_t f     = hw_irq_save();
    uint32_t base  = g_played_at_kick;
    uint32_t kickf = g_kick_bytes >> 2;
    uint32_t done;
    if (g_running) {
        uint32_t elapsed = mmio_read32(USEC_TIMER_ADDR) - g_kick_us;
        done = (uint32_t)(((uint64_t)elapsed * g_rate) / 1000000u);
    } else {
        done = g_stop_done >> 2;     /* where the stop cut it; see there */
    }
    hw_irq_restore(f);
    if (done > kickf) {
        done = kickf;                /* a late completion: the buffer is out,
                                      * the FIFO is repeating, nothing more
                                      * has been heard */
    }
    return base + done;
}

/*
 * Codec off, clocks gated, everything else untouched. Shared by suspend and
 * close; see g_cold for why it runs at most once between bring-ups.
 */
static void codec_power_off(void)
{
    if (g_cold) {
        return;
    }
    wm8758_powerdown();      /* codec cold (mute+VMID discharge) — MCLK still live */
    i2s_disable();           /* then gate the I2S + codec-MCLK clocks              */
    g_cold = 1;
}

/*
 * Boot: put the codec into a KNOWN cold state, whatever the previous image,
 * a Menu+Select warm reset (the codec's rails stay up across it) or disk
 * mode left behind. Nothing else touches the codec until the first track's
 * hal_audio_init, so without this an idle device inherits a live VMID and
 * open headphone amps from whoever ran last — a steady hiss out of the jack
 * with nothing playing (device, 2026-09-13). Same sequence a persistent
 * pause uses; the powerdown table is safe against a codec that is already
 * at its reset defaults.
 */
void hal_audio_boot_quiet(void)
{
    g_cold = 0;              /* assume live: force the powerdown to run once */
    codec_power_off();
}

void hal_audio_suspend(void)
{
    /*
     * Only over a stop. Powering the codec down under a running DMA would
     * leave the engine streaming into a FIFO nothing clocks out — the
     * completion IRQ would simply stop arriving and the player would sit in
     * hal_audio_drain's timeout. Refusing is right: the caller pauses first,
     * and a suspend that lands while playing is a caller bug, not a request.
     *
     * Deliberately NOT touching g_primed, g_stop_done, g_active, g_kick_bytes
     * or the source: they are what hal_audio_start() resumes into, and keeping
     * them is the entire difference between this and hal_audio_close(). See
     * audio.h.
     */
    if (g_running) {
        return;
    }
    codec_power_off();
}

int hal_audio_wake(void)
{
    if (!g_cold) {
        return 0;
    }
    /*
     * The same bring-up hal_audio_init performs, minus the state reset. The
     * rate preset is still latched inside wm8758.c from the last set_rate, so
     * wm8758_init programs the PLL for the stream we paused; i2s_init
     * re-pulses the block out of reset and re-ungates DEV_I2S + DEV_EXTCLOCKS;
     * dma_playback_init re-arms the channel's static config (the engine itself
     * kept nothing across the stop — every kick reprograms address and count).
     *
     * The I2S TX FIFO is cleared by i2s_init. That loses nothing: the FIFO's
     * ~16 frames were already written off when hal_audio_stop() sampled the
     * resume offset (it rounds to the frame the DMA had handed over, and the
     * FIFO tail is the ~0.4 ms of slack that offset is documented to carry).
     *
     * The restore hook is re-registered rather than assumed, for the same
     * reason hal_audio_init registers it every time: wm8758_init's first act
     * is a WM_RESET, and the user's volume/balance/tone must come back with
     * the codec, not a track change later.
     */
    i2c_init();
    i2s_init();
    wm8758_set_restore(hal_codec_restore);
    int codec_bad = wm8758_init();
    dma_playback_init();
    g_cold = 0;
    return codec_bad != 0 ? -2 : 0;
}

void hal_audio_close(void)
{
    hal_audio_stop();
    codec_power_off();       /* no-op if a suspended pause already did it        */
    g_primed = 0;            /* buffers are no longer related to any live stream   */
}

uint32_t audio_late_kicks(void)    { return g_late_kicks; }
uint32_t audio_late_worst_us(void) { return g_late_worst_us; }

void audio_fifo_service(void)
{
    /* Only while a transfer is supposed to be feeding the FIFO: stopped, the
     * FIFO is empty by design and would count every tick. One status read;
     * the same field i2s_write_stereo polls, so it is known to be live. */
    if (!g_running) {
        return;
    }
    uint32_t free = (mmio_read32(IISFIFO_CFG_ADDR) >> IISFIFO_CFG_TXFREE_SHIFT)
                    & IISFIFO_CFG_TXFREE_MASK;
    if (free > g_fifo_worst_free) {
        g_fifo_worst_free = free;
    }
    if (free >= IIS_TX_FIFO_DEPTH) {
        g_fifo_empty++;
    }
}

uint32_t audio_fifo_empty_samples(void) { return g_fifo_empty; }
uint32_t audio_fifo_worst_free(void)    { return g_fifo_worst_free; }

uint32_t audio_dma_completions(void)
{
    return g_completions;
}

uint32_t audio_underruns(void)
{
    return g_underruns;
}
