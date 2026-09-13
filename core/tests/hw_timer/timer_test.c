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

/*
 * Case 5: no single halt sleep_ms programs may outlast the audio DMA
 * deadline.
 *
 * sleep_ms is reachable mid-track (player.c's disk-retry backoff), and
 * playback is a single-shot DMA re-kicked from its completion ISR with
 * only the 16-frame I2S FIFO — ~363 us at 44.1 kHz — covering a late
 * re-kick (kernel/main.c, cpu_wait_us). The doc describes PROC_WAIT_CNT
 * as "Sleep until countdown" and does NOT promise an early wake on an
 * interrupt, so the halt length IS the worst-case ISR latency. This
 * decodes every CPU_CTL write the sleep emits — unit bit and 8-bit
 * count — and checks it against the deadline; it FAILS on the previous
 * PROC_CNT_MSEC | 1 (1000 us) halt. The count of halts is asserted too,
 * so the check cannot pass vacuously on an empty log.
 */
#define AUDIO_DMA_DEADLINE_US  363u

/* Duration in microseconds a PROC_WAIT_CNT word halts for; UINT32_MAX for
 * a word this test does not understand (no unit bit, or PROC_SLEEP —
 * which would never wake without an interrupt controller). */
static uint32_t halt_word_us(uint32_t v)
{
    uint32_t count = v & PROC_CNT_MASK;
    if (!(v & PROC_WAIT_CNT) || (v & PROC_SLEEP)) {
        return 0xFFFFFFFFu;
    }
    if (v & PROC_CNT_USEC) {
        return count;
    }
    if (v & PROC_CNT_MSEC) {
        return count * 1000u;
    }
    return 0xFFFFFFFFu;
}

static int test_sleep_halt_under_audio_deadline(void)
{
    int fails = 0;

    mmio_mock_reset();
    timer_test_set_tick(0);
    sched_yield_calls = 0;
    sleep_ms(25);                               /* 3 ticks -> 3 halts */

    const mmio_event *log = mmio_mock_log();
    size_t len   = mmio_mock_log_len();
    size_t halts = 0;
    uint32_t longest = 0;
    for (size_t i = 0; i < len; i++) {
        if (log[i].op != MMIO_OP_WRITE || log[i].addr != CPU_CTL_ADDR) {
            continue;
        }
        halts++;
        uint32_t us = halt_word_us(log[i].value);
        if (us > longest) {
            longest = us;
        }
    }

    fails += check("sleep halt: one halt per loop trip (non-vacuous)",
                   halts == 3 && halts == (size_t)sched_yield_calls);
    printf("[sleep halt] longest programmed halt = %u us (deadline %u us)\n",
           (unsigned)longest, (unsigned)AUDIO_DMA_DEADLINE_US);
    fails += check("sleep halt: every halt inside the audio DMA deadline",
                   longest <= AUDIO_DMA_DEADLINE_US);
    fails += check("sleep halt: total sleep unchanged (still 3 ticks)",
                   current_tick() == 3u);
    return fails;
}

/*
 * Case 6: timer_set_rate — the suspend loop's slow tick.
 *
 *   a. timer_set_rate(10) emits the same four-access arm grammar as
 *      timer_init with a 100 ms reload (99 999), and timer_set_rate(HZ)
 *      puts the 9 999 reload back.
 *   b. At the slow rate the tick COUNTER still advances in 10 ms units:
 *      an ISR that finds 100 ms on USEC_TIMER adds ten, not one, and the
 *      backlight's per-10-ms service runs ten times for it.
 *   c. Out-of-range rates fall back to HZ rather than programming a zero
 *      or a wrapped period.
 */
extern int backlight_service_calls;

static int test_set_rate(void)
{
    int fails = 0;

    /* a. grammar, slow then fast */
    mmio_mock_reset();
    timer_set_rate(10);
    trace_cursor tc = trace_begin("timer_set_rate(10)");
    expect_w(&tc, 32, TIMER1_CFG_ADDR, 0);
    expect_r(&tc, 32, TIMER1_VAL_ADDR);
    expect_w(&tc, 32, TIMER1_CFG_ADDR,
             TIMER_CFG_ENABLE | TIMER_CFG_IRQEN | (100000u - 1u));
    expect_w(&tc, 32, CPU_INT_EN_ADDR, 1u << TIMER1_IRQ);
    trace_expect_end(&tc);
    fails += trace_done(&tc);

    mmio_mock_reset();
    timer_set_rate(HZ);
    tc = trace_begin("timer_set_rate(HZ)");
    expect_w(&tc, 32, TIMER1_CFG_ADDR, 0);
    expect_r(&tc, 32, TIMER1_VAL_ADDR);
    expect_w(&tc, 32, TIMER1_CFG_ADDR,
             TIMER_CFG_ENABLE | TIMER_CFG_IRQEN | (10000u - 1u));
    expect_w(&tc, 32, CPU_INT_EN_ADDR, 1u << TIMER1_IRQ);
    trace_expect_end(&tc);
    fails += trace_done(&tc);

    /* b. the counter keeps its unit: seed at t=0, then one interrupt at
     * t=100 ms is ten ticks and ten backlight services. */
    mmio_mock_reset();
    timer_test_set_tick(0);
    const uint32_t us[] = { 0, 100000u, 200000u };
    mmio_mock_queue_read(USEC_TIMER_ADDR, us, 3);
    timer_tick_isr();                            /* seeds: +1 */
    backlight_service_calls = 0;
    timer_tick_isr();                            /* 100 ms later: +10 */
    fails += check("slow rate: one ISR at +100 ms advances 10 ticks",
                   current_tick() == 11u);
    fails += check("slow rate: backlight_service ran once per 10 ms tick",
                   backlight_service_calls == 10);
    timer_tick_isr();                            /* another 100 ms: +10 */
    fails += check("slow rate: no drift across a second interrupt",
                   current_tick() == 21u && backlight_service_calls == 20);

    /* c. bad rates fall back to HZ */
    mmio_mock_reset();
    timer_set_rate(0);
    fails += check("timer_set_rate(0) arms at HZ",
                   mmio_mock_log()[2].value ==
                       (TIMER_CFG_ENABLE | TIMER_CFG_IRQEN | (10000u - 1u)));
    mmio_mock_reset();
    timer_set_rate(TIMER_FREQ + 1u);
    fails += check("timer_set_rate(>1 MHz) arms at HZ",
                   mmio_mock_log()[2].value ==
                       (TIMER_CFG_ENABLE | TIMER_CFG_IRQEN | (10000u - 1u)));
    return fails;
}

int main(void)
{
    int fails = 0;
    fails += test_timer_init();
    fails += test_tick_isr();
    fails += test_irq_dispatch();
    fails += test_sleep_ms();
    fails += test_sleep_halt_under_audio_deadline();
    fails += test_set_rate();

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
