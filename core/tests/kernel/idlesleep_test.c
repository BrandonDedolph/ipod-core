/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/idlesleep_test.c — the idle-sleep policy on the host.
 *
 * idlesleep.c is pure (the caller passes the clock), so every scenario is a
 * script of main-loop passes at exact microsecond stamps. Pinned:
 *   1. The timeout is two minutes — Apple's figure, and the number the
 *      2026-09-24 drain report was written against.
 *   2. Nothing fires while there is a reason to be up (playing, the cable,
 *      an armed sleep timer), however long that lasts.
 *   3. Idle fires on the pass the timeout elapses, not one pass before.
 *   4. An input (a change of the stamp) restarts the countdown from the
 *      input's own moment.
 *   5. Busy -> idle starts a fresh countdown from the last busy pass (pause,
 *      or the cable coming out, does not sleep the device on the spot).
 *   6. The answer is one-shot: ignored, it is asked again a timeout later,
 *      not on every pass.
 *   7. A refused PMU standby latches the policy off for the session.
 *   8. Everything survives the 32-bit microsecond wrap.
 */

#include <stdio.h>

#include "idlesleep.h"

static int g_fail;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) g_fail = 1;
    return cond;
}

#define S(n)   ((uint32_t)(n) * 1000000u)      /* seconds -> us */
#define PASS_US 10000u                          /* the main loop's 10 ms halt */

/* Run the loop from `from` to `to` (exclusive) with a fixed input stamp and
 * busy flag; returns how many passes answered 1 and the stamp of the first. */
static int run(idlesleep_t *s, uint32_t from, uint32_t to, uint32_t input,
               int busy, uint32_t *first_fire)
{
    int fires = 0;
    /* By step count, so a span may cross the 32-bit wrap and may be hours. */
    uint32_t steps = (uint32_t)(to - from) / PASS_US;
    for (uint32_t i = 0; i < steps; i++) {
        uint32_t t = from + i * PASS_US;
        if (idlesleep_feed(s, t, input, busy)) {
            if (fires == 0 && first_fire) *first_fire = t;
            fires++;
        }
    }
    return fires;
}

int main(void)
{
    idlesleep_t s;
    uint32_t fire = 0;

    /* --- 1: the constant --- */
    check("timeout is two minutes", IDLESLEEP_TIMEOUT_US == S(120));

    /* --- 2: busy never fires --- */
    idlesleep_reset(&s, S(10));
    check("playing for an hour never fires",
          run(&s, S(10), S(3610), S(10), 1, 0) == 0);
    check("idle_us is at most one pass while busy",
          idlesleep_idle_us(&s, S(3610)) <= PASS_US);

    /* --- 3: the exact pass --- */
    idlesleep_reset(&s, S(100));
    check("one pass before the timeout: no",
          idlesleep_feed(&s, S(100) + IDLESLEEP_TIMEOUT_US - PASS_US, S(100), 0) == 0);
    check("idle_us counts from the reset",
          idlesleep_idle_us(&s, S(100) + S(60)) == S(60));
    check("at the timeout: fire",
          idlesleep_feed(&s, S(100) + IDLESLEEP_TIMEOUT_US, S(100), 0) == 1);

    /* --- 4: input restarts from the input's moment --- */
    idlesleep_reset(&s, S(0));
    check("90 s idle, nothing yet", run(&s, S(0), S(90), S(0), 0, 0) == 0);
    /* a press at 90 s: the stamp changes */
    check("the press itself does not fire", idlesleep_feed(&s, S(90) + PASS_US, S(90), 0) == 0);
    check("119 s after the press: still up", run(&s, S(90) + 2 * PASS_US, S(209), S(90), 0, 0) == 0);
    check("120 s after the press: sleep",
          run(&s, S(209), S(212), S(90), 0, &fire) == 1 && fire == S(90) + IDLESLEEP_TIMEOUT_US);

    /* --- 5: busy -> idle counts from the last busy pass --- */
    idlesleep_reset(&s, S(0));
    check("an hour on the cable, awake", run(&s, S(0), S(3600), S(0), 1, 0) == 0);
    /* unplugged at 3600 s: the last busy pass was 3600 - 10 ms */
    check("unplug does not sleep on the spot", run(&s, S(3600), S(3719), S(0), 0, 0) == 0);
    check("two minutes after the unplug it does",
          run(&s, S(3719), S(3722), S(0), 0, &fire) == 1 &&
          (uint32_t)(fire - S(3600)) >= IDLESLEEP_TIMEOUT_US - PASS_US &&
          (uint32_t)(fire - S(3600)) <= IDLESLEEP_TIMEOUT_US + PASS_US);

    /* paused at 5000 s after playing: same rule */
    idlesleep_reset(&s, S(4000));
    run(&s, S(4000), S(5000), S(4000), 1, 0);
    check("pause does not sleep on the spot", run(&s, S(5000), S(5119), S(4000), 0, 0) == 0);
    check("two minutes after the pause it does", run(&s, S(5119), S(5122), S(4000), 0, 0) == 1);

    /* --- 6: one-shot --- */
    idlesleep_reset(&s, S(0));
    check("ignored answer: exactly one fire per timeout, four in eight minutes",
          run(&s, S(0), S(8 * 60) + PASS_US, S(0), 0, 0) == 4);

    /* --- 7: the refused-standby latch --- */
    idlesleep_reset(&s, S(0));
    check("not off after reset", !idlesleep_is_off(&s));
    idlesleep_off(&s);
    check("off is reported", idlesleep_is_off(&s));
    check("off never fires", run(&s, S(0), S(1800), S(0), 0, 0) == 0);
    check("off reports no idle", idlesleep_idle_us(&s, S(1800)) == 0);
    idlesleep_reset(&s, S(1800));
    check("reset clears the latch", !idlesleep_is_off(&s) &&
          run(&s, S(1800), S(1921), S(1800), 0, 0) == 1);

    /* --- 8: the wrap --- */
    {
        uint32_t t0 = 0xFFFFFFFFu - S(60);        /* 60 s before the wrap */
        idlesleep_reset(&s, t0);
        check("straddling the wrap: nothing at 119 s",
              idlesleep_feed(&s, t0 + S(119), t0, 0) == 0);
        check("straddling the wrap: fires at 120 s (past zero)",
              idlesleep_feed(&s, t0 + S(120), t0, 0) == 1 && t0 + S(120) < S(120));
        /* an input stamped just before the wrap, checked after it */
        uint32_t in = 0xFFFFFFFFu - S(1);
        idlesleep_reset(&s, in - S(10));
        check("input before the wrap, counted after it",
              idlesleep_feed(&s, in + S(119), in, 0) == 0 &&
              idlesleep_feed(&s, in + S(120), in, 0) == 1);
        check("idle_us across the wrap", idlesleep_idle_us(&s, in + S(120) + S(5)) == S(5));
    }

    printf(g_fail ? "idlesleep: FAIL\n" : "idlesleep: OK\n");
    return g_fail;
}
