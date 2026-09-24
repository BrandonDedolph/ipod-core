/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/idlesleep.h — when a device nobody is using puts itself to sleep.
 *
 * WHY THIS EXISTS (device, 2026-09-24). The event log pulled that day showed
 * the same thing twice: a track paused, the drive parked, the backlight off,
 * the codec cold — and the main loop still awake, sampling the battery every
 * 5 s, for ten and for twelve hours, until the cell was at 3556 mV (0 %).
 * Boot 27: 3943 -> 3925 mV in 32 minutes paused (-33 mV/h), then dark to
 * 3556; boot 35: 3972 -> 3556 mV across a 1.9 MB `dropped` gap at the
 * paused-idle narration rate (~12 h). At -33 mV/h the middle of the discharge
 * curve (30 mV per 10 %) is ~11 %/h: "it went down 20 % in two hours doing
 * nothing", which is exactly what the owner reported. On the 600 mAh cell
 * that is 35-40 mA — the device is awake, it just has nothing to do.
 *
 * The suspend (suspend_to_ram, kernel/main.c) with its 30-minute escalation
 * to a PMU standby was reachable from exactly two places: a PLAY hold and the
 * sleep timer. Pause and put it down, and nothing ever took the device out of
 * the main loop. Apple's firmware sleeps a paused iPod after two minutes of
 * no input; this is that rule.
 *
 * THE RULE. Nothing playing (the DAC is stopped: paused, or no track), no
 * input and no other reason to stay up for IDLESLEEP_TIMEOUT_US -> sleep.
 * "Another reason to stay up" is what the caller passes as `busy`:
 *   - the DAC running (player_playing — NOT player_active, which is true
 *     across a pause and is the wrong gate for a power decision);
 *   - external power: on the cable there is nothing to save (the suspend
 *     does not escalate there either) and the Battery page is what the
 *     bench watches, so a docked device stays awake, as it always has;
 *   - a sleep timer armed: the user has stated a plan, and a suspend disarms
 *     the timer, so the plan wins.
 * The countdown restarts at the most recent of the last input and the last
 * busy pass, so an unplug or a pause starts a fresh two minutes rather than
 * sleeping on the spot.
 *
 * ONE-SHOT OFF. suspend_to_ram's escalation can come back with the PMU
 * having refused the standby command; enter_standby has by then stopped the
 * player and relit the screen. Retrying every 32 minutes would stop the
 * player each time, so a refusal latches this off for the session.
 *
 * Pure: the caller passes the clock (USEC_TIMER, wraps every ~71.6 min; all
 * compares are differences). Host-tested in tests/kernel/idlesleep_test.c.
 */
#ifndef CORE_KERNEL_IDLESLEEP_H
#define CORE_KERNEL_IDLESLEEP_H

#include <stdint.h>

/* No input and nothing to do for this long -> suspend. Apple's figure. */
#define IDLESLEEP_TIMEOUT_US   (2u * 60u * 1000000u)      /* 2 minutes */

typedef struct {
    uint32_t last_busy_us;     /* the most recent moment there was a reason to be up */
    uint32_t seen_input_us;    /* the input stamp as of the last feed              */
    int      off;              /* latched: a refused PMU standby                    */
} idlesleep_t;

/* Arm from `now_us`: the countdown starts here (boot, or the loop's entry). */
void idlesleep_reset(idlesleep_t *s, uint32_t now_us);

/* Latch off for the session (a refused PMU standby). */
void idlesleep_off(idlesleep_t *s);
int  idlesleep_is_off(const idlesleep_t *s);

/*
 * Feed once per main-loop pass. `input_us` is the loop's last-input stamp
 * (any change to it is an input); `busy` is 1 while there is a reason to
 * stay up (see above). Returns 1 on the pass the timeout elapses — once: the
 * countdown restarts from that pass, so a caller that ignores the answer is
 * asked again a timeout later, not every pass.
 */
int  idlesleep_feed(idlesleep_t *s, uint32_t now_us, uint32_t input_us, int busy);

/* Microseconds of idle so far (0 while busy or off); for a status line. */
uint32_t idlesleep_idle_us(const idlesleep_t *s, uint32_t now_us);

#endif /* CORE_KERNEL_IDLESLEEP_H */
