/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/keyhold.h — press-length arbitration for one button: tap or hold.
 *
 * A button that means one thing when tapped and another when held cannot be
 * decided at its down-edge, because the length of the press is only known
 * once it is released or once the hold threshold passes. This is the little
 * machine that makes that decision, fed once per main-loop pass with the
 * button's LIVE state (clickwheel_buttons(), not the latched down-edge event)
 * and the clock.
 *
 * WHY THIS FILE EXISTS
 *
 * FOUR of this device's five buttons are press-length buttons, and all four
 * are arbitrated here: PLAY (tap = pause, ~2 s hold = sleep), MENU (tap =
 * back one screen, 1 s hold = the main menu) and RIGHT/LEFT (tap = skip a
 * track, 0.5 s hold = seek inside it, through the seekhold machine in
 * ui/gesture.c, which is a keyhold plus an aim). Nothing in this file assumes
 * one threshold or one instance: `hold_us` is an argument of every feed and
 * all the state is per-instance, so a caller keeps as many instances as it
 * has such buttons.
 *
 * PLAY was the first, and is why this file exists. It is exactly such a
 * button — a tap toggles pause, a ~2 s hold sleeps the device — and until
 * this existed kernel/main.c decided the two halves in two different places:
 * the hold was timed from live state, but the pause toggle
 * fired on the DOWN-EDGE EVENT, unconditionally. So a hold-to-sleep paused the
 * music first and the wake did not resume it; and a hold while already paused
 * played for two seconds, went to sleep, and then resumed on wake. SELECT on
 * Now Playing already did this right (record the down-edge, decide on release
 * or at the threshold), and this is that logic pulled out so the host can
 * test it, the way ui/wheel.c was (commit ef8ee4f).
 *
 * Pure integer logic, no clock of its own: the caller passes `now_us` (the
 * free-running 1 MHz USEC_TIMER on the device). Elapsed time is unsigned
 * 32-bit subtraction, so a press that straddles the counter wrap is timed
 * correctly.
 */

#ifndef CORE_UI_KEYHOLD_H
#define CORE_UI_KEYHOLD_H

#include <stdint.h>

typedef struct {
    uint8_t  down;      /* a press is being timed                          */
    uint8_t  no_tap;    /* this press's tap was consumed by someone else    */
    uint8_t  no_hold;   /* ...and so was its hold (keyhold_void)            */
    uint8_t  grace;     /* idle feeds a pre-press swallow may wait for its press */
    uint8_t  fired;     /* KEYHOLD_HOLD has already been reported for it    */
    uint32_t down_us;   /* when the press began                             */
} keyhold_t;

typedef enum {
    KEYHOLD_NONE = 0,   /* nothing to do this pass                          */
    KEYHOLD_TAP,        /* released before the threshold: the short action  */
    KEYHOLD_HOLD,       /* still down at the threshold: the long action     */
} keyhold_action_t;

/* Zero state == idle; call this to forget a press part-way (e.g. the Hold
 * switch went on under the finger). */
void keyhold_reset(keyhold_t *k);

/*
 * Feed one sample. `is_down` is the button's live state, `now_us` the clock,
 * `hold_us` the threshold. Returns at most ONE action per press: HOLD the
 * first pass the press has lasted >= hold_us (while still down), or TAP on
 * the release if the threshold was never reached. A press whose tap was
 * swallowed (keyhold_swallow_tap) still reports HOLD; its release is silent.
 * Once HOLD has fired the rest of that press is silent too — the caller may
 * be away for a long time (the device was asleep) and must not see a TAP
 * when the finger finally comes off.
 */
keyhold_action_t keyhold_feed(keyhold_t *k, int is_down, uint32_t now_us,
                              uint32_t hold_us);

/*
 * The current press's down-edge was consumed elsewhere — it woke the
 * backlight, or dismissed a modal — so it must not ALSO produce the short
 * action on release. The long action survives: holding PLAY from a dark
 * screen is still how you turn the device off. Also valid BEFORE the press
 * has been fed: the event that consumes a down-edge can run before the live
 * sampler shows it, so a swallow while idle claims the next press's tap (and
 * only that one — it is cleared on release).
 */
void keyhold_swallow_tap(keyhold_t *k);

/*
 * Stronger than a swallow: this press produces NEITHER action. The press that
 * dismisses a modal, or that lights a dark screen, is spent on that — and for
 * a button whose hold would also do something (MENU jumping home, RIGHT
 * seeking) the hold has to go with the tap, or the same press both dismisses
 * the charging screen and walks the user back to the main menu. PLAY is the
 * documented exception and keeps keyhold_swallow_tap: holding it from a dark
 * screen is still how the device is turned off.
 *
 * Valid before the press has been fed, with the same grace rule as
 * keyhold_swallow_tap: it claims the next press, and only that one.
 */
void keyhold_void(keyhold_t *k);

/*
 * 1 while the press being timed is down AND has already produced
 * KEYHOLD_HOLD — i.e. the long action is in force right now. The caller's
 * long action may be a continuous one (a seek that aims while the finger
 * stays on the button) rather than a single event.
 */
int keyhold_held(const keyhold_t *k);

/* When the press being timed began (valid after KEYHOLD_HOLD is returned:
 * the caller's long action may want to keep timing from the same origin). */
uint32_t keyhold_down_us(const keyhold_t *k);

#endif /* CORE_UI_KEYHOLD_H */
