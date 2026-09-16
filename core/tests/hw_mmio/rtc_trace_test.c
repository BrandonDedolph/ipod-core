/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_mmio/rtc_trace_test.c — golden-trace test for the PCF50605 RTC
 * driver (hal/hw/rtc.c over hal/hw/i2c.c), compiled host-side against the
 * recording mock bus (-DMMIO_MOCK).
 *
 * WHY THIS MATTERS MORE THAN THE USUAL TRACE TEST. The RTC register map is not
 * in docs/hw/ — it is derived from the PCF50606 datasheet and cross-checked
 * against the six PMU registers 06-power.md does document — so the DEVICE
 * cannot confirm it until someone flashes an image and reads the raw bytes.
 * What can be settled here is everything around that uncertainty:
 *
 *   1. the exact bus grammar a read and a write emit (which registers, in
 *      which order, in which chunks) — so that when the bench says "the map is
 *      wrong", the fix is one table and not a re-read of the driver;
 *   2. that EVERY shape of wrong answer decodes as "unset" and never as a
 *      plausible date: bad BCD, the year-00 reset value, and in particular the
 *      NACK shape, where i2c_read cannot see the missing ack and hands back
 *      the DATA registers' last written bytes (hal/hw/battery.c:114 explains
 *      the same artefact in the gauge);
 *   3. that a second boundary between the two read chunks is caught and
 *      re-read — a tear at 23:59:59 would otherwise be a whole day wrong;
 *   4. that hal_rtc_set refuses an out-of-range epoch without touching the
 *      bus, and reports a read-back that disagrees rather than claiming
 *      success.
 *
 * Values are hand-derived from docs/hw/06-power.md and 09-i2c.md (via
 * pp5022.h), never extracted from Rockbox source — the cleanroom boundary
 * applies to test vectors too.
 *
 * MUTATION CHECK (performed while writing): dropping the third one-byte read
 * makes torn_read fail; accepting year 00 makes unset_year fail; writing the
 * year before the seconds makes write_order fail; a ±2 s read-back tolerance
 * makes readback_mismatch fail.
 */

#include "pp5022.h"
#include "rtc.h"
#include "hal.h"
#include "mmio_mock.h"
#include "trace_expect.h"

/* The expected I2C framing, built the way battery_trace_test.c builds it. */
#define PMU_DEV        0x08u
#define ADDR_WR        ((PMU_DEV << 1))                 /* 0x10, R/W=0 */
#define ADDR_RD        ((PMU_DEV << 1) | I2C_ADDR_RW)   /* 0x11, R/W=1 */
#define CTRL_WRMODE    0x00u
#define CTRL_CNT(n)    ((uint8_t)(((n) - 1) << 1))
#define CTRL_RDMODE(n) ((uint8_t)(I2C_READ | CTRL_CNT(n)))
#define CTRL_STROBE    (I2C_SEND)

/* The register map under test (rtc.c keeps its own copy; stating it again here
 * is the point — a map change has to be made in two places on purpose). */
#define R_SC 0x0Au
#define R_MN 0x0Bu
#define R_HR 0x0Cu
#define R_WD 0x0Du
#define R_DT 0x0Eu
#define R_MT 0x0Fu
#define R_YR 0x10u

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

/* One two-byte write: {register, value}. */
static void expect_write_reg(trace_cursor *tc, uint32_t reg, uint32_t val)
{
    expect_send_open(tc);
    expect_w(tc, 8, I2C_DATA0_ADDR, reg);
    expect_w(tc, 8, I2C_DATA1_ADDR, val);
    expect_send_close(tc, 2);
}

/* One register-pointer read of `n` bytes starting at `reg`. */
static void expect_read(trace_cursor *tc, uint32_t reg, int n)
{
    expect_send_open(tc);
    expect_w(tc, 8, I2C_DATA0_ADDR, reg);             /* the pointer write */
    expect_send_close(tc, 1);
    expect_r(tc, 8, I2C_STATUS_ADDR);                 /* turn-around wait  */
    expect_w(tc, 8, I2C_ADDR_ADDR, ADDR_RD);
    expect_r(tc, 8, I2C_CTRL_ADDR);
    expect_w(tc, 8, I2C_CTRL_ADDR, CTRL_RDMODE(n));
    expect_r(tc, 8, I2C_CTRL_ADDR);
    expect_w(tc, 8, I2C_CTRL_ADDR, CTRL_STROBE);
    expect_r(tc, 8, I2C_STATUS_ADDR);                 /* result valid only now */
    for (int i = 0; i < n; i++) {
        expect_r(tc, 8, (uint32_t)I2C_DATA_ADDR(i));
    }
}

/* The three transactions one rtc_read pass makes: SC MN HR WD, DT MT YR, SC. */
static void expect_calendar_read(trace_cursor *tc)
{
    expect_read(tc, R_SC, 4);
    expect_read(tc, R_DT, 3);
    expect_read(tc, R_SC, 1);
}

/* Seed a constant calendar: 2026-09-16 10:42:00, Wednesday (WD 3). */
static void seed_calendar(void)
{
    static const uint32_t d0[] = { 0x00u, 0x16u, 0x00u };   /* SC, DT, SC  */
    static const uint32_t d1[] = { 0x42u, 0x09u };          /* MN, MT      */
    static const uint32_t d2[] = { 0x10u, 0x26u };          /* HR, YR      */
    mmio_mock_queue_read(I2C_DATA0_ADDR, d0, 3);
    mmio_mock_queue_read(I2C_DATA1_ADDR, d1, 2);
    mmio_mock_queue_read(I2C_DATA2_ADDR, d2, 2);
    mmio_mock_set_read(I2C_DATA3_ADDR, 0x03u);              /* WD (ignored) */
}

static int test_read_grammar(void)
{
    mmio_mock_reset();
    seed_calendar();

    datetime_t d;
    int rc = rtc_read(&d);

    trace_cursor tc = trace_begin("rtc_read");
    expect_calendar_read(&tc);
    trace_expect_end(&tc);

    if (rc != 1 || d.year != 2026 || d.month != 9 || d.day != 16 ||
        d.hour != 10 || d.min != 42 || d.sec != 0) {
        fprintf(stderr, "[rtc_read] rc %d, %04d-%02d-%02d %02d:%02d:%02d; "
                        "expected 1, 2026-09-16 10:42:00\n",
                rc, d.year, d.month, d.day, d.hour, d.min, d.sec);
        tc.fails++;
    }
    /* The weekday register held 3 (Wednesday) and the date IS a Wednesday, so
     * agreeing proves nothing on its own — what is asserted is that the driver
     * computed it: the seeded byte is deliberately correct here and deliberately
     * wrong in the torn-read case below, where the answer must still be right. */
    if (d.wday != 3) {
        fprintf(stderr, "[rtc_read] wday %d, expected 3\n", d.wday);
        tc.fails++;
    }

    /* The HAL wrapper over the same pass, seeded afresh: the queued answers
     * above were consumed by the read under test. */
    mmio_mock_reset();
    seed_calendar();
    uint32_t epoch = 0;
    if (hal_rtc_get(&epoch) != 1 || epoch != 1789555320u) {
        fprintf(stderr, "[rtc_read] hal_rtc_get epoch %u, expected 1789555320\n",
                (unsigned)epoch);
        tc.fails++;
    }
    return trace_done(&tc);
}

/*
 * A second boundary between the two chunks. The first pass reads 23:59:59 on
 * the 16th, then the date chunk from AFTER the carry (the 17th), then sees the
 * seconds back at 00 — a whole day wrong if it were believed. The retry reads
 * a clean 2026-09-17 00:00:00, and that is the answer.
 */
static int test_torn_read(void)
{
    mmio_mock_reset();
    /* DATA0 across the six transactions: SC, DT, SC | SC, DT, SC */
    static const uint32_t d0[] = { 0x59u, 0x17u, 0x00u, 0x00u, 0x17u, 0x00u };
    static const uint32_t d1[] = { 0x59u, 0x09u, 0x00u, 0x09u };   /* MN, MT */
    static const uint32_t d2[] = { 0x23u, 0x26u, 0x00u, 0x26u };   /* HR, YR */
    static const uint32_t d3[] = { 0x06u, 0x00u };                 /* WD: wrong on purpose */
    mmio_mock_queue_read(I2C_DATA0_ADDR, d0, 6);
    mmio_mock_queue_read(I2C_DATA1_ADDR, d1, 4);
    mmio_mock_queue_read(I2C_DATA2_ADDR, d2, 4);
    mmio_mock_queue_read(I2C_DATA3_ADDR, d3, 2);

    datetime_t d;
    int rc = rtc_read(&d);

    trace_cursor tc = trace_begin("rtc_torn_read");
    expect_calendar_read(&tc);                 /* the torn pass  */
    expect_calendar_read(&tc);                 /* the retry      */
    trace_expect_end(&tc);

    if (rc != 1 || d.year != 2026 || d.month != 9 || d.day != 17 ||
        d.hour != 0 || d.min != 0 || d.sec != 0 || d.wday != 4) {
        fprintf(stderr, "[rtc_torn_read] rc %d, %04d-%02d-%02d %02d:%02d:%02d "
                        "wday %d; expected 1, 2026-09-17 00:00:00 wday 4\n",
                rc, d.year, d.month, d.day, d.hour, d.min, d.sec, d.wday);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* Every shape of "this is not a time" must read as unset — and cost exactly
 * one pass, since there is nothing a retry could fix. */
static int test_unset_shapes(void)
{
    trace_cursor tc = trace_begin("rtc_unset");
    datetime_t d;

    /* (a) The year register at its reset value: a drained cell. */
    mmio_mock_reset();
    seed_calendar();
    {
        static const uint32_t d2[] = { 0x10u, 0x00u };   /* HR 10, then YR 00 */
        mmio_mock_queue_read(I2C_DATA2_ADDR, d2, 2);
    }
    if (rtc_read(&d) != 0) {
        fprintf(stderr, "[rtc_unset] year 00 must read as unset\n");
        tc.fails++;
    }

    /* (b) Bytes that are not BCD at all — a wrong register map, or a bus
     * returning something else entirely. */
    mmio_mock_reset();
    seed_calendar();
    {
        static const uint32_t d0[] = { 0x4Au, 0x16u, 0x4Au };   /* SC = 0x4A */
        mmio_mock_queue_read(I2C_DATA0_ADDR, d0, 3);
    }
    if (rtc_read(&d) != 0) {
        fprintf(stderr, "[rtc_unset] a bad BCD nibble must read as unset\n");
        tc.fails++;
    }

    /* (c) A date that decodes cleanly but does not exist: 31 February. */
    mmio_mock_reset();
    seed_calendar();
    {
        static const uint32_t d0[] = { 0x00u, 0x31u, 0x00u };   /* DT = 31 */
        static const uint32_t d1[] = { 0x42u, 0x02u };          /* MT = 02 */
        mmio_mock_queue_read(I2C_DATA0_ADDR, d0, 3);
        mmio_mock_queue_read(I2C_DATA1_ADDR, d1, 2);
    }
    if (rtc_read(&d) != 0) {
        fprintf(stderr, "[rtc_unset] 31 February must read as unset\n");
        tc.fails++;
    }

    /*
     * (d) THE NACK SHAPE. i2c_read cannot see a missing ack: it waits for the
     * controller to go idle and latches whatever the DATA registers hold,
     * which after a pointer write is the pointer byte itself in DATA0 and
     * whatever was last written in DATA1..3. So an absent PMU "answers"
     * SC = 0x0A (the RTCSC pointer) — not a BCD digit — and the year comes
     * back as a stale byte. It must be unset, and it must NOT be three
     * retries: an unanswered bus is not a torn read.
     */
    mmio_mock_reset();
    {
        static const uint32_t d0[] = { 0x0Au, 0x0Eu, 0x0Au };
        mmio_mock_queue_read(I2C_DATA0_ADDR, d0, 3);
        mmio_mock_set_read(I2C_DATA1_ADDR, 0x30u);
        mmio_mock_set_read(I2C_DATA2_ADDR, 0x05u);
        mmio_mock_set_read(I2C_DATA3_ADDR, 0x00u);
    }
    uint32_t epoch = 0xDEADBEEFu;
    if (rtc_read(&d) != 0 || hal_rtc_get(&epoch) != 0 || epoch != 0xDEADBEEFu) {
        fprintf(stderr, "[rtc_unset] the NACK shape must read as unset and "
                        "leave the caller's epoch alone\n");
        tc.fails++;
    }
    {
        trace_cursor sub = trace_begin("rtc_unset_nack");
        expect_calendar_read(&sub);            /* rtc_read's pass...     */
        expect_calendar_read(&sub);            /* ...and hal_rtc_get's   */
        trace_expect_end(&sub);
        tc.fails += sub.fails;
    }
    return trace_done(&tc);
}

/* The write order is the safety argument: seconds first, so the counter
 * restarts and the next minute carry is a whole second away while the other
 * six registers are written. */
static int test_write_order(void)
{
    mmio_mock_reset();
    seed_calendar();

    datetime_t d = { 2026, 9, 16, 10, 42, 0, 0 };
    int rc = rtc_write(&d);

    trace_cursor tc = trace_begin("rtc_write");
    expect_write_reg(&tc, R_SC, 0x00u);
    expect_write_reg(&tc, R_MN, 0x42u);
    expect_write_reg(&tc, R_HR, 0x10u);
    expect_write_reg(&tc, R_WD, 0x03u);        /* Wednesday, plain binary */
    expect_write_reg(&tc, R_DT, 0x16u);
    expect_write_reg(&tc, R_MT, 0x09u);
    expect_write_reg(&tc, R_YR, 0x26u);
    expect_read(&tc, R_YR, 1);                 /* flush the lazy write path */
    trace_expect_end(&tc);

    if (rc != 0) {
        fprintf(stderr, "[rtc_write] rc %d, expected 0\n", rc);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* hal_rtc_set: the write, the flush, and the read-back compare. */
static int test_set_and_readback(void)
{
    mmio_mock_reset();
    /* DATA0 answers: YR flush (0x26), then the read-back pass SC, DT, SC. */
    {
        static const uint32_t d0[] = { 0x26u, 0x00u, 0x16u, 0x00u };
        static const uint32_t d1[] = { 0x42u, 0x09u };
        static const uint32_t d2[] = { 0x10u, 0x26u };
        mmio_mock_queue_read(I2C_DATA0_ADDR, d0, 4);
        mmio_mock_queue_read(I2C_DATA1_ADDR, d1, 2);
        mmio_mock_queue_read(I2C_DATA2_ADDR, d2, 2);
        mmio_mock_set_read(I2C_DATA3_ADDR, 0x03u);
    }

    int rc = hal_rtc_set(1789555320u);         /* 2026-09-16 10:42:00Z */

    trace_cursor tc = trace_begin("hal_rtc_set");
    expect_write_reg(&tc, R_SC, 0x00u);
    expect_write_reg(&tc, R_MN, 0x42u);
    expect_write_reg(&tc, R_HR, 0x10u);
    expect_write_reg(&tc, R_WD, 0x03u);
    expect_write_reg(&tc, R_DT, 0x16u);
    expect_write_reg(&tc, R_MT, 0x09u);
    expect_write_reg(&tc, R_YR, 0x26u);
    expect_read(&tc, R_YR, 1);
    expect_calendar_read(&tc);                 /* the read-back */
    trace_expect_end(&tc);

    if (rc != 0) {
        fprintf(stderr, "[hal_rtc_set] rc %d, expected 0\n", rc);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* A chip that did not take the write reports -2 — the editor then shows the
 * old time and says so, rather than claiming a clock it does not have. */
static int test_readback_mismatch(void)
{
    trace_cursor tc = trace_begin("hal_rtc_set_readback");

    /* The clock reads a minute earlier than what was written. */
    mmio_mock_reset();
    {
        static const uint32_t d0[] = { 0x26u, 0x00u, 0x16u, 0x00u };
        static const uint32_t d1[] = { 0x41u, 0x09u };      /* MN = 41, not 42 */
        static const uint32_t d2[] = { 0x10u, 0x26u };
        mmio_mock_queue_read(I2C_DATA0_ADDR, d0, 4);
        mmio_mock_queue_read(I2C_DATA1_ADDR, d1, 2);
        mmio_mock_queue_read(I2C_DATA2_ADDR, d2, 2);
        mmio_mock_set_read(I2C_DATA3_ADDR, 0x03u);
    }
    int rc = hal_rtc_set(1789555320u);
    if (rc != -2) {
        fprintf(stderr, "[hal_rtc_set_readback] rc %d for a disagreeing "
                        "read-back, expected -2\n", rc);
        tc.fails++;
    }

    /* One second later is agreement: the write takes a few hundred µs and the
     * seconds may carry while we look. */
    mmio_mock_reset();
    {
        static const uint32_t d0[] = { 0x26u, 0x01u, 0x16u, 0x01u };  /* SC 01 */
        static const uint32_t d1[] = { 0x42u, 0x09u };
        static const uint32_t d2[] = { 0x10u, 0x26u };
        mmio_mock_queue_read(I2C_DATA0_ADDR, d0, 4);
        mmio_mock_queue_read(I2C_DATA1_ADDR, d1, 2);
        mmio_mock_queue_read(I2C_DATA2_ADDR, d2, 2);
        mmio_mock_set_read(I2C_DATA3_ADDR, 0x03u);
    }
    if (hal_rtc_set(1789555320u) != 0) {
        fprintf(stderr, "[hal_rtc_set_readback] a one-second carry must be "
                        "agreement\n");
        tc.fails++;
    }

    /* A read-back that is not a date at all is also a failed set. */
    mmio_mock_reset();
    {
        static const uint32_t d0[] = { 0x26u, 0x4Au, 0x16u, 0x4Au };
        mmio_mock_queue_read(I2C_DATA0_ADDR, d0, 4);
        mmio_mock_set_read(I2C_DATA1_ADDR, 0x42u);
        mmio_mock_set_read(I2C_DATA2_ADDR, 0x10u);
        mmio_mock_set_read(I2C_DATA3_ADDR, 0x03u);
    }
    if (hal_rtc_set(1789555320u) != -2) {
        fprintf(stderr, "[hal_rtc_set_readback] an unreadable read-back must "
                        "be -2\n");
        tc.fails++;
    }
    return trace_done(&tc);
}

/* Out of range: refused before a single bus cycle. */
static int test_range_refusal(void)
{
    trace_cursor tc = trace_begin("hal_rtc_set_range");

    mmio_mock_reset();
    int lo = hal_rtc_set(946684800u);          /* 2000-01-01: the reset year */
    size_t quiet_lo = mmio_mock_log_len();

    mmio_mock_reset();
    int hi = hal_rtc_set(4102444800u);         /* 2100-01-01 */
    size_t quiet_hi = mmio_mock_log_len();

    if (lo != -3 || hi != -3 || quiet_lo != 0 || quiet_hi != 0) {
        fprintf(stderr, "[hal_rtc_set_range] rc %d/%d, %u/%u bus events; "
                        "expected -3/-3 and silence\n",
                lo, hi, (unsigned)quiet_lo, (unsigned)quiet_hi);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* A wedged controller (BUSY never clears) fails both directions with -1 and
 * latches nothing. */
static int test_wedged_bus(void)
{
    trace_cursor tc = trace_begin("rtc_wedged_bus");

    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, I2C_BUSY);
    uint32_t epoch = 0xA5A5A5A5u;
    int get = hal_rtc_get(&epoch);
    size_t latched = mmio_mock_count(MMIO_OP_READ, I2C_DATA0_ADDR);

    if (get != -1 || epoch != 0xA5A5A5A5u || latched != 0) {
        fprintf(stderr, "[rtc_wedged_bus] get %d, epoch %08X, %u DATA reads; "
                        "expected -1, untouched, 0\n",
                get, (unsigned)epoch, (unsigned)latched);
        tc.fails++;
    }

    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, I2C_BUSY);
    if (hal_rtc_set(1789555320u) != -1) {
        fprintf(stderr, "[rtc_wedged_bus] set must report -1 on a dead bus\n");
        tc.fails++;
    }
    return trace_done(&tc);
}

int main(void)
{
    int fails = 0;
    fails |= test_read_grammar();
    fails |= test_torn_read();
    fails |= test_unset_shapes();
    fails |= test_write_order();
    fails |= test_set_and_readback();
    fails |= test_readback_mismatch();
    fails |= test_range_refusal();
    fails |= test_wedged_bus();
    return fails;
}
