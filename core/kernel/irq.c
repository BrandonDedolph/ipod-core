/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kernel/irq.c — top-level interrupt dispatcher.
 *
 * Called from the assembly IRQ vector (boot/crt0.S irq_vector_entry)
 * with IRQs masked at the core. Reads the PP5022 low-bank interrupt
 * status (core/docs/hw/01-soc-pp5022.md, "Interrupt controller") and
 * dispatches each asserted source to its handler.
 *
 * Only TIMER1 (the system tick, IRQ #0) is wired today; further sources
 * chain onto the same status read as they come online.
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only,
 * hardware reached solely through hal/hw/mmio.h so the dispatch grammar
 * is trace-testable host-side under -DMMIO_MOCK.
 */

#include "irq.h"
#include "timer.h"
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/audio.h"

void irq_dispatch(void)
{
    uint32_t pending = mmio_read32(CPU_INT_STAT_ADDR);
    uint32_t handled = 0;

    if (pending & (1u << TIMER1_IRQ)) {
        timer_tick_isr();
        handled |= (1u << TIMER1_IRQ);
    }
    if (pending & (1u << DMA_IRQ)) {
        audio_dma_isr();
        handled |= (1u << DMA_IRQ);
    }
    /* future sources chain here, each OR-ing its bit into `handled` */

    /* Any asserted source we didn't handle would keep the IRQ line high
     * and re-enter us the instant CPSR unmasks on return — a hard
     * livelock. Mask the stragglers at the controller so a spurious IRQ,
     * or a source enabled before its handler exists, can't wedge the
     * core. */
    uint32_t unhandled = pending & ~handled;
    if (unhandled) {
        mmio_write32(CPU_INT_DIS_ADDR, unhandled);
    }
}
