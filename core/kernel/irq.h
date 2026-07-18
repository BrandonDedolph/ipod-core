/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kernel/irq.h — top-level IRQ dispatcher + core IRQ-mask primitives.
 *
 * Freestanding ARM7TDMI (PP5022). No libc; <stdint.h> only.
 *
 * irq_dispatch() is the C entry point called from the assembly IRQ
 * vector (boot/crt0.S irq_vector_entry): it reads the interrupt
 * controller's pending status and fans out to per-source handlers.
 *
 * arch_irq_enable/disable gate IRQs at the core via the CPSR I-bit.
 * ARMv4T (ARM7TDMI) predates the cpsie/cpsid mnemonics, so the mask is
 * flipped with the classic mrs/bic|orr/msr read-modify-write.
 */
#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include "hw/pp5022.h"   /* CPSR_I_BIT */

/* Called from the asm IRQ entry (crt0.S irq_vector_entry). */
void irq_dispatch(void);

/* Unmask IRQs at the core: clear the CPSR I-bit. */
static inline void arch_irq_enable(void)
{
    uint32_t tmp;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "bic %0, %0, %1\n\t"
        "msr cpsr_c, %0\n\t"
        : "=&r"(tmp)
        : "i"(CPSR_I_BIT)   /* 0x80 */
        : "cc", "memory");
}

/* Mask IRQs at the core: set the CPSR I-bit. */
static inline void arch_irq_disable(void)
{
    uint32_t tmp;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "orr %0, %0, %1\n\t"
        "msr cpsr_c, %0\n\t"
        : "=&r"(tmp)
        : "i"(CPSR_I_BIT)   /* 0x80 */
        : "cc", "memory");
}

#endif /* KERNEL_IRQ_H */
