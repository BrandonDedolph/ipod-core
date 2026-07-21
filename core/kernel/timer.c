/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kernel/timer.c — TIMER1 100 Hz system tick driver (PP5022).
 *
 * Implements the arm/ack sequence from core/docs/hw/01-soc-pp5022.md
 * ("Timers and the system tick"). The 1 MHz timer clock makes one count
 * one microsecond, so a HZ-hertz tick has a reload of (TIMER_FREQ/HZ)-1
 * microseconds; bit 30 makes the counter self-reload, so the ISR only
 * has to acknowledge (a read of TIMER1_VAL) and never reprograms CFG.
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only,
 * hardware reached solely through hal/hw/mmio.h so the register grammar
 * is trace-testable host-side under -DMMIO_MOCK.
 */

#include "timer.h"
#include "sched.h"
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/clickwheel.h"

/* Monotonic tick counter. In .bss, zeroed by crt0. volatile: written by
 * the ISR, read by mainline code (current_tick / sleep_ms). */
static volatile uint32_t g_tick;

void timer_init(void)
{
    /* 1. Disarm: clear any stale enable/reload. */
    mmio_write32(TIMER1_CFG_ADDR, 0);
    /* 2. Clear a pending IRQ latched from a prior arm. */
    (void)mmio_read32(TIMER1_VAL_ADDR);
    /* 3. Arm periodic: enable + IRQ/reload + (TIMER_FREQ/HZ)-1 us period. */
    mmio_write32(TIMER1_CFG_ADDR,
                 TIMER_CFG_ENABLE | TIMER_CFG_IRQEN | ((TIMER_FREQ / HZ) - 1u));
    /* 4. Unmask TIMER1_IRQ (#0) in the CPU interrupt-enable register. */
    mmio_write32(CPU_INT_EN_ADDR, 1u << TIMER1_IRQ);
}

void timer_tick_isr(void)
{
    g_tick++;
    (void)mmio_read32(TIMER1_VAL_ADDR);   /* ack: clears the pending IRQ */

    /* Sample the click wheel every tick (10 ms). Running this from the tick
     * — not the main loop — is what keeps a quick face-button tap from being
     * lost when the main loop is blocked in a ~100 ms synchronous disk read:
     * the tick still fires, so clickwheel_service() latches the press for the
     * loop to drain later. Bounded work (one OPTO packet); a no-op until
     * clickwheel_init() arms it. */
    clickwheel_service();
}

uint32_t current_tick(void)
{
    return g_tick;
}

void sleep_ms(uint32_t ms)
{
    /* PRECONDITION: IRQs must be enabled at the core — the wait ends only
     * when the timer ISR advances the tick, so calling this inside an
     * arch_irq_disable() critical section spins forever. Cooperative:
     * other tasks run while it waits.
     *
     * Round up to whole ticks (ceil); the 64-bit intermediate keeps a
     * large ms from overflowing the multiply. */
    uint32_t ticks = (uint32_t)(((uint64_t)ms * HZ + 999u) / 1000u);
    uint32_t start = current_tick();

    /* Unsigned subtraction is wrap-safe across the UINT32_MAX boundary. */
    while ((current_tick() - start) < ticks) {
        sched_yield();
    }
}

#ifdef MMIO_MOCK
/*
 * Host-test-only hook. Seeds the tick counter so a test can exercise
 * paths (e.g. the unsigned wrap in sleep_ms) that would otherwise
 * require billions of real ticks to reach. Compiled out of the
 * freestanding firmware image entirely — the arm-none-eabi build never
 * defines MMIO_MOCK, so g_tick stays a pure ISR-owned counter there.
 */
void timer_test_set_tick(uint32_t v)
{
    g_tick = v;
}
#endif
