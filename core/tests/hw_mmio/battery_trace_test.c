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

/*
 * battery_sample(): one conversion, and the numbers calibration needs.
 *
 * Two things are asserted that nothing could assert before. First that a
 * sample costs exactly ONE channel-select + result read — battery_millivolts()
 * followed by battery_percent() ran two conversions and returned two different
 * samples, and the main loop was doing exactly that every 5 s. Counting the
 * ADCC1 writes is the honest way to pin it.
 *
 * Second that mv_raw is the UNCLAMPED conversion. The plausibility clamp is
 * what makes a flat cell and an I2C glitch look identical (both land on 3300),
 * so the unclamped value is the only way a caller can tell them apart.
 */
static int test_battery_sample(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(I2C_DATA0_ADDR, 0xAA);
    mmio_mock_set_read(I2C_DATA1_ADDR, 0xFE);

    battery_sample_t bs;
    int rc = battery_sample(&bs);
    trace_cursor tc = trace_begin("battery_sample");

    if (rc != 0 || bs.raw != 682 || bs.mv_raw != 3996 || bs.mv != 3996) {
        fprintf(stderr, "[battery_sample] rc %d raw %d mv_raw %d mv %d; "
                        "expected 0/682/3996/3996\n",
                rc, bs.raw, bs.mv_raw, bs.mv);
        tc.fails++;
    }
    /* Exactly one conversion: ADCC1's value byte is written once. */
    /* DATA1 carries the ADCC1 value byte (channel | start) and nothing else in
     * this sequence — the pointer-read phase writes only DATA0. So one write
     * of DATA1 == one conversion; two would mean the old double-read is back. */
    size_t starts = mmio_mock_count(MMIO_OP_WRITE, I2C_DATA1_ADDR);
    if (starts != 1) {
        fprintf(stderr, "[battery_sample] expected exactly one conversion "
                        "(1 DATA1 write), saw %u\n", (unsigned)starts);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* A SMALL excursion outside the cell's operating band — inside the reject band
 * but under the 3300 mV floor, or over the 4200 mV ceiling — is clamped: mv
 * moves, mv_raw does not, so "flat cell under load" and "at the floor" stay
 * distinguishable to a caller that looks. */
static int test_battery_sample_clamp(void)
{
    trace_cursor tc = trace_begin("battery_sample_clamp");
    battery_sample_t bs;
    int rc;

    /* raw = (0x87<<2)|0 = 540 -> 540*6000>>10 = 3164 mV: under the floor,
     * inside the reject band. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_DATA0_ADDR, 0x87);
    mmio_mock_set_read(I2C_DATA1_ADDR, 0x00);
    rc = battery_sample(&bs);
    if (rc != 0 || bs.raw != 540 || bs.mv_raw != 3164 || bs.mv != 3300) {
        fprintf(stderr, "[%s] low: rc %d raw %d mv_raw %d mv %d; "
                        "expected 0/540/3164/3300\n",
                tc.name, rc, bs.raw, bs.mv_raw, bs.mv);
        tc.fails++;
    }

    /* raw = (0xB9<<2)|0 = 740 -> 4335 mV: over the ceiling (a cell being
     * driven by the charger), inside the reject band. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_DATA0_ADDR, 0xB9);
    mmio_mock_set_read(I2C_DATA1_ADDR, 0x00);
    rc = battery_sample(&bs);
    if (rc != 0 || bs.raw != 740 || bs.mv_raw != 4335 || bs.mv != 4200) {
        fprintf(stderr, "[%s] high: rc %d raw %d mv_raw %d mv %d; "
                        "expected 0/740/4335/4200\n",
                tc.name, rc, bs.raw, bs.mv_raw, bs.mv);
        tc.fails++;
    }
    return trace_done(&tc);
}

/*
 * THE NACK SHAPES. i2c_read() cannot see a missing ack: it waits for the
 * controller to go idle and latches whatever I2C_DATA0/1 hold. When the PMU
 * does not answer the result read those registers still hold the bytes the
 * driver itself last WROTE through them — DATA0 = 0x30 (the ADCS1 register
 * pointer) and DATA1 = 0x05 (the ADCC1 start byte from the select write) —
 * which assembles to raw 0xC1 = 1130 mV. Before this fix that clamped to the
 * 3300 mV floor, i.e. the shutoff line, and three such reads inside 15 s
 * carried the policy through DISKSAFE to a power-off on a healthy cell. The
 * inverse (a floating bus, 0xFF/0xFF -> 0x3FF = 5994 mV) clamped to 4200 and
 * hid a flat battery behind a full one. Both must be FAILED reads: -1
 * everywhere, so they enter nothing anywhere.
 *
 * The mock is seeded with exactly the register-file residue a NACKed read
 * leaves — the value it returns for DATA0/DATA1 is the value the driver wrote
 * there one transaction earlier.
 */
static int test_battery_sample_nack_residue_is_failure(void)
{
    trace_cursor tc = trace_begin("battery_sample_nack_residue_is_failure");
    battery_sample_t bs;
    int rc;

    /* What the register file holds after the pointer write: 0x30 / 0x05. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_DATA0_ADDR, 0x30);
    mmio_mock_set_read(I2C_DATA1_ADDR, 0x05);
    rc = battery_sample(&bs);
    if (rc == 0 || bs.raw != -1 || bs.mv_raw != -1 || bs.mv != -1) {
        fprintf(stderr, "[%s] residue 0x30/0x05 (1130 mV): rc %d raw %d "
                        "mv_raw %d mv %d; expected -1 everywhere\n",
                tc.name, rc, bs.raw, bs.mv_raw, bs.mv);
        tc.fails++;
    }
    if (battery_millivolts() != -1 || battery_percent() != -1) {
        fprintf(stderr, "[%s] wrappers returned a value for the residue\n",
                tc.name);
        tc.fails++;
    }

    /* A bus floating high: 0x3FF -> 5994 mV. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_DATA0_ADDR, 0xFF);
    mmio_mock_set_read(I2C_DATA1_ADDR, 0xFF);
    rc = battery_sample(&bs);
    if (rc == 0 || bs.raw != -1 || bs.mv_raw != -1 || bs.mv != -1) {
        fprintf(stderr, "[%s] floating 0x3FF (5994 mV): rc %d raw %d "
                        "mv_raw %d mv %d; expected -1 everywhere\n",
                tc.name, rc, bs.raw, bs.mv_raw, bs.mv);
        tc.fails++;
    }
    if (battery_millivolts() != -1 || battery_percent() != -1) {
        fprintf(stderr, "[%s] wrappers returned a value for a floating bus\n",
                tc.name);
        tc.fails++;
    }

    /* The band edges themselves, so the reject thresholds are pinned:
     * raw 478 -> 2800 mV is the lowest accepted (clamps to 3300),
     * raw 477 -> 2794 mV is rejected;
     * raw 785 -> 4599 mV is the highest accepted (clamps to 4200),
     * raw 786 -> 4605 mV is rejected. */
    struct { int d0, d1, want_rc, want_mv; const char *what; } edges[] = {
        { 0x77, 0x02,  0, 3300, "2800 mV accepted"  },   /* 478 */
        { 0x77, 0x01, -1,   -1, "2794 mV rejected"  },   /* 477 */
        { 0xC4, 0x01,  0, 4200, "4599 mV accepted"  },   /* 785 */
        { 0xC4, 0x02, -1,   -1, "4605 mV rejected"  },   /* 786 */
    };
    for (unsigned i = 0; i < sizeof edges / sizeof edges[0]; i++) {
        mmio_mock_reset();
        mmio_mock_set_read(I2C_DATA0_ADDR, (uint32_t)edges[i].d0);
        mmio_mock_set_read(I2C_DATA1_ADDR, (uint32_t)edges[i].d1);
        rc = battery_sample(&bs);
        if (rc != edges[i].want_rc || bs.mv != edges[i].want_mv) {
            fprintf(stderr, "[%s] %s: rc %d mv %d (mv_raw %d)\n",
                    tc.name, edges[i].what, rc, bs.mv, bs.mv_raw);
            tc.fails++;
        }
    }
    return trace_done(&tc);
}

/*
 * An all-zero ADC result is a failed read. i2c_read() only waits for the
 * controller to go idle — it never checks that the PMU acked — so a transfer
 * the PMU did not answer can return zeros and report success. Raw 0 used to
 * clamp to the 3300 mV floor, i.e. the shutoff line, so three of them in a
 * row would walk the policy from OK to power-off on a healthy cell. The
 * driver must reject it, and the policy must then see -1 and hold still.
 */
static int test_battery_sample_zero_is_failure(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(I2C_DATA0_ADDR, 0x00);
    mmio_mock_set_read(I2C_DATA1_ADDR, 0x00);

    battery_sample_t bs;
    int rc = battery_sample(&bs);
    trace_cursor tc = trace_begin("battery_sample_zero_is_failure");
    if (rc == 0 || bs.raw != -1 || bs.mv_raw != -1 || bs.mv != -1) {
        fprintf(stderr, "[%s] rc %d raw %d mv_raw %d mv %d; expected -1 everywhere\n",
                tc.name, rc, bs.raw, bs.mv_raw, bs.mv);
        tc.fails++;
    }
    /* The two wrappers must agree: no caller gets a 3300 out of this. */
    if (battery_millivolts() != -1 || battery_percent() != -1) {
        fprintf(stderr, "[%s] wrappers returned a value for a zero read\n",
                tc.name);
        tc.fails++;
    }
    return trace_done(&tc);
}

/* The curve as a pure function: same answer as battery_percent(), with no bus
 * traffic at all — which is what lets a caller filter several samples and
 * convert once. */
/* battery_percent_charging(): removes the charge lift, tapers it out at the
 * ceiling, never exceeds the resting-curve answer, passes -1 through. */
static int test_battery_percent_charging(void)
{
    int fails = 0;
    mmio_mock_reset();
    /* 500 mA -> 150 mV lift: 3990 on the charger reads like 3840 resting (50%),
     * where the plain curve would have said ~74%. */
    int p500 = battery_percent_charging(3990, 500);
    int plain = battery_percent_from_mv(3990);
    if (!(p500 == battery_percent_from_mv(3840) && p500 < plain)) {
        fprintf(stderr, "[battery_percent_charging] 3990@500mA: got %d, plain %d\n", p500, plain);
        fails++;
    }
    /* 100 mA -> 30 mV: a much smaller correction. */
    int p100 = battery_percent_charging(3990, 100);
    if (!(p100 == battery_percent_from_mv(3960) && p100 > p500)) {
        fprintf(stderr, "[battery_percent_charging] 3990@100mA: got %d\n", p100);
        fails++;
    }
    /* At the ceiling the lift is gone: 4200 is 100% either way. */
    if (battery_percent_charging(4200, 500) != 100) {
        fprintf(stderr, "[battery_percent_charging] 4200 should be 100\n");
        fails++;
    }
    /* Half-way into the taper the lift is halved: 4125 @500 -> 75 mV off. */
    if (battery_percent_charging(4125, 500) != battery_percent_from_mv(4050)) {
        fprintf(stderr, "[battery_percent_charging] taper at 4125: got %d\n",
                battery_percent_charging(4125, 500));
        fails++;
    }
    /* Never above the resting answer; -1 passes through; no bus traffic. */
    for (int mv = 3300; mv <= 4200; mv += 25) {
        if (battery_percent_charging(mv, 500) > battery_percent_from_mv(mv)) {
            fprintf(stderr, "[battery_percent_charging] %d exceeds resting\n", mv);
            fails++;
            break;
        }
    }
    if (battery_percent_charging(-1, 500) != -1 || mmio_mock_log_len() != 0) {
        fprintf(stderr, "[battery_percent_charging] -1 or bus traffic\n");
        fails++;
    }
    if (!fails) printf("[battery_percent_charging] PASS\n");
    return fails;
}

static int test_battery_percent_from_mv(void)
{
    mmio_mock_reset();
    trace_cursor tc = trace_begin("battery_percent_from_mv");

    struct { int mv, pct; } cases[] = {
        { 3500, 0 },      /* below the 0% point                */
        { 3600, 0 },      /* exactly the 0% point              */
        { 3996, 77 },     /* the seeded sample above           */
        { 4180, 100 },    /* exactly the 100% point            */
        { 4300, 100 },    /* above it                          */
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int got = battery_percent_from_mv(cases[i].mv);
        if (got != cases[i].pct) {
            fprintf(stderr, "[battery_percent_from_mv] %d mV: expected %d%%, "
                            "got %d%%\n", cases[i].mv, cases[i].pct, got);
            tc.fails++;
        }
    }
    if (mmio_mock_log_len() != 0) {
        fprintf(stderr, "[battery_percent_from_mv] touched the bus (%u events)"
                        "; it must be pure\n", (unsigned)mmio_mock_log_len());
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

/* =====================================================================
 * Low-battery policy (battery.h, "Low-battery policy").
 *
 * The policy is a pure state machine over the clamped mv that battery_sample()
 * produces, so these cases feed it directly and assert the EVENT it returns on
 * every sample — the event is what kernel/main.c acts on (flush + park at
 * DISKSAFE, power off at SHUTOFF), so "which sample fires" is exactly the
 * behaviour under test. One case drives a real battery_sample() failure
 * through the mock bus to prove the -1 route composes end to end.
 *
 * Every number below is hand-derived from the constants in battery.h at the
 * 5 s cadence main.c samples on: BATTERY_FILTER_N = 5 (median),
 * BATTERY_SHUTOFF_CONFIRM = 3, lines at 3500 / 3300, recovery at 3600.
 *
 * MUTATION CHECK (2026-09-09): each case was confirmed to FAIL against a
 * deliberately broken policy — see the commit message for the list.
 * ===================================================================== */

/* power_is_external() as seen by the policy for every feed below. Cases run on
 * battery (0) unless they are testing the charger gate. */
static int g_ext;

/* Feed `mv` `n` times, asserting NONE from every feed except the last, which
 * must return `last`. Returns the number of mismatches. */
static int feed_expect(trace_cursor *tc, int mv, int n, battery_event_t last)
{
    for (int i = 0; i < n; i++) {
        battery_event_t want = (i == n - 1) ? last : BATTERY_EVENT_NONE;
        battery_event_t got  = battery_policy_feed(mv, g_ext);
        if (got != want) {
            fprintf(stderr, "[%s] feed #%d of %d mV: expected event %d, got %d "
                            "(filtered %d)\n",
                    tc->name, i + 1, mv, (int)want, (int)got,
                    battery_filtered_mv());
            tc->fails++;
            return 1;
        }
    }
    return 0;
}

static void expect_level(trace_cursor *tc, battery_level_t want,
                         int writes_ok, const char *when)
{
    if (battery_policy_level() != want) {
        fprintf(stderr, "[%s] %s: expected level %d, got %d\n",
                tc->name, when, (int)want, (int)battery_policy_level());
        tc->fails++;
    }
    if (battery_disk_writes_allowed() != writes_ok) {
        fprintf(stderr, "[%s] %s: expected disk writes %s\n",
                tc->name, when, writes_ok ? "allowed" : "refused");
        tc->fails++;
    }
}

/* Prime a full ring of `mv` from reset: the policy is armed only once the
 * window holds BATTERY_FILTER_N samples, and none of those may fire. */
static int prime(trace_cursor *tc, int mv)
{
    battery_policy_reset();
    g_ext = 0;
    if (feed_expect(tc, mv, BATTERY_FILTER_N, BATTERY_EVENT_NONE)) {
        return 1;
    }
    if (!battery_filter_ready() || battery_filtered_mv() != mv) {
        fprintf(stderr, "[%s] after %d x %d mV: ready %d filtered %d\n",
                tc->name, BATTERY_FILTER_N, mv, battery_filter_ready(),
                battery_filtered_mv());
        tc->fails++;
        return 1;
    }
    return 0;
}

/*
 * THE FILTER. A healthy plateau cell (3780 mV = 30 %) with a drive spin-up
 * landing on one sample, then on two consecutive samples, then a garbage
 * clamp-floor sample: the filtered value must not move at all, and no
 * threshold may fire. 3350 is a deep sag — 430 mV, well past the "low
 * hundreds" a 1.8" HDD produces — and it sits BELOW the disk-safe line, so an
 * unfiltered policy (or an EMA of any useful alpha) would trip here.
 */
static int test_policy_sag_ignored(void)
{
    trace_cursor tc = trace_begin("policy_sag_ignored");
    mmio_mock_reset();

    /* Empty ring reads -1, not a plausible-looking 0 or 3300. */
    battery_policy_reset();
    if (battery_filtered_mv() != -1 || battery_filter_ready()) {
        fprintf(stderr, "[%s] empty ring: filtered %d ready %d\n", tc.name,
                battery_filtered_mv(), battery_filter_ready());
        tc.fails++;
    }

    if (prime(&tc, 3780)) {
        return trace_done(&tc);
    }
    /* One sag. */
    feed_expect(&tc, 3350, 1, BATTERY_EVENT_NONE);
    if (battery_filtered_mv() != 3780) {
        fprintf(stderr, "[%s] one sag moved the median to %d\n", tc.name,
                battery_filtered_mv());
        tc.fails++;
    }
    feed_expect(&tc, 3780, 5, BATTERY_EVENT_NONE);   /* push the sag out */
    /* Two in a row (a long refill burst). Still a minority of five. */
    feed_expect(&tc, 3350, 2, BATTERY_EVENT_NONE);
    if (battery_filtered_mv() != 3780) {
        fprintf(stderr, "[%s] two sags moved the median to %d\n", tc.name,
                battery_filtered_mv());
        tc.fails++;
    }
    feed_expect(&tc, 3780, 5, BATTERY_EVENT_NONE);   /* push them out */
    /* A clamp-floor sample (bus glitch that still ACKed): same story. */
    feed_expect(&tc, 3300, 1, BATTERY_EVENT_NONE);
    if (battery_filtered_mv() != 3780) {
        fprintf(stderr, "[%s] floor sample moved the median to %d\n", tc.name,
                battery_filtered_mv());
        tc.fails++;
    }
    expect_level(&tc, BATTERY_LEVEL_OK, 1, "after sags");

    /* The gauge follows the filter: 3780 is exactly the 30 % point. */
    if (battery_percent_from_mv(battery_filtered_mv()) != 30) {
        fprintf(stderr, "[%s] filtered percent: expected 30, got %d\n",
                tc.name, battery_percent_from_mv(battery_filtered_mv()));
        tc.fails++;
    }
    return trace_done(&tc);
}

/*
 * A GENUINE DECLINE crosses — on the sample the debounce says, not before.
 *
 * From a full 3700 ring the cell drops to 3450 (below disk-safe): the median
 * needs 3 of 5, so DISKSAFE fires on the THIRD 3450 (15 s), not the first.
 * Then it drops to the 3300 floor: the median reaches 3300 on the third floor
 * sample, and the confirm needs 3 consecutive such medians, so SHUTOFF fires
 * on the FIFTH floor sample (25 s after the first) and on no other. This is
 * the worst-case response argued in battery.c, pinned.
 */
static int test_policy_decline_crosses(void)
{
    trace_cursor tc = trace_begin("policy_decline_crosses");
    mmio_mock_reset();
    if (prime(&tc, 3700)) {
        return trace_done(&tc);
    }

    feed_expect(&tc, 3450, 3, BATTERY_EVENT_DISKSAFE);
    expect_level(&tc, BATTERY_LEVEL_DISKSAFE, 0, "after disksafe");
    if (battery_filtered_mv() != 3450) {
        fprintf(stderr, "[%s] disksafe median: expected 3450, got %d\n",
                tc.name, battery_filtered_mv());
        tc.fails++;
    }

    feed_expect(&tc, 3300, 5, BATTERY_EVENT_SHUTOFF);
    expect_level(&tc, BATTERY_LEVEL_SHUTOFF, 0, "after shutoff");

    /* Terminal: the device is powering off; nothing else may fire. */
    feed_expect(&tc, 3300, 3, BATTERY_EVENT_NONE);
    feed_expect(&tc, 4100, 6, BATTERY_EVENT_NONE);
    expect_level(&tc, BATTERY_LEVEL_SHUTOFF, 0, "terminal");
    return trace_done(&tc);
}

/*
 * DISKSAFE fires ONCE, and always BEFORE SHUTOFF — even on a cell that is
 * already at the floor when the policy arms (boot on a flat battery). The
 * fifth sample arms the policy and must produce DISKSAFE, so main.c gets to
 * flush and park; SHUTOFF follows only after the confirm window (samples 6, 7).
 * Sitting in DISKSAFE for a long time must not re-fire it.
 */
static int test_policy_disksafe_once_first(void)
{
    trace_cursor tc = trace_begin("policy_disksafe_once_first");
    mmio_mock_reset();

    battery_policy_reset();
    g_ext = 0;
    /* Not armed: four floor samples, no event, writes still allowed. */
    feed_expect(&tc, 3300, 4, BATTERY_EVENT_NONE);
    expect_level(&tc, BATTERY_LEVEL_OK, 1, "unarmed");
    /* Fifth arms it: the first stage, never the second. */
    feed_expect(&tc, 3300, 1, BATTERY_EVENT_DISKSAFE);
    expect_level(&tc, BATTERY_LEVEL_DISKSAFE, 0, "armed at floor");
    /* Confirm window: 2 more evaluations at the floor. */
    feed_expect(&tc, 3300, 2, BATTERY_EVENT_SHUTOFF);

    /* Once: park at 3450 and hover there for two minutes of samples. */
    if (prime(&tc, 3700)) {
        return trace_done(&tc);
    }
    feed_expect(&tc, 3450, 3, BATTERY_EVENT_DISKSAFE);
    int disksafe_events = 0, other_events = 0;
    for (int i = 0; i < 24; i++) {
        battery_event_t ev = battery_policy_feed((i & 1) ? 3450 : 3520, 0);
        if (ev == BATTERY_EVENT_DISKSAFE) {
            disksafe_events++;
        } else if (ev != BATTERY_EVENT_NONE) {
            other_events++;
        }
    }
    if (disksafe_events != 0 || other_events != 0) {
        fprintf(stderr, "[%s] hovering in DISKSAFE re-fired: %d disksafe, %d "
                        "other events\n", tc.name, disksafe_events,
                other_events);
        tc.fails++;
    }
    expect_level(&tc, BATTERY_LEVEL_DISKSAFE, 0, "hovering");
    return trace_done(&tc);
}

/*
 * BUS FAILURE IS NOT A FLAT BATTERY. A failed sample must enter nothing into
 * the ring (so the median cannot be dragged down by a -1 or a 0), must count
 * neither for nor against the debounce, and must never move the level in
 * EITHER direction — a flaky bus cannot power the device off, and cannot
 * clear a genuine DISKSAFE either.
 *
 * The last leg runs the real driver: the mock's I2C STATUS register reads
 * BUSY forever, i2c_wait_idle() gives up, battery_sample() returns -1, and
 * feeding that result (exactly as main.c does) changes nothing.
 */
static int test_policy_i2c_failure_inert(void)
{
    trace_cursor tc = trace_begin("policy_i2c_failure_inert");
    mmio_mock_reset();
    if (prime(&tc, 3700)) {
        return trace_done(&tc);
    }

    /* Ten straight failures on a healthy cell: no movement. */
    feed_expect(&tc, -1, 10, BATTERY_EVENT_NONE);
    if (battery_filtered_mv() != 3700 || !battery_filter_ready()) {
        fprintf(stderr, "[%s] failures corrupted the filter: %d (ready %d)\n",
                tc.name, battery_filtered_mv(), battery_filter_ready());
        tc.fails++;
    }
    expect_level(&tc, BATTERY_LEVEL_OK, 1, "after failures");

    /* Interleaved with a genuine decline: the failures are simply skipped, so
     * the third GOOD low sample still fires — no earlier, no later. */
    feed_expect(&tc, 3450, 1, BATTERY_EVENT_NONE);
    feed_expect(&tc, -1,   2, BATTERY_EVENT_NONE);
    feed_expect(&tc, 3450, 1, BATTERY_EVENT_NONE);
    feed_expect(&tc, -1,   1, BATTERY_EVENT_NONE);
    feed_expect(&tc, 3450, 1, BATTERY_EVENT_DISKSAFE);

    /* In DISKSAFE, failures can neither shut off nor recover. */
    feed_expect(&tc, -1, 10, BATTERY_EVENT_NONE);
    expect_level(&tc, BATTERY_LEVEL_DISKSAFE, 0, "failures in disksafe");

    /* And at the floor, a run of failures does not advance the confirm. */
    feed_expect(&tc, 3300, 3, BATTERY_EVENT_NONE);   /* median 3300, run 1 */
    feed_expect(&tc, -1,   5, BATTERY_EVENT_NONE);   /* would be 6 if counted */
    feed_expect(&tc, 3300, 1, BATTERY_EVENT_NONE);   /* run 2 */
    expect_level(&tc, BATTERY_LEVEL_DISKSAFE, 0, "failures at floor");

    /* End to end through the driver on a healthy ring: a run of all-zero
     * reads (a PMU that stopped answering) is as inert as a wedged bus. Three
     * of them, because three genuine floor samples is what SHUTOFF needs. */
    if (prime(&tc, 3700)) {
        return trace_done(&tc);
    }
    mmio_mock_reset();
    mmio_mock_set_read(I2C_DATA0_ADDR, 0x00);
    mmio_mock_set_read(I2C_DATA1_ADDR, 0x00);
    for (int i = 0; i < 3; i++) {
        battery_sample_t zb;
        int zrc = battery_sample(&zb);
        battery_event_t zev = battery_policy_feed(zrc == 0 ? zb.mv : -1, 0);
        if (zrc == 0 || zev != BATTERY_EVENT_NONE) {
            fprintf(stderr, "[%s] zero read %d: rc %d event %d\n",
                    tc.name, i, zrc, (int)zev);
            tc.fails++;
        }
    }
    if (battery_filtered_mv() != 3700) {
        fprintf(stderr, "[%s] zero reads moved the filter: %d\n",
                tc.name, battery_filtered_mv());
        tc.fails++;
    }
    expect_level(&tc, BATTERY_LEVEL_OK, 1, "after zero reads");

    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, I2C_BUSY);   /* bus wedged */
    battery_sample_t bs;
    int rc = battery_sample(&bs);
    if (rc == 0) {
        fprintf(stderr, "[%s] battery_sample succeeded on a wedged bus\n",
                tc.name);
        tc.fails++;
    }
    battery_event_t ev = battery_policy_feed(rc == 0 ? bs.mv : -1, 0);
    if (ev != BATTERY_EVENT_NONE || battery_filtered_mv() != 3700) {
        fprintf(stderr, "[%s] wedged-bus sample: event %d filtered %d\n",
                tc.name, (int)ev, battery_filtered_mv());
        tc.fails++;
    }
    expect_level(&tc, BATTERY_LEVEL_OK, 1, "after wedged bus");
    return trace_done(&tc);
}

/*
 * HYSTERESIS. A cell parked at 3450 that drifts up to 3550 (load released,
 * platters stopped) stays in DISKSAFE — that is below the 3600 recovery line
 * and exactly the rebound that must not thrash the drive. A charger plugged
 * in drives the terminal to 3900: RECOVERED fires on the third such sample
 * (median rule), writes are allowed again, and the state does not latch.
 * Dropping back below the disk-safe line afterwards fires DISKSAFE again —
 * the edges are per crossing, not once per boot.
 */
static int test_policy_recovers(void)
{
    trace_cursor tc = trace_begin("policy_recovers");
    mmio_mock_reset();
    if (prime(&tc, 3700)) {
        return trace_done(&tc);
    }
    feed_expect(&tc, 3450, 3, BATTERY_EVENT_DISKSAFE);

    /* Rebound inside the hysteresis band: still DISKSAFE. */
    feed_expect(&tc, 3550, 8, BATTERY_EVENT_NONE);
    expect_level(&tc, BATTERY_LEVEL_DISKSAFE, 0, "rebound below recover");

    /* Charger: three samples at 3900 make the median 3900. */
    feed_expect(&tc, 3900, 3, BATTERY_EVENT_RECOVERED);
    expect_level(&tc, BATTERY_LEVEL_OK, 1, "recovered");

    /* Unplugged again and sagging for real: a fresh DISKSAFE edge. */
    feed_expect(&tc, 3480, 3, BATTERY_EVENT_DISKSAFE);
    expect_level(&tc, BATTERY_LEVEL_DISKSAFE, 0, "second crossing");
    return trace_done(&tc);
}

/*
 * ON A CHARGER THE DESCENT IS OFF. Every downward edge exists to stop a
 * running-down cell browning the system out; with external power the charger
 * holds the rails and drives the cell UP, so neither hazard exists — and a
 * cell so flat it reads under the lines while charging is exactly the one that
 * must be left on the charger, not powered off the moment the filter fills.
 *
 * Pinned: a floor-level ring on a charger arms without DISKSAFE and never
 * reaches SHUTOFF; the confirm run does not accumulate while plugged in, so an
 * unplug starts the 15 s confirm from zero; the ring still fills (gauge);
 * RECOVERED still fires off the median with the charger attached; and the
 * gate is read per sample — unplug, and the descent resumes on the same ring.
 */
static int test_policy_external_gates_descent(void)
{
    trace_cursor tc = trace_begin("policy_external_gates_descent");
    mmio_mock_reset();

    /* Boot on a charger with a flat cell: arms, no DISKSAFE, no SHUTOFF, for
     * as long as it stays plugged in. */
    battery_policy_reset();
    g_ext = 1;
    feed_expect(&tc, 3300, 5, BATTERY_EVENT_NONE);       /* arms here      */
    feed_expect(&tc, 3300, 20, BATTERY_EVENT_NONE);      /* 100 s at floor */
    expect_level(&tc, BATTERY_LEVEL_OK, 1, "flat on charger");
    if (!battery_filter_ready() || battery_filtered_mv() != 3300) {
        fprintf(stderr, "[%s] charger stopped the ring: ready %d filtered %d\n",
                tc.name, battery_filter_ready(), battery_filtered_mv());
        tc.fails++;
    }

    /* Unplug on the same floor-level ring: DISKSAFE on the very next sample
     * (median already at the floor), then SHUTOFF only after a FULL confirm
     * window counted from the unplug — two more, because the unplug sample
     * itself was the first evaluation at the floor. Had the run accumulated
     * while plugged in, SHUTOFF would fire on the unplug sample. */
    g_ext = 0;
    feed_expect(&tc, 3300, 1, BATTERY_EVENT_DISKSAFE);
    feed_expect(&tc, 3300, 2, BATTERY_EVENT_SHUTOFF);
    expect_level(&tc, BATTERY_LEVEL_SHUTOFF, 0, "after unplug");

    /* A healthy ring, plugged in, then a genuine decline: nothing fires while
     * the charger is attached, however deep or long. */
    if (prime(&tc, 3700)) {
        return trace_done(&tc);
    }
    g_ext = 1;
    feed_expect(&tc, 3450, 10, BATTERY_EVENT_NONE);
    feed_expect(&tc, 3300, 10, BATTERY_EVENT_NONE);
    expect_level(&tc, BATTERY_LEVEL_OK, 1, "declining on charger");

    /* Plugged in while already in DISKSAFE: no SHUTOFF, and the charger
     * pushing the median over the recovery line clears it as usual. */
    if (prime(&tc, 3700)) {
        return trace_done(&tc);
    }
    feed_expect(&tc, 3450, 3, BATTERY_EVENT_DISKSAFE);
    g_ext = 1;
    feed_expect(&tc, 3300, 10, BATTERY_EVENT_NONE);
    expect_level(&tc, BATTERY_LEVEL_DISKSAFE, 0, "floor in disksafe, charger");
    feed_expect(&tc, 3900, 3, BATTERY_EVENT_RECOVERED);
    expect_level(&tc, BATTERY_LEVEL_OK, 1, "recovered on charger");
    g_ext = 0;
    return trace_done(&tc);
}

int main(void)
{
    int fails = 0;
    fails += test_battery_millivolts();
    fails += test_battery_percent();
    fails += test_battery_sample();
    fails += test_battery_sample_clamp();
    fails += test_battery_sample_zero_is_failure();
    fails += test_battery_sample_nack_residue_is_failure();
    fails += test_battery_percent_from_mv();
    fails += test_battery_percent_charging();

    /* Low-battery policy: filter, thresholds, debounce, bus failure, recovery. */
    fails += test_policy_sag_ignored();
    fails += test_policy_decline_crosses();
    fails += test_policy_disksafe_once_first();
    fails += test_policy_i2c_failure_inert();
    fails += test_policy_recovers();
    fails += test_policy_external_gates_descent();

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
