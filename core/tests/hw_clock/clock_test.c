/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_clock/clock_test.c — host-side golden-trace tests for the
 * PP5022 clock / PLL / boost driver (kernel/clock.c), compiled against
 * the recording mock bus (-DMMIO_MOCK), mirroring the timer/uart trace
 * tests.
 *
 * Proves:
 *   1. clock_init emits the exact ordered PP5022 switch grammar (10
 *      events, no pre-stage write) and lands at CPUFREQ_NORMAL.
 *   2. The DEV_INIT2 PLL-power set is a read-modify-write that preserves
 *      the other bits.
 *   3. The PLL-lock poll is a BOUNDED spin: it completes (never hangs)
 *      when the lock bit never sets, then DEGRADES — it stays on the
 *      24 MHz crystal (reports CPUFREQ_DEFAULT) rather than routing onto
 *      the unlocked PLL; and it consumes exactly the reads it needs when
 *      the bit sets after a few polls.
 *   4. cpu_boost/cpu_unboost are refcounted: only the 0<->1 edges move
 *      the clock; nested requests emit zero bus traffic.
 *   5. clock_init is idempotent: a second call re-runs the full sequence
 *      and still ends at CPUFREQ_NORMAL.
 *   6. clock_suspend emits the exact park grammar (bus to crystal, slow
 *      timing, PLL disable, PLL unpower — 6 events, both RMWs preserving
 *      the other bits) and reports CPUFREQ_DEFAULT; a second call is
 *      silent.
 *   7. clock_suspend REFUSES (no traffic, -1) while a boost is held or the
 *      audio DMA is streaming.
 *   8. clock_resume re-emits the full 30 MHz bring-up and is a no-op when
 *      the tree is not parked; a cpu_boost issued while parked un-parks
 *      it, so a later clock_resume then does nothing.
 *
 * NOTE: clock.c uses a smaller PLL_LOCK_SPIN_LIMIT under MMIO_MOCK (see
 * clock.c) purely so the never-locks path fits inside the mock's
 * fixed-capacity event log; the firmware bound is 1<<20.
 */

#include "pp5022.h"
#include "clock.h"
#include "mmio_mock.h"
#include "trace_expect.h"

/* From clock.c, host-test-only (MMIO_MOCK-guarded). */
extern void clock_test_reset(void);

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

/* ---- log-scanning helpers -------------------------------------------- */

/* Index of the first write to `addr` carrying `value`, or (size_t)-1. */
static size_t find_write(uint32_t addr, uint32_t value)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == addr &&
            log[i].value == value) {
            return i;
        }
    }
    return (size_t)-1;
}

/* Count reads of `addr` among log events [0, upto). */
static size_t count_reads_before(uint32_t addr, size_t upto)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    if (upto > len) {
        upto = len;
    }
    size_t c = 0;
    for (size_t i = 0; i < upto; i++) {
        if (log[i].op == MMIO_OP_READ && log[i].addr == addr) {
            c++;
        }
    }
    return c;
}

/* ---- cases ----------------------------------------------------------- */

/* Case 1: clock_init emits exactly the PP5022 switch (10 events). The
 * PLL locks on the first poll, so exactly one PLL_STATUS read appears.
 * RMW sources read 0 so the |= results are the pure set bits. There is
 * NO pre-stage write; DEV_TIMING1 appears twice (relax at step 4, then
 * the target operating value at step 7 — SLOW for the 30 MHz target). */
static int test_clock_init_grammar(void)
{
    mmio_mock_reset();
    clock_test_reset();
    mmio_mock_set_read(DEV_INIT2_ADDR,   0);
    mmio_mock_set_read(PLL_CONTROL_ADDR, 0);
    mmio_mock_set_read(PLL_STATUS_ADDR,  PLL_STATUS_LOCK);   /* locked now */

    clock_init();

    int fails = 0;
    trace_cursor tc = trace_begin("clock_init");
    /* 1. power up PLL (RMW, source 0 -> just the power bit) */
    expect_r(&tc, 32, DEV_INIT2_ADDR);
    expect_w(&tc, 32, DEV_INIT2_ADDR, DEV_INIT2_PLL_POWER);
    /* 2. enable PLL (RMW, source 0 -> just the enable bits) */
    expect_r(&tc, 32, PLL_CONTROL_ADDR);
    expect_w(&tc, 32, PLL_CONTROL_ADDR, PLL_CONTROL_ENABLE);
    /* 3. bus onto crystal */
    expect_w(&tc, 32, CLOCK_SOURCE_ADDR, CLOCK_SOURCE_XTAL);
    /* 4. relax timing before reprogramming the PLL */
    expect_w(&tc, 32, DEV_TIMING1_ADDR, DEV_TIMING1_SLOW);
    /* 5. target: 30 MHz (single write, no prestage) */
    expect_w(&tc, 32, PLL_CONTROL_ADDR, PLL_CONTROL_30MHZ);
    /* 6. lock poll (one read, locked immediately) */
    expect_r(&tc, 32, PLL_STATUS_ADDR);
    /* 7. operating timing for the 30 MHz target (SLOW) */
    expect_w(&tc, 32, DEV_TIMING1_ADDR, DEV_TIMING1_SLOW);
    /* 8. bus onto PLL */
    expect_w(&tc, 32, CLOCK_SOURCE_ADDR, CLOCK_SOURCE_PLL);
    trace_expect_end(&tc);
    fails += trace_done(&tc);

    fails += check("clock_init: freq == CPUFREQ_NORMAL",
                   cpu_frequency() == CPUFREQ_NORMAL);
    return fails;
}

/* Case 2: the PLL-power set preserves the other DEV_INIT2 bits. With the
 * source reading all-ones, the write must be (0xFFFFFFFF | power). */
static int test_dev_init2_rmw(void)
{
    mmio_mock_reset();
    clock_test_reset();
    mmio_mock_set_read(DEV_INIT2_ADDR,  0xFFFFFFFF);
    mmio_mock_set_read(PLL_STATUS_ADDR, PLL_STATUS_LOCK);

    clock_init();

    size_t idx = find_write(DEV_INIT2_ADDR, 0xFFFFFFFF | DEV_INIT2_PLL_POWER);
    return check("dev_init2 RMW: write == (0xFFFFFFFF | PLL_POWER)",
                 idx != (size_t)-1);
}

/* Case 3a: the lock bit never sets -> bounded spin COMPLETES (no hang),
 * then DEGRADES: it must NOT route the core onto the unlocked PLL — it
 * stays on the 24 MHz crystal and reports CPUFREQ_DEFAULT. */
static int test_lock_spin_bounded(void)
{
    int fails = 0;
    mmio_mock_reset();
    clock_test_reset();
    /* PLL_STATUS unprogrammed -> reads 0 forever (never locks). */

    clock_init();

    /* Bounded: it polled PLL_STATUS many times and returned (no hang). */
    fails += check("lock spin: bounded (many PLL_STATUS polls, no hang)",
                   count_reads_before(PLL_STATUS_ADDR, mmio_mock_log_len()) > 1);
    /* Degrade: never route the bus onto the unlocked PLL... */
    fails += check("lock spin: does NOT route to the unlocked PLL",
                   find_write(CLOCK_SOURCE_ADDR, CLOCK_SOURCE_PLL) == (size_t)-1);
    /* ...it stays on the crystal (the XTAL route is the last one made)... */
    fails += check("lock spin: stays on the 24 MHz crystal",
                   find_write(CLOCK_SOURCE_ADDR, CLOCK_SOURCE_XTAL) != (size_t)-1);
    /* ...and reports the true, degraded frequency. */
    fails += check("lock spin: reports CPUFREQ_DEFAULT",
                   cpu_frequency() == CPUFREQ_DEFAULT);
    return fails;
}

/* Case 3b: not-ready x3 then ready -> exactly 4 PLL_STATUS reads must
 * precede the CLOCK_SOURCE=pll write. */
static int test_lock_spin_then_ready(void)
{
    mmio_mock_reset();
    clock_test_reset();
    const uint32_t seq[] = { 0, 0, 0, PLL_STATUS_LOCK };
    mmio_mock_queue_read(PLL_STATUS_ADDR, seq, 4);

    clock_init();

    size_t pll_route = find_write(CLOCK_SOURCE_ADDR, CLOCK_SOURCE_PLL);
    return check("lock spin: exactly 4 PLL_STATUS reads before route",
                 pll_route != (size_t)-1 &&
                 count_reads_before(PLL_STATUS_ADDR, pll_route) == 4);
}

/* Case 4: boost refcount edges. Only the 0<->1 transitions touch the
 * bus; nested boosts/unboosts are silent. */
static int test_boost_refcount(void)
{
    int fails = 0;
    mmio_mock_reset();
    clock_test_reset();
    mmio_mock_set_read(PLL_STATUS_ADDR, PLL_STATUS_LOCK);   /* locks fast */

    /* 0->1: raise to 80 MHz. */
    cpu_boost();
    fails += check("boost 0->1: emits 80MHz target",
                   find_write(PLL_CONTROL_ADDR, PLL_CONTROL_80MHZ)
                       != (size_t)-1);
    fails += check("boost 0->1: operating DEV_TIMING1 == FAST (0x0808)",
                   find_write(DEV_TIMING1_ADDR, DEV_TIMING1_FAST)
                       != (size_t)-1);
    fails += check("boost 0->1: freq == CPUFREQ_MAX",
                   cpu_frequency() == CPUFREQ_MAX);

    /* 1->2: nested boost is silent. */
    size_t len_after_boost = mmio_mock_log_len();
    cpu_boost();
    fails += check("boost 1->2: zero further bus traffic",
                   mmio_mock_log_len() == len_after_boost &&
                   cpu_frequency() == CPUFREQ_MAX);

    /* 2->1: still boosted, silent. */
    cpu_unboost();
    fails += check("unboost 2->1: zero further bus traffic",
                   mmio_mock_log_len() == len_after_boost &&
                   cpu_frequency() == CPUFREQ_MAX);

    /* 1->0: drop back to 30 MHz. */
    mmio_mock_reset();
    mmio_mock_set_read(PLL_STATUS_ADDR, PLL_STATUS_LOCK);
    cpu_unboost();
    fails += check("unboost 1->0: emits 30MHz target",
                   find_write(PLL_CONTROL_ADDR, PLL_CONTROL_30MHZ)
                       != (size_t)-1);
    fails += check("unboost 1->0: operating DEV_TIMING1 == SLOW (0x0303)",
                   find_write(DEV_TIMING1_ADDR, DEV_TIMING1_SLOW)
                       != (size_t)-1);
    fails += check("unboost 1->0: freq == CPUFREQ_NORMAL",
                   cpu_frequency() == CPUFREQ_NORMAL);
    return fails;
}

/* Case 5: clock_init is idempotent. A second call re-emits the full
 * 10-event sequence and stays at CPUFREQ_NORMAL. */
static int test_idempotent(void)
{
    int fails = 0;
    mmio_mock_reset();
    clock_test_reset();
    mmio_mock_set_read(PLL_STATUS_ADDR, PLL_STATUS_LOCK);

    clock_init();
    size_t first = mmio_mock_log_len();
    fails += check("idempotent: first clock_init emits full sequence",
                   first == 10 && cpu_frequency() == CPUFREQ_NORMAL);

    clock_init();
    fails += check("idempotent: second clock_init re-emits full sequence",
                   mmio_mock_log_len() == 2 * first &&
                   cpu_frequency() == CPUFREQ_NORMAL);
    return fails;
}

/* Bring the driver to the locked 30 MHz state with a clean log, so the
 * suspend cases start where suspend_to_ram does (after cpu_unboost). */
static void settle_at_30mhz(void)
{
    mmio_mock_reset();
    clock_test_reset();
    mmio_mock_set_read(PLL_STATUS_ADDR, PLL_STATUS_LOCK);
    clock_init();
    mmio_mock_reset();
    mmio_mock_set_read(PLL_STATUS_ADDR, PLL_STATUS_LOCK);
}

/* Case 6: clock_suspend grammar. PLL_CONTROL reads back the 30 MHz program
 * word and DEV_INIT2 reads PLL-power + the I2S pad-routing bits, so the two
 * clears must leave the multiplier field and the pad routing alone. */
static int test_suspend_grammar(void)
{
    int fails = 0;
    settle_at_30mhz();
    mmio_mock_set_read(PLL_CONTROL_ADDR, PLL_CONTROL_30MHZ);
    mmio_mock_set_read(DEV_INIT2_ADDR,
                       DEV_INIT2_PLL_POWER | DEV_INIT2_I2S_PADS);

    fails += check("suspend: returns 0", clock_suspend() == 0);

    trace_cursor tc = trace_begin("clock_suspend");
    /* 1. bus off the PLL, onto the crystal */
    expect_w(&tc, 32, CLOCK_SOURCE_ADDR, CLOCK_SOURCE_XTAL);
    /* 2. slow timing */
    expect_w(&tc, 32, DEV_TIMING1_ADDR, DEV_TIMING1_SLOW);
    /* 3. PLL disable: RMW, enable bits cleared, program word kept */
    expect_r(&tc, 32, PLL_CONTROL_ADDR);
    expect_w(&tc, 32, PLL_CONTROL_ADDR,
             PLL_CONTROL_30MHZ & ~PLL_CONTROL_ENABLE);
    /* 4. PLL unpower: RMW, power bit cleared, pad routing kept */
    expect_r(&tc, 32, DEV_INIT2_ADDR);
    expect_w(&tc, 32, DEV_INIT2_ADDR, DEV_INIT2_I2S_PADS);
    trace_expect_end(&tc);
    fails += trace_done(&tc);

    fails += check("suspend: freq == CPUFREQ_DEFAULT (crystal)",
                   cpu_frequency() == CPUFREQ_DEFAULT);

    /* Already parked: a second call is a silent success. */
    size_t len = mmio_mock_log_len();
    fails += check("suspend: second call is silent and returns 0",
                   clock_suspend() == 0 && mmio_mock_log_len() == len);
    return fails;
}

/* Case 7: refusals. A held boost or a streaming DMA means the caller has
 * not finished quiescing; the park must not happen underneath either. */
static int test_suspend_refusals(void)
{
    int fails = 0;

    settle_at_30mhz();
    cpu_boost();
    mmio_mock_reset();
    fails += check("suspend under boost: refused, no traffic, still 80 MHz",
                   clock_suspend() == -1 && mmio_mock_log_len() == 0 &&
                   cpu_frequency() == CPUFREQ_MAX);
    cpu_unboost();

    settle_at_30mhz();
    clock_set_audio_dma_active(1);
    fails += check("suspend under DMA: refused, no traffic, still 30 MHz",
                   clock_suspend() == -1 && mmio_mock_log_len() == 0 &&
                   cpu_frequency() == CPUFREQ_NORMAL);
    clock_set_audio_dma_active(0);
    return fails;
}

/* Case 8: clock_resume is the full 30 MHz bring-up (the same 10 events as
 * clock_init), a no-op when not parked, and a no-op after a cpu_boost has
 * already un-parked the tree. */
static int test_resume(void)
{
    int fails = 0;

    /* Not parked: silent. */
    settle_at_30mhz();
    clock_resume();
    fails += check("resume when not parked: no traffic",
                   mmio_mock_log_len() == 0 &&
                   cpu_frequency() == CPUFREQ_NORMAL);

    /* Parked -> resume: exactly the clock_init grammar. */
    settle_at_30mhz();
    (void)clock_suspend();
    mmio_mock_reset();
    mmio_mock_set_read(DEV_INIT2_ADDR,   0);
    mmio_mock_set_read(PLL_CONTROL_ADDR, 0);
    mmio_mock_set_read(PLL_STATUS_ADDR,  PLL_STATUS_LOCK);
    clock_resume();
    trace_cursor tc = trace_begin("clock_resume");
    expect_r(&tc, 32, DEV_INIT2_ADDR);
    expect_w(&tc, 32, DEV_INIT2_ADDR, DEV_INIT2_PLL_POWER);
    expect_r(&tc, 32, PLL_CONTROL_ADDR);
    expect_w(&tc, 32, PLL_CONTROL_ADDR, PLL_CONTROL_ENABLE);
    expect_w(&tc, 32, CLOCK_SOURCE_ADDR, CLOCK_SOURCE_XTAL);
    expect_w(&tc, 32, DEV_TIMING1_ADDR, DEV_TIMING1_SLOW);
    expect_w(&tc, 32, PLL_CONTROL_ADDR, PLL_CONTROL_30MHZ);
    expect_r(&tc, 32, PLL_STATUS_ADDR);
    expect_w(&tc, 32, DEV_TIMING1_ADDR, DEV_TIMING1_SLOW);
    expect_w(&tc, 32, CLOCK_SOURCE_ADDR, CLOCK_SOURCE_PLL);
    trace_expect_end(&tc);
    fails += trace_done(&tc);
    fails += check("resume: freq == CPUFREQ_NORMAL",
                   cpu_frequency() == CPUFREQ_NORMAL);

    /* Parked, then a boost (an ATA transfer's clock hold, say): the boost
     * runs the full 80 MHz switch from the crystal, the unboost drops to
     * 30 MHz, and the tree is no longer parked — resume must not re-run. */
    settle_at_30mhz();
    (void)clock_suspend();
    mmio_mock_reset();
    mmio_mock_set_read(PLL_STATUS_ADDR, PLL_STATUS_LOCK);
    cpu_boost();
    fails += check("boost while parked: full 80 MHz switch, freq == MAX",
                   find_write(PLL_CONTROL_ADDR, PLL_CONTROL_80MHZ)
                       != (size_t)-1 &&
                   find_write(DEV_INIT2_ADDR, DEV_INIT2_PLL_POWER)
                       != (size_t)-1 &&
                   cpu_frequency() == CPUFREQ_MAX);
    cpu_unboost();
    fails += check("unboost after that: back at 30 MHz",
                   cpu_frequency() == CPUFREQ_NORMAL);
    size_t len = mmio_mock_log_len();
    clock_resume();
    fails += check("resume after a boost un-parked it: no traffic",
                   mmio_mock_log_len() == len);
    return fails;
}

int main(void)
{
    int fails = 0;
    fails += test_clock_init_grammar();
    fails += test_dev_init2_rmw();
    fails += test_lock_spin_bounded();
    fails += test_lock_spin_then_ready();
    fails += test_boost_refcount();
    fails += test_idempotent();
    fails += test_suspend_grammar();
    fails += test_suspend_refusals();
    fails += test_resume();

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
