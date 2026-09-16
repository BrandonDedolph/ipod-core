/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/wallclock.h — the software clock: time of day without touching
 * the I²C bus.
 *
 * WHY THIS FILE EXISTS
 *
 * The time of day lives in the PMIC, at the far end of the shared I²C bus
 * (hal/hw/rtc.c: three transactions, a millisecond or so). The status strip
 * and the main menu header want the time on every paint. Reading the chip per
 * frame would put PMU traffic in the middle of the audio pump for a readout
 * that changes once a minute.
 *
 * So the RTC is read rarely — at boot, after a manual set, after a suspend
 * wake, and every half hour — and the time in between is carried by the same
 * free-running 1 MHz USEC_TIMER everything else in the loop is timed by. This
 * module is that carry: an anchor (an epoch and the microsecond count it was
 * taken at) plus wrap-safe accumulation, exactly the arithmetic ui/sleeptimer.c
 * does for the countdown and for the same reason — the counter wraps every
 * ~71.6 minutes, so `now - anchor` is only meaningful between CONSECUTIVE
 * samples.
 *
 * THE FEED CONTRACT: call wallclock_tick() at least once every ~71 minutes
 * while the clock is valid. An unsigned delta cannot tell 1 second from
 * 1 second + 2^32 µs, so a longer gap silently loses a wrap. The main loop's
 * 5 s battery cadence is where the tick belongs; the half-hourly
 * wallclock_resync() then corrects whatever the tick could not.
 *
 * ACROSS A SUSPEND the timer is not usable: the PLL is parked and the tick
 * drops to 10 Hz, so the wake path RE-ANCHORS from the chip rather than
 * trusting the delta. That is a caller rule, not something this module can
 * enforce — it has no way to know it was asleep.
 *
 * PURE: no hardware, no clock of its own. `now_us` is handed in, so the host
 * suite can run a day through it, across as many wraps as it likes.
 */

#ifndef CORE_KERNEL_WALLCLOCK_H
#define CORE_KERNEL_WALLCLOCK_H

#include <stdint.h>

/* How long the RTC may read exactly the same second as at the last anchor
 * before the clock is declared STOPPED. Two seconds: a real RTC cannot
 * disagree with the µs timer by that much, and the PMU-not-answering shape
 * (hal/hw/rtc.c: the bus hands back stale DATA bytes) reads as a frozen
 * second. */
#define WALLCLOCK_STOPPED_S 2

/* How far the chip and the software clock may disagree at a re-anchor before
 * it is worth a log line. Five seconds over half an hour is ~2800 ppm, far
 * outside any plausible crystal error, so a reading this far off means one of
 * the two is not what we think it is. */
#define WALLCLOCK_DRIFT_S 5

typedef struct {
    int      valid;        /* 0 until anchored from a running RTC          */
    uint32_t epoch;        /* UTC seconds, advanced by wallclock_tick()    */
    uint32_t anchor_epoch; /* what the chip read at the last anchor        */
    uint32_t last_us;      /* the µs counter at the last tick/anchor       */
    uint32_t frac_us;      /* sub-second remainder, always < 1 000 000     */
} wallclock_t;

typedef enum {
    WALLCLOCK_RESYNC_OK = 0,   /* chip and software clock agree            */
    WALLCLOCK_RESYNC_DRIFT,    /* they disagree by more than DRIFT_S       */
    WALLCLOCK_RESYNC_STOPPED   /* the chip has not moved: not a clock      */
} wallclock_resync_t;

/* Forget everything: no time known. Zero-initialised storage IS this state. */
void wallclock_reset(wallclock_t *w);

/*
 * Anchor from an RTC reading. `valid` is hal_rtc_get()'s return treated as a
 * boolean (its 0 and its -1 are both "no time known"), in which case the clock
 * is reset rather than anchored to a number nobody vouches for.
 */
void wallclock_anchor(wallclock_t *w, int valid, uint32_t epoch,
                      uint32_t now_us);

/*
 * Fold the elapsed microseconds into the anchor. Call it from the main loop's
 * slow cadence — see the feed contract above. Cheap: one subtraction and, once
 * a second, one add. The sub-second remainder is CARRIED, so irregular feeds
 * accumulate to the exact second instead of drifting.
 */
void wallclock_tick(wallclock_t *w, uint32_t now_us);

/*
 * The time now, without mutating anything: the anchor plus the microseconds
 * since the last tick. Returns 1 and writes *epoch when the clock is known,
 * 0 otherwise (leaving *epoch alone).
 */
int wallclock_now(const wallclock_t *w, uint32_t now_us, uint32_t *epoch);

/*
 * The MINUTE the clock is in — epoch / 60 — for the repaint edge: the caller
 * compares it once per pass and repaints the strip when it changes. UTC
 * minutes, which is also local minutes: every real UTC offset is a whole
 * number of minutes, so both roll over on the same second.
 * Returns 1 and writes *minute when the clock is known, 0 otherwise.
 */
int wallclock_minute(const wallclock_t *w, uint32_t now_us, uint32_t *minute);

/*
 * Re-anchor from a fresh RTC reading and report what the chip was doing,
 * writing the chip-minus-software difference into *drift_s when it is not
 * NULL (0 when there was nothing to compare against).
 *
 *   STOPPED — the chip reads exactly the second it read at the last anchor
 *             while the µs timer has moved on by WALLCLOCK_STOPPED_S or more.
 *             That is not a running clock: the state is reset to "no time
 *             known" and `rtc_epoch` is NOT adopted.
 *   DRIFT   — they disagree by more than WALLCLOCK_DRIFT_S. The chip wins (it
 *             is the thing that survives a suspend); the caller logs it.
 *   OK      — anything else, including the first anchor of a boot.
 *
 * `rtc_valid` 0 resets the clock and reports OK: "the chip has no time" is not
 * a disagreement.
 */
wallclock_resync_t wallclock_resync(wallclock_t *w, int rtc_valid,
                                    uint32_t rtc_epoch, uint32_t now_us,
                                    int32_t *drift_s);

#endif /* CORE_KERNEL_WALLCLOCK_H */
