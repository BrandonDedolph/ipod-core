/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_clock/gate_test.c — host-side golden-trace tests for the
 * suspend-time DEV_EN gating (kernel/clock.c clock_gate_suspend/resume and
 * the per-driver gates in uart.c, piezo.c, i2c.c), compiled against the
 * recording mock bus (-DMMIO_MOCK).
 *
 * Proves:
 *   1. clock_gate_suspend clears exactly DEV_SER0, DEV_PWM, DEV_I2C — three
 *      masked read-modify-writes, one bit each, nothing else in DEV_EN
 *      touched (DEV_OPTO and the ROM's bits survive).
 *   2. clock_gate_resume restores them in reverse order, and ONLY the bits
 *      the suspend found set: a PWM block that was never enabled emits a
 *      read and no write on suspend, and nothing on resume.
 *   3. The blocks are self-restoring: the first i2c_send / uart_putc /
 *      piezo_click_ex after a suspend re-gates its own clock before its
 *      normal grammar, and the second does not. So no transaction can run
 *      against an unclocked block, and the existing i2c/uart golden traces
 *      (which never suspend) are unchanged.
 *   4. i2c_init while gated re-gates before its idle drain.
 */

#include "pp5022.h"
#include "clock.h"
#include "uart.h"
#include "piezo.h"
#include "i2c.h"
#include "mmio_mock.h"
#include "trace_expect.h"

extern void i2c_test_reset(void);

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

#define ALL_THREE  (DEV_SER0 | DEV_PWM | DEV_I2C)
/* What a booted device has on besides the three: the wheel, and a stand-in
 * for the ROM's undocumented boot bits. Must come through untouched. */
#define OTHERS     (DEV_OPTO | 0xC2000124u)

/* Case 1+2: suspend then resume, all three on. The mock serves a CONSTANT
 * DEV_EN, so each RMW's write is (constant minus / plus its own bit) — which
 * is exactly the "one bit per write, nothing else" property under test. */
static int test_gate_all_on(void)
{
    int fails = 0;
    mmio_mock_reset();
    i2c_test_reset();
    mmio_mock_set_read(DEV_EN_ADDR, OTHERS | ALL_THREE);

    clock_gate_suspend();

    trace_cursor tc = trace_begin("clock_gate_suspend");
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, (OTHERS | ALL_THREE) & ~DEV_SER0);
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, (OTHERS | ALL_THREE) & ~DEV_PWM);
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, (OTHERS | ALL_THREE) & ~DEV_I2C);
    trace_expect_end(&tc);
    fails += trace_done(&tc);

    /* Resume: now the bits read clear; each write sets its own bit only. */
    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR, OTHERS);
    clock_gate_resume();
    tc = trace_begin("clock_gate_resume");
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, OTHERS | DEV_I2C);
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, OTHERS | DEV_PWM);
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, OTHERS | DEV_SER0);
    trace_expect_end(&tc);
    fails += trace_done(&tc);

    /* A second resume has nothing left to restore. */
    mmio_mock_reset();
    clock_gate_resume();
    fails += check("gate: second resume is silent", mmio_mock_log_len() == 0);
    return fails;
}

/* Case 2b: PWM never enabled (no piezo_init). Suspend reads it and leaves
 * it; resume does not invent it. */
static int test_gate_pwm_was_off(void)
{
    int fails = 0;
    mmio_mock_reset();
    i2c_test_reset();
    /* Only clock_gate_resume's flags matter here; a fresh process has none
     * set for uart/piezo, and i2c_test_reset cleared i2c's. But the prior
     * case left uart/piezo restored, so their flags are clear too. */
    mmio_mock_set_read(DEV_EN_ADDR, OTHERS | DEV_SER0 | DEV_I2C);

    clock_gate_suspend();
    trace_cursor tc = trace_begin("gate_suspend_pwm_off");
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, (OTHERS | DEV_I2C));        /* SER0 off */
    expect_r(&tc, 32, DEV_EN_ADDR);                             /* PWM: look only */
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, (OTHERS | DEV_SER0));        /* I2C off  */
    trace_expect_end(&tc);
    fails += trace_done(&tc);

    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR, OTHERS);
    clock_gate_resume();
    fails += check("gate resume: PWM not restored (was never on)",
                   mmio_mock_count(MMIO_OP_WRITE, DEV_EN_ADDR) == 2 &&
                   mmio_mock_log_len() == 4);
    return fails;
}

/* Case 3a: i2c_send after a suspend re-gates first, once. */
static int test_i2c_self_restore(void)
{
    int fails = 0;
    mmio_mock_reset();
    i2c_test_reset();
    mmio_mock_set_read(DEV_EN_ADDR, ALL_THREE);
    clock_gate_suspend();

    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR, 0);
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);      /* idle */
    mmio_mock_set_read(I2C_CTRL_ADDR, 0);
    const uint8_t b[1] = { 0x2F };
    fails += check("i2c_send while gated: succeeds",
                   i2c_send(0x08, b, 1) == 0);

    trace_cursor tc = trace_begin("i2c_send_regates");
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_I2C);                    /* re-gate  */
    expect_r(&tc, 8,  I2C_STATUS_ADDR);                         /* then the */
    expect_w(&tc, 8,  I2C_ADDR_ADDR, 0x10);                     /* ordinary */
    expect_r(&tc, 8,  I2C_CTRL_ADDR);                           /* grammar  */
    expect_w(&tc, 8,  I2C_CTRL_ADDR, 0x00);
    expect_w(&tc, 8,  I2C_DATA_ADDR(0), 0x2F);
    expect_r(&tc, 8,  I2C_CTRL_ADDR);
    expect_w(&tc, 8,  I2C_CTRL_ADDR, 0x00);
    expect_r(&tc, 8,  I2C_CTRL_ADDR);
    expect_w(&tc, 8,  I2C_CTRL_ADDR, I2C_SEND);
    trace_expect_end(&tc);
    fails += trace_done(&tc);

    mmio_mock_reset();
    (void)i2c_send(0x08, b, 1);
    fails += check("second i2c_send: no DEV_EN traffic",
                   mmio_mock_count(MMIO_OP_READ, DEV_EN_ADDR) == 0 &&
                   mmio_mock_count(MMIO_OP_WRITE, DEV_EN_ADDR) == 0);

    /* Resume then has only uart + pwm left to restore. */
    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR, DEV_I2C);
    clock_gate_resume();
    fails += check("gate resume after i2c self-restore: 2 writes, no I2C",
                   mmio_mock_count(MMIO_OP_WRITE, DEV_EN_ADDR) == 2 &&
                   mmio_mock_log_len() == 4);
    return fails;
}

/* Case 3b: uart_putc after a suspend re-gates first, once. */
static int test_uart_self_restore(void)
{
    int fails = 0;
    mmio_mock_reset();
    i2c_test_reset();
    mmio_mock_set_read(DEV_EN_ADDR, ALL_THREE);
    clock_gate_suspend();

    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR, 0);
    mmio_mock_set_read(SER0_LSR_ADDR, SER0_LSR_THRE);
    uart_putc('x');
    trace_cursor tc = trace_begin("uart_putc_regates");
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_SER0);
    expect_r(&tc, 32, SER0_LSR_ADDR);
    expect_w(&tc, 32, SER0_THR_ADDR, 'x');
    trace_expect_end(&tc);
    fails += trace_done(&tc);

    mmio_mock_reset();
    uart_putc('y');
    fails += check("second uart_putc: no DEV_EN traffic",
                   mmio_mock_count(MMIO_OP_READ, DEV_EN_ADDR) == 0 &&
                   mmio_mock_count(MMIO_OP_WRITE, DEV_EN_ADDR) == 0);
    clock_gate_resume();
    return fails;
}

/* Case 3c: piezo_click_ex after a suspend re-gates first. */
static int test_piezo_self_restore(void)
{
    int fails = 0;
    mmio_mock_reset();
    i2c_test_reset();
    mmio_mock_set_read(DEV_EN_ADDR, ALL_THREE);
    clock_gate_suspend();

    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR, 0);
    /* The burst is timed on USEC_TIMER: t0 = 0, then "5000 us later". */
    const uint32_t us[] = { 0, 5000 };
    mmio_mock_queue_read(USEC_TIMER_ADDR, us, 2);
    piezo_click_ex(3000, 4000);
    fails += check("piezo click while gated: re-gates DEV_PWM first",
                   mmio_mock_log_len() >= 2 &&
                   mmio_mock_log()[0].op == MMIO_OP_READ &&
                   mmio_mock_log()[0].addr == DEV_EN_ADDR &&
                   mmio_mock_log()[1].op == MMIO_OP_WRITE &&
                   mmio_mock_log()[1].addr == DEV_EN_ADDR &&
                   mmio_mock_log()[1].value == DEV_PWM);
    clock_gate_resume();
    return fails;
}

/* Case 4: i2c_init while gated (a codec wake, say) re-gates before it
 * drains — the drain reads I2C_STATUS, which needs a clocked block. */
static int test_i2c_init_while_gated(void)
{
    int fails = 0;
    mmio_mock_reset();
    i2c_test_reset();
    mmio_mock_set_read(DEV_EN_ADDR, ALL_THREE);
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    i2c_init();                                  /* first init: full sequence */
    clock_gate_suspend();

    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR, 0);
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    i2c_init();                                  /* re-init: drain only */
    trace_cursor tc = trace_begin("i2c_init_regates");
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_I2C);
    expect_r(&tc, 8,  I2C_STATUS_ADDR);
    trace_expect_end(&tc);
    fails += trace_done(&tc);
    clock_gate_resume();
    return fails;
}

int main(void)
{
    int fails = 0;
    fails += test_gate_all_on();
    fails += test_gate_pwm_was_off();
    fails += test_i2c_self_restore();
    fails += test_uart_self_restore();
    fails += test_piezo_self_restore();
    fails += test_i2c_init_while_gated();

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
