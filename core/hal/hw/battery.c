/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/battery.c — battery gauge (PCF50605 PMU ADC over I2C) +
 * charge/power-state (plain GPIO input bits).
 *
 * Implements core/docs/hw/06-power.md. Freestanding: <stdint.h> only,
 * hardware touched exclusively through the mmio.h seam and the i2c.c
 * driver, so this host-compiles unchanged for the golden-trace test.
 *
 * WHY the split path:
 *   - Voltage lives behind the PMU's internal 10-bit ADC, reachable only
 *     over the shared on-SoC I2C bus (device 0x08). It costs a bus
 *     round-trip and its scaling is device-gated (see below).
 *   - Charger-present / charging are three GPIO pins with NO PMU/I2C
 *     involvement, so those two calls are near-free and calibration-safe.
 *
 * DEVICE-GATED (cannot be validated off-hardware): the x6000>>10 scaling
 * and the percent curve are the 2005 Apple-cell values. On real hardware,
 * display raw millivolts FIRST and confirm the ~3300..4200 mV range
 * before trusting battery_percent() or any shutdown threshold. Replacement
 * cells commonly read high at the low (steep) end of the curve.
 */

#include <stdint.h>

#include "pp5022.h"
#include "mmio.h"
#include "i2c.h"
#include "battery.h"

/* ---------- PCF50605 PMU (battery ADC over I2C) --------------------- */

/* 7-bit I2C device address of the PMU — the same bus the codec uses. */
#define PMU_ADDR            0x08

/*
 * ADC control/select register (ADCC1). Written to pick a channel and
 * start a conversion in one byte: (channel << 1) | START. The battery
 * sits on channel ADCVIN1 (0x2), which has the PMU's built-in resistive
 * divider, so the assembled value is (0x2 << 1) | 0x1 = 0x05.
 */
#define PMU_ADCC1           0x2F
#define PMU_ADC_START_VIN1  0x05

/*
 * ADC result registers. ADCS1 (0x30) holds the high 8 bits, ADCS2 (0x31)
 * holds the low 2 bits in its bottom two positions — a register-pointer
 * read of 2 bytes starting at ADCS1 fetches both (the pointer
 * auto-increments). 10-bit result = (data[0] << 2) | (data[1] & 0x03).
 */
#define PMU_ADCS1           0x30
#define PMU_ADC_RESULT_LEN  2
#define PMU_ADC_LOW_MASK    0x03

/*
 * Raw(0..1023) -> millivolts. Full-scale reference is 6000 mV over the
 * 10-bit range; the divider is INSIDE the PMU (ADCVIN1), so this is a
 * direct scale with no external divider math: mV = raw * 6000 / 1024.
 * (>>10 instead of /1024 keeps it integer-only, no libm.)
 */
#define PMU_ADC_FULLSCALE_MV 6000
#define PMU_ADC_BITS         10

/*
 * CONVERSION WAIT. i2c_send deliberately does not wait for its own completion
 * (09-i2c.md: the write path lets the NEXT transaction's leading BUSY-wait
 * cover it), so the old code strobed "start conversion" and immediately turned
 * the bus around to read the result. Every reading was therefore at least one
 * sample stale, and the first after boot could be whatever the result
 * registers happened to hold.
 *
 * 06-power.md documents no conversion-ready bit anywhere in the PCF50605
 * register set, so there is nothing to poll — a fixed wait is the only option.
 * The doc's own cadence for this channel is "5 reads per 2 s", i.e. ~400 ms
 * between samples, which tells us the conversion is expected to be far shorter
 * than that but not how much shorter. 2 ms is comfortably above any plausible
 * 10-bit SAR conversion time and is invisible next to a 400 ms poll interval.
 *
 * DEVICE-GATED: this needs confirming on hardware (watch that two consecutive
 * reads after a sudden load change actually differ, rather than lagging).
 *
 * Implemented as a CPU-cycle loop scaled by the core clock, NOT a USEC_TIMER
 * wait: the golden trace for battery_millivolts asserts the exact ordered bus
 * transaction and nothing else, so an MMIO read in the middle of it would be a
 * trace change.
 */
#define PMU_ADC_SETTLE_US    2000u
#define PMU_SETTLE_CALIB_HZ  30000000u
/* Cycles a volatile spin trip costs on ARM7TDMI (load/sub/store/cmp/branch),
 * conservative-low so the wait errs long rather than short. */
#define PMU_SETTLE_CYCLES    8u

/* Current core frequency; weak so battery.c still links standalone in the host
 * trace test (which has no kernel clock driver). Absent -> assume calibration. */
__attribute__((weak)) uint32_t cpu_frequency(void);

static void pmu_adc_settle(void)
{
    uint32_t hz = cpu_frequency ? cpu_frequency() : PMU_SETTLE_CALIB_HZ;
    if (hz == 0u) {
        hz = PMU_SETTLE_CALIB_HZ;
    }
    /* trips = us * MHz / cycles-per-trip */
    volatile uint32_t trips =
        PMU_ADC_SETTLE_US * (hz / 1000000u) / PMU_SETTLE_CYCLES;
    while (trips-- != 0) {
        /* wait for the conversion */
    }
}

/*
 * Plausibility band, in two tiers.
 *
 * i2c_read() cannot see a NACK: it waits for the controller to go idle and
 * then latches whatever I2C_DATA0/1 hold, and reports success. When the PMU
 * does not answer the result read, the data registers still hold the last
 * bytes WRITTEN through them — the register-pointer write of ADCS1 (0x30) and,
 * before it, the ADCC1 start byte (0x05) — so the "sample" assembles to raw
 * 0xC1 = 1130 mV. The inverse failure (bus floating high, 0x3FF) reads 5994 mV.
 * The old single-tier clamp pinned the first to the 3300 mV floor, which IS the
 * shutoff line: three such failed reads inside 15 s walked the policy through
 * DISKSAFE to a power-off on a healthy cell. The second pinned to 4200 mV and
 * hid a flat battery behind a full one.
 *
 * So: a converted value outside the REJECT band is not a measurement of this
 * cell at all — a single Li-Ion cannot run this CPU below ~2.8 V and cannot be
 * charged past ~4.6 V without the PMU's own protection cutting it — and is
 * reported as a FAILED read (-1). The caller already holds its last good
 * reading on a failure and the policy already ignores -1, so a bad sample now
 * enters nothing anywhere. Only a SMALL excursion — inside the reject band but
 * outside the cell's normal 3300..4200 mV operating range, which a genuinely
 * flat cell under load or a cell being driven by the charger can produce — is
 * clamped onto the operating range. Thresholds from 06-power.md, "Brown-out /
 * low-battery shutdown" (battery_level_shutoff = 3300) and the 100 % curve
 * point (4180, rounded up to the cell's 4200 mV charge ceiling).
 */
#define PMU_MV_REJECT_LO  2800   /* below: not a reading of this cell        */
#define PMU_MV_REJECT_HI  4600   /* above: not a reading of this cell        */
#define PMU_MV_MIN        3300   /* clamp floor for small excursions         */
#define PMU_MV_MAX        4200   /* clamp ceiling for small excursions       */

/* ---------- Power-state GPIO bits (no I2C) -------------------------- */

/* GPIOL input: main charger present is ACTIVE-LOW (bit clear = present);
 * USB charger present is ACTIVE-HIGH (bit set = present). */
#define POWER_MAIN_CHARGER_BIT  0x08   /* GPIOL bit 3, active-low  */
#define POWER_USB_CHARGER_BIT   0x10   /* GPIOL bit 4, active-high */

/* GPIOB input: currently charging is ACTIVE-LOW (bit clear = charging). */
#define POWER_CHARGING_BIT      0x01   /* GPIOB bit 0, active-low  */

/* ---------- Voltage -> percent curve --------------------------------
 * 11-point piecewise-linear discharge curve for the iPod Video cell
 * (0%,10%,...,100%). APPROXIMATE / DEVICE-GATED — see the header. Kept as
 * a static const table + integer lerp so there is no libm dependency and
 * the whole thing lives in the freestanding image.
 */
#define BATTERY_CURVE_POINTS 11
#define BATTERY_CURVE_STEP   10   /* percent between adjacent points */

static const uint16_t battery_v_curve[BATTERY_CURVE_POINTS] = {
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
};

/* Linear interpolation between the curve points, integer math with
 * round-to-nearest (+span/2 before the divide). Below the 0% point -> 0,
 * at/above the 100% point -> 100. */
int battery_percent_from_mv(int mv)
{
    if (mv <= battery_v_curve[0]) {
        return 0;
    }
    for (int i = 1; i < BATTERY_CURVE_POINTS; i++) {
        if (mv < battery_v_curve[i]) {
            int span = battery_v_curve[i] - battery_v_curve[i - 1];
            int into = mv - battery_v_curve[i - 1];
            return (i - 1) * BATTERY_CURVE_STEP
                   + (into * BATTERY_CURVE_STEP + span / 2) / span;
        }
    }
    return 100;
}

/*
 * Percent while ON THE CHARGER. The discharge curve maps RESTING voltage;
 * a cell being charged sits above its resting voltage by the charge current
 * times its internal resistance (plus the charger's own constant-voltage
 * hold near the top), so the same lookup reads it 20-30 points fuller the
 * instant the cable goes in, and drops back the instant it comes out — the
 * "jumps to the top when plugged in" the owner reported (device, 2026-09-13).
 *
 * Model: an offset proportional to the requested charge current (~0.3 mV
 * per mA: ~150 mV at the 500 mA HPWR budget, ~30 mV at the 100 mA cap —
 * a 2005-era cell's ~0.3 Ω), tapering linearly to zero over the last
 * BATTERY_CHG_TAPER_MV below the 4200 mV charge ceiling, where the charger
 * is in its constant-voltage phase and the terminal voltage IS the resting
 * voltage. Approximate — the point is to remove the jump, not to be a
 * coulomb counter — and clamped so the estimate can never exceed what the
 * raw voltage says.
 */
#define BATTERY_CHG_CEIL_MV   4200
#define BATTERY_CHG_TAPER_MV   150

int battery_percent_charging(int mv, int charge_ma)
{
    if (mv < 0) {
        return -1;
    }
    int off = (charge_ma * 3 + 5) / 10;                 /* 0.3 mV per mA */
    int head = BATTERY_CHG_CEIL_MV - mv;                /* room below the ceiling */
    if (head <= 0) {
        off = 0;
    } else if (head < BATTERY_CHG_TAPER_MV) {
        off = (off * head + BATTERY_CHG_TAPER_MV / 2) / BATTERY_CHG_TAPER_MV;
    }
    int pct = battery_percent_from_mv(mv - off);
    int cap = battery_percent_from_mv(mv);
    return pct > cap ? cap : pct;
}

/* ---------- Public API ---------------------------------------------- */

void battery_init(void)
{
    /* The shared I2C controller is initialized in the boot path (i2c_init,
     * needed for the codec too) and each read re-selects the ADC channel, so
     * there is nothing to prime on the bus. The policy state is zero-initialised
     * anyway; resetting it here makes "boot = empty ring, level OK" explicit. */
    battery_policy_reset();
}

int battery_sample(battery_sample_t *out)
{
    out->raw = out->mv_raw = out->mv = -1;

    /* Select channel ADCVIN1 and start the conversion (ADCC1 = 0x05). */
    uint8_t select[2] = { PMU_ADCC1, PMU_ADC_START_VIN1 };
    if (i2c_send(PMU_ADDR, select, 2) != 0) {
        return -1;
    }

    /* Let the conversion actually happen before latching the result — see
     * PMU_ADC_SETTLE_US. There is no ready bit to poll (06-power.md). */
    pmu_adc_settle();

    /* Read the two result bytes back (ADCS1 then auto-incremented ADCS2). */
    uint8_t data[PMU_ADC_RESULT_LEN];
    if (i2c_read(PMU_ADDR, PMU_ADCS1, data, PMU_ADC_RESULT_LEN) != 0) {
        return -1;
    }

    /* Assemble the 10-bit sample and scale to millivolts. */
    int raw = ((int)data[0] << 2) | (data[1] & PMU_ADC_LOW_MASK);
    int mv  = (raw * PMU_ADC_FULLSCALE_MV) >> PMU_ADC_BITS;

    /*
     * Outside the plausibility band this is a FAILED read, not a measurement
     * — see PMU_MV_REJECT_LO/HI: i2c_read() cannot see a NACK, so an
     * unanswered result read returns the register file's last written bytes
     * (raw 0xC1, 1130 mV) or a floating bus (raw 0x3FF, 5994 mV), and an
     * all-zero result (0 V on a cell that is demonstrably running this CPU)
     * is the same failure. Any of those clamped onto a policy threshold is a
     * spurious shutdown or a masked flat battery; returned as -1 it enters
     * nothing anywhere. `out` keeps its -1 fill.
     */
    if (mv < PMU_MV_REJECT_LO || mv > PMU_MV_REJECT_HI) {
        return -1;
    }

    out->raw    = raw;
    out->mv_raw = mv;

    /* Clamp a small excursion onto the cell's operating band (PMU_MV_MIN/MAX).
     * mv_raw above keeps the unclamped value, because the clamp is exactly what
     * makes a flat cell under load and a cell at the floor indistinguishable. */
    if (mv < PMU_MV_MIN) {
        mv = PMU_MV_MIN;
    } else if (mv > PMU_MV_MAX) {
        mv = PMU_MV_MAX;
    }
    out->mv = mv;
    return 0;
}

int battery_millivolts(void)
{
    battery_sample_t s;
    return (battery_sample(&s) == 0) ? s.mv : -1;
}

int battery_percent(void)
{
    battery_sample_t s;
    if (battery_sample(&s) != 0) {
        return -1;
    }
    return battery_percent_from_mv(s.mv);
}

/* ---------- Filter + low-battery policy -----------------------------
 *
 * WHY A MEDIAN AND NOT AN EMA. The disturbance we are defending against is a
 * short, deep, one-sided excursion: the drive spins up, the cell sags by 50 to
 * a few hundred millivolts for ~2 s, and at a 5 s cadence that lands on at most
 * one sample, occasionally two. An EMA is pulled by every sample in proportion
 * to its depth — with alpha = 1/4 a 200 mV sag moves the output 50 mV, which is
 * more than the whole 30 mV/10 % plateau step (a visible gauge jump) and, near
 * a threshold, a false crossing. A median of five ignores up to two outliers
 * COMPLETELY: the filtered value does not move at all until a majority of the
 * window agrees. That majority rule is also the debounce the policy needs —
 * the disk-safe line cannot be crossed by fewer than 3 of the last 5 samples
 * (15 s of agreement), and no single sample of any depth can do it.
 *
 * The cost is lag: the median trails a genuine monotonic decline by ~2
 * samples (10 s). Add the shutoff confirm and the worst-case response from
 * the true 3300 mV crossing to the power-off decision is ~25-30 s. That is
 * safe: below the shutoff line there is still >= 200 mV before the PMU or the
 * cell's own protection cuts power, and at this end of the discharge curve
 * that is minutes even at the ~300 mA a spinning drive draws — an order of
 * magnitude more than we spend deciding. What is NOT safe is the other
 * direction: powering the user's device off because a spin-up happened to
 * coincide with a sample.
 *
 * Only the clamped mv is filtered, so the ring holds values in 3300..4200 by
 * construction and the arithmetic below is trivially in range. External power
 * gates the descent (see battery_policy_feed): a cell on a charger cannot brown
 * the system out, so neither threshold fires while one is attached.
 */

static int      bat_ring[BATTERY_FILTER_N];
static int      bat_ring_n;         /* good samples held, 0..N               */
static int      bat_ring_head;      /* next slot to overwrite once full      */
static battery_level_t bat_level = BATTERY_LEVEL_OK;
static int      bat_shutoff_run;    /* consecutive evaluations <= shutoff    */

void battery_policy_reset(void)
{
    bat_ring_n       = 0;
    bat_ring_head    = 0;
    bat_level        = BATTERY_LEVEL_OK;
    bat_shutoff_run  = 0;
}

/* Median of the samples held. Insertion sort on a copy — N is 5, this runs
 * once every 5 s, and it keeps the ring itself in arrival order. For an even
 * count (only before the ring first fills) the two middles are averaged. */
int battery_filtered_mv(void)
{
    if (bat_ring_n == 0) {
        return -1;
    }
    int tmp[BATTERY_FILTER_N];
    for (int i = 0; i < bat_ring_n; i++) {
        int v = bat_ring[i];
        int j = i;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    if (bat_ring_n & 1) {
        return tmp[bat_ring_n / 2];
    }
    return (tmp[bat_ring_n / 2 - 1] + tmp[bat_ring_n / 2]) / 2;
}

int battery_filter_ready(void)
{
    return bat_ring_n == BATTERY_FILTER_N;
}

battery_level_t battery_policy_level(void)
{
    return bat_level;
}

int battery_disk_writes_allowed(void)
{
    return bat_level == BATTERY_LEVEL_OK;
}

battery_event_t battery_policy_feed(int mv, int external)
{
    /* Bus failure is not a flat battery. Nothing changes: the ring keeps its
     * history, the level keeps its state, and the caller keeps showing the
     * last good value. Letting -1 in here would be a 0 in the ring, and three
     * I2C hiccups in a row would power the device off. */
    if (mv < 0) {
        return BATTERY_EVENT_NONE;
    }

    if (bat_ring_n < BATTERY_FILTER_N) {
        bat_ring[bat_ring_n++] = mv;
    } else {
        bat_ring[bat_ring_head] = mv;
        bat_ring_head = (bat_ring_head + 1) % BATTERY_FILTER_N;
    }

    /* Not armed until the window is full — a policy decision on a partial
     * window is a decision on fewer samples than the filter was designed to
     * need, and the first 20 s after boot are the mount + index spin-up. */
    if (!battery_filter_ready()) {
        return BATTERY_EVENT_NONE;
    }

    int filt = battery_filtered_mv();

    /*
     * ON EXTERNAL POWER THE DESCENT IS OFF. Both thresholds exist to keep the
     * system from browning out mid-write (DISKSAFE) or mid-anything (SHUTOFF)
     * as the cell runs down; with a charger attached the rails are held by the
     * charger and the cell is being driven UP, so neither hazard exists — and
     * a cell so flat it still reads under the lines while charging is exactly
     * the one that must be allowed to sit on the charger, not powered off the
     * moment the filter fills. The ring keeps filling (the gauge still shows
     * the real terminal voltage) and RECOVERED still fires off the median, but
     * no downward edge can, and the shutoff confirm run is held at zero so an
     * unplug does not inherit a run counted while plugged in.
     */
    if (external) {
        bat_shutoff_run = 0;
    } else if (filt <= BATTERY_MV_SHUTOFF) {
        /* Second debounce, for the one irreversible action. Counts evaluations
         * whose MEDIAN is at/below the line, so each count already represents
         * a majority of the window; it resets the moment the median lifts. */
        bat_shutoff_run++;
    } else {
        bat_shutoff_run = 0;
    }

    switch (bat_level) {
    case BATTERY_LEVEL_OK:
        if (!external && filt <= BATTERY_MV_DISKSAFE) {
            /* Always the first stage, even if the median is already below the
             * shutoff line (a flat cell at boot): the caller gets to flush and
             * park BEFORE the confirm window for power-off starts running. */
            bat_level = BATTERY_LEVEL_DISKSAFE;
            return BATTERY_EVENT_DISKSAFE;
        }
        break;

    case BATTERY_LEVEL_DISKSAFE:
        if (filt >= BATTERY_MV_RECOVER) {
            /* Charger plugged in, or the load-release rebound turned out to be
             * larger than the hysteresis — either way the cell is above the
             * line by a margin, and latching DISKSAFE forever would refuse
             * every settings save until the next reboot. */
            bat_level = BATTERY_LEVEL_OK;
            return BATTERY_EVENT_RECOVERED;
        }
        if (!external && bat_shutoff_run >= BATTERY_SHUTOFF_CONFIRM) {
            bat_level = BATTERY_LEVEL_SHUTOFF;
            return BATTERY_EVENT_SHUTOFF;
        }
        break;

    case BATTERY_LEVEL_SHUTOFF:
        /* Terminal. The caller is powering off; if it somehow did not, the
         * next boot starts from battery_policy_reset() state anyway. */
        break;
    }
    return BATTERY_EVENT_NONE;
}

int power_is_external(void)
{
    return power_source() != 0;
}

int power_source(void)
{
    uint32_t l = mmio_read32(GPIOL_INPUT_VAL_ADDR);
    int src = 0;
    if ((l & POWER_MAIN_CHARGER_BIT) == 0) src |= POWER_SRC_MAIN;   /* active-low  */
    if ((l & POWER_USB_CHARGER_BIT)  != 0) src |= POWER_SRC_USB;    /* active-high */
    return src;
}

int power_is_charging(void)
{
    uint32_t b = mmio_read32(GPIOB_INPUT_VAL_ADDR);
    return (b & POWER_CHARGING_BIT) == 0;         /* active-low */
}

/* ---------- Charge-current gate (LTC4066) ----------------------------
 * 06-power.md, "Charge current control": the charger is autonomous and the
 * firmware can only gate it with two GPIO outputs.
 *   HPWR = GPIOA bit 0x04 — high = 500 mA permitted, low = 100 mA
 *   SUSP = GPIOL bit 0x04 — high = suspend ALL charging
 *
 * Since the LTC4066 is a LINEAR charger with the system load in front of the
 * cell, system load and battery charge share that input budget. At the
 * 100 mA cap the budget is the device: the event log of 2026-09-19 has the
 * cell sitting PAUSED with the drive parked and the backlight on for 71
 * minutes at 3861 -> 3867 mV, and PLAYING for three hours at 4037 -> 4048 mV,
 * both with the charger's CHRG pin asserted the whole time — "charging"
 * that never gets anywhere. The same log shows the cell only climbing while
 * the device slept. 500 mA is what Apple's firmware asks a PC port for, and
 * it is the difference between a device that charges while it is used and
 * one that only charges while it is off.
 *
 * Writes go through the atomic +GPIO_BITWISE_OFFSET alias (one masked 32-bit
 * write, no read-modify-write), so this cannot race the backlight driver's
 * unrelated GPIOL bits. The direction registers are written the same way.
 *
 * SPEC NOTE: 500 mA without USB enumeration is out of spec for a PC port — we
 * have no USB stack, so we can never be *entitled* to it. The hardware does not
 * interlock the two (HPWR is a dumb current-limit select), and this is exactly
 * what Apple's own firmware asserts after it negotiates. Safe on wall chargers
 * and on essentially all PC root ports; a strictly limited hub port may fold
 * back until replug. Settings > Battery > Charge Rate is the way back to
 * 100 mA (it is also the quieter setting on the headphone jack, since the
 * cable's ground loop carries whatever the port's supply does).
 *
 * The pin level is read back by charger_pin_state(): GPIOA_INPUT_VAL reads
 * the pad, so a 1 there with the pin enabled and output-driven is the request
 * ARRIVING at the charger, which is the closest thing to a current meter this
 * device has.
 */
#define CHG_HPWR_BIT  0x04u
#define CHG_SUSP_BIT  0x04u

/* Masked-write words for the +0x800 alias: set = (m << 8) | m, clear = m << 8. */
#define GPIO_BW_SET(m)   ((((uint32_t)(m)) << 8) | (uint32_t)(m))
#define GPIO_BW_CLEAR(m) (((uint32_t)(m)) << 8)

static int s_charger_ma = 100;      /* HPWR low is the LTC4066's reset state */

void charger_set_max_current(int milliamps)
{
    int fast = milliamps >= 500;

    /* Levels first: SUSP low (never suspend charging), HPWR per request. On a
     * pin the ROM already drives these are the whole operation; on one it
     * left as an input they are the value the pin will show the moment the
     * direction flips below, so there is no glitch through the wrong level. */
    mmio_write32(GPIOL_OUTPUT_VAL_ADDR + GPIO_BITWISE_OFFSET,
                 GPIO_BW_CLEAR(CHG_SUSP_BIT));
    mmio_write32(GPIOA_OUTPUT_VAL_ADDR + GPIO_BITWISE_OFFSET,
                 fast ? GPIO_BW_SET(CHG_HPWR_BIT) : GPIO_BW_CLEAR(CHG_HPWR_BIT));

    /* Then own the pins: GPIO function, output direction. Idempotent. */
    mmio_write32(GPIOL_ENABLE_ADDR    + GPIO_BITWISE_OFFSET, GPIO_BW_SET(CHG_SUSP_BIT));
    mmio_write32(GPIOL_OUTPUT_EN_ADDR + GPIO_BITWISE_OFFSET, GPIO_BW_SET(CHG_SUSP_BIT));
    mmio_write32(GPIOA_ENABLE_ADDR    + GPIO_BITWISE_OFFSET, GPIO_BW_SET(CHG_HPWR_BIT));
    mmio_write32(GPIOA_OUTPUT_EN_ADDR + GPIO_BITWISE_OFFSET, GPIO_BW_SET(CHG_HPWR_BIT));

    s_charger_ma = fast ? 500 : 100;
}

int charger_max_current(void)
{
    return s_charger_ma;
}

void charger_pin_state(charger_pins_t *out)
{
    uint32_t a_in = mmio_read32(GPIOA_INPUT_VAL_ADDR);
    uint32_t a_en = mmio_read32(GPIOA_ENABLE_ADDR);
    uint32_t a_oe = mmio_read32(GPIOA_OUTPUT_EN_ADDR);
    uint32_t l_in = mmio_read32(GPIOL_INPUT_VAL_ADDR);
    uint32_t l_en = mmio_read32(GPIOL_ENABLE_ADDR);
    uint32_t l_oe = mmio_read32(GPIOL_OUTPUT_EN_ADDR);

    out->hpwr      = (a_in & CHG_HPWR_BIT) ? 1 : 0;
    out->hpwr_gpio = (a_en & CHG_HPWR_BIT) ? 1 : 0;
    out->hpwr_out  = (a_oe & CHG_HPWR_BIT) ? 1 : 0;
    out->susp      = (l_in & CHG_SUSP_BIT) ? 1 : 0;
    out->susp_gpio = (l_en & CHG_SUSP_BIT) ? 1 : 0;
    out->susp_out  = (l_oe & CHG_SUSP_BIT) ? 1 : 0;
}
