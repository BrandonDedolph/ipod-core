/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_timer/timer_test_stubs.c — scheduler stub for the host timer
 * test.
 *
 * timer.c calls sched_yield() from sleep_ms(). On real hardware a yield
 * eventually returns after the tick ISR has advanced the clock; here we
 * model that directly: each yield advances the mocked tick by pumping
 * timer_tick_isr() once, so a sleeping caller makes progress and
 * sleep_ms() terminates. We also count the yields so the test can assert
 * the exact ceil()-tick math.
 *
 * Only sched_yield is referenced by timer.c, so it is the only symbol
 * the mock build needs to satisfy.
 */

#include "sched.h"
#include "timer.h"

/* Number of sched_yield() calls since the test last reset it. */
int sched_yield_calls;

void sched_yield(void)
{
    sched_yield_calls++;
    timer_tick_isr();   /* advance the mocked tick, as a real yield would */
}
