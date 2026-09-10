/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/wheel.h — wheel acceleration + the A-Z locator.
 *
 * The driver reports a differenced position count, so one detent is one row no
 * matter how fast you spin — with 1200 songs that is a very long spin. Real
 * iPods accelerate: the faster the wheel turns, the more rows each detent
 * covers, with a big letter shown while it's flying so you can aim. Velocity
 * is derived from the wheel's tick rate and decays on its own; past a
 * threshold the unit of movement becomes the letter rather than the row.
 *
 * WHY THIS FILE EXISTS
 *
 * All of this was file-local static in kernel/main.c, which meson builds only
 * for target == 'hw', so the acceleration state machine — the idle reset, the
 * smoothed ticks-per-second estimate, the velocity curve, the letter-mode
 * latch and its hold window, the accumulator that turns raw ticks into rows,
 * the letter stepping and the guard that lets screens without letters fall
 * through to row scrolling — could only ever be checked by spinning the
 * wheel and watching. It is pure integer logic. Three things it touches are
 * not portable and are injected rather than reached for (the same seam
 * chrome.h uses for the marquee's clock):
 *
 *   - the clock (USEC_TIMER on the device);
 *   - what the initial letter of row `idx` on the CURRENT screen is, which
 *     needs the screen stack and the sorted song view;
 *   - the navigation click (the piezo, through the clicker setting).
 *
 * Unset is always safe: no clock reads as 0, no initial-letter source means
 * no screen has letters (so nothing ever letter-steps), no click is silent.
 * kernel_main registers all three before the UI loop runs.
 *
 * DEPENDENCIES: pp5022.h for CW_WHEEL_SENSITIVITY only (a constants header;
 * no MMIO). No hw/ access, no globals from main.c.
 */

#ifndef CORE_UI_WHEEL_H
#define CORE_UI_WHEEL_H

#include <stdint.h>

#include "../hal/hw/pp5022.h"     /* CW_WHEEL_SENSITIVITY: ticks per detent */

/* Wheel scroll feel. The driver reports the raw differenced position count (up
 * to ~half a rotation per poll), and a single slow detent crosses the wheel's
 * sensitivity gate at ~CW_WHEEL_SENSITIVITY (4) units. Dividing by 3 left a
 * remainder every detent, so the carry periodically double-stepped (move 1,1,2)
 * — felt like "it skipped, then jumped two". Matching the divisor to the
 * sensitivity makes one detent advance exactly one row; MAX_DELTA keeps the
 * 2-rows-per-event headroom (8/4) so a fast flick still scrolls quickly. */
#define WHEEL_CLICKS_PER_ITEM CW_WHEEL_SENSITIVITY   /* = 4: one detent, one row */
#define WHEEL_MAX_DELTA       (2 * CW_WHEEL_SENSITIVITY) /* fast flick: <=2 rows/evt */

#define WHEEL_VEL_MAX   8                  /* rows per detent at full tilt      */
/* (There is deliberately no "fast gap" threshold: the gap between drained
 * events measures the main loop's period, not the wheel. See wheel_accel_step.) */
#define WHEEL_IDLE_US   200000u            /* > this gap => new gesture, reset  */
#define WHEEL_AZ_VEL    3                  /* velocity at which the letter shows */
#define WHEEL_AZ_HOLD   500000u            /* ...and how long after the last tick */
/* In letter mode the plate is the control surface, not a hint, so it lingers
 * well past the last detent — it must not blink out while you are still
 * deciding which letter to stop on. */
#define WHEEL_AZ_HOLD_LETTER 1200000u

/* Print the measured wheel speed (ticks/s) under the letter — a tuning aid for
 * calibrating WHEEL_TPS_ACCEL against a real spin. Off by default. */
#define AZ_SHOW_TPS 0

/* ---------- The injected seams ---------------------------------------- */

/* Free-running microsecond clock (wraps; only differences are taken). */
typedef uint32_t (*wheel_clock_fn)(void);
void wheel_set_clock(wheel_clock_fn fn);

/* The A-Z locator letter for row `idx` on the current screen, or 0 on a
 * screen that has no alphabetical order to locate within. */
typedef char (*wheel_initial_fn)(int idx);
void wheel_set_initial_at(wheel_initial_fn fn);

/* The navigation click, sounded when the cursor actually moves. */
typedef void (*wheel_click_fn)(void);
void wheel_set_click(wheel_click_fn fn);

/* ---------- The state machine ------------------------------------------ */

/* Called once per wheel event with that event's RAW tick delta; returns the
 * rows-per-detent velocity for this event. wheel_move calls it; the scrub
 * path calls it directly for the same velocity without moving a list. */
int wheel_accel_step(int delta);

/* 1 while a detent should move a whole letter rather than a run of rows. */
int wheel_letter_mode(void);

/* Forget the gesture entirely (backlight off, panel wake, screen change). */
void wheel_accel_reset(void);

/* True while the list is flying past fast enough to want the letter cue. */
int wheel_accelerating(void);

/* Clock reading at the last wheel event (0 after a reset) — the album-list
 * cover pump paces its disk reads on how recently the wheel moved. */
uint32_t wheel_last_us(void);

#if AZ_SHOW_TPS
/* Smoothed wheel speed in ticks/second (96 ticks = one full rotation). */
uint32_t wheel_tps(void);
#endif

/* Land on the FIRST entry of the next (dir > 0) / previous letter present in
 * the current list; `sel` unchanged at either end or on a letterless screen. */
int list_letter_step(int sel, int count, int dir);

/* Apply a wheel event to a selection index in [0, count) with acceleration.
 * `accum` carries the sub-detent tick remainder between events. */
int wheel_move(int sel, int count, int8_t delta, int *accum);

#endif /* CORE_UI_WHEEL_H */
