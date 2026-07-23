/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_mmio/battery_trace_test.c — golden-trace test for the battery
 * gauge + power-state driver (hal/hw/battery.c together with the new
 * hal/hw/i2c.c register-pointer read path), compiled host-side against
 * the recording mock bus (-DMMIO_MOCK).
 *
 * There is no clicky model of the PMU / I2C bus and the device would need
 * a logic analyzer, so this is the only automated check of the battery
 * read grammar. It asserts, in order:
 *
 *   1. battery_millivolts() emits the EXACT I2C transaction stream:
 *        write(0x08, {ADCC1=0x2F, 0x05})            channel-select+start
 *        then register-pointer read(0x08, reg=0x30, 2 bytes)  ADCS1/ADCS2
 *      — the write-mode/read-mode CTRL framing, the R/W address bit, the
 *      count fields, the strobes, the turn-around wait, and the two DATA
 *      latches — and returns the expected mV for a seeded 10-bit raw.
 *   2. power_is_external() / power_is_charging() read the RIGHT GPIO input
 *      register at the RIGHT width and decode the RIGHT bit + polarity
 *      (main charger active-low, USB active-high, charging active-low).
 *
 * Values are hand-derived from core/docs/hw/06-power.md (via pp5022.h),
 * never extracted from Rockbox source — the cleanroom boundary applies to
 * test vectors too.
 *
 * MUTATION CHECK (performed manually during bring-up, 2026-07-21): flipping
 * the expected mV assertion below from 3996 to 3997 makes the test FAIL,
 * and changing the expected ADCC1 select value from 0x05 to 0x04 makes the
 * grammar assertion FAIL — so both assertions have teeth.
 */

#include "pp5022.h"
#include "battery.h"
#include "mmio_mock.h"
#include "trace_expect.h"

/* I2C controller bits mirrored from the docs (pp5022.h), used to build
 * the expected CTRL/ADDR values the driver must emit. */
#define PMU_DEV        0x08u
#define ADDR_WR        ((PMU_DEV << 1))                 /* 0x10, R/W=0 */
#define ADDR_RD        ((PMU_DEV << 1) | I2C_ADDR_RW)   /* 0x11, R/W=1 */
#define CTRL_WRMODE    0x00u                            /* read bit cleared      */
#define CTRL_CNT(n)    ((uint8_t)(((n) - 1) << 1))      /* count in bits 2:1     */
#define CTRL_RDMODE(n) ((uint8_t)(I2C_READ | CTRL_CNT(n)))
#define CTRL_STROBE    (I2C_SEND)                       /* CTRL read-back is 0   */

/* Expect one i2c_send(dev-write) of `len` payload bytes b0[,b1]. The mock
 * serves every CTRL/STATUS read as 0, so the read-modify-write values are
 * deterministic. */
static void expect_send_open(trace_cursor *tc)
{
    expect_r(tc, 8, I2C_STATUS_ADDR);                 /* wait_idle */
    expect_w(tc, 8, I2C_ADDR_ADDR, ADDR_WR);
    expect_r(tc, 8, I2C_CTRL_ADDR);                   /* select write mode */
    expect_w(tc, 8, I2C_CTRL_ADDR, CTRL_WRMODE);
}

static void expect_send_close(trace_cursor *tc, int len)
{
    expect_r(tc, 8, I2C_CTRL_ADDR);                   /* set count */
    expect_w(tc, 8, I2C_CTRL_ADDR, CTRL_CNT(len));
    expect_r(tc, 8, I2C_CTRL_ADDR);                   /* strobe */
    expect_w(tc, 8, I2C_CTRL_ADDR, CTRL_STROBE);
}

/* battery_millivolts(): the full channel-select write + result read. */
static int test_battery_millivolts(void)
{
    mmio_mock_reset();
    /* Seed a known 10-bit raw: data[0]=0xAA (high 8), data[1]=0xFE
     * (only bits 1:0 count -> 0b10). raw = (0xAA<<2)|(0xFE&3) = 682.
     * mV = 682*6000>>10 = 3996. The 0xFE also proves the &0x03 mask. */
    mmio_mock_set_read(I2C_DATA0_ADDR, 0xAA);
    mmio_mock_set_read(I2C_DATA1_ADDR, 0xFE);

    int mv = battery_millivolts();

    trace_cursor tc = trace_begin("battery_millivolts");

    /* --- write ADCC1(0x2F) = 0x05 (channel ADCVIN1, start) --- */
    expect_send_open(&tc);
    expect_w(&tc, 8, I2C_DATA0_ADDR, 0x2F);           /* register ADCC1 */
    expect_w(&tc, 8, I2C_DATA1_ADDR, 0x05);           /* (ch<<1)|start  */
    expect_send_close(&tc, 2);

    /* --- register-pointer read of 2 bytes @ ADCS1(0x30) --- */
    /* i2c_read phase 1: 1-byte write of the register pointer 0x30 */
    expect_send_open(&tc);
    expect_w(&tc, 8, I2C_DATA0_ADDR, 0x30);           /* register ADCS1 */
    expect_send_close(&tc, 1);
    /* turn-around wait after the pointer write lands */
    expect_r(&tc, 8, I2C_STATUS_ADDR);
    /* phase 2: read transaction (addr R/W=1, read mode+count, strobe) */
    expect_w(&tc, 8, I2C_ADDR_ADDR, ADDR_RD);
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, CTRL_RDMODE(2));
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, CTRL_STROBE);
    /* block for completion before latching the result bytes */
    expect_r(&tc, 8, I2C_STATUS_ADDR);
    expect_r(&tc, 8, I2C_DATA0_ADDR);                 /* ADCS1 -> 0xAA */
    expect_r(&tc, 8, I2C_DATA1_ADDR);                 /* ADCS2 -> 0xFE */
    trace_expect_end(&tc);

    if (mv != 3996) {
        fprintf(stderr, "[battery_millivolts] value: expected 3996, got %d\n",
                mv);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* battery_percent(): sanity that the curve+read compose (raw 682 -> 3996
 * mV sits between the 80%(4020) and 70%(3950) points -> ~76%). */
static int test_battery_percent(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(I2C_DATA0_ADDR, 0xAA);
    mmio_mock_set_read(I2C_DATA1_ADDR, 0xFE);

    int pct = battery_percent();
    trace_cursor tc = trace_begin("battery_percent");
    /* 3996 mV: point[7]=3950 (70%), point[8]=4020 (80%); span 70,
     * into 46 -> 70 + (46*10+35)/70 = 70 + 495/70 = 70 + 7 = 77. */
    if (pct != 77) {
        fprintf(stderr, "[battery_percent] expected 77, got %d\n", pct);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* Assert power_is_external() reads GPIOL_INPUT_VAL (32-bit) and returns
 * `want` for the seeded pin word. */
static int expect_external(uint32_t gpiol, int want, const char *label)
{
    mmio_mock_reset();
    mmio_mock_set_read(GPIOL_INPUT_VAL_ADDR, gpiol);

    int got = power_is_external();

    trace_cursor tc = trace_begin(label);
    expect_r(&tc, 32, GPIOL_INPUT_VAL_ADDR);
    trace_expect_end(&tc);
    if ((got != 0) != (want != 0)) {
        fprintf(stderr, "[%s] GPIOL=%08X: expected %d, got %d\n",
                label, gpiol, want, got);
        tc.fails++;
    }
    return trace_done(&tc);
}

static int expect_charging(uint32_t gpiob, int want, const char *label)
{
    mmio_mock_reset();
    mmio_mock_set_read(GPIOB_INPUT_VAL_ADDR, gpiob);

    int got = power_is_charging();

    trace_cursor tc = trace_begin(label);
    expect_r(&tc, 32, GPIOB_INPUT_VAL_ADDR);
    trace_expect_end(&tc);
    if ((got != 0) != (want != 0)) {
        fprintf(stderr, "[%s] GPIOB=%08X: expected %d, got %d\n",
                label, gpiob, want, got);
        tc.fails++;
    }
    return trace_done(&tc);
}

int main(void)
{
    int fails = 0;
    fails += test_battery_millivolts();
    fails += test_battery_percent();

    /* power_is_external polarity matrix:
     *   main charger bit 0x08 is ACTIVE-LOW (clear = present),
     *   USB charger bit  0x10 is ACTIVE-HIGH (set  = present). */
    fails += expect_external(0x10, 1, "external_main");   /* 0x08 clear -> main present */
    fails += expect_external(0x08, 0, "external_none");   /* 0x08 set, 0x10 clear -> none */
    fails += expect_external(0x18, 1, "external_usb");    /* 0x08 set, 0x10 set -> USB */

    /* power_is_charging: bit 0x01 ACTIVE-LOW (clear = charging). */
    fails += expect_charging(0x00, 1, "charging_yes");
    fails += expect_charging(0x01, 0, "charging_no");

    return fails == 0 ? 0 : 1;
}
