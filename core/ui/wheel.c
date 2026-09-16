/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/wheel.c — wheel acceleration + the A-Z locator.
 *
 * Moved out of kernel/main.c with three substitutions and nothing else:
 * mmio_read32(USEC_TIMER_ADDR) became now_us(), list_initial_at() became
 * initial_at(), ui_click() became click() — each a thin call through the
 * hook registered from main.c. See wheel.h for why.
 */

#include "wheel.h"

static wheel_clock_fn       g_clock;
static wheel_initial_fn     g_initial_at;
static wheel_click_fn       g_click;
static wheel_letter_step_fn g_letter_step;

void wheel_set_clock(wheel_clock_fn fn)        { g_clock = fn; }
void wheel_set_initial_at(wheel_initial_fn fn) { g_initial_at = fn; }
void wheel_set_click(wheel_click_fn fn)        { g_click = fn; }
void wheel_set_letter_step(wheel_letter_step_fn fn) { g_letter_step = fn; }

static uint32_t now_us(void)
{
    return g_clock ? g_clock() : 0u;
}

static char initial_at(int idx)
{
    return g_initial_at ? g_initial_at(idx) : 0;
}

static void click(void)
{
    if (g_click) g_click();
}

static uint32_t g_wheel_last_us;
static int      g_wheel_vel = 1;
static int      g_wheel_letters;       /* 1 = a detent moves a whole letter    */

/*
 * Called once per wheel event with that event's RAW tick delta.
 *
 * Speed is measured as ticks per second, NOT as the gap between events. The
 * gap is the wrong signal: clickwheel_service() latches motion in the 100 Hz
 * ISR and clickwheel_get_event() drains the accumulator, so an "event" arrives
 * once per main-loop pass — the gap therefore measures how long the loop took
 * (render, disk, decode), not how fast the wheel is turning. Deriving velocity
 * from it meant a fast spin during playback, when passes are longest, looked
 * SLOWER than the same spin on an idle menu, and the top of the range was
 * effectively unreachable.
 *
 * delta is ticks accumulated since the last drain, so delta/dt is real angular
 * velocity and is independent of how often we happen to drain. CW_CLICKS_PER_ROT
 * is 96, so one turn a second is ~96 ticks/s.
 */
#define WHEEL_TPS_ACCEL   50u    /* above this, start multiplying rows      */
#define WHEEL_TPS_SPAN   200u    /* ticks/s from vel 1 to WHEEL_VEL_MAX     */

static uint32_t g_wheel_tps;     /* smoothed ticks/second                   */

int wheel_accel_step(int delta)
{
    uint32_t now = now_us();
    uint32_t dt  = now - g_wheel_last_us;
    g_wheel_last_us = now;

    if (dt > WHEEL_IDLE_US) {         /* new gesture: forget the old one */
        g_wheel_vel = 1;
        g_wheel_tps = 0;
        /*
         * ...except the letter latch, while the PLATE is still up. The plate
         * is the control surface in letter mode, not a hint, so the thing on
         * screen and the thing the wheel does have to share one clock: a
         * quarter-second pause used to drop the wheel back to rows (200 ms)
         * while the plate stayed up for a second more, so the next detent
         * both moved one row and took the plate down — the control changing
         * meaning under a thumb that had only paused to read it.
         *
         * So the latch outlives a pause shorter than WHEEL_AZ_HOLD_LETTER,
         * which is the hold the guide already describes ("the letter stays for
         * just over a second"). Speed still resets: lift for 300 ms and the
         * next detent is ONE letter, not eight rows' worth of them.
         */
        if (!g_wheel_letters || dt >= WHEEL_AZ_HOLD_LETTER) {
            g_wheel_letters = 0;
        }
        return 1;
    }
    if (dt < 1000u) {
        dt = 1000u;                   /* floor: keep the divide sane */
    }

    uint32_t mag = (uint32_t)(delta < 0 ? -delta : delta);
    uint32_t tps = mag * 1000000u / dt;
    /* Light smoothing so one long loop pass can't spike or drop the estimate. */
    g_wheel_tps = (g_wheel_tps * 3u + tps) / 4u;

    if (g_wheel_tps <= WHEEL_TPS_ACCEL) {
        g_wheel_vel = 1;
    } else {
        uint32_t over = g_wheel_tps - WHEEL_TPS_ACCEL;
        uint32_t v    = 1u + (over * (WHEEL_VEL_MAX - 1u)) / WHEEL_TPS_SPAN;
        g_wheel_vel   = (int)(v > (uint32_t)WHEEL_VEL_MAX ? (uint32_t)WHEEL_VEL_MAX : v);
    }

    /*
     * Letter mode engages at exactly the speed the A-Z plate appears, because
     * the plate IS the indicator for it: seeing the letter means the wheel is
     * stepping letters. Having a second, higher threshold created a band where
     * the letter was up but the wheel was still grinding through songs, which
     * reads as the cue simply not working.
     *
     * Latched for the rest of the gesture (cleared on the idle gap at the top
     * of this function). Re-testing the speed each detent would flip the unit
     * back and forth mid-spin as the estimate wavers around the threshold —
     * the control would change meaning under your thumb.
     */
    if (g_wheel_vel >= WHEEL_AZ_VEL) {
        g_wheel_letters = 1;
    }
    return g_wheel_vel;
}

/* 1 while a detent should move a whole letter rather than a run of rows. */
int wheel_letter_mode(void)
{
    return g_wheel_letters;
}

/* Smoothed wheel speed in ticks/second (96 ticks = one full rotation).
 * Only compiled in for the AZ_SHOW_TPS tuning readout. */
#if AZ_SHOW_TPS
uint32_t wheel_tps(void)
{
    return g_wheel_tps;
}
#endif

/* Forget the gesture entirely. Called when the UI is taken away from the user
 * (backlight off, panel wake, screen change) so a spin that ended before the
 * screen slept can't still be "in progress" when they come back to it. */
void wheel_accel_reset(void)
{
    g_wheel_vel     = 1;
    g_wheel_letters = 0;
    g_wheel_last_us = 0;
    g_wheel_tps     = 0;
}

/* True while the A-Z plate should be up: letter mode, within the hold of the
 * last detent. The plate stays up for the whole gesture: it IS the control
 * surface then, not a hint, so it must not blink out between detents.
 * Letter mode is the only test — wheel_accel_step latches it in the same
 * call that brings the velocity to WHEEL_AZ_VEL, so a velocity test here
 * (and the shorter non-letter hold that once went with it) could never
 * decide anything. */
int wheel_accelerating(void)
{
    if (!g_wheel_letters) return 0;
    return (uint32_t)(now_us() - g_wheel_last_us) < WHEEL_AZ_HOLD_LETTER;
}

uint32_t wheel_last_us(void)
{
    return g_wheel_last_us;
}

/*
 * Letter stepping: land on the FIRST entry of the next/previous letter present
 * in the list.
 *
 * Row acceleration alone tops out at WHEEL_VEL_MAX rows per detent, which on a
 * 1200-song list still means a long spin and a letter cue that only tells you
 * where you happen to have landed. Once the wheel is being spun in earnest the
 * useful unit stops being the row and becomes the letter — one detent, one
 * letter, so you can aim at "S" instead of scrubbing toward it.
 *
 * Walks the already-sorted view, so it is O(entries in the current letter) and
 * needs no index. Returns `sel` unchanged when there is no further letter, so
 * the ends of the list stop cleanly instead of wrapping under your thumb.
 */
int list_letter_step(int sel, int count, int dir)
{
    if (count <= 0) {
        return sel;
    }
    char cur = initial_at(sel);
    if (cur == 0) {
        return sel;                 /* screen has no alphabetised order */
    }
    int i = sel;
    if (dir > 0) {
        while (i < count - 1 && initial_at(i + 1) == cur) i++;
        if (i >= count - 1) return sel;          /* already in the last letter */
        return i + 1;                            /* first entry of the next    */
    }
    /* Backwards: to the head of this letter, and if already there, to the head
     * of the previous one — so a back-step is never a no-op mid-letter. */
    while (i > 0 && initial_at(i - 1) == cur) i--;
    if (i != sel) {
        return i;
    }
    if (i == 0) return sel;                      /* already in the first letter */
    char prev = initial_at(i - 1);
    i--;
    while (i > 0 && initial_at(i - 1) == prev) i--;
    return i;
}

/* Apply a wheel event to a selection index in [0, count) with acceleration. */
int wheel_move(int sel, int count, int8_t delta, int *accum)
{
    /* Feed the RAW delta: it is the tick count since the last drain, which is
     * what carries the wheel's speed. The clamp below is for the row maths and
     * would throw exactly that information away. */
    int vel = wheel_accel_step(delta);
    int wd = delta;
    if (wd >  WHEEL_MAX_DELTA) wd =  WHEEL_MAX_DELTA;
    if (wd < -WHEEL_MAX_DELTA) wd = -WHEEL_MAX_DELTA;
    *accum += wd;
    int move = *accum / WHEEL_CLICKS_PER_ITEM;
    *accum -= move * WHEEL_CLICKS_PER_ITEM;

    int old = sel;

    /* Sustained fast spin on an alphabetised list: one detent = one letter.
     * Stepping rows faster still makes you scrub past everything between here
     * and where you're going; stepping letters lets you aim. Falls through to
     * row acceleration on screens with no alphabetical order (list_letter_step
     * returns `sel` unchanged there). */
    /*
     * `initial_at(sel) != 0` is the load-bearing half of this guard: it
     * asks "does THIS screen have letters to step through at all". Without it
     * the branch was taken on every screen, list_letter_step returned `sel`
     * unchanged on the ones with no alphabetical order (menus, settings, the
     * queue, a tracklist), and the early return below meant the wheel never
     * fell through to row scrolling — so spinning fast on those screens did
     * nothing at all.
     */
    if (move != 0 && wheel_letter_mode() && initial_at(sel) != 0) {
        int dir  = (move > 0) ? 1 : -1;
        int step = (move > 0) ? move : -move;
        for (int i = 0; i < step; i++) {
            /* The index when the screen has one (ui/letterindex.c, registered
             * by kernel_main): O(log runs) instead of the walk's O(rows in
             * this letter), which on a 6000-song library is the difference
             * between a detent and a stutter. Unset falls back to the walk,
             * which needs nothing but initial_at — so an unregistered seam is
             * slower, never wrong. */
            int next = g_letter_step ? g_letter_step(sel, count, dir)
                                     : list_letter_step(sel, count, dir);
            if (next == sel) break;              /* ran out of letters */
            sel = next;
        }
        if (sel != old) {
            click();
        }
        return sel;
    }

    /* Acceleration: one detent still moves one row when you turn the wheel
     * deliberately (vel 1), but a fast spin covers up to WHEEL_VEL_MAX rows per
     * detent — the difference between 1200 songs being reachable and not. */
    move *= vel;
    sel += move;
    if (sel < 0)          sel = 0;
    if (sel >= count)     sel = count - 1;
    if (sel != old) {
        click();        /* click only when the cursor actually advances */
    }
    return sel;
}
