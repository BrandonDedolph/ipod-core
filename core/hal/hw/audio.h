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

#endif /* CORE_HAL_HW_AUDIO_H */
