/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/battery.h — battery gauge + charge/power-state contract.
 *
 * The iPod 5G/5.5G has NO SoC ADC for the cell: battery voltage comes
 * from the PCF50605 PMU's internal 10-bit ADC, read over the same on-SoC
 * I2C control bus the WM8758 codec uses (7-bit device address 0x08). See
 * battery.c and core/docs/hw/06-power.md for the mechanism and the
 * cleanroom facts it was built from.
 *
 * Charge/power-state (external supply present, actively charging) is
 * NOT on the PMU/I2C path — it is three plain GPIO input bits, so the
 * power-state calls are cheap and never touch the (slower, harder) I2C
 * read. A UI can poll power_is_external()/power_is_charging() every frame.
 *
 * DEVICE-GATED CALIBRATION: battery_millivolts()'s scaling and the
 * percent curve are transcribed from the 2005 Apple cell and CANNOT be
 * validated off-hardware. The safe first on-device step is to display
 * raw millivolts and sanity-check the ~3300..4200 mV range before
 * trusting battery_percent() or any shutdown threshold. Treat percent as
 * cosmetic until calibrated against the real (often replacement) cell.
 */

#ifndef CORE_HAL_HW_BATTERY_H
#define CORE_HAL_HW_BATTERY_H

/* One-time setup. Currently a no-op: the shared I2C controller is brought
 * up by i2c_init() in the boot path (the codec needs it too), and each
 * read re-selects the ADC channel, so there is no per-gauge state to
 * prime. Kept in the API so a future settling/calibration step has a home
 * that callers already invoke. */
void battery_init(void);

/*
 * Battery terminal voltage in millivolts via the PCF50605 ADC
 * (select channel ADCVIN1, read the 10-bit result, scale x6000>>10).
 * Returns -1 if the I2C transaction fails. DEVICE-GATED scaling.
 */
int battery_millivolts(void);

/*
 * State-of-charge 0..100 from the piecewise-linear voltage->percent
 * curve (integer lerp between the 11 calibration points). Returns -1 if
 * the underlying voltage read fails. APPROXIMATE / DEVICE-GATED — the
 * low end (0..20%) is the steep, cell-dependent region; do not drive
 * shutdown off percent, use a raw-mV threshold.
 */
int battery_percent(void);

/*
 * One conversion, everything it produced — the calibration primitive.
 *
 * The two calls above each run their OWN conversion, so a caller wanting both
 * voltage and percent paid two I2C round trips (and two settling delays) for
 * two DIFFERENT samples that could disagree. Worse, neither exposes what
 * calibration actually needs: the raw ADC code, and the millivolts BEFORE the
 * plausibility clamp. Without those you cannot tell a flat cell from a bus
 * glitch — both surface as the clamp floor — and you cannot check the x6000>>10
 * scaling against a meter at all.
 *
 * Fills `out` and returns 0, or returns -1 on I2C failure (in which case every
 * field is set to -1 rather than a plausible-looking zero). A conversion
 * outside ~2800..4600 mV is treated as a failure too: the I2C driver cannot
 * see a missing ack, so an unanswered result read hands back the register
 * file's stale bytes (1130 mV) or a floating bus (5994 mV), and neither is a
 * voltage this cell can be at while the firmware is running to ask. Inside
 * that band, `mv_raw` is the unclamped conversion and `mv` is clamped onto
 * the cell's 3300..4200 mV operating range — the value the gauge should use.
 */
typedef struct {
    int raw;      /* 10-bit ADC code as read (0..1023)                       */
    int mv_raw;   /* millivolts from the raw code, BEFORE the sanity clamp   */
    int mv;       /* millivolts after the clamp — the value the gauge uses   */
} battery_sample_t;

int battery_sample(battery_sample_t *out);

/*
 * The voltage->percent curve as a PURE function, so a caller holding a sample
 * (or a filtered average of several) can convert without triggering another
 * conversion. Same APPROXIMATE / DEVICE-GATED caveat as battery_percent().
 */
int battery_percent_from_mv(int mv);

/* ---------------------------------------------------------------------------
 * Low-battery policy: filtered millivolts, two thresholds, one state machine.
 *
 * Nothing in this firmware reacted to a low cell at all: every consumer of the
 * gauge only DREW it, and the device ran until the cell's protection IC or the
 * PMU cut power — which it can do in the middle of config_save() putting a
 * sector on the user's disk. 06-power.md documents the intended two-stage
 * policy (park the drive at 3500 mV, power off at 3300 mV); this is it.
 *
 * Driven by MILLIVOLTS, never percent: the percent curve is a 2005 Apple-cell
 * table that has never been measured against the fitted cell (see the header
 * comment), and its low end is exactly the region a shutdown decision lives
 * in. Millivolts are what the ADC actually measured.
 *
 * Driven by a FILTERED value, never a single sample: the reading is raw and
 * unfiltered, and a 1.8" HDD spin-up sags the cell by tens to low hundreds of
 * millivolts for a couple of seconds. In the 3720..3840 mV plateau the curve
 * is 30 mV per 10 %, so one sagged sample looks like a large sudden drop —
 * and, near either line, like a crossing that never happened. The policy sees
 * only the median of the last BATTERY_FILTER_N samples (see battery.c for why
 * a median and not an EMA), and the shutoff line must additionally hold for
 * BATTERY_SHUTOFF_CONFIRM consecutive evaluations.
 *
 * The caller (kernel/main.c battery_refresh) samples every 5 s and feeds each
 * result here; this module decides, the caller acts on the returned EVENT.
 * Events are EDGES, so an action fires exactly once per crossing.
 * ------------------------------------------------------------------------- */

/* Thresholds on the FILTERED, clamped millivolts (06-power.md, "Brown-out /
 * low-battery shutdown"). Compared with <=, not <: battery_sample() clamps mv
 * to a 3300 mV floor, which coincides with the shutoff line, so a strict
 * comparison could never fire on the clamped value. Neither fires while
 * external power is present — see battery_policy_feed(). */
#define BATTERY_MV_DISKSAFE   3500   /* park the drive, refuse disk writes     */
#define BATTERY_MV_SHUTOFF    3300   /* power off (PMU standby)                */

/* Hysteresis: DISKSAFE clears only once the filtered value has climbed back to
 * this. 100 mV is above any plausible load-release rebound at these voltages
 * but far below the plateau, so a genuinely low cell cannot bounce out of
 * DISKSAFE by merely having its drive parked, while a charger plugged in
 * (which drives the terminal straight up) clears it within one filter window. */
#define BATTERY_MV_RECOVER    3600

/* Median window. 5 samples at the 5 s cadence = 25 s of history; a median of 5
 * ignores up to TWO outliers outright, i.e. two consecutive spin-up-sagged
 * samples move the filtered value by exactly zero. */
#define BATTERY_FILTER_N      5

/* Consecutive full-ring evaluations with the median at/below the shutoff line
 * before SHUTOFF fires. Powering off is the one action that cannot be undone
 * by the next sample, so it gets a second, independent debounce on top of the
 * median: 3 evaluations = 15 s, during which the median has to stay down —
 * which in turn needs at least 3 of every 5 raw samples down. Worst-case
 * response from the true crossing is therefore ~25-30 s; see battery.c for
 * why that is well inside the margin the cell gives us. */
#define BATTERY_SHUTOFF_CONFIRM 3

typedef enum {
    BATTERY_LEVEL_OK = 0,       /* normal operation                           */
    BATTERY_LEVEL_DISKSAFE,     /* <= 3500 mV filtered: drive parked, no writes */
    BATTERY_LEVEL_SHUTOFF,      /* <= 3300 mV confirmed: powering off (terminal) */
} battery_level_t;

typedef enum {
    BATTERY_EVENT_NONE = 0,
    BATTERY_EVENT_DISKSAFE,     /* OK -> DISKSAFE: flush, park, stop writing  */
    BATTERY_EVENT_SHUTOFF,      /* DISKSAFE -> SHUTOFF: power off now         */
    BATTERY_EVENT_RECOVERED,    /* DISKSAFE -> OK: writes may resume          */
} battery_event_t;

/* Forget all history: empty ring, level OK. Also the state at boot. */
void battery_policy_reset(void);

/*
 * Feed one sample. `mv` is battery_sample()'s clamped mv, or -1 if that call
 * failed. A failed sample is NOT a low sample: it is not entered into the
 * ring and cannot move the filter or the level in either direction — a bus
 * glitch must never look like a flat battery (and cannot look like a
 * recovery either). Returns the edge this sample caused, or NONE.
 *
 * `external` is power_is_external() at the time of the sample. While it is
 * set the DESCENT is disabled: neither DISKSAFE nor SHUTOFF can fire, and the
 * shutoff confirm run is held at zero, because a cell on a charger cannot
 * brown the system out — the charger holds the rails and drives the cell UP.
 * A cell so flat it reads under the lines while charging is exactly the one
 * that must be left on the charger rather than powered off. The ring still
 * fills (the gauge shows the real terminal voltage) and RECOVERED still fires
 * off the median, so a DISKSAFE latched before the plug-in clears normally.
 *
 * The policy is ARMED only once the ring is full (BATTERY_FILTER_N good
 * samples, ~20 s after boot at the 5 s cadence). Before that the median is
 * computed over what is present, for the gauge, but no event can fire — the
 * boot path is one long disk spin-up, exactly the sag the filter exists to
 * ride through.
 */
battery_event_t battery_policy_feed(int mv, int external);

/* Current level (for a UI to read: warn at DISKSAFE, "goodbye" at SHUTOFF). */
battery_level_t battery_policy_level(void);

/* Median of the ring, or -1 if no good sample has been fed yet. This — not the
 * instantaneous sample — is what the gauge should convert with
 * battery_percent_from_mv(), so the status-strip glyph stops twitching on
 * every spin-up. */
int battery_filtered_mv(void);

/* 1 once the ring holds BATTERY_FILTER_N good samples (policy armed). */
int battery_filter_ready(void);

/*
 * 0 while the cell is at or below the disk-safe line: nothing should start a
 * disk WRITE, because there may not be enough energy left to finish it and a
 * torn sector is the one outcome this whole policy exists to prevent. Reads
 * are unaffected — playback can keep going down to the shutoff line.
 *
 * config_save() (kernel/config.c) is the only ata_write_sectors() caller in
 * the firmware, and settings_commit() in kernel/main.c is its only caller;
 * that is where this belongs. Pending changes should stay pending, not be
 * dropped, so they land on the next commit after RECOVERED.
 */
int battery_disk_writes_allowed(void);

/*
 * External power present: main charger (dock/FireWire/USB power) OR a
 * USB charger is attached. Pure GPIO read, no I2C. Returns 1/0.
 */
int power_is_external(void);

/*
 * The charger is actively charging the cell right now. Pure GPIO read,
 * no I2C. Returns 1/0. "Full while plugged" is approximately
 * (power_is_external() && !power_is_charging()).
 */
int power_is_charging(void);

/*
 * Tell the LTC4066 how much input current it may draw (06-power.md, "Charge
 * current control"). 500 => HPWR asserted, the charger may pull up to 500 mA;
 * anything less => the 100 mA default cap. The charger is otherwise autonomous
 * (CV/CC, fast/trickle, end-of-charge are all internal) — this is the only
 * charge knob the firmware has.
 *
 * Deliberately never raises SUSP: a latched SUSP high is "the iPod silently
 * refuses to charge", and we have no feature that needs zero-current mode.
 */
void charger_set_max_current(int milliamps);

#endif /* CORE_HAL_HW_BATTERY_H */
