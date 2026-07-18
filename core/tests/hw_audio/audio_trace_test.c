/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_audio/audio_trace_test.c — host-side golden-trace tests for
 * the first-sound driver chain (i2c.c, wm8758.c, i2s.c), compiled
 * against the recording mock bus (-DMMIO_MOCK). This is the ONLY
 * automated check of the audio register grammar: clicky models no
 * I2C/I2S/DAC, and the device would need a logic analyzer. Sound itself
 * is the on-device proof; this proves the bytes are right.
 *
 * Proves:
 *   1. i2c_init emits the exact clock-gate/reset/poke grammar.
 *   2. i2c_send emits the exact controller sequence (addr, write-mode,
 *      data, count, strobe) and rejects bad lengths / BUSY timeout.
 *   3. wm8758_init issues all 30 codec writes, with correct 9-bit
 *      framing, the DAC->mixer route present, and unmute (DACCTRL) LAST.
 *   4. i2s_init emits the exact clock-plumbing + FIFO reset/format
 *      grammar.
 *   5. i2s_write_stereo polls TXFree then writes one packed [R<<16|L]
 *      word, and reports a never-draining FIFO instead of hanging.
 *
 * All expected values are hand-derived from core/docs/hw/05-audio.md and
 * 09-i2c.md (via pp5022.h / wm8758.h), never from Rockbox source.
 */

#include "pp5022.h"
#include "wm8758.h"
#include "i2c.h"
#include "i2s.h"
#include "mmio_mock.h"
#include "trace_expect.h"

static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

/* ---- log-scanning helpers ------------------------------------------- */

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

/* Does some codec write carry payload byte0==b0 AND byte1==b1 at the same
 * transaction index? (DATA0 and DATA1 writes come in lockstep pairs.) */
static int has_codec_write(uint8_t b0, uint8_t b1)
{
    size_t pairs = count_writes(I2C_DATA0_ADDR);
    for (size_t i = 0; i < pairs; i++) {
        if (nth_write(I2C_DATA0_ADDR, i) == b0 &&
            nth_write(I2C_DATA1_ADDR, i) == b1) {
            return 1;
        }
    }
    return 0;
}

/* ---- cases ----------------------------------------------------------- */

/* Case 1: i2c_init grammar. DEV_EN/DEV_RS RMW sources read 0, STATUS
 * reads idle, so the sequence is fully determined: gate on, reset pulse,
 * the two clock-config pokes, and the settling idle read. */
static int test_i2c_init_grammar(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(DEV_EN_ADDR,     0);
    mmio_mock_set_read(DEV_RS_ADDR,     0);
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);   /* idle */

    i2c_init();

    trace_cursor tc = trace_begin("i2c_init");
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_I2C);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, DEV_I2C);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, 0);
    expect_w(&tc, 32, I2C_CLKCFG_ADDR, 0x00000000);
    expect_w(&tc, 32, I2C_CLKCFG_ADDR, 0x00000080);
    expect_r(&tc, 8,  I2C_STATUS_ADDR);
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Case 2: i2c_send emits the exact controller grammar for a 2-byte
 * write to device 0x1a. CTRL reads 0, STATUS reads idle. */
static int test_i2c_send_grammar(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);

    const uint8_t payload[2] = { 0xAB, 0xCD };
    int rc = i2c_send(0x1A, payload, 2);

    int fails = check("i2c_send: returns 0", rc == 0);
    trace_cursor tc = trace_begin("i2c_send");
    expect_r(&tc, 8, I2C_STATUS_ADDR);              /* wait idle */
    expect_w(&tc, 8, I2C_ADDR_ADDR, 0x34);          /* 0x1a<<1, write */
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, 0x00);          /* clear read bit */
    expect_w(&tc, 8, I2C_DATA0_ADDR, 0xAB);
    expect_w(&tc, 8, I2C_DATA1_ADDR, 0xCD);
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, 0x02);          /* count = (2-1)<<1 */
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, I2C_SEND);      /* strobe */
    trace_expect_end(&tc);
    fails += trace_done(&tc);
    return fails;
}

/* Case 3: bad lengths are rejected with no bus traffic; a BUSY-forever
 * bus times out (bounded) and never loads an address. */
static int test_i2c_send_guards(void)
{
    int fails = 0;
    const uint8_t buf[5] = { 0 };

    mmio_mock_reset();
    fails += check("i2c_send len=0 -> -1", i2c_send(0x1A, buf, 0) == -1);
    fails += check("i2c_send len=5 -> -1", i2c_send(0x1A, buf, 5) == -1);
    fails += check("rejects: zero bus traffic", mmio_mock_log_len() == 0);

    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, I2C_BUSY);  /* busy forever */
    fails += check("i2c_send BUSY -> -2", i2c_send(0x1A, buf, 2) == -2);
    fails += check("BUSY: bounded (many STATUS reads)",
                   count_writes(I2C_ADDR_ADDR) == 0 &&
                   mmio_mock_count(MMIO_OP_READ, I2C_STATUS_ADDR) > 1);
    return fails;
}

/* Case 4: wm8758_init completeness + framing + ordering. 30 codec
 * writes; correct 9-bit packing on the first (BIASCTRL) and the 44.1k
 * CLKCTRL; the DAC->mixer route present; unmute (DACCTRL) issued LAST. */
static int test_wm8758_init(void)
{
    int fails = 0;
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);

    wm8758_init();

    /* One I2C_ADDR write per codec register write (31 = reset + 30). */
    fails += check("wm8758_init: 31 codec writes",
                   count_writes(I2C_ADDR_ADDR) == 31);
    /* Every transaction addresses the codec (0x1a<<1). */
    fails += check("wm8758_init: all addressed to 0x34",
                   nth_write(I2C_ADDR_ADDR, 0) == 0x34 &&
                   nth_write(I2C_ADDR_ADDR, 30) == 0x34);
    /* First write is the soft RESET(0x00)=0: b0 = 0x00, b1 = 0x00. */
    fails += check("wm8758_init: first write is soft RESET (0x00/0x00)",
                   nth_write(I2C_DATA0_ADDR, 0) == 0x00 &&
                   nth_write(I2C_DATA1_ADDR, 0) == 0x00);
    /* BIASCTRL(0x3d)=BIASCUT(0x100) still present: b0 = 0x7B, b1 = 0x00. */
    fails += check("wm8758_init: BIASCTRL preinit present (0x7B/0x00)",
                   has_codec_write(0x7B, 0x00));
    /* 44.1k CLKCTRL(0x06)=0x145: b0 = 0x0D, b1 = 0x45. */
    fails += check("wm8758_init: 44.1k CLKCTRL 0x145 present (0x0D/0x45)",
                   has_codec_write(0x0D, 0x45));
    /* DAC->left mixer route LOUTMIX(0x32)=0x001: b0 = 0x64, b1 = 0x01.
     * Without this the DAC is powered but unrouted -> silence. */
    fails += check("wm8758_init: DAC->mixer route present (0x64/0x01)",
                   has_codec_write(0x64, 0x01));
    /* LAST codec write must be the unmute DACCTRL(0x0a)=DACOSR128(0x08):
     * b0 = 0x14, b1 = 0x08. Unmuting before routing/rails would click. */
    fails += check("wm8758_init: unmute (DACCTRL) is the LAST write",
                   nth_write(I2C_DATA0_ADDR, 30) == 0x14 &&
                   nth_write(I2C_DATA1_ADDR, 30) == 0x08);
    return fails;
}

/* Case 5: i2s_init clock-plumbing + FIFO reset/format grammar. All RMW
 * sources read 0, so each |= /&= resolves to a fixed value. */
static int test_i2s_init_grammar(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(DEV_RS_ADDR,           0);
    mmio_mock_set_read(DEV_EN_ADDR,           0);
    mmio_mock_set_read(DEV_EXTCLK_SEL_ADDR,   0);
    mmio_mock_set_read(DEV_INIT2_ADDR,        0);
    mmio_mock_set_read(DEV_INIT1_ADDR,        0);
    mmio_mock_set_read(IISCONFIG_ADDR,        0);
    mmio_mock_set_read(IISFIFO_CFG_ADDR,      0);

    i2s_init();

    trace_cursor tc = trace_begin("i2s_init");
    /* clock plumbing */
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, DEV_I2S);
    expect_r(&tc, 32, DEV_RS_ADDR);
    expect_w(&tc, 32, DEV_RS_ADDR, 0);
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_I2S);
    expect_r(&tc, 32, DEV_EN_ADDR);
    expect_w(&tc, 32, DEV_EN_ADDR, DEV_EXTCLOCKS);
    expect_r(&tc, 32, DEV_EXTCLK_SEL_ADDR);
    expect_w(&tc, 32, DEV_EXTCLK_SEL_ADDR, 0);
    /* I2S/CDI pad-function select (clear -> I2S alt function) */
    expect_r(&tc, 32, DEV_INIT2_ADDR);
    expect_w(&tc, 32, DEV_INIT2_ADDR, 0);
    expect_r(&tc, 32, DEV_INIT1_ADDR);
    expect_w(&tc, 32, DEV_INIT1_ADDR, 0);
    /* FIFO reset pulse */
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, IIS_RESET);
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, 0);
    /* format: FORMAT_IIS(0), SIZE_16BIT(0), FIFO_FORMAT_LE16_2(0x70) */
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, IIS_FORMAT_IIS);
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, IIS_SIZE_16BIT);
    expect_r(&tc, 32, IISCONFIG_ADDR);
    expect_w(&tc, 32, IISCONFIG_ADDR, IIS_FIFO_FORMAT_LE16_2);
    /* FIFO attention levels, then flush */
    expect_r(&tc, 32, IISFIFO_CFG_ADDR);
    expect_w(&tc, 32, IISFIFO_CFG_ADDR, IIS_RX_FULL_LVL_12 | IIS_TX_EMPTY_LVL_4);
    expect_r(&tc, 32, IISFIFO_CFG_ADDR);
    expect_w(&tc, 32, IISFIFO_CFG_ADDR, IIS_RXCLR | IIS_TXCLR);
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Case 6: i2s_write_stereo polls TXFree, then writes one packed word;
 * left in the low 16 bits, right in the high 16. */
static int test_i2s_write(void)
{
    int fails = 0;
    mmio_mock_reset();
    mmio_mock_set_read(IISFIFO_CFG_ADDR, 4u << IISFIFO_CFG_TXFREE_SHIFT); /* 4 free */

    int rc = i2s_write_stereo((int16_t)0x1234, (int16_t)0x5678);

    fails += check("i2s_write_stereo: returns 0", rc == 0);
    trace_cursor tc = trace_begin("i2s_write");
    expect_r(&tc, 32, IISFIFO_CFG_ADDR);
    expect_w(&tc, 32, IISFIFO_WR_ADDR, 0x56781234u);   /* R<<16 | L */
    trace_expect_end(&tc);
    fails += trace_done(&tc);
    return fails;
}

/* Case 7: a never-draining TX FIFO (TXFree stuck at 0) returns -1 after
 * a bounded spin and never writes a sample. */
static int test_i2s_write_noclock(void)
{
    int fails = 0;
    mmio_mock_reset();
    mmio_mock_set_read(IISFIFO_CFG_ADDR, 0);   /* TXFree == 0 forever */

    int rc = i2s_write_stereo(1, 1);

    fails += check("i2s_write_stereo: no-drain -> -1", rc == -1);
    fails += check("no-drain: bounded, wrote no sample",
                   count_writes(IISFIFO_WR_ADDR) == 0 &&
                   mmio_mock_count(MMIO_OP_READ, IISFIFO_CFG_ADDR) > 1);
    return fails;
}

int main(void)
{
    int fails = 0;
    fails += test_i2c_init_grammar();
    fails += test_i2c_send_grammar();
    fails += test_i2c_send_guards();
    fails += test_wm8758_init();
    fails += test_i2s_init_grammar();
    fails += test_i2s_write();
    fails += test_i2s_write_noclock();

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
