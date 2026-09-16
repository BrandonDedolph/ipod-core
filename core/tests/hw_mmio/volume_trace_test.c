/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_mmio/volume_trace_test.c — host-side tests for the WM8758
 * output-volume HAL (hal/hw/volume.c), compiled against the recording
 * mock bus (-DMMIO_MOCK).
 *
 * Three layers, none touching real MMIO:
 *   1. The PURE percent -> OUT1VOL data-word mapping
 *      (hal_volume_out1_word): monotonic gain, clamping of out-of-range
 *      input, 0% mutes, 100% is exactly 0 dB (0x39), never +6 dB.
 *   2. hal_volume_set emits the LOUT1VOL/ROUT1VOL two-write grammar over
 *      the reused i2c.c write path — VU set on the RIGHT write only — and
 *      hal_volume_get round-trips the clamped percent.
 *   3. hal_eq_set emits the DACVOL pre-cut pair followed by EQ1..EQ5, and
 *      hal_codec_restore replays exactly that. The expected
 *      words are DERIVED here from the documented field layout (gain code
 *      = 12 - dB, EQxC at bits 6:5, EQ3DMODE/EQxBW at bit 8, DACVOL 0.5 dB
 *      per step) rather than read back out of volume.c — and the two curves
 *      that matter most, flat and Bass Booster, are ALSO spelled out as
 *      literal bus bytes.
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

/* ---------------------------------------------------------------------------
 * EQ: the DACVOL pre-cut pair + EQ1..EQ5, and the restore replay.
 *
 * Expected words are BUILT here from the documented field layout, so the test
 * states the grammar independently of volume.c's arithmetic.
 * ------------------------------------------------------------------------- */

/* The register data word a band should carry. `band` is 0..4 (0 = the low
 * shelf on EQ1, 4 = the high shelf on EQ5). `dac_on` sets EQ3DMODE, which
 * lives at bit 8 of EQ1 ONLY; on bands 1..3 bit 8 is EQxBW, and band 4 has
 * no bit 8 at all. */
static uint16_t want_eq_word(int band, int gain_db, int cutoff, int narrow,
                             int dac_on)
{
    uint16_t w = (uint16_t)((((unsigned)cutoff << EQ_CUTOFF_SHIFT) &
                             EQ_CUTOFF_MASK) |
                            (((unsigned)(12 - gain_db)) & EQ_GAIN_MASK));
    if (band == 0) {
        if (dac_on) w |= EQ_DAC_MODE;
    } else if (band < 4 && narrow) {
        w |= EQ_BW_NARROW;
    }
    return w;
}

/* DAC digital volume for a pre-cut in dB: 0.5 dB per step from 0xFF = 0 dB. */
static uint16_t want_dacvol(int precut_db)
{
    return (uint16_t)(DACVOL_0DB - 2 * precut_db);
}

/* Check codec transaction `n` carried (reg, data) in the wm8758 framing:
 * byte0 = (reg << 1) | data bit 8, byte1 = data low 8. */
static int check_write(const char *label, size_t n, uint8_t reg, uint16_t data)
{
    uint32_t b0 = nth_write(I2C_DATA0_ADDR, n);
    uint32_t b1 = nth_write(I2C_DATA1_ADDR, n);
    uint32_t w0 = (uint32_t)(((unsigned)reg << 1) | ((data >> 8) & 1u));
    uint32_t w1 = (uint32_t)(data & 0xFFu);
    int ok = (b0 == w0 && b1 == w1);
    if (!ok) {
        printf("  write %u: got %02x %02x, want %02x %02x (reg %02x data %03x)\n",
               (unsigned)n, b0, b1, w0, w1, reg, data);
    }
    return check(label, ok);
}

static void eq_bus_reset(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0);
    mmio_mock_set_read(I2C_CTRL_ADDR,   0);
}

/*
 * Put the driver back to the flat curve — which also drives its idea of the
 * codec's pre-cut to 0 dB — and clear the bus log. The write ORDER depends on
 * that carried-over state, so every case below states where it starts from
 * instead of inheriting whatever the previous one left.
 */
static void eq_reset_to_flat(void)
{
    const int8_t  g[5] = { 0, 0, 0, 0, 0 };
    const uint8_t c[5] = { 1, 0, 0, 0, 1 };
    const uint8_t n[5] = { 0, 0, 0, 0, 0 };
    eq_bus_reset();
    hal_eq_set(g, c, n);
    eq_bus_reset();
}

/*
 * Assert the seven writes hal_eq_set owes, starting at transaction `base`:
 * the DACVOL pair (left, then right with DACVU) and EQ1..EQ5 in order.
 *
 * `prev_precut` is the pre-cut the codec held going in, which is what decides
 * WHICH ORDER those two groups come in — the attenuation may never be smaller
 * than the boost that is live, so a curve that needs more headroom cuts the
 * DAC first and one that needs less takes the gains off first.
 */
static int check_eq_burst(const char *what, size_t base, int prev_precut,
                          const int8_t gain[5], const uint8_t cutoff[5],
                          const uint8_t narrow[5])
{
    int fails = 0;
    int precut = 0, dac_on = 0;
    for (int b = 0; b < 5; b++) {
        if (gain[b] > precut) precut = gain[b];
        if (gain[b] != 0)     dac_on = 1;
    }
    static const uint8_t REG[5] = { WM_EQ1, WM_EQ2, WM_EQ3, WM_EQ4, WM_EQ5 };
    char label[64];

    int    dac_first = (precut >= prev_precut);
    size_t dac_at    = dac_first ? base     : base + 5;
    size_t eq_at     = dac_first ? base + 2 : base;

    snprintf(label, sizeof label, "%s: LDACVOL %s", what,
             dac_first ? "before the gains" : "after the gains");
    fails += check_write(label, dac_at + 0, WM_LDACVOL, want_dacvol(precut));
    snprintf(label, sizeof label, "%s: RDACVOL + DACVU", what);
    fails += check_write(label, dac_at + 1, WM_RDACVOL,
                         (uint16_t)(want_dacvol(precut) | DACVOL_DACVU));

    for (int b = 0; b < 5; b++) {
        snprintf(label, sizeof label, "%s: EQ%d", what, b + 1);
        fails += check_write(label, eq_at + (size_t)b, REG[b],
                             want_eq_word(b, gain[b], cutoff[b], narrow[b],
                                          dac_on));
    }
    return fails;
}

/*
 * Case 7: the REGRESSION PIN. A flat curve on the tone control's corners must
 * emit exactly what this firmware emitted before presets existed — the
 * DACVOL pair wm8758_init's own sequence writes plus the five EQ words the
 * flat tone control emitted, EQ3DMODE clear so the EQ stays on the inert ADC
 * path.
 * Spelled out as literal bytes here, because "unchanged" is the claim.
 */
static int test_eq_flat_is_the_shipped_tone_path(void)
{
    int fails = 0;
    const int8_t  g[5] = { 0, 0, 0, 0, 0 };
    const uint8_t c[5] = { 1, 0, 0, 0, 1 };
    const uint8_t n[5] = { 0, 0, 0, 0, 0 };

    eq_reset_to_flat();
    hal_eq_set(g, c, n);

    fails += check("eq flat: exactly seven codec writes",
                   count_writes(I2C_ADDR_ADDR) == 7);
    /* LDACVOL 0x0B -> 0x16 0xFF; RDACVOL 0x0C with DACVU -> 0x19 0xFF. */
    fails += check("eq flat: DACVOL pair is full scale",
                   nth_write(I2C_DATA0_ADDR, 0) == 0x16 &&
                   nth_write(I2C_DATA1_ADDR, 0) == 0xFF &&
                   nth_write(I2C_DATA0_ADDR, 1) == 0x19 &&
                   nth_write(I2C_DATA1_ADDR, 1) == 0xFF);
    /* EQ1..EQ5 = 0x02C, 0x00C, 0x00C, 0x00C, 0x02C. */
    fails += check("eq flat: EQ1..EQ5 are the shipped tone words",
                   nth_write(I2C_DATA0_ADDR, 2) == 0x24 &&
                   nth_write(I2C_DATA1_ADDR, 2) == 0x2C &&
                   nth_write(I2C_DATA0_ADDR, 3) == 0x26 &&
                   nth_write(I2C_DATA1_ADDR, 3) == 0x0C &&
                   nth_write(I2C_DATA0_ADDR, 4) == 0x28 &&
                   nth_write(I2C_DATA1_ADDR, 4) == 0x0C &&
                   nth_write(I2C_DATA0_ADDR, 5) == 0x2A &&
                   nth_write(I2C_DATA1_ADDR, 5) == 0x0C &&
                   nth_write(I2C_DATA0_ADDR, 6) == 0x2C &&
                   nth_write(I2C_DATA1_ADDR, 6) == 0x2C);
    fails += check_eq_burst("eq flat", 0, 0 /*from flat*/, g, c, n);
    return fails;
}

/*
 * Case 8: a real preset. Bass Booster's curve (ui/eq.c preset 2: +6 / +3 / 0
 * / 0 / 0 on the default centres) resolves to EQ1 0x126, EQ2 0x029, EQ3/EQ4
 * 0x04C and EQ5 0x02C, behind a 6 dB pre-cut (0xF3). Spelled literally for
 * the same reason as the flat case: these bytes are the feature.
 */
static int test_eq_preset_words(void)
{
    int fails = 0;
    const int8_t  g[5] = { 6, 3, 0, 0, 0 };     /* Bass Booster */
    const uint8_t c[5] = { 1, 1, 2, 2, 1 };     /* the default centres */
    const uint8_t n[5] = { 0, 0, 0, 0, 0 };

    eq_reset_to_flat();                         /* from no pre-cut at all */
    hal_eq_set(g, c, n);

    fails += check("eq preset: seven codec writes",
                   count_writes(I2C_ADDR_ADDR) == 7);
    fails += check("eq preset: 6 dB pre-cut on both DACVOL writes",
                   nth_write(I2C_DATA0_ADDR, 0) == 0x16 &&
                   nth_write(I2C_DATA1_ADDR, 0) == 0xF3 &&
                   nth_write(I2C_DATA0_ADDR, 1) == 0x19 &&
                   nth_write(I2C_DATA1_ADDR, 1) == 0xF3);
    fails += check("eq preset: Bass Booster resolves to the documented words",
                   nth_write(I2C_DATA0_ADDR, 2) == 0x25 &&
                   nth_write(I2C_DATA1_ADDR, 2) == 0x26 &&
                   nth_write(I2C_DATA0_ADDR, 3) == 0x26 &&
                   nth_write(I2C_DATA1_ADDR, 3) == 0x29 &&
                   nth_write(I2C_DATA0_ADDR, 4) == 0x28 &&
                   nth_write(I2C_DATA1_ADDR, 4) == 0x4C &&
                   nth_write(I2C_DATA0_ADDR, 5) == 0x2A &&
                   nth_write(I2C_DATA1_ADDR, 5) == 0x4C &&
                   nth_write(I2C_DATA0_ADDR, 6) == 0x2C &&
                   nth_write(I2C_DATA1_ADDR, 6) == 0x2C);
    fails += check_eq_burst("eq preset", 0, 0 /*from flat*/, g, c, n);
    return fails;
}

/* Case 9: out-of-range gains clamp to the +/-12 dB the field can express, a
 * cut-only curve takes no pre-cut, and EQ3DMODE is set if and only if some
 * band is non-zero. */
static int test_eq_clamp_and_path_select(void)
{
    int fails = 0;
    const uint8_t c[5] = { 1, 1, 2, 2, 1 };
    const uint8_t n[5] = { 0, 0, 0, 0, 0 };

    const int8_t over[5] = { 99, -99, 0, 0, 0 };
    eq_reset_to_flat();
    hal_eq_set(over, c, n);
    fails += check("eq clamp: +99 dB -> code 0x00 (+12 dB)",
                   (nth_write(I2C_DATA1_ADDR, 2) & EQ_GAIN_MASK) == 0x00);
    fails += check("eq clamp: -99 dB -> code 0x18 (-12 dB)",
                   (nth_write(I2C_DATA1_ADDR, 3) & EQ_GAIN_MASK) == 0x18);
    fails += check("eq clamp: pre-cut follows the CLAMPED boost (12 dB)",
                   nth_write(I2C_DATA1_ADDR, 0) == want_dacvol(12));

    const int8_t cut[5] = { -6, -3, 0, 0, 0 };   /* Bass Reducer */
    eq_reset_to_flat();
    hal_eq_set(cut, c, n);
    fails += check("eq cut-only: no pre-cut",
                   nth_write(I2C_DATA1_ADDR, 0) == DACVOL_0DB);
    fails += check("eq cut-only: EQ3DMODE set (the curve is doing something)",
                   (nth_write(I2C_DATA0_ADDR, 2) & 1u) == 1u);

    const int8_t flat[5] = { 0, 0, 0, 0, 0 };
    eq_reset_to_flat();
    hal_eq_set(flat, c, n);
    fails += check("eq flat: EQ3DMODE clear (EQ stays on the ADC path)",
                   (nth_write(I2C_DATA0_ADDR, 2) & 1u) == 0u);
    return fails;
}

/* Case 10: EQxBW is honoured on the peaking bands and refused on the shelves
 * — EQ1's bit 8 is the path select, so a shelf allowed to carry a bandwidth
 * flag would silently switch the EQ onto the playback path. */
static int test_eq_bandwidth_bit(void)
{
    int fails = 0;
    const int8_t  g[5] = { 0, 0, 4, 0, 0 };
    const uint8_t c[5] = { 1, 1, 2, 2, 1 };
    const uint8_t n[5] = { 1, 0, 1, 0, 1 };      /* both shelves ask for it */

    eq_reset_to_flat();
    hal_eq_set(g, c, n);
    fails += check("eq bw: band 3 (EQ3) carries EQxBW",
                   (nth_write(I2C_DATA0_ADDR, 4) & 1u) == 1u);
    fails += check("eq bw: the low shelf carries only EQ3DMODE",
                   nth_write(I2C_DATA0_ADDR, 2) == 0x25 &&
                   (nth_write(I2C_DATA1_ADDR, 2) & EQ_GAIN_MASK) == EQ_GAIN_0DB);
    fails += check("eq bw: the high shelf's bit 8 stays clear",
                   (nth_write(I2C_DATA0_ADDR, 6) & 1u) == 0u);
    return fails;
}

/* Case 11: the Bass/Treble tone control. It is not a separate entry point any
 * more — it is the flat curve with the two shelves moved, exactly what
 * eq_effective_curve() hands over at EQ Off — so what this pins is that the
 * words it produces are still the ones this firmware emitted before presets
 * existed, now with the pre-cut its boost has always needed. */
static int test_tone_curve(void)
{
    int fails = 0;
    /* eq_effective_curve(EQ_OFF, +3, -2): mids flat on centre code 00. */
    const int8_t  g[5] = { 3, 0, 0, 0, -2 };
    const uint8_t c[5] = { 1, 0, 0, 0, 1 };
    const uint8_t n[5] = { 0, 0, 0, 0, 0 };

    eq_reset_to_flat();
    hal_eq_set(g, c, n);
    fails += check("tone: seven codec writes",
                   count_writes(I2C_ADDR_ADDR) == 7);
    fails += check_eq_burst("tone", 0, 0 /*from flat*/, g, c, n);
    /* EQ1 = EQ3DMODE | 105 Hz | (12-3); EQ5 = 6.9 kHz | (12+2). */
    fails += check("tone: EQ1 is 0x129 with the DAC path selected",
                   nth_write(I2C_DATA0_ADDR, 2) == 0x25 &&
                   nth_write(I2C_DATA1_ADDR, 2) == 0x29);
    fails += check("tone: EQ5 is 0x02E",
                   nth_write(I2C_DATA0_ADDR, 6) == 0x2C &&
                   nth_write(I2C_DATA1_ADDR, 6) == 0x2E);
    fails += check("tone: +3 dB of boost is pre-cut",
                   nth_write(I2C_DATA1_ADDR, 0) == want_dacvol(3));
    return fails;
}

/*
 * Case 12: the WRITE ORDER, both ways, as literal bus bytes.
 *
 * The rule the driver claims is that the DAC attenuation is never smaller
 * than the boost that is live, at any instant — including the five or so I2C
 * transactions in the middle of hal_eq_set. That is only true if the order
 * follows the sign of the change, so both signs are pinned here:
 *
 *   flat -> Bass Booster  (pre-cut 0 -> 6): cut the DAC, THEN boost.
 *   Bass Booster -> flat  (pre-cut 6 -> 0): unboost, THEN let the DAC up.
 *
 * Get this backwards in either direction and there is a window where EQ1
 * holds +6 dB with LDACVOL at 0xFF.
 */
static int test_eq_write_order_both_ways(void)
{
    int fails = 0;
    const uint8_t c[5] = { 1, 1, 2, 2, 1 };
    const uint8_t n[5] = { 0, 0, 0, 0, 0 };
    const int8_t  boost[5] = { 6, 3, 0, 0, 0 };     /* Bass Booster, pre-cut 6 */
    const int8_t  flat[5]  = { 0, 0, 0, 0, 0 };     /* Off,          pre-cut 0 */

    /* --- going up: DACVOL (0xF3) leads, EQ1 (+6 dB) follows --- */
    eq_reset_to_flat();
    hal_eq_set(boost, c, n);
    fails += check("order up: LDACVOL 0xF3 is transaction 0",
                   nth_write(I2C_DATA0_ADDR, 0) == 0x16 &&
                   nth_write(I2C_DATA1_ADDR, 0) == 0xF3);
    fails += check("order up: RDACVOL 0xF3 + DACVU is transaction 1",
                   nth_write(I2C_DATA0_ADDR, 1) == 0x19 &&
                   nth_write(I2C_DATA1_ADDR, 1) == 0xF3);
    fails += check("order up: EQ1 +6 dB comes AFTER the cut",
                   nth_write(I2C_DATA0_ADDR, 2) == 0x25 &&
                   nth_write(I2C_DATA1_ADDR, 2) == 0x26);
    fails += check_eq_burst("order up", 0, 0, boost, c, n);

    /* --- going down: the gains come off first, DACVOL (0xFF) last --- */
    eq_bus_reset();
    hal_eq_set(flat, c, n);
    fails += check("order down: EQ1 back to 0 dB is transaction 0",
                   nth_write(I2C_DATA0_ADDR, 0) == 0x24 &&
                   nth_write(I2C_DATA1_ADDR, 0) == 0x2C);
    fails += check("order down: EQ5 is transaction 4",
                   nth_write(I2C_DATA0_ADDR, 4) == 0x2C &&
                   nth_write(I2C_DATA1_ADDR, 4) == 0x2C);
    fails += check("order down: LDACVOL 0xFF is transaction 5",
                   nth_write(I2C_DATA0_ADDR, 5) == 0x16 &&
                   nth_write(I2C_DATA1_ADDR, 5) == 0xFF);
    fails += check("order down: RDACVOL 0xFF + DACVU is transaction 6",
                   nth_write(I2C_DATA0_ADDR, 6) == 0x19 &&
                   nth_write(I2C_DATA1_ADDR, 6) == 0xFF);
    fails += check("order down: still exactly seven writes",
                   count_writes(I2C_ADDR_ADDR) == 7);
    fails += check_eq_burst("order down", 0, 6, flat, c, n);

    /* --- equal pre-cut: nothing to sequence, DACVOL leads --- */
    const int8_t rock[5]  = { 5, 2, -1, 2, 4 };     /* pre-cut 5 */
    const int8_t dance[5] = { 5, 2,  0, 3, 4 };     /* pre-cut 5 too */
    eq_reset_to_flat();
    hal_eq_set(rock, c, n);
    eq_bus_reset();
    hal_eq_set(dance, c, n);
    fails += check("order equal: LDACVOL 0xF5 leads",
                   nth_write(I2C_DATA0_ADDR, 0) == 0x16 &&
                   nth_write(I2C_DATA1_ADDR, 0) == 0xF5);
    fails += check_eq_burst("order equal", 0, 5, dance, c, n);
    return fails;
}

/*
 * Case 12: hal_audio_init resets the codec once per TRACK, so everything here
 * has to come back from RAM. hal_codec_restore must replay the OUT1 pair and
 * then the whole EQ burst — the same nine writes, same order — or the user's
 * preset would last exactly one track.
 */
static int test_codec_restore_replays_eq(void)
{
    int fails = 0;
    const int8_t  g[5] = { 5, 2, -1, 2, 4 };    /* ui/eq.c preset 12, Rock */
    const uint8_t c[5] = { 1, 1, 2, 2, 1 };
    const uint8_t n[5] = { 0, 0, 0, 0, 0 };

    eq_reset_to_flat();
    hal_volume_set(60);
    hal_eq_set(g, c, n);

    eq_bus_reset();
    hal_codec_restore();
    fails += check("restore: two OUT1 writes then the seven EQ writes",
                   count_writes(I2C_ADDR_ADDR) == 9);
    fails += check("restore: OUT1 pair first (L, then R with VU)",
                   nth_write(I2C_DATA0_ADDR, 0) == 0x68 &&
                   nth_write(I2C_DATA0_ADDR, 1) == 0x6B);
    /* The reset put DACVOL back to 0xFF behind the driver's back, so the
     * replay must re-cut BEFORE it re-boosts: DACVOL first, prev 0. */
    fails += check_eq_burst("restore", 2, 0, g, c, n);
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
    fails += test_eq_flat_is_the_shipped_tone_path();
    fails += test_eq_preset_words();
    fails += test_eq_clamp_and_path_select();
    fails += test_eq_bandwidth_bit();
    fails += test_tone_curve();
    fails += test_eq_write_order_both_ways();
    fails += test_codec_restore_replays_eq();

    if (fails == 0) {
        printf("ALL PASS\n");
    } else {
        printf("FAIL: %d check%s failed\n", fails, fails == 1 ? "" : "s");
    }
    return fails == 0 ? 0 : 1;
}
