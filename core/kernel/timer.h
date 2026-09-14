/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kernel/timer.h — TIMER1 system tick (PP5022).
 *
 * Freestanding ARM7TDMI. No libc; <stdint.h> only.
 *
 * TIMER1 is armed as a self-reloading periodic source at HZ ticks per
 * second (core/docs/hw/01-soc-pp5022.md, "Timers and the system tick").
 * Its IRQ (#0) is serviced by timer_tick_isr() via the irq_dispatch()
 * fan-out; the ISR bumps a monotonic tick counter and acknowledges the
 * timer. sleep_ms() is a cooperative delay that yields to the scheduler
 * until enough ticks have elapsed.
 */
#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H

#include <stdint.h>

/* System tick rate, ticks per second (10 ms period). */
#define HZ 100

/* Arm TIMER1 at HZ and unmask TIMER1_IRQ in the interrupt controller. */
void timer_init(void);

/*
 * Re-arm TIMER1 at `hz` interrupts per second (1..TIMER_FREQ; anything else
 * means HZ). The tick COUNTER keeps its HZ unit at any rate — the ISR
 * reconciles against USEC_TIMER — so current_tick()/sleep_ms() stay
 * correct, just coarser; the per-interrupt work (wheel sampling) runs at
 * the new rate. Used by suspend_to_ram: 10 Hz asleep, HZ awake.
 */
void timer_set_rate(uint32_t hz);

/* Tick ISR: advance the tick counter and ack the timer (read TIMER1_VAL). */
void timer_tick_isr(void);

/* Current value of the monotonic tick counter. */
uint32_t current_tick(void);

/* Cooperative delay: yield to the scheduler until >= ms have elapsed. */
void sleep_ms(uint32_t ms);

#endif /* KERNEL_TIMER_H */
