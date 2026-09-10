/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/audio.h — hw-specific audio glue.
 *
 * The portable playback contract (hal_audio_init / set_source / start /
 * stop / close) lives in hal/hal.h; core/hal/hw/audio.c implements it for
 * the device using i2c + wm8758 + i2s + dma. This header exposes only the
 * two hooks that are hw-specific: the DMA-completion ISR (called from the
 * kernel interrupt dispatcher) and a bring-up completion counter.
 */
#ifndef CORE_HAL_HW_AUDIO_H
#define CORE_HAL_HW_AUDIO_H

#include <stdint.h>

/* DMA-completion interrupt handler (DMA_IRQ / interrupt source 26).
 * Called from kernel/irq.c irq_dispatch when the channel-0 transfer
 * finishes: acks the IRQ, kicks the next buffer, refills the drained one. */
void audio_dma_isr(void);

/* Number of DMA chunks that have completed since the last
 * hal_audio_start() — a bring-up diagnostic (nonzero proves the DMA
 * completion IRQ path is live). */
uint32_t audio_dma_completions(void);

/*
 * Number of buffer refills that came up SHORT since hal_audio_init(), i.e.
 * how many times the source was starved and the HAL had to ramp the tail to
 * silence. Starvation used to be completely invisible — the zero-fill looked
 * exactly like normal operation — so this is the only handle on "is the disk
 * pump keeping up?". Monotonic within a stream; reset by hal_audio_init.
 */
uint32_t audio_underruns(void);

/*
 * Play out the PCM already sitting in the two ping-pong buffers, then return.
 *
 * The player advances on ring-empty, but at that instant up to two buffers
 * (~370 ms) of decoded audio have not been clocked to the DAC yet — and
 * hal_audio_stop() discards them, cutting the last fraction of a second off
 * every track. Call this BEFORE hal_audio_stop() at end-of-track to hear the
 * whole thing.
 *
 * Bounded by wall clock. Returns 0 when the in-flight buffers have retired,
 * -1 if `timeout_ms` elapsed first (a stalled DMA must not hang the caller).
 * Requires IRQs enabled at the core. A no-op returning 0 when not playing.
 */
int hal_audio_drain(uint32_t timeout_ms);

/*
 * Completions serviced too late to be covered by the I2S FIFO, and the worst
 * overshoot seen, since hal_audio_init().
 *
 * This is the OTHER way audio breaks, and until it was counted the firmware
 * was blind to it. audio_underruns() sees decode starvation (the ring came up
 * short). These see the ring being full and the CPU arriving late anyway —
 * which is what a long interrupt-masked region does, and what a listener
 * hears as a tick while the screen is busy. Nonzero here means something is
 * holding IRQs past ~363 us; the pixel stream in lcd.c is the usual suspect.
 */
uint32_t audio_late_kicks(void);
uint32_t audio_late_worst_us(void);

/*
 * Power the codec down across a PAUSE without forgetting where the pause was.
 *
 * hal_audio_stop() is the pause: it mutes and cuts the DMA, and nothing else.
 * The WM8758's PLL, BIAS, VMID, DACs, mixers and headphone amps stay live, the
 * I2S block keeps clocking and the codec MCLK (DEV_EXTCLOCKS) stays ungated —
 * the full analog budget, for as long as the listener leaves the device paused.
 * Until this existed the only power-down was hal_audio_close(), which the UI
 * runs on stop and at the end of the queue, never on pause, so a device paused
 * in a pocket drew roughly what a playing one did.
 *
 * hal_audio_close() is the wrong tool for a pause, and that is the whole
 * reason this pair exists: close severs the ping-pong buffers from the stream
 * (g_primed = 0), and those two buffers hold up to ~370 ms of PCM that was
 * already pulled from the ring and never heard. A close/init/start on unpause
 * would cold-prime from the ring and the listener would come back ~a third of
 * a second AHEAD of where they paused — a position jump on every long pause,
 * which is a worse bug than the power draw.
 *
 * hal_audio_suspend() runs the same pop-suppressed codec power-down and gates
 * the same clocks as close, but leaves the buffers, the mid-buffer resume
 * offset and the source registration exactly as hal_audio_stop() left them.
 * It refuses while the engine is running (a suspend is only meaningful over a
 * stop) and is a no-op when the codec is already cold.
 *
 * hal_audio_wake() is the inverse: it re-runs the codec bring-up (a WM_RESET,
 * so the user's volume/balance/tone are re-latched through the restore hook,
 * exactly as per-track hal_audio_init does), re-ungates the clocks and re-arms
 * the DMA block, and touches none of the buffer state. A following
 * hal_audio_start() then resumes INTO the retained buffers, at the retained
 * offset, precisely as an un-suspended unpause would. Returns 0, or -2 when
 * the codec did not answer on I2C (same signal as hal_audio_init). A no-op
 * returning 0 when the codec is not cold.
 *
 * The pair costs a codec reset plus the VMID settle (~40 ms) on the way back,
 * which is why the player only reaches for it once a pause has PERSISTED — a
 * quick pause/unpause never pays it.
 */
void hal_audio_suspend(void);
int  hal_audio_wake(void);

#endif /* CORE_HAL_HW_AUDIO_H */
