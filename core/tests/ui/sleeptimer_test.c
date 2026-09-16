/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/sleeptimer_test.c — the sleep timer's countdown
 * (core/ui/sleeptimer.c) on the host.
 *
 * THE POINT OF THIS FILE. The device's clock is the 1 MHz USEC_TIMER, a
 * free-running 32-bit counter that wraps every ~71.6 minutes. A 120-minute
 * sleep timer therefore crosses that wrap TWICE, and the only way to observe
 * a two-hour countdown is to drive it here: case 4 runs the whole thing,
 * 72 000 feeds, starting 30 seconds before the wrap, and asserts a TICK on
 * every minute boundary and the FIRE on the 120th minute — not the 119th, not
 * the 121st, and not twice.
 *
 * The rest pins the properties kernel/main.c leans on: the timer fires once
 * and disarms itself (a caller that was away asleep must not come back to a
 * second FIRE), the sub-minute remainder is carried so coarse or irregular
 * feeds do not drift, re-arming restarts, and `remaining` never reads 0 while
 * armed — the token would say "SLEEP 0" for a whole minute.
 *
 * Every case is a sequence of clock samples, the way the main loop feeds it
 * once per pass.
 */

#include <stdio.h>
#include <string.h>

#include "sleeptimer.h"
#include "../xfail.h"

#define MIN_US 60000000u

int main(void)
{
    xfail_ctx c = { "sleeptimer", 0, 0, 0 };
    sleeptimer_t t;
    char tok[16];

    /* --- 1. reset / off: inert forever ---------------------------------- */
    sleeptimer_reset(&t);
    xpect(&c, "off: not armed", sleeptimer_armed(&t) == 0);
    xpect(&c, "off: total is 0",     sleeptimer_total_min(&t) == 0);
    xpect(&c, "off: remaining is 0", sleeptimer_remaining_min(&t) == 0);
    memset(tok, 'x', sizeof tok);
    xpect(&c, "off: the token is empty",
          sleeptimer_token(&t, tok, (int)sizeof tok) == 0 && tok[0] == '\0');
    {
        int ok = 1;
        uint32_t now = 0;
        for (int i = 0; i < 500; i++) {
            now += 10000000u;            /* 10 s a feed, well past a minute */
            if (sleeptimer_feed(&t, now) != SLEEPTIMER_NONE) {
                ok = 0;
            }
        }
        xpect(&c, "off: feeding a disarmed timer never produces an event", ok);
        xpect(&c, "off: feeding does not arm it", sleeptimer_armed(&t) == 0);
    }

    /* --- 2. arm 15: the first minute reads 15, not 14 -------------------- */
    {
        uint32_t now = 1000u;
        sleeptimer_arm(&t, 15, now);
        xpect(&c, "arm 15: armed", sleeptimer_armed(&t) == 1);
        xpect(&c, "arm 15: total is 15", sleeptimer_total_min(&t) == 15);
        xpect(&c, "arm 15: remaining reads the FULL duration at once",
              sleeptimer_remaining_min(&t) == 15);
        xpect(&c, "arm 15: the token is SLEEP 15",
              sleeptimer_token(&t, tok, (int)sizeof tok) == 8 &&
              strcmp(tok, "SLEEP 15") == 0);

        /* 10 ms feeds for 59.99 s: silent, and still 15. */
        int ok = 1;
        for (int i = 0; i < 5999; i++) {
            now += 10000u;
            if (sleeptimer_feed(&t, now) != SLEEPTIMER_NONE ||
                sleeptimer_remaining_min(&t) != 15) {
                ok = 0;
            }
        }
        xpect(&c, "arm 15: silent for the first 59.99 s, still showing 15", ok);

        now += 10000u;                   /* the feed that crosses 60 s */
        xpect(&c, "arm 15: the minute boundary is a TICK",
              sleeptimer_feed(&t, now) == SLEEPTIMER_TICK);
        xpect(&c, "arm 15: and it now reads 14",
              sleeptimer_remaining_min(&t) == 14);

        ok = 1;
        for (int i = 0; i < 100; i++) {
            now += 10000u;
            if (sleeptimer_feed(&t, now) != SLEEPTIMER_NONE) {
                ok = 0;
            }
        }
        xpect(&c, "arm 15: no second TICK inside the same minute", ok);
    }

    /* --- 3. fires exactly once, and disarms itself ----------------------- */
    {
        uint32_t now = 500u;
        sleeptimer_arm(&t, 1, now);
        now += MIN_US - 1u;
        xpect(&c, "fire: one microsecond short is still silent",
              sleeptimer_feed(&t, now) == SLEEPTIMER_NONE &&
              sleeptimer_armed(&t) == 1);
        now += 1u;
        xpect(&c, "fire: the minute itself FIREs",
              sleeptimer_feed(&t, now) == SLEEPTIMER_FIRE);
        xpect(&c, "fire: the FIRE disarmed it", sleeptimer_armed(&t) == 0);
        xpect(&c, "fire: remaining is 0 afterwards",
              sleeptimer_remaining_min(&t) == 0);
        xpect(&c, "fire: the token is gone",
              sleeptimer_token(&t, tok, (int)sizeof tok) == 0);

        /* The caller may be away for hours (the device just went to sleep):
         * no later feed, wrapped or not, may fire it a second time. */
        int ok = 1;
        for (int i = 0; i < 1000; i++) {
            now += 300000000u;           /* 5 minutes a feed, wrapping */
            if (sleeptimer_feed(&t, now) != SLEEPTIMER_NONE) {
                ok = 0;
            }
        }
        xpect(&c, "fire: exactly once — a long absence cannot re-fire it", ok);
    }

    /* --- 4. the whole 120 minutes, across TWO 32-bit clock wraps --------- */
    {
        /* Start 30 s before the wrap so the first minute straddles it; at
         * 100 ms a feed, 120 minutes is 72 000 feeds and two more wraps. */
        uint32_t now = 0xFFFFFFFFu - 30000000u;
        sleeptimer_arm(&t, 120, now);

        int ticks = 0, fires = 0, fire_at = -1, order_ok = 1, zero_seen = 0;
        int prev = sleeptimer_remaining_min(&t);
        int monotone = (prev == 120);
        for (int i = 1; i <= 72000; i++) {
            now += 100000u;              /* 100 ms */
            sleeptimer_event_t e = sleeptimer_feed(&t, now);
            int boundary = (i % 600 == 0);          /* every 60 s exactly */
            if (e == SLEEPTIMER_FIRE) {
                fires++;
                if (fire_at < 0) fire_at = i;
                if (!boundary) order_ok = 0;
            } else if (e == SLEEPTIMER_TICK) {
                ticks++;
                if (!boundary) order_ok = 0;
                int r = sleeptimer_remaining_min(&t);
                if (r != prev - 1) monotone = 0;
                prev = r;
            } else if (boundary && fire_at < 0) {
                order_ok = 0;            /* a boundary that said nothing */
            }
            if (sleeptimer_armed(&t) && sleeptimer_remaining_min(&t) < 1) {
                zero_seen = 1;
            }
        }
        xpect(&c, "120 min: FIREs exactly once", fires == 1);
        xpect(&c, "120 min: FIREs on the 120th minute, not earlier or later",
              fire_at == 72000);
        xpect(&c, "120 min: a TICK on each of the first 119 minute boundaries",
              ticks == 119);
        xpect(&c, "120 min: events land only on minute boundaries", order_ok);
        xpect(&c, "120 min: remaining walks 120 down to 1, one at a time",
              monotone && prev == 1);
        xpect(&c, "120 min: never shows 0 while armed", zero_seen == 0);
        xpect(&c, "120 min: disarmed at the end", sleeptimer_armed(&t) == 0);
    }

    /* --- 5. coarse and irregular feeds carry their remainder ------------- */
    {
        /* 90 minutes at 5 s a feed: 5 s does not divide the minute evenly in
         * any way that matters, but 5 s does — so follow it with a 7/13/1 s
         * cycle whose period (21 s) does NOT divide 60 s. A truncating
         * accumulator drifts late here; this one must not. */
        uint32_t now = 12345u;
        sleeptimer_arm(&t, 90, now);
        uint64_t elapsed_us = 0;
        int fired_us = -1;
        int steps[3] = { 7000000, 13000000, 1000000 };
        for (int i = 0; elapsed_us < (uint64_t)95 * MIN_US; i++) {
            uint32_t step = (i < 360) ? 5000000u
                                      : (uint32_t)steps[(i - 360) % 3];
            now += step;
            elapsed_us += step;
            sleeptimer_event_t e = sleeptimer_feed(&t, now);
            if (e == SLEEPTIMER_FIRE && fired_us < 0) {
                /* The first feed at or past 90 minutes, and no earlier one. */
                fired_us = 1;
                xpect(&c, "irregular: FIREs on the first feed at/past 90 min",
                      elapsed_us >= (uint64_t)90 * MIN_US &&
                      elapsed_us < (uint64_t)90 * MIN_US + 13000000u);
            }
        }
        xpect(&c, "irregular: it fired at all", fired_us == 1);
    }
    {
        /* The remainder is what makes that exact: 61 feeds of 59 s must be
         * 59.98 minutes, i.e. 30 minutes left of 90 has not yet slipped. */
        uint32_t now = 7u;
        sleeptimer_arm(&t, 90, now);
        for (int i = 0; i < 61; i++) {
            now += 59000000u;            /* 59 s: never a whole minute alone */
            sleeptimer_feed(&t, now);
        }
        /* 61 * 59 s = 3599 s = 59 whole minutes + 59 s. */
        xpect(&c, "remainder: 61 x 59 s is 59 whole minutes, not 61",
              sleeptimer_remaining_min(&t) == 90 - 59);
    }

    /* --- 6. re-arming restarts; arming with 0 disarms -------------------- */
    {
        uint32_t now = 99u;
        sleeptimer_arm(&t, 30, now);
        for (int i = 0; i < 10; i++) {
            now += MIN_US;
            sleeptimer_feed(&t, now);
        }
        xpect(&c, "re-arm: 10 minutes into 30 reads 20",
              sleeptimer_remaining_min(&t) == 20);
        sleeptimer_arm(&t, 60, now);
        xpect(&c, "re-arm: 60 restarts from the full duration",
              sleeptimer_total_min(&t) == 60 &&
              sleeptimer_remaining_min(&t) == 60);
        /* The partial minute is dropped too, not carried into the new arm. */
        now += MIN_US - 1u;
        xpect(&c, "re-arm: the old sub-minute remainder did not come with it",
              sleeptimer_feed(&t, now) == SLEEPTIMER_NONE &&
              sleeptimer_remaining_min(&t) == 60);
        sleeptimer_arm(&t, 0, now);
        xpect(&c, "re-arm: 0 disarms",
              sleeptimer_armed(&t) == 0 && sleeptimer_remaining_min(&t) == 0);
        sleeptimer_arm(&t, -5, now);
        xpect(&c, "re-arm: a negative duration disarms too",
              sleeptimer_armed(&t) == 0);
        sleeptimer_arm(&t, 300, now);
        xpect(&c, "re-arm: a duration that would not fit the byte disarms",
              sleeptimer_armed(&t) == 0);
    }

    /* --- 7. token formatting + bounds ------------------------------------ */
    {
        uint32_t now = 1u;
        sleeptimer_arm(&t, 120, now);
        xpect(&c, "token: SLEEP 120",
              sleeptimer_token(&t, tok, (int)sizeof tok) == 9 &&
              strcmp(tok, "SLEEP 120") == 0);

        /* Walk down to 9 and then to 1 and read the token at each. */
        for (int i = 0; i < 111; i++) {
            now += MIN_US;
            sleeptimer_feed(&t, now);
        }
        xpect(&c, "token: SLEEP 9",
              sleeptimer_remaining_min(&t) == 9 &&
              sleeptimer_token(&t, tok, (int)sizeof tok) == 7 &&
              strcmp(tok, "SLEEP 9") == 0);
        for (int i = 0; i < 8; i++) {
            now += MIN_US;
            sleeptimer_feed(&t, now);
        }
        sleeptimer_token(&t, tok, (int)sizeof tok);
        xpect(&c, "token: SLEEP 1 is the last one shown",
              sleeptimer_remaining_min(&t) == 1 &&
              strcmp(tok, "SLEEP 1") == 0);

        /* Bounded: a buffer that cannot hold the whole token draws nothing
         * rather than a truncated lie. "SLEEP 1" needs 8 bytes with its NUL. */
        sleeptimer_arm(&t, 120, now);
        memset(tok, 'x', sizeof tok);
        xpect(&c, "token: a 9-byte buffer is too small for SLEEP 120",
              sleeptimer_token(&t, tok, 9) == 0 && tok[0] == '\0');
        xpect(&c, "token: 10 bytes is exactly enough for SLEEP 120",
              sleeptimer_token(&t, tok, 10) == 9 &&
              strcmp(tok, "SLEEP 120") == 0);
        xpect(&c, "token: a zero-length buffer is refused",
              sleeptimer_token(&t, tok, 0) == 0);
    }

    /* --- 8. remaining >= 1 for every armed state, at every duration ------ */
    {
        int ok = 1;
        const int durations[5] = { 15, 30, 60, 90, 120 };
        for (int d = 0; d < 5; d++) {
            uint32_t now = 0xFFFFFF00u;       /* start against the wrap again */
            sleeptimer_arm(&t, durations[d], now);
            for (;;) {
                if (sleeptimer_armed(&t) && sleeptimer_remaining_min(&t) < 1) {
                    ok = 0;
                }
                now += 30000000u;             /* 30 s a feed */
                if (sleeptimer_feed(&t, now) == SLEEPTIMER_FIRE) {
                    break;
                }
            }
            if (sleeptimer_armed(&t)) {
                ok = 0;
            }
        }
        xpect(&c, "armed implies remaining >= 1, at every offered duration",
              ok);
    }

    return xfail_done(&c);
}
