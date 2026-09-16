/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/sleeptimer.h — the sleep timer's countdown: a wrap-safe minute
 * accumulator over the free-running microsecond clock.
 *
 * WHY THIS FILE EXISTS
 *
 * Settings > Playback > Sleep Timer arms a duration (15/30/60/90/120 min);
 * when it runs out the device pauses and takes the same suspend path a
 * two-second PLAY hold takes. The only hard part is the arithmetic: the
 * device's clock is the 1 MHz USEC_TIMER, a free-running 32-bit counter that
 * wraps every ~71.6 minutes, so a 120-minute countdown crosses it twice and a
 * naive `now - armed_at >= total_us` compare is simply wrong. This is that
 * arithmetic, pulled out of kernel/main.c so the host can run a whole
 * two-hour countdown across both wraps in a few milliseconds
 * (tests/ui/sleeptimer_test.c), the way ui/keyhold.c was.
 *
 * The shape is keyhold.c's: no clock of its own, no settings_t dependency
 * (the caller hands it minutes), fed once per main-loop pass with `now_us`,
 * and elapsed time is unsigned 32-bit subtraction between CONSECUTIVE feeds —
 * so a wrap between two feeds cancels out and the accumulated total is exact.
 * The sub-minute remainder is CARRIED, never truncated, so coarse or
 * irregular feeds still land on the exact minute.
 *
 * THE FEED CONTRACT: at least once every ~71 minutes. An unsigned delta
 * cannot tell 1 second from 1 second + 2^32 us, so a gap longer than one wrap
 * silently loses time. The main loop feeds every pass (halts are <= 10 ms), and
 * every path that stops it for longer either DISARMS the timer first (a
 * suspend, and the PMU standby it can escalate into, since that is entered
 * from inside one) or NEVER RETURNS to the loop (disk mode, the low-battery
 * shut-off, a power-down — all of which come back through a boot, where the
 * timer starts disarmed again). A future long blocking path INSIDE the loop (a
 * multi-minute library rescan, say) must do one or the other, or feed.
 *
 * Behaviour worth stating, because none of it is arithmetic:
 *   - It fires regardless of what the player is doing (a paused or idle device
 *     still sleeps — that is what the feature means) and regardless of screen,
 *     Hold switch included: a locked, pocketed device is the canonical case.
 *   - It is NOT reset by input. It is a duration, not an idle timeout.
 *   - It fires exactly ONCE and disarms itself in the same call, so no later
 *     feed (after a wrap, or after the caller was away) can resurrect it.
 *   - Remaining counts the way the original iPod's did: it reads the full
 *     duration for the first minute, and never reaches 0 while armed.
 */

#ifndef CORE_UI_SLEEPTIMER_H
#define CORE_UI_SLEEPTIMER_H

#include <stdint.h>

typedef struct {
    uint8_t  armed;          /* a countdown is running                        */
    uint8_t  total_min;      /* the duration it was armed with; 0 = off       */
    uint16_t elapsed_min;    /* whole minutes accumulated since the arm       */
    uint32_t last_us;        /* the clock at the last feed                    */
    uint32_t acc_us;         /* sub-minute remainder, always < 60 000 000     */
} sleeptimer_t;

typedef enum {
    SLEEPTIMER_NONE = 0,     /* nothing to do this pass                       */
    SLEEPTIMER_TICK,         /* a whole minute passed: the token changed      */
    SLEEPTIMER_FIRE          /* the duration ran out — sleep (already disarmed) */
} sleeptimer_event_t;

/* Zero state == disarmed. Also how any suspend, standby or reset forgets a
 * running countdown. */
void sleeptimer_reset(sleeptimer_t *t);

/* Arm for `minutes` from `now_us`, discarding any countdown in progress.
 * `minutes` <= 0 (or out of range) disarms. Re-arming with the SAME duration
 * is still a fresh start — the Settings row cycling around to the value it
 * already had is a deliberate restart. */
void sleeptimer_arm(sleeptimer_t *t, int minutes, uint32_t now_us);

int sleeptimer_armed(const sleeptimer_t *t);

/* The duration it was armed with; 0 when off. main.c compares this against
 * g_settings.sleep_timer_min to decide whether the two sides have drifted. */
int sleeptimer_total_min(const sleeptimer_t *t);

/* Whole minutes left, rounded UP: the chosen duration for the first minute,
 * then one less each minute, and never 0 while armed. 0 when off. */
int sleeptimer_remaining_min(const sleeptimer_t *t);

/*
 * Feed one clock sample. Returns FIRE on the pass the duration ran out (the
 * timer is already disarmed by then), TICK on the pass a whole minute
 * elapsed without expiring (the caller repaints), NONE otherwise.
 */
sleeptimer_event_t sleeptimer_feed(sleeptimer_t *t, uint32_t now_us);

/*
 * Write the status token ("SLEEP 45") into `buf` and return its length. Off,
 * or a buffer too small for the whole token, writes "" and returns 0 — a
 * truncated "SLEEP 1" for 120 minutes would be a lie, so it draws nothing.
 * Needs 10 bytes for the longest token.
 */
int sleeptimer_token(const sleeptimer_t *t, char *buf, int buf_sz);

#endif /* CORE_UI_SLEEPTIMER_H */
