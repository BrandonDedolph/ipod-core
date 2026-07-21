/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_mmio/backlight_trace_test.c — golden-trace test for the LCD
 * backlight dimmer driver (hal/hw/backlight.c), compiled host-side
 * against the recording mock bus (-DMMIO_MOCK).
 *
 * The backlight is bit-banged over GPIO with no status register to poll,
 * so this asserts the exact ordered WRITE grammar the driver emits:
 *   - backlight_init: GPIO route (B/D ENABLE+OUTPUT_EN), circuit power +
 *     step-line-high, the 16-step walk from the power-up reference (16) up
 *     to BACKLIGHT_MAX (32), then the LED-enable trio (L);
 *   - backlight_set(0): a single atomic clear of the GPIOL LED line;
 *   - backlight_set(BACKLIGHT_MAX): re-assert the LED line, then walk the
 *     tracked level back up to full.
 * Every op is a single write to the +0x800 atomic set/clear alias (no
 * read-modify-write). Pulse WIDTH (up vs down) is a busy-loop count with
 * no MMIO, so it is deliberately invisible here — the trace validates the
 * register/value/count grammar, and the device validates the timing.
 *
 * bl_level is a file-static the cases thread through in main() order.
 */

#include "pp5022.h"
#include "backlight.h"
#include "mmio_mock.h"
#include "trace_expect.h"

/* Backlight GPIO bits (mirror of backlight.c's private #defines; the
 * cleanroom test vectors are derived from the docs, not the driver). */
#define BL_B  0x08u     /* GPIOB bit 3: circuit power */
#define BL_D  0x80u     /* GPIOD bit 7: step/clock     */
#define BL_L  0x80u     /* GPIOL bit 7: LED enable     */

/* Atomic-alias address and the masked-write value encoding (pp5022.h). */
#define ALIAS(addr)  ((addr) + GPIO_BITWISE_OFFSET)
#define SETV(m)      (((uint32_t)(m) << 8) | (uint32_t)(m))  /* set bits m  */
#define CLRV(m)      ((uint32_t)(m) << 8)                    /* clear bits m */

/* One dimmer step = a clear-then-set pulse on the GPIOD step line. The
 * up/down direction differs only in (untraced) low-pulse width, so both
 * emit this same two-write pair. */
static void expect_step(trace_cursor *tc)
{
    expect_w(tc, 32, ALIAS(GPIOD_OUTPUT_VAL_ADDR), CLRV(BL_D));
    expect_w(tc, 32, ALIAS(GPIOD_OUTPUT_VAL_ADDR), SETV(BL_D));
}

/* backlight_init: full config + power-up + walk 16->32 + LED on. */
static int test_backlight_init(void)
{
    mmio_mock_reset();
    backlight_init();

    trace_cursor tc = trace_begin("backlight_init");
    /* GPIO route: B then D, ENABLE then OUTPUT_EN */
    expect_w(&tc, 32, ALIAS(GPIOB_ENABLE_ADDR),    SETV(BL_B));
    expect_w(&tc, 32, ALIAS(GPIOB_OUTPUT_EN_ADDR), SETV(BL_B));
    expect_w(&tc, 32, ALIAS(GPIOD_ENABLE_ADDR),    SETV(BL_D));
    expect_w(&tc, 32, ALIAS(GPIOD_OUTPUT_EN_ADDR), SETV(BL_D));
    /* circuit power on + step line idle high */
    expect_w(&tc, 32, ALIAS(GPIOB_OUTPUT_VAL_ADDR), SETV(BL_B));
    expect_w(&tc, 32, ALIAS(GPIOD_OUTPUT_VAL_ADDR), SETV(BL_D));
    /* walk from the power-up reference (16) up to full (32): 16 steps */
    for (int i = 0; i < BACKLIGHT_MAX - 16; i++) {
        expect_step(&tc);
    }
    /* LED enable trio */
    expect_w(&tc, 32, ALIAS(GPIOL_ENABLE_ADDR),     SETV(BL_L));
    expect_w(&tc, 32, ALIAS(GPIOL_OUTPUT_EN_ADDR),  SETV(BL_L));
    expect_w(&tc, 32, ALIAS(GPIOL_OUTPUT_VAL_ADDR), SETV(BL_L));
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Dim to a mid level: LED stays on, walk 32->8 (24 down steps). Seeds a
 * below-max tracked level so the later set(MAX) is a real walk. */
static int test_backlight_dim(void)
{
    mmio_mock_reset();
    backlight_set(8);

    trace_cursor tc = trace_begin("backlight_dim");
    expect_w(&tc, 32, ALIAS(GPIOL_OUTPUT_VAL_ADDR), SETV(BL_L)); /* ensure on */
    for (int i = 0; i < 32 - 8; i++) {
        expect_step(&tc);
    }
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Off: exactly one atomic clear of the LED line, nothing else. */
static int test_backlight_off(void)
{
    mmio_mock_reset();
    backlight_set(0);

    trace_cursor tc = trace_begin("backlight_off");
    expect_w(&tc, 32, ALIAS(GPIOL_OUTPUT_VAL_ADDR), CLRV(BL_L));
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Full: re-assert the LED line (off didn't change the tracked level=8),
 * then walk 8->32 (24 up steps). */
static int test_backlight_full(void)
{
    mmio_mock_reset();
    backlight_set(BACKLIGHT_MAX);

    trace_cursor tc = trace_begin("backlight_full");
    expect_w(&tc, 32, ALIAS(GPIOL_OUTPUT_VAL_ADDR), SETV(BL_L));
    for (int i = 0; i < BACKLIGHT_MAX - 8; i++) {
        expect_step(&tc);
    }
    trace_expect_end(&tc);
    return trace_done(&tc);
}

int main(void)
{
    int fails = 0;
    /* order matters: the file-static tracked level threads through */
    fails += test_backlight_init();   /* level -> 32 */
    fails += test_backlight_dim();    /* level -> 8  */
    fails += test_backlight_off();    /* level stays 8, LED off */
    fails += test_backlight_full();   /* level 8 -> 32 (full) */
    return fails == 0 ? 0 : 1;
}
