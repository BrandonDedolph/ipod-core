/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_mmio/volume_trace_test.c — host-side tests for the WM8758
 * output-volume HAL (hal/hw/volume.c), compiled against the recording
 * mock bus (-DMMIO_MOCK).
 *
 * Two layers, neither touching real MMIO:
 *   1. The PURE percent -> OUT1VOL data-word mapping
 *      (hal_volume_out1_word): monotonic gain, clamping of out-of-range
 *      input, 0% mutes, 100% is exactly 0 dB (0x39), never +6 dB.
 *   2. hal_volume_set emits the LOUT1VOL/ROUT1VOL two-write grammar over
 *      the reused i2c.c write path — VU set on the RIGHT write only — and
 *      hal_volume_get round-trips the clamped percent.
 *
 * All expected values are derived from core/docs/hw/05-audio.md (via
 * wm8758.h), never from Rockbox source.
 */

#include <stdio.h>

#include "pp5022.h"
#include "wm8758.h"
#include "i2c.h"
#include "volume.h"
#include "mmio_mock.h"

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

/* Value of the n-th (0-based) write to `addr`; ~0u if there is none. */
static uint32_t nth_write(uint32_t addr, size_t n)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len(), c = 0;
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == addr) {
            if (c == n) {
                return log[i].value;
            }
            c++;
        }
    }
    return ~0u;
}

static size_t count_writes(uint32_t addr)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len(), c = 0;
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == addr) {
            c++;
        }
    }
    return c;
}

/* Case 1: pure mapping — endpoints, mute, and the +6 dB ceiling guard. */
static int test_mapping_endpoints(void)
{
    int fails = 0;

    /* 0% -> MUTE bit set, no VU (caller adds VU on the right write). */
    uint16_t w0 = hal_volume_out1_word(0);
    fails += check("map 0%: MUTE set", (w0 & OUTVOL_MUTE) != 0);
    fails += check("map 0%: no VU", (w0 & OUTVOL_VU) == 0);

    /* 100% -> gain field exactly 0 dB (0x39), MUTE clear. */
    uint16_t w100 = hal_volume_out1_word(100);
    fails += check("map 100%: gain == 0x39 (0 dB)",
                   (w100 & OUTVOL_GAIN_MASK) == 0x39);
    fails += check("map 100%: not muted", (w100 & OUTVOL_MUTE) == 0);
    fails += check("map 100%: never +6 dB (<= 0x39)",
                   (w100 & OUTVOL_GAIN_MASK) <= 0x39);

    /* ZC set across the audible range so gain changes are click-free. */
    fails += check("map 50%: ZC set",
                   (hal_volume_out1_word(50) & OUTVOL_ZC) != 0);
    return fails;
}

/* Case 2: pure mapping — clamping of out-of-range input. */
static int test_mapping_clamp(void)
{
    int fails = 0;
    /* Negative clamps to the 0% (mute) behaviour. */
    fails += check("map -10 clamps to mute",
                   (hal_volume_out1_word(-10) & OUTVOL_MUTE) != 0);
    /* Over 100 clamps to the 100% word. */
    fails += check("map 250 clamps to 100% word",
                   hal_volume_out1_word(250) == hal_volume_out1_word(100));
    return fails;
}

/* Case 3: pure mapping — monotonic non-decreasing gain over 1..100%. */
static int test_mapping_monotonic(void)
{
    int prev = -1;
    int ok = 1;
    for (int p = 1; p <= 100; p++) {
        int g = (int)(hal_volume_out1_word(p) & OUTVOL_GAIN_MASK);
        if (g < prev) {
            ok = 0;
            break;
        }
        prev = g;
    }
    /* And it must actually rise across the span (not a flat line). */
    int lo = (int)(hal_volume_out1_word(1)   & OUTVOL_GAIN_MASK);
    int hi = (int)(hal_volume_out1_word(100) & OUTVOL_GAIN_MASK);
    return check("map: monotonic non-decreasing over 1..100%", ok)
         + check("map: gain strictly rises across the span", hi > lo);
}

/* Case 4: hal_volume_set emits the L-then-R(+VU) codec grammar. A codec
 * write is byte0=(reg<<1)|data8, byte1=data&0xFF. For reg 0x34/0x35 the
 * data high bit is only set by the VU latch (0x100). */
static int test_set_grammar(void)
{
    int fails = 0;
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);   /* idle */
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);

    hal_volume_set(100);

    /* Exactly two codec transactions (L, then R). */
    fails += check("set(100): two codec writes",
                   count_writes(I2C_ADDR_ADDR) == 2);
    fails += check("set(100): both addressed to codec (0x1a<<1=0x34)",
                   nth_write(I2C_ADDR_ADDR, 0) == 0x34 &&
                   nth_write(I2C_ADDR_ADDR, 1) == 0x34);

    /* Word 0: LOUT1VOL(0x34) = 0x39|ZC = 0x0B9 -> b0=(0x34<<1)|0=0x68,
     * b1=0xB9. No VU, so data bit8 is clear. */
    fails += check("set(100): LOUT1VOL byte0 = 0x68",
                   nth_write(I2C_DATA0_ADDR, 0) == 0x68);
    fails += check("set(100): LOUT1VOL byte1 = 0xB9 (0x39|ZC)",
                   nth_write(I2C_DATA1_ADDR, 0) == 0xB9);

    /* Word 1: ROUT1VOL(0x35) = 0x39|ZC|VU = 0x1B9 -> b0=(0x35<<1)|1=0x6B
     * (VU rides the data bit8 -> first-byte LSB), b1=0xB9. */
    fails += check("set(100): ROUT1VOL byte0 = 0x6B (reg 0x35 + VU bit8)",
                   nth_write(I2C_DATA0_ADDR, 1) == 0x6B);
    fails += check("set(100): ROUT1VOL byte1 = 0xB9",
                   nth_write(I2C_DATA1_ADDR, 1) == 0xB9);
    return fails;
}

/* Case 5: hal_volume_set(0) mutes both channels (MUTE bit in byte1). */
static int test_set_mute(void)
{
    int fails = 0;
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);

    hal_volume_set(0);

    /* MUTE(0x40) | ZC(0x80) = 0xC0 in the low byte of both writes. */
    fails += check("set(0): LOUT1VOL byte1 has MUTE|ZC (0xC0)",
                   nth_write(I2C_DATA1_ADDR, 0) == 0xC0);
    fails += check("set(0): ROUT1VOL byte0 has VU bit (reg 0x35 -> 0x6B)",
                   nth_write(I2C_DATA0_ADDR, 1) == 0x6B);
    return fails;
}

/* Case 6: hal_volume_get round-trips the last (clamped) percent. */
static int test_get_roundtrip(void)
{
    int fails = 0;
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);

    hal_volume_set(42);
    fails += check("get after set(42) == 42", hal_volume_get() == 42);
    hal_volume_set(-5);
    fails += check("get after set(-5) clamps to 0", hal_volume_get() == 0);
    hal_volume_set(999);
    fails += check("get after set(999) clamps to 100", hal_volume_get() == 100);
    return fails;
}

int main(void)
{
    int fails = 0;
    fails += test_mapping_endpoints();
    fails += test_mapping_clamp();
    fails += test_mapping_monotonic();
    fails += test_set_grammar();
    fails += test_set_mute();
    fails += test_get_roundtrip();

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
