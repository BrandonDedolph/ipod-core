/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_timer/timer_test.c — host-side tests for the TIMER1 tick
 * driver (kernel/timer.c) and the IRQ dispatcher (kernel/irq.c),
 * compiled against the recording mock bus (-DMMIO_MOCK), mirroring the
 * uart/lcd trace tests.
 *
 * Proves:
 *   1. timer_init's exact register grammar (disarm -> clear -> arm -> unmask).
 *   2. timer_tick_isr advances the tick and acks (reads TIMER1_VAL).
 *   3. irq_dispatch fans TIMER1's pending bit to the ISR, and does
 *      nothing when the bit is clear.
 *   4. sleep_ms's ceil()-tick math, including the unsigned wrap.
 */

#include "pp5022.h"
#include "timer.h"
#include "irq.h"
#include "mmio_mock.h"
#include "trace_expect.h"

/* From timer.c, host-test-only (MMIO_MOCK-guarded). */
extern void timer_test_set_tick(uint32_t v);
/* From timer_test_stubs.c. */
extern int sched_yield_calls;

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

/* Case 1: timer_init emits exactly the documented arm sequence. */
static int test_timer_init(void)
{
    mmio_mock_reset();

    timer_init();

    trace_cursor tc = trace_begin("timer_init");
    expect_w(&tc, 32, TIMER1_CFG_ADDR, 0);            /* 1. disarm         */
    expect_r(&tc, 32, TIMER1_VAL_ADDR);               /* 2. clear stale    */
    expect_w(&tc, 32, TIMER1_CFG_ADDR, 0xC000270F);   /* 3. arm 100 Hz     */
    expect_w(&tc, 32, CPU_INT_EN_ADDR, 1u << TIMER1_IRQ); /* 4. unmask     */
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Case 2: the ISR advances the tick and acks by reading TIMER1_VAL. */
static int test_tick_isr(void)
{
    int fails = 0;
    mmio_mock_reset();
    timer_test_set_tick(0);

    timer_tick_isr();
    fails += check("tick_isr: first tick",
                   current_tick() == 1 &&
                   mmio_mock_count(MMIO_OP_READ, TIMER1_VAL_ADDR) == 1);

    timer_tick_isr();
    fails += check("tick_isr: second tick",
                   current_tick() == 2 &&
                   mmio_mock_count(MMIO_OP_READ, TIMER1_VAL_ADDR) == 2);
    return fails;
}

/* Case 3: irq_dispatch dispatches on TIMER1's pending bit, and only then. */
static int test_irq_dispatch(void)
{
    int fails = 0;

    /* Pending bit set -> exactly one tick + one ack. */
    mmio_mock_reset();
    timer_test_set_tick(0);
    mmio_mock_set_read(CPU_INT_STAT_ADDR, 1u << TIMER1_IRQ);
    irq_dispatch();
    fails += check("irq_dispatch: pending -> tick",
                   current_tick() == 1 &&
                   mmio_mock_count(MMIO_OP_READ, TIMER1_VAL_ADDR) == 1);

    /* Pending bit clear -> no tick, no ack. */
    mmio_mock_reset();
    timer_test_set_tick(7);
    mmio_mock_set_read(CPU_INT_STAT_ADDR, 0);
    irq_dispatch();
    fails += check("irq_dispatch: idle -> no tick",
                   current_tick() == 7 &&
                   mmio_mock_count(MMIO_OP_READ, TIMER1_VAL_ADDR) == 0);

    /* Unhandled source asserted (not TIMER1) -> no tick, and the
     * straggler is masked at CPU_INT_DIS so it can't livelock the core. */
    mmio_mock_reset();
    timer_test_set_tick(3);
    mmio_mock_set_read(CPU_INT_STAT_ADDR, 1u << 5);   /* some unwired source */
    irq_dispatch();
    fails += check("irq_dispatch: unhandled -> masked, no tick",
                   current_tick() == 3 &&
                   mmio_mock_count(MMIO_OP_WRITE, CPU_INT_DIS_ADDR) == 1);
    return fails;
}

/* Case 4: sleep_ms rounds up to whole ticks and is wrap-safe. The
 * sched_yield stub advances one tick per yield, so the yield count is
 * the number of ticks slept. */
static int test_sleep_ms(void)
{
    int fails = 0;

    /* 0 ms -> 0 ticks -> no yields. */
    mmio_mock_reset();
    timer_test_set_tick(0);
    sched_yield_calls = 0;
    sleep_ms(0);
    fails += check("sleep_ms(0): 0 yields", sched_yield_calls == 0);

    /* 10 ms -> ceil(10*100/1000) = 1 tick. */
    mmio_mock_reset();
    timer_test_set_tick(0);
    sched_yield_calls = 0;
    sleep_ms(10);
    fails += check("sleep_ms(10): 1 yield", sched_yield_calls == 1);

    /* 25 ms -> ceil(2500/1000) = 3 ticks. */
    mmio_mock_reset();
    timer_test_set_tick(0);
    sched_yield_calls = 0;
    sleep_ms(25);
    fails += check("sleep_ms(25): 3 yields", sched_yield_calls == 3);

    /* Wrap: start near UINT32_MAX; the unsigned diff still measures 3
     * ticks and the loop terminates rather than spinning forever. */
    mmio_mock_reset();
    timer_test_set_tick(0xFFFFFFFFu);
    sched_yield_calls = 0;
    sleep_ms(25);
    fails += check("sleep_ms(25) wrap: 3 yields, terminates",
                   sched_yield_calls == 3 && current_tick() == 2u);
    return fails;
}

int main(void)
{
    int fails = 0;
    fails += test_timer_init();
    fails += test_tick_isr();
    fails += test_irq_dispatch();
    fails += test_sleep_ms();

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
