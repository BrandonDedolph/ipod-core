/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/dma.h — PP502x DMA channel 0: RAM -> I2S TX FIFO playback.
 *
 * The low-level register ops for the continuous-playback path (the polled
 * i2s_write_stereo path needs none of this). SINGLE-shot transfers: each
 * kick plays one chunk and raises DMA_IRQ (source 26) on completion; the
 * caller re-kicks the next chunk from the ISR. See core/docs/hw/05-audio.md
 * ("DMA engine"). Asm-free for host trace tests.
 */
#ifndef CORE_HAL_HW_DMA_H
#define CORE_HAL_HW_DMA_H

#include <stdint.h>

/* Enable the DMA controller + channel-0 static config (peripheral dest =
 * I2S TX FIFO, fixed-address 32-bit transfers, IIS request line). Clears any
 * latched completion. Idempotent. */
void dma_playback_init(void);

/* Kick one RAM->FIFO transfer. `phys` is the PHYSICAL RAM address of `bytes`
 * bytes of packed 32-bit stereo frames (`bytes` must be >= 4 and a multiple
 * of 4, and fit the 16-bit count as bytes-4). Completion raises DMA_IRQ. */
void dma_playback_kick(uint32_t phys, uint32_t bytes);

/* Acknowledge/clear the channel's pending IRQ (a bare status read). Call as
 * the first action in the completion ISR. */
void dma_playback_ack(void);

/* Stop the channel: clear START/INTR, then spin (bounded) until it is idle. */
void dma_playback_stop(void);

#endif /* CORE_HAL_HW_DMA_H */
