/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/dma.c — PP502x DMA channel 0, RAM -> I2S TX FIFO.
 *
 * Implements core/docs/hw/05-audio.md ("DMA engine"). The DMA master
 * control/status/request registers live at 0x6000A000; the channel-0
 * register file at 0x6000B000. Channel 0 is the audio playback channel.
 *
 * The peripheral (destination) address is the I2S TX FIFO's MMIO port,
 * which is NOT remapped, so it is used as-is. Only the RAM (source)
 * address is a memory address the caller must present in a DMA-visible
 * form (see hal/hw/audio.c buf_phys()).
 */

#include "pp5022.h"
#include "mmio.h"
#include "dma.h"

/* Bounded stop-wait so a wedged channel can't hang teardown. */
#define DMA_STOP_SPIN_LIMIT (1u << 16)

void dma_playback_init(void)
{
    /* Force the DMA interrupt (source 26) to normal IRQ priority, not FIQ.
     * Our kernel handles this line through irq_dispatch (an IRQ); the FIQ
     * vector just traps-and-hangs. Rockbox routes DMA at FIQ, so a
     * chainloader or a non-zero reset default could leave bit 26 set for
     * FIQ — clearing it here guarantees the completion vectors to the IRQ
     * path we actually implement. (Setting a bit = FIQ; clearing = IRQ.) */
    mmio_write32(CPU_INT_PRIORITY_ADDR,
                 mmio_read32(CPU_INT_PRIORITY_ADDR) & ~DMA_MASK);

    /* Enable the whole DMA controller (master), then the IIS peripheral
     * request line for the channel. */
    mmio_write32(DMA_MASTER_CONTROL_ADDR,
                 mmio_read32(DMA_MASTER_CONTROL_ADDR) | DMA_MASTER_CONTROL_EN);
    mmio_write32(DMA_REQ_STATUS_ADDR,
                 mmio_read32(DMA_REQ_STATUS_ADDR) | (1u << DMA_REQ_IIS));

    /* Channel-0 static config: destination = I2S TX FIFO port (address
     * fixed, not incremented), 32-bit accesses; the misc flags word
     * Rockbox sets unconditionally for playback. */
    mmio_write32(DMA0_PER_ADDR_ADDR, IISFIFO_WR_ADDR);
    mmio_write32(DMA0_FLAGS_ADDR,    DMA_FLAGS_PLAY);
    mmio_write32(DMA0_INCR_ADDR,     DMA_INCR_PLAY);

    /* Clear any latched completion (a bare status read). */
    (void)mmio_read32(DMA0_STATUS_ADDR);
}

void dma_playback_kick(uint32_t phys, uint32_t bytes)
{
    mmio_write32(DMA0_RAM_ADDR_ADDR, phys);
    /* SIZE field is (bytes - 4); OR in START to launch. The config word
     * carries direction/req-id/single/wait-req/intr (05-audio.md). */
    mmio_write32(DMA0_CMD_ADDR,
                 DMA_PLAY_CONFIG
                     | ((bytes - DMA_SIZE_BIAS) & DMA_CMD_SIZE_MASK)
                     | DMA_CMD_START);
}

void dma_playback_ack(void)
{
    /* Reading the channel status register clears the pending interrupt. */
    (void)mmio_read32(DMA0_STATUS_ADDR);
}

void dma_playback_stop(void)
{
    mmio_write32(DMA0_CMD_ADDR,
                 mmio_read32(DMA0_CMD_ADDR)
                     & ~(DMA_CMD_START | DMA_CMD_INTR));

    uint32_t spin = DMA_STOP_SPIN_LIMIT;
    while ((mmio_read32(DMA0_STATUS_ADDR)
            & (DMA0_STATUS_BUSY | DMA0_STATUS_INTR))
           && --spin != 0) {
        /* wait for the channel to go idle */
    }
}
