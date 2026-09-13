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
 * PLAY is exactly such a button — a tap toggles pause, a ~2 s hold sleeps the
 * device — and until this existed kernel/main.c decided the two halves in two
 * different places: the hold was timed from live state, but the pause toggle
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
 * screen is still how you turn the device off. No-op when nothing is down.
 */
void keyhold_swallow_tap(keyhold_t *k);

/* When the press being timed began (valid after KEYHOLD_HOLD is returned:
 * the caller's long action may want to keep timing from the same origin). */
uint32_t keyhold_down_us(const keyhold_t *k);

#endif /* CORE_UI_KEYHOLD_H */
