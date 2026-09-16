/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/keyhold_test.c — the tap-vs-hold arbitration (core/ui/keyhold.c)
 * on the host.
 *
 * THE POINT OF THIS FILE. PLAY is "tap = pause, hold = sleep", and the bug
 * this unit replaced was that the two halves were decided in two places: the
 * pause toggled on the down-edge event, unconditionally, while the hold was
 * timed from live state. The observable consequences — hold-to-sleep paused
 * the music before sleeping so wake did not resume it; hold while paused
 * played for two seconds first — are exactly what these cases pin: a press
 * produces ONE action, and it is decided by its length, never at its start.
 *
 * Every case is a sequence of (is_down, clock) samples, the way the main
 * loop feeds it once per pass, and the assertions are the action returned on
 * each sample.
 */

#include <stdio.h>

#include "keyhold.h"
#include "../xfail.h"

#define HOLD_US 2000000u

static int feed_n(keyhold_t *k, int is_down, uint32_t *now, uint32_t step,
                  int n, keyhold_action_t want, xfail_ctx *c, const char *what)
{
    int ok = 1;
    for (int i = 0; i < n; i++) {
        *now += step;
        keyhold_action_t a = keyhold_feed(k, is_down, *now, HOLD_US);
        if (a != want) {
            ok = 0;
        }
    }
    xpect(c, what, ok);
    return ok;
}

int main(void)
{
    xfail_ctx c = { "keyhold", 0, 0, 0 };
    keyhold_t k;
    uint32_t now = 1000u;

    /* --- idle: released samples do nothing forever ---------------------- */
    keyhold_reset(&k);
    feed_n(&k, 0, &now, 10000u, 50, KEYHOLD_NONE, &c,
           "idle: a released button never produces an action");

    /* --- tap: down, then up well before the threshold ------------------- */
    keyhold_reset(&k);
    xpect(&c, "tap: the down-edge itself is silent (nothing decided yet)",
          keyhold_feed(&k, 1, now += 10000u, HOLD_US) == KEYHOLD_NONE);
    feed_n(&k, 1, &now, 10000u, 20, KEYHOLD_NONE, &c,
           "tap: held under the threshold, still silent");
    xpect(&c, "tap: the RELEASE is the short action",
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_TAP);
    xpect(&c, "tap: exactly once — the next released sample is silent",
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_NONE);

    /* --- hold: down through the threshold --------------------------------- */
    keyhold_reset(&k);
    uint32_t t_down = now + 10000u;
    keyhold_feed(&k, 1, t_down, HOLD_US);
    now = t_down;
    /* 199 passes of 10 ms = 1.99 s: one sample short of the threshold. */
    feed_n(&k, 1, &now, 10000u, 199, KEYHOLD_NONE, &c,
           "hold: silent right up to the threshold");
    xpect(&c, "hold: the first pass AT the threshold is the long action",
          keyhold_feed(&k, 1, now += 10000u, HOLD_US) == KEYHOLD_HOLD);
    xpect(&c, "hold: the origin is the down-edge time, for the caller's timer",
          keyhold_down_us(&k) == t_down);
    feed_n(&k, 1, &now, 10000u, 300, KEYHOLD_NONE, &c,
           "hold: no second HOLD while the finger stays down (5 s later)");
    xpect(&c, "hold: the eventual release is NOT a tap",
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_NONE);
    /* This is the wake case: the device slept for a long time under the
     * finger, the finger came off, and there must be nothing to act on. */

    /* --- swallowed tap: the down-edge woke the backlight ------------------ */
    keyhold_reset(&k);
    keyhold_feed(&k, 1, now += 10000u, HOLD_US);
    keyhold_swallow_tap(&k);
    feed_n(&k, 1, &now, 10000u, 10, KEYHOLD_NONE, &c,
           "swallow: still silent while down");
    xpect(&c, "swallow: the release of a consumed press is silent",
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_NONE);
    xpect(&c, "swallow: the NEXT press is a fresh one (its tap counts)",
          keyhold_feed(&k, 1, now += 10000u, HOLD_US) == KEYHOLD_NONE &&
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_TAP);

    /* --- swallowed tap, but HELD: the long action survives ---------------- *
     * Holding PLAY from a dark screen is how the device is turned off; the
     * wake-swallow must not eat that. */
    keyhold_reset(&k);
    keyhold_feed(&k, 1, now += 10000u, HOLD_US);
    keyhold_swallow_tap(&k);
    now += HOLD_US;
    xpect(&c, "swallow+hold: the hold still fires",
          keyhold_feed(&k, 1, now, HOLD_US) == KEYHOLD_HOLD);
    xpect(&c, "swallow+hold: and its release is silent",
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_NONE);

    /* --- swallow before the sampler showed the press ---------------------- *
     * The event drain (which swallows) can run in the same pass BEFORE the
     * 100 Hz sample that makes clickwheel_buttons() report the press, so the
     * swallow lands while keyhold still thinks nothing is down. It must claim
     * the press that follows — that press IS the wake — and only that one. */
    keyhold_reset(&k);
    keyhold_swallow_tap(&k);
    xpect(&c, "early swallow: the press it was meant for stays silent",
          keyhold_feed(&k, 1, now += 10000u, HOLD_US) == KEYHOLD_NONE &&
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_NONE);
    xpect(&c, "early swallow: the press after it is a fresh one",
          keyhold_feed(&k, 1, now += 10000u, HOLD_US) == KEYHOLD_NONE &&
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_TAP);
    keyhold_reset(&k);
    keyhold_swallow_tap(&k);
    now += 10000u;
    keyhold_feed(&k, 1, now, HOLD_US);
    now += HOLD_US;
    xpect(&c, "early swallow + hold: the hold still fires",
          keyhold_feed(&k, 1, now, HOLD_US) == KEYHOLD_HOLD);

    /* --- a swallow whose press never comes lapses --------------------------- *
     * The race is one pass wide; after a few idle samples the swallow is
     * stale, and a PLAY tap a minute later must still pause. */
    keyhold_reset(&k);
    keyhold_swallow_tap(&k);
    feed_n(&k, 0, &now, 10000u, 5, KEYHOLD_NONE, &c,
           "stale swallow: idle feeds are silent");
    xpect(&c, "stale swallow: the next press is a fresh one (its tap counts)",
          keyhold_feed(&k, 1, now += 10000u, HOLD_US) == KEYHOLD_NONE &&
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_TAP);

    /* --- reset mid-press: the Hold switch went on under the finger -------- */
    keyhold_reset(&k);
    keyhold_feed(&k, 1, now += 10000u, HOLD_US);
    keyhold_reset(&k);
    xpect(&c, "reset mid-press: the release is silent",
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_NONE);

    /* --- clock wrap: a press that straddles 2^32 us is timed correctly ---- */
    keyhold_reset(&k);
    now = 0xFFFFFFFFu - 500000u;             /* 0.5 s before the wrap */
    keyhold_feed(&k, 1, now, HOLD_US);
    now += 1000000u;                          /* 0.5 s past the wrap: 1.0 s in */
    xpect(&c, "wrap: 1 s into a press across the wrap is not yet a hold",
          keyhold_feed(&k, 1, now, HOLD_US) == KEYHOLD_NONE);
    now += 1000000u;                          /* 2.0 s in */
    xpect(&c, "wrap: 2 s into a press across the wrap IS a hold",
          keyhold_feed(&k, 1, now, HOLD_US) == KEYHOLD_HOLD);

    /* --- void: the press produces NEITHER action -------------------------- *
     * A press that dismissed the charging screen or lit a dark panel is spent
     * on that. For PLAY the hold survives (that is how you power off), but
     * MENU's hold walks you to the main menu and RIGHT's seeks — neither may
     * ride on a press the user aimed at a modal. */
    keyhold_reset(&k);
    now = 1000u;
    keyhold_feed(&k, 1, now += 10000u, HOLD_US);
    keyhold_void(&k);
    feed_n(&k, 1, &now, 10000u, 600, KEYHOLD_NONE, &c,
           "void: silent past three times the threshold");
    xpect(&c, "void: and the release is silent too",
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_NONE);

    /* Before the press, the same drain-then-sample race keyhold_swallow_tap
     * covers: the void claims the next press, tap AND hold. */
    keyhold_reset(&k);
    keyhold_void(&k);
    xpect(&c, "early void: the press it was meant for is silent at the down-edge",
          keyhold_feed(&k, 1, now += 10000u, HOLD_US) == KEYHOLD_NONE);
    feed_n(&k, 1, &now, 10000u, 300, KEYHOLD_NONE, &c,
           "early void: no hold from the press it claimed");
    xpect(&c, "early void: no tap from it either",
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_NONE);

    /* ...and only that one: the void clears on the release. */
    xpect(&c, "void clears on release: the NEXT press holds",
          keyhold_feed(&k, 1, now += 10000u, HOLD_US) == KEYHOLD_NONE &&
          keyhold_feed(&k, 1, now += HOLD_US, HOLD_US) == KEYHOLD_HOLD);
    keyhold_feed(&k, 0, now += 10000u, HOLD_US);

    /* A void whose press never arrives lapses with the same grace, so a
     * gesture a minute later is not eaten by it. */
    keyhold_reset(&k);
    keyhold_void(&k);
    feed_n(&k, 0, &now, 10000u, 5, KEYHOLD_NONE, &c,
           "stale void: idle feeds are silent");
    xpect(&c, "stale void: the next press is a fresh one (its tap counts)",
          keyhold_feed(&k, 1, now += 10000u, HOLD_US) == KEYHOLD_NONE &&
          keyhold_feed(&k, 0, now += 10000u, HOLD_US) == KEYHOLD_TAP);

    /* --- keyhold_held(): the long action is in force RIGHT NOW ------------ *
     * A seek aims for as long as the button is down, so the caller needs the
     * state, not just the edge. */
    keyhold_reset(&k);
    xpect(&c, "held: idle is not held", keyhold_held(&k) == 0);
    keyhold_feed(&k, 1, now += 10000u, HOLD_US);
    xpect(&c, "held: 0 before the threshold", keyhold_held(&k) == 0);
    xpect(&c, "held: 1 from the pass the hold fired",
          keyhold_feed(&k, 1, now += HOLD_US, HOLD_US) == KEYHOLD_HOLD &&
          keyhold_held(&k) == 1);
    feed_n(&k, 1, &now, 10000u, 10, KEYHOLD_NONE, &c,
           "held: the passes after it are silent");
    xpect(&c, "held: still 1 while the finger stays down", keyhold_held(&k) == 1);
    keyhold_feed(&k, 0, now += 10000u, HOLD_US);
    xpect(&c, "held: 0 once released", keyhold_held(&k) == 0);
    /* A voided press never fires, so it is never "held". */
    keyhold_reset(&k);
    keyhold_feed(&k, 1, now += 10000u, HOLD_US);
    keyhold_void(&k);
    keyhold_feed(&k, 1, now += HOLD_US, HOLD_US);
    xpect(&c, "held: a voided press is never held", keyhold_held(&k) == 0);
    keyhold_feed(&k, 0, now += 10000u, HOLD_US);

    /* --- two instances, two thresholds ------------------------------------ *
     * PLAY sleeps at 2 s, MENU jumps home at 1 s, RIGHT seeks at 0.5 s; all
     * of them are keyhold_t. Nothing in this file may assume one length, and
     * one instance must not disturb another. */
    {
        keyhold_t fast, slow;
        const uint32_t FAST_US = 500000u, SLOW_US = 2000000u;
        keyhold_reset(&fast);
        keyhold_reset(&slow);
        uint32_t t0 = 7000u, t = t0;
        keyhold_action_t fa = keyhold_feed(&fast, 1, t, FAST_US);
        keyhold_action_t sa = keyhold_feed(&slow, 1, t, SLOW_US);
        int ok = (fa == KEYHOLD_NONE && sa == KEYHOLD_NONE);
        int fast_holds = 0, slow_holds = 0;
        for (int i = 0; i < 300; i++) {          /* 3 s of 10 ms passes */
            t += 10000u;
            if (keyhold_feed(&fast, 1, t, FAST_US) == KEYHOLD_HOLD) {
                fast_holds++;
                ok = ok && (t - t0 == FAST_US);  /* fired at ITS threshold */
            }
            if (keyhold_feed(&slow, 1, t, SLOW_US) == KEYHOLD_HOLD) {
                slow_holds++;
                ok = ok && (t - t0 == SLOW_US);
            }
        }
        xpect(&c, "two thresholds: each instance fires once, at its own length",
              ok && fast_holds == 1 && slow_holds == 1);
        /* And the tap of one is not eaten by the other's void. */
        keyhold_reset(&fast);
        keyhold_reset(&slow);
        keyhold_void(&fast);
        xpect(&c, "two instances: voiding one leaves the other's tap alone",
              keyhold_feed(&fast, 1, t += 10000u, FAST_US) == KEYHOLD_NONE &&
              keyhold_feed(&slow, 1, t, SLOW_US) == KEYHOLD_NONE &&
              keyhold_feed(&fast, 0, t += 10000u, FAST_US) == KEYHOLD_NONE &&
              keyhold_feed(&slow, 0, t, SLOW_US) == KEYHOLD_TAP);
    }

    /* --- the threshold is >=, like SELECT's ------------------------------- */
    keyhold_reset(&k);
    now = 5000u;
    keyhold_feed(&k, 1, now, HOLD_US);
    xpect(&c, "threshold: one microsecond short is a tap on release",
          keyhold_feed(&k, 1, now + HOLD_US - 1u, HOLD_US) == KEYHOLD_NONE &&
          keyhold_feed(&k, 0, now + HOLD_US - 1u, HOLD_US) == KEYHOLD_TAP);
    keyhold_feed(&k, 1, now, HOLD_US);
    xpect(&c, "threshold: exactly hold_us is a hold",
          keyhold_feed(&k, 1, now + HOLD_US, HOLD_US) == KEYHOLD_HOLD);

    return xfail_done(&c);
}
