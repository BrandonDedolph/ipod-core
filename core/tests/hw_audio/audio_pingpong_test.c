/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_audio/audio_pingpong_test.c — the ping-pong PCM buffer state
 * machine in hal/hw/audio.c, driven host-side under -DMMIO_MOCK.
 *
 * The existing audio_trace_test asserts the *register grammar* of the driver
 * chain (i2c/wm8758/i2s/dma) but never calls hal_audio_start, audio_dma_isr or
 * hal_audio_stop — so the part of audio.c that actually decides WHICH bytes
 * reach the DAC, and in what order, had no test at all. That state machine is
 * where a glitch lives: a buffer kicked twice repeats ~100 ms of audio, a
 * buffer never kicked drops it, and a short source read that doesn't zero its
 * tail plays back whatever the previous track left behind.
 *
 * WRITTEN AGAINST THE CONTRACT, NOT THE IMPLEMENTATION. Everything here is
 * derived from hal/hal.h's hal_audio_* documentation:
 *   - "Fill buf with up to frames frames ... If you write fewer than frames,
 *      the HAL pads the remainder with silence."
 *   - "hal_audio_stop pauses output. The internal buffer is not cleared — a
 *      subsequent hal_audio_start resumes from where we left off."
 *   - "Pass fn = NULL to clear (silence on next pull)."
 * Buffer SIZE is deliberately never asserted: it is a tuning knob (currently
 * 4096 frames) and the test discovers it from the `frames` the driver asks
 * for. The test should survive that knob moving.
 *
 * The source records the buffer pointer it was handed, so the test can inspect
 * exactly the memory the driver then hands to the DMA — and cross-check that
 * the physical address kicked into the DMA engine really is that buffer's
 * (phys == SDRAM_NATIVE_BASE + (uint32_t)buffer, the dual-mapping audio.c
 * relies on).
 */

#include <stdio.h>
#include <string.h>

#include "pp5022.h"
#include "hal.h"
#include "audio.h"
#include "dma.h"
#include "mmio_mock.h"
#include "../xfail.h"

/* Count of cache_commit() calls, from audio_test_stubs.c. */
unsigned audio_test_cache_commits(void);
/* Count of codec state-restore callbacks — one per wm8758_init, i.e. one per
 * codec RESET. Lets a test see that a wake really re-initialised the codec. */
unsigned audio_test_codec_restores(void);

/* ---- counting source ------------------------------------------------
 * Every frame it produces carries a globally unique, monotonically increasing
 * value in BOTH channels. That turns "did the driver repeat, drop or reorder
 * audio?" into a statement about integers we can check exactly. */

static uint32_t g_next_val;     /* next value the source will emit           */
static int      g_short_after;  /* after this many calls, produce a short read */
static int      g_short_frames; /* how many frames the short read produces    */
static int      g_calls;
static int      g_return_neg;   /* source reports an error instead of frames  */

#define MAX_CALLS 32
static int16_t *g_call_buf[MAX_CALLS];    /* buffer handed to each call      */
static int      g_call_frames[MAX_CALLS]; /* frames requested                */
static int      g_call_got[MAX_CALLS];    /* frames produced                 */
static uint32_t g_call_first[MAX_CALLS];  /* first value written             */

static int counting_source(void *ud, int16_t *buf, int frames)
{
    (void)ud;
    int want = frames;
    if (g_return_neg) {
        want = -1;
    } else if (g_short_after >= 0 && g_calls == g_short_after) {
        want = g_short_frames;
    }

    if (g_calls < MAX_CALLS) {
        g_call_buf[g_calls]    = buf;
        g_call_frames[g_calls] = frames;
        g_call_got[g_calls]    = want;
        g_call_first[g_calls]  = g_next_val;
    }
    g_calls++;

    if (want < 0) {
        return want;
    }
    for (int f = 0; f < want; f++) {
        /* Values start at 1 so "0" unambiguously means "silence padding". */
        int16_t v = (int16_t)((g_next_val++ & 0x7FFFu) + 1u);
        buf[2 * f]     = v;
        buf[2 * f + 1] = v;
    }
    return want;
}

static void source_reset(void)
{
    g_next_val    = 0;
    g_calls       = 0;
    g_short_after = -1;
    g_short_frames = 0;
    g_return_neg  = 0;
    memset(g_call_buf, 0, sizeof g_call_buf);
    memset(g_call_frames, 0, sizeof g_call_frames);
    memset(g_call_got, 0, sizeof g_call_got);
    memset(g_call_first, 0, sizeof g_call_first);
}

/* ---- DMA kick observation -------------------------------------------
 * dma_playback_kick writes the source address and the byte count into the
 * channel-0 registers; the mock bus records them. Reading the kicks back out
 * of the log is how the test sees which buffer the DAC was pointed at. */

#define MAX_KICKS 64
static uint32_t g_kick_addr[MAX_KICKS];
static uint32_t g_kick_bytes[MAX_KICKS];
static int      g_kicks;

/* Rescan the whole mock log and rebuild the kick list. A kick is identified by
 * the write to the channel-0 source-address register; the byte count is the
 * next write to the size register. */
static void collect_kicks(void)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    g_kicks = 0;
    for (size_t i = 0; i < len && g_kicks < MAX_KICKS; i++) {
        if (log[i].op != MMIO_OP_WRITE || log[i].addr != DMA0_RAM_ADDR_ADDR) {
            continue;
        }
        g_kick_addr[g_kicks] = log[i].value;
        g_kick_bytes[g_kicks] = 0;
        for (size_t j = i + 1; j < len; j++) {
            if (log[j].op == MMIO_OP_WRITE && log[j].addr == DMA0_CMD_ADDR) {
                g_kick_bytes[g_kicks] = log[j].value;
                break;
            }
        }
        g_kicks++;
    }
}

/* ---- bus ORDER at a transition --------------------------------------
 * The DMA kick/stop and the codec's DACCTRL writes land in the same log, in
 * the order the driver issued them. That order is the whole difference
 * between a silent transition and an audible one: an unmute that precedes
 * the kick plays the idle serializer, a DMA cut that precedes the mute cuts
 * a live DAC. These return log INDICES so a test can compare them. */

/* First DMA0_CMD write whose START bit is set (started=1, a kick) or clear
 * (started=0, a stop); -1 if none. */
static long log_first_dma_cmd(int started)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == DMA0_CMD_ADDR &&
            ((log[i].value & DMA_CMD_START) != 0) == (started != 0)) {
            return (long)i;
        }
    }
    return -1;
}

/* First codec control write carrying payload (b0, b1) — the DATA0 write's
 * index, its DATA1 partner being the next DATA1 write after it; -1 if none.
 * b0 = reg<<1 | data bit 8, b1 = data low byte (05-audio.md). */
static long log_first_codec_write(uint8_t b0, uint8_t b1)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    for (size_t i = 0; i < len; i++) {
        if (log[i].op != MMIO_OP_WRITE || log[i].addr != I2C_DATA0_ADDR ||
            log[i].value != b0) {
            continue;
        }
        for (size_t j = i + 1; j < len; j++) {
            if (log[j].op == MMIO_OP_WRITE && log[j].addr == I2C_DATA1_ADDR) {
                if (log[j].value == b1) {
                    return (long)i;
                }
                break;
            }
        }
    }
    return -1;
}

/* Codec payloads the ordering cases look for (reg<<1 | bit8, low byte). */
#define CW_DACCTRL_UNMUTE_B0  0x14   /* DACCTRL 0x0A                          */
#define CW_DACCTRL_UNMUTE_B1  0x08   /*   = DACOSR128                          */
#define CW_DACCTRL_MUTE_B1    0x48   /*   = DACOSR128 | SOFTMUTE               */
#define CW_DACCTRL_OSR64_B1   0x40   /*   = SOFTMUTE alone (OSR dropped): bad  */
#define CW_PWRMGMT1_OFF_B0    0x02   /* PWRMGMT1 0x01 = 0: VMID/BIAS/PLL off  */
#define CW_PWRMGMT1_PLLOFF_B1 0x0D   /*   = BIASEN|BUFIOEN|VMIDSEL_75K         */
#define CW_PWRMGMT1_RUN_B1    0x2D   /*   = PLLEN|BIASEN|BUFIOEN|VMIDSEL_75K   */
#define CW_RESET_B0           0x00   /* WM_RESET                               */
#define CW_OUT4TOADC_B0       0x54   /* OUT4TOADC 0x2A                         */
#define CW_OUT4TOADC_DRAIN_B1 0x14   /*   = POBCTRL | VMIDTOG: VMID discharge  */
#define CW_PLLN_B0            0x48   /* PLLN 0x24                              */
#define CW_PLLN_48_B1         0x18   /*   = PLLPRESCALE | 8 (the 48 kHz preset)*/

/* First write of `value` to `addr`; -1 if none. */
static long log_first_write(uint32_t addr, uint32_t value)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == addr &&
            log[i].value == value) {
            return (long)i;
        }
    }
    return -1;
}

/* Physical address audio.c hands the DMA for a given host buffer. */
static uint32_t phys_of(const int16_t *p)
{
    return SDRAM_NATIVE_BASE + (uint32_t)(uintptr_t)p;
}

/* Program every status register the chain polls so no bounded spin runs long.
 * Values are "ready"/"idle" for each poll; the grammar itself is asserted by
 * audio_trace_test, not here. */
static void bus_ready(void)
{
    mmio_mock_reset();
    /* Every register the playback path polls (I2C BUSY, the DMA channel's
     * BUSY/INTR) reads 0 by default on the mock bus, which is exactly the
     * "idle / ready" state — so no bounded spin here runs to its limit. */
}

/* Set the free-running microsecond timer. Note bus_ready() puts it back to
 * 0 along with everything else. */
static void at_us(uint32_t us)
{
    mmio_mock_set_read(USEC_TIMER_ADDR, us);
}

/* The int16 sample the counting source writes for the n-th frame it ever
 * produced. Values start at 1 so 0 unambiguously means "silence padding". */
static int16_t sample_for(uint32_t n)
{
    return (int16_t)((n & 0x7FFFu) + 1u);
}

/* Host buffer behind a DMA physical address, or NULL if the driver kicked
 * something the source was never handed. */
static const int16_t *buf_for_phys(uint32_t phys)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        if (g_call_buf[i] != 0 && phys_of(g_call_buf[i]) == phys) {
            return g_call_buf[i];
        }
    }
    return 0;
}

/*
 * First PCM sample in the buffer the driver LAST pointed the DMA at, i.e. the
 * next audio the DAC will actually play. Valid immediately after a start() or
 * an ISR: audio.c kicks the already-filled buffer and only then refills the
 * OTHER one, so the kicked buffer's contents are still the ones being handed
 * out. Returns 0x7FFF_FFFF if nothing was kicked.
 */
#define NO_KICK 0x7FFFFFFF
static int32_t last_kick_first_sample(void)
{
    collect_kicks();
    if (g_kicks == 0) {
        return NO_KICK;
    }
    const int16_t *b = buf_for_phys(g_kick_addr[g_kicks - 1]);
    return b ? (int32_t)b[0] : NO_KICK;
}

/* Bring the driver to a known state: stopped, source re-armed, counters and
 * the mock log cleared, then started. hal_audio_start() is a no-op while the
 * driver is already running, so the stop is what makes this deterministic. */
/*
 * A genuinely COLD start.
 *
 * hal_audio_stop() no longer discards the primed buffers — that is the whole
 * point of the pause/resume fix (hal.h promises "the internal buffer is not
 * cleared ... a subsequent hal_audio_start resumes from where we left off",
 * and start() used to re-prime both buffers unconditionally, dropping up to a
 * full buffer of music on every un-pause). Only hal_audio_close() severs the
 * buffers from the stream. So a test that wants to observe priming has to
 * close and re-init, not stop and re-start.
 */
static void fresh_start(void)
{
    hal_audio_close();
    bus_ready();
    hal_audio_init(44100u, 2u);
    source_reset();
    bus_ready();
    hal_audio_set_source(counting_source, 0);
    hal_audio_start();
}

int main(void)
{
    xfail_ctx c = { "audio-pingpong", 0, 0, 0 };

    /* --- 1. init accepts the formats it advertises, and only those ------
     * 44.1 kHz stereo used to be the ONLY accepted format, which meant a
     * 48 kHz album or a mono podcast failed to open and the player skipped it
     * with no explanation. The codec now reconfigures its PLL per rate, so the
     * supported set is a real set — but it is still a set, and a rate the DAC
     * cannot clock must be refused rather than played at the wrong speed. */
    bus_ready();
    xpect(&c, "init accepts 44.1 kHz stereo", hal_audio_init(44100u, 2u) == 0);
    bus_ready();
    xpect(&c, "init accepts 48 kHz stereo",   hal_audio_init(48000u, 2u) == 0);
    bus_ready();
    xpect(&c, "init accepts mono",            hal_audio_init(44100u, 1u) == 0);
    bus_ready();
    xpect(&c, "init rejects a rate the PLL has no preset for",
          hal_audio_init(96000u, 2u) != 0);
    bus_ready();
    xpect(&c, "init rejects a channel count that is neither mono nor stereo",
          hal_audio_init(44100u, 6u) != 0);
    bus_ready();
    xpect(&c, "init re-accepts 44.1 kHz stereo", hal_audio_init(44100u, 2u) == 0);

    /* --- 2. start primes from the source and kicks a real buffer ------- */
    hal_audio_set_source(counting_source, 0);
    fresh_start();
    collect_kicks();

    xpect(&c, "start pulls from the source", g_calls > 0);
    xpect(&c, "start kicks the DMA exactly once", g_kicks == 1);
    if (g_calls == 0 || g_kicks == 0) {
        fprintf(stderr, "audio-pingpong: driver never started; "
                        "the rest of this suite cannot run\n");
        return 1;
    }

    const int frames = g_call_frames[0];
    xpect(&c, "source is asked for a positive frame count", frames > 0);
    xpect(&c, "start primes both ping-pong buffers before kicking",
          g_calls == 2 && g_call_buf[0] != g_call_buf[1]);
    xpect(&c, "every pull asks for the same frame count",
          g_call_frames[1] == frames);
    /* The command word carries the length as (bytes - DMA_SIZE_BIAS) in
     * DMA_CMD_SIZE_MASK, with START set. 4 bytes per interleaved stereo s16
     * frame is the packing audio.c documents. */
    xpect(&c, "kick length == frames * 4 bytes",
          (g_kick_bytes[0] & DMA_CMD_SIZE_MASK) + DMA_SIZE_BIAS ==
              (uint32_t)frames * 4u);
    xpect(&c, "kick sets the START bit",
          (g_kick_bytes[0] & DMA_CMD_START) != 0);
    xpect(&c, "kick address is a primed buffer, in the SDRAM alias",
          g_kick_addr[0] == phys_of(g_call_buf[0]));
    xpect(&c, "the buffer kicked first is the one filled first",
          last_kick_first_sample() == sample_for(0));

    /* --- 3. buffers ping-pong: exactly two, strictly alternating ------- */
    fresh_start();
    for (int i = 0; i < 6; i++) {
        audio_dma_isr();
    }
    collect_kicks();

    xpect(&c, "one kick per completion (1 prime + 6 ISRs)", g_kicks == 7);
    int distinct_ok = 1, alternating_ok = 1;
    for (int i = 0; i < g_kicks; i++) {
        if (g_kick_addr[i] != g_kick_addr[0] && g_kick_addr[i] != g_kick_addr[1]) {
            distinct_ok = 0;
        }
        if (i >= 1 && g_kick_addr[i] == g_kick_addr[i - 1]) {
            alternating_ok = 0;
        }
    }
    xpect(&c, "only two distinct PCM buffers are ever kicked", distinct_ok);
    xpect(&c, "consecutive kicks never reuse the same buffer", alternating_ok);
    xpect(&c, "buffers alternate A,B,A,B,...",
          g_kicks >= 4 && g_kick_addr[0] != g_kick_addr[1] &&
          g_kick_addr[2] == g_kick_addr[0] && g_kick_addr[3] == g_kick_addr[1]);
    xpect(&c, "the drained buffer is refilled after the other is kicked",
          g_calls == 8);   /* 2 priming pulls + one per completion */

    /* Continuity: step through completions and check, at each one, that the
     * audio the DMA was just pointed at continues exactly where the previous
     * chunk ended. A repeated chunk (same first sample twice) or a skipped one
     * (a jump of more than `frames`) is an audible glitch. */
    fresh_start();
    int continuity_ok = (last_kick_first_sample() == sample_for(0));
    for (uint32_t n = 1; n <= 5; n++) {
        audio_dma_isr();
        if (last_kick_first_sample() != sample_for(n * (uint32_t)frames)) {
            continuity_ok = 0;
        }
    }
    xpect(&c, "no audio repeated or dropped across completions", continuity_ok);

    /* --- 4. a short source read zero-pads EXACTLY the tail ------------- */
    hal_audio_close();                /* cold, so start() really primes       */
    bus_ready();
    hal_audio_init(44100u, 2u);
    source_reset();
    bus_ready();
    hal_audio_set_source(counting_source, 0);
    g_short_after  = 0;               /* short-change the first priming pull */
    g_short_frames = 17;              /* deliberately odd and tiny            */
    hal_audio_start();
    xpect(&c, "short pull was taken", g_calls >= 1 && g_call_got[0] == 17);
    {
        const int16_t *b = g_call_buf[0];
        int head_ok = 1, tail_ok = 1;
        /*
         * The source's frames are NOT left bit-identical any more: an underrun
         * used to zero-splice mid-waveform, which is an instantaneous jump to
         * silence — a click, not a graceful dropout. The driver now fades the
         * last AUDIO_TAIL_RAMP_FRAMES of real audio to zero before padding.
         * With only 17 frames delivered the ramp covers all of them, so what we
         * can assert is that each frame is a monotonically shrinking, correctly
         * signed attenuation of what the source wrote — and that the last one
         * has reached silence, so the pad it runs into is continuous.
         */
        for (int f = 0; f < 17; f++) {
            int32_t want = sample_for((uint32_t)f);
            int32_t got  = b[2 * f];
            if (b[2 * f + 1] != got) {
                head_ok = 0;                     /* both channels ramp alike */
            }
            if (want >= 0 ? (got < 0 || got > want) : (got > 0 || got < want)) {
                head_ok = 0;                     /* attenuated, never amplified
                                                  * and never sign-flipped     */
            }
        }
        if (b[2 * 16] != 0 || b[2 * 16 + 1] != 0) {
            head_ok = 0;                         /* ramp reaches true silence */
        }
        for (int f = 17; f < frames; f++) {
            if (b[2 * f] != 0 || b[2 * f + 1] != 0) {
                tail_ok = 0;
            }
        }
        xpect(&c, "short read: the source's frames are ramped down, not spliced",
              head_ok);
        xpect(&c, "short read: every frame past the tail is silence", tail_ok);
        xpect(&c, "short read: the chunk length handed to the DMA is unchanged",
              (g_kick_bytes[0] & DMA_CMD_SIZE_MASK) + DMA_SIZE_BIAS ==
                  (uint32_t)frames * 4u);
    }

    /* A source that reports an error must yield a fully silent buffer, not
     * whatever the previous track left in it. */
    hal_audio_close();                /* cold, so start() really primes       */
    bus_ready();
    hal_audio_init(44100u, 2u);
    source_reset();
    bus_ready();
    hal_audio_set_source(counting_source, 0);
    g_return_neg = 1;
    hal_audio_start();
    {
        const int16_t *b = g_call_buf[0];
        int all_zero = (g_calls >= 1);   /* a stop/start pair primes nothing:
                                          * without a pull there is no buffer
                                          * to inspect, and reading g_call_buf[0]
                                          * would be a wild pointer. */
        for (int i = 0; i < frames * 2; i++) {
            if (b[i] != 0) {
                all_zero = 0;
            }
        }
        xpect(&c, "failed pull yields silence, not stale PCM", all_zero);
    }

    /* --- 5. stop() then start() must not skip audio -------------------- *
     * hal.h: "hal_audio_stop pauses output. The internal buffer is not
     * cleared — a subsequent hal_audio_start resumes from where we left off."
     * The player's pause/resume is exactly this pair, so anything pulled from
     * the ring and never played is audio dropped on every un-pause. */
    fresh_start();
    audio_dma_isr();
    audio_dma_isr();
    /*
     * Three chunks have been handed out, so the DMA is part-way through the one
     * starting at 2*frames — NOT finished with it. "Resumes where we left off"
     * therefore means picking up INSIDE that chunk, not moving on to the next
     * one: jumping to 3*frames is precisely the bug, because the unplayed
     * remainder of chunk 2 was pulled from the ring and never heard.
     *
     * The mock's USEC_TIMER is a constant, so no time passes across the
     * stop/start and the resume point is the chunk's own start.
     */
    int32_t before = last_kick_first_sample();
    xpect(&c, "pre-stop position is where we think it is",
          before == sample_for(2u * (uint32_t)frames));

    hal_audio_stop();
    bus_ready();
    hal_audio_start();
    collect_kicks();
    xpect(&c, "stop() then start() resumes kicking the DMA", g_kicks >= 1);
    xpect(&c, "stop/start resumes where it left off (drops no audio)",
          last_kick_first_sample() == sample_for(2u * (uint32_t)frames));
    xpect(&c, "stop/start does not skip to the next chunk",
          last_kick_first_sample() != sample_for(3u * (uint32_t)frames));

    /* --- 6. a cleared source is silence, not a crash ------------------- */
    hal_audio_stop();
    hal_audio_set_source(0, 0);
    source_reset();
    bus_ready();
    hal_audio_start();
    audio_dma_isr();
    collect_kicks();
    xpect(&c, "NULL source still clocks buffers (silence, not a stall)",
          g_kicks == 2);
    xpect(&c, "NULL source is never called", g_calls == 0);

    /* --- 7. after stop, a stray completion must not kick the DMA ------- */
    hal_audio_set_source(counting_source, 0);
    fresh_start();
    hal_audio_stop();
    bus_ready();
    audio_dma_isr();
    collect_kicks();
    xpect(&c, "an ISR arriving after stop() does not restart playback",
          g_kicks == 0);
    xpect(&c, "an ISR arriving after stop() does not pull the source",
          g_calls == 2);   /* only the two priming pulls from fresh_start */

    /* --- 8. every refill is followed by a cache flush ------------------ *
     * The DMA reads the buffer's native SDRAM alias while the CPU writes it
     * through a write-back cache; a missed flush streams stale PCM. */
    unsigned commits_before = audio_test_cache_commits();
    fresh_start();
    audio_dma_isr();
    xpect(&c, "each buffer fill is followed by a cache flush",
          audio_test_cache_commits() - commits_before == (unsigned)g_calls);

    /* --- 9. suspend/wake keeps the resume position; close does not ---- *
     * A pause that persists has the codec powered down (audio.h,
     * hal_audio_suspend). The requirement on the way back is the same one
     * case 5 puts on a plain stop/start: resume INSIDE the chunk the DMA was
     * part-way through, dropping nothing. hal_audio_close() cannot deliver
     * that — it severs the buffers, so a close/init/start comes back two
     * chunks AHEAD — which is exactly why suspend exists and why the player
     * must not reach for close on a pause. Both halves are asserted so that
     * the contrast is on record, not just the happy path. */
    fresh_start();
    audio_dma_isr();
    audio_dma_isr();                  /* part-way through the chunk at 2*frames */
    hal_audio_stop();
    bus_ready();
    hal_audio_suspend();
    unsigned restores_before = audio_test_codec_restores();
    bus_ready();
    xpect(&c, "wake after suspend reports the codec came up",
          hal_audio_wake() == 0);
    xpect(&c, "wake re-latches the user's codec state (it is a reset)",
          audio_test_codec_restores() == restores_before + 1);
    int calls_before_start = g_calls;
    bus_ready();
    hal_audio_start();
    collect_kicks();
    xpect(&c, "suspend/wake/start kicks the DMA again", g_kicks >= 1);
    xpect(&c, "suspend/wake/start resumes exactly where the pause left off",
          last_kick_first_sample() == sample_for(2u * (uint32_t)frames));
    xpect(&c, "suspend/wake/start pulls nothing new from the source",
          g_calls == calls_before_start);

    /* The contrast: close discards what suspend keeps. */
    fresh_start();
    audio_dma_isr();
    audio_dma_isr();
    hal_audio_stop();
    hal_audio_close();
    bus_ready();
    hal_audio_init(44100u, 2u);
    hal_audio_set_source(counting_source, 0);
    bus_ready();
    hal_audio_start();
    xpect(&c, "close/init/start comes back AHEAD of the pause point (why "
              "suspend exists)",
          last_kick_first_sample() == sample_for(4u * (uint32_t)frames));

    /* Suspend refuses to act under a running DMA: nothing is powered down,
     * and the completion path is untouched. */
    fresh_start();
    restores_before = audio_test_codec_restores();
    hal_audio_suspend();              /* running: must be refused */
    xpect(&c, "wake after a refused suspend is a no-op",
          hal_audio_wake() == 0 &&
          audio_test_codec_restores() == restores_before);
    audio_dma_isr();
    collect_kicks();
    xpect(&c, "a suspend issued while running does not disturb playback",
          g_kicks == 2 && last_kick_first_sample() == sample_for((uint32_t)frames));

    /* Suspend, then close: the power-down runs once, and close still severs
     * the buffers — the UI closes on the active->inactive edge and a stop that
     * follows a suspended pause must not double-walk the codec sequence. */
    hal_audio_stop();
    hal_audio_suspend();
    restores_before = audio_test_codec_restores();
    hal_audio_close();
    bus_ready();
    hal_audio_init(44100u, 2u);
    xpect(&c, "close after suspend still brings the next init up as a reset",
          audio_test_codec_restores() == restores_before + 1);
    source_reset();
    bus_ready();
    hal_audio_set_source(counting_source, 0);
    hal_audio_start();
    xpect(&c, "close after suspend severed the buffers: start primes cold",
          g_calls == 2 && last_kick_first_sample() == sample_for(0));

    /* --- 10. a completion dropped in the stop window is not replayed --- *
     * hal_audio_stop() clears g_running, THEN masks the IRQ, THEN stops the
     * engine. A completion landing in that window (or swallowed by the
     * engine's status read) reaches audio_dma_isr with g_running == 0: acked
     * and dropped, no refill. The buffer it announced IS finished, so the
     * stop samples it as done and resume moves to the other (fresh) one —
     * and the completion after that kicks the dropped buffer again, still
     * holding what the listener already heard: ~186 ms replayed. */
    fresh_start();                    /* kick A=chunk0; B=chunk1             */
    audio_dma_isr();                  /* kick B=chunk1; A refilled = chunk2  */
    xpect(&c, "dropped: the DAC is on the chunk at 1*frames",
          last_kick_first_sample() == sample_for((uint32_t)frames));
    /* B finishes. Its completion lands after the stop cleared g_running. */
    mmio_mock_set_read(USEC_TIMER_ADDR, 1000000u);   /* well past 186 ms */
    hal_audio_stop();
    audio_dma_isr();                  /* the late completion: dropped        */
    int calls_before_resume = g_calls;
    bus_ready();
    hal_audio_start();
    collect_kicks();
    xpect(&c, "dropped: resume moves on to the fresh buffer (chunk 2)",
          g_kicks == 1 && last_kick_first_sample() ==
              sample_for(2u * (uint32_t)frames));
    xpect(&c, "dropped: resume refills the finished buffer the ISR never did",
          g_calls == calls_before_resume + 1);
    xpect(&c, "dropped: the buffer whose completion was lost still counts as "
              "played", hal_audio_frames_played() == 2u * (uint32_t)frames);
    audio_dma_isr();
    xpect(&c, "dropped: the next completion plays chunk 3, not chunk 1 again",
          last_kick_first_sample() == sample_for(3u * (uint32_t)frames));
    xpect(&c, "dropped: no chunk was skipped either",
          g_calls == calls_before_resume + 2);

    /* And the ordinary pause — no completion lost — still refills nothing on
     * resume: the on-demand refill must not turn into an unconditional one,
     * or every unpause would skip a buffer. */
    fresh_start();
    audio_dma_isr();
    hal_audio_stop();                 /* mid-buffer, nothing dropped         */
    calls_before_resume = g_calls;
    bus_ready();
    hal_audio_start();
    xpect(&c, "not dropped: a plain pause/resume pulls nothing new",
          g_calls == calls_before_resume);
    xpect(&c, "not dropped: and resumes inside the chunk it was on",
          last_kick_first_sample() == sample_for((uint32_t)frames));

    /* --- 11. hal_audio_frames_played(): where the DAC actually is ------ *
     * The player used to guess this by subtracting the driver's whole depth
     * from what it had handed out. hal.h now promises the real thing:
     * completed buffers plus the timed part of the current one, frozen
     * across a stop, unmoved by a flush, zeroed by init, counted at the
     * stream's rate. The mock's USEC_TIMER is set by hand, so every figure
     * here is exact: 44.1 frames per millisecond. */
    fresh_start();                               /* kicked at t = 0 */
    xpect(&c, "played: zero at the instant of the first kick",
          hal_audio_frames_played() == 0u);
    at_us(100000u);                              /* 100 ms in */
    xpect(&c, "played: 100 ms into a buffer at 44.1 kHz is 4410 frames",
          hal_audio_frames_played() == 4410u);
    at_us(5000000u);                             /* a completion that never came */
    xpect(&c, "played: a stalled buffer is credited at most its own length",
          hal_audio_frames_played() == (uint32_t)frames);
    at_us(185760u);                              /* the buffer's real end */
    audio_dma_isr();
    xpect(&c, "played: a completion retires the whole buffer and the next "
              "kick starts at zero",
          hal_audio_frames_played() == (uint32_t)frames);
    at_us(185760u + 50000u);
    xpect(&c, "played: ...and the next buffer counts from its own kick",
          hal_audio_frames_played() == (uint32_t)frames + 2205u);

    /* Pause: frozen where the stop sampled it, paused time never counted,
     * resume carrying on from the same figure. */
    hal_audio_stop();
    uint32_t at_stop = hal_audio_frames_played();
    xpect(&c, "played: a stop freezes the count where the engine was cut",
          at_stop == (uint32_t)frames + 2205u);
    at_us(185760u + 50000u + 30000000u);         /* 30 s paused */
    xpect(&c, "played: paused time is not played time",
          hal_audio_frames_played() == at_stop);
    hal_audio_stop();                            /* stop while stopped */
    xpect(&c, "played: stopping again while stopped changes nothing",
          hal_audio_frames_played() == at_stop);
    bus_ready();
    at_us(40000000u);
    hal_audio_start();                           /* resume, 40 s on the clock */
    xpect(&c, "played: resume carries on from the frozen count",
          hal_audio_frames_played() == at_stop);
    at_us(40000000u + 10000u);
    xpect(&c, "played: ...and runs again from the resume, not from the pause",
          hal_audio_frames_played() == at_stop + 441u);
    at_us(45000000u);                            /* past the remainder's end */
    audio_dma_isr();                             /* the resumed remainder completes */
    xpect(&c, "played: completing a resumed remainder retires exactly the "
              "remainder", hal_audio_frames_played() == 2u * (uint32_t)frames);

    /* Flush (a seek): the unheard PCM is discarded, the heard part of the
     * buffer is kept, and the cold start after it continues the count. */
    fresh_start();                               /* t = 0 */
    at_us(100000u);
    hal_audio_stop();                            /* 4410 frames in */
    hal_audio_flush();
    xpect(&c, "played: a flush discards PCM, not the count of what was heard",
          hal_audio_frames_played() == 4410u);
    hal_audio_flush();
    xpect(&c, "played: a second flush adds nothing",
          hal_audio_frames_played() == 4410u);
    bus_ready();
    at_us(200000u);
    hal_audio_start();                           /* cold prime from the source */
    xpect(&c, "played: a cold start after a flush continues the count",
          hal_audio_frames_played() == 4410u);
    at_us(210000u);
    xpect(&c, "played: ...timed from the new kick",
          hal_audio_frames_played() == 4410u + 441u);
    at_us(400000u);
    audio_dma_isr();
    xpect(&c, "played: the first completion after a flush retires one whole "
              "buffer on top", hal_audio_frames_played() == 4410u + (uint32_t)frames);

    /* The timer wraps under a kick: the in-flight part is a 32-bit
     * difference, not a comparison. */
    hal_audio_close();
    bus_ready();
    hal_audio_init(44100u, 2u);
    source_reset();
    bus_ready();
    hal_audio_set_source(counting_source, 0);
    at_us(0xFFFFFF00u);                          /* 256 us before the wrap */
    hal_audio_start();
    at_us(0x00000100u);                          /* 256 us after it: 512 us in */
    xpect(&c, "played: the in-flight part survives the timer wrapping (512 us "
              "= 22 frames)", hal_audio_frames_played() == 22u);

    /* Init is a new stream, at that stream's rate. */
    hal_audio_close();
    bus_ready();
    xpect(&c, "played: init starts a new stream at zero",
          hal_audio_init(48000u, 2u) == 0 && hal_audio_frames_played() == 0u);
    source_reset();
    bus_ready();
    hal_audio_set_source(counting_source, 0);
    hal_audio_start();
    at_us(100000u);
    xpect(&c, "played: counts at the stream's own rate (48 kHz: 4800 in 100 ms)",
          hal_audio_frames_played() == 4800u);

    /* --- bus order at every transition -----------------------------------
     * The noise on Play (device, 2026-09-22). The DAC was unmuted BEFORE the
     * DMA was kicked — over a TX FIFO that had been empty since the stop —
     * so the soft-unmute ramp played the serializer's underrun output, on a
     * cold start for the length of two buffer fills. The stop cut the DMA
     * the instant the mute write was issued (before the transaction was even
     * on the wire, and long before the ramp ended), and both writes flipped
     * the DAC's oversampling bit along with the mute. These pin the order:
     * kick, THEN unmute; mute (OSR kept), THEN stop; and a warm re-init
     * powers the codec down before it resets it. */
    {
        fresh_start();                            /* log holds only the start */
        long kick   = log_first_dma_cmd(1);
        long unmute = log_first_codec_write(CW_DACCTRL_UNMUTE_B0,
                                            CW_DACCTRL_UNMUTE_B1);
        xpect(&c, "order: a cold start kicks the DMA and unmutes the DAC",
              kick >= 0 && unmute >= 0);
        xpect(&c, "order: cold start — the kick comes BEFORE the unmute",
              kick >= 0 && unmute >= 0 && kick < unmute);
        xpect(&c, "order: cold start — nothing ever drops the DAC to 64x OSR",
              log_first_codec_write(CW_DACCTRL_UNMUTE_B0,
                                    CW_DACCTRL_OSR64_B1) < 0);

        bus_ready();
        hal_audio_stop();
        long mute = log_first_codec_write(CW_DACCTRL_UNMUTE_B0,
                                          CW_DACCTRL_MUTE_B1);
        long stop = log_first_dma_cmd(0);
        xpect(&c, "order: a stop soft-mutes the DAC and stops the DMA",
              mute >= 0 && stop >= 0);
        xpect(&c, "order: stop — the mute comes BEFORE the DMA cut",
              mute >= 0 && stop >= 0 && mute < stop);
        xpect(&c, "order: stop — the mute keeps DACOSR128 set",
              log_first_codec_write(CW_DACCTRL_UNMUTE_B0,
                                    CW_DACCTRL_OSR64_B1) < 0);
        xpect(&c, "order: stop — the mute waits out the ramp (reads the timer)",
              mmio_mock_count(MMIO_OP_READ, USEC_TIMER_ADDR) > 1);

        bus_ready();
        hal_audio_stop();                         /* stop while stopped */
        xpect(&c, "order: a stop while stopped touches the codec not at all",
              mmio_mock_count(MMIO_OP_WRITE, I2C_ADDR_ADDR) == 0);

        bus_ready();
        hal_audio_start();                        /* resume */
        kick   = log_first_dma_cmd(1);
        unmute = log_first_codec_write(CW_DACCTRL_UNMUTE_B0,
                                       CW_DACCTRL_UNMUTE_B1);
        xpect(&c, "order: resume — the kick comes BEFORE the unmute",
              kick >= 0 && unmute >= 0 && kick < unmute);

        /* A re-init on a WARM codec (Next/Prev, a track started from a row,
         * a rate change) does NOT reset it — the noise on every skip
         * (device, 2026-09-22) was the WM_RESET + VMID cycle this used to
         * run per track, and since this morning a power-down in front of
         * it. At the same rate it writes NOTHING to the codec; at a new rate
         * it re-programs the PLL under the mute with PLLEN toggled. Never a
         * reset, never the rails, never a VMID discharge, and the DAC is
         * unmuted only by the start that follows, after its kick. */
        hal_audio_stop();
        bus_ready();
        xpect(&c, "order: warm re-init succeeds", hal_audio_init(44100u, 2u) == 0);
        xpect(&c, "order: warm re-init at the same rate writes NOTHING to the codec",
              mmio_mock_count(MMIO_OP_WRITE, I2C_ADDR_ADDR) == 0);
        xpect(&c, "order: warm re-init — no WM_RESET, no rails off, no VMID discharge",
              log_first_codec_write(CW_RESET_B0, 0x00) < 0 &&
              log_first_codec_write(CW_PWRMGMT1_OFF_B0, 0x00) < 0 &&
              log_first_codec_write(CW_OUT4TOADC_B0, CW_OUT4TOADC_DRAIN_B1) < 0);
        xpect(&c, "order: warm re-init still resets the I2S FIFO and re-arms the DMA",
              log_first_write(IISCONFIG_ADDR, IIS_RESET) >= 0 &&
              mmio_mock_count(MMIO_OP_WRITE, DMA0_PER_ADDR_ADDR) == 1);
        source_reset();
        bus_ready();
        hal_audio_set_source(counting_source, 0);
        hal_audio_start();
        kick   = log_first_dma_cmd(1);
        unmute = log_first_codec_write(CW_DACCTRL_UNMUTE_B0,
                                       CW_DACCTRL_UNMUTE_B1);
        xpect(&c, "order: the start after a warm re-init kicks BEFORE it unmutes",
              kick >= 0 && unmute >= 0 && kick < unmute);

        /* A warm re-init at a NEW rate (a 48 kHz album after a 44.1 kHz
         * one): PLLEN off, the PLL re-programmed, PLLEN on, and only then
         * the I2S FIFO reset. Still no reset, no rails, no unmute. */
        hal_audio_stop();
        bus_ready();
        xpect(&c, "order: warm re-init at 48 kHz succeeds", hal_audio_init(48000u, 2u) == 0);
        long plloff = log_first_codec_write(CW_PWRMGMT1_OFF_B0, CW_PWRMGMT1_PLLOFF_B1);
        long plln   = log_first_codec_write(CW_PLLN_B0, CW_PLLN_48_B1);
        long pllon  = log_first_codec_write(CW_PWRMGMT1_OFF_B0, CW_PWRMGMT1_RUN_B1);
        long fifo   = log_first_write(IISCONFIG_ADDR, IIS_RESET);
        xpect(&c, "order: rate change — PLLEN off, PLL re-programmed, PLLEN on, in that order",
              plloff >= 0 && plln >= 0 && pllon >= 0 && plloff < plln && plln < pllon);
        xpect(&c, "order: rate change — the I2S FIFO reset comes AFTER the PLL is back",
              fifo >= 0 && fifo > pllon);
        xpect(&c, "order: rate change — no WM_RESET, no rails off, no VMID discharge",
              log_first_codec_write(CW_RESET_B0, 0x00) < 0 &&
              log_first_codec_write(CW_PWRMGMT1_OFF_B0, 0x00) < 0 &&
              log_first_codec_write(CW_OUT4TOADC_B0, CW_OUT4TOADC_DRAIN_B1) < 0);
        xpect(&c, "order: rate change — the DAC is never unmuted inside the re-init",
              log_first_codec_write(CW_DACCTRL_UNMUTE_B0,
                                    CW_DACCTRL_UNMUTE_B1) < 0);

        /* A re-init issued over a RUNNING stream (no caller does it, and
         * the discipline must not depend on that): the stop happens first —
         * mute, DMA cut — and only then the retune. */
        source_reset();
        bus_ready();
        hal_audio_set_source(counting_source, 0);
        hal_audio_start();
        bus_ready();
        xpect(&c, "order: re-init over a running stream succeeds",
              hal_audio_init(44100u, 2u) == 0);
        long mute2 = log_first_codec_write(CW_DACCTRL_UNMUTE_B0, CW_DACCTRL_MUTE_B1);
        long stop2 = log_first_dma_cmd(0);
        plloff = log_first_codec_write(CW_PWRMGMT1_OFF_B0, CW_PWRMGMT1_PLLOFF_B1);
        xpect(&c, "order: re-init over a running stream — mute, DMA cut, THEN the retune",
              mute2 >= 0 && stop2 >= 0 && plloff >= 0 &&
              mute2 < stop2 && stop2 < plloff);

        /* A re-init on a COLD codec (after close / suspend / boot_quiet):
         * the rails are already off, so the first codec write is the reset,
         * and the close's power-down drained VMID before it dropped them. */
        bus_ready();
        hal_audio_close();
        long drain = log_first_codec_write(CW_OUT4TOADC_B0, CW_OUT4TOADC_DRAIN_B1);
        long off   = log_first_codec_write(CW_PWRMGMT1_OFF_B0, 0x00);
        xpect(&c, "order: close — VMID discharge begun, the drain waited out, THEN rails off",
              drain >= 0 && off >= 0 && drain < off &&
              mmio_mock_count(MMIO_OP_READ, USEC_TIMER_ADDR) > 1);
        bus_ready();
        xpect(&c, "order: cold re-init succeeds", hal_audio_init(44100u, 2u) == 0);
        long reset  = log_first_codec_write(CW_RESET_B0, 0x00);
        long pd_off = log_first_codec_write(CW_PWRMGMT1_OFF_B0, 0x00);
        xpect(&c, "order: cold re-init — WM_RESET is the first codec write, "
                  "no power-down against cold rails",
              reset >= 0 && (pd_off < 0 || pd_off > reset) &&
              log_first_codec_write(CW_DACCTRL_UNMUTE_B0,
                                    CW_DACCTRL_MUTE_B1) > reset);
        xpect(&c, "order: cold re-init — POBCTRL off is the last codec write, after the DAC mute",
              log_first_codec_write(CW_OUT4TOADC_B0, 0x00) >
              log_first_codec_write(CW_DACCTRL_UNMUTE_B0, CW_DACCTRL_MUTE_B1));
    }

    /* --- 13. audio_fifo_service(): starvation INSIDE a transfer -------- *
     * The third failure, invisible to `underruns` (needs a short read) and
     * `late` (needs a late completion): the DMA mid-transfer loses the bus
     * and the TX FIFO drains. The tick samples IISFIFO_CFG.TX_FREE while
     * running; 16 free = empty = the DAC is being fed nothing right now. */
    {
        fresh_start();
        /* A healthy stream: the FIFO sits at the request level, 0..4 free. */
        mmio_mock_set_read(IISFIFO_CFG_ADDR, 3u << IISFIFO_CFG_TXFREE_SHIFT);
        audio_fifo_service();
        audio_fifo_service();
        xpect(&c, "fifo: a fed FIFO counts nothing",
              audio_fifo_empty_samples() == 0 && audio_fifo_worst_free() == 3u);
        /* Starved: every slot free. */
        mmio_mock_set_read(IISFIFO_CFG_ADDR, 16u << IISFIFO_CFG_TXFREE_SHIFT);
        audio_fifo_service();
        audio_fifo_service();
        audio_fifo_service();
        xpect(&c, "fifo: an empty FIFO is counted once per sample",
              audio_fifo_empty_samples() == 3 && audio_fifo_worst_free() == 16u);
        /* Low but not empty: worst tracks it, the empty count does not move. */
        mmio_mock_set_read(IISFIFO_CFG_ADDR, 12u << IISFIFO_CFG_TXFREE_SHIFT);
        audio_fifo_service();
        xpect(&c, "fifo: 12 free is low, not empty", audio_fifo_empty_samples() == 3);
        /* Stopped: the FIFO is empty by design and must not be counted. */
        hal_audio_stop();
        mmio_mock_set_read(IISFIFO_CFG_ADDR, 16u << IISFIFO_CFG_TXFREE_SHIFT);
        size_t reads_before = mmio_mock_count(MMIO_OP_READ, IISFIFO_CFG_ADDR);
        audio_fifo_service();
        xpect(&c, "fifo: nothing is sampled while stopped",
              audio_fifo_empty_samples() == 3 &&
              mmio_mock_count(MMIO_OP_READ, IISFIFO_CFG_ADDR) == reads_before);
        /* A new stream starts its count from zero. */
        bus_ready();
        hal_audio_init(44100u, 2u);
        xpect(&c, "fifo: init zeroes the counters",
              audio_fifo_empty_samples() == 0 && audio_fifo_worst_free() == 0);
    }

    return xfail_done(&c);
}
